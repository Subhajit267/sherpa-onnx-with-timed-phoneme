// scripts/node-addon-api/src/tts-playback.cc
//
// Non-blocking PipeWire audio playback for offline TTS, timed to start
// exactly when an Electron renderer has finished priming its lip-sync
// animation. Personal, single-deployment project (Jetson, Kokoro v0.19 /
// Piper-based single-language model only) -- consolidated into one file on
// purpose: the separate-files-per-class layout from the original
// multi-stage build existed for independent unit-testability and
// HarmonyOS-safety, neither of which applies here. See tts-playback.h for
// the one shared symbol other files need (RegisterTtsPlaybackExports) plus
// the phoneme-array helpers shared with non-streaming-tts.cc.
//
// ---- The three threads (and only these three) --------------------------
//   1. Synthesis thread (std::thread, spawned per speak() call): runs
//      RunSynthesis() -- calls the same SherpaOnnxOfflineTtsGenerateWithConfig
//      the existing sync/async wrappers in non-streaming-tts.cc use.
//   2. Arming thread (std::thread, persistent, one for the addon's life):
//      blocks on ReadyGate::WaitBoth(), then Play()s/polls/Pause()s.
//   3. PipeWire's own RT thread (server/rtkit-managed, not spawned by this
//      code): the ONLY thing that reads/copies PCM, via DoProcess().
// No other thread is added for "organization" -- if something needs doing
// off the main/V8 thread, it happens on one of these three.
//
// ---- RT-safety invariants (unrelated to file layout, do not relax) ------
//   - DoProcess() touches only atomics and does a plain memcpy: no locks,
//     no allocation, no logging, no exceptions, no JS.
//   - Utterance::pcm is read only by the RT thread; PCM never crosses into
//     V8/JS.
//   - Pause() clears current_ptr_ inside the same lock span as
//     pw_stream_set_active(false), not after.
//   - ReadyGate/arming-thread gating, the UtteranceState single-in-flight
//     discipline, and the single g_state reset point are unchanged from
//     the original multi-file design.
#include "tts-playback.h"

#include <pipewire/pipewire.h>
#include <pipewire/version.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/param.h>
#include <spa/pod/builder.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>

// See the "corrected in Fix 2" note in DoProcess()'s neighborhood below:
// PW_STREAM_FLAG_RT_PROCESS is a plain C enum value, not a #define, so
// #if defined(...) can never detect it -- gate on the actual libpipewire-0.3
// version instead. Verified against upstream (github.com/PipeWire/pipewire)
// that this flag has been present since the very first 0.3.0 tag.
#if !PW_CHECK_VERSION(0, 3, 0)
#error "This build requires libpipewire-0.3 >= 0.3.0 for " \
       "PW_STREAM_FLAG_RT_PROCESS. Upgrade libpipewire-0.3-dev on this " \
       "system before building sherpa-onnx-tts playback support."
#endif

namespace {

// ===========================================================================
// Utterance: one synthesized clip, handed from the synth thread to the RT
// audio thread. pcm/phonemes/sample_rate are written once by the synth
// thread and treated as immutable for the rest of the object's life --
// every other thread only reads them. Only the atomics below change after
// construction.
//
// Each atomic is alignas(64) because each has a different writer thread
// than its neighbors; padding prevents false sharing between the RT audio
// thread, the synth thread, and the arming/main threads landing on the same
// cache line.
// ===========================================================================
struct Utterance {
  int64_t id = 0;
  std::vector<float> pcm;               // mono, resampled to the negotiated
                                         // PipeWire rate; immutable after write
  std::vector<PhonemeTiming> phonemes;  // immutable after write
  int32_t sample_rate = 0;

  // Writer: synth thread (release, once). Reader: arming thread (acquire).
  alignas(64) std::atomic<bool> audio_ready{false};

  // Writer: RT process() callback, first frame emitted for this utterance
  // (release). Reader: arming thread, diagnostics only.
  alignas(64) std::atomic<bool> playback_start{false};

  // Writer: RT process() callback, on end-of-buffer or observing cancel
  // (release). Reader: arming thread's poll loop (acquire).
  alignas(64) std::atomic<bool> playback_done{false};

  // Writer: arming thread, once, with release, immediately before
  // pw_stream_set_active(true) -- resets to 0 for this utterance. This
  // release pairs with the acquire load the RT callback performs on every
  // invocation while this utterance is current. After that reset, only the
  // RT callback (single thread) mutates it, with relaxed same-thread
  // increments.
  alignas(64) std::atomic<uint32_t> read_index{0};

  // Writer: main/JS thread via stopSpeak() (release). Reader: RT process()
  // callback, every callback (acquire). Presence only -- no interrupt logic
  // beyond "stop emitting samples for this utterance" is implemented.
  alignas(64) std::atomic<bool> cancel{false};
};

// ===========================================================================
// ReadyGate: the rendezvous between "audio is synthesized" and "renderer
// has primed its animation." Non-RT only, never touched by the PipeWire
// thread.
// ===========================================================================
enum class GateResult { kOk, kAudioTimeout, kRendererTimeout, kBothTimeout };

class ReadyGate {
 public:
  void SetAudioReady() {
    {
      std::lock_guard<std::mutex> lk(mu_);
      audio_ready_ = true;
    }
    cv_.notify_all();
  }

  void SetRendererReady() {
    {
      std::lock_guard<std::mutex> lk(mu_);
      renderer_ready_ = true;
    }
    cv_.notify_all();
  }

  void Reset() {
    std::lock_guard<std::mutex> lk(mu_);
    audio_ready_ = false;
    renderer_ready_ = false;
  }

  // Blocks until both sides are ready or `timeout` elapses. On timeout,
  // reports precisely which side(s) never signaled so the caller can log a
  // diagnosable reason rather than inferring it from separate flag reads.
  GateResult WaitBoth(std::chrono::milliseconds timeout) {
    std::unique_lock<std::mutex> lk(mu_);
    bool ok = cv_.wait_for(lk, timeout,
                            [this] { return audio_ready_ && renderer_ready_; });
    if (ok) return GateResult::kOk;
    if (!audio_ready_ && !renderer_ready_) return GateResult::kBothTimeout;
    if (!audio_ready_) return GateResult::kAudioTimeout;
    return GateResult::kRendererTimeout;
  }

 private:
  std::mutex mu_;
  std::condition_variable cv_;
  bool audio_ready_ = false;
  bool renderer_ready_ = false;
};

const char *GateResultToString(GateResult r) {
  switch (r) {
    case GateResult::kOk: return "ok";
    case GateResult::kAudioTimeout: return "audio-timeout";
    case GateResult::kRendererTimeout: return "renderer-timeout";
    case GateResult::kBothTimeout: return "both-timeout";
  }
  return "unknown";
}

// ===========================================================================
// PwPlayback: owns the one PipeWire stream for the addon's entire lifetime.
// Created once, activated/paused per utterance, never reconnected.
// ===========================================================================
class PwPlayback {
 public:
  PwPlayback() = default;
  ~PwPlayback() { Shutdown(); }

  PwPlayback(const PwPlayback &) = delete;
  PwPlayback &operator=(const PwPlayback &) = delete;

  // Called once, at addon init. Connects the stream inactive, then does a
  // one-time warm-up activate -> wait for real format negotiation ->
  // deactivate. (An INACTIVE stream's adapter node reports SPA_PARAM_Format
  // as a null pod on this PipeWire version/session-manager combo -- the
  // concrete negotiated format is only echoed back once the stream is
  // actually driven. This warm-up still respects "stream created once, only
  // activated/paused per-utterance" -- it's the same Play()/Pause()-adjacent
  // primitive, just run once here with no utterance attached.)
  bool Start(int32_t preferred_sample_rate, int32_t timeout_ms = 5000) {
    pw_init(nullptr, nullptr);

    loop_ = pw_thread_loop_new("sherpa-onnx-tts-playback", nullptr);
    if (!loop_) {
      fprintf(stderr, "[tts-playback] pw_thread_loop_new failed\n");
      return false;
    }

    static const pw_stream_events kStreamEvents = [] {
      pw_stream_events events{};
      events.version = PW_VERSION_STREAM_EVENTS;
      events.state_changed = OnStateChanged;
      events.param_changed = OnParamChanged;
      events.process = OnProcess;
      return events;
    }();

    char latency[64];
    std::snprintf(latency, sizeof(latency), "256/%d", preferred_sample_rate);

    pw_properties *props = pw_properties_new(
        PW_KEY_MEDIA_TYPE, "Audio", PW_KEY_MEDIA_CATEGORY, "Playback",
        PW_KEY_MEDIA_ROLE, "Communication", PW_KEY_NODE_NAME,
        "sherpa-onnx-tts", PW_KEY_NODE_LATENCY, latency, nullptr);

    pw_thread_loop_lock(loop_);

    stream_ = pw_stream_new_simple(pw_thread_loop_get_loop(loop_),
                                    "sherpa-onnx-tts", props, &kStreamEvents,
                                    this);
    if (!stream_) {
      pw_thread_loop_unlock(loop_);
      fprintf(stderr, "[tts-playback] pw_stream_new_simple failed\n");
      return false;
    }

    uint8_t pod_buf[1024];
    spa_pod_builder b = SPA_POD_BUILDER_INIT(pod_buf, sizeof(pod_buf));

    spa_audio_info_raw info{};
    info.format = SPA_AUDIO_FORMAT_F32;
    info.channels = 1;
    info.rate = static_cast<uint32_t>(preferred_sample_rate);

    const spa_pod *params[1];
    params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info);

    // PW_STREAM_FLAG_RT_PROCESS: process() is invoked directly from the
    // graph's driver data thread (server/rtkit-managed), not from our own
    // pw_thread_loop_ -- this client has no direct API to introspect that
    // thread's actual scheduling policy/priority. Whether the daemon
    // obtained SCHED_FIFO (vs. silently falling back to SCHED_OTHER for
    // lack of CAP_SYS_NICE/rtkit access) is only observable from outside
    // the process -- e.g. `chrt -p <tid>` against the pipewire daemon's
    // data-loop thread on the target device. Jetson deployment
    // verification item, not something this code can log itself.
    auto flags = static_cast<pw_stream_flags>(
        PW_STREAM_FLAG_INACTIVE | PW_STREAM_FLAG_AUTOCONNECT |
        PW_STREAM_FLAG_MAP_BUFFERS | PW_STREAM_FLAG_RT_PROCESS);

    int rc = pw_stream_connect(stream_, PW_DIRECTION_OUTPUT, PW_ID_ANY, flags,
                                params, 1);
    pw_thread_loop_unlock(loop_);

    if (rc < 0) {
      fprintf(stderr, "[tts-playback] pw_stream_connect failed: %d\n", rc);
      return false;
    }
    if (pw_thread_loop_start(loop_) < 0) {
      fprintf(stderr, "[tts-playback] pw_thread_loop_start failed\n");
      return false;
    }

    pw_thread_loop_lock(loop_);
    pw_stream_set_active(stream_, true);
    pw_thread_loop_unlock(loop_);

    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeout_ms);
    bool ok = true;
    while (!negotiated_.load(std::memory_order_acquire)) {
      if (std::chrono::steady_clock::now() >= deadline) {
        fprintf(stderr,
                "[tts-playback] timed out waiting for format negotiation\n");
        ok = false;
        break;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    pw_thread_loop_lock(loop_);
    pw_stream_set_active(stream_, false);
    pw_thread_loop_unlock(loop_);

    if (!ok) return false;
    started_ = true;
    return true;
  }

  // Non-RT. Called only by the arming thread.
  void Play(std::shared_ptr<Utterance> utt) {
    owning_current_ = utt;
    utt->read_index.store(0, std::memory_order_release);
    current_ptr_.store(utt.get(), std::memory_order_release);

    pw_thread_loop_lock(loop_);
    pw_stream_set_active(stream_, true);
    pw_thread_loop_unlock(loop_);
  }

  // Non-RT. Called only by the arming thread, after observing
  // utt->playback_done. The pointer-clear happens inside the same lock span
  // as set_active(false) -- a buffer already in-flight inside DoProcess()
  // must not be left dereferencing current_ptr_ while owning_current_'s
  // last reference is being dropped concurrently from this non-RT thread.
  void Pause() {
    pw_thread_loop_lock(loop_);
    pw_stream_set_active(stream_, false);
    current_ptr_.store(nullptr, std::memory_order_release);
    owning_current_.reset();
    pw_thread_loop_unlock(loop_);
  }

  // Non-RT. Called only from the addon's napi_add_env_cleanup_hook.
  void Shutdown() {
    if (!started_) return;

    if (stream_) {
      pw_thread_loop_lock(loop_);
      pw_stream_set_active(stream_, false);
      current_ptr_.store(nullptr, std::memory_order_release);
      owning_current_.reset();
      pw_stream_disconnect(stream_);
      pw_thread_loop_unlock(loop_);

      pw_stream_destroy(stream_);
      stream_ = nullptr;
    }

    if (loop_) {
      pw_thread_loop_stop(loop_);
      pw_thread_loop_destroy(loop_);
      loop_ = nullptr;
    }

    pw_deinit();
    started_ = false;
  }

  int32_t NegotiatedSampleRate() const {
    return negotiated_rate_.load(std::memory_order_acquire);
  }

 private:
  static void OnProcess(void *data) {
    static_cast<PwPlayback *>(data)->DoProcess();
  }

  static void OnStateChanged(void *data, enum pw_stream_state /*old_state*/,
                              enum pw_stream_state state, const char *error) {
    (void)data;
    if (state == PW_STREAM_STATE_ERROR) {
      fprintf(stderr, "[tts-playback] stream error: %s\n",
              error ? error : "(unknown)");
    }
  }

  static void OnParamChanged(void *data, uint32_t id,
                              const struct spa_pod *param) {
    auto *self = static_cast<PwPlayback *>(data);
    if (param == nullptr || id != SPA_PARAM_Format) return;

    spa_audio_info_raw info{};
    if (spa_format_audio_raw_parse(param, &info) < 0) return;
    fprintf(stderr, "[tts-playback] negotiated rate=%u channels=%u\n",
            info.rate, info.channels);

    self->negotiated_rate_.store(static_cast<int32_t>(info.rate),
                                  std::memory_order_release);
    self->negotiated_.store(true, std::memory_order_release);
  }

  // RT: atomics + memcpy only. No locks, no allocation, no logging, no
  // exceptions, no JS -- regardless of how many files this lives in.
  void DoProcess() {
    pw_buffer *b = pw_stream_dequeue_buffer(stream_);
    if (!b) {
      dropped_buffers_.fetch_add(1, std::memory_order_relaxed);
      return;
    }

    spa_buffer *buf = b->buffer;
    spa_data &d = buf->datas[0];
    auto *dst = static_cast<float *>(d.data);
    const uint32_t stride = sizeof(float);

    // This libpipewire-0.3 (0.3.48 at development time) exposes no
    // per-callback "requested frame count" on pw_buffer (only
    // {buffer, user_data, size}) -- fill the buffer's full allocated
    // capacity every callback, silence-padding past the end of the
    // utterance; equivalent since PipeWire buffers are quantum-sized.
    uint32_t want = dst ? d.maxsize / stride : 0;
    uint32_t written = 0;

    Utterance *utt = current_ptr_.load(std::memory_order_acquire);

    if (dst && utt) {
      uint32_t idx = utt->read_index.load(std::memory_order_acquire);
      uint32_t pcm_size = static_cast<uint32_t>(utt->pcm.size());
      uint32_t remaining = idx < pcm_size ? pcm_size - idx : 0;
      written = remaining < want ? remaining : want;

      if (written > 0) {
        std::memcpy(dst, utt->pcm.data() + idx, written * stride);
      }
      if (written < want) {
        std::memset(dst + written, 0, (want - written) * stride);
      }

      idx += written;
      utt->read_index.store(idx, std::memory_order_relaxed);

      bool expected = false;
      utt->playback_start.compare_exchange_strong(expected, true,
                                                   std::memory_order_release);

      bool cancelled = utt->cancel.load(std::memory_order_acquire);
      if (idx >= pcm_size || cancelled) {
        // Never call pw_stream_set_active(false) here -- signal via atomic
        // only; the arming thread's poll loop performs the actual pause
        // from non-RT context.
        utt->playback_done.store(true, std::memory_order_release);
      }
    } else if (dst) {
      std::memset(dst, 0, want * stride);
    }

    if (buf->datas[0].chunk) {
      buf->datas[0].chunk->offset = 0;
      buf->datas[0].chunk->stride = static_cast<int32_t>(stride);
      buf->datas[0].chunk->size = want * stride;
    }

    pw_stream_queue_buffer(stream_, b);
  }

  pw_thread_loop *loop_ = nullptr;
  pw_stream *stream_ = nullptr;
  bool started_ = false;

  std::atomic<int32_t> negotiated_rate_{0};
  std::atomic<bool> negotiated_{false};

  // Deliberately a raw pointer, NOT std::atomic<shared_ptr<Utterance>> --
  // shared_ptr control-block refcounting is itself atomic RMW traffic we do
  // not want on the RT path. Ownership/lifetime is held separately in
  // owning_current_ (non-RT only).
  std::atomic<Utterance *> current_ptr_{nullptr};
  std::shared_ptr<Utterance> owning_current_;

  alignas(64) std::atomic<uint64_t> dropped_buffers_{0};
};

// ===========================================================================
// Synthesis (runs on the per-utterance synth thread). Calls the same C API
// the existing sync/async wrappers in non-streaming-tts.cc use -- no
// reimplementation. Resamples via the addon's existing
// SherpaOnnxLinearResampler C functions directly (bypassing N-API entirely,
// since this runs off the JS thread). Model-agnostic at the C-API level:
// nothing here branches on model type -- Kokoro/Piper is the only model
// this project loads, and this function never needs to know that.
// ===========================================================================
struct SynthResult {
  bool ok = false;
  std::string error;  // populated only when ok == false
};

SynthResult RunSynthesis(const SherpaOnnxOfflineTts *tts,
                         const std::string &text, int32_t sid, float speed,
                         int32_t target_sample_rate, Utterance *out) {
  SynthResult result;

  // Fix 4 finding: some bad-but-plausible inputs (confirmed: speed = NaN)
  // don't make SherpaOnnxOfflineTtsGenerateWithConfig return null -- they
  // throw a C++ exception (Ort::Exception, from deep inside onnxruntime)
  // that the C API layer does not catch, which would otherwise propagate
  // uncaught out of this function, run on its own detached std::thread,
  // and call std::terminate() -- crashing the entire process over a
  // single bad speak() call. Catch broadly and convert to a normal
  // SynthResult{false, ...} instead -- a safety net for whatever else might
  // throw, not just the one input confirmed by testing. SpeakWrapper
  // additionally rejects the specific known-bad speed values synchronously,
  // before a thread is even spawned.
  try {
    SherpaOnnxGenerationConfig cfg{};
    cfg.sid = sid;
    cfg.speed = speed;
    cfg.silence_scale = 0.2f;

    const SherpaOnnxGeneratedAudio *audio = SherpaOnnxOfflineTtsGenerateWithConfig(
        tts, text.c_str(), &cfg, nullptr, nullptr);

    if (audio == nullptr) {
      result.error = "SherpaOnnxOfflineTtsGenerateWithConfig returned null";
      return result;
    }

    if (audio->sample_rate != target_sample_rate) {
      const SherpaOnnxLinearResampler *resampler = SherpaOnnxCreateLinearResampler(
          audio->sample_rate, target_sample_rate, 0.0f, 0);
      const SherpaOnnxResampleOut *resampled = SherpaOnnxLinearResamplerResample(
          resampler, audio->samples, audio->n, /*flush=*/1);

      out->pcm.assign(resampled->samples, resampled->samples + resampled->n);

      SherpaOnnxLinearResamplerResampleFree(resampled);
      SherpaOnnxDestroyLinearResampler(resampler);
    } else {
      out->pcm.assign(audio->samples, audio->samples + audio->n);
    }
    out->sample_rate = target_sample_rate;

    // Shared with non-streaming-tts.cc's existing call sites -- see
    // tts-playback.h. Copy-then-destroy immediately: PCM never crosses
    // into V8 on this path, so there's no need for the
    // external-ArrayBuffer-with-finalizer trick the existing sync/async
    // N-API wrappers use to defer destruction.
    out->phonemes = CopyPhonemeTimings(audio);
    SherpaOnnxDestroyOfflineTtsGeneratedAudio(audio);
  } catch (const std::exception &e) {
    result.error = std::string("synthesis threw an exception: ") + e.what();
    return result;
  } catch (...) {
    result.error = "synthesis threw a non-standard exception";
    return result;
  }

  result.ok = true;
  return result;
}

// ===========================================================================
// N-API layer: global addon-lifetime state, the arming thread, and the
// exported speak()/startPlayback()/onRendererReady()/stopSpeak()/
// setOnComplete() wrappers.
// ===========================================================================
enum class UtteranceState { kIdle, kSynthesizing, kGated, kPlaying };

PwPlayback g_playback;
std::atomic<bool> g_playback_started{false};

// Pins the ttsHandle External passed to startPlayback() so V8 can never
// garbage-collect it (and run its finalizer, which calls
// SherpaOnnxDestroyOfflineTts) for as long as this addon might still
// dereference the raw SherpaOnnxOfflineTts* extracted from it. Without
// this, speak()'s synth thread holds a bare pointer with no JS-side
// reference keeping the underlying object alive -- if the caller's only
// JS reference to the handle/OfflineTts instance ever became unreachable,
// GC could free the C++ object out from under an in-flight or future
// speak() call. Confirmed with a WeakRef-based test: collected without
// this pin, survives with it.
Napi::Reference<Napi::Value> g_tts_handle_ref;

ReadyGate g_ready_gate;

std::atomic<UtteranceState> g_state{UtteranceState::kIdle};
std::atomic<int64_t> g_current_utterance_id{0};
std::atomic<int64_t> g_next_utterance_id{1};

// Guards g_active_utt, which stopSpeak()/onRendererReady() (indirectly, via
// id check) and the arming thread all touch. Non-RT only.
std::mutex g_active_mu;
std::shared_ptr<Utterance> g_active_utt;

// Only one utterance is ever in flight (speak() rejects concurrent calls),
// so a single tracked thread -- not a vector -- suffices. Joined
// opportunistically at the start of the next speak() call; joined
// unconditionally at teardown.
std::mutex g_synth_thread_mu;
std::thread g_synth_thread;

// Single-slot handoff from the synth thread to the arming thread -- a slot,
// not a FIFO, because only one utterance is ever in flight.
std::mutex g_pending_mu;
std::condition_variable g_pending_cv;
std::shared_ptr<Utterance> g_pending_utt;

std::atomic<bool> g_shutdown{false};
std::thread g_arming_thread;

// ---- Phoneme delivery (per-call TSFN) ------------------------------------
struct PhonemeCallbackData {
  int64_t utterance_id;
  std::vector<PhonemeTiming> phonemes;
  int32_t sample_rate;
};

void InvokePhonemeCallback(Napi::Env env, Napi::Function callback,
                           std::nullptr_t *, PhonemeCallbackData *data) {
  std::unique_ptr<PhonemeCallbackData> owned(data);
  if (env == nullptr || callback == nullptr) return;
  Napi::Object arg = Napi::Object::New(env);
  arg.Set("utteranceId",
          Napi::Number::New(env, static_cast<double>(owned->utterance_id)));
  arg.Set("phonemes", BuildPhonemesArray(env, owned->phonemes));
  arg.Set("sampleRate", Napi::Number::New(env, owned->sample_rate));
  callback.Call({arg});
}

using PhonemeTSFN = Napi::TypedThreadSafeFunction<std::nullptr_t, PhonemeCallbackData,
                                                  InvokePhonemeCallback>;

// ---- Completion delivery (persistent TSFN, registered via setOnComplete) --
struct CompletionCallbackData {
  int64_t utterance_id;
  bool ok;
  std::string error;
};

void InvokeCompletionCallback(Napi::Env env, Napi::Function callback,
                              std::nullptr_t *, CompletionCallbackData *data) {
  std::unique_ptr<CompletionCallbackData> owned(data);
  if (env == nullptr || callback == nullptr) return;
  Napi::Object arg = Napi::Object::New(env);
  arg.Set("utteranceId",
          Napi::Number::New(env, static_cast<double>(owned->utterance_id)));
  arg.Set("ok", Napi::Boolean::New(env, owned->ok));
  if (!owned->ok) arg.Set("error", Napi::String::New(env, owned->error));
  callback.Call({arg});
}

using CompletionTSFN =
    Napi::TypedThreadSafeFunction<std::nullptr_t, CompletionCallbackData,
                                  InvokeCompletionCallback>;

std::mutex g_completion_tsfn_mu;
bool g_completion_tsfn_set = false;
CompletionTSFN g_completion_tsfn;

void FireCompletion(int64_t utterance_id, bool ok, const std::string &error) {
  std::lock_guard<std::mutex> lk(g_completion_tsfn_mu);
  if (!g_completion_tsfn_set) {
    fprintf(stderr,
            "[tts-playback] completion for utterance %lld dropped -- "
            "setOnComplete() was never called\n",
            static_cast<long long>(utterance_id));
    return;
  }
  auto *data = new CompletionCallbackData{utterance_id, ok, error};
  g_completion_tsfn.NonBlockingCall(data);
}

// ---- Arming thread --------------------------------------------------------
void ArmingThreadMain() {
  while (true) {
    std::shared_ptr<Utterance> utt;
    {
      std::unique_lock<std::mutex> lk(g_pending_mu);
      g_pending_cv.wait(lk, [] {
        return g_pending_utt != nullptr ||
               g_shutdown.load(std::memory_order_acquire);
      });
      if (g_shutdown.load(std::memory_order_acquire) && !g_pending_utt) {
        return;
      }
      utt = g_pending_utt;
      g_pending_utt.reset();
    }
    if (!utt) continue;

    GateResult gr = g_ready_gate.WaitBoth(std::chrono::milliseconds(2000));
    if (gr != GateResult::kOk) {
      fprintf(stderr,
              "[tts-playback] utterance %lld: ReadyGate timeout (%s) -- "
              "force-starting playback\n",
              static_cast<long long>(utt->id), GateResultToString(gr));
    }

    g_playback.Play(utt);
    g_state.store(UtteranceState::kPlaying, std::memory_order_release);

    while (!utt->playback_done.load(std::memory_order_acquire)) {
      if (g_shutdown.load(std::memory_order_acquire)) break;
      std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    // Distinguish "finished playing" from "loop exited because shutdown
    // fired mid-playback" -- the latter must not be reported as success.
    bool completed_normally =
        utt->playback_done.load(std::memory_order_acquire);
    g_playback.Pause();

    // Single reset point for the happy path: right after Pause(),
    // immediately before firing the completion callback.
    g_state.store(UtteranceState::kIdle, std::memory_order_release);
    {
      std::lock_guard<std::mutex> lk(g_active_mu);
      g_active_utt.reset();
    }
    if (completed_normally) {
      FireCompletion(utt->id, /*ok=*/true, "");
    } else {
      FireCompletion(utt->id, /*ok=*/false,
                     "playback interrupted by addon shutdown");
    }

    if (g_shutdown.load(std::memory_order_acquire)) return;
  }
}

void EnsureArmingThreadStarted() {
  static std::once_flag once;
  std::call_once(once, [] { g_arming_thread = std::thread(ArmingThreadMain); });
}

// ---- speak(ttsHandle, text, sid, speed, onPhonemes) -----------------------
Napi::Value SpeakWrapper(const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();

  if (info.Length() != 5 || !info[0].IsExternal() || !info[1].IsString() ||
      !info[2].IsNumber() || !info[3].IsNumber() || !info[4].IsFunction()) {
    Napi::TypeError::New(env, "Expect (ttsHandle, text, sid, speed, onPhonemes)")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  if (!g_playback_started.load(std::memory_order_acquire)) {
    Napi::Error::New(env, "startPlayback(ttsHandle) must be called before speak()")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  // Fix 4 finding: speed = NaN reaches SherpaOnnxOfflineTtsGenerateWithConfig
  // and throws an uncaught exception from inside onnxruntime that crashes
  // the whole process (RunSynthesis() now catches that as a safety net).
  // Reject the known-bad shapes here too, synchronously, before a thread is
  // even spawned. (speed <= 0 is already rejected inside the C API itself
  // with a clean null return; checking it here too just avoids that round
  // trip.)
  float speed_check = info[3].As<Napi::Number>().FloatValue();
  if (!std::isfinite(speed_check) || speed_check <= 0.0f) {
    Napi::RangeError::New(env, "speed must be a finite number > 0")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  UtteranceState expected = UtteranceState::kIdle;
  if (!g_state.compare_exchange_strong(expected, UtteranceState::kSynthesizing,
                                        std::memory_order_acq_rel)) {
    Napi::Error::New(
        env, "An utterance is already being spoken; wait for the "
             "onComplete callback (ok or error) before calling speak() again.")
        .ThrowAsJavaScriptException();
    return env.Undefined();
  }

  auto *tts = info[0].As<Napi::External<SherpaOnnxOfflineTts>>().Data();
  std::string text = info[1].As<Napi::String>().Utf8Value();
  int32_t sid = info[2].As<Napi::Number>().Int32Value();
  float speed = info[3].As<Napi::Number>().FloatValue();
  Napi::Function on_phonemes = info[4].As<Napi::Function>();

  int64_t id = g_next_utterance_id.fetch_add(1, std::memory_order_relaxed);
  g_current_utterance_id.store(id, std::memory_order_release);

  auto utt = std::make_shared<Utterance>();
  utt->id = id;

  // Reset the gate BEFORE spawning the synth thread -- and therefore
  // strictly before either SetAudioReady() or a renderer-sent
  // onRendererReady(id) for THIS utterance can possibly fire. Resetting
  // later (e.g. only when the arming thread dequeues) would risk wiping
  // out a renderer-ready signal that legitimately arrived early.
  g_ready_gate.Reset();

  {
    std::lock_guard<std::mutex> lk(g_active_mu);
    g_active_utt = utt;
  }

  PhonemeTSFN phoneme_tsfn = PhonemeTSFN::New(
      env, on_phonemes, "TtsPlaybackPhonemes", 0, 1, nullptr,
      [](Napi::Env, void *, std::nullptr_t *) {});

  // Reap the previous synth thread (should already be finished -- speak()
  // only reaches here when g_state was Idle, meaning the previous
  // utterance's synth thread already ran to completion) before starting a
  // new one. Unconditional join at teardown covers the case where none of
  // this ever ran.
  {
    std::lock_guard<std::mutex> lk(g_synth_thread_mu);
    if (g_synth_thread.joinable()) g_synth_thread.join();

    g_synth_thread = std::thread([tts, text, sid, speed, utt,
                                  phoneme_tsfn]() mutable {
      int32_t target_rate = g_playback.NegotiatedSampleRate();
      SynthResult res = RunSynthesis(tts, text, sid, speed, target_rate, utt.get());

      if (!res.ok) {
        g_state.store(UtteranceState::kIdle, std::memory_order_release);
        {
          std::lock_guard<std::mutex> lk(g_active_mu);
          g_active_utt.reset();
        }
        FireCompletion(utt->id, /*ok=*/false, res.error);
        phoneme_tsfn.Release();
        return;
      }

      utt->audio_ready.store(true, std::memory_order_release);
      g_state.store(UtteranceState::kGated, std::memory_order_release);

      if (!g_shutdown.load(std::memory_order_acquire)) {
        {
          std::lock_guard<std::mutex> lk(g_pending_mu);
          g_pending_utt = utt;
        }
        g_pending_cv.notify_one();
      }

      g_ready_gate.SetAudioReady();

      // Same guard as the pending-utterance enqueue above: don't attempt
      // delivery into a JS environment that may already be tearing down.
      // The allocation itself must stay inside the guard too -- it's only
      // freed by InvokePhonemeCallback's unique_ptr, which never runs if
      // NonBlockingCall is skipped, so allocating unconditionally would
      // leak on the shutdown path.
      if (!g_shutdown.load(std::memory_order_acquire)) {
        auto *data = new PhonemeCallbackData{utt->id, utt->phonemes,
                                             utt->sample_rate};
        phoneme_tsfn.NonBlockingCall(data);
      }
      phoneme_tsfn.Release();
    });
  }

  return Napi::Number::New(env, static_cast<double>(id));
}

// ---- startPlayback(ttsHandle) ---------------------------------------------
Napi::Value StartPlaybackWrapper(const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();

  if (info.Length() != 1 || !info[0].IsExternal()) {
    Napi::TypeError::New(env, "Expect (ttsHandle)").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  if (g_playback_started.load(std::memory_order_acquire)) {
    return Napi::Boolean::New(env, true);  // idempotent
  }

  auto *tts = info[0].As<Napi::External<SherpaOnnxOfflineTts>>().Data();
  int32_t rate = SherpaOnnxOfflineTtsSampleRate(tts);

  bool ok = g_playback.Start(rate);
  if (ok) {
    // Pin the handle for the addon's lifetime -- speak() will hold and
    // dereference the raw pointer extracted from it on a background
    // thread, for as long as this addon considers playback "started",
    // with no other mechanism keeping it reachable from V8's perspective.
    g_tts_handle_ref = Napi::Persistent(info[0]);
    g_playback_started.store(true, std::memory_order_release);
    EnsureArmingThreadStarted();
  }
  return Napi::Boolean::New(env, ok);
}

// ---- onRendererReady(utteranceId) -----------------------------------------
Napi::Value OnRendererReadyWrapper(const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();
  if (info.Length() != 1 || !info[0].IsNumber()) {
    Napi::TypeError::New(env, "Expect (utteranceId)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  int64_t id = static_cast<int64_t>(info[0].As<Napi::Number>().DoubleValue());
  int64_t current = g_current_utterance_id.load(std::memory_order_acquire);
  if (id != current) {
    fprintf(stderr,
            "[tts-playback] onRendererReady(%lld) ignored -- current "
            "utterance is %lld\n",
            static_cast<long long>(id), static_cast<long long>(current));
    return env.Undefined();
  }
  g_ready_gate.SetRendererReady();
  return env.Undefined();
}

// ---- stopSpeak(utteranceId) ------------------------------------------------
Napi::Value StopSpeakWrapper(const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();
  if (info.Length() != 1 || !info[0].IsNumber()) {
    Napi::TypeError::New(env, "Expect (utteranceId)").ThrowAsJavaScriptException();
    return env.Undefined();
  }
  int64_t id = static_cast<int64_t>(info[0].As<Napi::Number>().DoubleValue());

  std::lock_guard<std::mutex> lk(g_active_mu);
  if (g_active_utt && g_active_utt->id == id) {
    g_active_utt->cancel.store(true, std::memory_order_release);
    return Napi::Boolean::New(env, true);
  }
  return Napi::Boolean::New(env, false);
}

// ---- setOnComplete(callback) -----------------------------------------------
Napi::Value SetOnCompleteWrapper(const Napi::CallbackInfo &info) {
  Napi::Env env = info.Env();
  if (info.Length() != 1 || !info[0].IsFunction()) {
    Napi::TypeError::New(env, "Expect (callback)").ThrowAsJavaScriptException();
    return env.Undefined();
  }

  std::lock_guard<std::mutex> lk(g_completion_tsfn_mu);
  g_completion_tsfn = CompletionTSFN::New(
      env, info[0].As<Napi::Function>(), "TtsPlaybackCompletion", 0, 1, nullptr,
      [](Napi::Env, void *, std::nullptr_t *) {});
  // A ThreadSafeFunction is ref'd (keeps the event loop alive) by default.
  // This one is persistent -- created once, released only from the
  // cleanup hook -- so left ref'd it would keep the Node process alive
  // forever, and the process would never reach the point of tearing down
  // its environment (which is exactly what's supposed to trigger our
  // cleanup hook / Release() call). Unref it so normal event-loop-drain
  // exit still works; the cleanup hook still runs synchronously during
  // teardown regardless of ref/unref state.
  g_completion_tsfn.Unref(env);
  g_completion_tsfn_set = true;
  return env.Undefined();
}

// ---- Teardown, registered via napi_add_env_cleanup_hook (through
// Napi::Env::AddCleanupHook) -- deterministic order:
//   1. Signal shutdown, wake the arming thread's queue-wait and poll loop.
//   2. Join the arming thread.
//   3+4. PwPlayback::Shutdown() -- pause/disconnect/destroy the stream,
//        stop+destroy the PipeWire thread loop, pw_deinit(). No-op if
//        Start() was never called.
//   5. Join the tracked synth thread unconditionally (accept the risk of a
//      hang here rather than silently detach-and-leak -- a hang indicates
//      a real bug, since inference finishes in well under a second in
//      practice).
//   6. Release the TTS handle pin, now that the synth thread (the last
//      thing that could dereference the raw pointer) has been joined.
//   7. Release the persistent completion TSFN (only after the arming
//      thread -- the only other thing that can fire it -- has been
//      joined, so no straggler can call it post-release).
//   8. ReadyGate / pending-utterance-queue destructors run automatically
//      (plain globals, single-threaded by this point).
void TeardownPlayback() {
  g_shutdown.store(true, std::memory_order_release);
  g_pending_cv.notify_all();

  if (g_arming_thread.joinable()) g_arming_thread.join();

  g_playback.Shutdown();

  {
    std::lock_guard<std::mutex> lk(g_synth_thread_mu);
    if (g_synth_thread.joinable()) g_synth_thread.join();
  }

  g_tts_handle_ref.Reset();

  {
    std::lock_guard<std::mutex> lk(g_completion_tsfn_mu);
    if (g_completion_tsfn_set) {
      g_completion_tsfn.Release();
      g_completion_tsfn_set = false;
    }
  }
}

}  // namespace

void RegisterTtsPlaybackExports(Napi::Env env, Napi::Object exports) {
  exports.Set(Napi::String::New(env, "startPlayback"),
              Napi::Function::New(env, StartPlaybackWrapper));
  exports.Set(Napi::String::New(env, "speak"),
              Napi::Function::New(env, SpeakWrapper));
  exports.Set(Napi::String::New(env, "onRendererReady"),
              Napi::Function::New(env, OnRendererReadyWrapper));
  exports.Set(Napi::String::New(env, "stopSpeak"),
              Napi::Function::New(env, StopSpeakWrapper));
  exports.Set(Napi::String::New(env, "setOnComplete"),
              Napi::Function::New(env, SetOnCompleteWrapper));

  env.AddCleanupHook([]() { TeardownPlayback(); });
}

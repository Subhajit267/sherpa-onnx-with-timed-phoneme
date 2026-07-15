// scripts/node-addon-api/src/tts-playback.h
//
// Additive, non-blocking PipeWire audio playback for offline TTS, layered
// on top of (not replacing) the phoneme-generation JS API in
// non-streaming-tts.cc. Personal, single-deployment project: Jetson,
// Kokoro v0.19 (Piper-based, single-language) only -- see tts-playback.cc
// for the full design rationale and RT-safety invariants.
#ifndef SHERPA_ONNX_NODE_ADDON_API_TTS_PLAYBACK_H_
#define SHERPA_ONNX_NODE_ADDON_API_TTS_PLAYBACK_H_

#include <napi.h>

#include <string>
#include <vector>

#include "sherpa-onnx/c-api/c-api.h"

// ---- Shared phoneme-array helpers ----------------------------------------
// The one implementation of "timed phoneme data -> JS array" in the addon,
// used by every generate path: the 5 existing sync/async call sites in
// non-streaming-tts.cc (which have a live SherpaOnnxGeneratedAudio* and a
// live Napi::Env at the same time) and the playback path below (whose synth
// thread must copy phoneme data into an owned structure *before* the
// C-owned struct is destroyed, then build the Napi::Array later, on the
// main thread, once it has a live Env). Both shapes funnel through the same
// BuildPhonemesArray() so there is exactly one field mapping to maintain.
struct PhonemeTiming {
  std::string phoneme;
  int32_t id;
  float startMs;
  float endMs;
};

inline std::vector<PhonemeTiming> CopyPhonemeTimings(
    const SherpaOnnxGeneratedAudio *audio) {
  std::vector<PhonemeTiming> out;
  out.reserve(audio->num_phonemes);
  for (int32_t i = 0; i < audio->num_phonemes; ++i) {
    const SherpaOnnxTimedPhoneme &p = audio->phonemes[i];
    out.push_back(PhonemeTiming{p.phoneme ? p.phoneme : "", p.id,
                                 p.start_second * 1000.0f,
                                 p.end_second * 1000.0f});
  }
  return out;
}

inline Napi::Array BuildPhonemesArray(Napi::Env env,
                                       const std::vector<PhonemeTiming> &ph) {
  Napi::Array arr = Napi::Array::New(env, ph.size());
  for (size_t i = 0; i < ph.size(); ++i) {
    Napi::Object o = Napi::Object::New(env);
    o.Set("phoneme", Napi::String::New(env, ph[i].phoneme));
    o.Set("id", Napi::Number::New(env, ph[i].id));
    o.Set("startMs", Napi::Number::New(env, ph[i].startMs));
    o.Set("endMs", Napi::Number::New(env, ph[i].endMs));
    arr[i] = o;
  }
  return arr;
}

// Convenience one-liner for the 5 existing call sites in non-streaming-tts.cc,
// which always have both a live audio pointer and a live env at hand.
inline Napi::Array BuildPhonemesArray(Napi::Env env,
                                       const SherpaOnnxGeneratedAudio *audio) {
  return BuildPhonemesArray(env, CopyPhonemeTimings(audio));
}

// Registers startPlayback/speak/onRendererReady/stopSpeak/setOnComplete on
// `exports` and the addon-teardown cleanup hook. Called once from
// non-streaming-tts.cc's InitNonStreamingTts -- this is the only symbol
// another file needs from the playback feature.
void RegisterTtsPlaybackExports(Napi::Env env, Napi::Object exports);

#endif  // SHERPA_ONNX_NODE_ADDON_API_TTS_PLAYBACK_H_

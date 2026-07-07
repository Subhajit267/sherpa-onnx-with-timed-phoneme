// sherpa-onnx/csrc/offline-tts.cc
//
// Copyright (c)  2023  Xiaomi Corporation
//
// Phoneme timing adaptation:
//   Added BuildTimedPhonemes() helper and, in each Generate() overload,
//   if enable_timed_phonemes is set and the model has a frontend,
//   phoneme spans are converted to timed phonemes using uniform
//   token durations.  This works for all TTS models.
#include "sherpa-onnx/csrc/offline-tts.h"

#include <cmath>
#include <map>
#include <string>
#include <utility>
#include <vector>

#if __ANDROID_API__ >= 9
#include "android/asset_manager.h"
#include "android/asset_manager_jni.h"
#endif

#if __OHOS__
#include "rawfile/raw_file_manager.h"
#endif

#include "sherpa-onnx/csrc/file-utils.h"
#include "sherpa-onnx/csrc/macros.h"
#include "sherpa-onnx/csrc/offline-tts-frontend.h"  // for PhonemeSpan
#include "sherpa-onnx/csrc/offline-tts-impl.h"
#include "sherpa-onnx/csrc/text-utils.h"

namespace sherpa_onnx {

struct SilenceInterval {
  int32_t start;
  int32_t end;
};

// ========== Phoneme timing helper (added) ==========
static std::vector<TimedPhoneme> BuildTimedPhonemes(
    const std::vector<PhonemeSpan> &spans,
    int32_t total_tokens,
    int32_t num_audio_samples,
    float sample_rate) {
  if (spans.empty() || total_tokens <= 0 || num_audio_samples == 0)
    return {};

  float total_duration = num_audio_samples / sample_rate;
  float duration_per_token = total_duration / total_tokens;

  std::vector<TimedPhoneme> result;
  int32_t id = 1;
  for (const auto &span : spans) {
    TimedPhoneme tp;
    tp.phoneme = span.phoneme;
    tp.id = id++;
    tp.start_second = span.start_token * duration_per_token;
    tp.end_second = span.end_token * duration_per_token;
    result.push_back(tp);
  }
  return result;
}
// ====================================================

GeneratedAudio GeneratedAudio::ScaleSilence(float scale) const {
  if (scale == 1) {
    return *this;
  }
  // if the interval is larger than 0.2 second, then we assume it is a pause
  int32_t threshold = static_cast<int32_t>(sample_rate * 0.2);

  std::vector<SilenceInterval> intervals;
  int32_t num_samples = static_cast<int32_t>(samples.size());

  int32_t last = -1;
  int32_t i;
  for (i = 0; i != num_samples; ++i) {
    if (fabs(samples[i]) <= 0.01) {
      if (last == -1) {
        last = i;
      }
      continue;
    }

    if (last != -1 && i - last < threshold) {
      last = -1;
      continue;
    }

    if (last != -1) {
      intervals.push_back({last, i});
      last = -1;
    }
  }

  if (last != -1 && num_samples - last > threshold) {
    intervals.push_back({last, num_samples});
  }

  if (intervals.empty()) {
    return *this;
  }

  GeneratedAudio ans;
  ans.sample_rate = sample_rate;
  ans.samples.reserve(samples.size());

  i = 0;
  for (const auto &interval : intervals) {
    ans.samples.insert(ans.samples.end(), samples.begin() + i,
                       samples.begin() + interval.start);
    i = interval.end;
    int32_t n = static_cast<int32_t>((interval.end - interval.start) * scale);

    ans.samples.insert(ans.samples.end(), samples.begin() + interval.start,
                       samples.begin() + interval.start + n);
  }

  if (i < num_samples) {
    ans.samples.insert(ans.samples.end(), samples.begin() + i, samples.end());
  }

  return ans;
}

std::string GenerationConfig::GetExtraString(
    const std::string &key, const std::string &def /*= ""*/) const {
  auto it = extra.find(key);
  return it == extra.end() ? def : it->second;
}

int32_t GenerationConfig::GetExtraInt(const std::string &key,
                                      int32_t def) const {
  auto it = extra.find(key);
  if (it == extra.end()) {
    return def;
  }

  return ToIntOrDefault(it->second, def);
}

float GenerationConfig::GetExtraFloat(const std::string &key, float def) const {
  auto it = extra.find(key);
  if (it == extra.end()) {
    return def;
  }

  return ToFloatOrDefault(it->second, def);
}

std::string GenerationConfig::ToString() const {
  std::ostringstream os;

  os << "GenerationConfig(";
  os << "silence_scale=" << silence_scale;
  os << ", speed=" << speed;
  os << ", sid=" << sid;
  os << ", num_steps=" << num_steps;
  os << ", reference_audio_len=" << reference_audio.size();
  os << ", reference_sample_rate=" << reference_sample_rate;

  if (!reference_text.empty()) {
    os << ", reference_text=\"" << reference_text << "\"";
  }

  if (!extra.empty()) {
    os << ", extra={";
    std::string sep;

    std::map<std::string, std::string> sorted(extra.begin(), extra.end());

    for (const auto &kv : sorted) {
      os << sep << kv.first << ": \"" << kv.second << "\"";
      sep = ", ";
    }
    os << "}";
  }

  os << ")";
  return os.str();
}

void OfflineTtsConfig::Register(ParseOptions *po) {
  model.Register(po);

  po->Register("tts-rule-fsts", &rule_fsts,
               "It not empty, it contains a list of rule FST filenames."
               "Multiple filenames are separated by a comma and they are "
               "applied from left to right. An example value: "
               "rule1.fst,rule2.fst,rule3.fst");

  po->Register("tts-rule-fars", &rule_fars,
               "It not empty, it contains a list of rule FST archive filenames."
               "Multiple filenames are separated by a comma and they are "
               "applied from left to right. An example value: "
               "rule1.far,rule2.far,rule3.far. Note that an *.far can contain "
               "multiple *.fst files");

  po->Register(
      "tts-max-num-sentences", &max_num_sentences,
      "Maximum number of sentences that we process at a time. "
      "This is to avoid OOM for very long input text. "
      "If you set it to -1, then we process all sentences in a single batch.");

  po->Register("tts-silence-scale", &silence_scale,
               "Duration of the pause is scaled by this number. So a smaller "
               "value leads to a shorter pause.");

  // ========== Phoneme timing adaptation ==========
  po->Register("tts-enable-timed-phonemes", &enable_timed_phonemes,
               "If true, GeneratedAudio will contain a timed phoneme list "
               "for each generated utterance. Used for avatar lip‑sync.");
  // =================================================
}

bool OfflineTtsConfig::Validate() const {
  if (!rule_fsts.empty()) {
    std::vector<std::string> files;
    SplitStringToVector(rule_fsts, ",", false, &files);
    for (const auto &f : files) {
      if (!FileExists(f)) {
        SHERPA_ONNX_LOGE("Rule fst '%s' does not exist. ", f.c_str());
        return false;
      }
    }
  }

  if (!rule_fars.empty()) {
    std::vector<std::string> files;
    SplitStringToVector(rule_fars, ",", false, &files);
    for (const auto &f : files) {
      if (!FileExists(f)) {
        SHERPA_ONNX_LOGE("Rule far '%s' does not exist. ", f.c_str());
        return false;
      }
    }
  }

  if (silence_scale < 0.001) {
    SHERPA_ONNX_LOGE("--tts-silence-scale '%.3f' is too small", silence_scale);
    return false;
  }

  return model.Validate();
}

std::string OfflineTtsConfig::ToString() const {
  std::ostringstream os;

  os << "OfflineTtsConfig(";
  os << "model=" << model.ToString() << ", ";
  os << "rule_fsts=\"" << rule_fsts << "\", ";
  os << "rule_fars=\"" << rule_fars << "\", ";
  os << "max_num_sentences=" << max_num_sentences << ", ";
  os << "silence_scale=" << silence_scale << ", ";
  os << "enable_timed_phonemes=" << (enable_timed_phonemes ? "true" : "false")
     << ")";

  return os.str();
}

// ========== Constructor now stores config ==========
OfflineTts::OfflineTts(const OfflineTtsConfig &config)
    : config_(config), impl_(OfflineTtsImpl::Create(config)) {}

template <typename Manager>
OfflineTts::OfflineTts(Manager *mgr, const OfflineTtsConfig &config)
    : config_(config), impl_(OfflineTtsImpl::Create(mgr, config)) {}
// ===================================================

OfflineTts::~OfflineTts() = default;

// ------------------------------------------------------------------
// Overload 1: simple sid/speed
// ------------------------------------------------------------------
GeneratedAudio OfflineTts::Generate(
    const std::string &text, int64_t sid /*=0*/, float speed /*= 1.0*/,
    GeneratedAudioCallback callback /*= nullptr*/) const {
  GenerationConfig config;
  config.sid = static_cast<int32_t>(sid);
  config.speed = speed;
  GeneratedAudio ans;
#if !defined(_WIN32)
  ans = impl_->Generate(text, config, std::move(callback));
#else
  if (IsUtf8(text)) {
    ans = impl_->Generate(text, config, std::move(callback));
  } else if (IsGB2312(text)) {
    auto utf8_text = Gb2312ToUtf8(text);
    static bool printed = false;
    if (!printed) {
      SHERPA_ONNX_LOGE(
          "Detected GB2312 encoded string! Converting it to UTF8.");
      printed = true;
    }
    ans = impl_->Generate(utf8_text, config, std::move(callback));
  } else {
    SHERPA_ONNX_LOGE(
        "Non UTF8 encoded string is received. You would not get expected "
        "results!");
    ans = impl_->Generate(text, config, std::move(callback));
  }
#endif

  // ========== Phoneme timing adaptation ==========
  if (config_.enable_timed_phonemes) {
    auto *frontend = impl_->GetFrontend();
    if (frontend) {
      auto spans = frontend->ConvertTextToPhonemeSpans(text, "");
      if (!spans.empty()) {
        int32_t total_tokens = 0;
        for (const auto &s : spans)
          if (s.end_token > total_tokens) total_tokens = s.end_token;
        ans.phonemes = BuildTimedPhonemes(spans, total_tokens,
                                          ans.samples.size(),
                                          ans.sample_rate);
      }
    }
  }
  // =================================================
  return ans;
}

// ------------------------------------------------------------------
// Overload 2: prompt‑conditioned (deprecated)
// ------------------------------------------------------------------
GeneratedAudio OfflineTts::Generate(
    const std::string &text, const std::string &prompt_text,
    const std::vector<float> &prompt_samples, int32_t sample_rate,
    float speed /*=1.0*/, int32_t num_steps /*=4*/,
    GeneratedAudioCallback callback /*=nullptr*/) const {
  GenerationConfig config;
  config.speed = speed;
  config.reference_audio = prompt_samples;
  config.reference_sample_rate = sample_rate;
  config.reference_text = prompt_text;
  config.num_steps = num_steps;
  GeneratedAudio ans;
#if !defined(_WIN32)
  ans = impl_->Generate(text, config, std::move(callback));
#else
  static bool printed = false;
  auto utf8_text = text;
  if (IsGB2312(text)) {
    utf8_text = Gb2312ToUtf8(text);
    if (!printed) {
      SHERPA_ONNX_LOGE("Detected GB2312 encoded text! Converting it to UTF8.");
      printed = true;
    }
  }
  auto utf8_prompt_text = prompt_text;
  if (IsGB2312(prompt_text)) {
    utf8_prompt_text = Gb2312ToUtf8(prompt_text);
    if (!printed) {
      SHERPA_ONNX_LOGE(
          "Detected GB2312 encoded prompt text! Converting it to UTF8.");
      printed = true;
    }
  }
  config.reference_text = utf8_prompt_text;
  if (IsUtf8(utf8_text) && IsUtf8(utf8_prompt_text)) {
    ans = impl_->Generate(utf8_text, config, std::move(callback));
  } else {
    SHERPA_ONNX_LOGE(
        "Non UTF8 encoded string is received. You would not get expected "
        "results!");
    ans = impl_->Generate(utf8_text, config, std::move(callback));
  }
#endif

  // ========== Phoneme timing adaptation ==========
  if (config_.enable_timed_phonemes) {
    auto *frontend = impl_->GetFrontend();
    if (frontend) {
      auto spans = frontend->ConvertTextToPhonemeSpans(text, "");
      if (!spans.empty()) {
        int32_t total_tokens = 0;
        for (const auto &s : spans)
          if (s.end_token > total_tokens) total_tokens = s.end_token;
        ans.phonemes = BuildTimedPhonemes(spans, total_tokens,
                                          ans.samples.size(),
                                          ans.sample_rate);
      }
    }
  }
  // =================================================
  return ans;
}

// ------------------------------------------------------------------
// Overload 3: GenerationConfig (the preferred modern API)
// ------------------------------------------------------------------
GeneratedAudio OfflineTts::Generate(
    const std::string &text, const GenerationConfig &config,
    GeneratedAudioCallback callback /*= nullptr*/) const {
  GeneratedAudio ans;
#if !defined(_WIN32)
  ans = impl_->Generate(text, config, std::move(callback));
#else
  if (IsUtf8(text)) {
    ans = impl_->Generate(text, config, std::move(callback));
  } else if (IsGB2312(text)) {
    auto utf8_text = Gb2312ToUtf8(text);
    static bool printed = false;
    if (!printed) {
      SHERPA_ONNX_LOGE(
          "Detected GB2312 encoded string! Converting it to UTF8.");
      printed = true;
    }
    ans = impl_->Generate(utf8_text, config, std::move(callback));
  } else {
    SHERPA_ONNX_LOGE(
        "Non UTF8 encoded string is received. You would not get expected "
        "results!");
    ans = impl_->Generate(text, config, std::move(callback));
  }
#endif

  // ========== Phoneme timing adaptation ==========
  if (config_.enable_timed_phonemes) {
    auto *frontend = impl_->GetFrontend();
    if (frontend) {
      auto spans = frontend->ConvertTextToPhonemeSpans(text, "");
      if (!spans.empty()) {
        int32_t total_tokens = 0;
        for (const auto &s : spans)
          if (s.end_token > total_tokens) total_tokens = s.end_token;
        ans.phonemes = BuildTimedPhonemes(spans, total_tokens,
                                          ans.samples.size(),
                                          ans.sample_rate);
      }
    }
  }
  // =================================================
  return ans;
}

int32_t OfflineTts::SampleRate() const { return impl_->SampleRate(); }

int32_t OfflineTts::NumSpeakers() const { return impl_->NumSpeakers(); }

#if __ANDROID_API__ >= 9
template OfflineTts::OfflineTts(AAssetManager *mgr,
                                const OfflineTtsConfig &config);
#endif

#if __OHOS__
template OfflineTts::OfflineTts(NativeResourceManager *mgr,
                                const OfflineTtsConfig &config);
#endif

}  // namespace sherpa_onnx

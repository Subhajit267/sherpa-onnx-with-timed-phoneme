// sherpa-onnx/csrc/offline-tts-frontend.h
//
// Copyright (c)  2023  Xiaomi Corporation
//
// Phoneme timing adaptation:
//   Added ConvertTextToPhonemeSpans() virtual method.  Any frontend that
//   can produce phoneme spans (e.g. Piper phonemize) overrides it.
//   The default returns an empty vector so the feature degrades gracefully
//   for models that do not natively expose phonemes.
#ifndef SHERPA_ONNX_CSRC_OFFLINE_TTS_FRONTEND_H_
#define SHERPA_ONNX_CSRC_OFFLINE_TTS_FRONTEND_H_

#include <cstdint>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "sherpa-onnx/csrc/macros.h"
#include "sherpa-onnx/csrc/offline-tts.h"  // for PhonemeSpan

namespace sherpa_onnx {

struct TokenIDs {
  TokenIDs() = default;

  /*implicit*/ TokenIDs(std::vector<int64_t> tokens)  // NOLINT
      : tokens{std::move(tokens)} {}

  /*implicit*/ TokenIDs(const std::vector<int32_t> &tokens)  // NOLINT
      : tokens{tokens.begin(), tokens.end()} {}

  TokenIDs(std::vector<int64_t> tokens,  // NOLINT
           std::vector<int64_t> tones)   // NOLINT
      : tokens{std::move(tokens)}, tones{std::move(tones)} {}

  std::string ToString() const;

  std::vector<int64_t> tokens;

  // Used only in MeloTTS
  std::vector<int64_t> tones;
};

class OfflineTtsFrontend {
 public:
  virtual ~OfflineTtsFrontend() = default;

  /** Convert a string to token IDs.
   *
   * @param text The input text.
   *             Example 1: "This is the first sample sentence; this is the
   *             second one." Example 2: "这是第一句。这是第二句。"
   * @param voice Optional. It is for espeak-ng.
   *
   * @return Return a vector-of-vector of token IDs. Each subvector contains
   *         a sentence that can be processed independently.
   *         If a frontend does not support splitting the text into sentences,
   *         the resulting vector contains only one subvector.
   */
  virtual std::vector<TokenIDs> ConvertTextToTokenIds(
      const std::string &text, const std::string &voice = "") const = 0;

  // ========== Phoneme timing adaptation ==========
  /** Convert text to phoneme spans that map phonemes to token indices.
   *
   *  Each PhonemeSpan records a phoneme string and the range of token
   *  indices that correspond to it.  The token indices refer to the
   *  flattened token sequence produced by ConvertTextToTokenIds().
   *
   *  The default implementation returns an empty vector, which means
   *  the frontend cannot provide phoneme information.
   *
   *  @param text  Input text (same as for ConvertTextToTokenIds).
   *  @param voice Optional voice parameter for espeak-ng.
   *  @return Ordered list of PhonemeSpan, or empty if unsupported.
   */
  virtual std::vector<PhonemeSpan> ConvertTextToPhonemeSpans(
      const std::string &text, const std::string &voice = "") const {
    return {};
  }
  // =================================================
};

// implementation is in ./piper-phonemize-lexicon.cc
void InitEspeak(const std::string &data_dir);

// implementation in ./piper-phonemize-lexicon.cc
std::vector<TokenIDs> ConvertTextToTokenIdsKokoroOrKitten(
    const std::unordered_map<char32_t, int32_t> &token2id,
    int32_t max_token_len, const std::string &text,
    const std::string &voice = "");

}  // namespace sherpa_onnx

#endif  // SHERPA_ONNX_CSRC_OFFLINE_TTS_FRONTEND_H_

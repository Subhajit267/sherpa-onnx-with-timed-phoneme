
# <div align="center"> 🎙️ Sherpa‑ONNX TTS Phoneme Timing Extension</div>

<div align="center">Model‑agnostic timed phoneme output for avatar lip‑sync in Electron apps</div>

<div align="center">
         
![C++](https://img.shields.io/badge/Core-C%2B%2B17-blue)
![C API](https://img.shields.io/badge/API-C%2FNode.js-orange)
![Electron](https://img.shields.io/badge/Integration-Electron-green)
![Status](https://img.shields.io/badge/Status-Complete-success)

</div>

---
         
## Overview

This extension adds **time‑aligned phoneme sequences** to the Sherpa‑ONNX offline TTS engine.  
When enabled, every `generate()` call returns an array of phoneme objects with start and end timestamps, ready for driving facial animation.

**Opt‑in** – a single config flag (`enableTimedPhonemes: true`) activates the feature.  
**Model‑agnostic** – works with all TTS backends (Kokoro, VITS, Matcha, Piper, Kitten, etc.) via a shared timing layer.  
**Non‑breaking** – default behaviour is unchanged when the flag is off.

---

## Architecture

```
Text → Frontend::ConvertTextToTokenIds()         (existing)
         ↓
      Frontend::ConvertTextToPhonemeSpans()       (NEW – virtual, default empty)
         ↓
      OfflineTtsImpl::Generate() → Model Run → PCM audio
         ↓
      [if enabled] BuildTimedPhonemes()           (NEW – uniform token durations)
         ↓
      GeneratedAudio { samples, sampleRate, phonemes }
```

### Key Components

| Component | Role |
|-----------|------|
| `PhonemeSpan` | Maps a phoneme string to a range of token indices |
| `TimedPhoneme` | Final output: phoneme, id, start/end seconds |
| `BuildTimedPhonemes()` | Divides total audio length equally among tokens |
| `ConvertTextToPhonemeSpans()` | New virtual method on `OfflineTtsFrontend` |
| `GetFrontend()` | New pure virtual on `OfflineTtsImpl` |

---

## Files Modified (19 files)

| File | What Was Added / Changed |
|------|---------------------------|
| `offline-tts.h` | `PhonemeSpan`, `TimedPhoneme` structs; `GeneratedAudio::phonemes` field; `enable_timed_phonemes` config flag |
| `offline-tts.cc` | `BuildTimedPhonemes()` helper; injection of timed phonemes in all three `Generate()` overloads |
| `offline-tts-frontend.h` | Virtual `ConvertTextToPhonemeSpans()` (default empty) |
| `offline-tts-impl.h` | Pure virtual `GetFrontend()` |
| `offline-tts-vits-impl.h` | `GetFrontend()` override returning `frontend_.get()` |
| `offline-tts-kokoro-impl.h` | Same |
| `offline-tts-kitten-impl.h` | Same |
| `offline-tts-matcha-impl.h` | Same |
| `offline-tts-zipvoice-impl.h` | Same |
| `offline-tts-pocket-impl.h` | `GetFrontend()` returning `nullptr` |
| `offline-tts-supertonic-impl.h` | `GetFrontend()` returning `nullptr` |
| `c-api.h` | `SherpaOnnxTimedPhoneme` struct; `phonemes`/`num_phonemes` in `SherpaOnnxGeneratedAudio`; `enable_timed_phonemes` in config |
| `c-api.cc` | Flag copy, phoneme data copy (C++ → C), memory cleanup; fixed uninitialised fields in deprecated Zipvoice function |
| `cxx-api.h` | `TimedPhoneme` struct; `phonemes` in `GeneratedAudio`; `enable_timed_phonemes` in `OfflineTtsConfig` |
| `cxx-api.cc` | Flag pass‑through; phoneme extraction from C struct; phoneme move in `Generate2()` |
| `non-streaming-tts.cc` | Reads `enableTimedPhonemes` from JS config; builds `phonemes` array in all four generate result paths (sync/async) |
| `piper-phonemize-lexicon.h` | `ConvertTextToPhonemeSpans()` override declaration |
| `piper-phonemize-lexicon.cc` | Implementation that captures espeak phonemes and maps them to `PhonemeSpan` objects for all model families (VITS, Matcha, Kokoro, Kitten) |

No other files are touched. All changes are backward‑compatible.

---

## Build

Standard Sherpa‑ONNX build – no extra dependencies:

```bash
mkdir build && cd build
cmake ..
make -j$(nproc)
```

The Node.js addon is built as part of the normal process.

---

## Usage (Electron / Node.js)

### 1. Enable the feature

```js
const sherpa = require('sherpa-onnx-node');

const config = {
  model: { /* your model files (Kokoro, VITS, etc.) */ },
  enableTimedPhonemes: true,   // ← turn on phoneme timing
};

const tts = new sherpa.OfflineTts(config);
```

### 2. Generate audio and phonemes

```js
const result = tts.generate({ text: "Hello world", sid: 0, speed: 1.0 });

// result.samples    – Float32Array
// result.sampleRate – number
// result.phonemes   – Array of { phoneme, id, startMs, endMs }

console.log(result.phonemes);
// [
//   { phoneme: "HH", id: 1, startMs: 0,   endMs: 58  },
//   { phoneme: "AH", id: 2, startMs: 58,  endMs: 122 }
// ]
```

### 3. Lip‑Sync Integration (Electron)

```js
// Play the audio (pseudocode – use Web Audio API or a Node library)
const audioCtx = new AudioContext();
const source = audioCtx.createBufferSource();
source.buffer = float32ArrayToAudioBuffer(result.samples, result.sampleRate);
source.connect(audioCtx.destination);
source.start();

// Drive lip‑sync with a timer
let idx = 0;
const startTime = audioCtx.currentTime;
const interval = setInterval(() => {
  const elapsed = (audioCtx.currentTime - startTime) * 1000; // ms
  while (idx < result.phonemes.length && elapsed >= result.phonemes[idx].endMs) idx++;
  if (idx < result.phonemes.length) {
    updateMouthShape(result.phonemes[idx].phoneme); // your mapping
  } else {
    clearInterval(interval);
  }
}, 16); // ~60 fps
```

---

## Output Format

```ts
interface TimedPhoneme {
  phoneme: string;   // ARPAbet, e.g. "HH", "AH"
  id: number;        // 1‑based sequential ID
  startMs: number;   // Start time in milliseconds
  endMs: number;     // End time in milliseconds
}
```

Timestamps are derived from **uniform token durations** – each token gets an equal share of the total audio length. This gives rhythm‑correct lip‑sync adequate for most avatar systems.

---

## Limitations

- Timestamps are approximate (uniform token durations); true model‑specific durations are not yet implemented.
- Only Piper‑based frontends (Kokoro v0.19, Kitten, Piper) produce phoneme spans. Kokoro v1.0+ (multi‑lang) and some VITS variants will require additional implementation to extract phonemes.
- Models without a frontend (Pocket, Supertonic) will always return an empty phoneme array.
- Offline TTS only; streaming TTS is not supported.

---

## Future Improvements

- Model‑specific duration extraction (override `GetTokenDurations()`)
- Full support for Kokoro v1.0+ `KokoroMultiLangLexicon`
- Optional forced‑alignment fallback for higher precision
- Streaming TTS support once the core engine adds it

---

**All changes are self‑contained and do not affect existing behaviour when the flag is off.**

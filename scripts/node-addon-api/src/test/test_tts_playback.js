// scripts/node-addon-api/test/test_tts_playback.js
//Just a test file used by me in dev machine, change paths accordingly for your use
// Exercises both halves of the TTS + PipeWire playback feature in one run:
//
//   PART A — phoneme extraction on the 4 pre-existing sync/async generate
//            paths (offlineTtsGenerate, offlineTtsGenerateWithConfig,
//            offlineTtsGenerateAsync, offlineTtsGenerateAsyncWithConfig).
//            These go through the shared BuildPhonemesArray() helper in
//            tts-playback.h.
//
//   PART B — the additive playback path: startPlayback -> speak (with its
//            own phoneme delivery) -> onRendererReady -> completion, plus
//            stopSpeak() cancellation and a concurrent-speak() rejection
//            check.
//
// Requires a Kokoro v0.19 model directory. Point SHERPA_ONNX_KOKORO_DIR at
// yours, or it defaults to a common local path.
//
//   node test/test_tts_playback.js
//   SHERPA_ONNX_KOKORO_DIR=/path/to/kokoro-en-v0_19 node test/test_tts_playback.js

const path = require('path');

const MODEL_DIR = process.env.SHERPA_ONNX_KOKORO_DIR ||
  '/home/subhajit/Desktop/kokoro-en-v0_19';

const { OfflineTts } = require(path.join(__dirname, '..', 'lib', 'non-streaming-tts.js'));
const addon = require(path.join(__dirname, '..', 'lib', 'addon.js'));

let failures = 0;

function check(label, condition, detail) {
  const status = condition ? 'PASS' : 'FAIL';
  console.log(`[${status}] ${label}${detail ? ' -- ' + detail : ''}`);
  if (!condition) failures++;
  return condition;
}

function isValidPhonemeArray(phonemes) {
  return Array.isArray(phonemes) && phonemes.length > 0 &&
    phonemes.every(p =>
      typeof p.phoneme === 'string' &&
      typeof p.id === 'number' &&
      typeof p.startMs === 'number' &&
      typeof p.endMs === 'number' &&
      p.endMs >= p.startMs);
}

function printAllPhonemes(label, phonemes) {
  console.log(`  ${label} all phonemes (count=${phonemes ? phonemes.length : 0}):`);
  console.log(JSON.stringify(phonemes, null, 2));
}

function sleep(ms) { return new Promise(r => setTimeout(r, ms)); }

const ttsConfig = {
  model: {
    kokoro: {
      model: path.join(MODEL_DIR, 'model.onnx'),
      voices: path.join(MODEL_DIR, 'voices.bin'),
      tokens: path.join(MODEL_DIR, 'tokens.txt'),
      dataDir: path.join(MODEL_DIR, 'espeak-ng-data'),
    },
    numThreads: 2,
  },
  enableTimedPhonemes: true,
};

const TEXT = 'Hello from the pipe wire playback test.';

// ---------------------------------------------------------------------------
// PART A: phoneme extraction on the 4 pre-existing generate paths
// ---------------------------------------------------------------------------
async function testPhonemeExtraction(handle) {
  console.log('\n=== PART A: phoneme extraction (4 existing generate paths) ===');

  const r1 = addon.offlineTtsGenerate(handle, { text: TEXT, sid: 0, speed: 1.0 });
  check('offlineTtsGenerate (sync legacy)', isValidPhonemeArray(r1.phonemes),
        `phonemeCount=${r1.phonemes && r1.phonemes.length}`);

  const r2 = addon.offlineTtsGenerateWithConfig(
    handle, { text: TEXT, generationConfig: { sid: 0, speed: 1.0 } });
  check('offlineTtsGenerateWithConfig (sync)', isValidPhonemeArray(r2.phonemes),
        `phonemeCount=${r2.phonemes && r2.phonemes.length}`);

  const r3 = await addon.offlineTtsGenerateAsync(handle, { text: TEXT, sid: 0, speed: 1.0 });
  check('offlineTtsGenerateAsync (async legacy)', isValidPhonemeArray(r3.phonemes),
        `phonemeCount=${r3.phonemes && r3.phonemes.length}`);

  const r4 = await addon.offlineTtsGenerateAsyncWithConfig(
    handle, { text: TEXT, generationConfig: { sid: 0, speed: 1.0 } });
  check('offlineTtsGenerateAsyncWithConfig (async)', isValidPhonemeArray(r4.phonemes),
        `phonemeCount=${r4.phonemes && r4.phonemes.length}`);

  // Print the FULL phoneme array per path (not just phonemes[0]) so every
  // entry can be eyeballed, matching what test_phonemes.js used to show.
  for (const [label, r] of [['sync', r1], ['sync+config', r2], ['async', r3], ['async+config', r4]]) {
    printAllPhonemes(label, r.phonemes);
  }
}

// ---------------------------------------------------------------------------
// PART B: the playback path (speak/startPlayback/onRendererReady/stopSpeak)
// ---------------------------------------------------------------------------
async function testPlaybackPath(handle) {
  console.log('\n=== PART B: playback path (speak/startPlayback/onRendererReady) ===');

  const started = addon.startPlayback(handle);
  check('startPlayback()', started === true);

  const completions = [];
  addon.setOnComplete((result) => completions.push(result));

  // --- B1: happy path -- phonemes delivered via speak(), then playback completes
  await new Promise((resolve) => {
    const id = addon.speak(handle, TEXT, 0, 1.0, (msg) => {
      check('speak() phoneme delivery', isValidPhonemeArray(msg.phonemes),
            `utteranceId=${msg.utteranceId} phonemeCount=${msg.phonemes.length} sampleRate=${msg.sampleRate}`);
      printAllPhonemes('speak()', msg.phonemes);
      addon.onRendererReady(msg.utteranceId);
    });
    check('speak() returns a numeric utteranceId', typeof id === 'number');

    const iv = setInterval(() => {
      if (completions.length >= 1) { clearInterval(iv); resolve(); }
    }, 50);
  });
  check('B1 happy-path completion', completions[0] && completions[0].ok === true,
        JSON.stringify(completions[0]));

  // --- B2: concurrent speak() must be rejected while one is in flight
  await new Promise((resolve) => {
    let rejected = false;
    addon.speak(handle, 'This one is in flight.', 0, 1.0, (msg) => {
      addon.onRendererReady(msg.utteranceId);
    });
    try {
      addon.speak(handle, 'This should be rejected.', 0, 1.0, () => {});
    } catch (e) {
      rejected = true;
    }
    check('B2 concurrent speak() rejected', rejected);

    const iv = setInterval(() => {
      if (completions.length >= 2) { clearInterval(iv); resolve(); }
    }, 50);
  });

  // --- B3: stopSpeak() cancels an in-flight utterance
  await new Promise((resolve) => {
    const id = addon.speak(
      handle,
      'This is a longer utterance meant to be cancelled part way through playback.',
      0, 1.0,
      (msg) => {
        addon.onRendererReady(msg.utteranceId);
        setTimeout(() => {
          const stopped = addon.stopSpeak(id);
          check('B3 stopSpeak() returns true for the in-flight id', stopped === true);
        }, 300);
      });

    const iv = setInterval(() => {
      if (completions.length >= 3) { clearInterval(iv); resolve(); }
    }, 50);
  });
  check('B3 completion still fires after cancellation', completions[2] !== undefined,
        JSON.stringify(completions[2]));
}

async function main() {
  console.log('Model dir:', MODEL_DIR);
  const tts = new OfflineTts(ttsConfig);
  console.log('TTS created. sampleRate =', tts.sampleRate);

  await testPhonemeExtraction(tts.handle);
  await testPlaybackPath(tts.handle);

  console.log(`\n${failures === 0 ? 'ALL TESTS PASSED' : failures + ' TEST(S) FAILED'}`);
  process.exitCode = failures === 0 ? 0 : 1;
  // No process.exit() call -- let the event loop drain naturally so the
  // addon's napi_add_env_cleanup_hook actually runs (see tts-playback.cc).
}

main().catch((e) => {
  console.error('FATAL:', e);
  process.exitCode = 1;
});

/*
  ESP32-S3 Touch Piano  (v3 - clean, no clicks on fast tapping)
  - 9 x TTP223 touch sensors (digital output, HIGH when touched)
  - MAX98357A I2S amplifier
  - Polyphonic

  Requires: Arduino-ESP32 core 3.x  (uses the built-in ESP_I2S library)

  Wiring (same as before)
  ------
    Key 1  C4  -> GPIO 7
    Key 2  D4  -> GPIO 15
    Key 3  E4  -> GPIO 16
    Key 4  F4  -> GPIO 17
    Key 5  G4  -> GPIO 18
    Key 6  A4  -> GPIO 8
    Key 7  B4  -> GPIO 9
    Key 8  C5  -> GPIO 10
    Key 9  D5  -> GPIO 11
    MAX98357: BCLK -> GPIO 4, LRC -> GPIO 5, DIN -> GPIO 6

  What is different from v1
  -------------------------
  1. Debounce: a key must be stable for a couple of blocks before it counts.
  2. Smooth exponential envelope instead of linear ramps (no slope clicks).
  3. Linear interpolation in the sine table (cleaner waveform).
  4. Gentle soft limiter instead of a hard clip.
*/

#include <ESP_I2S.h>
#include <math.h>

// ---------------- Pins ----------------
const int NUM_KEYS = 9;
const int keyPins[NUM_KEYS] = {7, 15, 16, 17, 18, 8, 9, 10, 11};

const int I2S_BCLK = 4;
const int I2S_LRC  = 5;
const int I2S_DIN  = 6;

// ---------------- Audio settings ----------------
const uint32_t SAMPLE_RATE  = 22050;
const int      BLOCK_FRAMES = 128;          // ~5.8 ms per block
const float    MASTER_VOLUME = 0.6f;        // same level as v1

// Time constants (seconds)
const float ATTACK_TAU  = 0.004f;           // fast but smooth rise
const float RELEASE_TAU = 0.060f;           // smooth fade-out after release
const float DECAY_TAU   = 1.2f;             // slow piano-like fade while held
const float SUSTAIN_LEVEL = 0.35f;          // level a held note decays toward

// Debounce: number of consecutive blocks the new state must persist
const int DEBOUNCE_BLOCKS = 2;              // ~12 ms

// Note frequencies: C4 D4 E4 F4 G4 A4 B4 C5 D5
const float noteFreq[NUM_KEYS] = {
  261.63f, 293.66f, 329.63f, 349.23f, 392.00f,
  440.00f, 493.88f, 523.25f, 587.33f
};

// ---------------- Sine lookup table (+1 guard point) ----------------
const int TABLE_SIZE = 1024;
float sineTable[TABLE_SIZE + 1];

inline float tableLerp(float p) {           // p in [0, TABLE_SIZE)
  int i = (int)p;
  float f = p - (float)i;
  return sineTable[i] + (sineTable[i + 1] - sineTable[i]) * f;
}

// ---------------- Per-key voice state ----------------
struct Voice {
  float phase;
  float phaseInc;
  float target;     // slowly decaying level while key is held
  float gain;       // smoothed output gain (what we actually apply)
};
Voice voices[NUM_KEYS];

bool keyDown[NUM_KEYS];        // debounced state
bool lastRaw[NUM_KEYS];
int  rawCount[NUM_KEYS];

float atkCoef, relCoef, decMul;

I2SClass i2s;
int16_t outBuf[BLOCK_FRAMES * 2];

void setup() {
  Serial.begin(115200);

  for (int i = 0; i < NUM_KEYS; i++) {
    pinMode(keyPins[i], INPUT_PULLDOWN);
    voices[i].phase = 0;
    voices[i].phaseInc = noteFreq[i] * TABLE_SIZE / (float)SAMPLE_RATE;
    voices[i].target = 0;
    voices[i].gain = 0;
    keyDown[i] = false;
    lastRaw[i] = false;
    rawCount[i] = 0;
  }

  for (int i = 0; i < TABLE_SIZE; i++) {
    sineTable[i] = sinf(2.0f * PI * i / TABLE_SIZE);
  }
  sineTable[TABLE_SIZE] = sineTable[0];

  atkCoef = 1.0f - expf(-1.0f / (ATTACK_TAU  * SAMPLE_RATE));
  relCoef = 1.0f - expf(-1.0f / (RELEASE_TAU * SAMPLE_RATE));
  decMul  = expf(-1.0f / (DECAY_TAU * SAMPLE_RATE));

  i2s.setPins(I2S_BCLK, I2S_LRC, I2S_DIN);
  if (!i2s.begin(I2S_MODE_STD, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT,
                 I2S_SLOT_MODE_STEREO)) {
    Serial.println("I2S init failed!");
    while (true) delay(1000);
  }
  Serial.println("Touch piano ready (smooth)");
}

// Debounced key reading, called once per audio block
void readKeys() {
  for (int i = 0; i < NUM_KEYS; i++) {
    bool raw = digitalRead(keyPins[i]) == HIGH;

    if (raw == lastRaw[i]) {
      if (rawCount[i] < DEBOUNCE_BLOCKS) rawCount[i]++;
    } else {
      rawCount[i] = 0;
      lastRaw[i] = raw;
    }

    // Accept the new state only after it has been stable long enough
    if (rawCount[i] >= DEBOUNCE_BLOCKS && raw != keyDown[i]) {
      keyDown[i] = raw;
      if (raw) voices[i].target = 1.0f;     // new press: restart level
    }
  }
}

void loop() {
  readKeys();

  for (int n = 0; n < BLOCK_FRAMES; n++) {
    float mix = 0.0f;

    for (int k = 0; k < NUM_KEYS; k++) {
      Voice &v = voices[k];

      // --- smooth envelope ---
      if (keyDown[k]) {
        v.target = SUSTAIN_LEVEL + (v.target - SUSTAIN_LEVEL) * decMul;
        v.gain  += (v.target - v.gain) * atkCoef;
      } else {
        v.gain  += (0.0f - v.gain) * relCoef;
        if (v.gain < 0.0003f) v.gain = 0.0f;   // inaudible, switch voice off
      }

      // --- oscillator (skipped when silent) ---
      if (v.gain > 0.0f) {
        float p2 = v.phase * 2.0f;                       // 2nd harmonic
        if (p2 >= TABLE_SIZE) p2 -= TABLE_SIZE;
        float s = tableLerp(v.phase) + 0.3f * tableLerp(p2);
        mix += s * v.gain;

        v.phase += v.phaseInc;
        if (v.phase >= TABLE_SIZE) v.phase -= TABLE_SIZE;
      }
    }

    mix *= MASTER_VOLUME / 4.0f;     // same level as v1
    mix = tanhf(mix);                // gentle limiter, no hard clipping

    int16_t sample = (int16_t)(mix * 32767.0f);
    outBuf[2 * n]     = sample;
    outBuf[2 * n + 1] = sample;
  }

  i2s.write((uint8_t *)outBuf, sizeof(outBuf));
}

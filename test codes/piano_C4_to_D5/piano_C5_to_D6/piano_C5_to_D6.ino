/*
  ESP32-S3 Touch Piano - C5 to D6 (octave 5)
  - 9 x TTP223 touch sensors (digital output, HIGH when touched)
  - MAX98357A I2S amplifier
  - Polyphonic, smooth envelope, debounced keys

  Requires: Arduino-ESP32 core 3.x  (uses the built-in ESP_I2S library)

  Key map
  -------
    Key 1  C5    523.25 Hz -> GPIO 7
    Key 2  D5    587.33 Hz -> GPIO 15
    Key 3  E5    659.25 Hz -> GPIO 16
    Key 4  F5    698.46 Hz -> GPIO 17
    Key 5  G5    783.99 Hz -> GPIO 18
    Key 6  A5    880.00 Hz -> GPIO 8
    Key 7  B5    987.77 Hz -> GPIO 9
    Key 8  C6   1046.50 Hz -> GPIO 10
    Key 9  D6   1174.66 Hz -> GPIO 11
  MAX98357: BCLK -> GPIO 4, LRC -> GPIO 5, DIN -> GPIO 6

  Notes
  -----
  - Sample rate is 44100 Hz so the high notes stay clean.
  - No low-frequency boost is used, because this octave is inside the speaker's
    good range.
  - When comparing with a real piano, play the SAME octave: C5 is the C one octave above middle C.
  - Tone: H2 is the amount of 2nd harmonic (0 = pure sine).
  - If it is too loud or sharp, lower MASTER_VOLUME.
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
const uint32_t SAMPLE_RATE   = 44100;
const int      BLOCK_FRAMES  = 128;            // ~2.9 ms per block
const float    MASTER_VOLUME = 0.6f;       // 0.0 - 1.0
const float    H2            = 0.25f;        // 2nd harmonic amount

// Time constants (seconds)
const float ATTACK_TAU    = 0.004f;
const float RELEASE_TAU   = 0.060f;
const float DECAY_TAU     = 1.2f;
const float SUSTAIN_LEVEL = 0.35f;

const int DEBOUNCE_BLOCKS = 4;                 // ~12 ms

// ---------------- Notes ----------------
// C5  D5  E5  F5  G5  A5  B5  C6  D6
const float noteFreq[NUM_KEYS] = {
  523.25f, 587.33f, 659.25f, 698.46f, 783.99f, 880.00f, 987.77f, 1046.50f, 1174.66f
};

// ---------------- Sine lookup table (+1 guard point) ----------------
const int TABLE_SIZE = 1024;
float sineTable[TABLE_SIZE + 1];

inline float tableLerp(float p) {              // p in [0, TABLE_SIZE)
  int i = (int)p;
  float f = p - (float)i;
  return sineTable[i] + (sineTable[i + 1] - sineTable[i]) * f;
}

// ---------------- Per-key voice state ----------------
struct Voice {
  float phase;
  float phaseInc;
  float target;
  float gain;
};
Voice voices[NUM_KEYS];

bool keyDown[NUM_KEYS];
bool lastRaw[NUM_KEYS];
int  rawCount[NUM_KEYS];

float atkCoef, relCoef, decMul;
float voiceScale;

I2SClass i2s;
int16_t outBuf[BLOCK_FRAMES * 2];

void setup() {
  Serial.begin(115200);

  voiceScale = 1.3f / (1.0f + H2);             // keeps loudness consistent

  for (int i = 0; i < NUM_KEYS; i++) {
    pinMode(keyPins[i], INPUT_PULLDOWN);
    voices[i].phase    = 0;
    voices[i].phaseInc = noteFreq[i] * TABLE_SIZE / (float)SAMPLE_RATE;
    voices[i].target   = 0;
    voices[i].gain     = 0;
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
  Serial.println("Touch piano ready (C5 to D6 (octave 5))");
}

void readKeys() {
  for (int i = 0; i < NUM_KEYS; i++) {
    bool raw = digitalRead(keyPins[i]) == HIGH;

    if (raw == lastRaw[i]) {
      if (rawCount[i] < DEBOUNCE_BLOCKS) rawCount[i]++;
    } else {
      rawCount[i] = 0;
      lastRaw[i] = raw;
    }

    if (rawCount[i] >= DEBOUNCE_BLOCKS && raw != keyDown[i]) {
      keyDown[i] = raw;
      if (raw) voices[i].target = 1.0f;
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
        if (v.gain < 0.0003f) v.gain = 0.0f;
      }

      // --- oscillator (skipped when silent) ---
      if (v.gain > 0.0f) {
        float s = tableLerp(v.phase);
        if (H2 > 0.0f) {
          float p2 = v.phase * 2.0f;
          if (p2 >= TABLE_SIZE) p2 -= TABLE_SIZE;
          s += H2 * tableLerp(p2);
        }
        mix += s * voiceScale * v.gain;

        v.phase += v.phaseInc;
        if (v.phase >= TABLE_SIZE) v.phase -= TABLE_SIZE;
      }
    }

    mix *= MASTER_VOLUME / 4.0f;
    mix = tanhf(mix);                          // gentle limiter

    int16_t sample = (int16_t)(mix * 32767.0f);
    outBuf[2 * n]     = sample;
    outBuf[2 * n + 1] = sample;
  }

  i2s.write((uint8_t *)outBuf, sizeof(outBuf));
}

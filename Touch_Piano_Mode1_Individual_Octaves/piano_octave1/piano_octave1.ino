/*
  =============================================================================
  ESP32-S3 Touch Piano - MODE 1: OCTAVE 1 (C1 to C2)
  =============================================================================
  Features:
  - 8-key standard diatonic piano octave (C1, D1, E1, F1, G1, A1, B1, C2)
  - 8 x TTP223 capacitive touch sensors (active HIGH)
  - MAX98357A I2S Class-D amplifier module driving 4 ohm 3W speaker
  - Real-time polyphonic additive sine synthesis with harmonic generation
  - Smooth exponential attack/decay/release envelope (anti-click)
  - Soft limiter (tanhf) preventing clipping distortion on chords
  
  Requires: Arduino ESP32 core v3.x (uses native ESP_I2S library)
  Board: ESP32-S3 Dev Module
  
  Key Mapping (8 Standard Keys):
  -----------------------------
    Key 1: C1  (32.70 Hz)  -> GPIO 15
    Key 2: D1  (36.71 Hz)  -> GPIO 16
    Key 3: E1  (41.20 Hz)  -> GPIO 17
    Key 4: F1  (43.65 Hz)  -> GPIO 18
    Key 5: G1  (49.00 Hz)  -> GPIO 8
    Key 6: A1  (55.00 Hz)  -> GPIO 9
    Key 7: B1  (61.74 Hz)  -> GPIO 10
    Key 8: C2  (65.41 Hz)  -> GPIO 11
  
  MAX98357A I2S Connections:
  --------------------------
    BCLK -> GPIO 4
    LRC  -> GPIO 5
    DIN  -> GPIO 6
    VIN  -> 5V
    GND  -> GND
    GAIN -> GND (12 dB gain)
  =============================================================================
*/

#include <ESP_I2S.h>
#include <math.h>

// ---------------- Hardware Pins ----------------
const int NUM_KEYS = 8;
// Keys 1 to 8 on GPIO 15, 16, 17, 18, 8, 9, 10, 11 (matches Mode 2 layout)
const int keyPins[NUM_KEYS] = {15, 16, 17, 18, 8, 9, 10, 11};
// (Note: If your hardware wires Key 1 to GPIO 7, uncomment the line below):
// const int keyPins[NUM_KEYS] = {7, 15, 16, 17, 18, 8, 9, 10};

const int I2S_BCLK = 4;
const int I2S_LRC  = 5;
const int I2S_DIN  = 6;

// ---------------- Audio Settings ----------------
const uint32_t SAMPLE_RATE   = 44100;
const int      BLOCK_FRAMES  = 128;           // ~2.9 ms latency
const float    MASTER_VOLUME = 0.70f;         // Master volume (0.0 to 1.0)
const float    H2            = 0.40f;         // 2nd harmonic (boosts audibility of sub-bass on small speakers)

// ADSR Envelope Time Constants (seconds)
const float ATTACK_TAU    = 0.005f;          // 5 ms smooth rise (no clicks)
const float RELEASE_TAU   = 0.080f;          // 80 ms smooth fade-out
const float DECAY_TAU     = 1.5f;            // Natural acoustic decay while held
const float SUSTAIN_LEVEL = 0.35f;

const int DEBOUNCE_BLOCKS = 4;                // ~12 ms debounce

// ---------------- Frequencies (Octave 1: C1 to C2) ----------------
const float noteFreq[NUM_KEYS] = {
  32.70f,  // C1
  36.71f,  // D1
  41.20f,  // E1
  43.65f,  // F1
  49.00f,  // G1
  55.00f,  // A1
  61.74f,  // B1
  65.41f   // C2
};

const char* noteNames[NUM_KEYS] = {"C1", "D1", "E1", "F1", "G1", "A1", "B1", "C2"};

// ---------------- Sine Wave Lookup Table ----------------
const int TABLE_SIZE = 1024;
float sineTable[TABLE_SIZE + 1];

inline float tableLerp(float p) {
  int i = (int)p;
  float f = p - (float)i;
  return sineTable[i] + (sineTable[i + 1] - sineTable[i]) * f;
}

// ---------------- Per-Key Voice State ----------------
struct Voice {
  float phase;
  float phaseInc;
  float target;
  float gain;
};

Voice voices[NUM_KEYS];
bool  keyDown[NUM_KEYS];
bool  lastRaw[NUM_KEYS];
int   rawCount[NUM_KEYS];

float atkCoef, relCoef, decMul;
I2SClass i2s;
int16_t outBuf[BLOCK_FRAMES * 2];

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("\n=============================================");
  Serial.println("  ESP32-S3 Touch Piano - Mode 1: Octave 1    ");
  Serial.println("  Range: C1 (32.70 Hz) to C2 (65.41 Hz)      ");
  Serial.println("=============================================");

  for (int i = 0; i < NUM_KEYS; i++) {
    pinMode(keyPins[i], INPUT_PULLDOWN);
    voices[i].phase = 0.0f;
    voices[i].phaseInc = noteFreq[i] * (float)TABLE_SIZE / (float)SAMPLE_RATE;
    voices[i].target = 0.0f;
    voices[i].gain = 0.0f;
    keyDown[i] = false;
    lastRaw[i] = false;
    rawCount[i] = 0;
    Serial.printf("  Key %d: %-2s (%6.2f Hz) -> GPIO %d\n", i + 1, noteNames[i], noteFreq[i], keyPins[i]);
  }

  // Precompute 1024-point sine table + 1 guard point for interpolation
  for (int i = 0; i < TABLE_SIZE; i++) {
    sineTable[i] = sinf(2.0f * PI * (float)i / (float)TABLE_SIZE);
  }
  sineTable[TABLE_SIZE] = sineTable[0];

  // Precompute envelope coefficients
  atkCoef = 1.0f - expf(-1.0f / (ATTACK_TAU  * (float)SAMPLE_RATE));
  relCoef = 1.0f - expf(-1.0f / (RELEASE_TAU * (float)SAMPLE_RATE));
  decMul  = expf(-1.0f / (DECAY_TAU * (float)SAMPLE_RATE));

  // Initialize I2S
  i2s.setPins(I2S_BCLK, I2S_LRC, I2S_DIN);
  if (!i2s.begin(I2S_MODE_STD, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO)) {
    Serial.println("[-] FATAL: I2S Initialization Failed!");
    while (true) delay(1000);
  }
  Serial.println("[+] I2S Initialized successfully (44100 Hz, 16-bit Stereo).");
  Serial.println("[+] Ready! Touch any sensor key to play.\n");
}

void readKeys() {
  for (int i = 0; i < NUM_KEYS; i++) {
    bool raw = (digitalRead(keyPins[i]) == HIGH);

    if (raw == lastRaw[i]) {
      if (rawCount[i] < DEBOUNCE_BLOCKS) rawCount[i]++;
    } else {
      rawCount[i] = 0;
      lastRaw[i] = raw;
    }

    if (rawCount[i] >= DEBOUNCE_BLOCKS && raw != keyDown[i]) {
      keyDown[i] = raw;
      if (raw) {
        voices[i].target = 1.0f;
        Serial.printf("[NOTE ON]  Key %d: %s (%6.2f Hz)\n", i + 1, noteNames[i], noteFreq[i]);
      } else {
        Serial.printf("[NOTE OFF] Key %d: %s\n", i + 1, noteNames[i]);
      }
    }
  }
}

void loop() {
  readKeys();

  for (int n = 0; n < BLOCK_FRAMES; n++) {
    float mix = 0.0f;

    for (int k = 0; k < NUM_KEYS; k++) {
      Voice &v = voices[k];

      if (keyDown[k]) {
        v.target = SUSTAIN_LEVEL + (v.target - SUSTAIN_LEVEL) * decMul;
        v.gain  += (v.target - v.gain) * atkCoef;
      } else {
        v.gain  += (0.0f - v.gain) * relCoef;
        if (v.gain < 0.0003f) v.gain = 0.0f;
      }

      if (v.gain > 0.0f) {
        float p2 = v.phase * 2.0f;
        if (p2 >= (float)TABLE_SIZE) p2 -= (float)TABLE_SIZE;

        float s = tableLerp(v.phase) + H2 * tableLerp(p2);
        mix += s * v.gain;

        v.phase += v.phaseInc;
        if (v.phase >= (float)TABLE_SIZE) v.phase -= (float)TABLE_SIZE;
      }
    }

    mix *= MASTER_VOLUME / 4.0f;
    mix = tanhf(mix); // Soft limiter: eliminates harsh digital clipping

    int16_t sample = (int16_t)(mix * 32767.0f);
    outBuf[2 * n]     = sample;
    outBuf[2 * n + 1] = sample;
  }

  i2s.write((uint8_t *)outBuf, sizeof(outBuf));
}

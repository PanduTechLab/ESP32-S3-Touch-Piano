/*
  =============================================================================
  ESP32-S3 Touch Piano - MODE 1: OCTAVE 7 (C7 to C8 - High Treble Octave)
  =============================================================================
  Features:
  - 8-key standard diatonic piano octave (C7, D7, E7, F7, G7, A7, B7, C8)
  - 8 x TTP223 capacitive touch sensors (active HIGH)
  - MAX98357A I2S Class-D amplifier module driving 4 ohm 3W speaker
  - Real-time polyphonic additive sine synthesis (pure sine for clean top octave)
  - Smooth exponential attack/decay/release envelope (anti-click)
  - Soft limiter (tanhf) preventing clipping distortion on chords
  
  Requires: Arduino ESP32 core v3.x (uses native ESP_I2S library)
  Board: ESP32-S3 Dev Module
  
  Key Mapping (8 Standard Keys - D8 removed to form standard 8-note octave):
  --------------------------------------------------------------------------
    Key 1: C7  (2093.00 Hz)  -> GPIO 15
    Key 2: D7  (2349.32 Hz)  -> GPIO 16
    Key 3: E7  (2637.02 Hz)  -> GPIO 17
    Key 4: F7  (2793.83 Hz)  -> GPIO 18
    Key 5: G7  (3135.96 Hz)  -> GPIO 8
    Key 6: A7  (3520.00 Hz)  -> GPIO 9
    Key 7: B7  (3951.07 Hz)  -> GPIO 10
    Key 8: C8  (4186.01 Hz)  -> GPIO 11
  
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
const int keyPins[NUM_KEYS] = {15, 16, 17, 18, 8, 9, 10, 11};
// (Note: If your hardware wires Key 1 to GPIO 7, uncomment the line below):
// const int keyPins[NUM_KEYS] = {7, 15, 16, 17, 18, 8, 9, 10};

const int I2S_BCLK = 4;
const int I2S_LRC  = 5;
const int I2S_DIN  = 6;

// ---------------- Audio Settings ----------------
const uint32_t SAMPLE_RATE   = 44100;
const int      BLOCK_FRAMES  = 128;           // ~2.9 ms latency
const float    MASTER_VOLUME = 0.40f;         // Tamed volume for very high frequencies
const float    H2            = 0.00f;         // Pure sine wave (harmonics above 4 kHz are piercing)

// ADSR Envelope Time Constants (seconds)
const float ATTACK_TAU    = 0.004f;
const float RELEASE_TAU   = 0.050f;
const float DECAY_TAU     = 1.0f;
const float SUSTAIN_LEVEL = 0.35f;

const int DEBOUNCE_BLOCKS = 4;                // ~12 ms debounce

// ---------------- Frequencies (Octave 7: C7 to C8) ----------------
const float noteFreq[NUM_KEYS] = {
  2093.00f,  // C7
  2349.32f,  // D7
  2637.02f,  // E7
  2793.83f,  // F7
  3135.96f,  // G7
  3520.00f,  // A7
  3951.07f,  // B7
  4186.01f   // C8 (Highest standard piano note)
};

const char* noteNames[NUM_KEYS] = {"C7", "D7", "E7", "F7", "G7", "A7", "B7", "C8"};

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
  Serial.println("  ESP32-S3 Touch Piano - Mode 1: Octave 7    ");
  Serial.println("  Range: C7 (2093.00 Hz) to C8 (4186.01 Hz)  ");
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

  for (int i = 0; i < TABLE_SIZE; i++) {
    sineTable[i] = sinf(2.0f * PI * (float)i / (float)TABLE_SIZE);
  }
  sineTable[TABLE_SIZE] = sineTable[0];

  atkCoef = 1.0f - expf(-1.0f / (ATTACK_TAU  * (float)SAMPLE_RATE));
  relCoef = 1.0f - expf(-1.0f / (RELEASE_TAU * (float)SAMPLE_RATE));
  decMul  = expf(-1.0f / (DECAY_TAU * (float)SAMPLE_RATE));

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
    mix = tanhf(mix);

    int16_t sample = (int16_t)(mix * 32767.0f);
    outBuf[2 * n]     = sample;
    outBuf[2 * n + 1] = sample;
  }

  i2s.write((uint8_t *)outBuf, sizeof(outBuf));
}

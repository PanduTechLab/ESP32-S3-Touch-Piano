/*
  =============================================================================
  ESP32-S3 Touch Piano - MODE 2: ALL-IN-ONE WITH TOUCH OCTAVE SELECTOR
  =============================================================================
  Features:
  - ALL 7 OCTAVES (Octave 1 to Octave 7) built into a single firmware!
  - Touch-based Octave Selector on GPIO 7:
      * Every tap cycles to the next octave: 1 -> 2 -> 3 -> 4 -> 5 -> 6 -> 7 -> 1...
      * Debounced tap detection prevents accidental double-triggering.
      * Serial monitor logs real-time octave change, active frequencies, and notes.
  - 8-key standard diatonic piano octave (C to High C, 8 notes)
  - 8 x TTP223 capacitive touch sensors for keys (GPIO 15, 16, 17, 18, 8, 9, 10, 11)
  - MAX98357A I2S Class-D amplifier module driving 4 ohm 3W speaker
  - Real-time polyphonic additive sine synthesis with per-octave harmonic tuning
  - Smooth exponential attack/decay/release envelope (anti-click)
  - Soft limiter (tanhf) preventing clipping distortion on chords
  
  Requires: Arduino ESP32 core v3.x (uses native ESP_I2S library)
  Board: ESP32-S3 Dev Module
  
  Wiring Overview:
  ----------------
    [Octave Selector Touch Pad]
      TTP223 Output -> GPIO 7   (Tap to change octave: 1, 2, 3, 4, 5, 6, 7, 1...)
      
    [8 Piano Keys]
      Key 1: Note C      -> GPIO 15
      Key 2: Note D      -> GPIO 16
      Key 3: Note E      -> GPIO 17
      Key 4: Note F      -> GPIO 18
      Key 5: Note G      -> GPIO 8
      Key 6: Note A      -> GPIO 9
      Key 7: Note B      -> GPIO 10
      Key 8: Note High C -> GPIO 11
      
    [All TTP223 Modules]
      VCC -> 3V3, GND -> GND
      
    [MAX98357A I2S Amplifier]
      BCLK -> GPIO 4
      LRC  -> GPIO 5
      DIN  -> GPIO 6
      VIN  -> 5V
      GND  -> GND
      GAIN -> GND (12 dB gain recommended)
      SD   -> Unconnected
  =============================================================================
*/

#include <ESP_I2S.h>
#include <math.h>

// ---------------- Hardware Pin Configuration ----------------
const int OCTAVE_PIN = 7;             // Touch sensor to change octave (GPIO 7)
const int NUM_KEYS   = 8;             // 8 white keys: C, D, E, F, G, A, B, C
const int keyPins[NUM_KEYS] = {15, 16, 17, 18, 8, 9, 10, 11};

const int I2S_BCLK = 4;
const int I2S_LRC  = 5;
const int I2S_DIN  = 6;

// ---------------- Audio Settings ----------------
const uint32_t SAMPLE_RATE   = 44100;
const int      BLOCK_FRAMES  = 128;   // ~2.9 ms latency
const int      TOTAL_OCTAVES = 7;

// Envelope Time Constants (seconds)
const float ATTACK_TAU    = 0.004f;   // 4 ms attack
const float RELEASE_TAU   = 0.060f;   // 60 ms release
const float DECAY_TAU     = 1.2f;     // Natural acoustic decay
const float SUSTAIN_LEVEL = 0.35f;

const int DEBOUNCE_BLOCKS = 4;        // ~12 ms key debounce
const int OCTAVE_TAP_BLOCKS = 6;      // ~17 ms debounce for octave selector

// ---------------- Octave Profiles (All 7 Octaves) ----------------
struct OctaveProfile {
  const char* name;
  const char* range;
  float freqs[NUM_KEYS];
  float masterVolume;
  float h2;                           // 2nd harmonic amount
};

const OctaveProfile octaves[TOTAL_OCTAVES] = {
  // Octave 1: Sub-bass (C1 - C2)
  {
    "Octave 1", "C1 to C2 (32.70 Hz - 65.41 Hz)",
    {32.70f, 36.71f, 41.20f, 43.65f, 49.00f, 55.00f, 61.74f, 65.41f},
    0.70f, 0.40f
  },
  // Octave 2: Bass (C2 - C3)
  {
    "Octave 2", "C2 to C3 (65.41 Hz - 130.81 Hz)",
    {65.41f, 73.42f, 82.41f, 87.31f, 98.00f, 110.00f, 123.47f, 130.81f},
    0.65f, 0.35f
  },
  // Octave 3: Low Mid (C3 - C4)
  {
    "Octave 3", "C3 to C4 (130.81 Hz - 261.63 Hz)",
    {130.81f, 146.83f, 164.81f, 174.61f, 196.00f, 220.00f, 246.94f, 261.63f},
    0.60f, 0.30f
  },
  // Octave 4: Middle Octave (C4 - C5)
  {
    "Octave 4", "C4 to C5 (261.63 Hz - 523.25 Hz)",
    {261.63f, 293.66f, 329.63f, 349.23f, 392.00f, 440.00f, 493.88f, 523.25f},
    0.60f, 0.25f
  },
  // Octave 5: Treble (C5 - C6)
  {
    "Octave 5", "C5 to C6 (523.25 Hz - 1046.50 Hz)",
    {523.25f, 587.33f, 659.25f, 698.46f, 783.99f, 880.00f, 987.77f, 1046.50f},
    0.55f, 0.20f
  },
  // Octave 6: High Treble (C6 - C7)
  {
    "Octave 6", "C6 to C7 (1046.50 Hz - 2093.00 Hz)",
    {1046.50f, 1174.66f, 1318.51f, 1396.91f, 1567.98f, 1760.00f, 1975.53f, 2093.00f},
    0.50f, 0.12f
  },
  // Octave 7: Top Octave (C7 - C8)
  {
    "Octave 7", "C7 to C8 (2093.00 Hz - 4186.01 Hz)",
    {2093.00f, 2349.32f, 2637.02f, 2793.83f, 3135.96f, 3520.00f, 3951.07f, 4186.01f},
    0.40f, 0.00f
  }
};

const char* diatonicNames[NUM_KEYS] = {"C", "D", "E", "F", "G", "A", "B", "C (high)"};

// Current active octave index (0 = Octave 1, 1 = Octave 2, ..., 3 = Octave 4, ..., 6 = Octave 7)
// Default start at Octave 4 (Middle Octave, the most natural for piano playing)
int currentOctaveIdx = 3;

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

// Octave Selector Sensor State
bool  octaveLastRaw  = false;
int   octaveRawCount = 0;
bool  octaveKeyDown  = false;

float atkCoef, relCoef, decMul;
I2SClass i2s;
int16_t outBuf[BLOCK_FRAMES * 2];

// Update voice frequencies when octave changes
void applyOctave(int octIdx, bool printInfo = true) {
  const OctaveProfile &oct = octaves[octIdx];
  for (int i = 0; i < NUM_KEYS; i++) {
    voices[i].phaseInc = oct.freqs[i] * (float)TABLE_SIZE / (float)SAMPLE_RATE;
  }

  if (printInfo) {
    Serial.println("\n-----------------------------------------------------------");
    Serial.printf(">>> [OCTAVE SWITCH] Active: %s (%s) <<<\n", oct.name, oct.range);
    Serial.println("Key Mapping:");
    for (int i = 0; i < NUM_KEYS; i++) {
      Serial.printf("  Key %d: %-8s %7.2f Hz  [GPIO %d]\n", i + 1, diatonicNames[i], oct.freqs[i], keyPins[i]);
    }
    Serial.printf("Settings: Master Volume = %.2f | 2nd Harmonic H2 = %.2f\n", oct.masterVolume, oct.h2);
    Serial.println("-----------------------------------------------------------\n");
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);

  Serial.println("\n===========================================================");
  Serial.println("    ESP32-S3 Touch Piano - MODE 2 (All-in-One Octaves)    ");
  Serial.println("===========================================================");
  Serial.println("Touch GPIO 7 to switch octaves (1 -> 2 -> 3 -> 4 -> 5 -> 6 -> 7 -> 1)");
  Serial.println("Touch GPIO 15, 16, 17, 18, 8, 9, 10, 11 to play the 8 notes");

  // Configure Octave Selector Pin
  pinMode(OCTAVE_PIN, INPUT_PULLDOWN);

  // Configure Key Pins
  for (int i = 0; i < NUM_KEYS; i++) {
    pinMode(keyPins[i], INPUT_PULLDOWN);
    voices[i].phase = 0.0f;
    voices[i].target = 0.0f;
    voices[i].gain = 0.0f;
    keyDown[i] = false;
    lastRaw[i] = false;
    rawCount[i] = 0;
  }

  // Precompute 1024-point sine lookup table + guard point
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

  // Apply default octave (Octave 4)
  applyOctave(currentOctaveIdx, true);
  Serial.println("[+] System Ready! Tap GPIO 7 to cycle octaves, touch keys to play.\n");
}

// Check Octave Switch sensor on GPIO 7 with clean edge-detection debouncing
void checkOctaveSelector() {
  bool raw = (digitalRead(OCTAVE_PIN) == HIGH);

  if (raw == octaveLastRaw) {
    if (octaveRawCount < OCTAVE_TAP_BLOCKS) octaveRawCount++;
  } else {
    octaveRawCount = 0;
    octaveLastRaw = raw;
  }

  // Detect rising edge (a new tap)
  if (octaveRawCount >= OCTAVE_TAP_BLOCKS && raw != octaveKeyDown) {
    octaveKeyDown = raw;
    if (octaveKeyDown) {
      // Advance to next octave: 0->1->2->3->4->5->6->0... (Display as Octave 1 to 7)
      currentOctaveIdx = (currentOctaveIdx + 1) % TOTAL_OCTAVES;
      applyOctave(currentOctaveIdx, true);
    }
  }
}

// Read and debounce the 8 piano keys
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
        Serial.printf("[NOTE ON]  Key %d: %-8s (%7.2f Hz) [Octave %d]\n",
                      i + 1, diatonicNames[i], octaves[currentOctaveIdx].freqs[i], currentOctaveIdx + 1);
      } else {
        Serial.printf("[NOTE OFF] Key %d: %-8s\n", i + 1, diatonicNames[i]);
      }
    }
  }
}

void loop() {
  // 1. Process Octave tap sensor (GPIO 7)
  checkOctaveSelector();

  // 2. Process 8 Piano keys
  readKeys();

  // 3. Synthesize audio block
  const OctaveProfile &oct = octaves[currentOctaveIdx];
  const float curVolume   = oct.masterVolume;
  const float curH2       = oct.h2;

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

        float s = tableLerp(v.phase) + curH2 * tableLerp(p2);
        mix += s * v.gain;

        v.phase += v.phaseInc;
        if (v.phase >= (float)TABLE_SIZE) v.phase -= (float)TABLE_SIZE;
      }
    }

    mix *= curVolume / 4.0f;
    mix = tanhf(mix); // Soft limiter: eliminates harsh digital clipping

    int16_t sample = (int16_t)(mix * 32767.0f);
    outBuf[2 * n]     = sample;
    outBuf[2 * n + 1] = sample;
  }

  i2s.write((uint8_t *)outBuf, sizeof(outBuf));
}

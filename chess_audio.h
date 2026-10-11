#pragma once
/*
 * Retro chiptune music + sound effects for the Guition JC3248W535.
 * The board has an NS4168 mono class-D amplifier on I2S driving the speaker connector.
 * Everything is synthesised on the fly (no audio files): 2 pulse/triangle melodic voices,
 * a quiet arpeggio voice, simple drums and a separate sound-effect voice.
 *
 * Needs arduino-esp32 3.x (uses the new ESP-IDF 5 I2S driver).
 */
#include <Arduino.h>
#include <math.h>
#include "driver/i2s_std.h"

#if defined(ESP_ARDUINO_VERSION_MAJOR) && ESP_ARDUINO_VERSION_MAJOR < 3
#error "Sound needs arduino-esp32 3.x (update the ESP32 board package in Boards Manager)"
#endif

#define AUDIO_BCLK    42
#define AUDIO_WS      2
#define AUDIO_DOUT    41
#define AUDIO_RATE    22050
#define AUDIO_VOLUME  0.5f     // 0.0 - 1.0 (master volume, lower it if it is too loud)
#define SONG_BPM      140

enum {
  SFX_TICK, SFX_SELECT, SFX_MOVE, SFX_CAPTURE, SFX_CASTLE, SFX_CHECK,
  SFX_PROMO, SFX_START, SFX_WIN, SFX_LOSE, SFX_DRAW, SFX_COUNT
};

// ---------------- sound effects: sequences of tones ----------------
// wave: 0 = 50% square, 1 = 25% square, 2 = noise.  hz = 0 means silence.
struct AudioTone { uint16_t hz; uint16_t ms; uint8_t wave; };
struct AudioSfx  { const AudioTone *t; uint8_t n; };

static const AudioTone T_TICK[]    = {{1500, 18, 0}};
static const AudioTone T_SELECT[]  = {{988, 28, 1}};
static const AudioTone T_MOVE[]    = {{330, 30, 0}, {494, 45, 0}};
static const AudioTone T_CAPTURE[] = {{300, 35, 2}, {196, 45, 0}, {131, 85, 0}};
static const AudioTone T_CASTLE[]  = {{392, 45, 0}, {523, 45, 0}, {659, 70, 0}};
static const AudioTone T_CHECK[]   = {{1047, 60, 1}, {0, 30, 0}, {1047, 60, 1}, {1397, 100, 1}};
static const AudioTone T_PROMO[]   = {{523, 60, 0}, {659, 60, 0}, {784, 60, 0}, {1047, 150, 0}};
static const AudioTone T_START[]   = {{523, 80, 0}, {659, 80, 0}, {784, 130, 0}};
static const AudioTone T_WIN[]     = {{523, 100, 0}, {659, 100, 0}, {784, 100, 0}, {1047, 180, 0},
                                      {0, 40, 0}, {784, 90, 0}, {1047, 280, 0}};
static const AudioTone T_LOSE[]    = {{392, 170, 1}, {370, 170, 1}, {349, 170, 1}, {262, 450, 1}};
static const AudioTone T_DRAW[]    = {{523, 150, 0}, {392, 150, 0}, {523, 150, 0}, {392, 320, 0}};

#define AN(a) ((uint8_t)(sizeof(a) / sizeof((a)[0])))
static const AudioSfx SFX_TABLE[SFX_COUNT] = {
  {T_TICK, AN(T_TICK)},       {T_SELECT, AN(T_SELECT)}, {T_MOVE, AN(T_MOVE)},
  {T_CAPTURE, AN(T_CAPTURE)}, {T_CASTLE, AN(T_CASTLE)}, {T_CHECK, AN(T_CHECK)},
  {T_PROMO, AN(T_PROMO)},     {T_START, AN(T_START)},   {T_WIN, AN(T_WIN)},
  {T_LOSE, AN(T_LOSE)},       {T_DRAW, AN(T_DRAW)}
};

// ---------------- the song (original loop, A minor, 8 bars) ----------------
// melody in eighth notes: MIDI note, 0 = rest, 1 = hold previous note
static const uint8_t SONG_MEL[64] = {
  76, 1, 72, 76, 81, 1, 76, 72,     // Am
  77, 1, 72, 77, 81, 1, 77, 72,     // F
  76, 1, 72, 76, 79, 1, 76, 72,     // C
  74, 1, 71, 74, 79, 1, 74, 71,     // G
  76, 1, 72, 76, 81, 1, 84, 81,     // Am
  81, 1, 77, 72, 77, 1, 81, 1,      // F
  79, 1, 74, 71, 74, 76, 77, 79,    // G
  76, 1, 71, 76, 80, 1, 76, 1       // E
};
static const uint8_t SONG_ROOT[8] = {45, 41, 48, 43, 45, 41, 43, 40};
static const uint8_t SONG_CHORD[8][3] = {
  {57, 60, 64}, {53, 57, 60}, {60, 64, 67}, {55, 59, 62},
  {57, 60, 64}, {53, 57, 60}, {55, 59, 62}, {52, 56, 59}
};
static const int8_t BASS_PAT[16] = {0, -1, 0, -1, 12, -1, 0, -1, 0, -1, 0, -1, 12, -1, 0, 0};
static const uint8_t ARP_PAT[4] = {0, 1, 2, 1};

// ---------------- shared state (written by the main loop, read by the audio task) ----------------
static volatile bool aMusicOn = false;
static volatile bool aRestart = false;
static volatile int  aSfxReq  = -1;
static volatile float aMusicVol = 0.64f;   // 0.0 - 1.0 (set with audioSetVolumes)
static volatile float aSfxVol   = 0.64f;
static i2s_chan_handle_t aTx  = NULL;

static inline float aTri(float p) { return p < 0.5f ? 4.0f * p - 1.0f : 3.0f - 4.0f * p; }
static inline float aHz(int n)    { return 440.0f * powf(2.0f, (n - 69) / 12.0f); }

static void audioTask(void *) {
  const int N = 256;
  static int16_t buf[N * 2];
  const int stepSamples = AUDIO_RATE * 15 / SONG_BPM;          // one sixteenth note
  const float R = (float)AUDIO_RATE;

  float phM = 0, phB = 0, phA = 0, phK = 0, phX = 0;
  float fM = 440, fB = 110, fA = 440;
  float gM = 0, gB = 0, gA = 0, tgM = 0, tgB = 0, tgA = 0;
  float ageM = 0, ageA = 0;
  float kickEnv = 0, snEnv = 0, hatEnv = 0;
  float musicGain = 0;
  uint32_t lfsr = 0xACE1u;
  int step = 127, stepPos = stepSamples;
  int sfxCur = -1, sfxIdx = 0, sfxPos = 0, sfxLen = 1;

  for (;;) {
    int req = aSfxReq;
    if (req >= 0 && req < SFX_COUNT) {
      aSfxReq = -1;
      sfxCur = req; sfxIdx = 0; sfxPos = 0; phX = 0;
      sfxLen = (int)SFX_TABLE[req].t[0].ms * AUDIO_RATE / 1000;
      if (sfxLen < 1) sfxLen = 1;
    }
    if (aRestart) {
      aRestart = false;
      step = 127; stepPos = stepSamples;
      tgM = tgB = tgA = 0;
      kickEnv = snEnv = hatEnv = 0;
    }

    for (int i = 0; i < N; i++) {
      musicGain += ((aMusicOn ? 1.0f : 0.0f) - musicGain) * 0.0006f;

      if (stepPos >= stepSamples) {                               // next sixteenth note
        stepPos = 0;
        step = (step + 1) & 127;
        int bar = step >> 4, s16 = step & 15;
        if ((step & 1) == 0) {
          uint8_t m = SONG_MEL[step >> 1];
          if (m == 0) tgM = 0;
          else if (m > 1) { fM = aHz(m); tgM = 1; ageM = 0; gM *= 0.4f; }
        }
        int8_t off = BASS_PAT[s16];
        if (off >= 0) { fB = aHz(SONG_ROOT[bar] + off); tgB = 1; gB *= 0.5f; }
        else tgB = 0;
        fA = aHz(SONG_CHORD[bar][ARP_PAT[step & 3]] + 12); tgA = 1; ageA = 0; gA *= 0.5f;
        if (s16 == 0 || s16 == 8) kickEnv = 1.0f;
        if (s16 == 4 || s16 == 12) snEnv = 1.0f;
        if ((s16 & 3) == 2) hatEnv = 1.0f;
      }
      stepPos++;

      uint32_t bit = (lfsr ^ (lfsr >> 2) ^ (lfsr >> 3) ^ (lfsr >> 5)) & 1u;
      lfsr = (lfsr >> 1) | (bit << 15);
      float nz = (lfsr & 1u) ? 1.0f : -1.0f;

      // melody: 25% pulse
      gM += (tgM - gM) * 0.01f;
      ageM += 1.0f;
      float envM = 1.0f - 0.45f * fminf(1.0f, ageM / (float)(stepSamples * 4));
      phM += fM / R; if (phM >= 1.0f) phM -= 1.0f;
      float vM = (phM < 0.25f ? 1.0f : -1.0f) * gM * envM * 0.20f;

      // bass: triangle
      gB += (tgB - gB) * 0.01f;
      phB += fB / R; if (phB >= 1.0f) phB -= 1.0f;
      float vB = aTri(phB) * gB * 0.30f;

      // arpeggio: thin 12.5% pulse
      gA += (tgA - gA) * 0.01f;
      ageA += 1.0f;
      float envA = 1.0f - 0.7f * fminf(1.0f, ageA / (float)stepSamples);
      phA += fA / R; if (phA >= 1.0f) phA -= 1.0f;
      float vA = (phA < 0.125f ? 1.0f : -1.0f) * gA * envA * 0.07f;

      // drums
      kickEnv *= 0.9995f; snEnv *= 0.99935f; hatEnv *= 0.9977f;
      phK += (45.0f + 110.0f * kickEnv) / R; if (phK >= 1.0f) phK -= 1.0f;
      float vD = sinf(6.2831853f * phK) * kickEnv * 0.30f + nz * snEnv * 0.15f + nz * hatEnv * 0.055f;

      float mus = (vM + vB + vA + vD) * musicGain;

      // sound effect voice
      float vX = 0;
      if (sfxCur >= 0) {
        const AudioTone &tn = SFX_TABLE[sfxCur].t[sfxIdx];
        if (tn.hz) {
          phX += (float)tn.hz / R; if (phX >= 1.0f) phX -= 1.0f;
          float w;
          if (tn.wave == 0) w = (phX < 0.5f) ? 1.0f : -1.0f;
          else if (tn.wave == 1) w = (phX < 0.25f) ? 1.0f : -1.0f;
          else w = nz;
          vX = w * 0.28f * (1.0f - 0.5f * (float)sfxPos / (float)sfxLen);
        }
        if (++sfxPos >= sfxLen) {
          sfxIdx++; sfxPos = 0;
          if (sfxIdx >= SFX_TABLE[sfxCur].n) sfxCur = -1;
          else {
            sfxLen = (int)SFX_TABLE[sfxCur].t[sfxIdx].ms * AUDIO_RATE / 1000;
            if (sfxLen < 1) sfxLen = 1;
          }
        }
      }

      float m = (mus * aMusicVol + vX * aSfxVol) * AUDIO_VOLUME;
      if (m > 1.0f) m = 1.0f; else if (m < -1.0f) m = -1.0f;
      int16_t s = (int16_t)(m * 30000.0f);
      buf[2 * i] = s;
      buf[2 * i + 1] = s;
    }
    size_t wr = 0;
    i2s_channel_write(aTx, buf, sizeof(buf), &wr, portMAX_DELAY);
  }
}

// ---------------- public API ----------------
static void audioBegin() {
  i2s_chan_config_t cc = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
  cc.auto_clear = true;
  if (i2s_new_channel(&cc, &aTx, NULL) != ESP_OK) {
    Serial.println("audio: i2s_new_channel failed");
    return;
  }
  i2s_std_config_t sc = {
    .clk_cfg  = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_RATE),
    .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
    .gpio_cfg = {
      .mclk = I2S_GPIO_UNUSED,
      .bclk = (gpio_num_t)AUDIO_BCLK,
      .ws   = (gpio_num_t)AUDIO_WS,
      .dout = (gpio_num_t)AUDIO_DOUT,
      .din  = I2S_GPIO_UNUSED,
      .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
    },
  };
  if (i2s_channel_init_std_mode(aTx, &sc) != ESP_OK || i2s_channel_enable(aTx) != ESP_OK) {
    Serial.println("audio: i2s init failed");
    aTx = NULL;
    return;
  }
  xTaskCreatePinnedToCore(audioTask, "audio", 8192, NULL, 3, NULL, 0);
  Serial.println("audio: started");
}

static void audioSfx(int id)        { if (aTx) aSfxReq = id; }
static void audioMusic(bool on)     { aMusicOn = on; }
static void audioRestartMusic()     { aRestart = true; }

// levels 0..10 (squared curve so the steps sound even)
static void audioSetVolumes(int musicLevel, int sfxLevel) {
  if (musicLevel < 0) musicLevel = 0; if (musicLevel > 10) musicLevel = 10;
  if (sfxLevel < 0) sfxLevel = 0;     if (sfxLevel > 10) sfxLevel = 10;
  aMusicVol = (musicLevel * musicLevel) / 100.0f;
  aSfxVol   = (sfxLevel * sfxLevel) / 100.0f;
}

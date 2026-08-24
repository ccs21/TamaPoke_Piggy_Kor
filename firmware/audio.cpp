#include "audio.h"
#include "adpcm_clip.h"
#include "species_chirp.h"
#include "pin_config.h"
#include <Arduino.h>
#include <LittleFS.h>
#include <Wire.h>
#include <ESP_I2S.h>
#include <Preferences.h>

// ---------------------------------------------------------------------------
// Audio del TamaPoke: códec ES8311 (DAC -> amplificador PA -> altavoz) por I2S.
// Init del ES8311 portado del driver oficial de Espressif (esp-bsp), fijado a
// MCLK=4.096MHz (256*fs), 16kHz, 16-bit, esclavo I2S. Los efectos son tonos
// cuadrados (estilo Game Boy) sintetizados en una tarea aparte para no
// bloquear el loop de juego.
// ---------------------------------------------------------------------------

#define ES8311_ADDR 0x18
#define SAMPLE_RATE 16000

static I2SClass i2s;
static bool gReady = false;
static uint8_t gMode = SOUND_FULL;
static QueueHandle_t gQ = nullptr;
static QueueHandle_t gSnorlaxHitQ = nullptr;
static TaskHandle_t gAudioTaskHandle = nullptr;
static volatile bool gBusy = false;
static volatile bool gAwakeAmpRequested = false;
static volatile bool gAwakeAmpHeld = false;
static volatile bool gMinigameSessionRequested = false;
static volatile bool gMinigameAmpHeld = false;
enum BackgroundMusic : uint8_t { BGM_NONE = 0, BGM_MINIGAME, BGM_BATTLE };
static volatile uint8_t gBgmRequested = BGM_NONE;
static uint8_t gBgmActive = BGM_NONE;
static AdpcmClip gBgmClip;
static uint8_t gPlaybackScalePct = 100;
static uint32_t gLastQueuedAt[4] = {0, 0, 0, 0};
static uint32_t gLastChirpAt = 0;
static uint32_t gLastMinigameChirpAt = 0;
static uint32_t gLastCareAlertAt = 0;

enum AudioEventKind : uint8_t {
  AUDIO_EVENT_SFX = 0,
  AUDIO_EVENT_MINIGAME_SFX,
  AUDIO_EVENT_CHIRP,
  AUDIO_EVENT_MINIGAME_CHIRP,
  AUDIO_EVENT_PIKACHU,
  AUDIO_EVENT_CARE_CHIRP,
  AUDIO_EVENT_CARE_ALERT,
  AUDIO_EVENT_PAIRING_MUSIC,
};
struct AudioEvent {
  uint8_t kind;
  uint8_t value;
};

struct SnorlaxHitEvent {
  bool critical;
};

// ---- I2C del códec ----
static bool esW(uint8_t reg, uint8_t val) {
  Wire.beginTransmission(ES8311_ADDR);
  Wire.write(reg);
  Wire.write(val);
  return Wire.endTransmission() == 0;
}
static uint8_t esR(uint8_t reg) {
  Wire.beginTransmission(ES8311_ADDR);
  Wire.write(reg);
  Wire.endTransmission(false);
  Wire.requestFrom(ES8311_ADDR, 1);
  return Wire.available() ? Wire.read() : 0;
}

// Secuencia de init VERIFICADA EN ESTA PLACA (proyecto PlaneRadar2.0, misma
// Waveshare 1.75). Clave: reloj DERIVADO DEL BCLK (reg01=0xBF, sin MCLK externo)
// y referencia interna que alimenta el DAC (reg44=0x58); sin esos dos el codec
// respondia por I2C pero no salia audio. 16kHz, 16-bit, esclavo I2S.
static bool es8311Init() {
  Wire.beginTransmission(ES8311_ADDR);
  if (Wire.endTransmission() != 0) return false;

  // open()
  esW(0x0D, 0xFA); esW(0x44, 0x08); esW(0x44, 0x08);  // power up + quirk de 1a escritura
  esW(0x01, 0x30); esW(0x02, 0x00); esW(0x03, 0x10); esW(0x16, 0x24);
  esW(0x04, 0x10); esW(0x05, 0x00); esW(0x0B, 0x00); esW(0x0C, 0x00);
  esW(0x10, 0x1F); esW(0x11, 0x7F);
  esW(0x00, 0x80); esW(0x00, 0x80);                   // reset clock, esclavo
  esW(0x01, 0xBF);                                    // clk src = BCLK (sin MCLK externo)
  { uint8_t r = esR(0x06); r &= ~0x20; esW(0x06, r); }  // SCLK no invertido
  esW(0x13, 0x10); esW(0x1B, 0x0A); esW(0x1C, 0x6A);
  esW(0x44, 0x58);                                    // referencia interna -> alimenta el DAC

  // config_sample(): BCLK*8 = DIG_MCLK
  esW(0x02, 0x18); esW(0x05, 0x00); esW(0x03, 0x10); esW(0x04, 0x20);
  { uint8_t r = esR(0x07); r &= 0xC0; esW(0x07, r); }
  esW(0x08, 0xFF);
  { uint8_t r = esR(0x06); r &= 0xE0; r |= 0x03; esW(0x06, r); }  // bclk_div=4

  // formato I2S 16-bit
  esW(0x09, 0x0C); esW(0x0A, 0x0C);

  // start() DAC esclavo
  esW(0x00, 0x80); esW(0x01, 0xBF); esW(0x09, 0x0C); esW(0x0A, 0x0C);
  esW(0x17, 0xBF); esW(0x0E, 0x02); esW(0x12, 0x00); esW(0x14, 0x1A);
  esW(0x0D, 0x01); esW(0x15, 0x40); esW(0x37, 0x08); esW(0x45, 0x00);

  // volumen + unmute
  esW(0x32, 0xBF);                                    // volumen DAC ~0 dB
  { uint8_t r = esR(0x31); r &= 0x9F; esW(0x31, r); }  // unmute
  return true;
}

// ---- sintetizador pequeño: onda, volumen, slide y ruido ----
enum Wave : uint8_t { W_SQUARE = 0, W_TRI, W_SOFT, W_NOISE };
struct Note {
  uint16_t f, ms;
  int16_t slide;
  uint8_t vol;
  uint8_t wave;
};

#define SQ(f, ms, vol)       {f, ms, 0, vol, W_SQUARE}
#define TRI(f, ms, vol)      {f, ms, 0, vol, W_TRI}
#define SOFT(f, ms, vol)     {f, ms, 0, vol, W_SOFT}
#define NS(ms, vol)          {0, ms, 0, vol, W_NOISE}
#define SL(f, ms, to, vol, w) {f, ms, (int16_t)((to) - (f)), vol, w}
#define SIL(ms)              {0, ms, 0, 0, W_SQUARE}

static const Note N_TAP[]    = {SQ(1175, 52, 88)};
static const Note N_EAT[]    = {SOFT(523, 42, 64), SIL(12), SOFT(659, 50, 70)};
static const Note N_PLAY[]   = {SL(760, 65, 1080, 92, W_TRI), SQ(1397, 55, 86)};
static const Note N_HEART[]  = {SOFT(1047, 70, 56), SIL(18), SOFT(1319, 105, 68)};
static const Note N_HATCH[]  = {TRI(523, 70, 60), TRI(659, 70, 64), TRI(784, 95, 68), SL(880, 190, 1320, 72, W_TRI)};
static const Note N_EVOLVE[] = {SL(392, 100, 560, 58, W_TRI), SL(523, 100, 740, 62, W_TRI), SL(659, 110, 960, 66, W_TRI), SL(880, 210, 1480, 76, W_SOFT)};
static const Note N_MEDAL[]  = {TRI(784, 60, 66), SIL(22), TRI(1047, 68, 72), SIL(20), SL(1175, 210, 1568, 78, W_TRI)};
static const Note N_DENY[]   = {SL(330, 120, 230, 70, W_SQUARE), SL(220, 150, 160, 64, W_SQUARE)};
static const Note N_BYE[]    = {SOFT(784, 130, 58), SOFT(659, 140, 55), SL(523, 260, 392, 54, W_SOFT)};
static const Note N_LEVEL[]  = {TRI(784, 65, 64), TRI(1047, 80, 70), SOFT(1319, 130, 70)};
static const Note N_BATTLE_WIN[]   = {TRI(659, 58, 66), TRI(784, 58, 68), TRI(988, 80, 72), SL(1175, 170, 1568, 76, W_TRI)};
static const Note N_BATTLE_LOSS[]  = {SL(392, 140, 330, 66, W_SOFT), SL(330, 140, 247, 62, W_SOFT), SOFT(196, 220, 56)};
// SFX_CATCH_OK is rendered by the dedicated three-voice fanfare below.
static const Note N_CATCH_OK[]     = {SIL(1)};
static const Note N_CATCH_FAIL[]   = {SL(300, 62, 620, 74, W_SOFT), NS(28, 52), SQ(880, 32, 66)};
// Una sola secuencia mantiene ES8311/I2S/PA activos durante los tres balanceos.
// En la placa real, tres SFX cortos separados podian perderse al apagar y volver
// a encender el amplificador. Los silencios alinean cada cierre con la imagen:
// 420 ms, 1190 ms y 1960 ms desde que comienza la captura.
static const Note N_CAPTURE_SHAKE[] = {
  SIL(412),
  SL(980, 58, 640, 94, W_SQUARE), SIL(14), SQ(520, 58, 84),
  SIL(640),
  SL(980, 58, 640, 94, W_SQUARE), SIL(14), SQ(520, 58, 84),
  SIL(640),
  SL(980, 58, 640, 94, W_SQUARE), SIL(14), SQ(520, 58, 84),
};

struct CaptureChord {
  uint16_t f1, f2, f3, ms;
};

// MIDI-style transcription of the user-provided capture fanfare.  These are
// synthesized chords, not samples from the MP3.  Timings include the short
// note tails between attacks so the complete cue lasts about 3.29 seconds.
static const CaptureChord CAPTURE_FANFARE[] = {
  {587, 698, 880, 410},  // D5 F5 A5
  {523, 622, 698, 250},  // C5 Eb5 F5
  {440, 523, 659, 750},  // A4 C5 E5
  {622, 784, 932, 130},  // Eb5 G5 Bb5
  {622, 784, 932, 120},
  {587, 659, 932, 220},  // D5 E5 Bb5
  {523, 698, 784, 130},  // C5 F5 G5
  {523, 698, 784, 130},
  {523, 659, 784, 120},  // C5 E5 G5
  {466, 784, 932, 150},  // Bb4 G5 Bb5
  {440, 523, 880, 880},  // A4 C5 A5, long release
};
static const Note N_DAILY_GOAL[]   = {TRI(1175, 50, 68), SIL(22), TRI(1568, 70, 74), SOFT(1760, 95, 68)};
static const Note N_EVENT_SPARKLE[] = {NS(35, 36), TRI(1568, 42, 56), TRI(1976, 62, 60), SIL(18), TRI(1760, 56, 54)};
static const Note N_REST[]         = {SL(523, 125, 392, 48, W_SOFT), SOFT(330, 170, 42)};
static const Note N_COUNTER[]      = {SL(784, 75, 1175, 62, W_TRI), SIL(16), SQ(1568, 70, 74), NS(40, 42)};
static const Note N_MENU[]         = {TRI(988, 56, 84), SQ(1319, 62, 90)};
static const Note N_GAME_START[]   = {TRI(659, 58, 72), TRI(880, 64, 78), SQ(1175, 74, 82)};
static const Note N_BALL_BOUNCE[]  = {SL(820, 42, 520, 72, W_SQUARE)};
static const Note N_BALL_MISS[]    = {NS(55, 56), SL(360, 110, 210, 68, W_SOFT)};
static const Note N_MEMO_STEP[]    = {SQ(1047, 54, 68)};
static const Note N_MEMO_PAD_0[]   = {SOFT(349, 82, 76)};
static const Note N_MEMO_PAD_1[]   = {TRI(523, 82, 76)};
static const Note N_MEMO_PAD_2[]   = {TRI(784, 82, 76)};
static const Note N_MEMO_PAD_3[]   = {SQ(1047, 82, 76)};
static const Note N_ATTACK_QUICK[] = {SL(980, 42, 1320, 90, W_TRI), SQ(1760, 38, 82)};
static const Note N_ATTACK_HEAVY[] = {NS(36, 46), SL(330, 74, 700, 92, W_SQUARE), SQ(880, 52, 86)};
static const Note N_ENEMY_HIT[]    = {SL(300, 70, 190, 82, W_SQUARE), NS(38, 44)};
static const Note N_EFFECTIVE[]    = {TRI(988, 48, 82), TRI(1319, 54, 90), SQ(1760, 64, 86)};
static const Note N_WEAK_HIT[]     = {SOFT(420, 70, 58), SOFT(360, 90, 50)};
static const Note N_MINIGAME_OK[]  = {SL(1047, 46, 1568, 88, W_TRI), TRI(1760, 42, 78)};
static const Note N_MINIGAME_BAD[] = {NS(42, 52), SL(300, 95, 180, 70, W_SOFT)};
static const Note N_LOW_HP[]       = {SQ(740, 70, 74), SIL(38), SQ(740, 70, 74)};
static const Note N_EXPEDITION_START[] = {TRI(523, 52, 64), TRI(659, 62, 70), SL(784, 115, 1047, 72, W_TRI)};
static const Note N_EXPEDITION_FOUND[] = {TRI(784, 55, 70), TRI(1047, 58, 76), TRI(1319, 65, 78), SOFT(1568, 130, 72)};
static const Note N_EXPEDITION_CLAIM[] = {SOFT(988, 55, 66), TRI(1319, 70, 74), SL(1568, 115, 1976, 76, W_TRI)};
static const Note N_ITEM_USE[] = {SOFT(659, 48, 62), SL(784, 95, 1175, 70, W_TRI)};
// Dedicated Diglett feedback: a rising pop for a hit and a separated,
// descending double beep for an empty hole.
// Give the physical amplifier time to settle, then use a longer mid-range pop.
// The previous 143 ms/high-pitched cue was audible on PC but vanished on-board.
static const Note N_DIGLETT_HIT[] = {SIL(28), SL(480, 170, 1180, 66, W_SQUARE), SQ(1320, 72, 62)};
static const Note N_DIGLETT_MISS[] = {SQ(440, 70, 62), SIL(28), SL(360, 140, 220, 58, W_SQUARE)};
// 짧게 위로 반짝이는 3음. 과일을 연속으로 받아도 지나치게 길게 남지 않는다.
static const Note N_EEVEE_FRUIT[] = {TRI(988, 42, 72), TRI(1319, 48, 78), SL(1568, 74, 2093, 72, W_SOFT)};

struct SfxDef { const Note *n; uint8_t len; };
static const SfxDef SFX[SFX_COUNT] = {
  {N_TAP, 1}, {N_EAT, 3}, {N_PLAY, 2}, {N_HEART, 2}, {N_HATCH, 4},
  {N_EVOLVE, 4}, {N_MEDAL, 5}, {N_DENY, 2}, {N_BYE, 3}, {N_LEVEL, 3},
  {N_BATTLE_WIN, 4}, {N_BATTLE_LOSS, 3}, {N_CATCH_OK, 1}, {N_CATCH_FAIL, 3},
  {N_CAPTURE_SHAKE, (uint8_t)(sizeof(N_CAPTURE_SHAKE) / sizeof(N_CAPTURE_SHAKE[0]))},
  {N_DAILY_GOAL, 4}, {N_EVENT_SPARKLE, 5}, {N_REST, 2}, {N_COUNTER, 4},
  {N_MENU, 2}, {N_GAME_START, 3}, {N_BALL_BOUNCE, 1}, {N_BALL_MISS, 2}, {N_MEMO_STEP, 1},
  {N_MEMO_PAD_0, 1}, {N_MEMO_PAD_1, 1}, {N_MEMO_PAD_2, 1}, {N_MEMO_PAD_3, 1},
  {N_ATTACK_QUICK, 2}, {N_ATTACK_HEAVY, 3}, {N_ENEMY_HIT, 2}, {N_EFFECTIVE, 3},
  {N_WEAK_HIT, 2}, {N_MINIGAME_OK, 2}, {N_MINIGAME_BAD, 2}, {N_LOW_HP, 3},
  {N_EXPEDITION_START, 3}, {N_EXPEDITION_FOUND, 4}, {N_EXPEDITION_CLAIM, 3}, {N_ITEM_USE, 2},
  {N_DIGLETT_HIT, 3}, {N_DIGLETT_MISS, 3}, {N_EEVEE_FRUIT, 3},
};

static const uint8_t SFX_MIN_MODE[SFX_COUNT] = {
  SOUND_FULL, // TAP: nur "viel"
  SOUND_MED,  // EAT
  SOUND_FULL, // PLAY: kleine Punkte/Klicks nur "viel"
  SOUND_MED,  // HEART
  SOUND_LOW,  // HATCH: grosses Ereignis
  SOUND_LOW,  // EVOLVE
  SOUND_LOW,  // MEDAL
  SOUND_LOW,  // DENY: wichtiges Feedback auch bei wenig
  SOUND_LOW,  // BYE
  SOUND_LOW,  // LEVEL
  SOUND_LOW,  // BATTLE_WIN
  SOUND_LOW,  // BATTLE_LOSS
  SOUND_LOW,  // CATCH_OK
  SOUND_LOW,  // CATCH_FAIL
  SOUND_LOW,  // CAPTURE_SHAKE: three audible ball locks
  SOUND_LOW,  // DAILY_GOAL
  SOUND_MED,  // EVENT_SPARKLE
  SOUND_MED,  // REST
  SOUND_MED,  // COUNTER
  SOUND_FULL, // MENU
  SOUND_MED,  // GAME_START
  SOUND_FULL, // BALL_BOUNCE
  SOUND_FULL, // BALL_MISS
  SOUND_FULL, // MEMO_STEP
  SOUND_FULL, // MEMO_PAD_0
  SOUND_FULL, // MEMO_PAD_1
  SOUND_FULL, // MEMO_PAD_2
  SOUND_FULL, // MEMO_PAD_3
  SOUND_FULL, // ATTACK_QUICK
  SOUND_FULL, // ATTACK_HEAVY
  SOUND_FULL, // ENEMY_HIT
  SOUND_MED,  // EFFECTIVE
  SOUND_FULL, // WEAK_HIT
  SOUND_MED,  // MINIGAME_OK
  SOUND_MED,  // MINIGAME_BAD
  SOUND_LOW,  // LOW_HP
  SOUND_MED,  // EXPEDITION_START
  SOUND_LOW,  // EXPEDITION_FOUND
  SOUND_MED,  // EXPEDITION_CLAIM
  SOUND_MED,  // ITEM_USE
  SOUND_MED,  // DIGLETT_HIT
  SOUND_MED,  // DIGLETT_MISS
  SOUND_MED,  // EEVEE_FRUIT
};

static int16_t buf[256 * 2];  // estéreo intercalado (L=R)

static uint16_t noiseState = 0xACE1;
static int16_t nextNoise() {
  noiseState = (uint16_t)((noiseState >> 1) ^ (-(noiseState & 1u) & 0xB400u));
  return (noiseState & 1) ? 1 : -1;
}

static int16_t oscSample(uint8_t wave, int phase, int period, int16_t amp) {
  if (wave == W_NOISE) return (int16_t)(nextNoise() * amp);
  if (period <= 1) return 0;
  int p = phase % period;
  if (wave == W_TRI) {
    int half = period / 2;
    int v = (p < half) ? (-amp + (2 * amp * p) / half) : (amp - (2 * amp * (p - half)) / (period - half));
    return (int16_t)v;
  }
  if (wave == W_SOFT) {
    int half = period / 2;
    int q = (p < half) ? p : period - p;
    int v = (2 * amp * q) / half - amp;
    return (int16_t)(v * 3 / 4);
  }
  return (p < period / 2) ? amp : -amp;
}

static uint8_t modeGainPct() {
  switch (gMode) {
    case SOUND_LOW: return 58;
    case SOUND_MED: return 82;
    case SOUND_FULL: return 118;
    default: return 0;
  }
}

static bool criticalSfx(uint8_t id) {
  // Diglett taps need immediate one-to-one feedback even in MED mode. They
  // remain muted in LOW/OFF through SFX_MIN_MODE, but bypass repeat thinning.
  return id < SFX_COUNT &&
         (SFX_MIN_MODE[id] == SOUND_LOW || id == SFX_DIGLETT_HIT ||
          id == SFX_DIGLETT_MISS || id == SFX_EEVEE_FRUIT);
}

static const char *backgroundPath(uint8_t kind) {
  if (kind == BGM_MINIGAME) return "/audio/minigame.tpa";
  if (kind == BGM_BATTLE) return "/audio/battle.tpa";
  return nullptr;
}

static void syncBackgroundRequest() {
  uint8_t requested = gBgmRequested;
  if (requested == gBgmActive) return;
  gBgmClip.unload();
  gBgmActive = BGM_NONE;
  const char *path = backgroundPath(requested);
  if (!path) return;
  if (!gBgmClip.load(path)) {
    Serial.printf("BGM load failed: %s\n", path);
    gBgmRequested = BGM_NONE;
    return;
  }
  gBgmActive = requested;
}

// Both tracks are stored at their original level.  Sixty percent of the
// pairing-event music is the normal bed; foreground effects duck it further.
static int16_t nextBackgroundSample(uint8_t duckPercent = 100) {
  if (gBgmActive == BGM_NONE || !gBgmClip.loaded()) return 0;
  int16_t raw = 0;
  if (!gBgmClip.next(&raw)) {
    // Minigames are fixed to one minute, so their track ends normally after
    // its fade. A battle can legitimately exceed one minute: keep restarting
    // that track until battleAudioEnd() closes the battle session.
    if (gBgmActive == BGM_BATTLE && gBgmRequested == BGM_BATTLE) {
      gBgmClip.rewind();
      if (!gBgmClip.next(&raw)) {
        gBgmRequested = BGM_NONE;
        return 0;
      }
    } else {
      gBgmRequested = BGM_NONE;
      return 0;
    }
  }
  int32_t sample = (int32_t)raw * modeGainPct() / 118;
  sample = sample * 60 / 100;
  sample = sample * duckPercent / 100;
  return (int16_t)sample;
}

static int16_t mixWithBackground(int32_t foreground, uint8_t duckPercent) {
  int32_t mixed = foreground + nextBackgroundSample(duckPercent);
  return (int16_t)constrain(mixed, -32767, 32767);
}

static void playBackgroundChunk() {
  constexpr int COUNT = 256;
  for (int i = 0; i < COUNT; i++) {
    int16_t sample = nextBackgroundSample();
    buf[i * 2] = sample;
    buf[i * 2 + 1] = sample;
  }
  i2s.write((uint8_t *)buf, COUNT * 4);
}

// reproduce una nota con rampa anti-click; f==0 es silencio salvo W_NOISE.
static void playTone(const Note &note) {
  uint16_t f = note.f;
  uint16_t ms = note.ms;
  int total = SAMPLE_RATE * ms / 1000;
  int done = 0;
  int phase = 0;
  int32_t scaledAmp = 7600L * note.vol * modeGainPct() / 10000;
  scaledAmp = scaledAmp * gPlaybackScalePct / 100;
  const int16_t maxAmp = (int16_t)scaledAmp;
  while (done < total) {
    int n = total - done; if (n > 256) n = 256;
    for (int i = 0; i < n; i++) {
      int16_t s = 0;
      int idx = done + i;
      if (note.wave == W_NOISE || f) {
        uint16_t curF = f;
        if (note.slide && total > 1) {
          int32_t v = (int32_t)note.f + ((int32_t)note.slide * idx) / total;
          curF = (v < 20) ? 20 : (uint16_t)v;
        }
        int period = curF ? (SAMPLE_RATE / curF) : 0;
        if (period < 2) period = 2;
        int16_t amp = maxAmp;
        if (idx < 64) s = (int16_t)(s * idx / 64);                 // ataque
        s = oscSample(note.wave, phase, period, amp);
        if (idx < 64) s = (int16_t)(s * idx / 64);
        else if (idx > total - 96) s = (int16_t)(s * (total - idx) / 96);
        phase++;
      }
      const bool foregroundActive = note.wave == W_NOISE || f;
      int16_t mixed = mixWithBackground(s, foregroundActive ? 28 : 100);
      buf[i * 2] = mixed; buf[i * 2 + 1] = mixed;
    }
    i2s.write((uint8_t *)buf, n * 4);
    done += n;
  }
}

static void playSpeciesChirp(uint8_t dex) {
  SpeciesChirpProfile profile{};
  if (!speciesChirpProfile(dex, &profile)) return;
  for (uint8_t i = 0; i < profile.count; i++) {
    const SpeciesChirpNote &src = profile.notes[i];
    Note note = { src.frequency, src.durationMs, src.slide, src.volume, src.wave };
    playTone(note);
  }
}

static void playCaptureFanfare() {
  const int16_t maxAmp = (int16_t)(8200L * modeGainPct() / 118);
  for (uint8_t chordIndex = 0;
       chordIndex < sizeof(CAPTURE_FANFARE) / sizeof(CAPTURE_FANFARE[0]);
       chordIndex++) {
    const CaptureChord &chord = CAPTURE_FANFARE[chordIndex];
    const bool finalChord = chordIndex + 1 == sizeof(CAPTURE_FANFARE) / sizeof(CAPTURE_FANFARE[0]);
    const int total = SAMPLE_RATE * chord.ms / 1000;
    const int releaseSamples = finalChord ? SAMPLE_RATE * 210 / 1000 : 96;
    int done = 0;
    int phase1 = 0, phase2 = 0, phase3 = 0;
    const int period1 = max(2, SAMPLE_RATE / chord.f1);
    const int period2 = max(2, SAMPLE_RATE / chord.f2);
    const int period3 = max(2, SAMPLE_RATE / chord.f3);
    while (done < total) {
      int count = total - done;
      if (count > 256) count = 256;
      for (int i = 0; i < count; i++) {
        int index = done + i;
        int32_t mixed = oscSample(W_SOFT, phase1++, period1, maxAmp);
        mixed += oscSample(W_TRI, phase2++, period2, maxAmp);
        mixed += oscSample(W_SQUARE, phase3++, period3, maxAmp);
        mixed /= 3;
        if (index < 56) mixed = mixed * index / 56;
        if (index > total - releaseSamples)
          mixed = mixed * max(0, total - index) / releaseSamples;
        int16_t sample = mixWithBackground(mixed, 24);
        buf[i * 2] = sample;
        buf[i * 2 + 1] = sample;
      }
      i2s.write((uint8_t *)buf, count * 4);
      done += count;
    }
  }
}

static bool playAdpcmEffect(const char *path) {
  AdpcmClip effect;
  if (!effect.load(path)) return false;
  const uint8_t gain = modeGainPct();
  bool playing = true;
  while (playing) {
    uint32_t count = 0;
    for (; count < 256; count++) {
      int16_t raw = 0;
      if (!effect.next(&raw)) {
        playing = false;
        break;
      }
      int32_t foreground = (int32_t)raw * gain / 118;
      int16_t sample = mixWithBackground(foreground, 20);
      buf[count * 2] = sample;
      buf[count * 2 + 1] = sample;
    }
    if (count) i2s.write((uint8_t *)buf, count * 4);
  }
  effect.unload();
  return true;
}

static void playPikachuReveal() {
  if (playAdpcmEffect("/audio/pikachu.tpa")) return;
  const Note fallback[] = { TRI(880, 70, 70), TRI(1175, 85, 74), TRI(1319, 100, 78) };
  for (const auto &note : fallback) playTone(note);
}

static void playCareAlert() {
  if (playAdpcmEffect("/audio/care.tpa")) return;
  const Note fallback[] = { TRI(784, 120, 76), SIL(45), TRI(988, 160, 82) };
  for (const auto &note : fallback) playTone(note);
}

// 잠만보는 30초 동안 매우 빠르게 연타하는 게임이다. 일반 SFX 큐에
// 타격음을 쌓으면 손을 뗀 뒤에도 남은 소리가 재생되므로, 짧은 보이스
// 네 개를 여기서 직접 섞는다. 새 터치는 4 ms 이내에 빈 보이스에 붙고
// 이전 타격의 꼬리와 실제로 겹칠 수 있다.
struct SnorlaxHitVoice {
  bool active;
  bool critical;
  uint16_t position;
  uint16_t total;
};

static void startSnorlaxHitVoice(SnorlaxHitVoice voices[4], bool critical) {
  uint8_t slot = 0;
  uint16_t oldest = 0;
  for (uint8_t i = 0; i < 4; i++) {
    if (!voices[i].active) {
      slot = i;
      oldest = UINT16_MAX;
      break;
    }
    if (voices[i].position >= oldest) {
      oldest = voices[i].position;
      slot = i;
    }
  }
  voices[slot] = { true, critical, 0,
                    (uint16_t)(critical ? SAMPLE_RATE * 78 / 1000
                                        : SAMPLE_RATE * 46 / 1000) };
}

static int32_t snorlaxHitPulse(uint16_t age, uint16_t length,
                               int32_t amplitude, uint16_t period) {
  if (age >= length) return 0;
  int32_t envelope;
  constexpr uint16_t ATTACK = 18;
  if (age < ATTACK) envelope = (int32_t)age * 256 / ATTACK;
  else envelope = (int32_t)(length - age) * 256 / (length - ATTACK);
  uint16_t safePeriod = period < 2 ? 2 : period;
  int32_t body = ((age / safePeriod) & 1U) ? amplitude : -amplitude;
  int32_t noise = (int32_t)nextNoise() * amplitude * 3 / 5;
  return (body + noise) * envelope / (256 * 2);
}

static void playSnorlaxHitMixer(const SnorlaxHitEvent &first) {
  SnorlaxHitVoice voices[4] = {};
  startSnorlaxHitVoice(voices, first.critical);
  const bool ampWasHeld = gAwakeAmpHeld || gMinigameAmpHeld;
  if (!ampWasHeld) {
    digitalWrite(PA, HIGH);
    delay(60);
  }

  const int32_t baseAmplitude = 9000L * modeGainPct() / 118 * 70 / 100;
  uint32_t lastTouchAt = millis();
  for (;;) {
    SnorlaxHitEvent next{};
    while (xQueueReceive(gSnorlaxHitQ, &next, 0) == pdTRUE) {
      startSnorlaxHitVoice(voices, next.critical);
      lastTouchAt = millis();
    }

    bool active = false;
    for (const auto &voice : voices) active = active || voice.active;
    // 미니게임 세션에서는 PA가 이미 계속 켜져 있으므로 실제 보이스가
    // 끝나는 즉시 태스크로 돌아간다. 세션 밖의 예외 호출만 짧게 유지한다.
    if (!active && (ampWasHeld || millis() - lastTouchAt >= 320UL)) break;

    constexpr uint16_t CHUNK = 64;  // 4 ms @ 16 kHz
    for (uint16_t sampleIndex = 0; sampleIndex < CHUNK; sampleIndex++) {
      int32_t mixed = 0;
      for (auto &voice : voices) {
        if (!voice.active) continue;
        uint16_t age = voice.position++;
        if (voice.critical) {
          // 첫 번째 '빠'와 24 ms 뒤의 더 낮은 '방'을 한 보이스에 넣는다.
          mixed += snorlaxHitPulse(age, 620, baseAmplitude, 46);
          if (age >= 384)
            mixed += snorlaxHitPulse((uint16_t)(age - 384), 760,
                                     baseAmplitude * 11 / 10, 62);
        } else {
          mixed += snorlaxHitPulse(age, voice.total, baseAmplitude * 4 / 5, 40);
        }
        if (voice.position >= voice.total) voice.active = false;
      }
      int16_t sample = (int16_t)constrain(mixed, -30000, 30000);
      sample = mixWithBackground(sample, active ? 22 : 100);
      buf[sampleIndex * 2] = sample;
      buf[sampleIndex * 2 + 1] = sample;
    }
    i2s.write((uint8_t *)buf, CHUNK * 4);
  }

  // 남은 DMA 샘플만 비운다. 입력 큐를 순차 재생하는 꼬리는 없다.
  if (!ampWasHeld) {
    delay(12);
    digitalWrite(PA, LOW);
  }
}

static void audioTask(void *) {
  AudioEvent event;
  for (;;) {
    bool hitPending = gSnorlaxHitQ && uxQueueMessagesWaiting(gSnorlaxHitQ) > 0;
    bool eventPending = gQ && uxQueueMessagesWaiting(gQ) > 0;
    bool bgmWork = gBgmRequested != gBgmActive || gBgmActive != BGM_NONE;
    bool sessionChange = (gAwakeAmpRequested != gAwakeAmpHeld) ||
                         (gMinigameSessionRequested != gMinigameAmpHeld);
    if (!hitPending && !eventPending && !sessionChange && !bgmWork) {
      ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    } else {
      // 여러 입력이 한꺼번에 들어온 경우 남은 알림 카운트는 큐 자체로
      // 확인하므로 여기서 지운다.
      ulTaskNotifyTake(pdTRUE, 0);
    }

    if (gAwakeAmpRequested && !gAwakeAmpHeld && gReady && gMode != SOUND_OFF) {
      gBusy = true;
      digitalWrite(PA, HIGH);
      // Wake the PA/codec once per active screen session.  Keeping it ready
      // until sleep avoids losing the beginning of short effects on hardware.
      const Note warmup = SIL(72);
      playTone(warmup);
      gAwakeAmpHeld = true;
      gBusy = false;
    } else if ((!gAwakeAmpRequested || !gReady || gMode == SOUND_OFF) &&
               gAwakeAmpHeld) {
      gAwakeAmpHeld = false;
      if (!gMinigameAmpHeld) digitalWrite(PA, LOW);
    }

    if (gMinigameSessionRequested && !gMinigameAmpHeld && gReady && gMode != SOUND_OFF) {
      // The awake session normally already holds the PA.  Keep this separate
      // activity hold for compatibility with battle/minigame lifetime calls.
      if (!gAwakeAmpHeld) {
        gBusy = true;
        digitalWrite(PA, HIGH);
        const Note warmup = SIL(72);
        playTone(warmup);
        gBusy = false;
      }
      gMinigameAmpHeld = true;
    } else if ((!gMinigameSessionRequested || !gReady || gMode == SOUND_OFF) &&
               gMinigameAmpHeld) {
      gMinigameAmpHeld = false;
      if (!gAwakeAmpHeld) digitalWrite(PA, LOW);
    }

    syncBackgroundRequest();

    SnorlaxHitEvent hit{};
    if (gSnorlaxHitQ && xQueueReceive(gSnorlaxHitQ, &hit, 0) == pdTRUE) {
      if (gReady && gMode != SOUND_OFF) {
        gBusy = true;
        playSnorlaxHitMixer(hit);
        gBusy = false;
      }
      continue;
    }
    if (!gReady) continue;
    if (!gQ || xQueueReceive(gQ, &event, 0) != pdTRUE) {
      if (gBgmActive != BGM_NONE && gMode != SOUND_OFF) playBackgroundChunk();
      continue;
    }
    bool isMinigame = event.kind == AUDIO_EVENT_MINIGAME_SFX ||
                      event.kind == AUDIO_EVENT_MINIGAME_CHIRP;
    bool isSfx = (event.kind == AUDIO_EVENT_SFX ||
                  event.kind == AUDIO_EVENT_MINIGAME_SFX) &&
                 event.value < SFX_COUNT;
    bool isChirp = event.value >= 1 && event.value <= 151 &&
                   ((event.kind == AUDIO_EVENT_CHIRP && gMode >= SOUND_MED) ||
                    (event.kind == AUDIO_EVENT_MINIGAME_CHIRP && gMode != SOUND_OFF) ||
                    (event.kind == AUDIO_EVENT_CARE_CHIRP && gMode != SOUND_OFF));
    bool isPikachu = event.kind == AUDIO_EVENT_PIKACHU && gMode != SOUND_OFF;
    bool isCareAlert = event.kind == AUDIO_EVENT_CARE_ALERT && gMode != SOUND_OFF;
    bool isPairingMusic = event.kind == AUDIO_EVENT_PAIRING_MUSIC && gMode != SOUND_OFF;
    const bool needsI2sWarmup = isChirp ||
                                (isSfx && event.value == SFX_EVOLVE);
    if (isSfx && gMode < SFX_MIN_MODE[event.value]) continue;
    if (isSfx || isChirp || isPikachu || isCareAlert || isPairingMusic) {
      gBusy = true;
      gPlaybackScalePct = isMinigame ? 70 : 100;
      const bool ampWasHeld = gAwakeAmpHeld || gMinigameAmpHeld;
      if (!ampWasHeld) {
        digitalWrite(PA, HIGH);  // enciende el amplificador
        if (needsI2sWarmup) {
          // Short chirps and the evolution jingle begin with meaningful audio
          // immediately.  An 8 ms GPIO delay is not enough for the board
          // amplifier/codec path to settle, so the beginning can disappear.
          // Feed real silence through I2S first, ensuring the first audible
          // sample reaches the speaker without delaying the UI animation.
          const Note warmup = SIL(72);
          playTone(warmup);
        } else {
          delay((isCareAlert || isMinigame) ? 60 : 8);
        }
      }
      if (isSfx) {
        if (event.value == SFX_EVOLVE) {
          if (!playAdpcmEffect("/audio/evolve.tpa")) {
            const SfxDef &d = SFX[event.value];
            for (uint8_t i = 0; i < d.len; i++) playTone(d.n[i]);
          }
        } else if (event.value == SFX_BYE) {
          if (!playAdpcmEffect("/audio/farewell.tpa")) {
            const SfxDef &d = SFX[event.value];
            for (uint8_t i = 0; i < d.len; i++) playTone(d.n[i]);
          }
        } else if (event.value == SFX_CATCH_OK) {
          if (!playAdpcmEffect("/audio/catch.tpa")) playCaptureFanfare();
        } else if (event.value == SFX_LOW_HP) {
          if (!playAdpcmEffect("/audio/lowhp.tpa")) {
            const SfxDef &d = SFX[event.value];
            for (uint8_t i = 0; i < d.len; i++) playTone(d.n[i]);
          }
        } else {
          const SfxDef &d = SFX[event.value];
          for (uint8_t i = 0; i < d.len; i++) playTone(d.n[i]);
        }
      } else if (isChirp) {
        playSpeciesChirp(event.value);
      } else if (isCareAlert) {
        playCareAlert();
      } else if (isPairingMusic) {
        playAdpcmEffect("/audio/marriage.tpa");
      } else {
        playPikachuReveal();
      }
      if (!gAwakeAmpHeld && !gMinigameAmpHeld) {
        delay(gMode == SOUND_FULL ? 90 : 60);  // deja salir la cola del DMA antes de cortar
        digitalWrite(PA, LOW);                 // apaga el amp entre sonidos (evita siseo)
      }
      gPlaybackScalePct = 100;
      gBusy = false;
    }
  }
}

static bool audioHardwareBegin() {
  // I2S primero: arranca el MCLK que necesita el códec para engancharse
  pinMode(PA, OUTPUT);
  digitalWrite(PA, LOW);   // amp apagado; la tarea lo enciende al reproducir

  i2s.setPins(I2S_BCK_IO, I2S_WS_IO, I2S_DO_IO, I2S_DI_IO, I2S_MCK_IO);
  if (!i2s.begin(I2S_MODE_STD, SAMPLE_RATE, I2S_DATA_BIT_WIDTH_16BIT,
                 I2S_SLOT_MODE_STEREO, I2S_STD_SLOT_BOTH)) {
    Serial.println("I2S begin fallo");
    return false;
  }
  if (!es8311Init()) {
    Serial.println("ES8311 no responde (audio off)");
    i2s.end();
    return false;
  }
  return true;
}

// Espressif's ES8311 suspend sequence. Merely lowering PA and stopping I2S
// leaves the codec's DAC, analogue reference and clock blocks powered. This
// sequence reduces the codec to its documented suspended state; audioWake()
// performs the complete board-tested initialization again.
static void es8311Suspend() {
  Wire.beginTransmission(ES8311_ADDR);
  if (Wire.endTransmission() != 0) return;
  esW(0x32, 0x00);  // DAC volume 0
  esW(0x17, 0x00);  // ADC off
  esW(0x0E, 0xFF);  // analogue PGA/ADC off
  esW(0x12, 0x02);  // DAC power down
  esW(0x14, 0x00);
  esW(0x0D, 0xFA);
  esW(0x15, 0x00);
  esW(0x02, 0x10);
  esW(0x00, 0x00);
  esW(0x00, 0x1F);
  esW(0x01, 0x30);
  esW(0x01, 0x00);
  esW(0x45, 0x00);
  esW(0x0D, 0xFC);  // analogue power down
  esW(0x02, 0x00);
}

void audioBegin() {
  if (!audioHardwareBegin()) return;

  Preferences p;
  p.begin("tamapoke", true);
  if (p.isKey("sndm")) {
    gMode = p.getUChar("sndm", SOUND_FULL);
  } else {
    gMode = p.getBool("snd", true) ? SOUND_FULL : SOUND_OFF;
  }
  p.end();
  if (gMode > SOUND_FULL) gMode = SOUND_FULL;

  gReady = true;
  gQ = xQueueCreate(32, sizeof(AudioEvent));
  gSnorlaxHitQ = xQueueCreate(12, sizeof(SnorlaxHitEvent));
  gAwakeAmpRequested = gMode != SOUND_OFF;
  gAwakeAmpHeld = false;
  xTaskCreatePinnedToCore(audioTask, "audio", 4096, nullptr, 1,
                          &gAudioTaskHandle, 0);
  if (gAudioTaskHandle) xTaskNotifyGive(gAudioTaskHandle);
  sfxPlay(SFX_HATCH);  // jingle de arranque (confirma que suena)
}

void audioPrepareSleep() {
  gBgmRequested = BGM_NONE;
  if (gAudioTaskHandle) xTaskNotifyGive(gAudioTaskHandle);
  uint32_t stopStartedAt = millis();
  while (gBgmActive != BGM_NONE && millis() - stopStartedAt < 80UL) delay(1);
  digitalWrite(PA, LOW);
  gAwakeAmpRequested = false;
  gAwakeAmpHeld = false;
  gMinigameSessionRequested = false;
  gMinigameAmpHeld = false;
  gBgmClip.unload();
  gBgmActive = BGM_NONE;
  if (!gReady) return;
  gReady = false;
  es8311Suspend();
  i2s.end();
}

void audioWake() {
  if (gReady || !gQ) return;
  gReady = audioHardwareBegin();
  gAwakeAmpRequested = gReady && gMode != SOUND_OFF;
  gAwakeAmpHeld = false;
  if (gAudioTaskHandle) xTaskNotifyGive(gAudioTaskHandle);
}

static bool queueSfx(uint8_t id, uint8_t eventKind) {
  // Silencio voluntario o hardware no disponible no debe bloquear las
  // animaciones que esperan a que aceptemos el evento.
  if (gMode == SOUND_OFF || id >= SFX_COUNT || gMode < SFX_MIN_MODE[id]) return true;
  if (!gReady || !gQ) return true;

  // Die Modi sollen sich spuerbar anfuehlen: "viel" spielt alles, "mittel"
  // laesst schnelle Wiederholungen etwas aus, "wenig" bleibt bei grossen
  // Ereignissen und klaren Warnungen.
  uint32_t now = millis();
  if (gMode == SOUND_MED && !criticalSfx(id)) {
    if (now - gLastQueuedAt[gMode] < 180UL) return false;
  } else if (gMode == SOUND_LOW) {
    if (now - gLastQueuedAt[gMode] < 650UL && !criticalSfx(id)) return false;
  }
  AudioEvent event = { eventKind, id };
  // Los sonidos importantes (incluido cada cierre de la Pokeball) esperan un
  // poco por la cola. Si aun asi esta llena, el llamador puede reintentarlos.
  TickType_t wait = criticalSfx(id) ? pdMS_TO_TICKS(120)
                                    : (gMode == SOUND_FULL ? pdMS_TO_TICKS(28) : 0);
  if (xQueueSend(gQ, &event, wait) != pdTRUE) return false;
  if (gAudioTaskHandle) xTaskNotifyGive(gAudioTaskHandle);
  gLastQueuedAt[gMode] = now;
  return true;
}

bool sfxPlay(uint8_t id) {
  return queueSfx(id, AUDIO_EVENT_SFX);
}

bool minigameSfxPlay(uint8_t id) {
  return queueSfx(id, AUDIO_EVENT_MINIGAME_SFX);
}

void minigameAudioBegin() {
  if (!gReady || gMode == SOUND_OFF) return;
  gMinigameSessionRequested = true;
  if (gAudioTaskHandle) xTaskNotifyGive(gAudioTaskHandle);
}

void minigameBgmStart() {
  if (!gReady || gMode == SOUND_OFF) return;
  gBgmRequested = BGM_MINIGAME;
  if (gAudioTaskHandle) xTaskNotifyGive(gAudioTaskHandle);
}

void minigameAudioEnd() {
  gBgmRequested = BGM_NONE;
  gMinigameSessionRequested = false;
  if (gAudioTaskHandle) xTaskNotifyGive(gAudioTaskHandle);
}

// Battles use the same physical PA-hold session as minigames, but keep normal
// (100%) SFX/chirp volume because playback scaling is selected by event kind.
// A battle and a minigame cannot be open at the same time in the UI.
void battleAudioBegin() {
  if (!gReady || gMode == SOUND_OFF) return;
  gMinigameSessionRequested = true;
  gBgmRequested = BGM_BATTLE;
  if (gAudioTaskHandle) xTaskNotifyGive(gAudioTaskHandle);
}

void battleAudioEnd() {
  gBgmRequested = BGM_NONE;
  gMinigameSessionRequested = false;
  if (gAudioTaskHandle) xTaskNotifyGive(gAudioTaskHandle);
}

void audioSetEnabled(bool on) {
  audioSetMode(on ? SOUND_FULL : SOUND_OFF);
}

bool audioEnabled() { return gMode != SOUND_OFF; }

void audioSetMode(uint8_t mode) {
  if (mode > SOUND_FULL) mode = SOUND_FULL;
  gMode = mode;
  if (gMode == SOUND_OFF) gBgmRequested = BGM_NONE;
  gAwakeAmpRequested = gReady && gMode != SOUND_OFF;
  if (gAudioTaskHandle) xTaskNotifyGive(gAudioTaskHandle);
  Preferences p;
  p.begin("tamapoke", false);
  p.putUChar("sndm", gMode);
  p.putBool("snd", gMode != SOUND_OFF);
  p.end();
}

uint8_t audioMode() { return gMode; }

bool audioBusy() {
  return gBusy || (gQ && uxQueueMessagesWaiting(gQ) > 0) ||
         (gSnorlaxHitQ && uxQueueMessagesWaiting(gSnorlaxHitQ) > 0);
}

void speciesChirpPlay(int16_t dex) {
  if (!gReady || !gQ || gMode < SOUND_MED || dex < 1 || dex > 151) return;
  uint32_t now = millis();
  if (now - gLastChirpAt < 800UL) return;
  gLastChirpAt = now;
  AudioEvent event = { AUDIO_EVENT_CHIRP, (uint8_t)dex };
  if (xQueueSend(gQ, &event, gMode == SOUND_FULL ? pdMS_TO_TICKS(28) : 0) == pdTRUE &&
      gAudioTaskHandle) xTaskNotifyGive(gAudioTaskHandle);
}

void minigameSpeciesChirpPlay(int16_t dex) {
  if (!gReady || !gQ || gMode == SOUND_OFF || dex < 1 || dex > 151) return;
  uint32_t now = millis();
  if (gLastMinigameChirpAt && now - gLastMinigameChirpAt < 500UL) return;
  gLastMinigameChirpAt = now;
  AudioEvent event = { AUDIO_EVENT_MINIGAME_CHIRP, (uint8_t)dex };
  if (xQueueSend(gQ, &event, gMode == SOUND_FULL ? pdMS_TO_TICKS(28) : 0) == pdTRUE &&
      gAudioTaskHandle) xTaskNotifyGive(gAudioTaskHandle);
}

void snorlaxHitPlay(bool critical) {
  if (!gReady || !gSnorlaxHitQ || gMode == SOUND_OFF) return;
  SnorlaxHitEvent event = { critical };
  // 55 ms 입력 제한보다 훨씬 넉넉한 12칸이다. 비정상적으로 가득 찬
  // 경우에는 새 입력을 버려 뒤늦은 재생 꼬리를 만들지 않는다.
  if (xQueueSend(gSnorlaxHitQ, &event, 0) == pdTRUE && gAudioTaskHandle)
    xTaskNotifyGive(gAudioTaskHandle);
}

void careAlertChirpPlay(int16_t dex) {
  (void)dex;
  careAlertSoundPlay();
}

void careAlertSoundPlay() {
  if (!gReady || !gQ || gMode == SOUND_OFF) return;
  const uint32_t now = millis();
  // A wake/RTC transition must never flood the audio queue with the same care
  // call. Legitimate later calls are at least 15 minutes apart.
  if (gLastCareAlertAt && now - gLastCareAlertAt < 10000UL) return;
  AudioEvent event = { AUDIO_EVENT_CARE_ALERT, 0 };
  if (xQueueSend(gQ, &event, pdMS_TO_TICKS(28)) == pdTRUE) {
    gLastCareAlertAt = now;
    if (gAudioTaskHandle) xTaskNotifyGive(gAudioTaskHandle);
  }
}

void pikachuRevealPlay() {
  if (!gReady || !gQ || gMode == SOUND_OFF) return;
  AudioEvent event = { AUDIO_EVENT_PIKACHU, 0 };
  if (xQueueSend(gQ, &event, pdMS_TO_TICKS(28)) == pdTRUE && gAudioTaskHandle)
    xTaskNotifyGive(gAudioTaskHandle);
}

void pairingMusicPlay() {
  if (!gReady || !gQ || gMode == SOUND_OFF) return;
  AudioEvent event = { AUDIO_EVENT_PAIRING_MUSIC, 0 };
  if (xQueueSend(gQ, &event, pdMS_TO_TICKS(28)) == pdTRUE && gAudioTaskHandle)
    xTaskNotifyGive(gAudioTaskHandle);
}

bool pairingMusicAvailable() {
  return LittleFS.exists("/audio/marriage.tpa");
}

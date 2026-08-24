// TamaPoke - tamagotchi pixel art inspirado en la gen 1
// para Waveshare ESP32-S3-Touch-AMOLED-1.75 / 1.75C
// Los dos modelos cargan sprites desde LittleFS interno.
//
// Librerias (Library Manager o repo de Waveshare):
//   - "GFX Library for Arduino" (moononournation), con soporte CO5300 QSPI
//   - "SensorLib" (Lewis He), driver tactil CST9217
//
// Build-All.ps1 genera las imagenes de 16MB (1.75) y 32MB (1.75C).
//
// Los sprites internos se generan con tools/pack_internal_sprites.py.

#include <Arduino.h>
#include <Wire.h>
#include <Preferences.h>
#include <esp_sleep.h>
#include <esp_timer.h>
#include <cstring>
#include "Arduino_GFX_Library.h"
#include "korean_canvas.h"
#include "TouchDrvCSTXXX.hpp"
#include "pin_config.h"
#include "species.h"
#include "dex.h"
#include "pet.h"
#include "sdmon.h"
#include "rtcbat.h"
#include "i18n.h"
#include "audio.h"
#include "battle.h"
#include "moves.h"
#include "walk_sensor.h"
#include "communication.h"
#include "time_sync.h"
#include "visual_assets.h"
#include "generated/manual_qr.h"

// Version del firmware. Subir este numero en cada release (y manifest.json para
// el instalador web). Se muestra en la pantalla de ajustes y por serie al arrancar.
#define FW_VERSION_BASE "1.48.0-ko"
#if TAMAPOKE_BOARD_175C
  #define FW_VERSION FW_VERSION_BASE "-175c"
#else
  #define FW_VERSION FW_VERSION_BASE "-175"
#endif
#define HELP_PAGE_COUNT 8
#define HELP_LINE_COUNT 6

Arduino_DataBus *bus = new Arduino_ESP32QSPI(
  LCD_CS, LCD_SCLK, LCD_SDIO0, LCD_SDIO1, LCD_SDIO2, LCD_SDIO3);
Arduino_CO5300 *panel = new Arduino_CO5300(
  bus, LCD_RESET, 0 /*rotation*/, LCD_WIDTH, LCD_HEIGHT, 6, 0, 0, 0);
// Framebuffer completo en PSRAM: dibujamos todo y hacemos flush() (sin parpadeo)
KoreanCanvas *gfx = new KoreanCanvas(LCD_WIDTH, LCD_HEIGHT, panel);

TouchDrvCST92xx touch;
Pet pet;

// sprite animado de LittleFS para la especie actual (si existe el archivo)
SdMon mon;          // sprite B/N (respaldo y minijuego si no hay PMD)
PmdMon pmd;         // sprite PMD multi-accion (pantalla principal)
PmdMon evoPmd;      // forma anterior, solo durante el parpadeo de evolucion
PmdMon wildPmd;     // rival salvaje en la pantalla de combate
PmdMon friendPmd;   // visitante del juego "amigo"
PmdMon sitterPmd;   // Chansey babysitter on the home screen
PmdMon starterPikachuPmd;  // Pikachu secreto: sprite PMD original, sin ampliar
PmdMon miniPmdA;    // fixed actors used only while a new minigame is open
PmdMon miniPmdB;
PmdMon miniPmdC;
PmdMon miniPmdD;
int16_t monFor = -2;
bool monShinyFor = false;

// comportamiento del bicho en pantalla
struct {
  uint8_t mode = 0;     // 0 idle, 1 paseo, 2 gesto one-shot
  uint8_t act = PMD_IDLE;
  uint32_t t0 = 0;      // inicio de la animacion en curso
  uint32_t until = 0;   // fin del estado actual
  float x = 233, targetX = 233;
} beh;
#define PET_GROUND 304  // linea de suelo de la mascota
PmdMon galleryPmd;  // sprite grande de la vista detalle de la galeria (PMD/TPK2, legal)

// galeria pokedex
bool galleryOpen = false;
bool galleryDirty = false;
int galleryPage = 0;        // 10 paginas de 16
int16_t galleryDetail = 0;  // dex en vista detalle, 0 = rejilla
uint8_t galleryFilter = 0;  // 0 todos, 1 criados, 2 capturados, 3 comunicacion
bool galleryShowShiny = false;  // 상세 화면의 일반/이로치 모습

bool screenOff = false;
uint32_t screenOffAt = 0;
uint64_t screenOnlyRestStartedUs = 0;
uint64_t screenOnlyNextCareCheckUs = 0;
uint32_t screenOnlyRestBaseEpoch = 0;
bool careAlertShownThisWake = false;
uint32_t wakeLoaderUntil = 0;
uint32_t ignorePwrUntil = 0;
bool suppressPwrHomeUntilRelease = false;
bool manualSleepRequested = false;
bool powerMenuOpen = false;
bool powerMenuDirty = false;
bool cardOpen = false;        // ficha del bicho (deslizar vertical)
bool kbOpen = false;          // teclado para renombrar al bicho
char nameBuf[12] = "";
uint8_t nameLen = 0;
#define CARD_COUNT 6
uint8_t cardPage = 0;
bool hubOpen = false;         // 오른쪽에서 왼쪽: 오늘의 목표/아이템/배틀/통신
bool hubDirty = true;
#define HUB_COUNT 4
uint8_t hubPage = 0;
bool moveRelearnOpen = false;
uint16_t moveRelearnCandidate = MOVE_NONE;
uint8_t boxPage = 0;
uint8_t boxSort = 0;          // 0 dex, 1 tipo, 2 criados primero
bool expeditionTrainChoiceOpen = false;
bool clockOpen = false;       // pantalla de ajuste de hora (deslizar abajo)
int clockH = 12, clockM = 0;  // hora en edicion
TimePanel timePanel = TIME_PANEL_SETTINGS;
TimeCorrectionReason timeReason = TIME_REASON_SETTINGS;
uint32_t timeGuardAtOpen = 0;
uint32_t wifiNextScanAt = 0;
uint8_t wifiApPage = 0;
char wifiSelectedSsid[33] = "";
bool wifiSelectedSecure = false;
char wifiPassword[65] = "";
uint8_t wifiPasswordLen = 0;
uint8_t wifiKeyMode = 1;      // 0 숫자, 1 대문자, 2 소문자, 3 특수문자
int8_t wifiLastGroup = -1;
uint8_t wifiGroupCycle = 0;
uint32_t wifiLastGroupAt = 0;
bool wifiAutoRecovery = false;
char manualTimeDigits[13] = "";  // YYYYMMDDHHMM
uint8_t manualTimeLen = 0;
char timeStatusText[80] = "";
bool powerSave = false;       // ahorro opcional: off por defecto
bool helpOpen = false;
uint8_t helpPage = 0;
bool uiDirty = true;
bool cardDirty = true;
bool clockDirty = true;
bool helpDirty = true;
bool keyboardDirty = true;
bool gameMenuDirty = true;
bool battleDirty = true;
bool starterDirty = true;

// escena de bano: espuma sobre el bicho y limpieza al reventar
uint32_t bathUntil = 0;
bool bathPending = false;
struct { int16_t x, y; uint8_t r, ph; } bubbles[14];
uint32_t feedMenuUntil = 0;   // selector de comida abierto hasta este millis
uint32_t nextAmbientSoundAt = 0;

// minijuego "toques": mantener la pokeball en el aire
bool gameOpen = false;
bool gameMenuOpen = false;
bool sleepMenuOpen = false;  // choose normal sleep or Chansey babysitter
uint8_t gameMode = 0;  // 0 ball, 1 catch, 2 memo, 3 Diglett, 4 type, 5 friend
uint32_t gameOverUntil = 0;
float ballX, ballY, ballVX, ballVY, gamePetX;
uint8_t gameScore, gameMisses;
float hitX, hitY;             // ultimo golpe (anillo de impacto)
uint32_t hitTime = 0;
uint32_t ballLastHitAt = 0;
bool gameNewHi = false;
uint8_t gameGain = 0;
uint32_t catchUntil = 0, catchTargetUntil = 0;
int16_t catchX = 0, catchY = 0;
uint8_t catchIcon = 0;
uint8_t memoSeq[14] = { 0 };
uint8_t memoLen = 0, memoShow = 0, memoInput = 0, memoRounds = 0;
uint32_t memoNextAt = 0;
bool memoShowing = false;
int8_t memoActivePad = -1, memoFlashPad = -1, memoHintPad = -1;
bool memoFlashGood = false;
uint32_t memoFlashUntil = 0, memoFailUntil = 0, memoTurnUntil = 0;
#define DIGLETT_MAX_ACTIVE 5
uint32_t diglettUntil = 0, diglettSpawnAt = 0;
uint32_t diglettShownAt[DIGLETT_MAX_ACTIVE] = { 0 };
uint32_t diglettHideAt[DIGLETT_MAX_ACTIVE] = { 0 };
int8_t diglettCells[DIGLETT_MAX_ACTIVE] = { -1, -1, -1, -1, -1 };
int8_t diglettLastCell = -1;
uint8_t typeEnemy = TYPE_GRASS;
uint8_t typeChoice[3] = { TYPE_FIRE, TYPE_WATER, TYPE_GRASS };
uint8_t typeCorrect = 0;
uint32_t typeUntil = 0;
int16_t friendDex = 0;
uint32_t friendPlayStartedAt = 0;
bool friendRewardApplied = false;
uint8_t friendSoundCue = 0;
uint32_t friendInviteUntil = 0;
int16_t friendInviteDex = 0;
uint32_t nextFriendCheckEpoch = 0;

// New 30-second activity games. Fixed species are loaded on entry and freed
// on exit so the regular pet and battle sprites keep their PSRAM headroom.
#define ACTIVITY_GAME_MS 60000UL
#define SNORLAX_GAME_MS 30000UL
enum : uint8_t {
  SNORLAX_HEAD_ARMS = 0,
  SNORLAX_BODY = 1,
  SNORLAX_FEET = 2,
  SNORLAX_AWAKE = 3,
};
uint32_t miniUntil = 0;
uint32_t miniPauseUntil = 0;
uint32_t miniSpawnAt = 0;
uint32_t miniLastUpdateAt = 0;
uint32_t miniStartedAt = 0;
uint32_t miniSpeedNoticeUntil = 0;
uint8_t miniSpeedStage = 0;
uint8_t runnerLane = 1;
float runnerBackgroundOffset = 0;
struct RunnerObstacle { float x; uint8_t lane; bool active; } runnerObstacles[3] = {};
float eeveeX = 233;
float eeveeTargetX = 233;
bool eeveeFacingLeft = false;
uint8_t eeveeLane = 2;
static const int16_t EEVEE_LANE_X[5] = { 86, 159, 233, 307, 380 };
struct FallingObject { float x, y; uint8_t kind; bool active; } fallingObjects[4] = {};
float magikarpY = 322, magikarpVY = 0;
float magikarpJumpScale = 1.0f;
float magikarpBackgroundOffset = 0;
struct JumpObstacle { float x; uint8_t kind; bool active; } jumpObstacles[2] = {};
uint32_t snorlaxHitAt = 0;
uint32_t snorlaxAwakeAt = 0;
bool snorlaxCritical = false;
bool snorlaxAwake = false;
bool snorlaxTimerStarted = false;

// saco de entrenamiento (entrena la fuerza)
// QMI8658 pedometer walk. The counter remains active while the AMOLED is off
// and is read again on wake; the gyro remains disabled throughout.
bool walkOpen = false;
bool walkFinished = false;
bool walkStartFailed = false;
uint16_t walkSteps = 0;
uint8_t walkTier = 0;
WalkReward walkReward = {};
uint32_t walkLastPollAt = 0;
uint16_t walkCheckpointSavedSteps = 0;
uint32_t walkCheckpointSavedAt = 0;
uint32_t walkStartedEpoch = 0;
uint64_t walkStartedUs = 0;
bool walkScreenRest = false;
void finishWalk(bool completionAlert = false);

namespace {
constexpr uint32_t WALK_CHECKPOINT_MAGIC = 0x57414C4BUL;  // WALK
constexpr uint16_t WALK_CHECKPOINT_VERSION = 2;
constexpr uint32_t WALK_MAX_SECONDS = 20UL * 60UL;

struct WalkCheckpoint {
  uint32_t magic;
  uint16_t version;
  uint8_t active;
  uint8_t reserved;
  uint32_t steps;
  uint32_t sensorRaw;
  uint32_t startedEpoch;
  uint32_t checksum;
};

uint32_t walkCheckpointChecksum(const WalkCheckpoint &state) {
  return state.magic ^ ((uint32_t)state.version << 16) ^ state.active ^
         state.steps ^ state.sensorRaw ^ state.startedEpoch ^ 0xA71C4E29UL;
}
}

void clearWalkCheckpoint() {
  Preferences preferences;
  if (preferences.begin("tp_walk", false)) {
    preferences.clear();
    preferences.end();
  }
  walkCheckpointSavedSteps = 0;
  walkCheckpointSavedAt = 0;
  walkStartedEpoch = 0;
  walkStartedUs = 0;
}

void saveWalkCheckpoint(bool force = false) {
  if (!walkOpen || walkFinished || !walkSensorActive()) return;
  const uint32_t measured = walkSensorSteps();
  walkSteps = measured > 65535UL ? 65535 : (uint16_t)measured;
  const uint32_t now = millis();
  if (!force && (walkSteps == walkCheckpointSavedSteps ||
      (walkSteps < walkCheckpointSavedSteps + 25U &&
       now - walkCheckpointSavedAt < 60000UL))) return;

  WalkCheckpoint state = {};
  state.magic = WALK_CHECKPOINT_MAGIC;
  state.version = WALK_CHECKPOINT_VERSION;
  state.active = 1;
  state.steps = walkSteps;
  state.sensorRaw = walkSensorRawSteps();
  state.startedEpoch = walkStartedEpoch;
  state.checksum = walkCheckpointChecksum(state);
  Preferences preferences;
  if (preferences.begin("tp_walk", false)) {
    preferences.putBytes("state", &state, sizeof(state));
    preferences.end();
    walkCheckpointSavedSteps = walkSteps;
    walkCheckpointSavedAt = now;
  }
}

bool restoreWalkCheckpoint() {
  WalkCheckpoint state = {};
  Preferences preferences;
  if (!preferences.begin("tp_walk", true)) return false;
  const size_t read = preferences.getBytes("state", &state, sizeof(state));
  preferences.end();
  if (read != sizeof(state) || state.magic != WALK_CHECKPOINT_MAGIC ||
      state.version != WALK_CHECKPOINT_VERSION || state.active != 1 ||
      state.checksum != walkCheckpointChecksum(state)) {
    return false;
  }
  if (!walkSensorRestore(state.steps, state.sensorRaw)) return false;

  const uint32_t restored = walkSensorSteps();
  walkSteps = restored > 65535UL ? 65535 : (uint16_t)restored;
  walkStartedEpoch = state.startedEpoch;
  walkStartedUs = (uint64_t)esp_timer_get_time();
  walkOpen = true;
  walkFinished = false;
  walkStartFailed = false;
  walkTier = 0;
  walkReward = {};
  walkLastPollAt = 0;
  cardOpen = false;
  hubOpen = false;
  pet.restoreWalkPause();
  saveWalkCheckpoint(true);
  Serial.printf("WALK restored steps=%u started=%lu\n", walkSteps,
                (unsigned long)walkStartedEpoch);
  return true;
}

uint32_t walkElapsedSeconds(uint32_t effectiveEpoch = 0) {
  if (walkStartedEpoch && effectiveEpoch >= walkStartedEpoch) {
    return effectiveEpoch - walkStartedEpoch;
  }
  if (!walkStartedUs) return 0;
  const uint64_t nowUs = (uint64_t)esp_timer_get_time();
  return nowUs >= walkStartedUs ? (uint32_t)((nowUs - walkStartedUs) / 1000000ULL) : 0;
}

bool walkTimeExpired(uint32_t effectiveEpoch = 0) {
  return walkOpen && !walkFinished && walkElapsedSeconds(effectiveEpoch) >= WALK_MAX_SECONDS;
}

CommunicationMode commSelectedMode = COMM_MODE_NONE;
bool commDone = false;
bool commWon = false;
bool commFailed = false;
uint32_t commStartedAt = 0;
int16_t commPeerSpecies = 0;
uint8_t commRounds = 0;
CommunicationReward commReward = {};
uint32_t commResultSentAt = 0;
CommunicationPetData commPeerData = {};
uint32_t commLocalNonce = 0;
uint32_t commTransaction = 0;
uint8_t commHostItemRoll = 0;
uint8_t commGuestItemRoll = 0;
uint8_t commHostShinyRoll = 1;
uint8_t commGuestShinyRoll = 1;
bool commPeerActionReady = false;
CommunicationBattleActionData commPeerAction = {};
CommunicationResultData commPendingResult = {};
bool commPendingResultReady = false;
uint32_t commPendingResultAt = 0;

// Full-screen link-pairing ceremony. The egg is persisted before this starts;
// the old local PMD remains loaded until the animation releases it.
bool pairingEventActive = false;
uint32_t pairingEventStartedAt = 0;
int16_t pairingLocalSpecies = 0;
int16_t pairingPeerSpecies = 0;
bool pairingLocalShiny = false;
bool pairingPeerShiny = false;
uint8_t pairingSoundCue = 0;
#define PAIRING_EVENT_MS 14500UL

void updateWalk(uint32_t now);
void updateCommunication(uint32_t now);
void updatePairingEvent(uint32_t now);
void renderPairingEvent();
void drawPmdActRatioM(PmdMon &m, uint8_t actId, int cx, int groundY,
                      uint32_t t, bool loop, uint8_t numerator, uint8_t denominator);

bool battleOpen = false;
bool battleResolved = false;
int16_t battleDex = 0;
uint8_t battleLevel = 1;
BattleStats battlePlayer = {};
BattleStats battleEnemy = {};
BattleResult battleResult = {};
BattleRuntime battleRun = {};
BattleTurnResult battleTurn = {};
BattleAction battleLastAction = BATTLE_BASIC;
BattleReward battleReward = {};
// Korean UTF-8 battle feedback can take three bytes per glyph.  Keep enough
// room for complete sentences such as "상대가 공격을 피했어!" plus numbers.
char battleMsg[96] = "";
uint32_t battleAttackMenuUntil = 0;
bool battleCatchOffered = false;
bool battleCatchTried = false;
bool battleCatchDone = false;
bool battleCatchSuccess = false;
bool battleRespectCatch = false;
uint8_t battleCatchChance = 0;
bool battleCaptureActive = false;
uint32_t battleCaptureStartedAt = 0;
uint8_t battleCaptureSoundStep = 0;
bool battleLowHpWarned = false;
bool battleMoveDiscFound = false;
BattleAnimationStage battleAnimStage = BATTLE_ANIM_NONE;
uint32_t battleAnimStageAt = 0;
uint16_t battleAnimPlayerHpBefore = 0;
uint16_t battleAnimEnemyHpBefore = 0;
uint16_t battleAnimPlayerHpAfterRest = 0;
bool battleAnimEnemyActs = false;
bool battleAnimPlayerActsAfterEnemy = false;
bool battleCommunication = false;
bool battleCommHost = false;
bool battleCommWaiting = false;
bool battleCommLocalActionReady = false;
BattleAction battleCommLocalAction = BATTLE_BASIC;
BattleAction battleCommEnemyAction = BATTLE_BASIC;

#define WILD_COOLDOWN_MS (20UL * 60UL * 1000UL)
#define WILD_PROMPT_MS 20000UL
#define WILD_WAKE_CHANCE_PCT 3
uint32_t wildPromptUntil = 0;
uint32_t nextWildEligible = 0;
int16_t wildPromptDex = 0;
uint8_t wildPromptLevel = 1;

void scheduleNextWild(uint32_t now);
void maybeOfferWildEncounterOnWake(uint32_t now);
bool maybeOfferFriendOnWake(uint32_t now);

#define PET_EVENT_COOLDOWN_MS (15UL * 60UL * 1000UL)
#define PET_EVENT_PROMPT_MS 18000UL
#define PET_EVENT_CHECK_MS 60000UL
uint32_t petEventUntil = 0;
uint32_t nextPetEventEligible = 0;
uint32_t lastPetEventCheck = 0;
uint8_t petEventType = PET_EVENT_BERRY;
uint32_t petEventFeedbackUntil = 0;
// UTF-8 Korean feedback can require three bytes per glyph.  The old 18-byte
// buffer truncated strings such as "잠깐만 기다려 줘" in the middle.
char petEventMsg[64] = "";
uint32_t statusNoticeUntil = 0;
char statusNoticeMsg[64] = "";

#define CX 233  // centro de la pantalla redonda
#define CY 233
#define PET_CY 202  // centro vertical del sprite

static const uint16_t INK_K = 0x18C4;  // spriteColor('k')

void loadPowerSave() {
  Preferences p;
  p.begin("tamapoke", true);
  powerSave = p.getBool("psave", false);
  p.end();
}

void setPowerSave(bool on) {
  powerSave = on;
  Preferences p;
  p.begin("tamapoke", false);
  p.putBool("psave", powerSave);
  p.end();
}

// botones de icono siguiendo el arco inferior de la pantalla redonda
// (los exteriores van mas altos para no salirse del circulo)
struct Btn {
  int16_t cx, cy;
  const char *const *icon;
};
Btn buttons[4] = {
  { 140, 390, SPR_ICON_FOOD },   // comer
  { 202, 404, SPR_ICON_PLAY },   // jugar
  { 264, 404, SPR_ICON_LIGHT },  // luz
  { 326, 390, SPR_ICON_CLEAN },  // bano
};
#define BTN_HALF 26  // boton de 52x52
#define BTN_HIT 36   // radio tactil (un poco mas generoso)

// grietas del huevo (pixeles 'k' sobre el sprite)
static const uint8_t CRACK1[][2] = { {15,8},{16,9},{15,10} };
static const uint8_t CRACK2[][2] = { {11,13},{12,14},{11,15},{20,12},{19,13},{20,14} };
// estrellas del modo noche
static const uint16_t STARS[][2] = { {120,140},{330,120},{370,210},{95,230},{280,90},{160,95} };

bool wasPressed = false;
// eleccion de inicial: los tres clasicos y, tras 30 s, Pikachu como opcion secreta
static const int16_t STARTER_DEX[3] = { 1, 4, 7 };
#define STARTER_ROW_Y 110
#define STARTER_ROW_H 70
#define STARTER_ROW_GAP 8
#define STARTER_UNLOCKED_ROW_Y 98
#define STARTER_UNLOCKED_ROW_GAP 2
#define STARTER_PIKA_X 70
#define STARTER_PIKA_Y (STARTER_UNLOCKED_ROW_Y + 3 * (STARTER_ROW_H + STARTER_UNLOCKED_ROW_GAP))
#define STARTER_PIKA_W 326
#define STARTER_PIKA_H STARTER_ROW_H
uint32_t starterOpenedAt = 0;
bool starterTimerArmed = false;
bool starterPikachuUnlocked = false;
// boton-CTA de evolucion (centrado, mitad de pantalla)
#define EVO_BTN_W 256
#define EVO_BTN_H 64
#define EVO_BTN_X (CX - EVO_BTN_W / 2)
#define EVO_BTN_Y 172
// boton-CTA de despedida (mas ancho: lleva el nombre + frase)
#define FAR_BTN_W 408
#define FAR_BTN_H 58
#define FAR_BTN_X (CX - FAR_BTN_W / 2)
#define FAR_BTN_Y 176
// el CST9217 avisa por el pin INT cuando hay datos tactiles; lo usamos para no
// leer el bus I2C mientras el chip esta dormido (esa lectura se colgaba ~1s)
volatile bool gTouchIrq = false;
void IRAM_ATTR touchIsr() { gTouchIrq = true; }
uint32_t lastRender = 0;
uint32_t lastLoopStart = 0;
uint32_t perfRenderLastMs = 0, perfRenderMaxMs = 0;
uint32_t perfLoopLastMs = 0, perfLoopMaxMs = 0;
uint32_t perfRenderCount = 0, perfRenderSkipCount = 0;
// proteccion del AMOLED: atenuado por inactividad
uint32_t lastInteract = 0;
uint8_t dimStage = 0;
bool swallowGesture = false; // el toque que despierta no acciona nada
uint32_t ignoreTouchUntil = 0;
uint32_t holdStart = 0;     // pulsacion larga sobre el bicho
uint32_t confirmUntil = 0;  // dialogo "soltar?" activo hasta este millis
uint8_t choiceKind = 0;     // dialogo de decision: 0 ninguno, 1 evolucion, 2 despedida
uint32_t choiceUntil = 0;   // se cierra solo a este millis
int16_t tX0, tY0, tXl, tYl; // gesto en curso (inicio y ultima posicion)
uint32_t tStart = 0;
bool holdFired = false;

enum ResetStage : uint8_t {
  RESET_NONE = 0,
  RESET_CONFIRM_FIRST,
  RESET_CONFIRM_FINAL,
  RESET_HOLD_REQUIRED,
  RESET_IN_PROGRESS,
};
uint8_t resetStage = RESET_NONE;
uint32_t resetHoldStartedAt = 0;
bool resetHolding = false;

void openClock();
void forceCareAlertScreenOn();

bool timeCorrectionMandatory() {
  return clockOpen && (timeReason == TIME_REASON_FIRST_BOOT ||
                       timeReason == TIME_REASON_RTC_RECOVERY);
}

void markUiDirty() {
  uiDirty = true;
  starterDirty = true;
  cardDirty = true;
  hubDirty = true;
  clockDirty = true;
  helpDirty = true;
  keyboardDirty = true;
  gameMenuDirty = true;
  battleDirty = true;
  galleryDirty = true;
  powerMenuDirty = true;
}

// Screen transitions are resolved on finger release, so they do not need an
// additional input lock.  Keeping this compatibility hook as a no-op lets
// menus accept the next swipe immediately.
void lockTouchBrief(uint16_t ms = 160) { (void)ms; }

void updateStarterUnlock(uint32_t now) {
  if (!pet.awaitingStarter() || clockOpen) {
    if (starterPikachuPmd.loaded) starterPikachuPmd.unload();
    starterTimerArmed = false;
    starterPikachuUnlocked = false;
    starterOpenedAt = 0;
    return;
  }
  if (!starterTimerArmed) {
    starterTimerArmed = true;
    starterOpenedAt = now;
    return;
  }
  if (!starterPikachuUnlocked && now - starterOpenedAt >= 30000UL) {
    starterPikachuUnlocked = true;
    starterPikachuPmd.load(25, false);
    starterDirty = true;
    lastInteract = now;
    pikachuRevealPlay();
  }
}

void performFactoryReset() {
  resetHolding = false;
  resetStage = RESET_IN_PROGRESS;
  clockDirty = true;
  renderClock();
  wifiTimeClearCredentials();
  clearWalkCheckpoint();
  pet.factoryReset();
  delay(180);
  ESP.restart();
}

const char *screenName() {
  if (pairingEventActive) return "pairing-event";
  if (powerMenuOpen) return "power-menu";
  if (clockOpen) return pet.initialClockPending() ? "first-time" : "settings";
  if (pet.awaitingStarter()) return "starter";
  if (galleryOpen) return galleryDetail ? "gallery-detail" : "gallery-grid";
  if (gameOpen) return "game";
  if (walkOpen) return walkFinished ? "walk-result" : "walk";
  if (battleOpen) return battleResolved ? "battle-result" : "battle";
  if (kbOpen) return "keyboard";
  if (helpOpen) return "help";
  if (cardOpen) return "card";
  if (hubOpen) return "battle-hub";
  if (gameMenuOpen) return "game-menu";
  if (sleepMenuOpen) return "sleep-menu";
  return screenOff ? "screen-off" : "main";
}

void closeBattle();

// PWR short press acts as a global Home button while the display is already
// awake.  Irreversible ceremonies are deliberately allowed to finish.
bool returnToMainScreen() {
  if (pet.initialClockPending() || timeCorrectionMandatory() || resetStage == RESET_IN_PROGRESS ||
      pet.evolving() || pet.ceremony != CER_NONE || pairingEventActive) return false;
  // PWR is not a cancel button for active play. These screens have their own
  // explicit finish/leave flow; abandoning them here can discard a walk,
  // desynchronize a link session, or skip game/battle result handling.
  if (walkOpen && !walkFinished) return false;
  if (gameOpen || battleOpen || communicationState() != COMM_OFF) return false;
  bool changed = galleryOpen || cardOpen || hubOpen || kbOpen || clockOpen || helpOpen ||
                 gameOpen || gameMenuOpen || sleepMenuOpen ||
                 walkOpen || battleOpen || bathUntil || feedMenuUntil ||
                 confirmUntil || choiceKind || wildPromptUntil || petEventUntil ||
                 powerMenuOpen || resetStage != RESET_NONE ||
                 communicationState() != COMM_OFF;
  if (!changed) return false;

  if (battleOpen) closeBattle();
  if (communicationState() != COMM_OFF) communicationStop();
  if (walkOpen && !walkFinished) saveWalkCheckpoint(true);
  if (walkOpen) walkSensorStop();
  if (clockOpen) wifiTimeStop();

  if (gameOpen) minigameAudioEnd();
  galleryOpen = false;
  galleryDetail = 0;
  galleryPmd.unload();
  cardOpen = false;
  cardPage = 0;
  hubOpen = false;
  hubPage = 0;
  moveRelearnOpen = false;
  moveRelearnCandidate = MOVE_NONE;
  expeditionTrainChoiceOpen = false;
  kbOpen = false;
  helpOpen = false;
  clockOpen = false;
  gameOpen = false;
  gameMenuOpen = false;
  sleepMenuOpen = false;
  powerMenuOpen = false;
  gameOverUntil = 0;
  friendPmd.unload();
  miniPmdA.unload();
  miniPmdB.unload();
  miniPmdC.unload();
  miniPmdD.unload();
  walkOpen = false;
  walkFinished = false;
  walkReward = {};
  bathUntil = 0;
  bathPending = false;
  feedMenuUntil = 0;
  confirmUntil = 0;
  choiceKind = 0;
  choiceUntil = 0;
  wildPromptUntil = 0;
  petEventUntil = 0;
  petEventFeedbackUntil = 0;
  statusNoticeUntil = 0;
  resetStage = RESET_NONE;
  resetHolding = false;
  resetHoldStartedAt = 0;
  commSelectedMode = COMM_MODE_NONE;
  commDone = false;
  commFailed = false;
  commStartedAt = 0;
  markUiDirty();
  sfxPlay(SFX_TAP);
  return true;
}

bool staticScreenClean() {
  if (pairingEventActive) return false;
  if (powerMenuOpen) return !powerMenuDirty;
  if (clockOpen) return !clockDirty;
  if (pet.awaitingStarter()) return !starterDirty;
  if (galleryOpen && !galleryDetail) return !galleryDirty;
  if (battleOpen && battleCaptureActive) return false;
  if (battleOpen && battleResolved) return !battleDirty;
  if (kbOpen) return !keyboardDirty;
  if (helpOpen) return !helpDirty;
  if (cardOpen) return !cardDirty;
  if (hubOpen) return !hubDirty;
  if (gameMenuOpen) return !gameMenuDirty;
  return false;
}

#define AUTO_SCREEN_OFF_MS 25000UL
#define AUTO_SLEEP_AFTER_OFF_MS 5000UL
#define CARE_CHECK_SECONDS (15UL * 60UL)
#define CARE_CHECK_US ((uint64_t)CARE_CHECK_SECONDS * 1000000ULL)
#define PWR_HOLD_MENU_MS 5000UL

bool activeWalkSession() {
  return walkOpen && !walkFinished && walkSensorActive();
}

void enterWalkScreenRest() {
  if (walkScreenRest || !activeWalkSession()) return;
  panel->setBrightness(0);
  panel->displayOff();
  audioPrepareSleep();
  walkScreenRest = true;
  Serial.printf("WALK screen rest steps=%u elapsed=%lus\n", walkSteps,
                (unsigned long)walkElapsedSeconds(rtcEpoch()));
}

void exitWalkScreenRest() {
  if (!walkScreenRest) return;
  panel->displayOn();
  audioWake();
  walkScreenRest = false;
  markUiDirty();
}

void noteUserActivity(uint32_t now) {
  lastInteract = now;
  dimStage = 0;
  if (screenOff) {
    exitWalkScreenRest();
    screenOff = false;
    screenOffAt = 0;
    screenOnlyRestStartedUs = 0;
    screenOnlyNextCareCheckUs = 0;
    screenOnlyRestBaseEpoch = 0;
    ignoreTouchUntil = now + 500;
    swallowGesture = true;
    wasPressed = false;
    markUiDirty();
  }
}

bool autoSleepSceneSafe() {
  if (wasPressed || audioBusy() || resetStage != RESET_NONE || pairingEventActive) return false;
  if (powerMenuOpen) return false;
  if (clockOpen) return false;
  if (communicationState() != COMM_OFF) return false;
  if (gameOpen || battleOpen || bathUntil || hubOpen) return false;
  if (pet.awaitingStarter() || feedMenuUntil || sleepMenuOpen || confirmUntil || choiceKind ||
      wildPromptUntil || petEventUntil) return false;
  if (pet.evolving() || pet.ceremony || pet.eating() || pet.showHeart()) return false;
  return true;
}

void drawWakeLoader() {
  gfx->fillScreen(0x0000);
  if (!drawVisualAsset(gfx, "/extra/L01.tvr", 58, 58)) {
    gfx->setTextColor(0xFFFF);
    gfx->setTextSize(3);
    const char *fallback = "TamaPoke";
    gfx->setCursor(CX - gfx->textWidth(fallback) / 2, 220);
    gfx->print(fallback);
  }
  gfx->flush();
}

void renderPowerMenu() {
  powerMenuDirty = false;
  gfx->fillScreen(UI_BG_NIGHT);
  gfx->setTextColor(UI_WHITE);
  gfx->setTextSize(4);
  const char *title = "전원";
  gfx->setCursor(CX - gfx->textWidth(title) / 2, 58);
  gfx->print(title);

  gfx->fillRoundRect(76, 142, 314, 66, 17, UI_BAR_BAD);
  gfx->drawRoundRect(76, 142, 314, 66, 17, UI_WHITE);
  gfx->setTextColor(UI_WHITE);
  gfx->setTextSize(3);
  const char *off = "전원 끄기";
  gfx->setCursor(CX - gfx->textWidth(off) / 2, 163);
  gfx->print(off);

  gfx->fillRoundRect(76, 230, 314, 66, 17, UI_BAR_OK);
  gfx->drawRoundRect(76, 230, 314, 66, 17, UI_WHITE);
  gfx->setTextColor(UI_INK);
  const char *sleep = "슬립";
  gfx->setCursor(CX - gfx->textWidth(sleep) / 2, 251);
  gfx->print(sleep);

  gfx->fillRoundRect(146, 354, 174, 46, 13, UI_WHITE);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(2);
  const char *cancel = "취소";
  gfx->setCursor(CX - gfx->textWidth(cancel) / 2, 368);
  gfx->print(cancel);
  gfx->flush();
}

void drawPoweringOff() {
  gfx->fillScreen(0x0000);
  gfx->setTextColor(UI_WHITE);
  gfx->setTextSize(3);
  const char *message = "전원을 끄는 중...";
  gfx->setCursor(CX - gfx->textWidth(message) / 2, 218);
  gfx->print(message);
  gfx->flush();
}

void performPowerOff() {
  powerMenuOpen = false;
  drawPoweringOff();

#ifdef _WIN32
  const uint32_t epoch = rtcEpoch();
  if (epoch) pet.setClock(epoch);
  pet.flushSave();
  persistVirtualRtc();
  hostPreferencesFlush();
  simulatedPoweredOff = true;
  screenOff = true;
  screenOffAt = millis();
  addSimulatorEvent(L"전원 메뉴: 전원 끄기");
  return;
#else
  // Persist the exact wall time and every pending action before the AXP2101
  // removes the rails. The 1.75 keeps its hardware RTC; the 1.75C will use
  // this guard for offline progress after its automatic NTP correction.
  const uint32_t epoch = rtcEpoch();
  if (epoch) pet.setClock(epoch);
  pet.flushSave();
  if (communicationState() != COMM_OFF) communicationStop();
  if (walkOpen && !walkFinished) saveWalkCheckpoint(true);
  if (walkOpen) walkSensorStop();
  wifiTimeStop();
  audioPrepareSleep();
  sdEnd();
  detachInterrupt(digitalPinToInterrupt(TP_INT));
  touch.sleep();
  panel->setBrightness(0);
  pmuDisablePanel();
  delay(120);

  Serial.println("POWER OFF");
  Serial.flush();
  if (!pwrShutdown()) {
    // This is only a defensive path for an unavailable AXP2101. A normal
    // board loses power during pwrShutdown() and never reaches this loop.
    Serial.println("AXP2101 shutdown failed");
  }
  for (;;) delay(1000);
#endif
}

void powerMenuTap(int16_t x, int16_t y) {
  if (x >= 76 && x <= 390 && y >= 142 && y <= 208) {
    performPowerOff();
    return;
  }
  if (x >= 76 && x <= 390 && y >= 230 && y <= 296) {
    powerMenuOpen = false;
    manualSleepRequested = true;
    markUiDirty();
    return;
  }
  if (x >= 146 && x <= 320 && y >= 354 && y <= 400) {
    powerMenuOpen = false;
    markUiDirty();
    sfxPlay(SFX_TAP);
  }
}

bool beginTouchController() {
  touch.setPins(TP_RESET, TP_INT);
  bool touchOk = false;
  for (int i = 0; i < 3 && !touchOk; i++) {
    touchOk = touch.begin(Wire, 0x5A, IIC_SDA, IIC_SCL);
    if (!touchOk) delay(150);
  }
  if (!touchOk) Serial.println("CST9217 no detectado");
  touch.reset();
  touch.setMaxCoordinates(LCD_WIDTH, LCD_HEIGHT);
  touch.setMirrorXY(true, true);
  pinMode(TP_INT, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(TP_INT), touchIsr, FALLING);
  return touchOk;
}

enum RestCareResult : uint8_t {
  REST_CARE_DISABLED = 0,
  REST_CARE_STABLE,
  REST_CARE_ALERT,
  REST_CARE_MISSED,
  REST_CARE_LOCKED,
};

// A care interval is measured by the monotonic ESP timer.  RTC supplies the
// wall-clock anchor, but a stalled or corrupt RTC must never prevent the
// 15-minute calculation from running (or fast-forward the pet by days).
uint32_t sleepEffectiveEpoch(uint32_t baseEpoch, uint64_t baseUs, uint64_t nowUs) {
  const uint32_t rtcNow = rtcEpoch();
  if (!baseEpoch || nowUs < baseUs) return rtcNow;

  const uint64_t elapsedSeconds = (nowUs - baseUs) / 1000000ULL;
  const uint64_t expected64 = (uint64_t)baseEpoch + elapsedSeconds;
  const uint32_t expected = expected64 > UINT32_MAX ? UINT32_MAX : (uint32_t)expected64;
  if (!rtcNow) return expected;

  const int64_t drift = (int64_t)rtcNow - (int64_t)expected;
  if (drift >= -120 && drift <= 120) return rtcNow;

  Serial.printf("SLEEP rtc drift rejected rtc=%lu expected=%lu drift=%llds\n",
                (unsigned long)rtcNow, (unsigned long)expected, (long long)drift);
  return expected;
}

uint8_t evaluateRestCare(uint32_t nowEpoch, const char *source) {
  if (!pet.tamagotchiModeEnabled()) return REST_CARE_DISABLED;

  Serial.printf("CARE check src=%s epoch=%lu stats=%u/%u/%u/%u need=%u missed=%d due=%lu\n",
                source, (unsigned long)nowEpoch, pet.fullness, pet.joy,
                pet.energy, pet.hygiene, (unsigned)pet.pendingCareNeed(),
                pet.careCallMissed(), (unsigned long)pet.careCallDeadline());

  const uint8_t mistakesBefore = pet.careMistakes;
  if (pet.applyDueCareMiss(nowEpoch)) {
    Serial.printf("CARE result=missed mistakes=%u->%u need=%u\n",
                  mistakesBefore, pet.careMistakes, (unsigned)pet.pendingCareNeed());
    return REST_CARE_MISSED;
  }

  if (pet.pendingCareNeed() != CARE_NEED_NONE) {
    if (!pet.careCallMissed() && pet.careCallDeadline() == 0) {
      Serial.printf("CARE result=alert-pending need=%u\n",
                    (unsigned)pet.pendingCareNeed());
      return REST_CARE_ALERT;
    }
    Serial.printf("CARE result=locked need=%u missed=%d due=%lu\n",
                  (unsigned)pet.pendingCareNeed(), pet.careCallMissed(),
                  (unsigned long)pet.careCallDeadline());
    return REST_CARE_LOCKED;
  }

  const CareNeed started = pet.startCareCallIfNeeded();
  if (started != CARE_NEED_NONE) {
    Serial.printf("CARE result=alert-new need=%u lowest=%u\n",
                  (unsigned)started, pet.lowestStat());
    return REST_CARE_ALERT;
  }

  Serial.printf("CARE result=stable lowest=%u\n", pet.lowestStat());
  return REST_CARE_STABLE;
}

void clearScreenOnlyRest() {
  screenOnlyRestStartedUs = 0;
  screenOnlyNextCareCheckUs = 0;
  screenOnlyRestBaseEpoch = 0;
}

// USB power deliberately blocks the hardware light-sleep loop so that the
// serial connection remains usable.  The display can still be off for hours
// during bench testing, therefore it needs the same 15-minute care scheduler.
// Pet::update() has already applied elapsed active-loop minutes before this is
// called; setClock() only persists the wall-clock anchor and does not age the
// pet a second time.
void updateScreenOnlyRest(bool eligible) {
  if (!eligible) {
    clearScreenOnlyRest();
    return;
  }

  const uint64_t monotonicNowUs = (uint64_t)esp_timer_get_time();
  if (!screenOnlyRestStartedUs) {
    screenOnlyRestStartedUs = monotonicNowUs;
    screenOnlyNextCareCheckUs = monotonicNowUs + CARE_CHECK_US;
    screenOnlyRestBaseEpoch = rtcEpoch();
    if (screenOnlyRestBaseEpoch) pet.setClock(screenOnlyRestBaseEpoch);
    if (careAlertShownThisWake && pet.pendingCareNeed() != CARE_NEED_NONE &&
        !pet.careCallMissed()) {
      pet.armCareCallDeadline(screenOnlyRestBaseEpoch);
    }
    pet.flushSave();
    Serial.printf("SLEEP screen-only enter epoch=%lu next=%lus stats=%u/%u/%u/%u need=%u due=%lu\n",
                  (unsigned long)screenOnlyRestBaseEpoch,
                  (unsigned long)CARE_CHECK_SECONDS, pet.fullness, pet.joy,
                  pet.energy, pet.hygiene, (unsigned)pet.pendingCareNeed(),
                  (unsigned long)pet.careCallDeadline());
    return;
  }

  if (monotonicNowUs < screenOnlyNextCareCheckUs) return;
  do {
    screenOnlyNextCareCheckUs += CARE_CHECK_US;
  } while (screenOnlyNextCareCheckUs <= monotonicNowUs);

  const uint32_t checkEpoch = sleepEffectiveEpoch(
      screenOnlyRestBaseEpoch, screenOnlyRestStartedUs, monotonicNowUs);
  if (checkEpoch) pet.setClock(checkEpoch);
  const uint8_t result = evaluateRestCare(checkEpoch, "screen-only");
  if (result != REST_CARE_ALERT) {
    Serial.printf("SLEEP screen-only pass result=%u next-in=%lus\n",
                  (unsigned)result, (unsigned long)CARE_CHECK_SECONDS);
    return;
  }

  careAlertShownThisWake = true;
  clearScreenOnlyRest();
  forceCareAlertScreenOn();
  if (pet.speciesId >= 1) careAlertSoundPlay();
}

// A power/RTC recovery or a real device-sleep wake is one transition, not a
// backlog of UI events. Discard transient prompts from the previous active
// session and delay ordinary ambient events until their normal cooldown has
// elapsed. The care call itself is handled separately and may still be shown
// once immediately after this reset.
void resetTransientWakeNotices(uint32_t now) {
  petEventUntil = 0;
  petEventFeedbackUntil = 0;
  statusNoticeUntil = 0;
  petEventMsg[0] = 0;
  statusNoticeMsg[0] = 0;
  wildPromptUntil = 0;
  feedMenuUntil = 0;
  confirmUntil = 0;
  choiceKind = 0;
  choiceUntil = 0;
  gameMenuOpen = false;
  sleepMenuOpen = false;
  nextPetEventEligible = now + PET_EVENT_COOLDOWN_MS;
  lastPetEventCheck = now;

  // Do not let the release/double tap used to finish time correction land on
  // a main-screen icon (notably the moon button) after the screen changes.
  ignoreTouchUntil = now + 900UL;
  swallowGesture = true;
  wasPressed = false;
  markUiDirty();
}

// El PWR del AXP2101 no llega a un GPIO RTC del ESP32. Por eso este reposo
// suspende AMOLED/ES8311, LittleFS, I2S y tactil, mantiene el ESP en light sleep
// y despierta brevemente para sondear solo el PMU (y el contador QMI durante un
// paseo). Asi se conserva el despertar por PWR sin dejar perifericos activos.
void enterDeviceSleep(bool buttonStillHeld) {
  if (communicationState() != COMM_OFF) communicationStop();
  if (clockOpen && timePanel != TIME_PANEL_SETTINGS &&
      timePanel != TIME_PANEL_CHOICE && timePanel != TIME_PANEL_MANUAL) {
    wifiTimeStop();
    timePanel = timeCorrectionMandatory() ? TIME_PANEL_CHOICE : TIME_PANEL_SETTINGS;
    clockDirty = true;
  }
  const uint64_t sleepStartedUs = (uint64_t)esp_timer_get_time();
  uint32_t sleepBaseEpoch = rtcEpoch();
  // If the calendar source momentarily fails, the last trusted save is still
  // a valid wall-clock anchor for this continuously powered sleep session.
  if (!sleepBaseEpoch) sleepBaseEpoch = pet.lastSeenEpoch;
  uint32_t sleepEpoch = sleepBaseEpoch;
  if (sleepEpoch) pet.setClock(sleepEpoch);
  if (careAlertShownThisWake && pet.pendingCareNeed() != CARE_NEED_NONE &&
      !pet.careCallMissed()) {
    pet.armCareCallDeadline(sleepEpoch);
  }
  pet.flushSave();
  clearScreenOnlyRest();

#ifndef _WIN32
  // The software detector cannot run while the ESP is suspended. Preserve
  // its current total and let the QMI8658 autonomous counter add the sleep
  // interval on top of it.
  if (walkOpen && !walkFinished && walkSensorActive()) {
    walkSensorPrepareSleep();
    const uint32_t measured = walkSensorSteps();
    walkSteps = measured > 65535UL ? 65535 : (uint16_t)measured;
    saveWalkCheckpoint(true);
  }
#endif

  Serial.printf("SLEEP enter epoch=%lu next=%lus mode=%s moon=%d sitter=%d stats=%u/%u/%u/%u need=%u due=%lu\n",
                (unsigned long)sleepEpoch, (unsigned long)CARE_CHECK_SECONDS,
                pet.tamagotchiModeEnabled() ? "tamagotchi" : "poketama",
                pet.sleeping, pet.sitterActive(sleepEpoch),
                pet.fullness, pet.joy, pet.energy, pet.hygiene,
                (unsigned)pet.pendingCareNeed(),
                (unsigned long)pet.careCallDeadline());

  panel->setBrightness(0);
  panel->displayOff();
  screenOff = true;
  detachInterrupt(digitalPinToInterrupt(TP_INT));
  touch.sleep();
  audioPrepareSleep();
  sdEnd();
  pmuDisablePanel();

  bool waitForRelease = buttonStillHeld;
  bool wakeForCare = false;
  bool wakeForWalkComplete = false;
  bool wakeForPwr = false;
  bool wakePwrNeedsRelease = false;
  uint64_t nextCareCheckUs = sleepStartedUs + CARE_CHECK_US;
  for (;;) {
    esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
    // PWR IRQ is latched by the AXP2101, so a one-second poll keeps ordinary
    // device sleep efficient without losing a short press. Active walks never
    // enter this function; their screen-only rest keeps the pedometer loop up.
    esp_sleep_enable_timer_wakeup(1000000ULL);
    esp_light_sleep_start();

    uint8_t power = pwrEvents();
    if (waitForRelease) {
      if (power & (PWR_EVENT_RELEASE | PWR_EVENT_SHORT)) waitForRelease = false;
    } else if (power & (PWR_EVENT_PRESS | PWR_EVENT_SHORT)) {
      wakeForPwr = true;
      wakePwrNeedsRelease = (power & PWR_EVENT_PRESS) != 0 &&
                            (power & (PWR_EVENT_RELEASE | PWR_EVENT_SHORT)) == 0;
      break;
    }

    const uint64_t monotonicNowUs = (uint64_t)esp_timer_get_time();
    if (walkOpen && !walkFinished) {
      if (walkSensorActive()) {
        const uint32_t measured = walkSensorSteps();
        walkSteps = measured > 65535UL ? 65535 : (uint16_t)measured;
        saveWalkCheckpoint(false);
      }
      const uint32_t walkEpoch = sleepEffectiveEpoch(
          sleepBaseEpoch, sleepStartedUs, monotonicNowUs);
      if (walkSteps >= 1000 || walkTimeExpired(walkEpoch)) {
        wakeForWalkComplete = true;
        break;
      }
    }

    if (monotonicNowUs < nextCareCheckUs) continue;

    // Advance the schedule from its original anchor. This avoids drift even
    // after many consecutive sleep intervals.
    do {
      nextCareCheckUs += CARE_CHECK_US;
    } while (nextCareCheckUs <= monotonicNowUs);

    const uint32_t checkEpoch = sleepEffectiveEpoch(
        sleepBaseEpoch, sleepStartedUs, monotonicNowUs);
    if (checkEpoch) {
      // Progression is independent of the care-call mode. Persist all elapsed
      // age and need decay every 15 minutes even in PokeTama mode, then decide
      // separately whether a Tamagotchi-style call is required.
      const uint32_t ageBefore = pet.ageMinutes;
      const uint8_t fullBefore = pet.fullness;
      const uint8_t joyBefore = pet.joy;
      const uint8_t energyBefore = pet.energy;
      const uint8_t hygieneBefore = pet.hygiene;
      pet.syncClock(checkEpoch);
      sleepEpoch = checkEpoch;
      Serial.printf("SLEEP progress epoch=%lu age=%lu->%lu stats=%u/%u/%u/%u->%u/%u/%u/%u\n",
                    (unsigned long)checkEpoch, (unsigned long)ageBefore,
                    (unsigned long)pet.ageMinutes, fullBefore, joyBefore,
                    energyBefore, hygieneBefore, pet.fullness, pet.joy,
                    pet.energy, pet.hygiene);
    }

    if (!pet.tamagotchiModeEnabled()) continue;

    const uint8_t result = evaluateRestCare(checkEpoch, "device-sleep");
    if (result == REST_CARE_ALERT) {
      wakeForCare = true;
      break;
    }
  }

  const uint64_t wakeUs = (uint64_t)esp_timer_get_time();
  uint32_t wakeEpoch = sleepEffectiveEpoch(sleepBaseEpoch, sleepStartedUs, wakeUs);
  if (wakeEpoch) {
    const uint32_t ageBefore = pet.ageMinutes;
    const uint8_t fullBefore = pet.fullness;
    const uint8_t joyBefore = pet.joy;
    const uint8_t energyBefore = pet.energy;
    const uint8_t hygieneBefore = pet.hygiene;
    pet.syncClock(wakeEpoch);
    Serial.printf("SLEEP wake epoch=%lu age=%lu->%lu stats=%u/%u/%u/%u->%u/%u/%u/%u\n",
                  (unsigned long)wakeEpoch, (unsigned long)ageBefore,
                  (unsigned long)pet.ageMinutes, fullBefore, joyBefore,
                  energyBefore, hygieneBefore, pet.fullness, pet.joy,
                  pet.energy, pet.hygiene);
  }
  pmuEnablePanel();
#if PANEL_TOUCH_SHARE_RESET
  // 1.75C: TP_RESET and LCD_RESET share GPIO2. Reset touch first, then panel.
  touch.reset();
  touch.setMaxCoordinates(LCD_WIDTH, LCD_HEIGHT);
  touch.setMirrorXY(true, true);
  pinMode(TP_INT, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(TP_INT), touchIsr, FALLING);
#endif
  if (!panel->begin(80000000)) Serial.println("panel wake fallo");
  panel->setBrightness(180);
  drawWakeLoader();
  wakeLoaderUntil = millis() + 800UL;
#if !PANEL_TOUCH_SHARE_RESET
  // Original 1.75 has independent reset pins, so the panel can wake first.
  touch.reset();
  touch.setMaxCoordinates(LCD_WIDTH, LCD_HEIGHT);
  touch.setMirrorXY(true, true);
  pinMode(TP_INT, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(TP_INT), touchIsr, FALLING);
#endif
  sdBegin();
  audioWake();

#ifndef _WIN32
  if (walkOpen && !walkFinished && walkSensorActive()) {
    walkSensorResumeFromSleep();
    const uint32_t measured = walkSensorSteps();
    walkSteps = measured > 65535UL ? 65535 : (uint16_t)measured;
    walkLastPollAt = 0;
    saveWalkCheckpoint(true);
  }
#endif

  careAlertShownThisWake = false;
  screenOff = false;
  screenOffAt = 0;
  lastInteract = millis();
  dimStage = 0;
  ignoreTouchUntil = millis() + 900;
  // AXP2101 can publish PRESS first and the matching SHORT/RELEASE after the
  // display has already resumed. Drain that tail without interpreting it as
  // the global Home shortcut that would otherwise cancel an active walk.
  if (wakeForPwr) {
    ignorePwrUntil = millis() + 1500UL;
    // AXP2101 often reports PRESS to wake the device and delivers the matching
    // SHORT/RELEASE later. Consume that tail instead of treating it as a new
    // awake-screen Home command, which previously cancelled an active walk.
    suppressPwrHomeUntilRelease = wakePwrNeedsRelease;
  }
  swallowGesture = true;
  wasPressed = false;
  resetTransientWakeNotices(millis());
  if (wakeForWalkComplete) {
    // finishWalk stops QMI, calculates the 1000-step reward and queues the
    // completion sound after ES8311 has been fully reinitialized.
    finishWalk(true);
    markUiDirty();
  }
  // Random encounters are a rare wake-up event, never a background minute
  // lottery. Care alarms keep priority and do not also trigger a battle.
  if (!wakeForCare && !wakeForWalkComplete) {
    bool friendOffered = maybeOfferFriendOnWake(millis());
    if (!friendOffered) maybeOfferWildEncounterOnWake(millis());
  }
  if (wakeForCare && !careAlertShownThisWake) {
    careAlertShownThisWake = true;
    forceCareAlertScreenOn();
    if (pet.speciesId >= 1) careAlertSoundPlay();
  }
}

void forceCareAlertScreenOn() {
  const uint32_t now = millis();
  screenOff = false;
  screenOffAt = 0;
  dimStage = 0;
  lastInteract = now;
  panel->setBrightness(180);
  markUiDirty();
}

void setup() {
  Serial.setRxBufferSize(8192);  // mantenimiento de LittleFS en bloques de 2 KB
  Serial.begin(115200);
  // CRITICO: sin esto, Serial.print BLOQUEA el juego cuando no hay un
  // monitor serie abierto en el host (el bufer TX del USB CDC se llena
  // y nadie lo vacia) -> con timeout 0 los mensajes se descartan
  Serial.setTxTimeoutMs(0);
  Serial.printf("TamaPoke fw v%s\n", FW_VERSION);
  loadLang();  // idioma guardado (ES por defecto)
  Wire.begin(IIC_SDA, IIC_SCL);
  // CST9217 and AXP2101 share this I2C bus. Only the original 1.75 also has a
  // PCF85063; the 1.75C uses the ESP software clock initialized below.
  // Red de seguridad para PMU/RTC (SensorLib NO respeta este timeout en el
  // tactil; el cuelgue del tactil dormido se resuelve gateando por INT, ver
  // handleTouch).
  Wire.setTimeOut(50);

  // Board-specific OLED power handling is isolated in rtcbat.cpp.
  pmuEnablePanel();

#if !TAMAPOKE_BOARD_175C
  // esptool의 RTS 재시작은 AXP2101과 OLED 전원 상태까지 초기화하지 않는다.
  // 배터리가 연결된 완성품도 케이블 재연결 없이 부팅할 수 있도록 패널
  // 전원 레일을 한 번 완전히 내렸다가 다시 올린다.
  pmuDisablePanel();
  delay(80);
  pmuEnablePanel();
  delay(120);
#endif

#if PANEL_TOUCH_SHARE_RESET
  // 1.75C shares GPIO2 reset, so touch must be initialized before the panel.
  beginTouchController();
#endif

  // QSPI a 80MHz (por defecto 40): el flush del framebuffer es el cuello de
  // botella del fps (~56ms a 40MHz). Si el panel mostrara basura, bajar a 40M.
  // Optional artwork and all downloaded sprites live in the same LittleFS
  // image, so mount it before the first loading frame is drawn.
  sdBegin();
  if (!gfx->begin(80000000)) Serial.println("gfx->begin() fallo");
  panel->setBrightness(180);
  drawWakeLoader();
#if !PANEL_TOUCH_SHARE_RESET
  beginTouchController();
#endif

  pet.begin();
  Serial.printf("SAVE %s spec=%d lv=%u wins=%u losses=%u streak=%u\n",
                pet.saveLoadedFromNvs ? "loaded" : "created",
                pet.speciesId, pet.level(), pet.battleWins, pet.battleLosses, pet.battleStreak);
  thumbs.load();

  // reloj real: aplica el tiempo que estuvo apagado
  rtcBegin();
  batBegin();
  pwrSetup();
  restoreWalkCheckpoint();
  uint32_t e = rtcEpoch();
  const uint32_t guard = pet.rtcGuardEpoch;
  const bool rtcStopped = !rtcClockIntegrityValid();
  const bool rtcRollback = guard != 0 && e <= guard;
  const bool needsClock = pet.initialClockPending() || e == 0 || rtcStopped || rtcRollback;
  if (needsClock) {
    // rtc_guard/seen은 보정 성공 전까지 절대 덮어쓰지 않는다. 그래야 방전
    // 전 마지막 정상 저장시각부터 오프라인 진행을 다시 계산할 수 있다.
    pet.lastSeenEpoch = guard;
    openTimeCorrection(pet.initialClockPending() ? TIME_REASON_FIRST_BOOT
                                                 : TIME_REASON_RTC_RECOVERY);
    Serial.printf("RTC correction required: epoch=%lu guard=%lu os=%d rollback=%d\n",
                  (unsigned long)e, (unsigned long)guard, rtcStopped, rtcRollback);
  } else {
    pet.syncClock(e);
  }
  loadPowerSave();

  audioBegin();  // ES8311 + I2S + amplificador (suena un jingle de arranque)

  // A fresh game confirms local time before the starter selection.  The flag
  // is persisted separately so a power loss on this screen resumes here, while
  // existing saves upgraded from an older firmware are not interrupted.
  if (!needsClock && pet.tamagotchiModeEnabled() && pet.pendingCareNeed() != CARE_NEED_NONE &&
      !pet.careCallMissed() && pet.careCallDeadline() == 0) {
    careAlertShownThisWake = true;
    forceCareAlertScreenOn();
    if (pet.speciesId >= 1) careAlertSoundPlay();
  }

  lastInteract = millis();
  scheduleNextWild(lastInteract);
}

// carga/descarga el sprite de LittleFS cuando cambia la especie
void ensureMon() {
  if (pet.speciesId == monFor && monShinyFor == pet.shiny && !sdDirty) return;
  sdDirty = false;
  monFor = pet.speciesId;
  monShinyFor = pet.shiny;
  mon.unload();
  pmd.unload();
  beh.x = beh.targetX = 233;
  beh.mode = 0;
  beh.until = 0;
  if (pet.speciesId >= 1 && pet.speciesId <= DEX_COUNT) {
    pmd.load(pet.speciesId, pet.shiny);          // principal: PMD
    if (!pmd.loaded) mon.load(pet.speciesId, pet.shiny);  // respaldo: B/N
  }
}

bool mainScreenReadyForAmbientSound() {
  if (audioMode() != SOUND_FULL || screenOff || dimStage > 0) return false;
  if (powerMenuOpen) return false;
  if (pet.awaitingStarter() || pet.isEgg() || pet.sleeping || pet.ceremony) return false;
  if (battleOpen || gameOpen || gameMenuOpen || walkOpen || cardOpen || galleryOpen || kbOpen || clockOpen || helpOpen) return false;
  if (feedMenuUntil || confirmUntil || choiceKind || bathUntil || wildPromptUntil || petEventUntil) return false;
  if (pet.evolving() || pet.wantEvolveButton() || pet.canRunawayNow() || pet.wantFarewellButton()) return false;
  return true;
}

void maybePlayAmbientSound(uint32_t now) {
  if (!mainScreenReadyForAmbientSound()) {
    nextAmbientSoundAt = now + 9000;
    return;
  }
  if (nextAmbientSoundAt == 0) nextAmbientSoundAt = now + 8000 + random(8000);
  if (now < nextAmbientSoundAt) return;
  uint8_t r = (uint8_t)random(3);
  sfxPlay(r == 0 ? SFX_HEART : (r == 1 ? SFX_EVENT_SPARKLE : SFX_MENU));
  nextAmbientSoundAt = now + 8000 + random(8000);
}

uint16_t renderIntervalMs() {
  if (screenOff) return 5000;
  if (pairingEventActive) return 70;
  if (powerMenuOpen) return 140;
  if (battleOpen) return (battleCaptureActive || battleAnimStage != BATTLE_ANIM_NONE) ? 70 : (battleResolved ? 320 : 190);
  if (gameOpen || walkOpen) return 115;
  // Static pages skip drawing when clean, so checking them at 100 ms is cheap
  // and makes consecutive card/gallery swipes feel immediate.
  if (galleryOpen || cardOpen) return 100;
  if (kbOpen || clockOpen || helpOpen || gameMenuOpen) return powerSave ? 240 : 140;
  if (!powerSave) return 100;
  if (dimStage >= 2) return 650;
  if (dimStage >= 1) return 350;
  return 180;
}

bool lightSleepAllowed(uint32_t now) {
  if (!powerSave || usbPresent() || audioBusy() || Serial.available()) return false;
  // During a walk, software step detection and the 20-minute deadline must
  // keep running even after the AMOLED is switched off.
  if (activeWalkSession()) return false;
  if (pairingEventActive) return false;
  if (powerMenuOpen) return false;
  if (!screenOff && dimStage == 0) return false;
  if (!screenOff && now - lastInteract < 1500UL) return false;
  if (wasPressed || gTouchIrq || now < ignoreTouchUntil) return false;
  if (gameOpen || battleOpen || bathUntil) return false;
  if (pet.awaitingStarter() || feedMenuUntil || confirmUntil || choiceKind || wildPromptUntil || petEventUntil) return false;
  if (pet.evolving() || pet.ceremony || pet.eating() || pet.showHeart()) return false;
  if (galleryOpen || cardOpen || kbOpen || clockOpen || helpOpen || gameMenuOpen) return false;
  return true;
}

uint16_t lightSleepMs(uint32_t now) {
  if (!lightSleepAllowed(now)) return 0;
  uint16_t maxMs = screenOff ? 750 : (dimStage >= 2 ? 300 : 180);
  uint16_t ri = renderIntervalMs();
  uint32_t sinceRender = now - lastRender;
  if (sinceRender < ri) {
    uint32_t untilRender = ri - sinceRender;
    if (untilRender < maxMs) maxMs = untilRender;
  }
  if (maxMs < 40) return 0;
  return maxMs;
}

void maybeLightSleep(uint32_t now) {
  uint16_t ms = lightSleepMs(now);
  if (!ms) return;

  esp_sleep_disable_wakeup_source(ESP_SLEEP_WAKEUP_ALL);
  esp_sleep_enable_timer_wakeup((uint64_t)ms * 1000ULL);
  esp_sleep_enable_ext0_wakeup((gpio_num_t)TP_INT, 0);
  esp_light_sleep_start();

  if (digitalRead(TP_INT) == LOW || esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_EXT0) {
    gTouchIrq = true;
  }
}

void loop() {
  uint32_t now = millis();
  uint32_t loopStart = now;
  updateTimeCorrection(now);
  // updateTimeCorrection() may call pet.syncClock(), which refreshes lastTick
  // with a newer millis() value.  Never pass the older loop timestamp into
  // Pet::update or unsigned time arithmetic can simulate a 49-day jump.
  now = millis();
  if (!timeCorrectionMandatory()) pet.update(now);
  updateStarterUnlock(now);
  updateBattleAnimation(now);
  updateWalk(now);
  updateCommunication(now);
  updatePairingEvent(now);

  // avisa con un sonido cuando el bicho pasa a estar listo para evolucionar
  // (incluye el caso de cumplir al despertar). canEvolveNow es false durmiendo.
  static bool wasEvoReady = false;
  bool evoReady = pet.wantEvolveButton();
  if (evoReady && !wasEvoReady) sfxPlay(SFX_MEDAL);
  wasEvoReady = evoReady;
  // aviso sombrio cuando el bicho esta a punto de escaparse por abandono
  static bool wasRunReady = false;
  bool runReady = pet.canRunawayNow();
  if (runReady && !wasRunReady) sfxPlay(SFX_DENY);
  wasRunReady = runReady;

  handleTouch();
  handleSerial();
  // createCommunicationEgg() changes speciesId immediately. Keep the old PMD
  // alive until the full-screen pairing ceremony has finished.
  if (!pairingEventActive) ensureMon();
  bool sitterNow = pet.sitterActive(pet.lastSeenEpoch);
  if (sitterNow && !sitterPmd.loaded) sitterPmd.load(113, false);
  else if (!sitterNow && sitterPmd.loaded) sitterPmd.unload();
  setPowerCacheInterval(powerSave ? 10000UL : 2000UL);
  pet.ensureDailyGoals();
  maybeOfferPetEvent(now);
  if (friendInviteUntil && (int32_t)(now - friendInviteUntil) >= 0) {
    int16_t visitor = friendInviteDex;
    friendInviteUntil = 0;
    friendInviteDex = 0;
    if (visitor > 0) startFriendGameWithDex(visitor);
  }
  maybePlayAmbientSound(now);

  // PWR corto = actividad/Home. Mantener 5 s abre el menu de energia. El AXP
  // conserva un reinicio de emergencia si el firmware llegara a bloquearse.
  static uint32_t lastPwr = 0;
  static uint32_t pwrHeldFrom = 0;
  static bool pwrHomeEligible = false;
  if (now - lastPwr > 250) {
    lastPwr = now;
    uint8_t power = pwrEvents();
    if (pairingEventActive) {
      power = PWR_EVENT_NONE;
      pwrHeldFrom = 0;
      pwrHomeEligible = false;
    }
    if (suppressPwrHomeUntilRelease) {
      if (power & (PWR_EVENT_RELEASE | PWR_EVENT_SHORT)) {
        suppressPwrHomeUntilRelease = false;
      }
      power = PWR_EVENT_NONE;
    }
    if ((int32_t)(now - ignorePwrUntil) < 0) power = PWR_EVENT_NONE;
    if (power & PWR_EVENT_PRESS) {
      pwrHomeEligible = !screenOff;
      pwrHeldFrom = now;
      noteUserActivity(now);
    }
    if ((power & PWR_EVENT_SHORT) && pwrHeldFrom == 0) {
      bool wasAwake = !screenOff;
      noteUserActivity(now);
      if (wasAwake) returnToMainScreen();
      pwrHomeEligible = false;
    }
    if (power & PWR_EVENT_RELEASE) {
      if (pwrHeldFrom && now - pwrHeldFrom >= PWR_HOLD_MENU_MS) {
        powerMenuOpen = true;
        powerMenuDirty = true;
        noteUserActivity(now);
      } else if (pwrHeldFrom && pwrHomeEligible) {
        returnToMainScreen();
      }
      pwrHeldFrom = 0;
      pwrHomeEligible = false;
    }
  }
  if (pwrHeldFrom && now - pwrHeldFrom >= PWR_HOLD_MENU_MS) {
    pwrHeldFrom = 0;
    pwrHomeEligible = false;
    powerMenuOpen = true;
    powerMenuDirty = true;
    noteUserActivity(now);
  }

  // Touch and PWR handling above may have advanced lastInteract beyond the
  // loop timestamp captured at the top.  Always use a fresh timestamp here;
  // otherwise unsigned subtraction can look like roughly 49 days of idle time
  // and switch the display off immediately after a harmless background tap.
  now = millis();
  updateBrightness(now);

  const bool sleepSceneReady = screenOff && screenOffAt &&
                               now - screenOffAt >= AUTO_SLEEP_AFTER_OFF_MS &&
                               autoSleepSceneSafe();
  const bool usbConnected = usbPresent();
  const bool walkActive = activeWalkSession();
  bool autoSleepDue = sleepSceneReady && !usbConnected && !walkActive;
  // The secret Pikachu choice unlocks after 30 seconds on this screen. Never
  // allow either automatic or manual sleep to interrupt that waiting period.
  if (manualSleepRequested && walkActive) {
    manualSleepRequested = false;
    screenOff = true;
    screenOffAt = now;
    enterWalkScreenRest();
    now = millis();
  } else if (manualSleepRequested && !pairingEventActive && !audioBusy() &&
      resetStage != RESET_IN_PROGRESS) {
    manualSleepRequested = false;
    enterDeviceSleep(false);
    now = millis();
  } else if (autoSleepDue && !audioBusy() && !pet.awaitingStarter() && !clockOpen &&
             communicationState() == COMM_OFF) {
    enterDeviceSleep(false);
    now = millis();
  }

  // With USB attached, keep serial alive but run the exact same 15-minute
  // care decision while the display remains off.  This also makes connected
  // diagnostics representative instead of silently disabling care calls.
  updateScreenOnlyRest(sleepSceneReady && usbConnected &&
                       !pet.awaitingStarter() && !clockOpen &&
                       communicationState() == COMM_OFF);

  // vuelca el autoguardado periodico SOLO con la pantalla atenuada/apagada o
  // durmiendo: la escritura a NVS congela ~1s ambos cores (caché de flash off),
  // y aqui no hay animacion que se corte ni dedo esperando respuesta. Con 90s
  // de inactividad la pantalla ya atenua, asi que se vuelca enseguida; el uso
  // activo persiste igual por los guardados de cada accion (comer/jugar/...).
  if (pet.savePending() && (screenOff || dimStage >= 1 || pet.sleeping)) {
    pet.flushSave();
  }

  // anota la hora real cada 30 s (se persiste en cada save del juego)
  static uint32_t lastClock = 0;
  uint32_t clockPollMs = powerSave ? 60000UL : 30000UL;
  if (now - lastClock > clockPollMs) {
    lastClock = now;
    uint32_t e = rtcEpoch();
    if (e) {
      pet.lastSeenEpoch = e;
      if (pet.expireSitterIfNeeded(e)) {
        sitterPmd.unload();
        markUiDirty();
      }
    }
  }

  // latido de salud cada 5 min (para el soak test; se descarta si no hay monitor)
  static uint32_t lastHealth = 0;
  uint32_t healthMs = powerSave ? 600000UL : 300000UL;
  if (now - lastHealth > healthMs) {
    lastHealth = now;
    Serial.printf("HEALTH up=%lus heap=%u min=%u rMax=%lums lMax=%lums screen=%s\n",
                  (unsigned long)(now / 1000), ESP.getFreeHeap(), ESP.getMinFreeHeap(),
                  (unsigned long)perfRenderMaxMs, (unsigned long)perfLoopMaxMs, screenName());
    perfRenderMaxMs = 0;
    perfLoopMaxMs = 0;
  }

  // Juego/combate usan intervalos conservadores para que el redibujado no pise
  // el envio DMA del frame anterior; las pantallas estaticas se saltan si no
  // estan "dirty".
  // While a walk is resting, keep the pedometer/timer loop responsive but do
  // not render frames into a powered-down AMOLED controller.
  if (!walkScreenRest && now - lastRender >= renderIntervalMs()) {
    lastRender = now;
    uint32_t rt0 = millis();
    render();
    perfRenderLastMs = millis() - rt0;
    if (perfRenderLastMs > perfRenderMaxMs) perfRenderMaxMs = perfRenderLastMs;
    perfRenderCount++;
  }

  maybeLightSleep(millis());
  perfLoopLastMs = millis() - loopStart;
  if (perfLoopLastMs > perfLoopMaxMs) perfLoopMaxMs = perfLoopLastMs;
  lastLoopStart = loopStart;
}

// 25 s sin uso apagan el AMOLED; 5 s mas tarde loop() entra en reposo.
void updateBrightness(uint32_t now) {
  if (pet.awaitingStarter() || clockOpen || powerMenuOpen || pairingEventActive ||
      communicationState() != COMM_OFF) {
    lastInteract = now;
    screenOff = false;
    screenOffAt = 0;
  }
  // los eventos visibles despiertan la pantalla solos
  if (pet.evolving() || pet.ceremony || pet.eating() || pet.showHeart()) {
    lastInteract = now;
  }
  uint32_t idle = (int32_t)(now - lastInteract) < 0 ? 0 : now - lastInteract;
  dimStage = 0;
  if (!screenOff && idle >= AUTO_SCREEN_OFF_MS && autoSleepSceneSafe()) {
    screenOff = true;
    screenOffAt = now;
  }
  if (screenOff && activeWalkSession()) enterWalkScreenRest();
  // Sleeping changes the scene and the pet's behaviour, but it must not make
  // the AMOLED look switched off.  Only the normal inactivity timer may set
  // the panel brightness to zero.
  uint8_t target = usbPresent() ? 180 : 145;
  if (screenOff) target = 0;
  static uint8_t current = 255;
  if (target != current) {
    current = target;
    panel->setBrightness(target);
  }
}

// ---------- consola serie (mantenimiento de LittleFS + depuracion) ----------

void handleSerial() {
  if (!Serial.available()) return;
  String line = Serial.readStringUntil('\n');
  line.trim();
  if (line.length() == 0) return;
  if (sdSerialCommand(line)) return;

  if (line == "PERF") {
    Serial.printf("screen=%s render=%lums max=%lums count=%lu skip=%lu loop=%lums loopMax=%lums interval=%u dirty ui=%d card=%d clock=%d help=%d kb=%d menu=%d battle=%d gallery=%d\n",
                  screenName(),
                  (unsigned long)perfRenderLastMs, (unsigned long)perfRenderMaxMs,
                  (unsigned long)perfRenderCount, (unsigned long)perfRenderSkipCount,
                  (unsigned long)perfLoopLastMs, (unsigned long)perfLoopMaxMs,
                  renderIntervalMs(), uiDirty, cardDirty, clockDirty, helpDirty,
                  keyboardDirty, gameMenuDirty, battleDirty, galleryDirty);
    perfRenderMaxMs = 0;
    perfLoopMaxMs = 0;
    Serial.println("DONE");
  } else if (line == "HATCH") {
    pet.eggTap(); pet.eggTap(); pet.eggTap();
    Serial.println("DONE");
  } else if (line.startsWith("SPEC ")) {
    int n = line.substring(5).toInt();
    if (n >= 1 && n <= DEX_COUNT) {
      pet.prevSpeciesId = pet.speciesId;
      pet.speciesId = n;
      Serial.printf("especie #%d %s\n", n, DEX_TBL[n].name);
    }
    Serial.println("DONE");
  } else if (line.startsWith("LVL ")) {
    long value = line.substring(4).toInt();
    if (value >= 0 && value <= 255)
      pet.dbgSetAgeMinutes((uint32_t)value * MINUTES_PER_LEVEL);
    Serial.printf("age=%lu days=%lu level=%u\n", (unsigned long)pet.ageMinutes,
                  (unsigned long)(pet.ageMinutes / 1440UL), pet.level());
    Serial.println("DONE");
  } else if (line.startsWith("AGEDAYS ")) {
    long days = line.substring(8).toInt();
    if (days >= 0 && days <= 365) {
      pet.dbgSetAgeMinutes((uint32_t)days * 1440UL);
      Serial.printf("age=%lu days=%ld level=%u saved=1\n",
                    (unsigned long)pet.ageMinutes, days, pet.level());
    } else {
      Serial.println("ERROR: AGEDAYS range is 0..365");
    }
    Serial.println("DONE");
  } else if (line.startsWith("TIME ")) {
    uint32_t e = (uint32_t)line.substring(5).toInt();
    rtcSetEpoch(e);
    pet.setClock(e);
    if (clockOpen && pet.initialClockPending()) {
      clockH = (e / 3600) % 24;
      clockM = (e / 60) % 60;
      clockDirty = true;
    }
    Serial.printf("rtc=%u\n", rtcEpoch());
    Serial.println("DONE");
  } else if (line.startsWith("RTCSET ")) {  // solo RTC (simular apagados en pruebas)
    rtcSetEpoch((uint32_t)line.substring(7).toInt());
    Serial.printf("rtc=%u\n", rtcEpoch());
    Serial.println("DONE");
  } else if (line == "TIME") {
    Serial.printf("rtc=%u\n", rtcEpoch());
    Serial.println("DONE");
  } else if (line == "GAL") {
    galleryOpen = !galleryOpen;
    galleryDetail = 0;
    galleryDirty = true;
    if (!galleryOpen) galleryPmd.unload();
    Serial.println("DONE");
  } else if (line == "EGGS") {
    // simula 20 tiradas de huevo (no cambia el estado del juego)
    for (int i = 0; i < 20; i++) {
      int16_t d = pet.pickEggSpecies();
      Serial.printf("%d:%s(r%u) ", d, DEX_TBL[d].name, DEX_TBL[d].rarity);
    }
    Serial.println();
    Serial.println("DONE");
  } else if (line == "SHINY") {  // alterna shiny del actual (pruebas)
    pet.shiny = !pet.shiny;
    Serial.printf("shiny=%d\n", pet.shiny);
    Serial.println("DONE");
  } else if (line.startsWith("NICK ")) {
    pet.rename(line.substring(5).c_str());
    Serial.printf("nick=%s\n", pet.nick);
    Serial.println("DONE");
  } else if (line == "CAREDAY") {  // simula un dia nuevo cuidado (pruebas)
    pet.setClock(pet.lastSeenEpoch + 86400);
    pet.caress();
    Serial.printf("streak=%u bond=%u medals=0x%X\n", pet.streak, pet.bond, pet.medals);
    Serial.println("DONE");
  } else if (line == "BYE") {
    pet.startFarewell();
    Serial.println("DONE");
  } else if (line == "RUN") {
    pet.startRunaway();
    Serial.println("DONE");
  } else if (line == "BEEP") {
    sfxPlay(SFX_HATCH);  // prueba de audio
    Serial.println("DONE");
  } else if (line == "SHAKE") {
    // Reproduce la secuencia completa sin tener que iniciar un combate.
    sfxPlay(SFX_CAPTURE_SHAKE);
    Serial.println("DONE");
  } else if (line == "ABANDON") {
    pet.dbgRunawayReady();  // fuerza el estado "lista para escaparse" (test del boton)
    Serial.println("DONE");
  } else if (line == "WIPE") {
    pet.factoryReset();     // borra NVS y reinicia -> partida nueva (eleccion de inicial)
    Serial.println("DONE");
    delay(100);
    ESP.restart();
  } else if (line == "REG") {
    Serial.printf("pokedex %u/151:", pet.registeredCount());
    for (int i = 1; i <= 151; i++)
      if (pet.isRegistered(i)) Serial.printf(" %d", i);
    Serial.println();
    Serial.println("DONE");
  } else if (line == "SAVEINFO") {
    // PC 플래셔가 NVS 원본을 읽기 전에 지연 저장분까지 확정한다.
    if (pet.savePending()) pet.flushSave();
    Serial.printf("fw=%s save=%s createdBoot=%d spec=%d level=%u egg=%d starter=%d age=%lu\n",
                  FW_VERSION, pet.saveLoadedFromNvs ? "loaded" : "created",
                  pet.saveCreatedThisBoot, pet.speciesId, pet.level(), pet.isEgg(),
                  pet.awaitingStarter(), (unsigned long)pet.ageMinutes);
    Serial.printf("battle wins=%u losses=%u streak=%u best=%u nick=%s\n",
                  pet.battleWins, pet.battleLosses, pet.battleStreak,
                  pet.bestBattleStreak, pet.nick);
    Serial.printf("records runner=%u snorlax=%u eevee=%u diglett=%u magikarp=%u\n",
                  pet.gameHi, pet.catchHi, pet.memoHi, pet.diglettHi, pet.typeHi);
    Serial.println("DONE");
  } else if (line == "HEALTH") {
    Serial.printf("up=%lus heap=%u min=%u sd=%d mon=%d\n",
                  (unsigned long)(millis() / 1000), ESP.getFreeHeap(),
                  ESP.getMinFreeHeap(), sdReady, pmd.loaded || mon.loaded);
    Serial.println("DONE");
  } else if (line == "STATS") {
    Serial.printf("spec=%d nv=%u com=%u fel=%u ene=%u lim=%u desc=%u sd=%d mon=%d bat=%d usb=%d rtc=%u\n",
                  pet.speciesId, pet.level(), pet.fullness, pet.joy, pet.energy,
                  pet.hygiene, pet.careMistakes, sdReady, mon.loaded,
                  batPercent(), usbPresent(), rtcEpoch());
    Serial.printf("peso=%u fue=%u def=%u vel=%u genes=%u/%u/%u tr=%u/%u/%u baya=%d\n",
                  pet.weight, pet.atkStat(), pet.defStat(), pet.speStat(),
                  pet.geneAtk, pet.geneDef, pet.geneSpe,
                  pet.trAtk, pet.trDef, pet.trSpe, pet.berryKnown);
    Serial.printf("shiny=%d streak=%u/%u bond=%u medals=0x%X(%u) nick=%s\n",
                  pet.shiny, pet.streak, pet.bestStreak, pet.bond, pet.medals,
                  pet.totalMedals, pet.nick);
    Serial.printf("care mode=%d need=%u missed=%d due=%lu shown=%d screen=%d rest=%d\n",
                  pet.tamagotchiModeEnabled(), (unsigned)pet.pendingCareNeed(),
                  pet.careCallMissed(), (unsigned long)pet.careCallDeadline(),
                  careAlertShownThisWake, screenOff,
                  screenOnlyRestStartedUs != 0);
    Serial.println("DONE");
  }
}

// ---------- entrada tactil ----------

bool inPetZone(int16_t x, int16_t y) {
  return x > 110 && x < 356 && y > 95 && y < 310;
}

// El sondeo fisico y la interpretacion del gesto estan separados para que el
// simulador de Windows use exactamente la misma entrada que el firmware.
void processTouchSample(bool pressed, int16_t x, int16_t y, uint32_t now);

void handleTouch() {
  static uint32_t lastPoll = 0;
  uint32_t now = millis();
  if (now - lastPoll < (powerSave && screenOff ? 80UL : 20UL)) return;  // 50 Hz activo; menos si pantalla apagada
  lastPoll = millis();
  // solo tocamos el bus si el chip aviso por INT o si el dedo sigue abajo (hay
  // que detectar el levantamiento). Leer el CST9217 dormido se colgaba ~1s y
  // congelaba el loop entero; SensorLib no respeta el timeout de Wire.
  if (!gTouchIrq && !wasPressed && digitalRead(TP_INT) != LOW) return;
  gTouchIrq = false;
  int16_t x, y;
  bool pressed = touch.getPoint(&x, &y, 1) > 0;
  processTouchSample(pressed, x, y, now);
}

// el toque se resuelve al LEVANTAR el dedo para distinguir tap de deslizar
void processTouchSample(bool pressed, int16_t x, int16_t y, uint32_t now) {

  if (pairingEventActive) {
    // Track the release so a finger held across the final frame cannot turn
    // into a delayed tap, but discard every gesture during the ceremony.
    wasPressed = pressed;
    swallowGesture = true;
    return;
  }

  if (now < ignoreTouchUntil) {
    // Wake-up suppression must still follow the finger until release.  If we
    // clear wasPressed here, a level-low INT can lose its next falling edge and
    // make the touch panel appear frozen.
    if (pressed) {
      if (!wasPressed) {
        tX0 = x;
        tY0 = y;
        tStart = now;
      }
      tXl = x;
      tYl = y;
      swallowGesture = true;
    }
    wasPressed = pressed;
    return;
  }

  // 마지막 단계는 버튼 안에서 3초 동안 계속 누르고 있어야 한다. 손을
  // 떼거나 버튼 밖으로 움직이면 처음부터 다시 세며 다른 제스처는 막는다.
  if (!powerMenuOpen && clockOpen && resetStage == RESET_HOLD_REQUIRED) {
    bool inside = x >= 108 && x <= 358 && y >= 166 && y <= 270;
    if (pressed && !wasPressed) {
      if (inside) {
        resetHolding = true;
        resetHoldStartedAt = now;
        clockDirty = true;
      } else if (x >= 178 && x <= 288 && y >= 362 && y <= 404) {
        resetHolding = false;
        resetHoldStartedAt = 0;
        resetStage = RESET_NONE;
        clockDirty = true;
        sfxPlay(SFX_TAP);
      }
    } else if (pressed && resetHolding) {
      if (!inside) {
        resetHolding = false;
        resetHoldStartedAt = 0;
      } else if (now - resetHoldStartedAt >= 3000UL) {
        wasPressed = pressed;
        performFactoryReset();
        return;
      }
      clockDirty = true;
    } else if (!pressed) {
      resetHolding = false;
      resetHoldStartedAt = 0;
      clockDirty = true;
    }
    wasPressed = pressed;
    return;
  }

  // Once the display is off, touch is ignored completely. Only PWR (or an
  // automatic Tamagotchi care alert) may wake the device.
  if (screenOff) {
    wasPressed = false;
    holdFired = false;
    swallowGesture = true;
    return;
  }

  if (pressed && !wasPressed) {  // empieza el gesto
    tX0 = tXl = x;
    tY0 = tYl = y;
    tStart = millis();
    holdFired = false;
    swallowGesture = (dimStage > 0) || screenOff;  // si estaba a oscuras, solo despierta
    screenOff = false;
    lastInteract = millis();
    // Reassert the active level after every physical touch.  On the original
    // 1.75 AMOLED this also recovers a panel whose brightness command was
    // latched at zero while the touch controller was being read.
    panel->setBrightness(usbPresent() ? 180 : 145);
    if (!swallowGesture && gameOpen && gameMode == 0) {
      gameTap(x, y);         // ball: cuenta al tocar, no al soltar
      swallowGesture = true; // evita doble tap al levantar el dedo
    } else if (!swallowGesture && gameOpen && gameMode == 1) {
      gameTap(x, y);         // 잠만보 게임은 새 터치 판정을 사용한다
      swallowGesture = true; // evita doble tap al levantar el dedo
    }
  } else if (pressed) {  // sigue apoyado
    tXl = x;
    tYl = y;
    // pulsacion larga sin moverse sobre el bicho -> dialogo de soltar
    if (!holdFired && !swallowGesture && !powerMenuOpen && !galleryOpen && !cardOpen && !kbOpen && !clockOpen && !helpOpen && !sleepMenuOpen &&
        !pet.sitterActive(pet.lastSeenEpoch) && millis() - tStart > 3000 &&
        abs(tXl - tX0) < 30 && abs(tYl - tY0) < 30 && inPetZone(tX0, tY0) &&
        !pet.isEgg() && !confirmUntil && !pet.ceremony) {
      confirmUntil = millis() + 10000;
      holdFired = true;
    }
  } else if (wasPressed) {  // levanta el dedo: resolver gesto
    lastInteract = millis();
    int dx = tXl - tX0, dy = tYl - tY0;
    uint32_t dt = millis() - tStart;
    if (!holdFired && !swallowGesture) {
      int ax = abs(dx), ay = abs(dy);
      if (ax > 60 && ax > ay + 8 && dt < 1200) onSwipe(dx > 0 ? 1 : -1);
      else if (ay > 60 && ay > ax + 8 && dt < 1200) onSwipeV(dy > 0 ? 1 : -1);
      else if (dt < 1500 && ax < 45 && ay < 45) onTap(tX0, tY0);
    }
  }
  wasPressed = pressed;
}

// deslizar vertical: abre/cierra la ficha del bicho
int16_t boxDexAt(uint16_t index);
uint8_t boxPageCount();
uint8_t currentDayPhase();
StrId dayPhaseTextId(uint8_t phase);
void startDiglettGame();
void startTypeGame();
void diglettTap(int16_t x, int16_t y);
void typeTap(int16_t x, int16_t y);
void communicationCardTap(int16_t x, int16_t y);
void itemsHubTap(int16_t x, int16_t y);
void renderHub();
void updateCommunication(uint32_t now);

void onSwipeV(int dir) {
  if (pairingEventActive) return;
  if (powerMenuOpen) return;
  if (helpOpen) { helpOpen = false; clockOpen = true; timePanel = TIME_PANEL_SETTINGS; clockDirty = true; lockTouchBrief(); sfxPlay(SFX_TAP); return; }
  if (pet.awaitingStarter()) return;  // bloqueado durante la eleccion de inicial
  if (wildPromptUntil && millis() < wildPromptUntil) return;
  if (wildPromptUntil) wildPromptUntil = 0;
  if (gameMenuOpen || sleepMenuOpen || pet.sitterActive(pet.lastSeenEpoch)) return;
  if (gameOpen || walkOpen || galleryOpen || kbOpen || battleOpen || pet.ceremony) return;
  if (hubOpen) return;
  if (clockOpen) {
    if (timeCorrectionMandatory()) return;
    if (timePanel != TIME_PANEL_SETTINGS) {
      wifiTimeStop();
      timePanel = TIME_PANEL_SETTINGS;
      clockDirty = true;
    } else {
      clockOpen = false;
      markUiDirty();
    }
    lockTouchBrief();
    sfxPlay(SFX_TAP);
    return;
  }
  if (cardOpen) {
    if (moveRelearnOpen) return;
    if (dir < 0) {
      cardOpen = false;
      expeditionTrainChoiceOpen = false;
      moveRelearnOpen = false;
      moveRelearnCandidate = MOVE_NONE;
      markUiDirty();
      lockTouchBrief();
      sfxPlay(SFX_TAP);
    }  // arriba cierra la ficha
    return;
  }
  if (dir > 0) {                    // deslizar abajo: ajustar hora
    if (!confirmUntil && !feedMenuUntil) openClock();
  } else if (!pet.isEgg() && !confirmUntil && !feedMenuUntil) {
    cardOpen = true;                // deslizar arriba: ficha
    cardPage = 0;
    moveRelearnOpen = false;
    moveRelearnCandidate = MOVE_NONE;
    cardDirty = true;
    lockTouchBrief();
    sfxPlay(SFX_MENU);
  }
}

// deslizar: dir +1 = hacia la derecha
void onSwipe(int dir) {
  if (pairingEventActive) return;
  if (powerMenuOpen) return;
  if (helpOpen) return;
  if (pet.awaitingStarter()) return;  // bloqueado durante la eleccion de inicial
  if (wildPromptUntil && millis() < wildPromptUntil) return;
  if (wildPromptUntil) wildPromptUntil = 0;
  if (gameMenuOpen || sleepMenuOpen || pet.sitterActive(pet.lastSeenEpoch)) return;
  if (gameOpen || walkOpen || kbOpen || clockOpen || battleOpen) return;
  if (cardOpen) {  // dentro de la ficha: cambiar paginas
    if (moveRelearnOpen) return;
    int p = (int)cardPage + (dir > 0 ? -1 : 1);  // izquierda avanza
    uint8_t old = cardPage;
    cardPage = p < 0 ? 0 : (p >= CARD_COUNT ? CARD_COUNT - 1 : p);
    if (cardPage != old) {
      expeditionTrainChoiceOpen = false;
      commSelectedMode = COMM_MODE_NONE;
      commDone = commFailed = false;
      cardDirty = true;
      sfxPlay(SFX_MENU);
    }
    return;
  }
  if (hubOpen) {
    int p = (int)hubPage + (dir > 0 ? -1 : 1);
    uint8_t old = hubPage;
    if (p < 0) {
      if (communicationState() != COMM_OFF) communicationStop();
      hubOpen = false;
      hubPage = 0;
      markUiDirty();
      sfxPlay(SFX_TAP);
      return;
    }
    if (p >= HUB_COUNT) p = HUB_COUNT - 1;
    hubPage = (uint8_t)p;
    if (hubPage != old) {
      if (old == 3 && communicationState() != COMM_OFF) communicationStop();
      expeditionTrainChoiceOpen = false;
      commSelectedMode = COMM_MODE_NONE;
      commDone = commFailed = false;
      hubDirty = true;
      sfxPlay(SFX_MENU);
    }
    return;
  }
  if (!galleryOpen) {
    if (!pet.ceremony && !confirmUntil) {
      if (dir > 0) {
        galleryOpen = true;
        galleryPage = 0;
        galleryDetail = 0;
        galleryDirty = true;
      } else {
        hubOpen = true;
        hubPage = 0;
        hubDirty = true;
      }
      lockTouchBrief();
      sfxPlay(SFX_MENU);
    }
    return;
  }
  if (galleryDetail) {  // en detalle: volver a la rejilla
    galleryDetail = 0;
    galleryShowShiny = false;
    galleryPmd.unload();
    galleryDirty = true;
    lockTouchBrief();
    return;
  }
  int np = galleryPage - dir;  // deslizar a la izquierda avanza pagina
  int maxPage = galleryPageCount() - 1;
  if (np < 0) {                // retroceder desde la primera = salir
    galleryOpen = false;
    galleryPmd.unload();
    markUiDirty();
    lockTouchBrief();
    sfxPlay(SFX_TAP);
    return;
  }
  if (np > maxPage) np = maxPage;
  if (np != galleryPage) {
    galleryPage = np;
    galleryDirty = true;
    sfxPlay(SFX_MENU);
  }
}

struct GameMenuTile {
  int16_t x, y, w, h;
  uint8_t game;
};

static const GameMenuTile GAME_MENU_TILES[6] = {
  { 88, 156, 138, 58, 0 },
  { 240, 156, 138, 58, 1 },
  { 88, 226, 138, 58, 2 },
  { 240, 226, 138, 58, 3 },
  { 88, 296, 138, 62, 4 },
  { 240, 296, 138, 62, 5 },
};

int8_t gameMenuHit(int16_t x, int16_t y) {
  for (uint8_t i = 0; i < 6; i++) {
    const GameMenuTile &t = GAME_MENU_TILES[i];
    if (x >= t.x && x <= t.x + t.w && y >= t.y && y <= t.y + t.h) return i;
  }
  return -1;
}

void startGameMenuChoice(uint8_t idx) {
  switch (GAME_MENU_TILES[idx].game) {
    case 0: startRunnerGame(); break;
    case 1: startSnorlaxGame(); break;
    case 2: startEeveeGame(); break;
    case 3: startDiglettGame(); break;
    case 4: startMagikarpGame(); break;
    case 5: gameMenuOpen = false; startWalk(); break;
  }
}

void onTap(int16_t x, int16_t y) {
  if (pairingEventActive) return;
  // Serial.printf("TOUCH %d %d\n", x, y);  // diagnostico (silenciado: satura el log)
  if (powerMenuOpen) {
    powerMenuTap(x, y);
    return;
  }
  if (clockOpen) {
    clockTap(x, y);
    return;
  }
  if (pet.awaitingStarter()) {  // primera partida: elegir inicial
    for (int i = 0; i < 3; i++) {
      int baseY = starterPikachuUnlocked ? STARTER_UNLOCKED_ROW_Y : STARTER_ROW_Y;
      int gap = starterPikachuUnlocked ? STARTER_UNLOCKED_ROW_GAP : STARTER_ROW_GAP;
      int ry = baseY + i * (STARTER_ROW_H + gap);
      if (x >= 70 && x <= 396 && y >= ry && y <= ry + STARTER_ROW_H) {
        pet.chooseStarter(STARTER_DEX[i]);
        markUiDirty();
        lockTouchBrief();
        sfxPlay(SFX_TAP);
        break;
      }
    }
    if (starterPikachuUnlocked && x >= STARTER_PIKA_X && x <= STARTER_PIKA_X + STARTER_PIKA_W &&
        y >= STARTER_PIKA_Y && y <= STARTER_PIKA_Y + STARTER_PIKA_H) {
      pet.chooseStarter(25);
      markUiDirty();
      lockTouchBrief();
      sfxPlay(SFX_TAP);
    }
    return;
  }
  if (friendInviteUntil && millis() < friendInviteUntil) return;
  if (walkOpen) {
    if (x >= 128 && x <= 338 && y >= 382 && y <= 430) {
      if (!walkFinished) finishWalk();
      else {
        walkOpen = false;
        walkFinished = false;
        walkReward = {};
        markUiDirty();
      }
      sfxPlay(SFX_TAP);
      lockTouchBrief();
    }
    return;
  }
  if (galleryOpen) {
    galleryTap(x, y);
    return;
  }
  if (kbOpen) {
    keyboardTap(x, y);
    return;
  }
  if (helpOpen) {
    helpTap(x, y);
    return;
  }
  if (pet.ceremony) return;  // durante la despedida no hay botones
  if (sleepMenuOpen) {
    bool sleepHit = x >= 94 && x <= 372 && y >= 170 && y <= 220;
    bool sitterHit = x >= 94 && x <= 372 && y >= 236 && y <= 286;
    if (sleepHit) {
      sleepMenuOpen = false;
      pet.toggleLight();
      sfxPlay(SFX_TAP);
    } else if (sitterHit) {
      uint32_t e = rtcEpoch();
      if (e) pet.lastSeenEpoch = e;
      if (pet.startSitter(e)) {
        sitterPmd.load(113, false);
        beh.x = beh.targetX = 190;
        sfxPlay(SFX_HEART);
      } else {
        if (e && (e % 86400UL) >= 19UL * 3600UL) {
          snprintf(statusNoticeMsg, sizeof(statusNoticeMsg), "%s", T(S_SITTER_UNAVAILABLE));
          statusNoticeUntil = millis() + 3500UL;
        }
        sfxPlay(SFX_DENY);
      }
      sleepMenuOpen = false;
    } else {
      sleepMenuOpen = false;
      sfxPlay(SFX_TAP);
    }
    markUiDirty();
    lockTouchBrief();
    return;
  }
  if (cardOpen) {
    if (cardPage == 3 && moveRelearnOpen) {
      if (y >= 338 && y <= 394 && x >= 70 && x <= 224) {
        if (pet.learnMove1(moveRelearnCandidate)) sfxPlay(SFX_LEVEL);
        else sfxPlay(SFX_DENY);
        moveRelearnOpen = false;
        moveRelearnCandidate = MOVE_NONE;
        cardDirty = true;
      } else if (y >= 338 && y <= 394 && x >= 242 && x <= 396) {
        if (pet.keepMove1()) sfxPlay(SFX_TAP);
        else sfxPlay(SFX_DENY);
        moveRelearnOpen = false;
        moveRelearnCandidate = MOVE_NONE;
        cardDirty = true;
      }
      lockTouchBrief();
      return;
    }
    if (cardPage == 0 && y < 96) openKeyboard();  // 이름을 눌러 변경
    else if (cardPage == 3 &&
             x >= 92 && x <= 374 && y >= 246 && y <= 304) {
      moveRelearnCandidate = pet.previewMove1((uint32_t)random(0x7fffffffL) ^ rtcEpoch());
      if (moveRelearnCandidate != MOVE_NONE) {
        moveRelearnOpen = true;
        sfxPlay(SFX_MENU);
      } else {
        sfxPlay(SFX_DENY);
      }
      cardDirty = true;
      lockTouchBrief();
    }
    else if (y >= 400) {
      cardOpen = false;
      markUiDirty();
      lockTouchBrief();
      sfxPlay(SFX_TAP);
    }
    return;
  }
  if (gameOpen) {
    gameTap(x, y);
    return;
  }
  if (battleOpen) {
    battleTap(x, y);
    return;
  }
  if (petEventUntil && millis() < petEventUntil && inPetEventHit(x, y)) {
    acceptPetEvent();
    return;
  }
  if (gameMenuOpen) {
    int8_t hit = gameMenuHit(x, y);
    if (hit >= 0) { lockTouchBrief(140); startGameMenuChoice((uint8_t)hit); }
    else { gameMenuOpen = false; markUiDirty(); lockTouchBrief(); sfxPlay(SFX_TAP); }
    return;
  }
  if (wildPromptUntil) {
    if (millis() < wildPromptUntil) {
      bool fight = (x >= 93 && x <= 373 && y >= 226 && y <= 270);
      bool later = (x >= 93 && x <= 373 && y >= 278 && y <= 322);
      if (fight) {
        startBattleWith(wildPromptDex, wildPromptLevel);
      } else if (later) {
        wildPromptUntil = 0;
        scheduleNextWild(millis());
        sfxPlay(SFX_TAP);
      }
    } else {
      wildPromptUntil = 0;
    }
    return;
  }
  if (hubOpen) {
    if (hubPage == 1) {
      itemsHubTap(x, y);
    } else if (hubPage == 2 && x >= 72 && x <= 394 && y >= 190 && y <= 268) {
      hubOpen = false;
      markUiDirty();
      lockTouchBrief();
      startBattle();
    } else if (hubPage == 3) {
      communicationCardTap(x, y);
    } else if (y >= 396) {
      hubOpen = false;
      markUiDirty();
      lockTouchBrief();
      sfxPlay(SFX_TAP);
    }
    return;
  }

  if (pet.sitterActive(pet.lastSeenEpoch)) {
    int dx = x - buttons[2].cx, dy = y - buttons[2].cy;
    if (dx * dx + dy * dy <= BTN_HIT * BTN_HIT) {
      pet.stopSitter();
      sitterPmd.unload();
      beh.x = beh.targetX = CX;
      sfxPlay(SFX_TAP);
      markUiDirty();
    }
    return;
  }
  if (choiceKind) {          // dialogo de decision: boton accion (arriba) / mantener (abajo)
    bool b1 = (x >= 93 && x <= 373 && y >= 206 && y <= 258);  // accion
    bool b2 = (x >= 93 && x <= 373 && y >= 268 && y <= 320);  // mantener / quedaros
    if (choiceKind == 1) {                 // evolucion
      if (b1) { int16_t old = pet.speciesId; pet.evolve(); evoPmd.load(old, pet.shiny); }
      else if (b2) pet.declineEvolve(rtcEpoch());
    } else if (choiceKind == 2) {          // despedida
      if (b1) pet.startFarewell();
      else if (b2) pet.declineFarewell(rtcEpoch());
    }
    choiceKind = 0;
    return;
  }
  if (confirmUntil) {        // dialogo "soltar?": SI / NO
    if (millis() < confirmUntil && x >= 118 && x <= 218 && y >= 252 && y <= 304) {
      pet.release();
    }
    confirmUntil = 0;
    return;
  }
  if (feedMenuUntil) {       // selector de comida
    if (millis() < feedMenuUntil && y >= 288 && y <= 352 && x >= 101 && x <= 365) {
      int item = (x - 101) / 66;
      if (item == 3) {
        // Candy remains usable at full fullness.
        pet.feedCandy();
        sfxPlay(SFX_EAT);
      } else if (pet.fullness >= 100) {
        snprintf(statusNoticeMsg, sizeof(statusNoticeMsg), "배가 불러...!");
        statusNoticeUntil = millis() + 2600UL;
        sfxPlay(SFX_DENY);
      } else {
        pet.feedBerry(item);
        sfxPlay(SFX_EAT);
      }
    }
    feedMenuUntil = 0;
    return;
  }
  if (pet.isEgg()) {
    pet.eggTap();
    sfxPlay(SFX_TAP);
    return;
  }
  // boton de evolucion: abre el dialogo evolucionar/mantener
  if (pet.wantEvolveButton() && x >= EVO_BTN_X && x <= EVO_BTN_X + EVO_BTN_W &&
      y >= EVO_BTN_Y && y <= EVO_BTN_Y + EVO_BTN_H) {
    choiceKind = 1; choiceUntil = millis() + 12000;
    return;
  }
  // botones de final (mismo recuadro): escapada directa; despedida abre dialogo
  if (x >= FAR_BTN_X && x <= FAR_BTN_X + FAR_BTN_W &&
      y >= FAR_BTN_Y && y <= FAR_BTN_Y + FAR_BTN_H) {
    if (pet.canRunawayNow()) { pet.startRunaway(); return; }
    if (pet.wantFarewellButton()) { choiceKind = 2; choiceUntil = millis() + 12000; return; }
  }
  for (int i = 0; i < 4; i++) {
    int dx = x - buttons[i].cx, dy = y - buttons[i].cy;
    if (dx * dx + dy * dy <= BTN_HIT * BTN_HIT) {
      Serial.printf("BTN %d\n", i);
      if (i == 0) {
        if (!pet.sleeping) {
          feedMenuUntil = millis() + 6000;
        }
      } else if (i == 1) {
        if (!pet.sleeping) {
          gameMenuOpen = true;
          gameMenuDirty = true;
          lockTouchBrief();
        }
      } else if (i == 2) {
        sfxPlay(SFX_TAP);
        if (pet.sleeping) pet.toggleLight();
        else sleepMenuOpen = true;
      } else {
        sfxPlay(SFX_TAP);
        startBath();
      }
      return;
    }
  }
  // tocar al bicho = caricia
  if (inPetZone(x, y)) {
    Serial.println("PET");
    uint8_t r = pet.interactPet(currentDayPhase() == 2);
    StrId msg = S_WAIT;
    if (r & PET_INTERACT_BOND) msg = S_BOND_GAIN;
    else if (r & PET_INTERACT_JOY) msg = S_HAPPY_FB;
    snprintf(petEventMsg, sizeof(petEventMsg), "%s", T(msg));
    petEventFeedbackUntil = millis() + 1600;
    if (!pet.sleeping) sfxPlay((r & PET_INTERACT_BOND) ? SFX_HEART : SFX_TAP);
    if (r != PET_INTERACT_NONE && audioMode() == SOUND_FULL) speciesChirpPlay(pet.speciesId);
  }
}

// ---------- render ----------

bool gNight = false;  // noche real (por hora) o durmiendo: lo fija render()
uint16_t inkColor() { return gNight ? UI_INK_NIGHT : UI_INK; }

// ---------- escena de fondo: bioma del tipo + hora real del RTC ----------

#define C565(r, g, b) ((uint16_t)((((r) >> 3) << 11) | (((g) >> 2) << 5) | ((b) >> 3)))
#define HORIZON 232  // linea donde el cielo se encuentra con el suelo

uint16_t lerp565(uint16_t a, uint16_t b, int i, int n) {
  if (n <= 0) return a;
  int ar = (a >> 11) & 31, ag = (a >> 5) & 63, ab = a & 31;
  int br = (b >> 11) & 31, bg = (b >> 5) & 63, bb = b & 31;
  return (uint16_t)((((ar + (br - ar) * i / n) << 11)) |
                    (((ag + (bg - ag) * i / n) << 5)) | (ab + (bb - ab) * i / n));
}

// hora del dia 0-23 (de la hora real cacheada cada 30s; 13 si no hay reloj)
int sceneHour() {
  uint32_t e = pet.lastSeenEpoch;
  return e ? (int)((e / 3600) % 24) : 13;
}

uint8_t currentDayPhase() {
  int h = sceneHour();
  if (h >= 6 && h < 12) return 0;
  if (h >= 12 && h < 18) return 1;
  if (h >= 18 && h < 22) return 2;
  return 3;
}

StrId dayPhaseTextId(uint8_t phase) {
  switch (phase) {
    case 0: return S_MORNING;
    case 2: return S_EVENING;
    case 3: return S_NIGHT;
    default: return S_DAY;
  }
}

// suelo de cada bioma de dia (de noche se mezcla hacia el azul nocturno)
static const uint16_t BIOME_SOIL[6] = {
  C565(0x7e, 0xc0, 0x7f),  // 0 pradera
  C565(0xdc, 0xca, 0x94),  // 1 playa (arena)
  C565(0x4f, 0x8a, 0x55),  // 2 bosque
  C565(0x8a, 0x55, 0x44),  // 3 volcan
  C565(0xa8, 0x90, 0x6a),  // 4 montana
  C565(0xe6, 0xee, 0xf5),  // 5 nieve
};

void drawClouds(uint32_t now, uint16_t col) {
  for (int k = 0; k < 2; k++) {
    int cx = (int)((now / 50 + k * 250) % 560) - 40;
    int cy = 70 + k * 34;
    gfx->fillCircle(cx, cy, 16, col);
    gfx->fillCircle(cx + 18, cy + 3, 13, col);
    gfx->fillCircle(cx - 15, cy + 4, 12, col);
  }
}

void drawScene(uint8_t biome, uint32_t now, bool night) {
  int h = sceneHour();
  uint16_t top, bot;
  if (night)            { top = C565(0x0c, 0x12, 0x24); bot = C565(0x1e, 0x26, 0x46); }
  else if (h < 8)       { top = C565(0xd1, 0x6a, 0x86); bot = C565(0xf3, 0xb8, 0x7c); }  // amanecer
  else if (h < 18)      { top = C565(0x8f, 0xc8, 0xea); bot = C565(0xdc, 0xee, 0xe6); }  // dia
  else                  { top = C565(0xc7, 0x5a, 0x4a); bot = C565(0xf0, 0xae, 0x64); }  // atardecer

  // cielo en bandas
  for (int y = 0; y < HORIZON; y += 8)
    gfx->fillRect(0, y, 466, 8, lerp565(top, bot, y, HORIZON));

  // sol o luna
  if (night) {
    gfx->fillCircle(360, 78, 24, C565(0xe8, 0xee, 0xf5));
    gfx->fillCircle(370, 72, 22, lerp565(top, bot, 78, HORIZON));  // creciente
    for (auto &st : STARS) gfx->fillRect(st[0], st[1], 4, 4, UI_WHITE);
  } else if (h < 18) {
    gfx->fillCircle(360, 84, 26, h < 8 ? C565(0xff, 0xd9, 0x8a) : C565(0xff, 0xe7, 0x9f));
    drawClouds(now, C565(0xff, 0xff, 0xff));
  } else {
    gfx->fillCircle(233, HORIZON - 6, 34, C565(0xff, 0xf1, 0xc8));  // sol poniente
  }

  // mar de la playa: una franja de agua sobre la arena
  uint16_t soil = BIOME_SOIL[biome < 6 ? biome : 0];
  if (night) soil = lerp565(soil, C565(0x16, 0x1c, 0x30), 9, 16);
  if (biome == 1) {
    uint16_t sea = night ? C565(0x1c, 0x34, 0x52) : C565(0x4f, 0x96, 0xc4);
    gfx->fillRect(0, HORIZON - 26, 466, 26, sea);
    for (int i = 0; i < 3; i++) {
      int wy = HORIZON - 22 + i * 7;
      uint16_t fc = night ? C565(0x3a, 0x58, 0x78) : C565(0xbf, 0xe6, 0xf5);
      gfx->fillRect(60 + ((now / 60 + i * 30) % 60), wy, 26, 2, fc);
      gfx->fillRect(300 - ((now / 60 + i * 20) % 60), wy, 26, 2, fc);
    }
  }

  // suelo
  gfx->fillRect(0, HORIZON, 466, 466 - HORIZON, soil);
  uint16_t hill = lerp565(soil, night ? C565(0x0c, 0x12, 0x24) : C565(0xff, 0xff, 0xff), 3, 16);
  gfx->fillRoundRect(-60, HORIZON - 14, 586, 60, 30, hill);

  // detalles del bioma
  uint16_t dk = lerp565(soil, C565(0x10, 0x18, 0x20), night ? 11 : 7, 16);
  if (biome == 2) {  // bosque: coniferas en silueta
    for (int tx : { 60, 150, 360, 416 }) {
      gfx->fillTriangle(tx, HORIZON - 46, tx - 16, HORIZON, tx + 16, HORIZON, dk);
      gfx->fillTriangle(tx, HORIZON - 60, tx - 12, HORIZON - 28, tx + 12, HORIZON - 28, dk);
    }
  } else if (biome == 3) {  // volcan: rocas y brasas
    gfx->fillTriangle(70, HORIZON, 40, HORIZON + 30, 100, HORIZON + 30, dk);
    gfx->fillTriangle(400, HORIZON + 4, 372, HORIZON + 30, 430, HORIZON + 30, dk);
    if (!night)
      for (int e = 0; e < 4; e++)
        gfx->fillRect(120 + e * 70, HORIZON + 8 + (e % 2) * 6, 4, 4, C565(0xff, 0x9b, 0x3a));
  } else if (biome == 4) {  // montana: cumbres al fondo
    gfx->fillTriangle(140, HORIZON - 50, 60, HORIZON, 220, HORIZON, dk);
    gfx->fillTriangle(330, HORIZON - 38, 250, HORIZON, 410, HORIZON, dk);
  } else if (biome == 5 && !night) {  // nieve: copos cayendo
    for (int f = 0; f < 10; f++) {
      int fx = (f * 53 + now / 40) % 466;
      int fy = (f * 90 + now / 18) % HORIZON;
      gfx->fillRect(fx, fy, 3, 3, UI_WHITE);
    }
  } else if (biome == 0) {  // pradera: matas de hierba
    for (int gx : { 80, 175, 300, 395 })
      for (int b = -1; b <= 1; b++)
        gfx->fillRect(gx + b * 5, HORIZON + 6, 2, 8 + (b == 0 ? 4 : 0), dk);
  }
}

// primera partida: elige inicial entre Bulbasaur / Charmander / Squirtle
void renderStarterSelect() {
  if (!starterDirty) { perfRenderSkipCount++; return; }
  starterDirty = false;
  gfx->fillScreen(UI_BG_DAY);
  const char *t = T(S_CHOOSE_STARTER);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(2);
  gfx->setCursor(CX - gfx->textWidth(t) / 2, 68);
  gfx->print(t);
  for (int i = 0; i < 3; i++) {
    int16_t d = STARTER_DEX[i];
    const DexEntry &de = DEX_TBL[d];
    int baseY = starterPikachuUnlocked ? STARTER_UNLOCKED_ROW_Y : STARTER_ROW_Y;
    int gap = starterPikachuUnlocked ? STARTER_UNLOCKED_ROW_GAP : STARTER_ROW_GAP;
    int ry = baseY + i * (STARTER_ROW_H + gap);
    gfx->fillRoundRect(70, ry, 326, STARTER_ROW_H, 14, lerp565(de.accent, UI_WHITE, 6, 8));
    gfx->drawRoundRect(70, ry, 326, STARTER_ROW_H, 14, de.accent);
    const uint8_t *th = thumbs.get(d);     // miniatura del inicial desde LittleFS
    if (th) drawThumb(th, 76, ry - 5, 3, false);
    gfx->setTextColor(UI_INK);
    gfx->setTextSize(3);
    gfx->setCursor(178, ry + 24);
    gfx->print(dexName(d));
  }
  if (starterPikachuUnlocked) {
    const int16_t d = 25;
    const DexEntry &de = DEX_TBL[d];
    gfx->fillRoundRect(STARTER_PIKA_X, STARTER_PIKA_Y, STARTER_PIKA_W, STARTER_PIKA_H,
                       14, lerp565(de.accent, UI_WHITE, 5, 8));
    gfx->drawRoundRect(STARTER_PIKA_X, STARTER_PIKA_Y, STARTER_PIKA_W, STARTER_PIKA_H,
                       14, de.accent);
    // La miniatura tiene menos detalle que el PMD. El primer frame PMD ocupa
    // 19x25 px reales: a escala entera x2 queda en 38x50 px, muy cerca del
    // tamano visual de los otros iniciales sin el deterioro del thumbnail x3.
    if (starterPikachuPmd.loaded) {
      drawPmdActM(starterPikachuPmd, PMD_IDLE, 113, STARTER_PIKA_Y + 63,
                  0, false, false, 2);
    } else {
      const uint8_t *th = thumbs.get(d);
      if (th) drawThumb(th, STARTER_PIKA_X + 6, STARTER_PIKA_Y - 5, 3, false);
    }
    gfx->setTextColor(UI_INK);
    gfx->setTextSize(3);
    gfx->setCursor(178, STARTER_PIKA_Y + 24);
    gfx->print(dexName(d));
  }
  gfx->flush();
}

void drawSleepMenu() {
  gfx->fillRoundRect(70, 150, 326, 158, 18, UI_WHITE);
  gfx->drawRoundRect(70, 150, 326, 158, 18, inkColor());

  gfx->fillRoundRect(94, 170, 278, 50, 12, UI_BAR_OK);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(3);
  const char *sleep = T(S_SLEEP_ACTION);
  gfx->setCursor(CX - gfx->textWidth(sleep) / 2, 185);
  gfx->print(sleep);

  bool available = pet.canStartSitter(pet.lastSeenEpoch);
  uint16_t sitterColor = available ? C565(0xf0, 0x9a, 0xc2) : UI_TRACK;
  gfx->fillRoundRect(94, 236, 278, 50, 12, sitterColor);
  gfx->setTextColor(available ? UI_INK : UI_WHITE);
  const char *sitter = T(S_BABYSITTER);
  gfx->setCursor(CX - gfx->textWidth(sitter) / 2, 251);
  gfx->print(sitter);
}

void drawSitterScene() {
  if (sitterPmd.loaded) {
    // Chansey deliberately stays almost still on the right: only the sprite's
    // gentle idle cycle and a two-pixel breathing bob are used.
    int bob = ((millis() / 900UL) & 1U) ? 0 : 2;
    drawPmdActRatioM(sitterPmd, PMD_IDLE, 350, PET_GROUND - bob,
                     millis(), true, 9, 2);
  }
}

void render() {
  if (wakeLoaderUntil) {
    if ((int32_t)(wakeLoaderUntil - millis()) > 0) return;
    wakeLoaderUntil = 0;
  }
  if (pairingEventActive) {
    renderPairingEvent();
    return;
  }
  if (staticScreenClean()) { perfRenderSkipCount++; return; }
  if (powerMenuOpen) {
    renderPowerMenu();
    return;
  }
  if (clockOpen) {
    renderClock();
    return;
  }
  if (pet.awaitingStarter()) {  // primera partida: elegir inicial (prioridad total)
    renderStarterSelect();
    return;
  }
  if (galleryOpen) {
    renderGallery();
    return;
  }
  if (gameOpen) {
    renderGame();
    return;
  }
  if (walkOpen) {
    renderWalk();
    return;
  }
  if (battleOpen) {
    renderBattle();
    return;
  }
  if (kbOpen) {
    renderKeyboard();
    return;
  }
  if (helpOpen) {
    renderHelp();
    return;
  }
  if (cardOpen) {
    renderCard();
    return;
  }
  if (hubOpen) {
    renderHub();
    return;
  }
  int h = sceneHour();
  gNight = pet.sleeping || h < 6 || h >= 20;
  // drawScene cubre los 466x466 completos: sin fillScreen(NEGRO) previo para
  // que un flush DMA solapado nunca capture negro a medias (anti-parpadeo)
  drawScene(pet.isEgg() ? 0 : DEX_TBL[pet.speciesId].biome, millis(), gNight);

  if (pet.ceremony) {
    const DexEntry &d = DEX_TBL[pet.speciesId];
    const char *msg = (pet.ceremony == CER_FAREWELL) ? T(S_FAREWELL)
                      : (pet.ceremony == CER_RUNAWAY) ? T(S_RUNAWAY)
                                                      : T(S_GOODBYE);
    drawHeader(dexName(pet.speciesId), d.accent, msg);
    drawCeremony();
    gfx->flush();
    return;
  }

  if (pet.isEgg()) {
    drawHeader(T(S_EGG_HDR), inkColor(), eggMsg());
    int s = 5, x = CX - 16 * s, y = PET_CY - 16 * s;
    drawMap(SPR_EGG, SPRITE_H, x, y, s, false);
    if (pet.eggCracks() >= 1)
      for (auto &c : CRACK1) gfx->fillRect(x + c[0] * s, y + c[1] * s, s, s, INK_K);
    if (pet.eggCracks() >= 2)
      for (auto &c : CRACK2) gfx->fillRect(x + c[0] * s, y + c[1] * s, s, s, INK_K);
    if (pet.eggRarity() >= R_RARO) {
      const char *rar = (pet.eggRarity() == R_LEGENDARIO) ? T(S_EGG_LEGEND) : T(S_EGG_RARE);
      gfx->setTextColor(pet.eggRarity() == R_LEGENDARIO ? UI_BAR_WARN : 0x4C98);
      gfx->setTextSize(2);
      gfx->setCursor(CX - gfx->textWidth(rar) / 2, 316);
      gfx->print(rar);
    }
    char reg[24];
    snprintf(reg, sizeof(reg), T(S_POKEDEX_FMT), pet.registeredCount());
    gfx->fillRect(0, 312, 466, 154, gNight ? UI_BG_NIGHT : UI_BG_DAY);
    gfx->setTextColor(inkColor());
    gfx->setTextSize(2);
    gfx->setCursor(CX - gfx->textWidth(reg) / 2, 348);
    gfx->print(reg);
  } else {
    const DexEntry &d = DEX_TBL[pet.speciesId];
    char name[28];
    const char *base = pet.nick[0] ? pet.nick : dexName(pet.speciesId);
    snprintf(name, sizeof(name), T(S_NAME_FMT), pet.shiny ? "*" : "", base, pet.level());
    drawHeader(name, gNight ? UI_INK_NIGHT : d.accent, statusMsg());
    drawStreakBadge();
    drawPet();
    if (pet.sitterActive(pet.lastSeenEpoch)) drawSitterScene();
    drawBath();
    drawPoops();
    // panel inferior: base limpia para barras y botones sobre el paisaje
    gfx->fillRect(0, 312, 466, 154, gNight ? UI_BG_NIGHT : UI_BG_DAY);
    drawBars();
    drawButtons();
    drawCelebration();
    drawPetEvent();
    if (gameMenuOpen) drawGameMenu();
    if (sleepMenuOpen) drawSleepMenu();
    if (pet.wantEvolveButton()) drawEvolveButton();        // CTA rojo: evolucionar
    else if (pet.canRunawayNow()) drawRunawayButton();     // CTA sombrio: escapada (abandono)
    else if (pet.wantFarewellButton()) drawFarewellButton();  // CTA dorado: despedida
  }

  if (pet.sleeping) {
    gfx->setTextColor(UI_INK_NIGHT);
    gfx->setTextSize(3);
    gfx->setCursor(320, 130);
    gfx->print("Zz");
  }

  if (wildPromptUntil) {
    if (millis() > wildPromptUntil) wildPromptUntil = 0;
    else drawWildPrompt();
  }

  if (friendInviteUntil && millis() < friendInviteUntil && friendInviteDex > 0) {
    gfx->fillRoundRect(72, 172, 322, 96, 18, UI_WHITE);
    gfx->drawRoundRect(72, 172, 322, 96, 18, UI_INK);
    char invite[64];
    snprintf(invite, sizeof(invite), "%s가 놀러왔다!", dexName(friendInviteDex));
    gfx->setTextColor(UI_INK);
    gfx->setTextSize(2);
    gfx->setCursor(CX - gfx->textWidth(invite) / 2, 208);
    gfx->print(invite);
  }

  // selector de comida
  if (feedMenuUntil) {
    if (millis() > feedMenuUntil) {
      feedMenuUntil = 0;
    } else {
      gfx->fillRoundRect(101, 288, 264, 64, 14, UI_WHITE);
      gfx->drawRoundRect(101, 288, 264, 64, 14, inkColor());
      drawMap(SPR_ICON_FOOD, 16, 110, 296, 3, false);
      drawMap(SPR_ICON_BERRY_B, 16, 176, 296, 3, false);
      drawMap(SPR_ICON_BERRY_G, 16, 242, 296, 3, false);
      drawMap(SPR_ICON_CANDY, 16, 308, 296, 3, false);
    }
  }

  // dialogo "soltar?" (pulsacion larga sobre el bicho)
  if (confirmUntil) {
    if (millis() > confirmUntil) {
      confirmUntil = 0;
    } else {
      gfx->fillRoundRect(94, 168, 278, 152, 16, UI_WHITE);
      gfx->drawRoundRect(94, 168, 278, 152, 16, UI_INK);
      char q[28];
      snprintf(q, sizeof(q), T(S_RELEASE_FMT), dexName(pet.speciesId));
      gfx->setTextColor(UI_INK);
      gfx->setTextSize(2);
      gfx->setCursor(CX - gfx->textWidth(q) / 2, 196);
      gfx->print(q);
      gfx->fillRoundRect(118, 252, 100, 52, 12, UI_BAR_OK);
      gfx->setTextColor(UI_INK);
      gfx->setCursor(118 + (100 - gfx->textWidth(T(S_YES))) / 2, 270);
      gfx->print(T(S_YES));
      gfx->fillRoundRect(248, 252, 100, 52, 12, UI_BAR_BAD);
      gfx->setCursor(248 + (100 - gfx->textWidth(T(S_NO))) / 2, 270);
      gfx->print(T(S_NO));
    }
  }

  // dialogo de decision (evolucionar/mantener, despedirse/quedaros)
  if (choiceKind) {
    if (millis() > choiceUntil) choiceKind = 0;
    else drawChoiceDialog();
  }

  gfx->flush();
}

// ---------- minijuego: toques con la pokeball ----------

void drawGameMenu() {
  gameMenuDirty = false;
  gfx->fillRoundRect(78, 112, 310, 266, 18, UI_WHITE);
  gfx->drawRoundRect(78, 112, 310, 266, 18, UI_INK);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(3);
  const char *title = "놀이";
  gfx->setCursor(CX - gfx->textWidth(title) / 2, 124);
  gfx->print(title);
  const char *labels[6] = { "달려 피카츄", "깨워 잠만보", "냠냠 이브이",
                            "뿅뿅 디그다", "뛰어 잉어킹", "산책" };
  uint16_t cols[6] = { UI_BAR_BAD, UI_BAR_WARN, 0x4C98, UI_BAR_OK, 0xF3B7, 0xA51F };
  for (int i = 0; i < 6; i++) {
    const GameMenuTile &t = GAME_MENU_TILES[i];
    gfx->fillRoundRect(t.x, t.y, t.w, t.h, 14, cols[i]);
    gfx->drawRoundRect(t.x, t.y, t.w, t.h, 14, UI_INK);
    gfx->setTextColor(UI_INK);
    gfx->setTextSize(2);
    const char *label = labels[i];
    gfx->setCursor(t.x + (t.w - gfx->textWidth(label)) / 2, t.y + (t.h - 16) / 2);
    gfx->print(label);
  }
}

void unloadActivitySprites() {
  miniPmdA.unload();
  miniPmdB.unload();
  miniPmdC.unload();
  miniPmdD.unload();
}

void beginActivityGame(uint8_t mode) {
  unloadActivitySprites();
  gameMenuOpen = false;
  gameOpen = true;
  gameMode = mode;
  gameOverUntil = 0;
  gameScore = 0;
  gameMisses = 0;
  gameNewHi = false;
  gameGain = 0;
  hitTime = 0;
  miniPauseUntil = 0;
  miniStartedAt = millis();
  miniUntil = miniStartedAt + (mode == 1 ? SNORLAX_GAME_MS : ACTIVITY_GAME_MS);
  miniLastUpdateAt = miniStartedAt;
  miniSpawnAt = miniStartedAt + 700UL;
  miniSpeedStage = 0;
  miniSpeedNoticeUntil = 0;
  minigameAudioBegin();
  minigameSfxPlay(SFX_GAME_START);
}

void startRunnerGame() {
  if (pet.isEgg() || pet.sleeping || pet.ceremony) return;
  beginActivityGame(0);
  miniPmdA.load(25, false);   // Pikachu
  miniPmdB.load(109, false);  // Koffing
  runnerLane = 1;
  runnerBackgroundOffset = 0;
  for (auto &obstacle : runnerObstacles) obstacle = {};
  minigameBgmStart();
}

void startSnorlaxGame() {
  if (pet.isEgg() || pet.sleeping || pet.ceremony) return;
  beginActivityGame(1);
  snorlaxHitAt = 0;
  snorlaxAwakeAt = 0;
  snorlaxCritical = false;
  snorlaxAwake = false;
  snorlaxTimerStarted = false;
  minigameBgmStart();
}

void startEeveeGame() {
  if (pet.isEgg() || pet.sleeping || pet.ceremony) return;
  beginActivityGame(2);
  miniPmdA.load(133, false);  // Eevee
  miniPmdB.load(23, false);   // Ekans: Arbok was too large for this game
  eeveeLane = 2;
  eeveeX = eeveeTargetX = EEVEE_LANE_X[eeveeLane];
  eeveeFacingLeft = false;
  for (auto &object : fallingObjects) object = {};
  minigameBgmStart();
}

void startMagikarpGame() {
  if (pet.isEgg() || pet.sleeping || pet.ceremony) return;
  beginActivityGame(4);
  miniPmdA.load(129, false);  // Magikarp
  miniPmdB.load(29, false);   // Nidoran female
  miniPmdC.load(32, false);   // Nidoran male
  miniPmdD.load(17, false);   // Pidgeotto
  magikarpY = 330;
  magikarpVY = 0;
  magikarpJumpScale = 1.0f;
  magikarpBackgroundOffset = 0;
  for (auto &obstacle : jumpObstacles) obstacle = {};
  minigameBgmStart();
}

void finishActivityGame() {
  if (gameOverUntil) return;
  if (gameMode == 0) {
    gameNewHi = gameScore > pet.gameHi;
    uint8_t old = pet.trSpe;
    pet.playResult(gameScore);
    gameGain = pet.trSpe - old;
  } else if (gameMode == 1) {
    gameNewHi = gameScore > pet.catchHi;
    gameGain = pet.applyCatchResult(gameScore);
  } else if (gameMode == 2) {
    gameNewHi = gameScore > pet.memoHi;
    pet.applyMemoResult(gameScore);
    gameGain = 0;  // 생활 수치 회복 게임: 전투 능력치 +N 표시는 하지 않는다.
  } else if (gameMode == 4) {
    gameNewHi = gameScore > pet.typeHi;
    pet.applyTypeResult(gameScore);
    gameGain = 0;
  }
  gameOverUntil = millis() + 4000UL;
  minigameSfxPlay(gameNewHi && gameScore ? SFX_MEDAL : SFX_LEVEL);
}

uint32_t activityDelta(uint32_t now) {
  uint32_t delta = now - miniLastUpdateAt;
  miniLastUpdateAt = now;
  return delta > 60UL ? 60UL : delta;
}

float updateTimedGameSpeed(uint32_t now) {
  uint32_t elapsed = now - miniStartedAt;
  uint8_t stage = elapsed >= 45000UL ? 2 : elapsed >= 25000UL ? 1 : 0;
  if (stage > miniSpeedStage) {
    miniSpeedStage = stage;
    miniSpeedNoticeUntil = now + 1200UL;
  }
  return stage == 2 ? 1.4f : stage == 1 ? 1.2f : 1.0f;
}

void drawSpeedUpNotice(uint32_t now) {
  if (now >= miniSpeedNoticeUntil) return;
  const char *notice = "속도 UP!";
  gfx->fillRoundRect(163, 112, 140, 38, 11, UI_BAR_WARN);
  gfx->drawRoundRect(163, 112, 140, 38, 11, UI_INK);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(2);
  gfx->setCursor(CX - gfx->textWidth(notice) / 2, 123);
  gfx->print(notice);
}

void drawActivityLives() {
  if (gameMode == 1) return;  // 잠만보는 라이프가 없는 연타 게임
  for (uint8_t i = 0; i < 3; i++) {
    if (i < 3 - gameMisses) gfx->fillCircle(205 + i * 28, 122, 7, UI_BAR_BAD);
    else gfx->drawCircle(205 + i * 28, 122, 7, UI_TRACK);
  }
}

void registerActivityCollision(uint32_t now, int16_t x, int16_t y) {
  miniPauseUntil = now + 1000UL;
  hitTime = now;
  hitX = x;
  hitY = y;
  if (gameMisses < 3) gameMisses++;
  minigameSfxPlay(SFX_DIGLETT_MISS);
  if (gameMisses >= 3) finishActivityGame();
}

void drawActivityHeader(const char *title, uint16_t record, uint32_t now) {
  bool night = sceneHour() < 6 || sceneHour() >= 20;
  uint16_t ink = night ? UI_INK_NIGHT : UI_INK;
  gfx->setTextColor(ink);
  gfx->setTextSize(3);
  gfx->setCursor(CX - gfx->textWidth(title) / 2, 22);
  gfx->print(title);
  char score[18], rec[18];
  snprintf(score, sizeof(score), T(S_SCORE_FMT), gameScore);
  snprintf(rec, sizeof(rec), T(S_REC_FMT), record);
  gfx->setTextSize(2);
  gfx->setCursor(58, 66);
  gfx->print(score);
  gfx->setCursor(300, 66);
  gfx->print(rec);
  int bw = 286;
  uint32_t left = miniUntil > now ? miniUntil - now : 0;
  uint32_t duration = gameMode == 1 ? SNORLAX_GAME_MS : ACTIVITY_GAME_MS;
  int fw = (int)((uint64_t)bw * left / duration);
  gfx->fillRoundRect(CX - bw / 2, 92, bw, 13, 5, UI_TRACK);
  if (fw > 2) gfx->fillRoundRect(CX - bw / 2, 92, fw, 13, 5, UI_BAR_OK);
}

void drawActivityResult(uint16_t record, const char *rewardText) {
  if (millis() > gameOverUntil) {
    minigameAudioEnd();
    gameOpen = false;
    unloadActivitySprites();
    markUiDirty();
    return;
  }
  drawGameScene();
  bool night = sceneHour() < 6 || sceneHour() >= 20;
  uint16_t ink = night ? UI_INK_NIGHT : UI_INK;
  char score[24], rec[24];
  snprintf(score, sizeof(score), T(S_SCORE_FMT), gameScore);
  snprintf(rec, sizeof(rec), T(S_RECORD_FMT), record);
  gfx->setTextColor(ink);
  gfx->setTextSize(4);
  gfx->setCursor(CX - gfx->textWidth(score) / 2, 142);
  gfx->print(score);
  gfx->setTextSize(2);
  gfx->setCursor(CX - gfx->textWidth(rec) / 2, 205);
  gfx->print(rec);
  if (gameNewHi && gameScore) {
    gfx->setTextColor(UI_BAR_WARN);
    gfx->setCursor(CX - gfx->textWidth(T(S_NEW_RECORD)) / 2, 239);
    gfx->print(T(S_NEW_RECORD));
  }
  gfx->setTextColor(ink);
  gfx->setCursor(CX - gfx->textWidth(rewardText) / 2, 278);
  gfx->print(rewardText);
  if (gameGain) {
    char gain[18];
    snprintf(gain, sizeof(gain), "+%u", gameGain);
    gfx->setTextSize(3);
    gfx->setCursor(CX - gfx->textWidth(gain) / 2, 316);
    gfx->print(gain);
  }
  gfx->flush();
}

void runnerTap(int16_t x, int16_t y) {
  if (gameOverUntil || y < 350) return;
  if (x < CX && runnerLane > 0) runnerLane--;
  else if (x >= CX && runnerLane < 2) runnerLane++;
}

void snorlaxTap(int16_t x, int16_t y) {
  if (gameOverUntil || snorlaxAwake || x < 54 || x > 412 || y < 104 || y > 430) return;
  uint32_t now = millis();
  if (now - snorlaxHitAt < 55UL) return;
  snorlaxCritical = random(100) < 10;
  uint8_t points = snorlaxCritical ? 3 : 1;
  gameScore = gameScore + points >= 100 ? 100 : gameScore + points;
  snorlaxHitAt = now;
  hitX = x;
  hitY = y;
  hitTime = now;
  snorlaxHitPlay(snorlaxCritical);
  if (gameScore >= 100) {
    snorlaxAwake = true;
    snorlaxAwakeAt = now;
    minigameSfxPlay(SFX_EVENT_SPARKLE);
  }
}

void eeveeTap(int16_t x, int16_t y) {
  if (gameOverUntil || y < 348) return;
  eeveeFacingLeft = x < CX;
  if (eeveeFacingLeft && eeveeLane > 0) eeveeLane--;
  else if (!eeveeFacingLeft && eeveeLane < 4) eeveeLane++;
  eeveeTargetX = EEVEE_LANE_X[eeveeLane];
}

void magikarpTap(int16_t, int16_t y) {
  if (gameOverUntil || y < 350 || magikarpY < 329.0f) return;
  // A lower, longer arc gives the scrolling obstacles enough time to pass.
  // 50% of the previous jump height and 80% of its airtime.
  magikarpJumpScale = updateTimedGameSpeed(millis());
  magikarpVY = -181.0f * magikarpJumpScale;
  minigameSpeciesChirpPlay(129);
}

void drawSnorlaxLayer(uint8_t layer, int16_t x, int16_t y, uint8_t scale) {
  static const char *paths[] = {
    "/extra/S01.tvr", "/extra/S02.tvr", "/extra/S03.tvr", "/extra/S04.tvr"
  };
  if (layer < 4) drawVisualAsset(gfx, paths[layer], x, y, scale);
}

void renderRunnerGame() {
  uint32_t now = millis();
  if (gameOverUntil) { drawActivityResult(pet.gameHi, "속도와 기분이 올랐어!"); return; }
  if (now >= miniUntil) { finishActivityGame(); return; }
  uint32_t dt = activityDelta(now);
  float timedSpeed = updateTimedGameSpeed(now);
  // 이전 기본값(약 179 px/s)보다 약 20% 빠른 214 px/s로 시작한다.
  // 장애물과 배경 표식이 같은 기준 속도를 사용해야 가속이 눈에 보인다.
  float speed = (138.0f + gameScore * 7.0f) * 1.55f;
  if (speed > 460.0f) speed = 460.0f;
  speed *= timedSpeed;
  if (now >= miniPauseUntil) {
    for (auto &obstacle : runnerObstacles) {
      if (!obstacle.active) continue;
      obstacle.x += speed * dt / 1000.0f;
      if (obstacle.lane == runnerLane && obstacle.x > 318 && obstacle.x < 410) {
        obstacle.active = false;
        registerActivityCollision(now, 365, 160 + runnerLane * 86);
        return;  // 같은 프레임의 다른 장애물로 라이프가 중복 차감되지 않게 한다.
      } else if (obstacle.x > 438) {
        obstacle.active = false;
        gameScore++;
        minigameSfxPlay(SFX_MINIGAME_OK);
      }
    }
    if (now >= miniSpawnAt) {
      for (auto &obstacle : runnerObstacles) if (!obstacle.active) {
        obstacle.active = true;
        obstacle.x = 28;
        obstacle.lane = (uint8_t)random(3);
        uint32_t gap = 1250UL + random(500);
        uint32_t cut = gameScore * 28UL;
        uint32_t plannedGap = gap > cut ? gap - cut : 0;
        // Convert a real 220-pixel safety distance into time at the current
        // speed.  Lanes stay random without allowing visually overlapping or
        // inescapable Koffing patterns at either slow or fast game speeds.
        uint32_t safeGap = (uint32_t)(220000.0f / speed);
        if (safeGap < 1000UL) safeGap = 1000UL;
        miniSpawnAt = now + (plannedGap > safeGap ? plannedGap : safeGap);
        break;
      }
    }
  }
  gfx->fillScreen(C565(0x9e, 0xd8, 0xef));
  // A clean field replaces the high-contrast diagonal hatch.  Small ground
  // marks travel left-to-right so the requested direction remains visible.
  gfx->fillRect(0, 108, 466, 358, C565(0xa9, 0xd6, 0x72));
  gfx->drawLine(0, 108, 465, 108, C565(0x55, 0x91, 0x45));
  runnerBackgroundOffset += speed * dt * 0.85f / 1000.0f;
  while (runnerBackgroundOffset >= 96.0f) runnerBackgroundOffset -= 96.0f;
  int scroll = (int)runnerBackgroundOffset;
  for (int x = -70 + scroll; x < 450; x += 96) {
    gfx->fillRoundRect(x, 226, 42, 5, 2, C565(0x6f, 0xa9, 0x4e));
    gfx->fillRoundRect(x + 32, 312, 54, 5, 2, C565(0x6f, 0xa9, 0x4e));
  }
  for (uint8_t lane = 0; lane < 3; lane++)
    gfx->drawLine(42, 190 + lane * 86, 424, 190 + lane * 86, C565(0xe9, 0xf5, 0xd8));
  drawActivityHeader("달려 피카츄", pet.gameHi, now);
  for (auto &obstacle : runnerObstacles) if (obstacle.active && miniPmdB.loaded)
    drawPmdActM(miniPmdB, miniPmdB.has(PMD_WALKR) ? PMD_WALKR : PMD_IDLE,
                (int)obstacle.x, 188 + obstacle.lane * 86, now, true, false, 2);
  uint8_t act = now < miniPauseUntil && miniPmdA.has(PMD_HURT) ? PMD_HURT :
                miniPmdA.has(PMD_WALKL) ? PMD_WALKL :
                miniPmdA.has(PMD_ATTACK_L) ? PMD_ATTACK_L : PMD_IDLE;
  if (miniPmdA.loaded)
    drawPmdActRatioM(miniPmdA, act, 365, 188 + runnerLane * 86,
                     now, true, 5, 2);
  gfx->fillRoundRect(78, 380, 140, 48, 12, UI_WHITE);
  gfx->fillRoundRect(248, 380, 140, 48, 12, UI_WHITE);
  gfx->setTextColor(UI_INK); gfx->setTextSize(3);
  gfx->setCursor(133, 392); gfx->print("▲");
  gfx->setCursor(303, 392); gfx->print("▼");
  drawActivityLives();
  drawSpeedUpNotice(now);
  gfx->flush();
}

void renderSnorlaxGame() {
  uint32_t now = millis();
  if (gameOverUntil) { drawActivityResult(pet.catchHi, "방어와 체력이 올랐어!"); return; }
  if (snorlaxAwake) {
    drawGameScene();
    drawActivityHeader("깨워 잠만보", pet.catchHi, now);
    drawSnorlaxLayer(SNORLAX_AWAKE, 59, 100, 3);
    if (now - snorlaxAwakeAt >= 1200UL) finishActivityGame();
    gfx->flush();
    return;
  }
  if (snorlaxTimerStarted && now >= miniUntil) { finishActivityGame(); return; }
  drawGameScene();
  drawActivityHeader("깨워 잠만보", pet.catchHi, now);
  gfx->setTextColor(UI_INK); gfx->setTextSize(2);
  const char *guide = "잠만보를 두드려 깨우자!";
  gfx->setCursor(CX - gfx->textWidth(guide) / 2, 112); gfx->print(guide);
  int shake = 0;
  uint32_t hitAge = now - snorlaxHitAt;
  if (snorlaxHitAt && hitAge < 150UL) shake = ((hitAge / 24UL) & 1U) ? (snorlaxCritical ? 8 : 4) : (snorlaxCritical ? -8 : -4);
  drawSnorlaxLayer(SNORLAX_HEAD_ARMS, 59, 100, 3);
  drawSnorlaxLayer(SNORLAX_BODY, 59 + shake, 100, 3);
  drawSnorlaxLayer(SNORLAX_FEET, 59, 100, 3);
  uint32_t impactDuration = snorlaxCritical ? 360UL : 260UL;
  if (snorlaxHitAt && hitAge < impactDuration) {
    int radius = snorlaxCritical ? 28 + hitAge / 4 : 24 + hitAge / 5;
    uint16_t col = snorlaxCritical ? UI_BAR_WARN : UI_BAR_OK;
    uint8_t thickness = snorlaxCritical ? 10 : 5;
    for (uint8_t i = 0; i < thickness; i++)
      gfx->drawCircle((int)hitX, (int)hitY, radius + i, col);
  }
  gfx->flush();
  // Start the 30 seconds only after the expensive first frame is actually on
  // the AMOLED.  This also makes the first touch consistently count.
  if (!snorlaxTimerStarted) {
    snorlaxTimerStarted = true;
    uint32_t ready = millis();
    miniUntil = ready + SNORLAX_GAME_MS;
    miniLastUpdateAt = ready;
  }
}

void renderEeveeGame() {
  uint32_t now = millis();
  if (gameOverUntil) { drawActivityResult(pet.memoHi, "기분과 배부름이 올랐어!"); return; }
  if (now >= miniUntil) { finishActivityGame(); return; }
  uint32_t dt = activityDelta(now);
  float timedSpeed = updateTimedGameSpeed(now);
  if (now >= miniPauseUntil) {
    float dx = eeveeTargetX - eeveeX;
    float step = 420.0f * timedSpeed * dt / 1000.0f;
    if (fabsf(dx) <= step) eeveeX = eeveeTargetX;
    else eeveeX += dx < 0 ? -step : step;

    float speed = 150.0f + gameScore * 6.0f;
    if (speed > 276.0f) speed = 276.0f;
    speed *= timedSpeed;
    for (auto &object : fallingObjects) {
      if (!object.active) continue;
      object.y += speed * dt / 1000.0f;
      float hitRadius = object.kind == 3 ? 48.0f : 52.0f;
      if (object.y > 300 && object.y < 382 && fabsf(object.x - eeveeX) < hitRadius) {
        object.active = false;
        if (object.kind == 3) {
          registerActivityCollision(now, (int16_t)eeveeX, 344);
          return;
        } else {
          gameScore++;
          minigameSfxPlay(SFX_EEVEE_FRUIT);
        }
      } else if (object.y > 410) object.active = false;
    }
    uint8_t fallingObjectCount = 0;
    for (auto &object : fallingObjects) if (object.active) fallingObjectCount++;
    if (fallingObjectCount < 2 && now >= miniSpawnAt)
      for (auto &object : fallingObjects) if (!object.active) {
        object.active = true;
        object.x = EEVEE_LANE_X[random(5)];
        object.y = 112;
        object.kind = random(100) < 24 ? 3 : (uint8_t)random(3);
        uint32_t cut = gameScore * 24UL;
        miniSpawnAt = now + (900UL > cut + 420UL ? 900UL - cut : 420UL);
        break;
      }
  }
  drawGameScene();
  // Keep falling fruit readable against a quiet, field-like background.
  gfx->fillRect(26, 108, 414, 112, C565(0xa9, 0xdc, 0xf2));
  gfx->fillRect(26, 220, 414, 150, C565(0xb8, 0xdc, 0x83));
  gfx->drawLine(26, 220, 440, 220, C565(0x62, 0x9b, 0x4c));
  gfx->fillRoundRect(54, 322, 358, 7, 3, C565(0x82, 0xb4, 0x58));
  drawActivityHeader("냠냠 이브이", pet.memoHi, now);
  for (auto &object : fallingObjects) if (object.active) {
    if (object.kind == 3 && miniPmdB.loaded)
      drawPmdActM(miniPmdB, PMD_IDLE, (int)object.x, (int)object.y + 54,
                  now, true, false, 2);
    else {
      const char *const *icon = object.kind == 0 ? SPR_ICON_FOOD : object.kind == 1 ? SPR_ICON_BERRY_B : SPR_ICON_BERRY_G;
      drawMap(icon, 16, (int)object.x - 24, (int)object.y - 24, 3, false);
    }
  }
  bool moving = fabsf(eeveeTargetX - eeveeX) > 1.0f;
  uint8_t act = PMD_IDLE;
  if (now < miniPauseUntil && miniPmdA.has(PMD_HURT)) act = PMD_HURT;
  else if (moving && eeveeFacingLeft && miniPmdA.has(PMD_WALKL)) act = PMD_WALKL;
  else if (moving && !eeveeFacingLeft && miniPmdA.has(PMD_WALKR)) act = PMD_WALKR;
  if (miniPmdA.loaded) drawPmdActM(miniPmdA, act, (int)eeveeX, 354, now, true, false, 3);
  gfx->fillRoundRect(78, 386, 140, 44, 12, UI_WHITE);
  gfx->fillRoundRect(248, 386, 140, 44, 12, UI_WHITE);
  gfx->setTextColor(UI_INK); gfx->setTextSize(3);
  gfx->setCursor(133, 395); gfx->print("◀");
  gfx->setCursor(303, 395); gfx->print("▶");
  drawActivityLives();
  drawSpeedUpNotice(now);
  gfx->flush();
}

PmdMon &jumpObstaclePmd(uint8_t kind) {
  if (kind == 0) return miniPmdB;
  if (kind == 1) return miniPmdC;
  return miniPmdD;
}

void renderMagikarpGame() {
  uint32_t now = millis();
  if (gameOverUntil) { drawActivityResult(pet.typeHi, "기분과 체력이 올랐어!"); return; }
  if (now >= miniUntil) { finishActivityGame(); return; }
  uint32_t dt = activityDelta(now);
  float timedSpeed = updateTimedGameSpeed(now);
  float motionSpeed = magikarpY < 329.0f ? magikarpJumpScale : timedSpeed;
  if (now >= miniPauseUntil) {
    magikarpVY += 360.0f * motionSpeed * motionSpeed * dt / 1000.0f;
    magikarpY += magikarpVY * dt / 1000.0f;
    if (magikarpY > 330) {
      magikarpY = 330;
      magikarpVY = 0;
      magikarpJumpScale = timedSpeed;
      motionSpeed = timedSpeed;
    }
    float speed = 165.0f + gameScore * 7.0f;
    if (speed > 300.0f) speed = 300.0f;
    for (auto &obstacle : jumpObstacles) {
      if (!obstacle.active) continue;
      float obstacleSpeed = (obstacle.kind == 2 ? speed : speed * 1.3f) * motionSpeed;
      obstacle.x -= obstacleSpeed * dt / 1000.0f;
      if (obstacle.x > 72 && obstacle.x < 150) {
        bool collision = obstacle.kind == 2 ? magikarpY < 305.0f : magikarpY > 310.0f;
        if (collision) {
          obstacle.active = false;
          miniSpawnAt = now + 750UL;
          registerActivityCollision(now, 110, (int)magikarpY - 35);
          return;
        }
      }
      if (obstacle.active && obstacle.x < 28) {
        obstacle.active = false;
        miniSpawnAt = now + 550UL;
        gameScore++;
        minigameSfxPlay(SFX_MINIGAME_OK);
      }
    }
    bool hasJumpObstacle = false;
    for (auto &obstacle : jumpObstacles) if (obstacle.active) hasJumpObstacle = true;
    if (!hasJumpObstacle && now >= miniSpawnAt) for (auto &obstacle : jumpObstacles) if (!obstacle.active) {
      obstacle.active = true;
      obstacle.x = 438;
      // Flying Pidgeotto is a rare variation.  Only one obstacle is active at
      // once, preventing unavoidable Nidoran pairs or air/ground traps.
      obstacle.kind = random(100) < 8 ? 2 : (uint8_t)random(2);
      miniSpawnAt = now + 600UL;
      break;
    }
  }
  drawGameScene();
  magikarpBackgroundOffset += motionSpeed * dt / 10.0f;
  while (magikarpBackgroundOffset >= 72.0f) magikarpBackgroundOffset -= 72.0f;
  int scroll = (int)magikarpBackgroundOffset;
  gfx->drawLine(30, 338, 436, 338, UI_INK);
  for (int x = 40 - scroll; x < 450; x += 72) gfx->fillRect(x, 344, 34, 5, C565(0x8d, 0x66, 0x3f));
  drawActivityHeader("뛰어 잉어킹", pet.typeHi, now);
  for (auto &obstacle : jumpObstacles) if (obstacle.active) {
    PmdMon &actor = jumpObstaclePmd(obstacle.kind);
    int ground = obstacle.kind == 2 ? 235 : 338;
    uint8_t act = obstacle.kind == 2 && actor.has(PMD_ATTACK_L) ? PMD_ATTACK_L : actor.has(PMD_WALKL) ? PMD_WALKL : PMD_IDLE;
    if (actor.loaded) drawPmdActM(actor, act, (int)obstacle.x, ground, now, true, false, 2);
  }
  uint8_t act = magikarpY < 329 && miniPmdA.has(PMD_HOP) ? PMD_HOP : PMD_IDLE;
  if (now < miniPauseUntil && miniPmdA.has(PMD_HURT)) act = PMD_HURT;
  if (miniPmdA.loaded) drawPmdActM(miniPmdA, act, 110, (int)magikarpY, now, true, false, 3);
  gfx->fillRoundRect(148, 382, 170, 50, 13, UI_WHITE);
  gfx->drawRoundRect(148, 382, 170, 50, 13, UI_INK);
  gfx->setTextColor(UI_INK); gfx->setTextSize(2);
  const char *jump = "점프";
  gfx->setCursor(CX - gfx->textWidth(jump) / 2, 398); gfx->print(jump);
  drawActivityLives();
  drawSpeedUpNotice(now);
  gfx->flush();
}

void startGame() {
  if (pet.isEgg() || pet.sleeping || pet.ceremony) return;
  gameMenuOpen = false;
  gameOpen = true;
  gameMode = 0;
  gameOverUntil = 0;
  gameScore = 0;
  gameMisses = 0;
  gameNewHi = false;
  gameGain = 0;
  hitTime = 0;
  ballLastHitAt = 0;
  gamePetX = 233;
  respawnBall();
  minigameAudioBegin();
  minigameSfxPlay(SFX_GAME_START);
}

void spawnCatchTarget() {
  catchX = 86 + random(294);
  catchY = 118 + random(206);
  catchIcon = random(3);
  uint32_t life = 980;
  uint32_t speedup = (uint32_t)gameScore * 35;
  if (speedup > 530) speedup = 530;
  life -= speedup;
  catchTargetUntil = millis() + life;
}

void startCatchGame() {
  if (pet.isEgg() || pet.sleeping || pet.ceremony) return;
  gameMenuOpen = false;
  gameOpen = true;
  gameMode = 1;
  gameOverUntil = 0;
  gameScore = 0;
  gameMisses = 0;
  gameNewHi = false;
  gameGain = 0;
  catchUntil = millis() + 20000;
  spawnCatchTarget();
  minigameAudioBegin();
  minigameSfxPlay(SFX_GAME_START);
}

void startMemoRound() {
  if (memoLen < 14) memoSeq[memoLen++] = random(4);
  memoShow = 0;
  memoInput = 0;
  memoActivePad = -1;
  memoHintPad = -1;
  memoShowing = true;
  memoNextAt = millis() + 350;
  memoTurnUntil = 0;
}

void startMemoGame() {
  if (pet.isEgg() || pet.sleeping || pet.ceremony) return;
  gameMenuOpen = false;
  gameOpen = true;
  gameMode = 2;
  gameOverUntil = 0;
  gameScore = 0;
  gameNewHi = false;
  gameGain = 0;
  memoLen = 0;
  memoRounds = 0;
  memoFlashPad = -1;
  memoHintPad = -1;
  memoFlashUntil = memoFailUntil = memoTurnUntil = 0;
  startMemoRound();
  minigameAudioBegin();
  minigameSfxPlay(SFX_GAME_START);
}

bool anyDiglettActive(uint32_t now) {
  for (uint8_t i = 0; i < DIGLETT_MAX_ACTIVE; i++) {
    if (diglettCells[i] >= 0 && now < diglettHideAt[i]) return true;
  }
  return false;
}

// Frame 1 is the empty hole. Frames 2-4 reveal Diglett progressively. The
// same sequence is reversed near the hide deadline, so the character appears
// to emerge from and retreat into the actual hole drawn by the user.
uint8_t diglettFrameAt(uint32_t now, uint32_t shownAt, uint32_t hideAt) {
  if (now < shownAt || now >= hideAt) return 0;
  const uint32_t age = now - shownAt;
  const uint32_t remaining = hideAt - now;
  constexpr uint32_t STEP_MS = 70UL;
  if (age < STEP_MS || remaining <= STEP_MS) return 0;
  if (age < STEP_MS * 2 || remaining <= STEP_MS * 2) return 1;
  if (age < STEP_MS * 3 || remaining <= STEP_MS * 3) return 2;
  return 3;
}

// Twelve-hole layout: the four corners of the old 4 x 4 board are removed.
// Wider 102 px horizontal and 67 px vertical spacing gives every hole a large
// 99 x 65 touch rectangle while leaving a small non-overlapping gap.
static const int16_t DIGLETT_CELL_X[4] = {80, 182, 284, 386};
static const int16_t DIGLETT_CELL_Y[4] = {143, 210, 277, 344};

bool diglettCellEnabled(int8_t cell) {
  if (cell < 0 || cell >= 16) return false;
  return cell != 0 && cell != 3 && cell != 12 && cell != 15;
}

int16_t diglettCellX(int8_t cell) {
  return DIGLETT_CELL_X[cell & 3];
}

int16_t diglettCellY(int8_t cell) {
  return DIGLETT_CELL_Y[cell >> 2];
}

void clearDiglettWave() {
  for (uint8_t i = 0; i < DIGLETT_MAX_ACTIVE; i++) {
    diglettCells[i] = -1;
    diglettShownAt[i] = 0;
    diglettHideAt[i] = 0;
  }
}

void spawnDiglettWave() {
  clearDiglettWave();
  uint8_t count = (uint8_t)random(1, DIGLETT_MAX_ACTIVE + 1);
  uint16_t usedCells = 0;
  uint32_t shownAt = millis();
  // Early waves pause clearly at the fully emerged frame for younger players.
  // Rising and retreating take 210 ms each; the full-frame hold therefore
  // starts at 780 ms and gradually shrinks to a still-readable 280 ms.
  uint32_t visibleMs = 1200;
  uint32_t speedup = (uint32_t)gameScore * 25UL;
  if (speedup > 500) speedup = 500;
  for (uint8_t i = 0; i < count; i++) {
    int8_t cell;
    do {
      cell = (int8_t)random(16);
    } while (!diglettCellEnabled(cell) || (usedCells & (1U << cell)) ||
             (i == 0 && cell == diglettLastCell));
    usedCells |= (uint16_t)(1U << cell);
    diglettCells[i] = cell;
    diglettShownAt[i] = shownAt;
    diglettHideAt[i] = shownAt + visibleMs - speedup;
    if (i == 0) diglettLastCell = cell;
  }
}

void startDiglettGame() {
  if (pet.isEgg() || pet.sleeping || pet.ceremony) return;
  gameMenuOpen = false;
  gameOpen = true;
  gameMode = 3;
  gameOverUntil = 0;
  gameScore = 0;
  gameMisses = 0;
  gameNewHi = false;
  gameGain = 0;
  clearDiglettWave();
  diglettLastCell = -1;
  diglettUntil = millis() + ACTIVITY_GAME_MS;
  diglettSpawnAt = millis() + 260;
  minigameAudioBegin();
  minigameSfxPlay(SFX_GAME_START);
  minigameBgmStart();
}

static const uint8_t TYPE_DEF_POOL[] = {
  TYPE_GRASS, TYPE_FIRE, TYPE_WATER, TYPE_ELECTRIC, TYPE_ROCK, TYPE_GROUND,
  TYPE_FLYING, TYPE_POISON, TYPE_PSYCHIC, TYPE_GHOST, TYPE_DRAGON, TYPE_ICE
};
static const uint8_t TYPE_COUNTER_POOL[] = {
  TYPE_FIRE, TYPE_WATER, TYPE_GRASS, TYPE_GROUND, TYPE_WATER, TYPE_WATER,
  TYPE_ELECTRIC, TYPE_PSYCHIC, TYPE_BUG, TYPE_GHOST, TYPE_ICE, TYPE_FIRE
};
static const uint8_t TYPE_OPTION_POOL[] = {
  TYPE_NORMAL, TYPE_FIRE, TYPE_WATER, TYPE_ELECTRIC, TYPE_GRASS, TYPE_ICE,
  TYPE_FIGHTING, TYPE_POISON, TYPE_GROUND, TYPE_FLYING, TYPE_PSYCHIC, TYPE_BUG,
  TYPE_ROCK, TYPE_GHOST, TYPE_DRAGON
};

void nextTypeQuestion() {
  uint8_t q = (uint8_t)random(sizeof(TYPE_DEF_POOL));
  typeEnemy = TYPE_DEF_POOL[q];
  uint8_t correct = TYPE_COUNTER_POOL[q];
  typeCorrect = (uint8_t)random(3);
  for (uint8_t i = 0; i < 3; i++) typeChoice[i] = TYPE_NONE;
  typeChoice[typeCorrect] = correct;
  for (uint8_t i = 0; i < 3; i++) {
    if (i == typeCorrect) continue;
    uint8_t cand;
    do {
      cand = TYPE_OPTION_POOL[random(sizeof(TYPE_OPTION_POOL))];
    } while (cand == correct || cand == typeChoice[0] || cand == typeChoice[1] || cand == typeChoice[2] ||
             battleTypeEffectPct(cand, typeEnemy, TYPE_NONE) > 100);
    typeChoice[i] = cand;
  }
  typeUntil = millis() + 4200;
}

void startTypeGame() {
  if (pet.isEgg() || pet.sleeping || pet.ceremony) return;
  gameMenuOpen = false;
  gameOpen = true;
  gameMode = 4;
  gameOverUntil = 0;
  gameScore = 0;
  gameMisses = 0;
  gameNewHi = false;
  gameGain = 0;
  nextTypeQuestion();
  minigameAudioBegin();
  minigameSfxPlay(SFX_GAME_START);
}

int16_t pickKnownFriend() {
  uint16_t choices = 0;
  for (int16_t dex = 1; dex <= DEX_COUNT; dex++) {
    if (dex != pet.speciesId && pet.isKnown(dex)) choices++;
  }
  if (!choices) return 0;
  uint16_t selected = (uint16_t)random(choices);
  for (int16_t dex = 1; dex <= DEX_COUNT; dex++) {
    if (dex == pet.speciesId || !pet.isKnown(dex)) continue;
    if (selected-- == 0) return dex;
  }
  return 0;
}

void startFriendGameWithDex(int16_t visitor) {
  if (pet.isEgg() || pet.sleeping || pet.ceremony || !pet.friendPlayUnlocked()) {
    minigameSfxPlay(SFX_DENY);
    return;
  }
  if (visitor <= 0 || !friendPmd.load((uint8_t)visitor, pet.isShinyRegistered(visitor))) {
    minigameSfxPlay(SFX_DENY);
    return;
  }
  friendDex = visitor;
  friendPlayStartedAt = millis();
  friendRewardApplied = false;
  friendSoundCue = 0;
  gameMenuOpen = false;
  gameOpen = true;
  gameMode = 5;
  gameOverUntil = 0;
  minigameAudioBegin();
  minigameSfxPlay(SFX_GAME_START);
}

void startFriendGame() {
  startFriendGameWithDex(pickKnownFriend());
}

void respawnBall() {
  ballX = 112 + random(242);
  ballY = 82;
  float sp = 3.35f + gameScore * 0.14f;
  if (sp > 8.4f) sp = 8.4f;
  ballVX = random(2) ? sp : -sp;
  ballVX += ((int)random(9) - 4) * 0.28f;
  ballVY = 2.05f;
}

int ballHitRadius() {
  if (gameScore >= 20) return 38;
  if (gameScore >= 8) return 44;
  return 50;
}

void gameTap(int16_t x, int16_t y) {
  if (gameMode == 5) return;  // 친구와 놀기는 지켜보기만 하는 연출
  if (gameMode == 0) { runnerTap(x, y); return; }
  if (gameMode == 1) {
    snorlaxTap(x, y);
    return;
  }
  if (gameMode == 2) {
    eeveeTap(x, y);
    return;
  }
  if (gameMode == 3) {
    diglettTap(x, y);
    return;
  }
  if (gameMode == 4) {
    magikarpTap(x, y);
    return;
  }
}

void finishCatchGame() {
  gameNewHi = (gameScore > pet.catchHi);
  gameGain = pet.applyCatchResult(gameScore);
  minigameSfxPlay(gameNewHi && gameScore > 0 ? SFX_MEDAL : SFX_LEVEL);
  gameOverUntil = millis() + 4000;
}

void catchTap(int16_t x, int16_t y) {
  if (gameOverUntil) return;
  if (y < 72) { minigameAudioEnd(); gameOpen = false; return; }
  int dx = x - catchX, dy = y - catchY;
  if (dx * dx + dy * dy <= 52 * 52) {
    gameScore++;
    hitX = catchX;
    hitY = catchY;
    hitTime = millis();
    minigameSfxPlay(SFX_MINIGAME_OK);
    spawnCatchTarget();
  } else if (++gameMisses >= 3) {
    finishCatchGame();
  } else {
    minigameSfxPlay(SFX_MINIGAME_BAD);
  }
}

void finishMemoGame() {
  gameScore = memoRounds;
  gameNewHi = (memoRounds > pet.memoHi);
  gameGain = pet.applyMemoResult(memoRounds);
  minigameSfxPlay(gameNewHi && memoRounds > 0 ? SFX_MEDAL : SFX_LEVEL);
  gameOverUntil = millis() + 4000;
}

int memoPadAt(int16_t x, int16_t y) {
  const int16_t px[4] = { 142, 324, 142, 324 };
  const int16_t py[4] = { 164, 164, 318, 318 };
  for (int i = 0; i < 4; i++) {
    int dx = x - px[i], dy = y - py[i];
    if (dx * dx + dy * dy <= 54 * 54) return i;
  }
  return -1;
}

void memoPadSound(uint8_t pad) {
  if (pad < 4) minigameSfxPlay((uint8_t)(SFX_MEMO_PAD_0 + pad));
}

void memoTap(int16_t x, int16_t y) {
  if (gameOverUntil) return;
  if (y < 72) { minigameAudioEnd(); gameOpen = false; return; }
  if (memoShowing || memoFailUntil || millis() < memoTurnUntil) return;
  int pad = memoPadAt(x, y);
  if (pad < 0) return;
  if (pad != memoSeq[memoInput]) {
    memoFlashPad = pad;
    memoFlashGood = false;
    memoFlashUntil = millis() + 620;
    memoHintPad = memoSeq[memoInput];
    memoFailUntil = memoFlashUntil;
    minigameSfxPlay(SFX_MINIGAME_BAD);
    return;
  }
  memoFlashPad = pad;
  memoFlashGood = true;
  memoFlashUntil = millis() + 180;
  memoPadSound((uint8_t)pad);
  memoInput++;
  if (memoInput >= memoLen) {
    memoRounds++;
    if (memoLen >= 14) finishMemoGame();
    else startMemoRound();
  }
}

void finishDiglettGame() {
  gameNewHi = (gameScore > pet.diglettHi);
  gameGain = pet.applyDiglettResult(gameScore);
  clearDiglettWave();
  minigameSfxPlay(gameNewHi && gameScore > 0 ? SFX_MEDAL : SFX_LEVEL);
  gameOverUntil = millis() + 4000;
}

void diglettTap(int16_t x, int16_t y) {
  if (gameOverUntil) return;
  if (y < 72) {
    minigameAudioEnd();
    gameOpen = false;
    return;
  }
  uint32_t now = millis();

  // Check visible Diglett first. The old circular cell lookup overlapped
  // vertically, so a tap on a lower Diglett could be assigned to the empty
  // hole above it and counted as a miss. Each active target uses a 99 x 65
  // rectangle. The expanded layout leaves a small gap between every hit box,
  // so a touch can never belong to two holes.
  int8_t hitSlot = -1;
  int32_t nearestDistance = INT32_MAX;
  for (uint8_t i = 0; i < DIGLETT_MAX_ACTIVE; i++) {
    int8_t cell = diglettCells[i];
    if (cell < 0 || diglettFrameAt(now, diglettShownAt[i], diglettHideAt[i]) == 0) continue;
    int16_t cx = diglettCellX(cell);
    int16_t cy = diglettCellY(cell) + 5;
    int16_t dx = x - cx, dy = y - cy;
    if (abs(dx) > 49 || abs(dy) > 32) continue;
    int32_t distance = (int32_t)dx * dx + (int32_t)dy * dy;
    if (distance < nearestDistance) {
      nearestDistance = distance;
      hitSlot = (int8_t)i;
    }
  }
  if (hitSlot >= 0) {
    if (gameScore < 255) gameScore++;
    int8_t hitCell = diglettCells[hitSlot];
    hitX = diglettCellX(hitCell);
    hitY = diglettCellY(hitCell);
    hitTime = now;
    diglettCells[hitSlot] = -1;
    if (!anyDiglettActive(now)) diglettSpawnAt = now + 150;
    minigameSfxPlay(SFX_DIGLETT_HIT);
    return;
  }

  // No visible target was hit: use the same non-overlapping rectangle for an
  // empty-hole miss, so every grid cell has one unambiguous touch region.
  int8_t tappedCell = -1;
  for (int8_t cell = 0; cell < 16; cell++) {
    if (!diglettCellEnabled(cell)) continue;
    int16_t cx = diglettCellX(cell);
    int16_t cy = diglettCellY(cell) + 5;
    int16_t dx = x - cx, dy = y - cy;
    if (abs(dx) <= 49 && abs(dy) <= 32) {
      tappedCell = cell;
      break;
    }
  }
  if (tappedCell < 0) return;
  if (++gameMisses >= 3) finishDiglettGame();
  else minigameSfxPlay(SFX_DIGLETT_MISS);
}

void finishTypeGame() {
  gameNewHi = (gameScore > pet.typeHi);
  gameGain = pet.applyTypeResult(gameScore);
  minigameSfxPlay(gameNewHi && gameScore > 0 ? SFX_MEDAL : SFX_LEVEL);
  gameOverUntil = millis() + 4000;
}

void typeTap(int16_t x, int16_t y) {
  if (gameOverUntil) return;
  if (y < 72) { minigameAudioEnd(); gameOpen = false; return; }
  int idx = -1;
  for (int i = 0; i < 3; i++) {
    int by = 210 + i * 60;
    if (x >= 70 && x <= 396 && y >= by - 8 && y <= by + 56) idx = i;
  }
  if (idx < 0) return;
  if ((uint8_t)idx == typeCorrect) {
    gameScore++;
    minigameSfxPlay(SFX_MINIGAME_OK);
    nextTypeQuestion();
  } else {
    if (++gameMisses >= 3) finishTypeGame();
  else minigameSfxPlay(SFX_MINIGAME_BAD);
  }
}

void stepGame() {
  float grav = 1.14f + gameScore * 0.046f;
  if (gameScore >= 5) grav += 0.14f;
  if (gameScore >= 12) grav += 0.18f;
  if (grav > 2.10f) grav = 2.10f;
  ballVX += sinf((millis() + gameScore * 97) * 0.018f) * 0.11f;
  if (random(100) < 7) ballVX += ((int)random(7) - 3) * 0.22f;
  ballVY += grav;
  ballX += ballVX;
  ballY += ballVY;
  // rebote en la pared circular
  float dx = ballX - CX, dy = ballY - CY;
  float d = sqrtf(dx * dx + dy * dy);
  if (d > 205) {
    float nx = dx / d, ny = dy / d;
    float dot = ballVX * nx + ballVY * ny;
    if (dot > 0) {
      ballVX = (ballVX - 2 * dot * nx) * 1.05f;
      ballVY = (ballVY - 2 * dot * ny) * 0.88f;
      ballVX += ((int)random(9) - 4) * 0.18f;
    minigameSfxPlay(SFX_BALL_BOUNCE);
    }
    ballX = CX + nx * 205;
    ballY = CY + ny * 205;
  }
  if (ballY > 384) {  // al suelo
    if (++gameMisses >= 3) {
      gameNewHi = (gameScore > pet.gameHi);
      pet.playResult(gameScore);  // actualiza el record y da felicidad
  minigameSfxPlay(gameNewHi && gameScore > 0 ? SFX_MEDAL : SFX_LEVEL);
      gameOverUntil = millis() + 4000;
    } else {
      respawnBall();
  minigameSfxPlay(SFX_BALL_MISS);
    }
  }
  // el bicho la sigue por abajo
  float chase = (ballX - gamePetX) * 0.12f;
  if (chase > 7) chase = 7;
  if (chase < -7) chase = -7;
  gamePetX += chase;
}

// ---------- saco de entrenamiento (entrena la fuerza) ----------

void startWalk() {
  if (pet.isEgg() || pet.sleeping || pet.ceremony ||
      pet.sitterActive(pet.lastSeenEpoch) || pet.expeditionInventoryFull()) {
    walkStartFailed = false;
    sfxPlay(SFX_DENY);
    return;
  }
  if (!pet.canStartWalk()) {
    walkStartFailed = false;
    snprintf(statusNoticeMsg, sizeof(statusNoticeMsg), "먼저 포켓몬을 돌봐주세요");
    statusNoticeUntil = millis() + 3500UL;
    sfxPlay(SFX_DENY);
    return;
  }
  if (!walkSensorStart()) {
    walkStartFailed = true;
    cardDirty = true;
    sfxPlay(SFX_DENY);
    return;
  }
  if (!pet.beginWalk()) {
    walkSensorStop();
    walkStartFailed = false;
    sfxPlay(SFX_DENY);
    return;
  }
  walkStartFailed = false;
  walkOpen = true;
  walkFinished = false;
  walkSteps = 0;
  walkTier = 0;
  walkReward = {};
  walkLastPollAt = 0;
  walkStartedEpoch = rtcEpoch();
  if (!walkStartedEpoch) walkStartedEpoch = pet.lastSeenEpoch;
  walkStartedUs = (uint64_t)esp_timer_get_time();
  cardOpen = false;
  hubOpen = false;
  lastInteract = millis();
  saveWalkCheckpoint(true);
  sfxPlay(SFX_EXPEDITION_START);
}

void finishWalk(bool completionAlert) {
  if (!walkOpen || walkFinished) return;
  uint32_t measured = walkSensorSteps();
  if (measured > walkSteps) walkSteps = measured > 65535UL ? 65535 : (uint16_t)measured;
  walkSensorStop();
  pet.endWalkPause();
  clearWalkCheckpoint();
  walkTier = walkSteps >= 1000 ? 4 : walkSteps >= 600 ? 3 : walkSteps >= 300 ? 2 : walkSteps >= 150 ? 1 : 0;
  walkReward = pet.applyWalkReward(walkSteps, (uint8_t)random(100));
  walkFinished = true;
  lastInteract = millis();
  if (completionAlert) careAlertSoundPlay();
  else sfxPlay(walkReward.count == 0 ? SFX_TAP : SFX_EXPEDITION_FOUND);
}

void updateWalk(uint32_t now) {
  if (!walkOpen || walkFinished) return;
  walkSensorUpdate(now);
  if (now - walkLastPollAt < 250UL) return;
  walkLastPollAt = now;
  uint32_t measured = walkSensorSteps();
  walkSteps = measured > 65535UL ? 65535 : (uint16_t)measured;
  saveWalkCheckpoint(false);
  uint32_t epoch = rtcEpoch();
  if (!epoch) epoch = pet.lastSeenEpoch;
  if (walkSteps >= 1000 || walkTimeExpired(epoch)) {
    if (screenOff) noteUserActivity(now);
    finishWalk(true);
  }
}

void renderWalk() {
  drawGameScene();
  uint16_t ink = (sceneHour() < 6 || sceneHour() >= 20) ? UI_INK_NIGHT : UI_INK;
  gfx->setTextColor(ink);
  gfx->setTextSize(4);
  const char *title = walkFinished ? T(S_WALK_REWARD) : T(S_WALK);
  gfx->setCursor(CX - gfx->textWidth(title) / 2, 48);
  gfx->print(title);

  char stepsText[24];
  snprintf(stepsText, sizeof(stepsText), T(S_STEPS_FMT), walkSteps);
  gfx->setTextSize(5);
  gfx->setCursor(CX - gfx->textWidth(stepsText) / 2, 96);
  gfx->print(stepsText);

  if (!walkFinished) {
    uint32_t phase = (millis() / 16UL) % 560UL;
    bool right = phase < 280;
    int x = right ? 92 + (int)phase : 372 - (int)(phase - 280);
    uint8_t act = right && pmd.has(PMD_WALKR) ? PMD_WALKR :
                  !right && pmd.has(PMD_WALKL) ? PMD_WALKL : PMD_IDLE;
    if (pmd.loaded) drawPmdActM(pmd, act, x, 314, millis(), true, false, 4);

    int progress = walkSteps >= 1000 ? 300 : (int)((uint32_t)walkSteps * 300UL / 1000UL);
    gfx->fillRoundRect(83, 332, 300, 18, 6, UI_TRACK);
    if (progress > 2) gfx->fillRoundRect(83, 332, progress, 18, 6, UI_BAR_OK);
    const uint16_t marks[4] = { 150, 300, 600, 1000 };
    for (uint8_t i = 0; i < 4; i++) {
      int mx = 83 + marks[i] * 300 / 1000;
      gfx->drawLine(mx, 328, mx, 354, ink);
    }
    uint16_t next = walkSteps < 150 ? 150 : walkSteps < 300 ? 300 : walkSteps < 600 ? 600 : 1000;
    char nextText[32];
    snprintf(nextText, sizeof(nextText), T(S_WALK_TIER_FMT), next);
    gfx->setTextSize(2);
    gfx->setCursor(CX - gfx->textWidth(nextText) / 2, 358);
    gfx->print(nextText);
  } else if (walkReward.count > 0) {
    gfx->setTextSize(2);
    for (uint8_t i = 0; i < walkReward.count; i++) {
      ExpeditionItem item = walkReward.items[i];
      int y = 184 + i * 34;
      uint16_t col = expeditionItemColor(item);
      gfx->fillCircle(116, y + 9, 10, col);
      gfx->setTextColor(ink);
      const char *reward = T(expeditionItemText(item));
      gfx->setCursor(142, y);
      gfx->print(reward);
    }
  } else {
    gfx->setTextColor(UI_BAR_BAD);
    gfx->setTextSize(2);
    const char *none = T(S_WALK_NO_REWARD);
    gfx->setCursor(CX - gfx->textWidth(none) / 2, 238);
    gfx->print(none);
  }

  gfx->fillRoundRect(128, 382, 210, 48, 12, UI_WHITE);
  gfx->drawRoundRect(128, 382, 210, 48, 12, ink);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(2);
  const char *button = walkFinished ? T(S_OK) : T(S_RETURN);
  gfx->setCursor(CX - gfx->textWidth(button) / 2, 397);
  gfx->print(button);
  gfx->flush();
}

// fondo del minijuego: hatibat del bicho (cielo por hora + suelo del bioma)
void drawGameScene() {
  int hh = sceneHour();
  bool night = hh < 6 || hh >= 20;
  uint16_t top, bot;
  if (night)       { top = C565(0x0c, 0x12, 0x24); bot = C565(0x1e, 0x26, 0x46); }
  else if (hh < 8) { top = C565(0xd1, 0x6a, 0x86); bot = C565(0xf3, 0xb8, 0x7c); }
  else if (hh < 18){ top = C565(0x8f, 0xc8, 0xea); bot = C565(0xdc, 0xee, 0xe6); }
  else             { top = C565(0xc7, 0x5a, 0x4a); bot = C565(0xf0, 0xae, 0x64); }
  int hor = 376;
  for (int y = 0; y < hor; y += 8)
    gfx->fillRect(0, y, 466, 8, lerp565(top, bot, y, hor));
  if (night)
    for (auto &st : STARS) gfx->fillRect(st[0], st[1], 4, 4, UI_WHITE);
  uint8_t bio = pet.isEgg() ? 0 : DEX_TBL[pet.speciesId].biome;
  uint16_t soil = BIOME_SOIL[bio < 6 ? bio : 0];
  if (night) soil = lerp565(soil, C565(0x16, 0x1c, 0x30), 9, 16);
  gfx->fillRect(0, hor, 466, 466 - hor, soil);
}

void drawGameResult(const char *recordFmt, uint16_t record, StrId gainFmt) {
  drawGameScene();
  if (millis() > gameOverUntil) {
    minigameAudioEnd();
    gameOpen = false;
    return;
  }
  bool night = sceneHour() < 6 || sceneHour() >= 20;
  uint16_t ink = night ? UI_INK_NIGHT : UI_INK;
  char buf[22];
  snprintf(buf, sizeof(buf), T(S_SCORE_FMT), gameScore);
  gfx->setTextColor(ink);
  gfx->setTextSize(4);
  gfx->setCursor(CX - gfx->textWidth(buf) / 2, 148);
  gfx->print(buf);
  char gain[18];
  snprintf(gain, sizeof(gain), T(gainFmt), gameGain);
  gfx->setTextColor(gainFmt == S_DEF_GAIN_FMT ? 0x4C98 : (gainFmt == S_HYG_GAIN_FMT ? UI_BAR_OK : UI_BAR_WARN));
  gfx->setTextSize(3);
  gfx->setCursor(CX - gfx->textWidth(gain) / 2, 204);
  gfx->print(gain);
  gfx->setTextSize(2);
  if (gameNewHi && gameScore > 0) {
    gfx->setTextColor(UI_BAR_WARN);
    gfx->setCursor(CX - gfx->textWidth(T(S_NEW_RECORD)) / 2, 256);
    gfx->print(T(S_NEW_RECORD));
  } else {
    char rec[20];
    snprintf(rec, sizeof(rec), recordFmt, record);
    gfx->setTextColor(ink);
    gfx->setCursor(CX - gfx->textWidth(rec) / 2, 256);
    gfx->print(rec);
  }
  gfx->flush();
}

void renderCatchGame() {
  uint32_t now = millis();
  if (gameOverUntil) {
    drawGameResult(T(S_RECORD_FMT), pet.catchHi, S_SPD_GAIN_FMT);
    return;
  }
  if (now >= catchUntil || gameMisses >= 3) {
    finishCatchGame();
    return;
  }
  if (now > catchTargetUntil) {
    if (++gameMisses >= 3) {
      finishCatchGame();
      return;
    }
    spawnCatchTarget();
  }
  drawGameScene();
  bool night = sceneHour() < 6 || sceneHour() >= 20;
  uint16_t ink = night ? UI_INK_NIGHT : UI_INK;
  gfx->setTextColor(ink);
  gfx->setTextSize(3);
  gfx->setCursor(CX - gfx->textWidth(T(S_CATCH_TITLE)) / 2, 32);
  gfx->print(T(S_CATCH_TITLE));
  char score[16], rec[16];
  snprintf(score, sizeof(score), T(S_SCORE_FMT), gameScore);
  snprintf(rec, sizeof(rec), T(S_REC_FMT), pet.catchHi);
  gfx->setTextSize(2);
  gfx->setCursor(50, 78);
  gfx->print(score);
  gfx->setCursor(294, 78);
  gfx->print(rec);
  for (int i = 0; i < 3; i++) {
    if (i < 3 - gameMisses) gfx->fillCircle(180 + i * 28, 104, 6, UI_BAR_BAD);
    else gfx->drawCircle(180 + i * 28, 104, 6, UI_TRACK);
  }
  const char *const *icon = catchIcon == 0 ? SPR_ICON_FOOD : (catchIcon == 1 ? SPR_ICON_BERRY_B : SPR_ICON_BERRY_G);
  gfx->fillCircle(catchX, catchY, 34, UI_WHITE);
  gfx->drawCircle(catchX, catchY, 36, UI_BAR_WARN);
  drawMap(icon, 16, catchX - 24, catchY - 24, 3, false);
  int bw = 280;
  int fw = (int)((uint32_t)bw * (catchUntil - now) / 20000);
  if (fw < 0) fw = 0;
  gfx->fillRoundRect(CX - bw / 2, 362, bw, 16, 5, UI_TRACK);
  if (fw > 2) gfx->fillRoundRect(CX - bw / 2, 362, fw, 16, 5, UI_BAR_OK);
  uint32_t ht = millis() - hitTime;
  if (hitTime && ht < 220) gfx->drawCircle((int)hitX, (int)hitY, 42 + ht / 8, UI_BAR_WARN);
  gfx->flush();
}

void stepMemoGame() {
  uint32_t now = millis();
  if (memoFailUntil) {
    if (now >= memoFailUntil) {
      memoFailUntil = 0;
      memoHintPad = -1;
      finishMemoGame();
    }
    return;
  }
  if (!memoShowing || now < memoNextAt) return;
  if (memoActivePad >= 0) {
    memoActivePad = -1;
    memoShow++;
    if (memoShow >= memoLen) {
      memoShowing = false;
      memoInput = 0;
      memoTurnUntil = now + 520;
    } else {
      memoNextAt = now + 150;
    }
    return;
  }
  memoActivePad = memoSeq[memoShow];
  memoPadSound((uint8_t)memoActivePad);
  memoNextAt = now + 480;
}

void renderMemoGame() {
  if (gameOverUntil) {
    drawGameResult(T(S_RECORD_FMT), pet.memoHi, S_DEF_GAIN_FMT);
    return;
  }
  stepMemoGame();
  drawGameScene();
  bool night = sceneHour() < 6 || sceneHour() >= 20;
  uint16_t ink = night ? UI_INK_NIGHT : UI_INK;
  char roundBuf[18], rec[16];
  snprintf(roundBuf, sizeof(roundBuf), T(S_ROUND_FMT), memoRounds + 1);
  snprintf(rec, sizeof(rec), T(S_REC_FMT), pet.memoHi);
  gfx->setTextColor(ink);
  gfx->setTextSize(3);
  gfx->setCursor(CX - gfx->textWidth(T(S_GAME_MEMO)) / 2, 34);
  gfx->print(T(S_GAME_MEMO));
  gfx->setTextSize(2);
  gfx->setCursor(60, 82);
  gfx->print(roundBuf);
  gfx->setCursor(310, 82);
  gfx->print(rec);
  const int16_t px[4] = { 142, 324, 142, 324 };
  const int16_t py[4] = { 164, 164, 318, 318 };
  const uint16_t col[4] = { UI_BAR_BAD, UI_BAR_WARN, 0x4C98, UI_BAR_OK };
  int active = memoShowing ? memoActivePad : (memoFailUntil ? memoHintPad : -1);
  for (int i = 0; i < 4; i++) {
    uint16_t fill = i == active ? lerp565(col[i], UI_WHITE, 5, 8) : col[i];
    gfx->fillCircle(px[i], py[i], 48, fill);
    gfx->drawCircle(px[i], py[i], 52, ink);
    if (i == active) {
      int pulse = 56 + (int)((millis() / 70) % 5);
      gfx->drawCircle(px[i], py[i], pulse, col[i]);
    }
    if (i == memoFlashPad && millis() < memoFlashUntil) {
      gfx->drawCircle(px[i], py[i], 60, memoFlashGood ? UI_BAR_OK : UI_BAR_BAD);
      gfx->drawCircle(px[i], py[i], 64, memoFlashGood ? UI_BAR_OK : UI_BAR_BAD);
    }
  }
  char phase[28];
  if (memoFailUntil) snprintf(phase, sizeof(phase), "%s", T(S_MEMO_WRONG));
  else if (memoShowing) snprintf(phase, sizeof(phase), "%s", T(S_MEMO_WATCH));
  else snprintf(phase, sizeof(phase), T(S_MEMO_TURN_FMT), memoInput + 1, memoLen);
  gfx->setTextColor(memoFailUntil ? UI_BAR_BAD : (memoShowing ? UI_BAR_WARN : UI_BAR_OK));
  gfx->setTextSize(2);
  gfx->setCursor(CX - gfx->textWidth(phase) / 2, 112);
  gfx->print(phase);
  gfx->flush();
}

void drawDiglettFrame(uint8_t frame, int16_t x, int16_t y, uint8_t scale) {
  static const char *paths[] = {
    "/extra/D01.tvr", "/extra/D02.tvr", "/extra/D03.tvr", "/extra/D04.tvr"
  };
  if (frame > 3) frame = 0;
  drawVisualAsset(gfx, paths[frame], x, y, scale);
}

void renderDiglettGame() {
  uint32_t now = millis();
  if (gameOverUntil) {
    drawGameResult(T(S_RECORD_FMT), pet.diglettHi, S_ATK_GAIN_FMT);
    return;
  }
  if (now >= diglettUntil || gameMisses >= 3) {
    finishDiglettGame();
    return;
  }
  for (uint8_t i = 0; i < DIGLETT_MAX_ACTIVE; i++) {
    if (diglettCells[i] >= 0 && now >= diglettHideAt[i]) diglettCells[i] = -1;
  }
  if (!anyDiglettActive(now) && now >= diglettSpawnAt) {
    spawnDiglettWave();
  }
  drawGameScene();
  bool night = sceneHour() < 6 || sceneHour() >= 20;
  uint16_t ink = night ? UI_INK_NIGHT : UI_INK;
  gfx->setTextColor(ink);
  gfx->setTextSize(3);
  gfx->setCursor(CX - gfx->textWidth(T(S_CLEAN_TITLE)) / 2, 32);
  gfx->print(T(S_CLEAN_TITLE));
  char score[16], rec[16];
  snprintf(score, sizeof(score), T(S_SCORE_FMT), gameScore);
  snprintf(rec, sizeof(rec), T(S_REC_FMT), pet.diglettHi);
  gfx->setTextSize(2);
  gfx->setCursor(50, 78);
  gfx->print(score);
  gfx->setCursor(294, 78);
  gfx->print(rec);
  for (int i = 0; i < 3; i++) {
    if (i < 3 - gameMisses) gfx->fillCircle(180 + i * 28, 104, 6, UI_BAR_BAD);
    else gfx->drawCircle(180 + i * 28, 104, 6, UI_TRACK);
  }
  // The empty hole is a separate user-supplied asset; D01-D04 are animation.
  for (int8_t cell = 0; cell < 16; cell++) {
    if (!diglettCellEnabled(cell)) continue;
    int16_t cx = diglettCellX(cell);
    int16_t cy = diglettCellY(cell);
    drawVisualAsset(gfx, "/extra/DD.tvr", cx - 33, cy - 20, 3);
  }
  for (uint8_t i = 0; i < DIGLETT_MAX_ACTIVE; i++) {
    int8_t cell = diglettCells[i];
    if (cell < 0 || now < diglettShownAt[i] || now >= diglettHideAt[i]) continue;
    int16_t cx = diglettCellX(cell);
    int16_t cy = diglettCellY(cell);
    uint8_t frame = diglettFrameAt(now, diglettShownAt[i], diglettHideAt[i]);
    drawDiglettFrame(frame, cx - 33, cy - 20, 3);
  }
  int bw = 280;
  int fw = (int)((uint32_t)bw * (diglettUntil - now) / ACTIVITY_GAME_MS);
  if (fw < 0) fw = 0;
  gfx->fillRoundRect(CX - bw / 2, 382, bw, 16, 5, UI_TRACK);
  if (fw > 2) gfx->fillRoundRect(CX - bw / 2, 382, fw, 16, 5, UI_BAR_OK);
  uint32_t ht = millis() - hitTime;
  if (hitTime && ht < 240) {
    int radius = 24 + ht / 6;
    for (uint8_t i = 0; i < 5; i++) gfx->drawCircle((int)hitX, (int)hitY, radius + i, UI_BAR_OK);
    for (uint8_t i = 0; i < 8; i++) {
      float angle = (float)i * PI / 4.0f;
      int x1 = (int)hitX + (int)(cosf(angle) * (radius + 4));
      int y1 = (int)hitY + (int)(sinf(angle) * (radius + 4));
      int x2 = (int)hitX + (int)(cosf(angle) * (radius + 15));
      int y2 = (int)hitY + (int)(sinf(angle) * (radius + 15));
      gfx->drawLine(x1, y1, x2, y2, UI_BAR_WARN);
    }
  }
  gfx->flush();
}

void renderTypeGame() {
  uint32_t now = millis();
  if (gameOverUntil) {
    drawGameResult(T(S_RECORD_FMT), pet.typeHi, S_ATK_GAIN_FMT);
    return;
  }
  if (now >= typeUntil) {
    if (++gameMisses >= 3) {
      finishTypeGame();
      return;
    }
    minigameSfxPlay(SFX_MINIGAME_BAD);
    nextTypeQuestion();
  }
  drawGameScene();
  bool night = sceneHour() < 6 || sceneHour() >= 20;
  uint16_t ink = night ? UI_INK_NIGHT : UI_INK;
  gfx->setTextColor(ink);
  gfx->setTextSize(3);
  gfx->setCursor(CX - gfx->textWidth(T(S_TYPE_TITLE)) / 2, 32);
  gfx->print(T(S_TYPE_TITLE));
  char score[16], rec[16];
  snprintf(score, sizeof(score), T(S_SCORE_FMT), gameScore);
  snprintf(rec, sizeof(rec), T(S_REC_FMT), pet.typeHi);
  gfx->setTextSize(2);
  gfx->setCursor(50, 78);
  gfx->print(score);
  gfx->setCursor(294, 78);
  gfx->print(rec);
  for (int i = 0; i < 3; i++) {
    if (i < 3 - gameMisses) gfx->fillCircle(180 + i * 28, 104, 6, UI_BAR_BAD);
    else gfx->drawCircle(180 + i * 28, 104, 6, UI_TRACK);
  }

  const char *enemy = battleTypeName(typeEnemy);
  gfx->fillRoundRect(118, 126, 230, 54, 14, lerp565(battleTypeColor(typeEnemy), UI_WHITE, 4, 8));
  gfx->drawRoundRect(118, 126, 230, 54, 14, ink);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(3);
  gfx->setCursor(CX - gfx->textWidth(enemy) / 2, 143);
  gfx->print(enemy);

  for (int i = 0; i < 3; i++) {
    int bx = 88;
    int by = 210 + i * 60;
    const char *label = battleTypeName(typeChoice[i]);
    gfx->fillRoundRect(bx, by, 290, 48, 12, lerp565(battleTypeColor(typeChoice[i]), UI_WHITE, 5, 8));
    gfx->drawRoundRect(bx, by, 290, 48, 12, ink);
    gfx->setTextColor(UI_INK);
    gfx->setTextSize(2);
    gfx->setCursor(bx + (290 - gfx->textWidth(label)) / 2, by + 17);
    gfx->print(label);
  }
  int bw = 280;
  int fw = (int)((uint32_t)bw * (typeUntil - now) / 4200);
  if (fw < 0) fw = 0;
  gfx->fillRoundRect(CX - bw / 2, 392, bw, 14, 5, UI_TRACK);
  if (fw > 2) gfx->fillRoundRect(CX - bw / 2, 392, fw, 14, 5, UI_BAR_OK);
  gfx->flush();
}

uint8_t friendSpriteAction(PmdMon &mon, uint8_t preferred, uint8_t fallback) {
  if (mon.has(preferred)) return preferred;
  if (mon.has(fallback)) return fallback;
  return PMD_IDLE;
}

uint32_t friendSpriteTime(PmdMon &mon, uint8_t action, uint32_t elapsed, uint32_t duration) {
  if (!mon.has(action) || duration == 0) return elapsed;
  return (uint32_t)((uint64_t)elapsed * pmdActTotalMs(mon.acts[action]) / duration);
}

void playFriendGameSounds(uint32_t elapsed) {
  // 장면당 한 번만 재생한다. 시작 효과음 뒤에 서로 울음소리로 인사하고,
  // 몸짓/추격/점프/행복 장면에는 짧은 효과음을 붙인다.
  if (friendSoundCue == 0 && elapsed >= 350UL) {
    minigameSpeciesChirpPlay(pet.speciesId);
    friendSoundCue = 1;
  } else if (friendSoundCue == 1 && elapsed >= 1350UL) {
    minigameSpeciesChirpPlay(friendDex);
    friendSoundCue = 2;
  } else if (friendSoundCue == 2 && elapsed >= 2850UL) {
    minigameSfxPlay(SFX_EVENT_SPARKLE);
    friendSoundCue = 3;
  } else if (friendSoundCue == 3 && elapsed >= 4450UL) {
    minigameSfxPlay(SFX_PLAY);
    friendSoundCue = 4;
  } else if (friendSoundCue == 4 && elapsed >= 6150UL) {
    minigameSfxPlay(SFX_PLAY);
    friendSoundCue = 5;
  } else if (friendSoundCue == 5 && elapsed >= 7050UL) {
    minigameSfxPlay(SFX_HEART);
    friendSoundCue = 6;
  }
}

void renderFriendGame() {
  uint32_t elapsed = millis() - friendPlayStartedAt;
  if (elapsed >= 10000UL) {
    if (!friendRewardApplied) {
      friendRewardApplied = true;
      pet.playWithFriend();
      minigameSfxPlay(SFX_HEART);
    }
    friendPmd.unload();
    friendDex = 0;
    minigameAudioEnd();
    gameOpen = false;
    markUiDirty();
    return;
  }

  playFriendGameSounds(elapsed);

  drawGameScene();
  bool night = sceneHour() < 6 || sceneHour() >= 20;
  uint16_t ink = night ? UI_INK_NIGHT : UI_INK;
  gfx->setTextColor(ink);
  gfx->setTextSize(3);
  const char *title = T(S_GAME_FRIEND);
  gfx->setCursor(CX - gfx->textWidth(title) / 2, 34);
  gfx->print(title);

  char visitor[28];
  snprintf(visitor, sizeof(visitor), "%s & %s", dexName(pet.speciesId), dexName(friendDex));
  gfx->setTextSize(2);
  gfx->setCursor(CX - gfx->textWidth(visitor) / 2, 78);
  gfx->print(visitor);

  if (elapsed < 1200UL) {
    // 서로 마주 보고 천천히 인사한다.
    uint8_t mainAct = friendSpriteAction(pmd, PMD_NOD, PMD_POSE);
    uint8_t guestAct = friendSpriteAction(friendPmd, PMD_NOD, PMD_POSE);
    drawPmdAct(mainAct, 150, 360, friendSpriteTime(pmd, mainAct, elapsed, 1200), false, false, 3);
    drawPmdActM(friendPmd, guestAct, 316, 360,
                friendSpriteTime(friendPmd, guestAct, elapsed > 180 ? elapsed - 180 : 0, 1020),
                false, false, 3);
  } else if (elapsed < 2800UL) {
    // 양쪽에서 중앙으로 걸어와 만난다. 수직 점프는 사용하지 않는다.
    uint32_t t = elapsed - 1200UL;
    uint16_t p = (uint16_t)(t * 1000UL / 1600UL);
    int leftX = 142 + 46 * p / 1000;
    int rightX = 324 - 46 * p / 1000;
    uint8_t mainAct = friendSpriteAction(pmd, PMD_WALKR, PMD_IDLE);
    uint8_t guestAct = friendSpriteAction(friendPmd, PMD_WALKL, PMD_IDLE);
    drawPmdAct(mainAct, leftX, 360, t, true, false, 3);
    drawPmdActM(friendPmd, guestAct, rightX, 360, t + 160, true, false, 3);
  } else if (elapsed < 4400UL) {
    // 한 마리씩 번갈아 포즈를 보여 주고 상대는 고개를 끄덕인다.
    uint32_t t = elapsed - 2800UL;
    bool mainTurn = t < 800UL;
    uint32_t turnT = mainTurn ? t : t - 800UL;
    uint8_t mainAct = mainTurn ? friendSpriteAction(pmd, PMD_POSE, PMD_NOD)
                               : friendSpriteAction(pmd, PMD_NOD, PMD_BREATH);
    uint8_t guestAct = mainTurn ? friendSpriteAction(friendPmd, PMD_NOD, PMD_BREATH)
                                : friendSpriteAction(friendPmd, PMD_POSE, PMD_NOD);
    drawPmdAct(mainAct, 188, 360, friendSpriteTime(pmd, mainAct, turnT, 800), false, false, 3);
    drawPmdActM(friendPmd, guestAct, 278, 360,
                friendSpriteTime(friendPmd, guestAct, turnT, 800), false, false, 3);
  } else if (elapsed < 6100UL) {
    // 방문 포켓몬에 따라 추격 방향을 바꿔 매번 같은 장면처럼 보이지 않게 한다.
    uint32_t t = elapsed - 4400UL;
    uint16_t p = (uint16_t)(t * 1000UL / 1700UL);
    bool chaseRight = (friendDex & 1) == 0;
    int leftX = chaseRight ? 142 + 70 * p / 1000 : 204 - 62 * p / 1000;
    int rightX = chaseRight ? 262 + 62 * p / 1000 : 324 - 70 * p / 1000;
    uint8_t mainAct = friendSpriteAction(pmd, chaseRight ? PMD_WALKR : PMD_WALKL, PMD_IDLE);
    uint8_t guestAct = friendSpriteAction(friendPmd, chaseRight ? PMD_WALKR : PMD_WALKL, PMD_IDLE);
    drawPmdAct(mainAct, leftX, 360, t, true, false, 3);
    drawPmdActM(friendPmd, guestAct, rightX, 360, t + 130, true, false, 3);
  } else if (elapsed < 7000UL) {
    // 마지막에는 둘 중 한 마리만 한 번 뛰고 다른 한 마리는 반응한다.
    uint32_t t = elapsed - 6100UL;
    bool mainHops = (friendDex & 1) != 0;
    uint8_t mainAct = mainHops ? friendSpriteAction(pmd, PMD_HOP, PMD_POSE)
                               : friendSpriteAction(pmd, PMD_NOD, PMD_POSE);
    uint8_t guestAct = mainHops ? friendSpriteAction(friendPmd, PMD_NOD, PMD_POSE)
                                : friendSpriteAction(friendPmd, PMD_HOP, PMD_POSE);
    drawPmdAct(mainAct, 188, 360, friendSpriteTime(pmd, mainAct, t, 900), false, false, 3);
    drawPmdActM(friendPmd, guestAct, 278, 360,
                friendSpriteTime(friendPmd, guestAct, t, 900), false, false, 3);
  } else {
    // 행복 구간은 연속 점프 대신 포즈와 호흡/끄덕임을 한 번씩 보여 준다.
    uint32_t happyT = elapsed - 7000UL;
    bool swap = happyT >= 1500UL;
    uint32_t poseT = swap ? happyT - 1500UL : happyT;
    uint8_t mainAct = swap ? friendSpriteAction(pmd, PMD_BREATH, PMD_NOD)
                           : friendSpriteAction(pmd, PMD_POSE, PMD_NOD);
    uint8_t guestAct = swap ? friendSpriteAction(friendPmd, PMD_POSE, PMD_NOD)
                            : friendSpriteAction(friendPmd, PMD_NOD, PMD_BREATH);
    drawPmdAct(mainAct, 170, 360, friendSpriteTime(pmd, mainAct, poseT, 1500), false, false, 3);
    drawPmdActM(friendPmd, guestAct, 296, 360,
                friendSpriteTime(friendPmd, guestAct, poseT, 1500), false, false, 3);
    int pulse = (int)((elapsed / 180) % 8);
    drawMap(SPR_HEART, 32, 108, 126 - pulse, 2, false);
    drawMap(SPR_HEART, 32, 294, 126 - (7 - pulse), 2, false);
    for (uint8_t i = 0; i < 3; i++) {
      int sx = 202 + i * 31;
      int sy = 176 + ((elapsed / 220 + i * 3) % 9);
      gfx->fillRect(sx - 6, sy - 1, 13, 3, UI_BAR_WARN);
      gfx->fillRect(sx - 1, sy - 6, 3, 13, UI_BAR_WARN);
    }
    gfx->setTextColor(UI_BAR_BAD);
    gfx->setTextSize(3);
    const char *happy = T(S_GREAT_JOY);
    gfx->setCursor(CX - gfx->textWidth(happy) / 2, 394);
    gfx->print(happy);
  }
  gfx->flush();
}

void renderGame() {
  // sin fillScreen(NEGRO): drawGameScene cubre los 466x466 completos. Si el
  // DMA del flush anterior aun lee el buffer, vera contenido valido (no negro
  // a medio pintar), que era el parpadeo a 25 fps.
  bool night = sceneHour() < 6 || sceneHour() >= 20;
  uint16_t ink = night ? UI_INK_NIGHT : UI_INK;

  if (gameMode == 5) {
    renderFriendGame();
    return;
  }
  if (gameMode == 0) { renderRunnerGame(); return; }
  if (gameMode == 1) { renderSnorlaxGame(); return; }
  if (gameMode == 2) { renderEeveeGame(); return; }
  if (gameMode == 3) { renderDiglettGame(); return; }
  if (gameMode == 4) { renderMagikarpGame(); return; }

  if (gameOverUntil) {
    drawGameScene();
    if (millis() > gameOverUntil) {
      minigameAudioEnd();
      gameOpen = false;
      return;
    }
    char buf[22];
    snprintf(buf, sizeof(buf), T(S_SCORE_FMT), gameScore);
    gfx->setTextColor(ink);
    gfx->setTextSize(4);
    gfx->setCursor(CX - gfx->textWidth(buf) / 2, 160);
    gfx->print(buf);
    gfx->setTextSize(2);
    if (gameNewHi && gameScore > 0) {
      gfx->setTextColor(UI_BAR_WARN);
      gfx->setCursor(CX - gfx->textWidth(T(S_NEW_RECORD)) / 2, 214);
      gfx->print(T(S_NEW_RECORD));
    } else {
      char rec[20];
      snprintf(rec, sizeof(rec), T(S_RECORD_FMT), pet.gameHi);
      gfx->setTextColor(ink);
      gfx->setCursor(CX - gfx->textWidth(rec) / 2, 214);
      gfx->print(rec);
    }
    const char *msg = gameScore >= 10 ? T(S_GREAT_JOY) : T(S_PLUS_JOY);
    gfx->setTextColor(ink);
    gfx->setCursor(CX - gfx->textWidth(msg) / 2, 250);
    gfx->print(msg);
    gfx->flush();
    return;
  }

  drawGameScene();
  stepGame();

  // marcador, record y vidas
  char buf[8];
  snprintf(buf, sizeof(buf), "%u", gameScore);
  gfx->setTextColor(ink);
  gfx->setTextSize(4);
  gfx->setCursor(CX - gfx->textWidth(buf) / 2, 30);
  gfx->print(buf);
  char rec[12];
  snprintf(rec, sizeof(rec), T(S_REC_FMT), pet.gameHi);
  gfx->setTextSize(2);
  gfx->setCursor(CX - gfx->textWidth(rec) / 2, 76);
  gfx->print(rec);
  for (int i = 0; i < 3; i++) {
    if (i < 3 - gameMisses) gfx->fillCircle(180 + i * 28, 104, 6, UI_BAR_BAD);
    else gfx->drawCircle(180 + i * 28, 104, 6, UI_TRACK);
  }

  if (pmd.loaded) {
    uint8_t act = (ballX > gamePetX + 4) ? PMD_WALKR : (ballX < gamePetX - 4) ? PMD_WALKL : PMD_IDLE;
    if (!pmd.has(act)) act = PMD_IDLE;
    drawPmdAct(act, (int)gamePetX, 394, millis(), true, false, 3);
  } else if (mon.loaded) {
    int s = (mon.h * 2 > 130) ? 1 : 2;
    int w = mon.w * s, h = mon.h * s;
    uint16_t fm = mon.frameMs ? mon.frameMs : 100;
    uint16_t fi = (millis() / fm) % mon.frames;
    const uint8_t *fr = mon.data + (uint32_t)fi * mon.w * mon.h;
    int px = (int)gamePetX - w / 2, py = 394 - h;
    for (int r = 0; r < mon.h; r++)
      for (int c = 0; c < mon.w; c++) {
        uint8_t idx = fr[r * mon.w + c];
        if (idx == 0xFF) continue;
        gfx->fillRect(px + c * s, py + r * s, s, s, mon.pal[idx]);
      }
  }

  // anillo de impacto que se expande y desvanece (feedback suave del golpe)
  uint32_t ht = millis() - hitTime;
  if (hitTime && ht < 260) {
    int rad = 22 + (int)(ht / 6);
    gfx->drawCircle((int)hitX, (int)hitY, rad, C565(0xff, 0xe7, 0x9f));
    gfx->drawCircle((int)hitX, (int)hitY, rad - 2, C565(0xff, 0xd9, 0x8a));
  }

  // la pokeball
  drawMap(SPR_ICON_PLAY, 16, (int)ballX - 24, (int)ballY - 24, 3, false);

  gfx->flush();
}

// ---------- combate salvaje manual ----------

uint16_t battleHpFor(const BattleStats &stats) {
  return stats.hp ? stats.hp : (uint16_t)(30 + stats.level * 5 + stats.def);
}

BattleStats petBattleStats() {
  BattleStats stats = {};
  stats.level = pet.level();
  const uint16_t rawAtk = pet.atkStat();
  const uint16_t rawDef = pet.defStat();
  const uint16_t rawSpe = pet.speStat();
  int16_t powerPct = pet.weight >= 80 ? 12 : pet.weight >= 60 ? 8 : pet.weight >= 40 ? 4 : 0;
  int16_t speedPct = pet.weight >= 80 ? -18 : pet.weight >= 60 ? -10 : pet.weight >= 40 ? -4 : 0;
  stats.atk = (uint16_t)((uint32_t)rawAtk * (100 + powerPct) / 100);
  stats.def = (uint16_t)((uint32_t)rawDef * (100 + powerPct) / 100);
  stats.spe = (uint16_t)((uint32_t)rawSpe * (100 + speedPct) / 100);
  if (stats.spe == 0 && rawSpe > 0) stats.spe = 1;
  // 몸무게의 방어 보정은 피해량만 줄인다. 최대 HP까지 이중으로
  // 증가하지 않도록 HP는 보정 전 방어력으로 확정한다.
  stats.hp = (uint16_t)(30 + (uint32_t)stats.level * 5 + rawDef);
  if (!pet.isEgg() && pet.speciesId >= 1 && pet.speciesId <= DEX_COUNT) {
    stats.type1 = DEX_TBL[pet.speciesId].type1;
    stats.type2 = DEX_TBL[pet.speciesId].type2;
    stats.move1 = pet.battleMove1;
    stats.move2 = pet.signatureMove();
  }
  return stats;
}

void scheduleNextWild(uint32_t now) {
  nextWildEligible = now + WILD_COOLDOWN_MS;
}

bool mainScreenReadyForWild() {
  if (screenOff || pet.awaitingStarter() || pet.isEgg() || pet.sleeping || pet.ceremony ||
      pet.sitterActive(pet.lastSeenEpoch)) return false;
  if (communicationState() != COMM_OFF || battleOpen || gameOpen || gameMenuOpen ||
      sleepMenuOpen || walkOpen || cardOpen || galleryOpen || kbOpen ||
      hubOpen || clockOpen || helpOpen || resetStage != RESET_NONE) return false;
  if (feedMenuUntil || confirmUntil || choiceKind || bathUntil || petEventUntil) return false;
  if (pet.evolving() || pet.wantEvolveButton() || pet.canRunawayNow() || pet.wantFarewellButton()) return false;
  return true;
}

void maybeOfferWildEncounterOnWake(uint32_t now) {
  if (nextWildEligible == 0) {
    scheduleNextWild(now);
    return;
  }
  if (wildPromptUntil) return;
  if ((int32_t)(now - nextWildEligible) < 0) return;
  if (!mainScreenReadyForWild()) return;
  if ((uint8_t)random(100) >= WILD_WAKE_CHANCE_PCT) return;

  wildPromptDex = pickWildSpecies((uint8_t)random(100));
  wildPromptLevel = wildLevelFor(pet.level(), (uint8_t)random(100));
  wildPromptUntil = now + WILD_PROMPT_MS;
  scheduleNextWild(now);
  markUiDirty();
  sfxPlay(SFX_MENU);
}

bool maybeOfferFriendOnWake(uint32_t now) {
  if (!pet.friendPlayUnlocked() || !mainScreenReadyForWild()) return false;
  uint32_t epoch = rtcEpoch();
  if (!epoch) epoch = pet.lastSeenEpoch;
  if (!epoch) return false;
  if (nextFriendCheckEpoch == 0) {
    nextFriendCheckEpoch = epoch + 3600UL;
    return false;
  }
  if (epoch < nextFriendCheckEpoch) return false;
  // 성공 여부와 관계없이 다음 판정은 한 시간 뒤다. 자주 화면을 켜도
  // 확률을 연속으로 다시 굴려 방문 이벤트를 강제로 띄울 수 없다.
  nextFriendCheckEpoch = epoch + 3600UL;
  if ((uint8_t)random(100) >= 15) return false;
  int16_t visitor = pickKnownFriend();
  if (visitor <= 0) return false;
  friendInviteDex = visitor;
  friendInviteUntil = now + 3000UL;
  markUiDirty();
  sfxPlay(SFX_MENU);
  return true;
}

void scheduleNextPetEvent(uint32_t now) {
  nextPetEventEligible = now + PET_EVENT_COOLDOWN_MS;
}

bool mainScreenReadyForPetEvent() {
  if (screenOff || pet.awaitingStarter() || pet.isEgg() || pet.sleeping || pet.ceremony) return false;
  if (battleOpen || gameOpen || gameMenuOpen || cardOpen || galleryOpen || kbOpen || clockOpen || helpOpen) return false;
  if (feedMenuUntil || confirmUntil || choiceKind || bathUntil || wildPromptUntil || friendInviteUntil) return false;
  if (pet.evolving() || pet.wantEvolveButton() || pet.canRunawayNow() || pet.wantFarewellButton()) return false;
  return true;
}

void maybeOfferPetEvent(uint32_t now) {
  if (nextPetEventEligible == 0) {
    scheduleNextPetEvent(now);
    return;
  }
  if (petEventUntil) {
    if (now > petEventUntil) petEventUntil = 0;
    return;
  }
  if (now - lastPetEventCheck < PET_EVENT_CHECK_MS) return;
  lastPetEventCheck = now;
  if (now < nextPetEventEligible) return;
  if (!mainScreenReadyForPetEvent()) return;
  uint8_t phase = currentDayPhase();
  uint8_t chance = (phase == 0) ? 14 : (phase == 3 ? 8 : 10);
  if ((uint8_t)random(100) >= chance) return;

  petEventType = (uint8_t)random(3);
  petEventUntil = now + PET_EVENT_PROMPT_MS;
  scheduleNextPetEvent(now);
  sfxPlay(SFX_EVENT_SPARKLE);
}

bool inPetEventHit(int16_t x, int16_t y) {
  int16_t ex = 366, ey = 286;
  int dx = x - ex, dy = y - ey;
  return dx * dx + dy * dy <= 44 * 44;
}

void acceptPetEvent() {
  uint8_t type = petEventType;
  petEventUntil = 0;
  scheduleNextPetEvent(millis());
  if (!pet.applyPetEvent(type)) return;
  StrId msg = S_EVENT_FOUND;
  if (type == PET_EVENT_HEART) msg = S_EVENT_PET;
  else if (type == PET_EVENT_SPARKLE) msg = S_EVENT_LUCKY;
  snprintf(petEventMsg, sizeof(petEventMsg), "%s", T(msg));
  petEventFeedbackUntil = millis() + 1800;
  sfxPlay(type == PET_EVENT_BERRY ? SFX_EAT : (type == PET_EVENT_SPARKLE ? SFX_EVENT_SPARKLE : SFX_HEART));
}

void closeBattle() {
  bool wasCommunicationBattle = battleCommunication;
  battleAudioEnd();
  battleOpen = false;
  battleResolved = false;
  battleAnimStage = BATTLE_ANIM_NONE;
  battleAnimStageAt = 0;
  battleAttackMenuUntil = 0;
  battleCatchOffered = false;
  battleCatchTried = false;
  battleCatchDone = false;
  battleCatchSuccess = false;
  battleRespectCatch = false;
  battleCatchChance = 0;
  battleCaptureActive = false;
  battleCaptureStartedAt = 0;
  battleCaptureSoundStep = 0;
  battleCommunication = false;
  battleCommHost = false;
  battleCommWaiting = false;
  battleCommLocalActionReady = false;
  commPeerActionReady = false;
  if (wasCommunicationBattle) {
    if (communicationState() != COMM_OFF) communicationStop();
    commSelectedMode = COMM_MODE_NONE;
    commDone = false;
    commWon = false;
    commFailed = false;
    commStartedAt = 0;
    commResultSentAt = 0;
  }
  wildPmd.unload();
  markUiDirty();
  lockTouchBrief();
}

void startBattleWith(int16_t forcedDex, uint8_t forcedLevel) {
  if (!canStartWildBattle(pet.isEgg(), pet.sleeping, pet.ceremony)) return;
  wildPromptUntil = 0;
  scheduleNextWild(millis());
  if (forcedDex >= 1 && forcedDex <= DEX_COUNT) {
    battleDex = forcedDex;
    battleLevel = forcedLevel ? forcedLevel : wildLevelFor(pet.level(), (uint8_t)random(100));
  } else {
    uint8_t speciesRoll = random(100);
    uint8_t levelRoll = random(100);
    battleDex = pickWildSpecies(speciesRoll);
    battleLevel = wildLevelFor(pet.level(), levelRoll);
  }
  battlePlayer = petBattleStats();
  battleEnemy = wildBattleStats(battleDex, battleLevel);
  battleEnemy.hp = 0;
  battleResult = {};
  battleRun = beginBattleRuntime(battlePlayer, battleEnemy);
  battleTurn = {};
  battleReward = {};
  battleMsg[0] = 0;
  battleAttackMenuUntil = 0;
  battleLowHpWarned = false;
  battleMoveDiscFound = false;
  battleCatchOffered = false;
  battleCatchTried = false;
  battleCatchDone = false;
  battleCatchSuccess = false;
  battleRespectCatch = false;
  battleCatchChance = 0;
  battleCaptureActive = false;
  battleCaptureStartedAt = 0;
  battleCaptureSoundStep = 0;
  battleResolved = false;
  battleCommunication = false;
  battleAnimStage = BATTLE_ANIM_NONE;
  battleAnimStageAt = 0;
  battleAnimEnemyActs = false;
  battleAnimPlayerActsAfterEnemy = false;
  battleOpen = true;
  battleDirty = true;
  wildPmd.unload();
  wildPmd.load(battleDex, false);
  battleAudioBegin();
  sfxPlay(SFX_TAP);
  speciesChirpPlay(battleDex);
}

void startCommunicationBattle(const CommunicationPetData &peer, bool localIsHost) {
  if (peer.species < 1 || peer.species > DEX_COUNT || pet.isEgg()) return;
  battleDex = peer.species;
  battleLevel = peer.level ? peer.level : 1;
  battlePlayer = petBattleStats();
  const DexEntry &theirs = DEX_TBL[battleDex];
  battleEnemy = { peer.hp, peer.atk, peer.def, peer.spe, battleLevel, theirs.type1, theirs.type2,
                  peer.move1, signatureMoveForSpecies(battleDex) };
  battleResult = {};
  battleRun = beginBattleRuntime(battlePlayer, battleEnemy);
  battleTurn = {};
  battleReward = {};
  snprintf(battleMsg, sizeof(battleMsg), "%s", T(S_COMM_CHOOSE));
  battleAttackMenuUntil = 0;
  battleLowHpWarned = false;
  battleMoveDiscFound = false;
  battleCatchOffered = false;
  battleCatchTried = false;
  battleCatchDone = false;
  battleCatchSuccess = false;
  battleRespectCatch = false;
  battleCatchChance = 0;
  battleCaptureActive = false;
  battleCaptureStartedAt = 0;
  battleCaptureSoundStep = 0;
  battleResolved = false;
  battleAnimStage = BATTLE_ANIM_NONE;
  battleAnimStageAt = 0;
  battleAnimEnemyActs = false;
  battleAnimPlayerActsAfterEnemy = false;
  battleCommunication = true;
  battleCommHost = localIsHost;
  battleCommWaiting = false;
  battleCommLocalActionReady = false;
  commPeerActionReady = false;
  battleOpen = true;
  cardOpen = false;
  // Explicitly leave the communication hub on both devices. battleOpen has
  // rendering priority, but closing the parent prevents a stale room screen
  // from resurfacing during the first transition frame.
  hubOpen = false;
  hubPage = 0;
  battleDirty = true;
  wildPmd.unload();
  // The peer packet already carries the shiny bit. Use it when loading the
  // opponent PMD instead of forcing every linked opponent to normal colors.
  wildPmd.load(battleDex, (peer.flags & COMM_PET_SHINY) != 0);
  battleAudioBegin();
  sfxPlay(SFX_TAP);
  speciesChirpPlay(battleDex);
}

void startBattle() {
  startBattleWith(0, 0);
}

void finishBattle() {
  if (battleResolved) return;
  battleAnimStage = BATTLE_ANIM_NONE;
  battleResolved = true;
  battleDirty = true;
  if (battleCommunication) {
    battleCatchOffered = false;
    battleRespectCatch = false;
    sfxPlay(battleTurn.playerWon ? SFX_BATTLE_WIN : SFX_BATTLE_LOSS);
    return;
  }
  if (battleTurn.playerRan) {
    battleCatchOffered = false;
    battleRespectCatch = false;
    sfxPlay(SFX_TAP);
    return;
  }
  if (battleTurn.playerWon) {
    bool closeWin = battleRun.playerHp <= battleRun.playerMaxHp / 3;
    battleReward = pet.applyBattleWin(battleDex, closeWin);
    battleCatchOffered = true;
    battleRespectCatch = false;
    battleCatchChance = pet.catchChanceForWild(battleDex, battleLevel, battlePlayer.level, closeWin);
    battleMoveDiscFound = pet.awardMoveDisc(6, (uint8_t)random(100));
    sfxPlay(SFX_BATTLE_WIN);
  } else {
    battleReward = {};
    pet.applyBattleLoss();
    battleCatchChance = 0;
    battleCatchOffered = false;
    battleRespectCatch = false;
    sfxPlay(SFX_BATTLE_LOSS);
  }
}

uint32_t battleAnimationDuration(BattleAnimationStage stage) {
  switch (stage) {
    case BATTLE_ANIM_PLAYER_ATTACK:
      return battleLastAction == BATTLE_SKILL2 ? 860UL :
             (battleLastAction == BATTLE_SKILL1 ? 720UL : 620UL);
    case BATTLE_ANIM_ENEMY_REACT: return battleTurn.enemyDodged ? 460UL : 540UL;
    case BATTLE_ANIM_PLAYER_REST: return 720UL;
    case BATTLE_ANIM_ENEMY_ATTACK: return 680UL;
    case BATTLE_ANIM_PLAYER_REACT: return battleTurn.playerDodged ? 480UL : 540UL;
    case BATTLE_ANIM_STATUS: return 820UL;
    default: return 1UL;
  }
}

void setBattleAnimationStage(BattleAnimationStage stage, uint32_t now) {
  battleAnimStage = stage;
  battleAnimStageAt = now;
  battleDirty = true;
}

bool battleActionIsAttack(BattleAction action) {
  return action == BATTLE_BASIC || action == BATTLE_SKILL1 || action == BATTLE_SKILL2;
}

uint16_t battleActionMoveId(const BattleStats &stats, BattleAction action) {
  if (action == BATTLE_SKILL1) return stats.move1;
  if (action == BATTLE_SKILL2) return stats.move2;
  return MOVE_NONE;
}

const char *battleActionName(const BattleStats &stats, BattleAction action) {
  uint16_t moveId = battleActionMoveId(stats, action);
  return moveId == MOVE_NONE ? T(S_BASIC_ATTACK) : moveDef(moveId).name;
}

uint8_t battleActionEffect(const BattleStats &stats, BattleAction action) {
  uint16_t moveId = battleActionMoveId(stats, action);
  return moveId == MOVE_NONE ? MOVE_FX_IMPACT : moveDef(moveId).effect;
}

uint8_t battleActionType(const BattleStats &stats, BattleAction action) {
  uint16_t moveId = battleActionMoveId(stats, action);
  return moveId == MOVE_NONE ? TYPE_NORMAL : moveDef(moveId).type;
}

void setBattlePlayerHitMessage() {
  if (battleTurn.enemyDodged) {
    snprintf(battleMsg, sizeof(battleMsg), T(S_ENEMY_DODGED));
  } else if (battleTurn.playerDamage > 0) {
    if (battleTurn.playerTypePct > 100)
      snprintf(battleMsg, sizeof(battleMsg), "%s %u", T(S_EFFECTIVE), battleTurn.playerDamage);
    else if (battleTurn.playerTypePct < 100)
      snprintf(battleMsg, sizeof(battleMsg), "%s %u", T(S_NOT_EFFECTIVE), battleTurn.playerDamage);
    else
      snprintf(battleMsg, sizeof(battleMsg), T(S_HIT_FMT), battleTurn.playerDamage);
  } else {
    snprintf(battleMsg, sizeof(battleMsg), T(S_MISSED));
  }
}

void finishBattleAnimation() {
  battleAnimStage = BATTLE_ANIM_NONE;
  battleAnimStageAt = 0;
  battleDirty = true;
  if (battleTurn.battleEnded) {
    finishBattle();
    return;
  }
  if (!battleLowHpWarned && battleRun.playerHp > 0 &&
      battleRun.playerHp <= battleRun.playerMaxHp * 3 / 10) {
    battleLowHpWarned = true;
    sfxPlay(SFX_LOW_HP);
  }
}

void updateBattleAnimation(uint32_t now) {
  if (!battleOpen || battleAnimStage == BATTLE_ANIM_NONE) return;
  if (now - battleAnimStageAt < battleAnimationDuration(battleAnimStage)) return;

  switch (battleAnimStage) {
    case BATTLE_ANIM_PLAYER_ATTACK:
      setBattlePlayerHitMessage();
      if (battleTurn.enemyDodged) sfxPlay(SFX_TAP);
      else if (battleTurn.playerTypePct > 100) sfxPlay(SFX_EFFECTIVE);
      else if (battleTurn.playerTypePct < 100) sfxPlay(SFX_WEAK_HIT);
      else sfxPlay(battleTurn.playerDamage > 0 ? SFX_PLAY : SFX_TAP);
      setBattleAnimationStage(BATTLE_ANIM_ENEMY_REACT, now);
      break;

    case BATTLE_ANIM_ENEMY_REACT:
      if (battleAnimEnemyActs) {
        snprintf(battleMsg, sizeof(battleMsg), "%s: %s", dexName(battleDex),
                 battleActionName(battleEnemy, battleCommEnemyAction));
        speciesChirpPlay(battleDex);
        sfxPlay(battleCommEnemyAction == BATTLE_SKILL2 ? SFX_ATTACK_HEAVY : SFX_ATTACK_QUICK);
        setBattleAnimationStage(BATTLE_ANIM_ENEMY_ATTACK, now);
      } else {
        finishBattleAnimation();
      }
      break;

    case BATTLE_ANIM_PLAYER_REST:
      if (battleAnimEnemyActs) {
        snprintf(battleMsg, sizeof(battleMsg), "%s: %s", dexName(battleDex),
                 battleActionName(battleEnemy, battleCommEnemyAction));
        speciesChirpPlay(battleDex);
        sfxPlay(battleCommEnemyAction == BATTLE_SKILL2 ? SFX_ATTACK_HEAVY : SFX_ATTACK_QUICK);
        setBattleAnimationStage(BATTLE_ANIM_ENEMY_ATTACK, now);
      } else {
        finishBattleAnimation();
      }
      break;

    case BATTLE_ANIM_ENEMY_ATTACK:
      if (battleTurn.playerDodged) {
        snprintf(battleMsg, sizeof(battleMsg), T(S_DODGED));
        sfxPlay(SFX_TAP);
      } else if (battleTurn.enemyDamage > 0) {
        snprintf(battleMsg, sizeof(battleMsg), T(S_HIT_FMT), battleTurn.enemyDamage);
        sfxPlay(SFX_ENEMY_HIT);
      } else {
        snprintf(battleMsg, sizeof(battleMsg), T(S_MISSED));
        sfxPlay(SFX_TAP);
      }
      setBattleAnimationStage(BATTLE_ANIM_PLAYER_REACT, now);
      break;

    case BATTLE_ANIM_PLAYER_REACT:
      if (battleAnimPlayerActsAfterEnemy) {
        battleAnimPlayerActsAfterEnemy = false;
        battleLastAction = battleCommLocalAction;
        snprintf(battleMsg, sizeof(battleMsg), "%s", battleActionName(battlePlayer, battleLastAction));
        speciesChirpPlay(pet.speciesId);
        sfxPlay(battleLastAction == BATTLE_SKILL2 ? SFX_ATTACK_HEAVY : SFX_ATTACK_QUICK);
        setBattleAnimationStage(BATTLE_ANIM_PLAYER_ATTACK, now);
      } else {
        finishBattleAnimation();
      }
      break;
    case BATTLE_ANIM_STATUS:
      finishBattleAnimation();
      break;

    default:
      finishBattleAnimation();
      break;
  }
}

void beginCommunicationBattleAnimation(BattleAction action,
                                       BattleAction enemyAction,
                                       uint16_t playerHpBefore,
                                       uint16_t enemyHpBefore) {
  battleLastAction = action;
  battleCommLocalAction = action;
  battleCommEnemyAction = enemyAction;
  battleAnimPlayerHpBefore = playerHpBefore;
  battleAnimEnemyHpBefore = enemyHpBefore;
  uint32_t restored = (uint32_t)playerHpBefore + battleTurn.playerHeal;
  battleAnimPlayerHpAfterRest = restored > battleRun.playerMaxHp
                                    ? battleRun.playerMaxHp : (uint16_t)restored;
  battleAnimEnemyActs = battleTurn.enemyActed;
  battleAnimPlayerActsAfterEnemy = false;
  battleCommWaiting = false;
  battleCommLocalActionReady = false;
  battleAttackMenuUntil = 0;
  uint32_t now = millis();

  if (battleTurn.moveUnavailable) {
    snprintf(battleMsg, sizeof(battleMsg), "%s", T(S_NO_USES));
    sfxPlay(SFX_DENY);
    setBattleAnimationStage(BATTLE_ANIM_STATUS, now);
  } else if (battleTurn.recoveryFailed) {
    snprintf(battleMsg, sizeof(battleMsg), "%s", T(S_NO_REST));
    sfxPlay(SFX_DENY);
    setBattleAnimationStage(BATTLE_ANIM_STATUS, now);
  } else if (battleTurn.playerRan) {
    snprintf(battleMsg, sizeof(battleMsg), "%s", T(S_RAN));
    sfxPlay(SFX_TAP);
    setBattleAnimationStage(BATTLE_ANIM_STATUS, now);
  } else if (battleTurn.playerForfeited) {
    snprintf(battleMsg, sizeof(battleMsg), "%s", T(S_FORFEIT));
    sfxPlay(SFX_TAP);
    setBattleAnimationStage(BATTLE_ANIM_STATUS, now);
  } else if (enemyAction == BATTLE_RUN && battleTurn.battleEnded) {
    snprintf(battleMsg, sizeof(battleMsg), "%s", T(S_OPPONENT_FORFEITED));
    sfxPlay(SFX_TAP);
    setBattleAnimationStage(BATTLE_ANIM_STATUS, now);
  } else if (battleTurn.playerRecovered) {
    char healMsg[18];
    snprintf(healMsg, sizeof(healMsg), T(S_RESTED_FMT), battleTurn.playerHeal);
    snprintf(battleMsg, sizeof(battleMsg), "%s", healMsg);
    sfxPlay(SFX_REST);
    setBattleAnimationStage(BATTLE_ANIM_PLAYER_REST, now);
  } else if (battleTurn.runFailed) {
    snprintf(battleMsg, sizeof(battleMsg), "%s", T(S_RUN_FAILED));
    speciesChirpPlay(battleDex);
    sfxPlay(SFX_DENY);
    setBattleAnimationStage(BATTLE_ANIM_ENEMY_ATTACK, now);
  } else if (battleActionIsAttack(action) && !battleTurn.playerActedFirst && battleTurn.enemyActed) {
    battleAnimEnemyActs = false;
    battleAnimPlayerActsAfterEnemy = battleRun.playerHp > 0;
    snprintf(battleMsg, sizeof(battleMsg), "%s: %s", dexName(battleDex),
             battleActionName(battleEnemy, enemyAction));
    speciesChirpPlay(battleDex);
    sfxPlay(enemyAction == BATTLE_SKILL2 ? SFX_ATTACK_HEAVY : SFX_ATTACK_QUICK);
    setBattleAnimationStage(BATTLE_ANIM_ENEMY_ATTACK, now);
  } else {
    snprintf(battleMsg, sizeof(battleMsg), "%s", battleActionName(battlePlayer, action));
    speciesChirpPlay(pet.speciesId);
    sfxPlay(action == BATTLE_SKILL2 ? SFX_ATTACK_HEAVY : SFX_ATTACK_QUICK);
    setBattleAnimationStage(BATTLE_ANIM_PLAYER_ATTACK, now);
  }
}

void applyCommunicationBattleTurn(const CommunicationBattleTurnData &packet) {
  if (!battleCommunication || battleCommHost || packet.transaction != commTransaction) return;
  if (packet.round != battleRun.round + 1) return;

  uint16_t playerBefore = battleRun.playerHp;
  uint16_t enemyBefore = battleRun.enemyHp;
  battleRun.round = packet.round;
  battleRun.playerHp = packet.guestHp;
  battleRun.enemyHp = packet.hostHp;
  battleRun.restUsesLeft = packet.guestRestLeft;
  battleRun.enemyRestUsesLeft = packet.hostRestLeft;
  battleRun.skill1UsesLeft = packet.guestSkill1Left;
  battleRun.skill2UsesLeft = packet.guestSkill2Left;
  battleRun.enemySkill1UsesLeft = packet.hostSkill1Left;
  battleRun.enemySkill2UsesLeft = packet.hostSkill2Left;

  battleTurn = {};
  BattleAction localAction = (BattleAction)packet.guestAction;
  BattleAction enemyAction = (BattleAction)packet.hostAction;
  battleTurn.playerDamage = packet.guestDamage;
  battleTurn.enemyDamage = packet.hostDamage;
  battleTurn.playerHeal = packet.guestHeal;
  battleTurn.playerDodged = (packet.flags & COMM_TURN_GUEST_DODGED) != 0;
  battleTurn.enemyDodged = (packet.flags & COMM_TURN_HOST_DODGED) != 0;
  battleTurn.playerRecovered = localAction == BATTLE_RECOVER;
  battleTurn.playerForfeited = (packet.flags & COMM_TURN_GUEST_FORFEIT) != 0;
  battleTurn.playerActedFirst = (packet.flags & COMM_TURN_HOST_FIRST) == 0;
  battleTurn.enemyActed = (packet.flags & COMM_TURN_HOST_ACTED) != 0;
  battleTurn.battleEnded = (packet.flags & COMM_TURN_ENDED) != 0;
  battleTurn.playerWon = battleTurn.battleEnded && (packet.flags & COMM_TURN_HOST_WON) == 0;
  battleTurn.playerTypePct = packet.guestTypePct;
  battleTurn.enemyTypePct = packet.hostTypePct;
  battleTurn.playerMoveId = battleActionMoveId(battlePlayer, localAction);
  battleTurn.enemyMoveId = battleActionMoveId(battleEnemy, enemyAction);

  uint16_t playerAfterHeal = playerBefore + packet.guestHeal;
  if (playerAfterHeal > battleRun.playerMaxHp) playerAfterHeal = battleRun.playerMaxHp;
  uint16_t enemyAfterHeal = enemyBefore + packet.hostHeal;
  if (enemyAfterHeal > battleRun.enemyMaxHp) enemyAfterHeal = battleRun.enemyMaxHp;
  battleRun.enemyDamageTotal += playerAfterHeal > packet.guestHp ? playerAfterHeal - packet.guestHp : 0;
  battleRun.playerDamageTotal += enemyAfterHeal > packet.hostHp ? enemyAfterHeal - packet.hostHp : 0;

  beginCommunicationBattleAnimation(localAction, enemyAction, playerBefore, enemyBefore);
}

void performBattleAction(BattleAction action) {
  if (battleResolved || battleAnimStage != BATTLE_ANIM_NONE) return;
  if ((action == BATTLE_SKILL1 || action == BATTLE_SKILL2) &&
      !battleActionUnlocked(battlePlayer, action)) {
    snprintf(battleMsg, sizeof(battleMsg), "%s", T(S_SKILL_LOCKED));
    battleDirty = true;
    sfxPlay(SFX_DENY);
    return;
  }
  if ((action == BATTLE_SKILL1 && battleRun.skill1UsesLeft == 0) ||
      (action == BATTLE_SKILL2 && battleRun.skill2UsesLeft == 0)) {
    snprintf(battleMsg, sizeof(battleMsg), "%s", T(S_NO_USES));
    battleDirty = true;
    sfxPlay(SFX_DENY);
    return;
  }
  bool fullyCharged = battleRun.skill1UsesLeft == 3 && battleRun.skill2UsesLeft == 2;
  if (action == BATTLE_RECOVER &&
      (battleRun.restUsesLeft == 0 || (battleRun.playerHp == battleRun.playerMaxHp && fullyCharged))) {
    snprintf(battleMsg, sizeof(battleMsg), "%s", T(S_NO_REST));
    battleDirty = true;
    sfxPlay(SFX_DENY);
    return;
  }
  if (battleCommunication) {
    if (battleCommWaiting || battleCommLocalActionReady) return;
    battleAttackMenuUntil = 0;
    battleCommLocalAction = action;
    battleCommLocalActionReady = true;
    battleCommWaiting = true;
    snprintf(battleMsg, sizeof(battleMsg), "%s", T(S_COMM_WAIT_TURN));
    battleDirty = true;
    sfxPlay(SFX_TAP);
    if (!battleCommHost) {
      CommunicationBattleActionData packet;
      packet.round = battleRun.round + 1;
      packet.action = (uint8_t)action;
      packet.transaction = commTransaction;
      if (!communicationSendBattleAction(packet)) {
        battleCommWaiting = false;
        battleCommLocalActionReady = false;
        commFailed = true;
        snprintf(battleMsg, sizeof(battleMsg), "%s", T(S_COMM_FAILED));
        sfxPlay(SFX_DENY);
      }
    }
    return;
  }
  battleAttackMenuUntil = 0;
  battleAnimPlayerHpBefore = battleRun.playerHp;
  battleAnimEnemyHpBefore = battleRun.enemyHp;
  battleTurn = stepBattle(battleRun, action, (uint8_t)random(100));
  beginCommunicationBattleAnimation(action, battleTurn.enemyAction,
                                     battleAnimPlayerHpBefore, battleAnimEnemyHpBefore);
}

void battleTap(int16_t x, int16_t y) {
  if (battleCaptureActive) return;
  if (battleResolved) {
    if (battleTurn.playerWon && battleCatchOffered && !battleCatchDone) {
      if (x >= 76 && x <= 224 && y >= 392 && y <= 448) {
        bool closeWin = battleRun.playerHp <= battleRun.playerMaxHp / 3;
        battleCatchTried = true;
        battleCatchDone = true;
        if (battleRespectCatch) {
          battleCatchSuccess = pet.tryRespectCatchWild(battleDex, battleLevel, battlePlayer.level, (uint8_t)random(100));
          battleCatchChance = pet.respectCatchChanceForWild(battleDex, battleLevel, battlePlayer.level);
        } else {
          battleCatchSuccess = pet.tryCatchWild(battleDex, battleLevel, battlePlayer.level, closeWin, (uint8_t)random(100));
          battleCatchChance = pet.catchChanceForWild(battleDex, battleLevel, battlePlayer.level, closeWin);
        }
        if (battleCatchSuccess)
          battleMoveDiscFound = pet.awardMoveDisc(6, (uint8_t)random(100)) || battleMoveDiscFound;
        battleCaptureActive = true;
        battleCaptureStartedAt = millis();
        battleCaptureSoundStep = 0;
        galleryDirty = true;
        battleDirty = true;
        return;
      }
      if (x >= 242 && x <= 390 && y >= 392 && y <= 448) {
        battleCatchDone = true;
        battleCatchTried = false;
        battleDirty = true;
        sfxPlay(SFX_TAP);
        return;
      }
      return;
    }
    if (x >= 118 && x <= 348 && y >= 392 && y <= 454) closeBattle();
    return;
  }
  if (battleAnimStage != BATTLE_ANIM_NONE || battleCommWaiting) return;
  if (battleAttackMenuUntil) {
    if (x >= 34 && x <= 150 && y >= 288 && y <= 350) {
      performBattleAction(BATTLE_BASIC);
      return;
    }
    if (x >= 157 && x <= 273 && y >= 288 && y <= 350) {
      performBattleAction(BATTLE_SKILL1);
      return;
    }
    if (x >= 280 && x <= 432 && y >= 288 && y <= 350) {
      performBattleAction(BATTLE_SKILL2);
      return;
    }
    battleAttackMenuUntil = 0;
    battleDirty = true;
    sfxPlay(SFX_TAP);
    return;
  }
  if (x >= 46 && x <= 174 && y >= 344 && y <= 428) {
    battleAttackMenuUntil = 1;
    sfxPlay(SFX_TAP);
  } else if (x >= 169 && x <= 297 && y >= 344 && y <= 428) {
    performBattleAction(BATTLE_RECOVER);
  } else if (x >= 292 && x <= 420 && y >= 344 && y <= 428) {
    performBattleAction(BATTLE_RUN);
  }
}

void drawBattleHpBar(int x, int y, int w, uint16_t cur, uint16_t maxHp, uint16_t color) {
  if (maxHp == 0) maxHp = 1;
  int fw = (int)((uint32_t)cur * w / maxHp);
  if (fw > w) fw = w;
  gfx->fillRoundRect(x, y, w, 14, 4, UI_TRACK);
  if (fw > 2) gfx->fillRoundRect(x, y, fw, 14, 4, color);
}

void battleRewardText(char *buf, size_t len) {
  if (battleReward.amount == 0) { buf[0] = 0; return; }
  StrId fmt = S_SPD_GAIN_FMT;
  if (battleReward.stat == BATTLE_REWARD_ATK) fmt = S_ATK_GAIN_FMT;
  else if (battleReward.stat == BATTLE_REWARD_DEF) fmt = S_DEF_GAIN_FMT;
  snprintf(buf, len, T(fmt), battleReward.amount);
}

void drawBattleButtonLabel(int x, int y, int w, const char *label) {
  gfx->setTextSize(2);
  int px = gfx->textWidth(label);
  if (px <= w - 8) {
    gfx->setCursor(x + (w - px) / 2, y);
    gfx->print(label);
  } else {
    gfx->setTextSize(1);
    gfx->setCursor(x + (w - gfx->textWidth(label)) / 2, y + 3);
    gfx->print(label);
    gfx->setTextSize(2);
  }
}

const char *battleTypeName(uint8_t type) {
  return typeNameKo(type);
}

uint16_t battleTypeColor(uint8_t type) {
  switch (type) {
    case TYPE_FIRE: return 0xEA87;
    case TYPE_WATER: return 0x4C98;
    case TYPE_ELECTRIC: return 0xBCA1;
    case TYPE_GRASS: return 0x3C49;
    case TYPE_ICE: return 0x5D99;
    case TYPE_FIGHTING: return 0xA2A5;
    case TYPE_POISON: return 0x8A73;
    case TYPE_GROUND: return 0xB447;
    case TYPE_FLYING: return 0x8D7F;
    case TYPE_PSYCHIC: return 0xD28F;
    case TYPE_BUG: return 0x7CC4;
    case TYPE_ROCK: return 0x9407;
    case TYPE_GHOST: return 0x6B33;
    case TYPE_DRAGON: return 0x5A5F;
    case TYPE_DARK: return 0x5ACB;
    case TYPE_STEEL: return 0xA534;
    case TYPE_FAIRY: return 0xF3B7;
    default: return 0x8C4D;
  }
}

void typeText(char *buf, size_t len, const DexEntry &d) {
  if (d.type2 == TYPE_NONE) snprintf(buf, len, "%s", battleTypeName(d.type1));
  else snprintf(buf, len, "%s %s", battleTypeName(d.type1), battleTypeName(d.type2));
}

int typeChipWidth(uint8_t type) {
  gfx->setTextSize(1);
  return gfx->textWidth(battleTypeName(type)) + 14;
}

void drawTypeChip(int x, int y, uint8_t type) {
  if (type == TYPE_NONE) return;
  const char *label = battleTypeName(type);
  int w = typeChipWidth(type);
  gfx->fillRoundRect(x, y, w, 22, 5, lerp565(battleTypeColor(type), UI_WHITE, 5, 8));
  gfx->drawRoundRect(x, y, w, 22, 5, UI_INK);
  gfx->setTextSize(1);
  gfx->setTextColor(UI_INK);
  gfx->setCursor(x + 7, y + 3);
  gfx->print(label);
}

void drawTypeChips(int x, int y, const DexEntry &d, bool alignRight) {
  int w1 = typeChipWidth(d.type1);
  int w2 = d.type2 == TYPE_NONE ? 0 : typeChipWidth(d.type2);
  int total = w1 + (w2 ? 4 + w2 : 0);
  int sx = alignRight ? x - total : x;
  drawTypeChip(sx, y, d.type1);
  if (d.type2 != TYPE_NONE) drawTypeChip(sx + w1 + 4, y, d.type2);
  gfx->setTextSize(2);
}

void drawWildPrompt() {
  gfx->fillRoundRect(82, 156, 302, 178, 18, UI_WHITE);
  gfx->drawRoundRect(82, 156, 302, 178, 18, UI_INK);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(3);
  gfx->setCursor(CX - gfx->textWidth(T(S_WILD_Q)) / 2, 176);
  gfx->print(T(S_WILD_Q));
  char name[28];
  snprintf(name, sizeof(name), "%s Lv.%u", dexName(wildPromptDex), wildPromptLevel);
  gfx->setTextSize(2);
  gfx->setCursor(CX - gfx->textWidth(name) / 2, 206);
  gfx->print(name);

  gfx->fillRoundRect(93, 226, 280, 44, 12, UI_BAR_BAD);
  gfx->fillRoundRect(93, 278, 280, 44, 12, UI_TRACK);
  gfx->setTextColor(UI_INK);
  gfx->setCursor(CX - gfx->textWidth(T(S_FIGHT)) / 2, 240);
  gfx->print(T(S_FIGHT));
  gfx->setTextColor(UI_WHITE);
  gfx->setCursor(CX - gfx->textWidth(T(S_LATER)) / 2, 292);
  gfx->print(T(S_LATER));
}

void drawPetEvent() {
  uint32_t now = millis();
  if (petEventUntil && now > petEventUntil) petEventUntil = 0;
  if (!petEventUntil && (!petEventFeedbackUntil || now > petEventFeedbackUntil)) return;

  int16_t ex = 366, ey = 286;
  if (petEventUntil) {
    gfx->fillCircle(ex, ey, 32, UI_WHITE);
    gfx->drawCircle(ex, ey, 34, UI_BAR_WARN);
    if (petEventType == PET_EVENT_BERRY) {
      drawMap(SPR_ICON_BERRY_G, 16, ex - 24, ey - 24, 3, false);
    } else if (petEventType == PET_EVENT_HEART) {
      drawMap(SPR_HEART, 32, ex - 32, ey - 32, 2, false);
    } else {
      gfx->setTextColor(UI_BAR_WARN);
      gfx->setTextSize(4);
      gfx->setCursor(ex - 12, ey - 18);
      gfx->print("*");
      gfx->fillCircle(ex - 14, ey + 12, 4, UI_WHITE);
      gfx->fillCircle(ex + 16, ey + 10, 4, UI_WHITE);
    }
  }
  if (petEventFeedbackUntil && now <= petEventFeedbackUntil) {
    gfx->setTextColor(UI_BAR_WARN);
    gfx->setTextSize(2);
    gfx->setCursor(CX - gfx->textWidth(petEventMsg) / 2, 292);
    gfx->print(petEventMsg);
  }
}

uint16_t battleLerpHp(uint16_t from, uint16_t to, uint16_t progress) {
  if (progress > 1000) progress = 1000;
  if (to >= from) return from + (uint32_t)(to - from) * progress / 1000;
  return from - (uint32_t)(from - to) * progress / 1000;
}

uint16_t battleAnimationProgress(uint32_t now) {
  if (battleAnimStage == BATTLE_ANIM_NONE) return 1000;
  uint32_t duration = battleAnimationDuration(battleAnimStage);
  uint32_t elapsed = now - battleAnimStageAt;
  if (elapsed >= duration) return 1000;
  return (uint16_t)((uint64_t)elapsed * 1000ULL / duration);
}

uint16_t battleDisplayedPlayerHp(uint16_t progress) {
  switch (battleAnimStage) {
    case BATTLE_ANIM_PLAYER_ATTACK:
    case BATTLE_ANIM_ENEMY_REACT:
      return battleTurn.playerActedFirst ? battleAnimPlayerHpBefore : battleRun.playerHp;
    case BATTLE_ANIM_PLAYER_REST:
      return battleLerpHp(battleAnimPlayerHpBefore, battleAnimPlayerHpAfterRest, progress);
    case BATTLE_ANIM_ENEMY_ATTACK:
      return battleAnimPlayerHpAfterRest;
    case BATTLE_ANIM_PLAYER_REACT:
      return battleLerpHp(battleAnimPlayerHpAfterRest, battleRun.playerHp, progress);
    case BATTLE_ANIM_STATUS:
      return battleAnimPlayerHpBefore;
    default:
      return battleRun.playerHp;
  }
}

uint16_t battleDisplayedEnemyHp(uint16_t progress) {
  if (battleAnimStage == BATTLE_ANIM_PLAYER_ATTACK) return battleAnimEnemyHpBefore;
  if (battleAnimStage == BATTLE_ANIM_ENEMY_REACT)
    return battleLerpHp(battleAnimEnemyHpBefore, battleRun.enemyHp, progress);

  // When the opponent moves first, battleRun.enemyHp already contains the
  // damage that our later attack will inflict.  Showing that final value
  // during ENEMY_ATTACK/PLAYER_REACT made the attacking opponent's own HP bar
  // drop, refill when our attack began, and then drop a second time.  Keep the
  // opponent at its pre-turn HP until our attack and reaction stages actually
  // play.  Wild and communication battles share this animation path.
  if (!battleTurn.playerActedFirst &&
      (battleAnimStage == BATTLE_ANIM_ENEMY_ATTACK ||
       battleAnimStage == BATTLE_ANIM_PLAYER_REACT))
    return battleAnimEnemyHpBefore;

  return battleRun.enemyHp;
}

uint8_t battleSpriteAction(PmdMon &mon, uint8_t preferred, uint8_t fallback) {
  if (mon.has(preferred)) return preferred;
  if (mon.has(fallback)) return fallback;
  return PMD_IDLE;
}

uint32_t battleSpriteTime(PmdMon &mon, uint8_t action, uint32_t elapsed, uint32_t duration) {
  if (!mon.has(action) || duration == 0) return elapsed;
  return (uint32_t)((uint64_t)elapsed * pmdActTotalMs(mon.acts[action]) / duration);
}

void drawBattleBoldLine(int x0, int y0, int x1, int y1, uint16_t color, uint8_t width) {
  int half = width / 2;
  for (int offset = -half; offset <= half; offset++) {
    gfx->drawLine(x0 + offset, y0, x1 + offset, y1, color);
    gfx->drawLine(x0, y0 + offset, x1, y1 + offset, color);
  }
}

void drawBattleBoldCircle(int cx, int cy, int radius, uint16_t color, uint8_t width) {
  for (uint8_t i = 0; i < width && radius - i > 0; i++)
    gfx->drawCircle(cx, cy, radius - i, color);
}

void drawBattleStar(int cx, int cy, int radius, uint16_t color, uint8_t width) {
  int diagonal = radius * 2 / 3;
  drawBattleBoldLine(cx - radius, cy, cx + radius, cy, color, width);
  drawBattleBoldLine(cx, cy - radius, cx, cy + radius, color, width);
  drawBattleBoldLine(cx - diagonal, cy - diagonal, cx + diagonal, cy + diagonal,
                     color, width);
  drawBattleBoldLine(cx - diagonal, cy + diagonal, cx + diagonal, cy - diagonal,
                     color, width);
  gfx->fillCircle(cx, cy, width + 1, UI_WHITE);
}

int8_t captureBallTilt(uint32_t elapsed) {
  static const uint16_t starts[3] = { 420, 1190, 1960 };
  for (uint8_t i = 0; i < 3; i++) {
    if (elapsed < starts[i] || elapsed >= starts[i] + 520UL) continue;
    uint16_t local = (uint16_t)(elapsed - starts[i]);
    if (local < 110) return (int8_t)(-18 * (int)local / 110);
    if (local < 330) return (int8_t)(-18 + 36 * (int)(local - 110) / 220);
    return (int8_t)(18 - 18 * (int)(local - 330) / 190);
  }
  return 0;
}

void updateCaptureSounds(uint32_t elapsed) {
  // Una sola orden contiene los tres cierres y mantiene el amplificador activo.
  // Si la cola estuviera llena, se vuelve a intentar en el siguiente frame.
  if (battleCaptureSoundStep == 0) {
    if (sfxPlay(SFX_CAPTURE_SHAKE)) battleCaptureSoundStep = 1;
  }
  if (battleCaptureSoundStep == 1 && elapsed >= 2860UL) {
    if (sfxPlay(battleCatchSuccess ? SFX_CATCH_OK : SFX_CATCH_FAIL))
      battleCaptureSoundStep = 2;
  }
}

void drawCaptureSuccessEffect(uint32_t resultElapsed) {
  static const int8_t starX[8] = { -104, -75, 0, 82, 110, 90, -82, -118 };
  static const int8_t starY[8] = { -70, 70, -112, -82, 24, 86, 102, 16 };
  static const uint16_t colors[4] = { 0xFFE0, 0xFFFF, 0xFBE0, 0x07FF };
  for (uint8_t i = 0; i < 8; i++) {
    uint32_t delayAt = (uint32_t)i * 65UL;
    if (resultElapsed < delayAt) continue;
    uint16_t phase = (uint16_t)((resultElapsed - delayAt) / 70UL);
    uint8_t radius = 4 + (phase % 5);
    drawBattleStar(CX + starX[i], 216 + starY[i], radius,
                   colors[i & 3], radius >= 7 ? 3 : 2);
  }
  if (resultElapsed < 680UL) {
    int radius = 88 + (int)(resultElapsed * 54UL / 680UL);
    drawBattleBoldCircle(CX, 216, radius,
                         resultElapsed < 340UL ? 0xFFE0 : 0x7BEF, 2);
  }
}

void renderBattleCapture() {
  const uint32_t elapsed = millis() - battleCaptureStartedAt;
  updateCaptureSounds(elapsed);
  gfx->fillScreen(0x0000);

  const int ballX = CX - 80;
  const int ballY = 136;
  if (elapsed < 2860UL || battleCatchSuccess) {
    drawVisualAsset(gfx, "/extra/B01.tvr", ballX, ballY, 1, captureBallTilt(elapsed));
  } else {
    drawVisualAsset(gfx, "/extra/B02.tvr", ballX, ballY);
  }

  if (elapsed >= 2860UL) {
    uint32_t resultElapsed = elapsed - 2860UL;
    if (battleCatchSuccess) drawCaptureSuccessEffect(resultElapsed);
    const char *message = battleCatchSuccess ? T(S_CAUGHT_OK) : T(S_ESCAPED);
    gfx->setTextColor(battleCatchSuccess ? 0xFFE0 : UI_WHITE);
    gfx->setTextSize(3);
    gfx->setCursor(CX - gfx->textWidth(message) / 2, 340);
    gfx->print(message);
  }

  gfx->flush();
  uint32_t finishAt = battleCatchSuccess ? 6500UL : 4400UL;
  if (elapsed >= finishAt) {
    battleCaptureActive = false;
    battleDirty = true;
  }
}

// 공격 동작 중 기술의 속성과 방향을 보여 주는 이동 이펙트. 피격 단계의
// 폭발 효과와 별도로 표시하므로 무효 상성에서도 기술 자체는 보인다.
void drawBattleMoveTrail(int sx, int sy, int tx, int ty, uint16_t progress,
                         uint16_t color, uint8_t effect, bool heavy) {
  if (progress > 1000) progress = 1000;
  int px = sx + (int)((int32_t)(tx - sx) * progress / 1000);
  int py = sy + (int)((int32_t)(ty - sy) * progress / 1000);
  int dir = tx >= sx ? 1 : -1;
  int size = heavy ? 14 : 10;
  uint8_t bold = heavy ? 5 : 3;

  if (effect == MOVE_FX_ELECTRIC) {
    uint16_t yellow = 0xFFE0;
    int tail = heavy ? 94 : 70;
    int x0 = px - dir * tail;
    int x1 = x0 + dir * tail / 3;
    int x2 = x0 + dir * tail * 2 / 3;
    drawBattleBoldLine(x0, sy, x1, sy - 13, yellow, bold);
    drawBattleBoldLine(x1, sy - 13, x2, sy + 11, UI_WHITE, bold);
    drawBattleBoldLine(x2, sy + 11, px, py, yellow, bold);
    gfx->fillCircle(px, py, heavy ? 11 : 8, yellow);
    gfx->fillCircle(px, py, heavy ? 5 : 3, UI_WHITE);
    return;
  }
  if (effect == MOVE_FX_FIRE) {
    gfx->fillCircle(px, py, size, 0xF800);
    gfx->fillCircle(px - dir * 4, py + 2, size * 2 / 3, 0xFBE0);
    gfx->fillTriangle(px - dir * size, py,
                      px - dir * (size + 20), py - 9,
                      px - dir * (size + 13), py + 10, color);
    for (int i = 1; i <= 3; i++)
      gfx->fillCircle(px - dir * (size + i * 10), py + (i & 1 ? -8 : 8),
                      heavy ? 4 : 3, i == 2 ? 0xFBE0 : color);
    return;
  }
  if (effect == MOVE_FX_WATER) {
    uint16_t aqua = 0x5DDF;
    gfx->fillCircle(px, py, size, aqua);
    gfx->fillCircle(px - dir * 3, py - 3, heavy ? 5 : 3, UI_WHITE);
    gfx->fillTriangle(px - dir * size, py - size / 2,
                      px - dir * (size + 23), py,
                      px - dir * size, py + size / 2, aqua);
    for (int i = 1; i <= 3; i++) {
      int drop = heavy ? 5 : 4;
      gfx->fillCircle(px - dir * (size + i * 13), py + (i & 1 ? -10 : 10), drop, aqua);
      gfx->fillCircle(px - dir * (size + i * 13) - dir, py + (i & 1 ? -12 : 8), 2, UI_WHITE);
    }
    return;
  }
  if (effect == MOVE_FX_ICE) {
    gfx->fillTriangle(px + dir * size, py, px, py - size, px - dir * size, py, color);
    gfx->fillTriangle(px + dir * size, py, px, py + size, px - dir * size, py, UI_WHITE);
    for (int i = 1; i <= 3; i++)
      gfx->fillCircle(px - dir * (i * 15), py + (i & 1 ? -8 : 8), heavy ? 4 : 3, color);
    return;
  }
  if (effect == MOVE_FX_PSYCHIC) {
    gfx->fillCircle(px, py, size, color);
    drawBattleBoldCircle(px, py, size + 5, UI_WHITE, heavy ? 3 : 2);
    drawBattleBoldCircle(px, py, size + 12, color, heavy ? 3 : 2);
    drawBattleBoldCircle(px - dir * 26, py, heavy ? 8 : 6, color, 2);
    return;
  }
  if (effect == MOVE_FX_GHOST) {
    gfx->fillCircle(px, py - 2, size, color);
    gfx->fillTriangle(px - size, py, px - dir * (size + 24), py - 10,
                      px - dir * (size + 16), py + 12, color);
    gfx->fillCircle(px - dir * 4, py - 5, 3, UI_WHITE);
    gfx->fillCircle(px + dir * 4, py - 5, 3, UI_WHITE);
    drawBattleBoldCircle(px - dir * 28, py + 8, heavy ? 8 : 6, color, 2);
    return;
  }
  if (effect == MOVE_FX_DARK) {
    uint16_t shadow = 0x18E3;
    gfx->fillCircle(px, py, size, shadow);
    drawBattleBoldCircle(px, py, size + 4, color, heavy ? 4 : 3);
    for (int i = -1; i <= 1; i++)
      drawBattleBoldLine(px - dir * (heavy ? 68 : 50), py + i * 9,
                         px + dir * size, py + i * 4, i == 0 ? UI_WHITE : color, bold);
    return;
  }
  if (effect == MOVE_FX_DRAGON) {
    gfx->fillCircle(px, py, size, color);
    gfx->fillCircle(px + dir * 3, py, heavy ? 6 : 4, UI_WHITE);
    for (int i = -1; i <= 1; i++)
      gfx->fillTriangle(px - dir * size, py + i * 7,
                        px - dir * (heavy ? 58 : 42), py + i * 12 - 7,
                        px - dir * (heavy ? 50 : 36), py + i * 12 + 7,
                        i == 0 ? UI_WHITE : color);
    return;
  }
  if (effect == MOVE_FX_FAIRY) {
    drawBattleStar(px, py, heavy ? 17 : 13, color, heavy ? 4 : 3);
    drawBattleStar(px - dir * 27, py - 12, heavy ? 8 : 6, UI_WHITE, 2);
    drawBattleStar(px - dir * 46, py + 10, heavy ? 7 : 5, color, 2);
    return;
  }
  if (effect == MOVE_FX_GRASS) {
    drawBattleBoldLine(px - dir * 55, py, px, py, color, bold);
    for (int i = 0; i < 4; i++) {
      int lx = px - dir * (i * 15);
      int ly = py + (i & 1 ? -9 : 9);
      gfx->fillCircle(lx, ly, heavy ? 6 : 4, color);
      drawBattleBoldLine(lx, ly, lx + dir * 8, py, UI_WHITE, 2);
    }
    return;
  }
  if (effect == MOVE_FX_BUG) {
    gfx->fillCircle(px, py, size, color);
    gfx->fillTriangle(px, py, px - dir * (size + 16), py - size,
                      px - dir * (size + 5), py - 2, UI_WHITE);
    gfx->fillTriangle(px, py, px - dir * (size + 16), py + size,
                      px - dir * (size + 5), py + 2, UI_WHITE);
    for (int i = 1; i <= 4; i++)
      gfx->fillCircle(px - dir * (size + i * 11), py + (i & 1 ? -10 : 10),
                      heavy ? 5 : 3, color);
    return;
  }
  if (effect == MOVE_FX_FIGHTING) {
    gfx->fillCircle(px - dir * 5, py + 4, size, color);
    for (int i = -2; i <= 1; i++)
      gfx->fillCircle(px + dir * (size - 2), py + i * (heavy ? 7 : 6),
                      heavy ? 6 : 5, i == 0 ? UI_WHITE : color);
    drawBattleBoldLine(px - dir * (heavy ? 62 : 46), py, px - dir * size, py,
                       color, bold);
    return;
  }
  if (effect == MOVE_FX_POISON) {
    gfx->fillCircle(px, py, size, color);
    gfx->fillCircle(px - 4, py - 4, heavy ? 5 : 3, UI_WHITE);
    for (int i = 1; i <= 5; i++) {
      int bubble = (heavy ? 7 : 5) - i / 2;
      drawBattleBoldCircle(px - dir * (i * 13), py + (i & 1 ? -11 : 10),
                           bubble, color, 2);
    }
    return;
  }
  if (effect == MOVE_FX_GROUND) {
    drawBattleBoldLine(px - dir * (heavy ? 80 : 60), py + size,
                       px + dir * size, py + size, color, bold);
    for (int i = 0; i < 4; i++) {
      int rx = px - dir * (i * 17);
      int ry = py + size - (i & 1 ? 12 : 6);
      gfx->fillTriangle(rx - 7, ry + 7, rx, ry - 7, rx + 8, ry + 7,
                        i & 1 ? 0xA306 : color);
    }
    return;
  }
  if (effect == MOVE_FX_FLYING) {
    for (int i = -2; i <= 2; i++) {
      int y = py + i * 8;
      drawBattleBoldLine(px - dir * (heavy ? 76 : 56), y,
                         px + dir * size, y - dir * 5, i == 0 ? UI_WHITE : color,
                         heavy ? 4 : 3);
      gfx->fillCircle(px - dir * (22 + (i < 0 ? -i : i) * 9), y,
                      heavy ? 4 : 3, color);
    }
    return;
  }
  if (effect == MOVE_FX_ROCK) {
    gfx->fillTriangle(px + dir * size, py, px, py - size, px - dir * size, py, color);
    gfx->fillTriangle(px + dir * size, py, px, py + size, px - dir * size, py, 0xA306);
    for (int i = 1; i <= 3; i++) {
      int rx = px - dir * (i * 18);
      int ry = py + (i & 1 ? -9 : 9);
      gfx->fillTriangle(rx - 6, ry + 5, rx, ry - 6, rx + 7, ry + 5, color);
    }
    return;
  }
  if (effect == MOVE_FX_STEEL) {
    gfx->fillTriangle(px + dir * size, py, px, py - size, px - dir * size, py, 0xC618);
    gfx->fillTriangle(px + dir * size, py, px, py + size, px - dir * size, py, color);
    drawBattleStar(px, py, heavy ? 16 : 12, UI_WHITE, heavy ? 4 : 3);
    drawBattleBoldLine(px - dir * (heavy ? 66 : 48), py, px - dir * size, py,
                       0xC618, bold);
    return;
  }
  if (effect == MOVE_FX_NORMAL) {
    gfx->fillCircle(px, py, heavy ? 11 : 8, UI_WHITE);
    drawBattleBoldCircle(px, py, size + 6, color, heavy ? 4 : 3);
    drawBattleBoldCircle(px - dir * 26, py, heavy ? 9 : 6, UI_WHITE, 2);
    return;
  }

  // 물리·비행·강철 계열도 일반 공격보다 넓은 속도선을 남긴다.
  for (int i = -2; i <= 2; i++) {
    int y = py + i * 7;
    int length = (heavy ? 72 : 52) - (i < 0 ? -i : i) * 5;
    drawBattleBoldLine(px - dir * length, y, px + dir * size, y - dir * 3, color,
                       heavy ? 4 : 3);
  }
  gfx->fillCircle(px, py, heavy ? 9 : 6, UI_WHITE);
}

void drawBattleImpact(int cx, int cy, uint16_t progress, uint16_t color,
                      uint8_t effect, bool heavy) {
  uint16_t pulse = progress < 500 ? progress * 2 : (1000 - progress) * 2;
  bool skillEffect = effect != MOVE_FX_IMPACT;
  int radius = 14 + (int)((uint32_t)pulse *
               (skillEffect ? (heavy ? 58 : 46) : (heavy ? 40 : 30)) / 1000);
  uint8_t bold = skillEffect ? (heavy ? 5 : 4) : 3;
  if (effect == MOVE_FX_ELECTRIC) {
    uint16_t yellow = 0xFFE0;
    static const int8_t vx[8] = { 0, 7, 10, 7, 0, -7, -10, -7 };
    static const int8_t vy[8] = { -10, -7, 0, 7, 10, 7, 0, -7 };
    gfx->fillCircle(cx, cy, heavy ? 15 : 12, yellow);
    gfx->fillCircle(cx, cy, heavy ? 8 : 6, UI_WHITE);
    for (uint8_t i = 0; i < 8; i++) {
      int mx = cx + vx[i] * radius / 22 + vy[i] / 2;
      int my = cy + vy[i] * radius / 22 - vx[i] / 2;
      int ex = cx + vx[i] * radius / 10;
      int ey = cy + vy[i] * radius / 10;
      drawBattleBoldLine(cx, cy, mx, my, UI_WHITE, bold);
      drawBattleBoldLine(mx, my, ex, ey, yellow, bold);
      gfx->fillCircle(ex, ey, heavy ? 5 : 4, yellow);
    }
    return;
  }
  if (effect == MOVE_FX_SPEED) {
    for (int i = -3; i <= 3; i++) {
      int y = cy + i * 8;
      drawBattleBoldLine(cx - radius - 26 - i * 3, y, cx + radius, y - 4,
                         color, bold);
    }
    gfx->fillCircle(cx, cy, heavy ? 15 : 11, UI_WHITE);
    return;
  }
  if (effect == MOVE_FX_SLASH) {
    drawBattleBoldLine(cx - radius, cy + radius, cx + radius, cy - radius, UI_WHITE, bold);
    drawBattleBoldLine(cx - radius + 11, cy + radius, cx + radius + 11, cy - radius, color, bold);
    drawBattleBoldLine(cx - radius - 9, cy + radius - 14, cx + radius - 9,
                       cy - radius - 14, color, bold);
    return;
  }
  if (effect == MOVE_FX_STEEL) {
    drawBattleStar(cx, cy, radius, UI_WHITE, bold);
    gfx->fillTriangle(cx, cy - radius * 2 / 3, cx - radius * 2 / 3, cy,
                      cx, cy, 0xC618);
    gfx->fillTriangle(cx, cy - radius * 2 / 3, cx + radius * 2 / 3, cy,
                      cx, cy, color);
    gfx->fillTriangle(cx, cy + radius * 2 / 3, cx - radius * 2 / 3, cy,
                      cx, cy, color);
    gfx->fillTriangle(cx, cy + radius * 2 / 3, cx + radius * 2 / 3, cy,
                      cx, cy, 0xC618);
    drawBattleBoldCircle(cx, cy, radius, UI_WHITE, heavy ? 4 : 3);
    return;
  }
  if (effect == MOVE_FX_FIRE) {
    gfx->fillCircle(cx, cy + radius / 4, radius * 2 / 3, color);
    gfx->fillCircle(cx - radius / 3, cy, radius / 2, 0xFBE0);
    gfx->fillCircle(cx + radius / 3, cy - radius / 5, radius / 2, 0xF800);
    gfx->fillTriangle(cx - radius / 2, cy, cx - radius / 4, cy - radius,
                      cx, cy + radius / 5, 0xFBE0);
    gfx->fillTriangle(cx, cy, cx + radius / 3, cy - radius,
                      cx + radius / 2, cy + radius / 4, 0xF800);
    for (int i = -2; i <= 2; i++)
      gfx->fillCircle(cx + i * radius / 3, cy - radius / 2 - (i & 1 ? 8 : 0),
                      heavy ? 5 : 4, i & 1 ? 0xFBE0 : color);
    return;
  }
  if (effect == MOVE_FX_WATER) {
    uint16_t aqua = 0x5DDF;
    drawBattleBoldCircle(cx, cy, radius, aqua, bold);
    drawBattleBoldCircle(cx, cy, radius * 2 / 3, UI_WHITE, bold - 1);
    gfx->fillCircle(cx, cy, heavy ? 12 : 9, color);
    static const int8_t vx[8] = { 10, 7, 0, -7, -10, -7, 0, 7 };
    static const int8_t vy[8] = { 0, 7, 10, 7, 0, -7, -10, -7 };
    for (uint8_t i = 0; i < 8; i++) {
      int dx = vx[i] * radius / 10;
      int dy = vy[i] * radius / 10;
      gfx->fillCircle(cx + dx, cy + dy, heavy ? 7 : 5, aqua);
      gfx->fillCircle(cx + dx - 2, cy + dy - 2, 2, UI_WHITE);
    }
    return;
  }
  if (effect == MOVE_FX_ICE) {
    for (uint8_t i = 0; i < 8; i++) {
      int dx = (i == 0 || i == 4) ? radius : (i == 2 || i == 6 ? 0 : radius * 2 / 3);
      int dy = (i == 2 || i == 6) ? radius : (i == 0 || i == 4 ? 0 : radius * 2 / 3);
      if (i >= 4) dx = -dx;
      if (i >= 2 && i < 6) dy = -dy;
      drawBattleBoldLine(cx, cy, cx + dx, cy + dy, UI_WHITE, bold);
      gfx->fillTriangle(cx + dx, cy + dy - 6, cx + dx + 6, cy + dy,
                        cx + dx, cy + dy + 6, color);
    }
    return;
  }
  if (effect == MOVE_FX_GRASS) {
    for (uint8_t i = 0; i < 8; i++) {
      int dx = ((int)i - 4) * radius / 4;
      int dy = (i & 1) ? radius * 2 / 3 : -radius * 2 / 3;
      drawBattleBoldLine(cx, cy, cx + dx, cy + dy, color, bold);
      gfx->fillCircle(cx + dx, cy + dy, heavy ? 8 : 6, color);
      drawBattleBoldLine(cx + dx - 4, cy + dy, cx + dx + 4, cy + dy,
                         UI_WHITE, 2);
    }
    return;
  }
  if (effect == MOVE_FX_BUG) {
    drawBattleBoldCircle(cx, cy, radius / 2, color, bold);
    for (int i = -2; i <= 2; i++) {
      int y = cy + i * radius / 4;
      gfx->fillCircle(cx - radius * 2 / 3, y, heavy ? 7 : 5, color);
      gfx->fillCircle(cx + radius * 2 / 3, y, heavy ? 7 : 5, color);
      drawBattleBoldLine(cx - radius / 3, cy, cx - radius * 2 / 3, y,
                         UI_WHITE, 2);
      drawBattleBoldLine(cx + radius / 3, cy, cx + radius * 2 / 3, y,
                         UI_WHITE, 2);
    }
    gfx->fillTriangle(cx - radius / 4, cy, cx - radius, cy - radius,
                      cx - radius * 2 / 3, cy + radius / 4, color);
    gfx->fillTriangle(cx + radius / 4, cy, cx + radius, cy - radius,
                      cx + radius * 2 / 3, cy + radius / 4, color);
    return;
  }
  if (effect == MOVE_FX_PSYCHIC) {
    drawBattleBoldCircle(cx, cy, radius, color, bold);
    drawBattleBoldCircle(cx, cy, radius * 2 / 3, UI_WHITE, bold - 1);
    drawBattleBoldCircle(cx, cy, radius / 3, color, bold);
    gfx->fillCircle(cx, cy, heavy ? 9 : 6, UI_WHITE);
    return;
  }
  if (effect == MOVE_FX_GHOST) {
    gfx->fillCircle(cx, cy - radius / 6, radius * 2 / 3, color);
    gfx->fillTriangle(cx - radius * 2 / 3, cy,
                      cx - radius, cy + radius,
                      cx - radius / 5, cy + radius / 2, color);
    gfx->fillTriangle(cx + radius * 2 / 3, cy,
                      cx + radius, cy + radius,
                      cx + radius / 5, cy + radius / 2, color);
    gfx->fillCircle(cx - radius / 4, cy - radius / 5, heavy ? 5 : 4, UI_WHITE);
    gfx->fillCircle(cx + radius / 4, cy - radius / 5, heavy ? 5 : 4, UI_WHITE);
    for (int i = -1; i <= 1; i++)
      drawBattleBoldCircle(cx + i * radius, cy + radius / 2,
                           heavy ? 10 : 7, color, 2);
    return;
  }
  if (effect == MOVE_FX_DARK) {
    uint16_t shadow = 0x18E3;
    gfx->fillCircle(cx, cy, radius * 2 / 3, shadow);
    drawBattleBoldCircle(cx, cy, radius, color, bold);
    for (int i = -1; i <= 1; i++)
      drawBattleBoldLine(cx - radius, cy + i * 12 + radius / 2,
                         cx + radius, cy + i * 12 - radius / 2,
                         i == 0 ? UI_WHITE : color, bold);
    return;
  }
  if (effect == MOVE_FX_FIGHTING) {
    gfx->fillCircle(cx, cy + radius / 5, radius / 2, color);
    for (int i = -2; i <= 1; i++)
      gfx->fillCircle(cx + i * radius / 4, cy - radius / 2,
                      heavy ? 10 : 8, i == 0 ? UI_WHITE : color);
    drawBattleBoldCircle(cx, cy, radius, color, bold);
    drawBattleBoldLine(cx - radius, cy + radius, cx + radius, cy - radius,
                       UI_WHITE, bold);
    return;
  }
  if (effect == MOVE_FX_POISON) {
    gfx->fillCircle(cx, cy, radius / 2, color);
    for (uint8_t i = 0; i < 8; i++) {
      int dx = ((int)(i % 3) - 1) * radius * 2 / 3;
      int dy = ((int)i / 3 - 1) * radius * 2 / 3;
      int bubble = heavy ? 11 - (i & 1) * 2 : 8 - (i & 1) * 2;
      drawBattleBoldCircle(cx + dx, cy + dy, bubble, color, 3);
      gfx->fillCircle(cx + dx - 2, cy + dy - 2, 2, UI_WHITE);
    }
    return;
  }
  if (effect == MOVE_FX_GROUND) {
    drawBattleBoldLine(cx - radius, cy + radius / 2,
                       cx + radius, cy + radius / 2, color, bold);
    drawBattleBoldLine(cx, cy + radius / 2, cx - radius / 2, cy - radius,
                       0xA306, bold);
    drawBattleBoldLine(cx, cy + radius / 2, cx + radius / 2, cy - radius,
                       color, bold);
    drawBattleBoldLine(cx, cy + radius / 2, cx - radius, cy - radius / 4,
                       UI_WHITE, heavy ? 4 : 3);
    drawBattleBoldLine(cx, cy + radius / 2, cx + radius, cy - radius / 4,
                       UI_WHITE, heavy ? 4 : 3);
    for (int i = -2; i <= 2; i++)
      gfx->fillTriangle(cx + i * radius / 3 - 7, cy + radius / 2,
                        cx + i * radius / 3, cy + radius / 4 - (i & 1 ? 8 : 0),
                        cx + i * radius / 3 + 8, cy + radius / 2, color);
    return;
  }
  if (effect == MOVE_FX_FLYING) {
    for (int i = -2; i <= 2; i++) {
      int y = cy + i * radius / 4;
      drawBattleBoldLine(cx - radius, y, cx + radius, y - i * 4,
                         i == 0 ? UI_WHITE : color, bold);
      drawBattleBoldCircle(cx + (i & 1 ? radius / 2 : -radius / 2), y,
                           heavy ? 12 : 9, color, 2);
    }
    return;
  }
  if (effect == MOVE_FX_ROCK) {
    for (int i = 0; i < 7; i++) {
      int dx = ((i % 3) - 1) * radius * 2 / 3;
      int dy = ((i / 3) - 1) * radius * 2 / 3;
      int chunk = heavy ? 13 : 10;
      gfx->fillTriangle(cx + dx - chunk, cy + dy + chunk,
                        cx + dx, cy + dy - chunk,
                        cx + dx + chunk, cy + dy + chunk,
                        i & 1 ? 0xA306 : color);
      drawBattleBoldLine(cx + dx - chunk / 2, cy + dy,
                         cx + dx + chunk / 3, cy + dy + chunk / 2,
                         UI_WHITE, 2);
    }
    return;
  }
  if (effect == MOVE_FX_DRAGON) {
    drawBattleBoldCircle(cx, cy, radius, color, bold);
    gfx->fillCircle(cx, cy, radius / 3, color);
    gfx->fillCircle(cx, cy, heavy ? 8 : 6, UI_WHITE);
    for (int i = 0; i < 6; i++) {
      int dx = (i - 3) * radius / 3;
      int dy = (i & 1) ? radius : -radius;
      gfx->fillTriangle(cx + dx - 8, cy + dy,
                        cx + dx, cy + (i & 1 ? radius / 3 : -radius / 3),
                        cx + dx + 8, cy + dy, i & 1 ? UI_WHITE : color);
    }
    return;
  }
  if (effect == MOVE_FX_FAIRY) {
    drawBattleStar(cx, cy, radius, color, bold);
    drawBattleStar(cx - radius, cy - radius / 2, heavy ? 13 : 9, UI_WHITE, 2);
    drawBattleStar(cx + radius, cy + radius / 3, heavy ? 13 : 9, color, 2);
    drawBattleStar(cx + radius / 3, cy - radius, heavy ? 10 : 7, UI_WHITE, 2);
    return;
  }
  if (effect == MOVE_FX_NORMAL) {
    drawBattleBoldCircle(cx, cy, radius, color, bold);
    drawBattleBoldCircle(cx, cy, radius * 2 / 3, UI_WHITE, bold - 1);
    drawBattleStar(cx, cy, radius / 2, UI_WHITE, heavy ? 4 : 3);
    return;
  }
  drawBattleBoldCircle(cx, cy, radius, color, bold);
  static const int8_t vx[8] = { 10, 7, 0, -7, -10, -7, 0, 7 };
  static const int8_t vy[8] = { 0, 7, 10, 7, 0, -7, -10, -7 };
  for (uint8_t i = 0; i < 8; i++) {
    int x1 = cx + vx[i] * radius / 14;
    int y1 = cy + vy[i] * radius / 14;
    int x2 = cx + vx[i] * radius / 9;
    int y2 = cy + vy[i] * radius / 9;
    drawBattleBoldLine(x1, y1, x2, y2, color, bold);
    if ((i & 1) == 0) gfx->fillCircle(x2, y2, heavy ? 6 : 4, UI_WHITE);
  }
}

void drawBattlePokemon(uint32_t now, uint16_t progress) {
  uint32_t elapsed = battleAnimStage == BATTLE_ANIM_NONE ? now : now - battleAnimStageAt;
  uint32_t duration = battleAnimationDuration(battleAnimStage);
  int playerX = 142, enemyX = 328;
  uint8_t playerAct = PMD_IDLE, enemyAct = PMD_IDLE;
  bool playerLoop = true, enemyLoop = true;
  uint32_t playerTime = now, enemyTime = now;
  int arc = progress <= 500 ? progress * 2 : (1000 - progress) * 2;
  bool playerDefeated = battleResolved && !battleTurn.playerWon && !battleTurn.playerRan;
  bool enemyDefeated = battleResolved && battleTurn.playerWon && !battleTurn.playerRan;

  // 결과 화면은 전투 애니메이션이 끝나 BATTLE_ANIM_NONE이 되므로 기본
  // idle로 돌아가 버린다. 패배한 포켓몬은 확인 버튼을 누를 때까지
  // 피격(Hurt) 자세와 작은 떨림을 유지해 승리 때처럼 뛰지 않게 한다.
  if (playerDefeated) {
    playerAct = battleSpriteAction(pmd, PMD_HURT, PMD_SIT);
    playerX += ((now / 120UL) & 1U) ? 2 : -2;
    playerTime = now;
    playerLoop = true;
  }

  if (!playerDefeated && battleAnimStage == BATTLE_ANIM_PLAYER_ATTACK) {
    playerAct = battleSpriteAction(pmd, PMD_ATTACK_R, PMD_ATTACK);
    playerX += (int)((uint32_t)arc * (battleLastAction == BATTLE_SKILL2 ? 48 : 32) / 1000);
    playerTime = battleSpriteTime(pmd, playerAct, elapsed, duration);
    playerLoop = false;
  } else if (battleAnimStage == BATTLE_ANIM_ENEMY_REACT) {
    if (battleTurn.enemyDodged) {
      enemyAct = battleSpriteAction(wildPmd, PMD_WALKR, PMD_IDLE);
      enemyX += (int)((uint32_t)arc * 42 / 1000);
    } else {
      enemyAct = battleSpriteAction(wildPmd, PMD_HURT, PMD_IDLE);
      enemyX += (int)((uint32_t)arc * 22 / 1000) + ((elapsed / 60) & 1 ? 3 : -3);
    }
    enemyTime = battleSpriteTime(wildPmd, enemyAct, elapsed, duration);
    enemyLoop = false;
  } else if (battleAnimStage == BATTLE_ANIM_PLAYER_REST) {
    playerAct = battleSpriteAction(pmd, PMD_BREATH, PMD_SIT);
    playerTime = battleSpriteTime(pmd, playerAct, elapsed, duration);
    playerLoop = false;
  } else if (battleAnimStage == BATTLE_ANIM_ENEMY_ATTACK) {
    enemyAct = battleSpriteAction(wildPmd, PMD_ATTACK_L, PMD_ATTACK);
    enemyX -= (int)((uint32_t)arc * 38 / 1000);
    enemyTime = battleSpriteTime(wildPmd, enemyAct, elapsed, duration);
    enemyLoop = false;
  } else if (battleAnimStage == BATTLE_ANIM_PLAYER_REACT) {
    if (battleTurn.playerDodged) {
      playerAct = battleSpriteAction(pmd, PMD_WALKL, PMD_IDLE);
      playerX -= (int)((uint32_t)arc * 42 / 1000);
    } else {
      playerAct = battleSpriteAction(pmd, PMD_HURT, PMD_IDLE);
      playerX -= (int)((uint32_t)arc * 22 / 1000) + ((elapsed / 60) & 1 ? 3 : -3);
    }
    playerTime = battleSpriteTime(pmd, playerAct, elapsed, duration);
    playerLoop = false;
  }

  // 승리 결과에서도 상대 포켓몬이 idle로 돌아가지 않도록 내 포켓몬의
  // 패배 연출과 동일한 Hurt 자세와 떨림을 유지한다.
  if (enemyDefeated) {
    enemyAct = battleSpriteAction(wildPmd, PMD_HURT, PMD_SIT);
    enemyX += ((now / 120UL) & 1U) ? 2 : -2;
    enemyTime = now;
    enemyLoop = true;
  }

  if (pmd.loaded) drawPmdAct(playerAct, playerX, 286, playerTime, playerLoop, false, 3);
  else {
    const uint8_t *th = thumbs.get(pet.speciesId);
    if (th) drawThumb(th, 94, 166, 3, false);
  }
  if (wildPmd.loaded) drawPmdActM(wildPmd, enemyAct, enemyX, 286, enemyTime, enemyLoop, false, 3);
  else {
    const uint8_t *th = thumbs.get(battleDex);
    if (th) drawThumb(th, 280, 166, 3, false);
  }

  // Hurt 스프라이트만으로 표정이 잘 드러나지 않는 작은 포켓몬도
  // 패배 상태를 알아볼 수 있도록 천천히 떨어지는 눈물을 덧그린다.
  if (playerDefeated) {
    int tearY = 212 + (int)((now / 75UL) % 18UL);
    uint16_t tear = C565(0x80, 0xc8, 0xf0);
    gfx->fillRect(playerX + 20, tearY, 3, 6, tear);
    gfx->fillRect(playerX + 21, tearY + 6, 2, 2, tear);
  }
  if (enemyDefeated) {
    int tearY = 212 + (int)((now / 75UL) % 18UL);
    uint16_t tear = C565(0x80, 0xc8, 0xf0);
    gfx->fillRect(enemyX - 22, tearY, 3, 6, tear);
    gfx->fillRect(enemyX - 21, tearY + 6, 2, 2, tear);
  }

  if (battleAnimStage == BATTLE_ANIM_PLAYER_ATTACK &&
      (battleLastAction == BATTLE_SKILL1 || battleLastAction == BATTLE_SKILL2))
    drawBattleMoveTrail(playerX + 24, 226, enemyX - 24, 226, progress,
                        battleTypeColor(battleActionType(battlePlayer, battleLastAction)),
                        battleActionEffect(battlePlayer, battleLastAction),
                        battleLastAction == BATTLE_SKILL2);
  if (battleAnimStage == BATTLE_ANIM_ENEMY_ATTACK &&
      (battleCommEnemyAction == BATTLE_SKILL1 || battleCommEnemyAction == BATTLE_SKILL2))
    drawBattleMoveTrail(enemyX - 24, 226, playerX + 24, 226, progress,
                        battleTypeColor(battleActionType(battleEnemy, battleCommEnemyAction)),
                        battleActionEffect(battleEnemy, battleCommEnemyAction),
                        battleCommEnemyAction == BATTLE_SKILL2);

  if (battleAnimStage == BATTLE_ANIM_ENEMY_REACT && !battleTurn.enemyDodged && battleTurn.playerDamage > 0)
    drawBattleImpact(enemyX - 16, 228, progress,
                     battleTypeColor(battleActionType(battlePlayer, battleLastAction)),
                     battleActionEffect(battlePlayer, battleLastAction),
                     battleLastAction == BATTLE_SKILL2);
  if (battleAnimStage == BATTLE_ANIM_PLAYER_REACT && !battleTurn.playerDodged && battleTurn.enemyDamage > 0)
    drawBattleImpact(playerX + 16, 228, progress,
                     battleTypeColor(battleActionType(battleEnemy, battleCommEnemyAction)),
                     battleActionEffect(battleEnemy, battleCommEnemyAction),
                     battleCommEnemyAction == BATTLE_SKILL2);
}

void renderBattle() {
  if (battleCaptureActive) {
    renderBattleCapture();
    return;
  }
  if (battleResolved) battleDirty = false;
  drawGameScene();
  bool night = sceneHour() < 6 || sceneHour() >= 20;
  uint16_t ink = night ? UI_INK_NIGHT : UI_INK;
  const DexEntry &mine = DEX_TBL[pet.speciesId];
  const DexEntry &wild = DEX_TBL[battleDex];

  gfx->setTextColor(ink);
  gfx->setTextSize(3);
  const char *battleTitle = battleCommunication ? T(S_LINK_BATTLE) : T(S_WILD_BATTLE);
  gfx->setCursor(CX - gfx->textWidth(battleTitle) / 2, 34);
  gfx->print(battleTitle);

  const char *leftName = pet.nick[0] ? pet.nick : dexName(pet.speciesId);
  const char *rightName = dexName(battleDex);
  auto drawCenteredBattleName = [&](const char *name, int cx) {
    gfx->setTextSize(2);
    int size = gfx->textWidth(name) <= 142 ? 2 : 1;
    gfx->setTextSize(size);
    gfx->setCursor(cx - gfx->textWidth(name) / 2, size == 2 ? 78 : 84);
    gfx->print(name);
  };
  drawCenteredBattleName(leftName, 134);
  drawCenteredBattleName(rightName, 332);
  gfx->setTextSize(2);

  uint32_t now = millis();
  uint16_t animProgress = battleAnimationProgress(now);
  uint16_t playerMax = battleRun.playerMaxHp;
  uint16_t enemyMax = battleRun.enemyMaxHp;
  uint16_t playerCur = battleDisplayedPlayerHp(animProgress);
  uint16_t enemyCur = battleDisplayedEnemyHp(animProgress);
  drawBattleHpBar(58, 110, 146, playerCur, playerMax, UI_BAR_OK);
  drawBattleHpBar(262, 110, 146, enemyCur, enemyMax, UI_BAR_BAD);
  drawMap(SPR_ICON_PLAY, 16, CX - 16, 101, 2, false);

  char leftLv[10], rightLv[10];
  snprintf(leftLv, sizeof(leftLv), "Lv.%u", battlePlayer.level);
  snprintf(rightLv, sizeof(rightLv), "Lv.%u", battleLevel);
  gfx->setTextSize(1);
  gfx->setTextColor(ink);
  gfx->setCursor(58, 132);
  gfx->print(leftLv);
  drawTypeChips(112, 128, mine, false);
  drawTypeChips(262, 128, wild, false);
  gfx->setTextSize(1);
  gfx->setTextColor(ink);
  gfx->setCursor(408 - gfx->textWidth(rightLv), 132);
  gfx->print(rightLv);
  gfx->setTextSize(2);

  drawBattlePokemon(now, animProgress);

  if (battleResolved) {
    const char *res = battleTurn.playerRan ? T(S_RAN) : (battleTurn.playerWon ? T(S_WIN) : T(S_LOSS));
    gfx->setTextColor(battleTurn.playerRan ? UI_BAR_WARN : (battleTurn.playerWon ? UI_BAR_OK : UI_BAR_BAD));
    gfx->setTextSize(4);
    gfx->setCursor(CX - gfx->textWidth(res) / 2, 278);
    gfx->print(res);
    char rounds[20], dealt[32], received[32];
    snprintf(rounds, sizeof(rounds), T(S_ROUNDS_FMT), battleRun.round);
    snprintf(dealt, sizeof(dealt), "준 피해 %u", battleRun.playerDamageTotal);
    snprintf(received, sizeof(received), "받은 피해 %u", battleRun.enemyDamageTotal);
    gfx->setTextColor(ink);
    gfx->setTextSize(2);
    gfx->setCursor(CX - gfx->textWidth(rounds) / 2, 316);
    gfx->print(rounds);
    gfx->setCursor(CX - gfx->textWidth(dealt) / 2, 338);
    gfx->print(dealt);
    gfx->setCursor(CX - gfx->textWidth(received) / 2, 360);
    gfx->print(received);
    if (battleCommunication && commReward.item != EXP_ITEM_NONE) {
      char reward[34];
      snprintf(reward, sizeof(reward), "%s x%u",
               T(expeditionItemText(commReward.item)), commReward.amount);
      gfx->setTextColor(UI_BAR_WARN);
      gfx->setCursor(CX - gfx->textWidth(reward) / 2, 382);
      gfx->print(reward);
    } else if (battleMoveDiscFound) {
      gfx->setTextColor(UI_BAR_WARN);
      gfx->setCursor(CX - gfx->textWidth(T(S_MOVE_DISC)) / 2, 382);
      gfx->print(T(S_MOVE_DISC));
    } else if (battleTurn.playerWon) {
      char reward[20];
      battleRewardText(reward, sizeof(reward));
      if (reward[0]) {
        gfx->setTextColor(UI_BAR_WARN);
        gfx->setCursor(CX - gfx->textWidth(reward) / 2, 382);
        gfx->print(reward);
      }
    }
    if (battleTurn.playerWon && battleCatchOffered && !battleCatchDone) {
      gfx->fillRoundRect(76, 396, 148, 52, 14, UI_BAR_OK);
      gfx->fillRoundRect(242, 396, 148, 52, 14, UI_TRACK);
      gfx->setTextColor(UI_INK);
      drawBattleButtonLabel(76, 414, 148, T(S_CATCH_WILD));
      gfx->setTextColor(UI_WHITE);
      drawBattleButtonLabel(242, 414, 148, T(S_LEAVE_WILD));
    } else {
      if (battleCatchDone && battleCatchTried) {
        const char *catchMsg = battleCatchSuccess ? T(S_CAUGHT_OK) : T(S_ESCAPED);
        gfx->setTextColor(battleCatchSuccess ? UI_BAR_OK : UI_BAR_BAD);
        gfx->setTextSize(2);
        gfx->setCursor(CX - gfx->textWidth(catchMsg) / 2, 382);
        gfx->print(catchMsg);
      }
      gfx->fillRoundRect(118, 396, 230, 52, 14, UI_BAR_OK);
      gfx->setTextColor(UI_INK);
      gfx->setTextSize(3);
      gfx->setCursor(CX - gfx->textWidth(T(S_OK)) / 2, 413);
      gfx->print(T(S_OK));
    }
  } else {
    char roundBuf[14];
    snprintf(roundBuf, sizeof(roundBuf), "R%u", battleRun.round + 1);
    gfx->setTextColor(ink);
    gfx->setTextSize(2);
    gfx->setCursor(32, 318);
    gfx->print(roundBuf);
    if (battleMsg[0]) {
      gfx->setTextSize(gfx->textWidth(battleMsg) > 410 ? 1 : 2);
      gfx->setCursor(CX - gfx->textWidth(battleMsg) / 2, 318);
      gfx->print(battleMsg);
      gfx->setTextSize(2);
    }

    if (battleAnimStage == BATTLE_ANIM_NONE && battleAttackMenuUntil) {
      bool skill1Ready = battleActionUnlocked(battlePlayer, BATTLE_SKILL1) && battleRun.skill1UsesLeft > 0;
      bool skill2Ready = battleActionUnlocked(battlePlayer, BATTLE_SKILL2) && battleRun.skill2UsesLeft > 0;
      gfx->fillRoundRect(34, 294, 116, 50, 12, UI_BAR_BAD);
      gfx->fillRoundRect(157, 294, 116, 50, 12, skill1Ready ? UI_BAR_WARN : UI_TRACK);
      gfx->fillRoundRect(280, 294, 152, 50, 12, skill2Ready ? 0x4C98 : UI_TRACK);
      gfx->setTextColor(UI_INK);
      drawBattleButtonLabel(34, 303, 116, T(S_BASIC_ATTACK));
      char skill1[42], skill2[42];
      if (battlePlayer.level < 10) snprintf(skill1, sizeof(skill1), "Lv.10");
      else snprintf(skill1, sizeof(skill1), "%s %u", moveDef(battlePlayer.move1).name, battleRun.skill1UsesLeft);
      if (battlePlayer.level < 20) snprintf(skill2, sizeof(skill2), "Lv.20");
      else snprintf(skill2, sizeof(skill2), "%s %u", moveDef(battlePlayer.move2).name, battleRun.skill2UsesLeft);
      gfx->setTextColor(skill1Ready ? UI_INK : UI_WHITE);
      drawBattleButtonLabel(157, 303, 116, skill1);
      gfx->setTextColor(skill2Ready ? UI_INK : UI_WHITE);
      drawBattleButtonLabel(280, 303, 152, skill2);
    }

    bool animating = battleAnimStage != BATTLE_ANIM_NONE || battleCommWaiting;
    gfx->fillRoundRect(58, 358, 108, 58, 13, animating ? UI_TRACK : UI_BAR_BAD);
    gfx->fillRoundRect(179, 358, 108, 58, 13, animating ? UI_TRACK : 0x4C98);
    gfx->fillRoundRect(300, 358, 108, 58, 13, animating ? UI_TRACK : UI_BAR_OK);
    gfx->setTextColor(animating ? UI_WHITE : UI_INK);
    drawBattleButtonLabel(58, 380, 108, T(S_FIGHT));
    char recoverLabel[18];
    snprintf(recoverLabel, sizeof(recoverLabel), "%s %u", T(S_RECOVER), battleRun.restUsesLeft);
    drawBattleButtonLabel(179, 380, 108, recoverLabel);
    drawBattleButtonLabel(300, 380, 108, battleCommunication ? T(S_FORFEIT) : T(S_RUN_BATTLE));
  }

  gfx->flush();
}

// ---------- ficha del bicho (deslizar vertical) ----------

void drawCardStat(int y, const char *label, uint16_t val, uint16_t maxBar, uint16_t color) {
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(2);
  gfx->setCursor(96, y);
  gfx->print(label);
  char num[8];
  snprintf(num, sizeof(num), "%u", val);
  gfx->setCursor(330, y);
  gfx->print(num);
  int bw = 160;
  int fw = (int)val * bw / maxBar;
  if (fw > bw) fw = bw;
  gfx->fillRoundRect(150, y + 2, bw, 11, 3, UI_TRACK);
  if (fw > 2) gfx->fillRoundRect(150, y + 2, fw, 11, 3, color);
}

// ---------- 설정 / RTC 복구 / Wi-Fi 시간 보정 ----------

bool leapYear(int year) {
  return (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
}

uint8_t monthDays(int year, int month) {
  static const uint8_t DAYS[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (month < 1 || month > 12) return 0;
  return month == 2 && leapYear(year) ? 29 : DAYS[month - 1];
}

int64_t daysFromCivil(int year, unsigned month, unsigned day) {
  year -= month <= 2;
  const int era = (year >= 0 ? year : year - 399) / 400;
  const unsigned yoe = (unsigned)(year - era * 400);
  const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
  const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  return era * 146097LL + (int)doe - 719468LL;
}

bool makeWallEpoch(int year, int month, int day, int hour, int minute, uint32_t &out) {
  if (year < 2025 || year > 2099 || month < 1 || month > 12 || day < 1 ||
      day > monthDays(year, month) || hour < 0 || hour > 23 || minute < 0 || minute > 59) {
    return false;
  }
  const int64_t seconds = daysFromCivil(year, (unsigned)month, (unsigned)day) * 86400LL +
                          hour * 3600LL + minute * 60LL;
  if (seconds <= 0 || seconds > 0xFFFFFFFFLL) return false;
  out = (uint32_t)seconds;
  return true;
}

void civilFromDays(int64_t z, int &year, unsigned &month, unsigned &day) {
  z += 719468;
  const int era = (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe = (unsigned)(z - era * 146097);
  const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  year = (int)yoe + era * 400;
  const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  const unsigned mp = (5 * doy + 2) / 153;
  day = doy - (153 * mp + 2) / 5 + 1;
  month = mp + (mp < 10 ? 3 : -9);
  year += month <= 2;
}

void epochParts(uint32_t epoch, int &year, unsigned &month, unsigned &day,
                uint8_t &hour, uint8_t &minute) {
  civilFromDays(epoch / 86400UL, year, month, day);
  const uint32_t daytime = epoch % 86400UL;
  hour = daytime / 3600UL;
  minute = (daytime / 60UL) % 60UL;
}

void clearTimeEntryState() {
  wifiSelectedSsid[0] = 0;
  wifiSelectedSecure = false;
  wifiPassword[0] = 0;
  wifiPasswordLen = 0;
  wifiKeyMode = 1;
  wifiLastGroup = -1;
  wifiGroupCycle = 0;
  wifiAutoRecovery = false;
  manualTimeDigits[0] = 0;
  manualTimeLen = 0;
  timeStatusText[0] = 0;
}

void openTimeCorrection(TimeCorrectionReason reason) {
  wifiTimeStop();
  timeReason = reason;
  timeGuardAtOpen = pet.rtcGuardEpoch;
  timePanel = TIME_PANEL_CHOICE;
  clockOpen = true;
  clockDirty = true;
  clearTimeEntryState();
  // A true reset on the 1.75C clears its software wall clock.  Reuse the last
  // successful network automatically so this recovery is normally seamless.
  // First setup and a failed retry still expose the normal Wi-Fi/manual menu.
  if (reason == TIME_REASON_RTC_RECOVERY &&
      wifiTimeLoadCredentials(wifiSelectedSsid, sizeof(wifiSelectedSsid),
                              wifiPassword, sizeof(wifiPassword))) {
    wifiPasswordLen = strlen(wifiPassword);
    wifiSelectedSecure = wifiPasswordLen != 0;
    wifiAutoRecovery = true;
    wifiTimeStartConnect(wifiSelectedSsid, wifiPassword);
    timePanel = TIME_PANEL_CONNECTING;
  }
  lastInteract = millis();
  lockTouchBrief();
}

void openClock() {
  wifiTimeStop();
  timeReason = TIME_REASON_SETTINGS;
  timePanel = TIME_PANEL_SETTINGS;
  timeGuardAtOpen = pet.rtcGuardEpoch;
  clockOpen = true;
  clockDirty = true;
  clearTimeEntryState();
  lockTouchBrief();
  sfxPlay(SFX_MENU);
}

void startWifiApList() {
  wifiTimeStop();
  wifiAutoRecovery = false;
  timePanel = TIME_PANEL_AP_LIST;
  wifiApPage = 0;
  timeStatusText[0] = 0;
  wifiTimeStartScan();
  wifiNextScanAt = millis() + 10000UL;
  clockDirty = true;
}

void startManualTimeEntry() {
  wifiTimeStop();
  wifiAutoRecovery = false;
  timePanel = TIME_PANEL_MANUAL;
  manualTimeDigits[0] = 0;
  manualTimeLen = 0;
  timeStatusText[0] = 0;
  clockDirty = true;
}

void finishCorrectedTime(uint32_t epoch) {
  if (timeReason == TIME_REASON_RTC_RECOVERY && timeGuardAtOpen && epoch <= timeGuardAtOpen) {
    std::strncpy(timeStatusText, "저장된 시간 이후로 입력해 주세요.", sizeof(timeStatusText) - 1);
    timeStatusText[sizeof(timeStatusText) - 1] = 0;
    if (timePanel == TIME_PANEL_CONNECTING) {
      wifiTimeStop();
      timePanel = TIME_PANEL_CHOICE;
    }
    clockDirty = true;
    return;
  }
  rtcSetEpoch(epoch);
  if (!rtcEpoch()) {
    std::strncpy(timeStatusText, "기기에 시간을 저장하지 못했습니다.", sizeof(timeStatusText) - 1);
    timeStatusText[sizeof(timeStatusText) - 1] = 0;
    if (timePanel == TIME_PANEL_CONNECTING) {
      wifiTimeStop();
      timePanel = TIME_PANEL_CHOICE;
    }
    clockDirty = true;
    return;
  }

  if (timeReason == TIME_REASON_RTC_RECOVERY) pet.syncClock(epoch);
  else pet.setClock(epoch);

  pet.expireSitterIfNeeded(epoch);
  if (pet.initialClockPending()) {
    pet.confirmInitialClock();
    starterDirty = true;
  }

  wifiTimeStop();
  if (timeReason == TIME_REASON_SETTINGS) {
    timePanel = TIME_PANEL_SETTINGS;
    timeStatusText[0] = 0;
    clockDirty = true;
  } else {
    clockOpen = false;
    timePanel = TIME_PANEL_SETTINGS;
    resetTransientWakeNotices(millis());
  }

  // 완전 방전 복구 직후 위험 수치가 계산됐다면 화면을 확실히 켠 뒤
  // 최초 호출을 알린다. 이미 저장된 호출도 같은 경로로 복구한다.
  if (pet.tamagotchiModeEnabled() && pet.pendingCareNeed() == CARE_NEED_NONE)
    pet.startCareCallIfNeeded();
  if (pet.tamagotchiModeEnabled() && pet.pendingCareNeed() != CARE_NEED_NONE &&
      !pet.careCallMissed() && !careAlertShownThisWake) {
    careAlertShownThisWake = true;
    forceCareAlertScreenOn();
    if (pet.speciesId >= 1) careAlertSoundPlay();
  }
}

void updateTimeCorrection(uint32_t now) {
  if (!clockOpen) return;
  wifiTimeUpdate();

  static WifiTimeState previousState = WIFI_TIME_OFF;
  const WifiTimeState state = wifiTimeState();
  if (state != previousState) {
    previousState = state;
    clockDirty = true;
    if (timePanel == TIME_PANEL_AP_LIST && state == WIFI_TIME_READY)
      wifiNextScanAt = now + 10000UL;
  }

  if (timePanel == TIME_PANEL_AP_LIST &&
      (state == WIFI_TIME_READY || state == WIFI_TIME_ERROR) &&
      now >= wifiNextScanAt) {
    wifiTimeStartScan();
    wifiNextScanAt = now + 10000UL;
    clockDirty = true;
  }

  if (timePanel == TIME_PANEL_CONNECTING) {
    if (state == WIFI_TIME_SUCCESS) {
      wifiTimeRemember(wifiSelectedSsid, wifiPassword);
      finishCorrectedTime(wifiTimeEpoch());
    } else if (state == WIFI_TIME_ERROR) {
      std::strncpy(timeStatusText, wifiTimeError(), sizeof(timeStatusText) - 1);
      timeStatusText[sizeof(timeStatusText) - 1] = 0;
      wifiTimeStop();
      if (wifiAutoRecovery) {
        wifiAutoRecovery = false;
        timePanel = TIME_PANEL_CHOICE;
      } else {
        timePanel = wifiSelectedSecure ? TIME_PANEL_PASSWORD : TIME_PANEL_AP_LIST;
      }
      if (timePanel == TIME_PANEL_AP_LIST) {
        wifiTimeStartScan();
        wifiNextScanAt = now + 10000UL;
      }
      clockDirty = true;
    }
  }

  static uint32_t lastSettingsMinute = 0;
  if (timePanel == TIME_PANEL_SETTINGS) {
    const uint32_t minute = rtcEpoch() / 60UL;
    if (minute != lastSettingsMinute) {
      lastSettingsMinute = minute;
      clockDirty = true;
    }
  }
}

// pildoras de idioma centradas en y; rellena la activa
#define LANG_PILL_Y 276
#define LANG_PILL_H 30
#define LANG_PILL_X 336          // pildora de idioma (cicla los 6 al tocar)
#define LANG_PILL_W 96
#define SOUND_PILL_X 24
#define SOUND_PILL_W 116
#define PSAVE_PILL_X 150
#define PSAVE_PILL_W 176

const char *soundModeLabel() {
  switch (audioMode()) {
    case SOUND_FULL: return T(S_SND_FULL);
    case SOUND_MED: return T(S_SND_MED);
    case SOUND_LOW: return T(S_SND_LOW);
    default: return T(S_SND_OFF);
  }
}

uint8_t nextSoundMode() {
  switch (audioMode()) {
    case SOUND_FULL: return SOUND_MED;
    case SOUND_MED: return SOUND_LOW;
    case SOUND_LOW: return SOUND_OFF;
    default: return SOUND_FULL;
  }
}
const char *powerSaveLabel() {
  return powerSave ? T(S_PSAVE_ON) : T(S_PSAVE_OFF);
}

const char *careModeLabel() {
  return pet.tamagotchiModeEnabled() ? "다마고치" : "포케타마";
}

void drawStatusLine(int y, const char *label, const char *value, uint16_t valueColor) {
  gfx->setTextSize(1);
  gfx->setTextColor(UI_TRACK);
  gfx->setCursor(94, y);
  gfx->print(label);
  gfx->setTextColor(valueColor);
  gfx->setCursor(172, y);
  gfx->print(value);
}

#if 0  // 원본 다국어 도움말. 한글판은 generated/help_ko.h를 사용한다.
static const char *const HELP_WORD[LANG_COUNT] = { "AYUDA", "HELP", "AIDE", "HILFE", "AIUTO", "AJUDA" };
static const char *const HELP_OK[LANG_COUNT] = { "OK", "OK", "OK", "OK", "OK", "OK" };

static const char *const HELP_TITLES[LANG_COUNT][HELP_PAGE_COUNT] = {
  { "CUIDADO", "SUENO/ENERGIA", "MINIJUEGOS", "COMBATE 1", "COMBATE 2", "COLECCION", "EXTRAS", "EXPEDICION" },
  { "CARE", "SLEEP/ENERGY", "MINIGAMES", "BATTLE 1", "BATTLE 2", "COLLECTION", "EXTRAS", "EXPEDITION" },
  { "SOIN", "SOMMEIL/ENE", "MINI-JEUX", "COMBAT 1", "COMBAT 2", "COLLECTION", "EXTRAS", "EXPEDITION" },
  { "PFLEGE", "SCHLAF/ENERGIE", "MINISPIELE", "KAMPF 1", "KAMPF 2", "SAMMLUNG", "EXTRAS", "EXPEDITION" },
  { "CURA", "SONNO/ENERGIA", "MINIGIOCHI", "LOTTA 1", "LOTTA 2", "COLLEZIONE", "EXTRA", "SPEDIZIONE" },
  { "CUIDADO", "SONO/ENERGIA", "MINIJOGOS", "BATALHA 1", "BATALHA 2", "COLECAO", "EXTRAS", "EXPEDICAO" },
};

static const char *const HELP_LINES[LANG_COUNT][HELP_PAGE_COUNT][HELP_LINE_COUNT] = {
  {
    { "Comida baja = descuido.", "Jugar sube alegria.", "Bano limpia suciedad.", "Tocar da alegria/vinc.", "Peso alto te frena.", "Dulce alegra, engorda." },
    { "Dormir recupera energia.", "Durmiendo todo baja lento.", "Luz despierta o duerme.", "PWR corto apaga pantalla.", "Ahorro usa light sleep.", "Sin borrar conserva save." },
    { "Bola: toca la bola.", "Atrapa: toca iconos.", "Memo: repite secuencia.", "Limpia: toca manchas.", "Tipo: elige ventaja.", "Dan records y entreno." },
    { "Rapido: menos dano.", "Rival esquiva poco.", "Recibes algo menos dano.", "Fuerte: mas dano.", "Riesgo y contra mayor.", "No siempre conviene." },
    { "Esquivar evita dano.", "Si sale: Contra listo.", "Prox ataque pega mas.", "Ruhe/Descanso cura 2x.", "Tambien da Guardia.", "Tipos suben/bajan dano." },
    { "Pokedex: desliza lado.", "Criado y atrapado cuentan.", "10/25/50/100/151: marcos.", "Perfil: elige marco.", "Detalle conocido: chirp.", "SON TODO: toca pet." },
    { "Diario da metas diarias.", "Eventos salen raros.", "Batallas salvajes opc.", "Captura tras ganar.", "Rachas y medallas quedan.", "Sonido se ajusta abajo." },
    { "Expedicion: 15/30/60 min.", "Cuesta energia al salir.", "El bicho sigue disponible.", "Buen cuidado mejora premio.", "Recoge 1 objeto al volver.", "Objetos max. x15." },
  },
  {
    { "Low food = slip-up.", "Play raises joy.", "Bath cleans dirt.", "Petting gives joy/bond.", "High weight slows you.", "Candy cheers but fattens." },
    { "Sleep restores energy.", "Needs decay slower asleep.", "Light toggles sleep.", "Short PWR screen off.", "Power Save light-sleeps.", "No erase keeps saves." },
    { "Ball: tap the ball.", "Catch: tap icons.", "Memo: repeat sequence.", "Clean: tap stains.", "Type: pick advantage.", "Records and training." },
    { "Quick: lower damage.", "Enemy dodges less.", "You take less damage.", "Heavy: more damage.", "More risk/counterplay.", "Not always best." },
    { "Dodge avoids damage.", "Success: Counter ready.", "Next attack hits harder.", "Rest heals only 2x.", "Rest also gives Guard.", "Types change damage." },
    { "Pokedex: side swipe.", "Raised and caught count.", "10/25/50/100/151: frames.", "Profile: choose frame.", "Known detail: species chirp.", "SND ALL: tap pet." },
    { "Daily gives small goals.", "Events appear rarely.", "Wild battles are optional.", "Catch after winning.", "Streaks/medals persist.", "Sound is in settings." },
    { "Expedition: 15/30/60 min.", "Energy is spent at start.", "Pet stays available.", "Care and bond improve finds.", "Claim 1 item when back.", "Items hold max x15." },
  },
  {
    { "Faim basse = erreur.", "Jouer monte la joie.", "Bain nettoie.", "Caresse donne lien/joie.", "Poids haut ralentit.", "Bonbon rend gros." },
    { "Sommeil rend energie.", "Besoins baissent moins.", "Lumiere dort/reveille.", "PWR court eteint ecran.", "Eco utilise light sleep.", "Sans erase garde save." },
    { "Balle: touche la balle.", "Attrape: touche icones.", "Memo: repete sequence.", "Nettoie: touche taches.", "Type: choisis avantage.", "Records et entrainement." },
    { "Rapide: degats bas.", "Ennemi esquive moins.", "Tu subis moins.", "Fort: degats hauts.", "Risque plus grand.", "Pas toujours meilleur." },
    { "Esquive evite degats.", "Succes: Contre pret.", "Prochaine attaque plus.", "Repos soigne 2 fois.", "Repos donne Garde.", "Types changent degats." },
    { "Pokedex: glisse cote.", "Eleve et capture comptent.", "10/25/50/100/151: cadres.", "Profil: choisis cadre.", "Detail connu: chirp.", "SON TOUT: touche pet." },
    { "Quotidien donne buts.", "Events rares.", "Combats sauvages option.", "Capture apres victoire.", "Series/medailles restent.", "Son dans reglages." },
    { "Expedition: 15/30/60 min.", "Energie payee au depart.", "Le pet reste disponible.", "Soin/lien aide le butin.", "Prends 1 objet au retour.", "Objets max x15." },
  },
  {
    { "Food 0 = Patzer.", "Spielen hebt Freude.", "Bad reinigt Hygiene.", "Streicheln gibt Bond.", "Hohes Gewicht bremst.", "Candy freut, macht dick." },
    { "Schlaf gibt Energie.", "Needs sinken langsamer.", "Licht: schlafen/wach.", "PWR kurz: Screen aus.", "Sparen nutzt Light Sleep.", "Ohne Erase bleibt Save." },
    { "Ball: Ball antippen.", "Fangen: Icons treffen.", "Memo: Folge merken.", "Putzen: Flecken tippen.", "Typ: Vorteil waehlen.", "Gibt Rekorde/Training." },
    { "Schnell: weniger Schaden.", "Gegner weicht selten aus.", "Du kassierst weniger.", "Stark: mehr Schaden.", "Mehr Risiko/Gegendruck.", "Nicht immer beste Wahl." },
    { "Ausweichen meidet Schaden.", "Klappt es: Konter bereit.", "Naechster Angriff staerker.", "Ruhen heilt nur 2x.", "Ruhen gibt auch Schutz.", "Typen aendern Schaden." },
    { "Pokedex: seitlich wischen.", "Aufz./gefangen zaehlen.", "10/25/50/100/151: Rahmen.", "Profil: Rahmen waehlen.", "Bekanntes Detail: Chirp.", "TON VIEL: Pet tippen." },
    { "Taeglich gibt Ziele.", "Events sind selten.", "Wildkampf ist optional.", "Fangen nach Sieg.", "Serien/Medaillen bleiben.", "Ton unten einstellen." },
    { "Expedition: 15/30/60 Min.", "Kostet beim Start Energie.", "Pet bleibt verfuegbar.", "Pflege/Bond verbessert Fund.", "Fund danach einsammeln.", "Items maximal x15." },
  },
  {
    { "Cibo 0 = errore.", "Gioca aumenta gioia.", "Bagno pulisce.", "Carezza da legame.", "Peso alto rallenta.", "Dolce rallegra, ingrassa." },
    { "Sonno da energia.", "Bisogni calano meno.", "Luce dorme/sveglia.", "PWR corto spegne schermo.", "Risparmio usa light sleep.", "Senza erase salva." },
    { "Palla: tocca palla.", "Prendi: tocca icone.", "Memo: ripeti sequenza.", "Pulisci: tocca macchie.", "Tipo: scegli vantaggio.", "Record e allenamento." },
    { "Rapido: meno danni.", "Nemico schiva meno.", "Subisci meno danni.", "Forte: piu danni.", "Piu rischio.", "Non sempre migliore." },
    { "Schiva evita danni.", "Successo: contro pronto.", "Prox attacco piu forte.", "Riposo cura solo 2x.", "Riposo da Guardia.", "Tipi cambiano danni." },
    { "Pokedex: scorri lato.", "Allevato e preso contano.", "10/25/50/100/151: cornici.", "Profilo: scegli cornice.", "Dettaglio noto: chirp.", "SON TUTTO: tocca pet." },
    { "Quotidiano da obiettivi.", "Eventi rari.", "Lotte selvatiche opz.", "Cattura dopo vittoria.", "Serie/medaglie restano.", "Audio nei settaggi." },
    { "Spedizione: 15/30/60 min.", "Energia spesa alla partenza.", "Il pet resta disponibile.", "Cura/legame migliora premio.", "Ritira 1 oggetto al ritorno.", "Oggetti max x15." },
  },
  {
    { "Comida 0 = falha.", "Jogar sobe alegria.", "Banho limpa.", "Carinho da vinculo.", "Peso alto atrasa.", "Doce alegra, engorda." },
    { "Sono da energia.", "Necessidades caem menos.", "Luz dorme/acorda.", "PWR curto apaga tela.", "Poupanca usa light sleep.", "Sem erase guarda save." },
    { "Bola: toque na bola.", "Pegar: toque icones.", "Memo: repita sequencia.", "Limpa: toque manchas.", "Tipo: escolha vantagem.", "Recordes e treino." },
    { "Rapido: dano menor.", "Rival desvia menos.", "Voce recebe menos.", "Forte: dano maior.", "Mais risco.", "Nem sempre melhor." },
    { "Desviar evita dano.", "Sucesso: contra pronto.", "Prox ataque mais forte.", "Descanso cura so 2x.", "Descanso da Guarda.", "Tipos mudam dano." },
    { "Pokedex: deslize lado.", "Criado e apanhado contam.", "10/25/50/100/151: molduras.", "Perfil: escolha moldura.", "Detalhe conhecido: chirp.", "SOM TODO: toque pet." },
    { "Diario da metas.", "Eventos sao raros.", "Batalha selvagem opc.", "Captura apos vitoria.", "Series/medalhas ficam.", "Som nos ajustes." },
    { "Expedicao: 15/30/60 min.", "Energia gasta ao sair.", "Pet fica disponivel.", "Cuidado/laco melhora premio.", "Recolhe 1 item ao voltar.", "Itens max x15." },
  },
};
#endif

void renderHelp() {
  helpDirty = false;
  gfx->fillScreen(UI_BG_DAY);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(3);
  const char *h = helpWordKo();
  gfx->setCursor(CX - gfx->textWidth(h) / 2, 42);
  gfx->print(h);

  const int scale = 7;
  const int quiet = 4;
  const int qrPixels = (MANUAL_QR_SIZE + quiet * 2) * scale;
  const int qrX = CX - qrPixels / 2;
  const int qrY = 92;
  gfx->fillRect(qrX, qrY, qrPixels, qrPixels, UI_WHITE);
  for (uint8_t row = 0; row < MANUAL_QR_SIZE; row++) {
    for (uint8_t col = 0; col < MANUAL_QR_SIZE; col++) {
      uint16_t bit = (uint16_t)row * MANUAL_QR_SIZE + col;
      if (MANUAL_QR_BITS[bit >> 3] & (0x80 >> (bit & 7))) {
        gfx->fillRect(qrX + (col + quiet) * scale, qrY + (row + quiet) * scale,
                      scale, scale, UI_INK);
      }
    }
  }

  gfx->setTextSize(2);
  gfx->setTextColor(UI_INK);
  const char *scan = "스마트폰으로 QR 코드를 스캔하세요";
  gfx->setCursor(CX - gfx->textWidth(scan) / 2, 362);
  gfx->print(scan);
  gfx->fillRoundRect(178, 402, 110, 40, 12, UI_BAR_OK);
  gfx->setTextColor(UI_INK);
  const char *ok = helpOkKo();
  gfx->setCursor(178 + (110 - gfx->textWidth(ok)) / 2, 414);
  gfx->print(ok);
  gfx->flush();
}

void openHelp() {
  helpPage = 0;
  helpOpen = true;
  clockOpen = false;
  helpDirty = true;
  lockTouchBrief();
  sfxPlay(SFX_MENU);
}

void helpTap(int16_t x, int16_t y) {
  if (x >= 178 && x <= 288 && y >= 398 && y <= 448) {
    helpOpen = false;
    clockOpen = true;
    timePanel = TIME_PANEL_SETTINGS;
    clockDirty = true;
    lockTouchBrief();
    sfxPlay(SFX_TAP);
  }
}

void drawResetChoiceButton(int x, const char *label, bool destructive) {
  uint16_t fill = destructive ? UI_BAR_BAD : UI_WHITE;
  gfx->fillRoundRect(x, 294, 130, 54, 14, fill);
  gfx->drawRoundRect(x, 294, 130, 54, 14, destructive ? UI_BAR_BAD : UI_INK);
  gfx->setTextSize(2);
  gfx->setTextColor(UI_INK);
  gfx->setCursor(x + (130 - gfx->textWidth(label)) / 2, 313);
  gfx->print(label);
}

void renderResetScreen() {
  gfx->fillScreen(UI_BG_DAY);
  gfx->setTextColor(UI_BAR_BAD);
  gfx->setTextSize(3);
  const char *title = T(S_RESET_DATA);
  gfx->setCursor(CX - gfx->textWidth(title) / 2, 48);
  gfx->print(title);

  if (resetStage == RESET_HOLD_REQUIRED) {
    uint32_t held = resetHolding ? millis() - resetHoldStartedAt : 0;
    if (held > 3000UL) held = 3000UL;
    gfx->fillRoundRect(108, 166, 250, 104, 18, resetHolding ? UI_BAR_BAD : UI_WHITE);
    gfx->drawRoundRect(108, 166, 250, 104, 18, UI_BAR_BAD);
    if (resetHolding && held > 0) {
      int width = (int)(230UL * held / 3000UL);
      gfx->fillRoundRect(118, 250, width, 8, 4, UI_BAR_WARN);
    }
    gfx->setTextColor(resetHolding ? UI_WHITE : UI_BAR_BAD);
    gfx->setTextSize(4);
    const char *hold = T(S_RESET_HOLD);
    gfx->setCursor(CX - gfx->textWidth(hold) / 2, 197);
    gfx->print(hold);
    gfx->setTextColor(UI_INK);
    gfx->setTextSize(2);
    const char *hint = T(S_RESET_HOLD_HINT);
    gfx->setCursor(CX - gfx->textWidth(hint) / 2, 300);
    gfx->print(hint);
    gfx->fillRoundRect(178, 362, 110, 42, 12, UI_WHITE);
    gfx->drawRoundRect(178, 362, 110, 42, 12, UI_INK);
    const char *cancel = T(S_RESET_CANCEL);
    gfx->setCursor(CX - gfx->textWidth(cancel) / 2, 375);
    gfx->print(cancel);
    return;
  }

  if (resetStage == RESET_IN_PROGRESS) {
    gfx->setTextColor(UI_BAR_BAD);
    gfx->setTextSize(3);
    const char *resetting = T(S_RESETTING);
    gfx->setCursor(CX - gfx->textWidth(resetting) / 2, 208);
    gfx->print(resetting);
    return;
  }

  const char *warning = resetStage == RESET_CONFIRM_FIRST ? T(S_RESET_WARN1) : T(S_RESET_WARN2);
  const char *question = resetStage == RESET_CONFIRM_FIRST ? T(S_RESET_Q1) : T(S_RESET_Q2);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(2);
  gfx->setCursor(CX - gfx->textWidth(warning) / 2, 132);
  gfx->print(warning);
  gfx->setTextSize(3);
  gfx->setCursor(CX - gfx->textWidth(question) / 2, 190);
  gfx->print(question);
  drawResetChoiceButton(88, T(S_RESET_CANCEL), false);
  drawResetChoiceButton(248, T(S_RESET_CONFIRM), true);
}

void drawTimeButton(int x, int y, int w, int h, const char *label,
                    uint16_t fill, uint16_t ink) {
  gfx->fillRoundRect(x, y, w, h, 11, fill);
  gfx->drawRoundRect(x, y, w, h, 11, ink);
  gfx->setTextColor(ink);
  gfx->setTextSize(2);
  gfx->setCursor(x + (w - gfx->textWidth(label)) / 2, y + (h - 16) / 2);
  gfx->print(label);
}

void drawTimeButton(int x, int y, int w, int h, const char *label, uint16_t fill) {
  drawTimeButton(x, y, w, h, label, fill, UI_INK);
}

void drawTimeButton(int x, int y, int w, int h, const char *label) {
  drawTimeButton(x, y, w, h, label, UI_WHITE, UI_INK);
}

void drawSettingsBattery() {
  // First row of the settings screen, centred above date and time.
  const int x = 171, y = 18, w = 116, h = 28;
  const int innerX = x + 4, innerY = y + 4;
  const int innerW = w - 8, innerH = h - 8;
  int percent = batPercent();
  if (percent > 100) percent = 100;

  // Smartphone-style battery body and terminal.
  gfx->fillRoundRect(x, y, w, h, 7, UI_WHITE);
  gfx->drawRoundRect(x, y, w, h, 7, UI_INK);
  gfx->fillRoundRect(x + w, y + 8, 8, 12, 3, UI_INK);

  if (percent >= 0) {
    const uint16_t levelColor = percent <= 15 ? UI_BAR_BAD
                                : percent <= 35 ? UI_BAR_WARN
                                                : UI_BAR_OK;
    const int levelW = innerW * percent / 100;
    if (levelW > 0)
      gfx->fillRoundRect(innerX, innerY, levelW, innerH, 4, levelColor);
  }

  // Charging uses a small vector lightning bolt, so no extra font glyph is
  // needed and it remains readable over every charge-level colour.
  if (batCharging()) {
    gfx->fillTriangle(x + 18, y + 3, x + 9, y + 15, x + 17, y + 15, UI_INK);
    gfx->fillTriangle(x + 15, y + 12, x + 23, y + 12, x + 12, y + 25, UI_INK);
  }

  char batteryText[10];
  if (percent >= 0) snprintf(batteryText, sizeof(batteryText), "%d%%", percent);
  else snprintf(batteryText, sizeof(batteryText), "%s", usbPresent() ? "USB" : "--%");
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(2);
  gfx->setCursor(x + (w - gfx->textWidth(batteryText)) / 2, y + 6);
  gfx->print(batteryText);
}

void renderTimeChoice() {
  gfx->fillScreen(UI_BG_DAY);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(3);
  const char *title = "시간 보정";
  gfx->setCursor(CX - gfx->textWidth(title) / 2, 54);
  gfx->print(title);
  gfx->setTextSize(2);
  const char *line1 = "지금 시간을 보정하려면";
  const char *line2 = "Wi-Fi 연결이 필요해요.";
  gfx->setCursor(CX - gfx->textWidth(line1) / 2, 132);
  gfx->print(line1);
  gfx->setCursor(CX - gfx->textWidth(line2) / 2, 164);
  gfx->print(line2);
  drawTimeButton(78, 238, 310, 54, "Wi-Fi 연결", UI_BAR_OK, UI_INK);
  drawTimeButton(78, 312, 310, 54, "일자·시간 수동 입력");
  if (timeStatusText[0]) {
    gfx->setTextColor(UI_BAR_BAD);
    gfx->setTextSize(1);
    gfx->setCursor(CX - gfx->textWidth(timeStatusText) / 2, 390);
    gfx->print(timeStatusText);
  }
}

void renderWifiApList() {
  gfx->fillScreen(UI_BG_DAY);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(3);
  const char *title = "Wi-Fi 선택";
  gfx->setCursor(CX - gfx->textWidth(title) / 2, 25);
  gfx->print(title);
  gfx->setTextSize(1);
  gfx->setTextColor(UI_INK);
  const WifiTimeState state = wifiTimeState();
  const char *status = state == WIFI_TIME_SCANNING ? "주변 네트워크 검색 중..." :
                       state == WIFI_TIME_ERROR ? wifiTimeError() : "10초마다 목록을 새로 확인해요.";
  gfx->setCursor(CX - gfx->textWidth(status) / 2, 62);
  gfx->print(status);

  const uint8_t count = wifiTimeApCount();
  const uint8_t first = wifiApPage * 5;
  for (uint8_t row = 0; row < 5; ++row) {
    const uint8_t index = first + row;
    if (index >= count) break;
    const WifiTimeAp *ap = wifiTimeAp(index);
    const int y = 86 + row * 58;
    gfx->fillRoundRect(54, y, 358, 48, 10, UI_WHITE);
    gfx->drawRoundRect(54, y, 358, 48, 10, UI_INK);
    gfx->setTextColor(UI_INK);
    gfx->setTextSize(2);
    gfx->setCursor(70, y + 9);
    gfx->print(ap->ssid);
    char detail[24];
    snprintf(detail, sizeof(detail), "%s %lddBm", ap->secure ? "잠금" : "개방", (long)ap->rssi);
    gfx->setTextSize(1);
    gfx->setTextColor(UI_INK);
    gfx->setCursor(298, y + 28);
    gfx->print(detail);
  }
  if (count == 0 && state != WIFI_TIME_SCANNING) {
    gfx->setTextColor(UI_INK);
    gfx->setTextSize(2);
    const char *none = "검색된 Wi-Fi가 없습니다.";
    gfx->setCursor(CX - gfx->textWidth(none) / 2, 220);
    gfx->print(none);
  }
  drawTimeButton(60, 402, 100, 38, "이전");
  drawTimeButton(173, 402, 120, 38, "돌아가기");
  drawTimeButton(306, 402, 100, 38, "다음");
}

const char *wifiKeyLabel(uint8_t mode, uint8_t key) {
  static const char *UPPER[9] = {"ABC", "DEF", "GHI", "JKL", "MNO", "PQRS", "TUV", "WXYZ", ".-_"};
  static const char *LOWER[9] = {"abc", "def", "ghi", "jkl", "mno", "pqrs", "tuv", "wxyz", ".-_"};
  static const char *SYMBOL[9] = {"!@#", "$%^", "&*(", ")_-", "+=~", "[]{}", ";:'", "\"/\\", ".,?"};
  static const char *NUMBER[9] = {"1", "2", "3", "4", "5", "6", "7", "8", "9"};
  if (key < 9) {
    if (mode == 0) return NUMBER[key];
    if (mode == 1) return UPPER[key];
    if (mode == 2) return LOWER[key];
    return SYMBOL[key];
  }
  if (key == 9) return "SP";
  if (key == 10) return mode == 0 ? "0" : "";
  return "<-";
}

void renderWifiPassword() {
  gfx->fillScreen(UI_BG_DAY);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(2);
  gfx->setCursor(CX - gfx->textWidth(wifiSelectedSsid) / 2, 28);
  gfx->print(wifiSelectedSsid);

  gfx->fillRoundRect(62, 58, 342, 42, 9, UI_WHITE);
  gfx->drawRoundRect(62, 58, 342, 42, 9, UI_INK);
  char shown[35] = {};
  const uint8_t start = wifiPasswordLen > 30 ? wifiPasswordLen - 30 : 0;
  uint8_t out = 0;
  for (uint8_t i = start; i < wifiPasswordLen && out < sizeof(shown) - 1; ++i)
    shown[out++] = wifiPassword[i];
  gfx->setCursor(76, 70);
  gfx->print(shown[0] ? shown : "비밀번호");

  static const char *MODES[4] = {"123", "ABC", "abc", "!?#"};
  for (uint8_t i = 0; i < 4; ++i) {
    const int x = 42 + i * 96;
    drawTimeButton(x, 112, 88, 34, MODES[i], i == wifiKeyMode ? UI_BAR_OK : UI_WHITE,
                   UI_INK);
  }

  for (uint8_t key = 0; key < 12; ++key) {
    const int x = 66 + (key % 3) * 112;
    const int y = 158 + (key / 3) * 55;
    drawTimeButton(x, y, 100, 47, wifiKeyLabel(wifiKeyMode, key));
  }
  if (timeStatusText[0]) {
    gfx->setTextColor(UI_BAR_BAD);
    gfx->setTextSize(1);
    gfx->setCursor(CX - gfx->textWidth(timeStatusText) / 2, 383);
    gfx->print(timeStatusText);
  }
  drawTimeButton(66, 406, 150, 38, "취소");
  drawTimeButton(250, 406, 150, 38, "연결", UI_BAR_OK, UI_INK);
}

void renderWifiConnecting() {
  gfx->fillScreen(UI_BG_DAY);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(3);
  const char *title = wifiTimeState() == WIFI_TIME_NTP ? "시간 확인 중" : "Wi-Fi 연결 중";
  gfx->setCursor(CX - gfx->textWidth(title) / 2, 110);
  gfx->print(title);
  gfx->setTextSize(2);
  gfx->setTextColor(UI_INK);
  gfx->setCursor(CX - gfx->textWidth(wifiSelectedSsid) / 2, 184);
  gfx->print(wifiSelectedSsid);
  const char *hint = wifiTimeState() == WIFI_TIME_NTP ? "온라인으로 한국 시간을 받고 있어요." :
                                                       "잠시만 기다려 주세요.";
  gfx->setTextSize(1);
  gfx->setCursor(CX - gfx->textWidth(hint) / 2, 236);
  gfx->print(hint);
  drawTimeButton(158, 338, 150, 42, "취소");
}

void formatManualEntry(char *out, size_t size) {
  const char pattern[] = "____-__-__ __:__";
  std::strncpy(out, pattern, size - 1);
  out[size - 1] = 0;
  static const uint8_t POS[12] = {0, 1, 2, 3, 5, 6, 8, 9, 11, 12, 14, 15};
  for (uint8_t i = 0; i < manualTimeLen && i < 12; ++i) out[POS[i]] = manualTimeDigits[i];
}

void renderManualTime() {
  gfx->fillScreen(UI_BG_DAY);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(3);
  const char *title = "일자·시간 입력";
  gfx->setCursor(CX - gfx->textWidth(title) / 2, 24);
  gfx->print(title);
  char entry[24];
  formatManualEntry(entry, sizeof(entry));
  gfx->setTextSize(3);
  gfx->setCursor(CX - gfx->textWidth(entry) / 2, 76);
  gfx->print(entry);
  if (timeStatusText[0]) {
    gfx->setTextColor(UI_BAR_BAD);
    gfx->setTextSize(1);
    gfx->setCursor(CX - gfx->textWidth(timeStatusText) / 2, 116);
    gfx->print(timeStatusText);
  }
  for (uint8_t key = 0; key < 12; ++key) {
    static const char *KEYS[12] = {"1", "2", "3", "4", "5", "6", "7", "8", "9", "", "0", "<-"};
    const int x = 76 + (key % 3) * 106;
    const int y = 138 + (key / 3) * 58;
    if (key != 9) drawTimeButton(x, y, 94, 50, KEYS[key]);
  }
  drawTimeButton(72, 406, 150, 38, "취소");
  drawTimeButton(244, 406, 150, 38, "확인", UI_BAR_OK, UI_INK);
}

void renderSettingsPanel() {
  gfx->fillScreen(UI_BG_DAY);
  drawSettingsBattery();

  const uint32_t epoch = rtcEpoch();
  int year = 0;
  unsigned month = 0, day = 0;
  uint8_t hour = 0, minute = 0;
  char dateText[20] = "----.--.--";
  char timeText[8] = "--:--";
  if (epoch) {
    epochParts(epoch, year, month, day, hour, minute);
    snprintf(dateText, sizeof(dateText), "%04d.%02u.%02u", year, month, day);
    snprintf(timeText, sizeof(timeText), "%02u:%02u", hour, minute);
  }
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(2);
  gfx->setCursor(CX - gfx->textWidth(dateText) / 2, 58);
  gfx->print(dateText);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(6);
  gfx->setCursor(CX - gfx->textWidth(timeText) / 2, 84);
  gfx->print(timeText);

  drawTimeButton(44, 176, 182, 46, "Wi-Fi 시간보정");
  drawTimeButton(240, 176, 182, 46, "수동 보정");

  uint8_t sndMode = audioMode();
  const char *sl = soundModeLabel();
  gfx->fillRoundRect(SOUND_PILL_X, LANG_PILL_Y, SOUND_PILL_W, LANG_PILL_H, 8, sndMode ? UI_BAR_OK : UI_WHITE);
  gfx->drawRoundRect(SOUND_PILL_X, LANG_PILL_Y, SOUND_PILL_W, LANG_PILL_H, 8, UI_INK);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(2);
  gfx->setCursor(SOUND_PILL_X + (SOUND_PILL_W - gfx->textWidth(sl)) / 2, LANG_PILL_Y + 8);
  gfx->print(sl);

  const char *pl = powerSaveLabel();
  gfx->fillRoundRect(PSAVE_PILL_X, LANG_PILL_Y, PSAVE_PILL_W, LANG_PILL_H, 8, powerSave ? UI_BAR_WARN : UI_WHITE);
  gfx->drawRoundRect(PSAVE_PILL_X, LANG_PILL_Y, PSAVE_PILL_W, LANG_PILL_H, 8, UI_INK);
  gfx->setTextColor(UI_INK);
  gfx->setCursor(PSAVE_PILL_X + (PSAVE_PILL_W - gfx->textWidth(pl)) / 2, LANG_PILL_Y + 8);
  gfx->print(pl);

  bool tamaMode = pet.tamagotchiModeEnabled();
  gfx->fillRoundRect(LANG_PILL_X, LANG_PILL_Y, LANG_PILL_W, LANG_PILL_H, 8,
                     tamaMode ? UI_BAR_OK : UI_WHITE);
  gfx->drawRoundRect(LANG_PILL_X, LANG_PILL_Y, LANG_PILL_W, LANG_PILL_H, 8,
                     UI_INK);
  const char *lp = careModeLabel();
  gfx->setTextColor(UI_INK);
  gfx->setCursor(LANG_PILL_X + (LANG_PILL_W - gfx->textWidth(lp)) / 2, LANG_PILL_Y + 8);
  gfx->print(lp);

  drawTimeButton(64, 326, 134, 40, helpWordKo());
  drawTimeButton(210, 326, 192, 40, T(S_RESET_DATA), UI_WHITE, UI_BAR_BAD);

  char fwLine[34];
  snprintf(fwLine, sizeof(fwLine), "v%s", FW_VERSION);
  gfx->setTextColor(UI_TRACK);
  gfx->setTextSize(1);
  gfx->setCursor(CX - gfx->textWidth(fwLine) / 2, 382);
  gfx->print(fwLine);
  drawTimeButton(156, 408, 154, 38, T(S_DONE), UI_BAR_OK, UI_INK);
}

void renderClock() {
  clockDirty = false;
  if (resetStage != RESET_NONE) renderResetScreen();
  else if (timePanel == TIME_PANEL_CHOICE) renderTimeChoice();
  else if (timePanel == TIME_PANEL_AP_LIST) renderWifiApList();
  else if (timePanel == TIME_PANEL_PASSWORD) renderWifiPassword();
  else if (timePanel == TIME_PANEL_CONNECTING) renderWifiConnecting();
  else if (timePanel == TIME_PANEL_MANUAL) renderManualTime();
  else renderSettingsPanel();
  gfx->flush();
}

void appendWifiPassword(char value) {
  if (wifiPasswordLen >= sizeof(wifiPassword) - 1) return;
  wifiPassword[wifiPasswordLen++] = value;
  wifiPassword[wifiPasswordLen] = 0;
}

void wifiPasswordKeyTap(uint8_t key) {
  if (key == 11) {
    if (wifiPasswordLen) wifiPassword[--wifiPasswordLen] = 0;
    wifiLastGroup = -1;
    return;
  }
  if (key == 9) {
    appendWifiPassword(' ');
    wifiLastGroup = -1;
    return;
  }
  if (key == 10) {
    if (wifiKeyMode == 0) appendWifiPassword('0');
    wifiLastGroup = -1;
    return;
  }
  const char *group = wifiKeyLabel(wifiKeyMode, key);
  if (wifiKeyMode == 0) {
    appendWifiPassword(group[0]);
    wifiLastGroup = -1;
    return;
  }
  const uint8_t length = strlen(group);
  const uint32_t now = millis();
  if (wifiLastGroup == key && wifiPasswordLen && now - wifiLastGroupAt <= 900UL) {
    wifiGroupCycle = (wifiGroupCycle + 1) % length;
    wifiPassword[wifiPasswordLen - 1] = group[wifiGroupCycle];
  } else {
    wifiGroupCycle = 0;
    appendWifiPassword(group[0]);
  }
  wifiLastGroup = key;
  wifiLastGroupAt = now;
}

void returnFromTimeSubPanel() {
  wifiTimeStop();
  timeStatusText[0] = 0;
  if (timeReason == TIME_REASON_SETTINGS) timePanel = TIME_PANEL_SETTINGS;
  else timePanel = TIME_PANEL_CHOICE;
  clockDirty = true;
}

void clockTap(int16_t x, int16_t y) {
  if (resetStage == RESET_CONFIRM_FIRST || resetStage == RESET_CONFIRM_FINAL) {
    if (y >= 294 && y <= 348 && x >= 88 && x <= 218) {
      resetStage = RESET_NONE;
      clockDirty = true;
      sfxPlay(SFX_TAP);
    } else if (y >= 294 && y <= 348 && x >= 248 && x <= 378) {
      resetStage = resetStage == RESET_CONFIRM_FIRST ? RESET_CONFIRM_FINAL : RESET_HOLD_REQUIRED;
      resetHolding = false;
      resetHoldStartedAt = 0;
      clockDirty = true;
      sfxPlay(resetStage == RESET_HOLD_REQUIRED ? SFX_DENY : SFX_MENU);
    }
    return;
  }

  if (timePanel == TIME_PANEL_CHOICE) {
    if (x >= 78 && x <= 388 && y >= 238 && y <= 292) startWifiApList();
    else if (x >= 78 && x <= 388 && y >= 312 && y <= 366) startManualTimeEntry();
    return;
  }

  if (timePanel == TIME_PANEL_AP_LIST) {
    if (y >= 86 && y < 376 && x >= 54 && x <= 412) {
      const uint8_t row = (y - 86) / 58;
      const uint8_t index = wifiApPage * 5 + row;
      const WifiTimeAp *ap = wifiTimeAp(index);
      if (ap) {
        strncpy(wifiSelectedSsid, ap->ssid, sizeof(wifiSelectedSsid) - 1);
        wifiSelectedSsid[sizeof(wifiSelectedSsid) - 1] = 0;
        wifiSelectedSecure = ap->secure;
        wifiTimeStop();
        timeStatusText[0] = 0;
        if (ap->secure) {
          wifiPassword[0] = 0;
          wifiTimeLoadPassword(ap->ssid, wifiPassword, sizeof(wifiPassword));
          wifiPasswordLen = strlen(wifiPassword);
          wifiKeyMode = 1;
          wifiLastGroup = -1;
          timePanel = TIME_PANEL_PASSWORD;
        } else {
          wifiPassword[0] = 0;
          wifiPasswordLen = 0;
          wifiTimeStartConnect(wifiSelectedSsid, "");
          timePanel = TIME_PANEL_CONNECTING;
        }
        clockDirty = true;
      }
      return;
    }
    if (y >= 398) {
      const uint8_t pages = (wifiTimeApCount() + 4) / 5;
      if (x < 166 && wifiApPage > 0) wifiApPage--;
      else if (x > 300 && wifiApPage + 1 < pages) wifiApPage++;
      else if (x >= 166 && x <= 300) returnFromTimeSubPanel();
      clockDirty = true;
    }
    return;
  }

  if (timePanel == TIME_PANEL_PASSWORD) {
    if (y >= 108 && y <= 150 && x >= 42 && x < 426) {
      const int mode = (x - 42) / 96;
      if (mode >= 0 && mode < 4) {
        wifiKeyMode = mode;
        wifiLastGroup = -1;
        clockDirty = true;
      }
      return;
    }
    if (y >= 158 && y < 378 && x >= 66 && x < 402) {
      const int col = (x - 66) / 112;
      const int row = (y - 158) / 55;
      if (col >= 0 && col < 3 && row >= 0 && row < 4) {
        wifiPasswordKeyTap(row * 3 + col);
        timeStatusText[0] = 0;
        clockDirty = true;
      }
      return;
    }
    if (y >= 400 && x <= 230) {
      startWifiApList();
      return;
    }
    if (y >= 400 && x >= 238) {
      if (wifiPasswordLen < 8) {
        strncpy(timeStatusText, "비밀번호는 8자 이상 입력해 주세요.", sizeof(timeStatusText) - 1);
      } else {
        wifiTimeStartConnect(wifiSelectedSsid, wifiPassword);
        timePanel = TIME_PANEL_CONNECTING;
      }
      clockDirty = true;
      return;
    }
    return;
  }

  if (timePanel == TIME_PANEL_CONNECTING) {
    if (y >= 330 && y <= 390) startWifiApList();
    return;
  }

  if (timePanel == TIME_PANEL_MANUAL) {
    if (y >= 138 && y < 374 && x >= 76 && x < 394) {
      const int col = (x - 76) / 106;
      const int row = (y - 138) / 58;
      const int key = row * 3 + col;
      if (key >= 0 && key < 12) {
        if (key == 11) {
          if (manualTimeLen) manualTimeDigits[--manualTimeLen] = 0;
        } else if (key != 9 && manualTimeLen < 12) {
          const uint8_t digit = key == 10 ? 0 : key + 1;
          manualTimeDigits[manualTimeLen++] = '0' + digit;
          manualTimeDigits[manualTimeLen] = 0;
        }
        timeStatusText[0] = 0;
        clockDirty = true;
      }
      return;
    }
    if (y >= 400 && x < 234) {
      returnFromTimeSubPanel();
      return;
    }
    if (y >= 400 && x >= 234) {
      if (manualTimeLen != 12) {
        strncpy(timeStatusText, "12자리를 모두 입력해 주세요.", sizeof(timeStatusText) - 1);
      } else {
        const int year = (manualTimeDigits[0] - '0') * 1000 + (manualTimeDigits[1] - '0') * 100 +
                         (manualTimeDigits[2] - '0') * 10 + manualTimeDigits[3] - '0';
        const int month = (manualTimeDigits[4] - '0') * 10 + manualTimeDigits[5] - '0';
        const int day = (manualTimeDigits[6] - '0') * 10 + manualTimeDigits[7] - '0';
        const int hour = (manualTimeDigits[8] - '0') * 10 + manualTimeDigits[9] - '0';
        const int minute = (manualTimeDigits[10] - '0') * 10 + manualTimeDigits[11] - '0';
        uint32_t epoch = 0;
        if (!makeWallEpoch(year, month, day, hour, minute, epoch))
          strncpy(timeStatusText, "일자와 시간을 확인해 주세요.", sizeof(timeStatusText) - 1);
        else finishCorrectedTime(epoch);
      }
      clockDirty = true;
      return;
    }
    return;
  }

  // 일반 설정 화면
  if (y >= 172 && y <= 228) {
    if (x >= 40 && x <= 232) startWifiApList();
    else if (x >= 234 && x <= 426) startManualTimeEntry();
    return;
  }
  if (y >= LANG_PILL_Y && y <= LANG_PILL_Y + LANG_PILL_H) {
    if (x >= SOUND_PILL_X && x < SOUND_PILL_X + SOUND_PILL_W) {
      audioSetMode(nextSoundMode());
      clockDirty = true;
      if (audioEnabled()) sfxPlay(SFX_LEVEL);  // confirma el nuevo modo si no esta apagado
      return;
    }
    if (x >= PSAVE_PILL_X && x < PSAVE_PILL_X + PSAVE_PILL_W) {
      setPowerSave(!powerSave);
      clockDirty = true;
      sfxPlay(powerSave ? SFX_MENU : SFX_TAP);
      return;
    }
    if (x >= LANG_PILL_X && x < LANG_PILL_X + LANG_PILL_W) {
      pet.setTamagotchiMode(!pet.tamagotchiModeEnabled());
      careAlertShownThisWake = false;
      clockDirty = true;
      sfxPlay(pet.tamagotchiModeEnabled() ? SFX_MENU : SFX_TAP);
      return;
    }
  }
  if (y >= 326 && y <= 366 && x >= 64 && x <= 198) { openHelp(); return; }
  if (y >= 326 && y <= 366 && x >= 210 && x <= 402) {
    resetStage = RESET_CONFIRM_FIRST;
    resetHolding = false;
    clockDirty = true;
    sfxPlay(SFX_DENY);
    return;
  }
  if (y >= 404 && y <= 450 && x >= 150 && x <= 316) {
    wifiTimeStop();
    clockOpen = false;
    markUiDirty();
    return;
  }
}

// llama + numero de racha arriba a la izquierda
void drawStreakBadge() {
  if (pet.streak < 1) return;
  int x = 26, y = 16;
  gfx->fillTriangle(x + 8, y, x + 1, y + 17, x + 15, y + 17, UI_BAR_BAD);
  gfx->fillTriangle(x + 8, y + 7, x + 4, y + 17, x + 12, y + 17, UI_BAR_WARN);
  char s[6];
  snprintf(s, sizeof(s), "%u", pet.streak);
  gfx->setTextColor(inkColor());
  gfx->setTextSize(2);
  gfx->setCursor(x + 22, y + 2);
  gfx->print(s);
}

// banner temporal: medalla nueva o hito de racha
void drawCelebration() {
  const char *l1 = nullptr, *l2 = nullptr;
  char buf[20];
  if (pet.showMedal()) {
    for (int i = 0; i < MED_COUNT; i++)
      if (pet.newMedal & (1 << i)) { l2 = medalName(i); break; }
    l1 = T(S_MEDAL_BANNER);
  } else if (pet.showMilestone()) {
    snprintf(buf, sizeof(buf), T(S_STREAK_DAYS_FMT), pet.streak);
    l1 = T(S_GREAT);
    l2 = buf;
  }
  if (!l1) return;
  gfx->fillRoundRect(73, 150, 320, 96, 16, UI_BAR_WARN);
  gfx->drawRoundRect(73, 150, 320, 96, 16, UI_INK);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(3);
  gfx->setCursor(CX - gfx->textWidth(l1) / 2, 176);
  gfx->print(l1);
  gfx->setTextSize(2);
  gfx->setCursor(CX - gfx->textWidth(l2) / 2, 212);
  gfx->print(l2);
}

// medallas en la ficha: badge con etiqueta, color si conseguida
void drawMedalBadge(int x, int y, int i) {
  bool got = pet.hasMedal(1 << i);
  gfx->fillRoundRect(x, y, 100, 24, 6, got ? UI_BAR_OK : UI_TRACK);
  if (!got) gfx->drawRoundRect(x, y, 100, 24, 6, UI_TRACK);
  gfx->setTextColor(got ? UI_INK : UI_WHITE);
  gfx->setTextSize(2);
  gfx->setCursor(x + (100 - gfx->textWidth(medalLabel(i))) / 2, y + 5);
  gfx->print(medalLabel(i));
}

// 기술 페이지. 기술 정보와 기술 디스크 사용을 한곳에 모은다.
void renderCardMoves() {
  const DexEntry &d = DEX_TBL[pet.speciesId];
  const char *nm = pet.nick[0] ? pet.nick : dexName(pet.speciesId);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(4);
  const char *title = "기술";
  gfx->setCursor(CX - gfx->textWidth(title) / 2, 34);
  gfx->print(title);
  char head[34];
  snprintf(head, sizeof(head), "%s%s  Lv.%u", pet.shiny ? "*" : "", nm, pet.level());
  gfx->setTextColor(d.accent);
  gfx->setTextSize(2);
  if (gfx->textWidth(head) > 330) gfx->setTextSize(1);
  gfx->setCursor(CX - gfx->textWidth(head) / 2, 78);
  gfx->print(head);

  constexpr int moveY = 108;
  constexpr int moveH = 76;
  constexpr int move1X = 62;
  constexpr int move2X = 238;
  constexpr int moveW = 166;
  constexpr int discY = 206;
  constexpr int relearnX = 92;
  constexpr int relearnY = 246;
  constexpr int relearnW = 282;
  constexpr int relearnH = 58;

  auto drawMoveTile = [&](int x, StrId labelId, uint16_t moveId, uint8_t unlockLevel) {
    bool unlocked = pet.level() >= unlockLevel;
    gfx->fillRoundRect(x, moveY, moveW, moveH, 10,
                       unlocked ? UI_WHITE : UI_TRACK);
    gfx->drawRoundRect(x, moveY, moveW, moveH, 10,
                       unlocked ? d.accent : UI_TRACK);
    gfx->setTextSize(2);
    gfx->setTextColor(unlocked ? d.accent : UI_WHITE);
    gfx->setCursor(x + (moveW - gfx->textWidth(T(labelId))) / 2, moveY + 10);
    gfx->print(T(labelId));
    char moveLine[42];
    if (unlocked) snprintf(moveLine, sizeof(moveLine), "%s", moveDef(moveId).name);
    else snprintf(moveLine, sizeof(moveLine), "Lv.%u", unlockLevel);
    gfx->setTextSize(2);
    int moveTextSize = gfx->textWidth(moveLine) <= moveW - 16 ? 2 : 1;
    gfx->setTextSize(moveTextSize);
    gfx->setTextColor(unlocked ? UI_INK : UI_WHITE);
    gfx->setCursor(x + (moveW - gfx->textWidth(moveLine)) / 2,
                   moveY + (moveTextSize == 2 ? 43 : 48));
    gfx->print(moveLine);
  };
  drawMoveTile(move1X, S_MOVE1, pet.battleMove1, 10);
  drawMoveTile(move2X, S_MOVE2, pet.signatureMove(), 20);

  char discs[30];
  snprintf(discs, sizeof(discs), "%s x%u", T(S_MOVE_DISC), pet.itemCounts[EXP_ITEM_MOVE]);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(3);
  gfx->setCursor(CX - gfx->textWidth(discs) / 2, discY);
  gfx->print(discs);
  bool canRelearn = pet.canRelearnMove1();
  gfx->fillRoundRect(relearnX, relearnY, relearnW, relearnH, 12,
                     canRelearn ? UI_BAR_WARN : UI_TRACK);
  gfx->setTextColor(canRelearn ? UI_INK : UI_WHITE);
  gfx->setTextSize(3);
  gfx->setCursor(CX - gfx->textWidth(T(S_RELEARN_MOVE)) / 2,
                 relearnY + 18);
  gfx->print(T(S_RELEARN_MOVE));

  if (moveRelearnOpen && moveRelearnCandidate != MOVE_NONE) {
    gfx->fillRoundRect(48, 226, 370, 174, 18, UI_WHITE);
    gfx->drawRoundRect(48, 226, 370, 174, 18, UI_INK);
    gfx->setTextColor(UI_INK);
    gfx->setTextSize(2);
    gfx->setCursor(CX - gfx->textWidth(T(S_RELEARN_Q)) / 2, 250);
    gfx->print(T(S_RELEARN_Q));
    gfx->setTextColor(d.accent);
    gfx->setTextSize(3);
    gfx->setCursor(CX - gfx->textWidth(moveDef(moveRelearnCandidate).name) / 2, 286);
    gfx->print(moveDef(moveRelearnCandidate).name);
    gfx->fillRoundRect(70, 338, 154, 56, 12, UI_BAR_OK);
    gfx->fillRoundRect(242, 338, 154, 56, 12, UI_TRACK);
    gfx->setTextColor(UI_INK);
    gfx->setTextSize(2);
    gfx->setCursor(147 - gfx->textWidth(T(S_LEARN_MOVE)) / 2, 357);
    gfx->print(T(S_LEARN_MOVE));
    gfx->setTextColor(UI_WHITE);
    gfx->setCursor(319 - gfx->textWidth(T(S_KEEP_MOVE)) / 2, 357);
    gfx->print(T(S_KEEP_MOVE));
  }
}

// 상태 카드 첫 페이지: 이름, 현재 모습, 레벨과 함께한 날짜만 간결하게 표시한다.
void renderCardProfile() {
  const DexEntry &d = DEX_TBL[pet.speciesId];
  const char *nm = pet.nick[0] ? pet.nick : dexName(pet.speciesId);
  char head[34];
  snprintf(head, sizeof(head), "%s%s", pet.shiny ? "*" : "", nm);
  gfx->setTextColor(d.accent);
  gfx->setTextSize(4);
  if (gfx->textWidth(head) > 250) gfx->setTextSize(3);
  gfx->setCursor(CX - gfx->textWidth(head) / 2, 38);
  gfx->print(head);
  gfx->setTextColor(UI_TRACK);
  gfx->setTextSize(2);
  const char *rename = "이름을 누르면 변경";
  gfx->setCursor(CX - gfx->textWidth(rename) / 2, 78);
  gfx->print(rename);

  if (pmd.loaded) drawPmdAct(PMD_IDLE, CX, 214, millis(), true, false, 5);

  char level[24];
  snprintf(level, sizeof(level), "%s Lv.%u", dexName(pet.speciesId), pet.level());
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(3);
  gfx->setCursor(CX - gfx->textWidth(level) / 2, 320);
  gfx->print(level);
  char days[32];
  snprintf(days, sizeof(days), "함께한 지 %lu일", (unsigned long)(pet.ageMinutes / 1440UL));
  gfx->setTextColor(UI_TRACK);
  gfx->setTextSize(2);
  gfx->setCursor(CX - gfx->textWidth(days) / 2, 352);
  gfx->print(days);
  const char *berry = !pet.berryKnown ? T(S_BERRY_UNK)
                      : pet.lovesBerry(0) ? T(S_BERRY_RED)
                      : pet.lovesBerry(1) ? T(S_BERRY_BLUE)
                                          : T(S_BERRY_GREEN);
  char berryLine[46];
  snprintf(berryLine, sizeof(berryLine), "좋아하는 나무열매: %s", berry);
  gfx->setCursor(CX - gfx->textWidth(berryLine) / 2, 376);
  gfx->print(berryLine);
}

StrId personalityNameId(PetPersonality p) {
  switch (p) {
    case PERS_PLAYFUL: return S_PERS_PLAYFUL;
    case PERS_BRAVE: return S_PERS_BRAVE;
    case PERS_CALM: return S_PERS_CALM;
    case PERS_LAZY: return S_PERS_LAZY;
    default: return S_PERS_BALANCED;
  }
}

StrId personalityHintId(PetPersonality p) {
  switch (p) {
    case PERS_PLAYFUL: return S_PERS_PLAYFUL_HINT;
    case PERS_BRAVE: return S_PERS_BRAVE_HINT;
    case PERS_CALM: return S_PERS_CALM_HINT;
    case PERS_LAZY: return S_PERS_LAZY_HINT;
    default: return S_PERS_BALANCED_HINT;
  }
}

uint16_t personalityColor(PetPersonality p) {
  switch (p) {
    case PERS_PLAYFUL: return UI_BAR_WARN;
    case PERS_BRAVE: return UI_BAR_BAD;
    case PERS_CALM: return 0x4C98;
    case PERS_LAZY: return 0xB3C8;
    default: return UI_BAR_OK;
  }
}

void drawPersonalityRecord(int x, int y, const char *label, uint16_t val, uint16_t color) {
  gfx->fillRoundRect(x, y, 118, 34, 8, UI_WHITE);
  gfx->drawRoundRect(x, y, 118, 34, 8, color);
  gfx->setTextColor(color);
  gfx->setTextSize(1);
  gfx->setCursor(x + 10, y + 6);
  gfx->print(label);
  char num[8];
  snprintf(num, sizeof(num), "%u", val);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(2);
  gfx->setCursor(x + 118 - 12 - gfx->textWidth(num), y + 14);
  gfx->print(num);
}

// pagina 1: personalidad calculada + records largos, sin tocar balance
void renderCardPersonality() {
  PetPersonality pers = pet.personality();
  const char *title = T(S_PERSONALITY);
  const char *name = T(personalityNameId(pers));
  const char *hint = T(personalityHintId(pers));
  uint16_t col = personalityColor(pers);

  gfx->setTextColor(UI_INK);
  gfx->setTextSize(3);
  gfx->setCursor(CX - gfx->textWidth(title) / 2, 44);
  gfx->print(title);

  gfx->fillRoundRect(62, 86, 342, 70, 16, col);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(3);
  int nts = gfx->textWidth(name) <= 300 ? 3 : 2;
  gfx->setTextSize(nts);
  gfx->setCursor(CX - gfx->textWidth(name) / 2, nts == 3 ? 104 : 111);
  gfx->print(name);
  gfx->setTextSize(2);
  gfx->setCursor(CX - gfx->textWidth(hint) / 2, 136);
  gfx->print(hint);

  drawCardStat(182, T(S_VIN), pet.bond, 100, C565(0xd4, 0x52, 0x7e));
  drawCardStat(220, T(S_BAR_JOY), pet.joy, 100, UI_BAR_WARN);

  gfx->setTextColor(UI_INK);
  gfx->setTextSize(2);
  gfx->setCursor(CX - gfx->textWidth(T(S_RECORDS)) / 2, 264);
  gfx->print(T(S_RECORDS));
  drawPersonalityRecord(52, 290, "피카츄", pet.gameHi, UI_BAR_OK);
  drawPersonalityRecord(178, 290, "잠만보", pet.catchHi, UI_BAR_WARN);
  drawPersonalityRecord(304, 290, "이브이", pet.memoHi, 0x4C98);
  drawPersonalityRecord(52, 334, "디그다", pet.diglettHi, UI_BAR_BAD);
  drawPersonalityRecord(178, 334, "잉어킹", pet.typeHi, 0xF3B7);
  drawPersonalityRecord(304, 334, T(S_BATTLE), pet.bestBattleStreak, UI_BAR_BAD);
}

const char *dailyGoalLabel(uint8_t goalType) {
  switch (goalType) {
    case DAILY_GOAL_CARE: return T(S_GOAL_CARE);
    case DAILY_GOAL_PLAY: return T(S_GOAL_PLAY);
    case DAILY_GOAL_BATTLE: return T(S_GOAL_BATTLE);
    case DAILY_GOAL_ITEM: return "아이템 사용";
    case DAILY_GOAL_WALK: return "산책하기";
    default: return T(S_GOAL_CARE);
  }
}

uint16_t dailyGoalColor(uint8_t goalType) {
  switch (goalType) {
    case DAILY_GOAL_CARE: return C565(0xd4, 0x52, 0x7e);
    case DAILY_GOAL_PLAY: return UI_BAR_WARN;
    case DAILY_GOAL_BATTLE: return UI_BAR_BAD;
    case DAILY_GOAL_ITEM: return 0xB3C8;
    case DAILY_GOAL_WALK: return UI_BAR_OK;
    default: return UI_INK;
  }
}

void drawDailyGoalRow(int y, uint8_t idx) {
  uint8_t type = pet.dailyGoalType[idx];
  uint8_t target = pet.dailyGoalTarget(type);
  uint8_t progress = pet.dailyGoalProgress[idx] > target ? target : pet.dailyGoalProgress[idx];
  bool done = pet.dailyGoalComplete(idx);
  uint16_t col = dailyGoalColor(type);
  gfx->fillRoundRect(58, y, 350, 60, 12, done ? col : UI_WHITE);
  gfx->drawRoundRect(58, y, 350, 60, 12, col);
  gfx->setTextSize(3);
  gfx->setTextColor(UI_INK);
  gfx->setCursor(76, y + 19);
  gfx->print(dailyGoalLabel(type));

  char prog[12];
  snprintf(prog, sizeof(prog), "%u/%u", progress, target);
  gfx->setCursor(286, y + 19);
  gfx->print(done ? T(S_DONE) : prog);
  if (done) {
    gfx->fillCircle(378, y + 30, 12, UI_BG_DAY);
    gfx->setTextColor(col);
    gfx->setCursor(370, y + 19);
    gfx->print("v");
  }
}

// pagina 2: objetivos diarios
void renderCardDaily() {
  pet.ensureDailyGoals();
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(4);
  gfx->setCursor(CX - gfx->textWidth(T(S_DAILY)) / 2, 34);
  gfx->print(T(S_DAILY));
  const char *phase = T(dayPhaseTextId(currentDayPhase()));
  gfx->setTextColor(UI_TRACK);
  gfx->setTextSize(2);
  gfx->setCursor(CX - gfx->textWidth(phase) / 2, 74);
  gfx->print(phase);

  uint8_t done = 0;
  for (uint8_t i = 0; i < DAILY_GOAL_COUNT; i++) {
    if (pet.dailyGoalComplete(i)) done++;
    drawDailyGoalRow(104 + i * 68, i);
  }

  char bonus[24];
  snprintf(bonus, sizeof(bonus), "%s %u/%u", T(S_REWARD), done, DAILY_GOAL_COUNT);
  gfx->setTextColor(done == DAILY_GOAL_COUNT ? UI_BAR_OK : UI_TRACK);
  gfx->setTextSize(3);
  gfx->setCursor(CX - gfx->textWidth(bonus) / 2, 326);
  gfx->print(bonus);
}

#define BOX_ROWS 5

bool boxComesBefore(int16_t a, int16_t b) {
  if (boxSort == 1) {
    const DexEntry &da = DEX_TBL[a];
    const DexEntry &db = DEX_TBL[b];
    if (da.type1 != db.type1) return da.type1 < db.type1;
    if (da.type2 != db.type2) return da.type2 < db.type2;
  } else if (boxSort == 2) {
    bool ra = pet.isRegistered(a);
    bool rb = pet.isRegistered(b);
    if (ra != rb) return ra;
  }
  return a < b;
}

uint16_t boxBuildList(int16_t *out) {
  uint16_t n = 0;
  for (int16_t dex = 1; dex <= DEX_COUNT; dex++)
    if (pet.isCaught(dex)) out[n++] = dex;
  for (uint16_t i = 1; i < n; i++) {
    int16_t v = out[i];
    int j = i - 1;
    while (j >= 0 && boxComesBefore(v, out[j])) {
      out[j + 1] = out[j];
      j--;
    }
    out[j + 1] = v;
  }
  return n;
}

uint8_t boxPageCount() {
  uint16_t count = pet.caughtCount();
  uint8_t pages = (count + BOX_ROWS - 1) / BOX_ROWS;
  return pages > 0 ? pages : 1;
}

int16_t boxDexAt(uint16_t index) {
  int16_t list[DEX_COUNT];
  uint16_t n = boxBuildList(list);
  return index < n ? list[index] : 0;
}

const char *boxSortLabel() {
  if (boxSort == 1) return T(S_SORT_TYPE);
  if (boxSort == 2) return T(S_SORT_RAISED);
  return T(S_SORT_DEX);
}

void renderCardBox() {
  uint8_t pages = boxPageCount();
  if (boxPage >= pages) boxPage = pages - 1;

  gfx->setTextColor(UI_INK);
  gfx->setTextSize(3);
  gfx->setCursor(CX - gfx->textWidth(T(S_BOX)) / 2, 42);
  gfx->print(T(S_BOX));

  const char *sort = boxSortLabel();
  gfx->fillRoundRect(302, 62, 106, 28, 9, UI_WHITE);
  gfx->drawRoundRect(302, 62, 106, 28, 9, UI_INK);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(1);
  gfx->setCursor(302 + (106 - gfx->textWidth(sort)) / 2, 73);
  gfx->print(sort);

  char caught[24], known[24];
  snprintf(caught, sizeof(caught), T(S_CAUGHT_COUNT_FMT), pet.caughtCount());
  snprintf(known, sizeof(known), T(S_KNOWN_FMT), pet.knownDexCount());
  gfx->setTextSize(2);
  gfx->setTextColor(UI_INK);
  gfx->setCursor(72, 70);
  gfx->print(caught);
  gfx->setTextColor(UI_TRACK);
  gfx->setCursor(72, 92);
  gfx->print(known);

  if (pet.caughtCount() == 0) {
    gfx->fillRoundRect(82, 178, 302, 72, 16, UI_WHITE);
    gfx->drawRoundRect(82, 178, 302, 72, 16, UI_TRACK);
    gfx->setTextColor(UI_TRACK);
    gfx->setTextSize(2);
    gfx->setCursor(CX - gfx->textWidth(T(S_NO_CATCHES)) / 2, 207);
    gfx->print(T(S_NO_CATCHES));
    return;
  }

  for (uint8_t i = 0; i < BOX_ROWS; i++) {
    int16_t dex = boxDexAt((uint16_t)boxPage * BOX_ROWS + i);
    if (dex <= 0) break;
    const DexEntry &d = DEX_TBL[dex];
    int y = 122 + i * 42;
    bool raised = pet.isRegistered(dex);
    gfx->fillRoundRect(58, y, 350, 40, 9, UI_WHITE);
    gfx->drawRoundRect(58, y, 350, 40, 9, d.accent);
    char name[24];
    snprintf(name, sizeof(name), "#%03d %s", dex, dexName(dex));
    gfx->setTextSize(2);
    int ts = gfx->textWidth(name) <= 190 ? 2 : 1;
    gfx->setTextSize(ts);
    gfx->setTextColor(UI_INK);
    gfx->setCursor(72, y + (ts == 2 ? 7 : 5));
    gfx->print(name);
    char types[22];
    typeText(types, sizeof(types), d);
    gfx->setTextSize(1);
    gfx->setTextColor(battleTypeColor(d.type1));
    gfx->setCursor(72, y + 24);
    gfx->print(types);
    if (raised) {
      gfx->setTextColor(UI_BAR_OK);
      gfx->setCursor(330, y + 15);
      gfx->print(T(S_RAISED_MARK));
    }
  }
  bool canPrev = boxPage > 0;
  bool canNext = boxPage + 1 < pages;
  gfx->fillRoundRect(76, 348, 94, 38, 11, canPrev ? UI_TRACK : UI_WHITE);
  gfx->fillRoundRect(296, 348, 94, 38, 11, canNext ? UI_TRACK : UI_WHITE);
  gfx->drawRoundRect(76, 348, 94, 38, 11, canPrev ? UI_TRACK : UI_INK);
  gfx->drawRoundRect(296, 348, 94, 38, 11, canNext ? UI_TRACK : UI_INK);
  gfx->setTextColor(canPrev ? UI_WHITE : UI_TRACK);
  gfx->setTextSize(3);
  gfx->setCursor(111, 357);
  gfx->print("<");
  gfx->setTextColor(canNext ? UI_WHITE : UI_TRACK);
  gfx->setCursor(331, 357);
  gfx->print(">");
  char pg[12];
  snprintf(pg, sizeof(pg), T(S_PAGE_FMT), boxPage + 1, pages);
  gfx->setTextColor(UI_TRACK);
  gfx->setTextSize(2);
  gfx->setCursor(CX - gfx->textWidth(pg) / 2, 360);
  gfx->print(pg);
}

void drawStatusTile(int x, int y, const char *label, uint16_t value,
                    uint16_t maximum, uint16_t color) {
  gfx->fillRoundRect(x, y, 170, 58, 11, UI_WHITE);
  gfx->drawRoundRect(x, y, 170, 58, 11, color);
  gfx->setTextColor(UI_TRACK);
  gfx->setTextSize(2);
  gfx->setCursor(x + 12, y + 9);
  gfx->print(label);
  char number[12];
  snprintf(number, sizeof(number), "%u", value);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(3);
  gfx->setCursor(x + 158 - gfx->textWidth(number), y + 8);
  gfx->print(number);
  gfx->fillRoundRect(x + 12, y + 41, 146, 9, 3, UI_TRACK);
  uint16_t capped = value > maximum ? maximum : value;
  uint16_t fill = maximum ? (uint16_t)((uint32_t)146 * capped / maximum) : 0;
  if (fill) gfx->fillRoundRect(x + 12, y + 41, fill, 9, 3, color);
}

// 상태 페이지: 육성 수치와 생활 수치를 한 화면에서 확인한다.
void renderCardStats() {
  BattleStats actual = petBattleStats();
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(3);
  const char *title = "상태";
  gfx->setCursor(CX - gfx->textWidth(title) / 2, 40);
  gfx->print(title);
  drawStatusTile(54, 84, T(S_STAT_ATK), actual.atk, 260, UI_BAR_BAD);
  drawStatusTile(242, 84, T(S_STAT_DEF), actual.def, 260, 0x4C98);
  drawStatusTile(54, 150, T(S_STAT_SPE), actual.spe, 260, UI_BAR_WARN);
  drawStatusTile(242, 150, "몸무게", pet.weight, 100, 0xB3C8);
  drawStatusTile(54, 216, "배부름", pet.fullness, 100, UI_BAR_WARN);
  drawStatusTile(242, 216, "기분", pet.joy, 100, C565(0xd4, 0x52, 0x7e));
  drawStatusTile(54, 282, "체력", pet.energy, 100, UI_BAR_OK);
  drawStatusTile(242, 282, "청결", pet.hygiene, 100, 0x4C98);

  char weightHint[44];
  if (pet.weight >= 80) snprintf(weightHint, sizeof(weightHint), "무거움: 공방 +12%% / 속도 -18%%");
  else if (pet.weight >= 60) snprintf(weightHint, sizeof(weightHint), "무거움: 공방 +8%% / 속도 -10%%");
  else if (pet.weight >= 40) snprintf(weightHint, sizeof(weightHint), "무거움: 공방 +4%% / 속도 -4%%");
  else snprintf(weightHint, sizeof(weightHint), "몸무게 보정 없음");
  gfx->setTextColor(UI_TRACK);
  gfx->setTextSize(2);
  gfx->setCursor(CX - gfx->textWidth(weightHint) / 2, 358);
  gfx->print(weightHint);
}

// pagina 2: medallas con etiqueta descriptiva
void renderCardMedals() {
  int got = 0;
  for (int i = 0; i < MED_COUNT; i++)
    if (pet.hasMedal(1 << i)) got++;
  char head[20];
  snprintf(head, sizeof(head), T(S_MEDALS_FMT), got, MED_COUNT);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(3);
  gfx->setCursor(CX - gfx->textWidth(head) / 2, 48);
  gfx->print(head);

  for (int i = 0; i < MED_COUNT; i++) {
    int x = 28 + (i % 2) * 206, y = 104 + (i / 2) * 54;
    bool g = pet.hasMedal(1 << i);
    gfx->fillRoundRect(x, y, 196, 44, 10, g ? UI_BAR_OK : UI_TRACK);
    if (g) {  // marca de conseguida
      gfx->fillCircle(x + 22, y + 22, 11, UI_BG_DAY);
      gfx->setTextColor(UI_BAR_OK);
      gfx->setTextSize(2);
      gfx->setCursor(x + 16, y + 13);
      gfx->print("v");
    }
    gfx->setTextColor(g ? UI_INK : UI_WHITE);
    gfx->setTextSize(2);
    gfx->setCursor(x + 44, y + 14);
    gfx->print(medalDesc(i));
  }
}

// pagina 3: progreso (nivel, evolucion, descuidos) — saca a la luz mecanicas
// que antes eran invisibles (cuanto falta para subir/evolucionar y por que)
void renderCardProgress() {
  const DexEntry &d = DEX_TBL[pet.speciesId];
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(3);
  gfx->setCursor(CX - gfx->textWidth(T(S_PROGRESS)) / 2, 44);
  gfx->print(T(S_PROGRESS));

  // nivel grande
  char lv[10];
  snprintf(lv, sizeof(lv), T(S_LVL_FMT), pet.level());
  gfx->setTextSize(5);
  gfx->setCursor(CX - gfx->textWidth(lv) / 2, 86);
  gfx->print(lv);

  // barra de progreso al siguiente nivel (1 nivel = 120 min de juego)
  uint8_t into = pet.ageMinutes % MINUTES_PER_LEVEL;
  int bx = 93, bw = 280, by = 158, bh = 22;
  gfx->fillRoundRect(bx, by, bw, bh, 6, UI_TRACK);
  int fw = (bw - 4) * into / MINUTES_PER_LEVEL;
  if (fw > 0) gfx->fillRoundRect(bx + 2, by + 2, fw, bh - 4, 5, UI_BAR_OK);
  char nx[26];
  snprintf(nx, sizeof(nx), T(S_NEXT_LVL_FMT), MINUTES_PER_LEVEL - into, pet.level() + 1);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(3);
  if (gfx->textWidth(nx) > 350) gfx->setTextSize(2);
  gfx->setCursor(CX - gfx->textWidth(nx) / 2, by + 32);
  gfx->print(nx);

  // estado de evolucion
  gfx->setTextColor(UI_TRACK);
  gfx->setTextSize(3);
  gfx->setCursor(CX - gfx->textWidth(T(S_EVO_LABEL)) / 2, 230);
  gfx->print(T(S_EVO_LABEL));
  char evoBuf[28];
  const char *evo;
  uint16_t evoCol = UI_INK;
  if (d.evolvesTo == 0) {
    evo = T(S_FINAL_FORM);
  } else {
    int needed = d.evolveLevel + pet.careMistakes;
    if (pet.level() >= needed) {
      if (pet.lowestStat() >= 40) { evo = T(S_EVO_READY); evoCol = UI_BAR_OK; }
      else { evo = T(S_EVO_BLOCKED); evoCol = UI_BAR_BAD; }
    } else {
      snprintf(evoBuf, sizeof(evoBuf), T(S_EVO_IN_FMT), needed - pet.level());
      evo = evoBuf;
    }
  }
  gfx->setTextColor(evoCol);
  gfx->setTextSize(3);
  if (gfx->textWidth(evo) > 350) gfx->setTextSize(2);
  gfx->setCursor(CX - gfx->textWidth(evo) / 2, 256);
  gfx->print(evo);

  // descuidos (retrasan la evolucion)
  char ms[24];
  snprintf(ms, sizeof(ms), T(S_MISTAKES_FMT), pet.careMistakes);
  gfx->setTextColor(pet.careMistakes > 0 ? UI_BAR_BAD : UI_INK);
  gfx->setTextSize(3);
  gfx->setCursor(CX - gfx->textWidth(ms) / 2, 312);
  gfx->print(ms);
}

StrId expeditionItemText(ExpeditionItem item) {
  switch (item) {
    case EXP_ITEM_SNACK: return S_ITEM_SNACK;
    case EXP_ITEM_ENERGY: return S_ITEM_ENERGY;
    case EXP_ITEM_CARE: return S_ITEM_CARE;
    case EXP_ITEM_TRAIN: return S_ITEM_TRAIN;
    case EXP_ITEM_MOVE: return S_MOVE_DISC;
    default: return S_WAIT;
  }
}

uint16_t expeditionItemColor(ExpeditionItem item) {
  switch (item) {
    case EXP_ITEM_SNACK: return UI_BAR_WARN;
    case EXP_ITEM_ENERGY: return 0x4C98;
    case EXP_ITEM_CARE: return UI_BAR_OK;
    case EXP_ITEM_TRAIN: return UI_BAR_BAD;
    case EXP_ITEM_MOVE: return 0xF81F;
    default: return UI_TRACK;
  }
}

void drawExpeditionItem(int x, int y, ExpeditionItem item) {
  const int w = 172, h = 54;
  uint8_t count = pet.itemCounts[item];
  uint16_t col = expeditionItemColor(item);
  gfx->fillRoundRect(x, y, w, h, 9, count ? UI_WHITE : C565(0xe4, 0xe8, 0xee));
  gfx->drawRoundRect(x, y, w, h, 9, count ? col : UI_TRACK);
  gfx->fillCircle(x + 22, y + 27, 12, count ? col : UI_TRACK);
  if (item == EXP_ITEM_ENERGY) gfx->fillRect(x + 20, y + 17, 5, 20, UI_WHITE);
  else if (item == EXP_ITEM_CARE) gfx->fillCircle(x + 22, y + 22, 4, UI_WHITE);
  else if (item == EXP_ITEM_TRAIN) gfx->fillRect(x + 16, y + 25, 12, 4, UI_WHITE);

  const char *label = T(expeditionItemText(item));
  gfx->setTextSize(1);
  gfx->setTextColor(count ? UI_INK : UI_TRACK);
  gfx->setCursor(x + 42, y + 16);
  gfx->print(label);
  char amount[6];
  snprintf(amount, sizeof(amount), "x%u", count);
  gfx->setTextSize(2);
  gfx->setCursor(x + 136, y + 29);
  gfx->print(amount);
}

void renderExpeditionTrainChoice() {
  gfx->fillRoundRect(58, 118, 350, 190, 14, UI_WHITE);
  gfx->drawRoundRect(58, 118, 350, 190, 14, UI_INK);
  const char *title = T(S_ITEM_TRAIN);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(2);
  gfx->setCursor(CX - gfx->textWidth(title) / 2, 136);
  gfx->print(title);

  const StrId labels[3] = { S_TRAIN_ATK, S_TRAIN_DEF, S_TRAIN_SPE };
  const uint8_t values[3] = { pet.trAtk, pet.trDef, pet.trSpe };
  const uint16_t cols[3] = { UI_BAR_BAD, 0x4C98, UI_BAR_WARN };
  for (uint8_t i = 0; i < 3; i++) {
    int x = 74 + i * 108;
    bool usable = values[i] < 100;
    gfx->fillRoundRect(x, 172, 102, 66, 9, usable ? cols[i] : UI_TRACK);
    gfx->setTextColor(usable ? UI_INK : UI_WHITE);
    gfx->setTextSize(2);
    const char *label = T(labels[i]);
    gfx->setCursor(x + (102 - gfx->textWidth(label)) / 2, 184);
    gfx->print(label);
    if (usable) {
      gfx->setTextSize(1);
      gfx->setCursor(x + 37, 210);
      gfx->print("+2");
    } else {
      const char *maxed = T(S_ITEM_MAXED);
      gfx->setTextSize(1);
      gfx->setCursor(x + (102 - gfx->textWidth(maxed)) / 2, 210);
      gfx->print(maxed);
    }
  }
  gfx->setTextColor(UI_TRACK);
  gfx->setTextSize(2);
  gfx->setCursor(CX - gfx->textWidth(T(S_BACK)) / 2, 268);
  gfx->print(T(S_BACK));
}

// 새 허브의 아이템 페이지. 시간제 탐험과 산책 시작 버튼은 제거하고
// 소지품 확인/사용만 담당한다.
void renderItemsHub() {
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(3);
  const char *title = "아이템";
  gfx->setCursor(CX - gfx->textWidth(title) / 2, 44);
  gfx->print(title);
  gfx->setTextColor(UI_TRACK);
  gfx->setTextSize(2);
  const char *hint = "사용할 아이템을 선택하세요";
  gfx->setCursor(CX - gfx->textWidth(hint) / 2, 78);
  gfx->print(hint);

  drawExpeditionItem(50, 112, EXP_ITEM_SNACK);
  drawExpeditionItem(244, 112, EXP_ITEM_ENERGY);
  drawExpeditionItem(50, 176, EXP_ITEM_CARE);
  drawExpeditionItem(244, 176, EXP_ITEM_TRAIN);

  char discs[36];
  snprintf(discs, sizeof(discs), "%s  x%u", T(S_MOVE_DISC), pet.itemCounts[EXP_ITEM_MOVE]);
  gfx->fillRoundRect(112, 256, 242, 54, 11, UI_WHITE);
  gfx->drawRoundRect(112, 256, 242, 54, 11, UI_BAR_WARN);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(2);
  gfx->setCursor(CX - gfx->textWidth(discs) / 2, 273);
  gfx->print(discs);
  gfx->setTextColor(UI_TRACK);
  gfx->setTextSize(2);
  const char *moveHint = "기술 페이지에서 사용";
  gfx->setCursor(CX - gfx->textWidth(moveHint) / 2, 326);
  gfx->print(moveHint);
  if (expeditionTrainChoiceOpen) renderExpeditionTrainChoice();
}

int8_t hubItemAt(int16_t x, int16_t y) {
  if ((y < 112 || y > 166) && (y < 176 || y > 230)) return -1;
  bool left = x >= 50 && x <= 222;
  bool right = x >= 244 && x <= 416;
  if (!left && !right) return -1;
  return (y >= 176 ? 2 : 0) + (right ? 1 : 0);
}

void itemsHubTap(int16_t x, int16_t y) {
  if (expeditionTrainChoiceOpen) {
    if (y >= 172 && y <= 238 && x >= 74 && x <= 398) {
      int8_t stat = (x - 74) / 108;
      if (stat > TRAIN_STAT_SPE || !pet.useExpeditionItem(EXP_ITEM_TRAIN, stat)) sfxPlay(SFX_DENY);
      else sfxPlay(SFX_ITEM_USE);
    } else {
      sfxPlay(SFX_TAP);
    }
    expeditionTrainChoiceOpen = false;
    hubDirty = true;
    lockTouchBrief();
    return;
  }
  if (y >= 396) {
    hubOpen = false;
    markUiDirty();
    lockTouchBrief();
    sfxPlay(SFX_TAP);
    return;
  }
  if (x >= 112 && x <= 354 && y >= 256 && y <= 310) {
    sfxPlay(SFX_DENY);
    return;
  }
  int8_t item = hubItemAt(x, y);
  if (item < 0) return;
  ExpeditionItem selected = (ExpeditionItem)item;
  if (pet.itemCounts[selected] == 0) {
    sfxPlay(SFX_DENY);
    return;
  }
  if (selected == EXP_ITEM_TRAIN) {
    expeditionTrainChoiceOpen = true;
    hubDirty = true;
    sfxPlay(SFX_MENU);
  } else if (pet.useExpeditionItem(selected)) {
    hubDirty = true;
    sfxPlay(SFX_ITEM_USE);
    lockTouchBrief();
  } else {
    sfxPlay(SFX_DENY);
  }
}

uint8_t pairingSpriteAction(PmdMon &sprite, uint8_t preferred, uint8_t fallback) {
  if (sprite.has(preferred)) return preferred;
  if (sprite.has(fallback)) return fallback;
  return PMD_IDLE;
}

void startPairingEvent(const CommunicationPetData &peer, uint8_t shinyRoll) {
  if (pairingEventActive || pet.isEgg()) return;

  pairingLocalSpecies = pet.speciesId;
  pairingPeerSpecies = peer.species;
  pairingLocalShiny = pet.shiny;
  pairingPeerShiny = (peer.flags & COMM_PET_SHINY) != 0;

  // The normal PMD remains the local actor. Load only the visitor, avoiding a
  // second copy of the potentially large local sprite in PSRAM.
  if (!pmd.loaded && pairingLocalSpecies >= 1)
    pmd.load((uint8_t)pairingLocalSpecies, pairingLocalShiny);
  friendPmd.unload();
  if (pairingPeerSpecies >= 1)
    friendPmd.load((uint8_t)pairingPeerSpecies, pairingPeerShiny);

  // Persist the irreversible result first. If power is lost during the
  // ceremony, both devices still resume safely with the new egg.
  pet.createCommunicationEgg(shinyRoll);
  if (!pet.isEgg()) {
    friendPmd.unload();
    return;
  }

  pairingEventActive = true;
  pairingEventStartedAt = millis();
  // A custom 15.5 s soundtrack is optional. When it is absent, keep the
  // ceremony lively with the firmware's synthesized heart/firework/egg cues.
  // The optional user BGM is independent from the event sound effects.
  // Keep the heart, fireworks and hatch cues even when a custom track exists.
  pairingSoundCue = 0;
  if (pairingMusicAvailable()) pairingMusicPlay();
  cardOpen = false;
  // The communication page lives inside the battle hub. Close the parent as
  // well so the completed ceremony reveals the newly created egg at home.
  hubOpen = false;
  hubPage = 0;
  powerMenuOpen = false;
  manualSleepRequested = false;
  screenOff = false;
  screenOffAt = 0;
  lastInteract = pairingEventStartedAt;
  wasPressed = false;
  swallowGesture = true;
  markUiDirty();
}

void finishPairingEvent(uint32_t now) {
  if (!pairingEventActive) return;
  pairingEventActive = false;
  pairingEventStartedAt = 0;
  pairingSoundCue = 0;
  friendPmd.unload();
  if (communicationState() != COMM_OFF) communicationStop();
  commSelectedMode = COMM_MODE_NONE;
  commDone = false;
  commFailed = false;
  commStartedAt = 0;
  commResultSentAt = 0;
  cardOpen = false;
  hubOpen = false;
  hubPage = 0;
  screenOff = false;
  screenOffAt = 0;
  lastInteract = now;
  ignoreTouchUntil = now + 500UL;
  swallowGesture = true;
  markUiDirty();
}

void updatePairingEvent(uint32_t now) {
  if (!pairingEventActive) return;
  // updateCommunication() may start the ceremony after this loop iteration's
  // `now` snapshot was taken. On real hardware that makes `now` a few
  // milliseconds older than pairingEventStartedAt; unsigned subtraction then
  // looked like 49 days and ended the 14.5 s ceremony immediately. Wait for
  // the next fresh tick instead. The signed delta also remains wrap-safe for
  // this short animation.
  const int32_t elapsedSigned = (int32_t)(now - pairingEventStartedAt);
  if (elapsedSigned < 0) return;
  const uint32_t elapsed = (uint32_t)elapsedSigned;
  lastInteract = now;
  screenOff = false;
  screenOffAt = 0;

  // Retry a cue until the non-blocking audio queue accepts it. This prevents
  // a busy codec from silently dropping the kiss or fireworks sound.
  if (pairingSoundCue == 0 && elapsed >= 2700UL && sfxPlay(SFX_HEART)) pairingSoundCue = 1;
  else if (pairingSoundCue == 1 && elapsed >= 6200UL && sfxPlay(SFX_EVENT_SPARKLE)) pairingSoundCue = 2;
  else if (pairingSoundCue == 2 && elapsed >= 7900UL && sfxPlay(SFX_EVENT_SPARKLE)) pairingSoundCue = 3;
  else if (pairingSoundCue == 3 && elapsed >= 9600UL && sfxPlay(SFX_EVENT_SPARKLE)) pairingSoundCue = 4;
  else if (pairingSoundCue == 4 && elapsed >= 12100UL && sfxPlay(SFX_HATCH)) pairingSoundCue = 5;

  if (elapsed >= PAIRING_EVENT_MS) finishPairingEvent(now);
}

void drawPairingHeart(int16_t x, int16_t y, uint8_t size, uint16_t color) {
  if (size < 3) size = 3;
  const int16_t r = size / 2;
  gfx->fillCircle(x - r, y, r, color);
  gfx->fillCircle(x + r, y, r, color);
  gfx->fillTriangle(x - size, y, x + size, y, x, y + size + r, color);
}

void drawPairingFirework(uint32_t elapsed, uint32_t startAt, int16_t cx,
                         int16_t cy, uint16_t color) {
  if (elapsed < startAt) return;
  const uint32_t age = elapsed - startAt;
  if (age > 1900UL) return;
  static const int8_t ray[16][2] = {
    { 10, 0 }, { 9, 4 }, { 7, 7 }, { 4, 9 }, { 0, 10 }, { -4, 9 }, { -7, 7 }, { -9, 4 },
    { -10, 0 }, { -9, -4 }, { -7, -7 }, { -4, -9 }, { 0, -10 }, { 4, -9 }, { 7, -7 }, { 9, -4 }
  };
  const uint32_t growAge = age > 950UL ? 950UL : age;
  const int16_t radius = 4 + (int16_t)(growAge * 68UL / 950UL);
  const int16_t fall = age > 800UL ? (int16_t)((age - 800UL) * (age - 800UL) / 42000UL) : 0;
  uint16_t spark = age > 1200UL ? lerp565(color, C565(8, 9, 27), (int)(age - 1200UL), 700) : color;
  for (uint8_t i = 0; i < 16; ++i) {
    const int16_t ex = cx + ray[i][0] * radius / 10;
    const int16_t ey = cy + ray[i][1] * radius / 10 + fall;
    const int16_t innerRadius = radius > 16 ? radius - 14 : 2;
    const int16_t sx = cx + ray[i][0] * innerRadius / 10;
    const int16_t sy = cy + ray[i][1] * innerRadius / 10 + fall / 2;
    gfx->drawLine(sx, sy, ex, ey, spark);
    gfx->drawLine(sx + 1, sy, ex + 1, ey, spark);
    gfx->fillCircle(ex, ey, age < 1200UL ? 3 : 2, spark);
  }
  if (age < 650UL) gfx->fillCircle(cx, cy, 4, UI_WHITE);
}

void renderPairingEvent() {
  const uint32_t elapsed = millis() - pairingEventStartedAt;
  const uint16_t skyTop = C565(5, 8, 28);
  const uint16_t skyBottom = elapsed < 6500UL ? C565(102, 43, 108) : C565(32, 22, 68);
  for (int16_t y = 0; y < 466; y += 16) {
    gfx->fillRect(0, y, 466, 16, lerp565(skyTop, skyBottom, y, 466));
  }

  static const int16_t stars[24][2] = {
    { 48, 72 }, { 86, 118 }, { 129, 55 }, { 176, 103 }, { 222, 48 }, { 269, 91 },
    { 318, 62 }, { 367, 112 }, { 414, 76 }, { 63, 181 }, { 111, 153 }, { 158, 198 },
    { 205, 146 }, { 251, 183 }, { 299, 139 }, { 344, 194 }, { 401, 162 }, { 34, 244 },
    { 92, 224 }, { 147, 260 }, { 284, 239 }, { 337, 273 }, { 389, 231 }, { 431, 286 }
  };
  for (uint8_t i = 0; i < 24; ++i) {
    const uint8_t r = ((elapsed / 240UL + i) % 5U == 0U) ? 2 : 1;
    gfx->fillCircle(stars[i][0], stars[i][1], r, i % 3 ? UI_WHITE : C565(255, 226, 151));
  }

  // Moving the horizon and actors down together reads as a camera tilt up.
  int16_t cameraDrop = 0;
  if (elapsed > 5000UL) {
    uint32_t pan = elapsed - 5000UL;
    if (pan > 3200UL) pan = 3200UL;
    cameraDrop = (int16_t)(pan * 190UL / 3200UL);
  }
  const int16_t horizon = 350 + cameraDrop;
  if (horizon < 466) {
    gfx->fillRect(0, horizon, 466, 466 - horizon, C565(55, 39, 66));
    gfx->fillRect(0, horizon, 466, 4, C565(235, 125, 145));
  }

  int16_t localX = 216;
  int16_t peerX = 250;
  uint8_t localAct = pairingSpriteAction(pmd, PMD_WALKR, PMD_IDLE);
  uint8_t peerAct = pairingSpriteAction(friendPmd, PMD_WALKL, PMD_IDLE);
  bool loop = elapsed < 3000UL;
  if (elapsed < 3000UL) {
    const int16_t progress = (int16_t)(elapsed * 1000UL / 3000UL);
    localX = 74 + (142 * progress / 1000);
    peerX = 392 - (142 * progress / 1000);
  } else {
    localAct = pairingSpriteAction(pmd, PMD_NOD, PMD_POSE);
    peerAct = pairingSpriteAction(friendPmd, PMD_NOD, PMD_POSE);
  }
  const int16_t groundY = 348 + cameraDrop;
  if (groundY < 520) {
    if (pmd.loaded) drawPmdActM(pmd, localAct, localX, groundY, elapsed, loop, false, 3);
    if (friendPmd.loaded) drawPmdActM(friendPmd, peerAct, peerX, groundY,
                                     elapsed + 130UL, loop, false, 3);
  }

  // A stream of differently sized hearts rises from the kiss into the sky and
  // remains visible while the camera begins to tilt upward.
  if (elapsed >= 2750UL && elapsed < 7600UL) {
    static const int8_t drift[8] = { -38, 30, -12, 46, 8, -52, 24, -25 };
    for (uint8_t i = 0; i < 8; ++i) {
      const uint32_t born = 2750UL + i * 410UL;
      if (elapsed < born || elapsed - born > 1700UL) continue;
      const uint32_t age = elapsed - born;
      const int16_t hx = CX + drift[i] * (int16_t)age / 1700;
      const int16_t hy = 244 - (int16_t)(age * 96UL / 1700UL) + cameraDrop / 4;
      drawPairingHeart(hx, hy, (uint8_t)(7 + (i % 3) * 2),
                       i % 2 ? C565(255, 91, 148) : C565(255, 174, 208));
    }
  }

  drawPairingFirework(elapsed, 6100UL, 130, 142, C565(255, 109, 160));
  drawPairingFirework(elapsed, 7600UL, 326, 119, C565(105, 205, 255));
  drawPairingFirework(elapsed, 9000UL, 228, 206, C565(255, 211, 91));
  drawPairingFirework(elapsed, 10400UL, 92, 251, C565(159, 126, 255));
  drawPairingFirework(elapsed, 11100UL, 374, 239, C565(110, 238, 169));

  if (elapsed >= 12100UL) {
    gfx->fillRoundRect(64, 350, 338, 58, 16, C565(246, 238, 252));
    gfx->drawRoundRect(64, 350, 338, 58, 16, C565(255, 137, 188));
    gfx->setTextColor(UI_INK);
    gfx->setTextSize(2);
    const char *message = T(S_COMM_SUCCESS);
    gfx->setCursor(CX - gfx->textWidth(message) / 2, 371);
    gfx->print(message);
  }
  gfx->flush();
}

CommunicationPetData localCommunicationPet(CommunicationMode selectedMode) {
  CommunicationPetData data;
  data.mode = selectedMode;
  data.species = (uint8_t)pet.speciesId;
  data.level = pet.level();
  BattleStats battle = petBattleStats();
  data.atk = battle.atk;
  data.def = battle.def;
  data.spe = battle.spe;
  data.hp = battle.hp;
  data.move1 = (uint8_t)pet.battleMove1;
  uint32_t days = pet.ageMinutes / 1440UL;
  data.ageDays = days > 255 ? 255 : (uint8_t)days;
  data.flags = pet.shiny ? COMM_PET_SHINY : 0;
  data.nonce = (uint32_t)random(0x7fffffffL) ^ rtcEpoch();
  return data;
}

void resetCommunicationUi() {
  if (communicationState() != COMM_OFF) communicationStop();
  commSelectedMode = COMM_MODE_NONE;
  commDone = false;
  commWon = false;
  commFailed = false;
  commStartedAt = 0;
  commResultSentAt = 0;
  commPeerSpecies = 0;
  commRounds = 0;
  commReward = {};
  commPeerData = {};
  commLocalNonce = 0;
  commTransaction = 0;
  commPeerActionReady = false;
  commPendingResultReady = false;
  commPendingResultAt = 0;
  cardDirty = true;
  hubDirty = true;
}

void beginCommunicationRole(CommunicationRole selectedRole) {
  if (commSelectedMode == COMM_MODE_NONE) return;
  CommunicationPetData local = localCommunicationPet(commSelectedMode);
  commLocalNonce = local.nonce;
  bool started = selectedRole == COMM_ROLE_HOST
                     ? communicationStartHost(commSelectedMode, local)
                     : communicationStartGuest(commSelectedMode, local);
  commStartedAt = millis();
  commFailed = !started;
  commDone = false;
  commResultSentAt = 0;
  commPeerActionReady = false;
  commPendingResultReady = false;
  commPendingResultAt = 0;
  cardDirty = true;
  hubDirty = true;
  sfxPlay(started ? SFX_MENU : SFX_DENY);
}

CommunicationBattleTurnData communicationTurnPacket(const LinkBattleTurnResult &turn,
                                                     BattleAction hostAction,
                                                     BattleAction guestAction) {
  CommunicationBattleTurnData packet;
  packet.round = battleRun.round;
  packet.hostAction = (uint8_t)hostAction;
  packet.guestAction = (uint8_t)guestAction;
  packet.hostRestLeft = battleRun.restUsesLeft;
  packet.guestRestLeft = battleRun.enemyRestUsesLeft;
  packet.hostSkill1Left = battleRun.skill1UsesLeft;
  packet.hostSkill2Left = battleRun.skill2UsesLeft;
  packet.guestSkill1Left = battleRun.enemySkill1UsesLeft;
  packet.guestSkill2Left = battleRun.enemySkill2UsesLeft;
  packet.hostTypePct = turn.host.playerTypePct;
  packet.guestTypePct = turn.guest.playerTypePct;
  packet.hostHp = battleRun.playerHp;
  packet.guestHp = battleRun.enemyHp;
  packet.hostDamage = turn.host.playerDamage;
  packet.guestDamage = turn.guest.playerDamage;
  packet.hostHeal = turn.host.playerHeal;
  packet.guestHeal = turn.guest.playerHeal;
  packet.transaction = commTransaction;
  if (turn.host.playerDodged) packet.flags |= COMM_TURN_HOST_DODGED;
  if (turn.guest.playerDodged) packet.flags |= COMM_TURN_GUEST_DODGED;
  if (turn.host.playerActedFirst) packet.flags |= COMM_TURN_HOST_FIRST;
  if (turn.host.playerForfeited) packet.flags |= COMM_TURN_HOST_FORFEIT;
  if (turn.guest.playerForfeited) packet.flags |= COMM_TURN_GUEST_FORFEIT;
  if (turn.guest.enemyActed) packet.flags |= COMM_TURN_HOST_ACTED;
  if (turn.host.battleEnded) packet.flags |= COMM_TURN_ENDED;
  if (turn.host.playerWon) packet.flags |= COMM_TURN_HOST_WON;
  return packet;
}

void resolveCommunicationHostTurn(uint32_t now) {
  if (!battleCommunication || !battleCommHost || battleResolved ||
      battleAnimStage != BATTLE_ANIM_NONE ||
      !battleCommLocalActionReady || !commPeerActionReady) return;
  if (commPeerAction.transaction != commTransaction ||
      commPeerAction.round != battleRun.round + 1 || commPeerAction.action > BATTLE_RUN) {
    commPeerActionReady = false;
    return;
  }

  BattleAction hostAction = battleCommLocalAction;
  BattleAction guestAction = (BattleAction)commPeerAction.action;
  uint16_t hostHpBefore = battleRun.playerHp;
  uint16_t guestHpBefore = battleRun.enemyHp;
  uint8_t luck = (uint8_t)((commTransaction + battleRun.round * 37UL +
                            (uint8_t)hostAction * 11UL + (uint8_t)guestAction * 19UL) % 100UL);
  LinkBattleTurnResult turn = stepLinkBattle(battleRun, hostAction, guestAction, luck);
  CommunicationBattleTurnData packet = communicationTurnPacket(turn, hostAction, guestAction);
  if (!communicationSendBattleTurn(packet)) {
    commFailed = true;
    battleCommWaiting = false;
    battleCommLocalActionReady = false;
    commPeerActionReady = false;
    snprintf(battleMsg, sizeof(battleMsg), "%s", T(S_COMM_FAILED));
    battleDirty = true;
    sfxPlay(SFX_DENY);
    return;
  }

  battleTurn = turn.host;
  commPeerActionReady = false;
  beginCommunicationBattleAnimation(hostAction, guestAction, hostHpBefore, guestHpBefore);

  if (turn.host.battleEnded) {
    commWon = turn.host.playerWon;
    commRounds = battleRun.round;
    commReward = pet.applyCommunicationBattleReward(commWon, commHostItemRoll);
    commDone = true;
    commPendingResult = {};
    commPendingResult.mode = COMM_MODE_BATTLE;
    commPendingResult.hostWon = commWon ? 1 : 0;
    commPendingResult.hostItemRoll = commHostItemRoll;
    commPendingResult.guestItemRoll = commGuestItemRoll;
    commPendingResult.rounds = commRounds;
    commPendingResult.transaction = commTransaction;
    commPendingResultReady = true;
    commPendingResultAt = now + 180UL;
  }
}

void updateCommunication(uint32_t now) {
  if (communicationState() == COMM_OFF) return;
  communicationPoll();

  static CommunicationState previousState = COMM_OFF;
  CommunicationState currentState = communicationState();
  if (currentState != previousState) {
    previousState = currentState;
    cardDirty = true;
    hubDirty = true;
  }

  if (currentState == COMM_ERROR) {
    commFailed = true;
    commDone = false;
    if (battleCommunication) {
      battleAudioEnd();
      battleOpen = false;
      battleCommunication = false;
      wildPmd.unload();
      hubOpen = true;
      hubPage = 3;
    }
    communicationStop();
    cardDirty = true;
    hubDirty = true;
    sfxPlay(SFX_DENY);
    return;
  }

  CommunicationRole role = communicationRole();
  CommunicationPetData peer;
  if (communicationTakePeer(peer)) {
    commPeerData = peer;
    commPeerSpecies = peer.species;
    commTransaction = commLocalNonce ^ peer.nonce ^ 0x504F4B45UL;
    pet.registerLinked(peer.species);
    galleryDirty = true;

    if (commSelectedMode == COMM_MODE_BATTLE) {
      if (role == COMM_ROLE_HOST) {
        commHostItemRoll = (uint8_t)random(100);
        commGuestItemRoll = (uint8_t)random(100);
      }
      commStartedAt = 0;
      startCommunicationBattle(peer, role == COMM_ROLE_HOST);
    } else if (role == COMM_ROLE_HOST) {
      CommunicationResultData result;
      result.mode = COMM_MODE_PAIRING;
      result.hostShinyRoll = (uint8_t)random(4);
      result.guestShinyRoll = (uint8_t)random(4);
      result.transaction = commTransaction;
      if (pet.ageMinutes < FAREWELL_AGE_MIN || peer.ageDays < 3) {
        result.hostWon = 2;
        commFailed = true;
      } else {
        result.hostWon = 1;
      }
      if (!communicationSendResult(result)) commFailed = true;
      else {
        commDone = !commFailed;
        commResultSentAt = now;
        if (!commFailed) startPairingEvent(peer, result.hostShinyRoll);
      }
      cardDirty = true;
      hubDirty = true;
    }
  }

  CommunicationBattleActionData action;
  if (role == COMM_ROLE_HOST && communicationTakeBattleAction(action)) {
    if (battleCommunication && action.transaction == commTransaction &&
        action.round == battleRun.round + 1 && action.action <= BATTLE_RUN) {
      commPeerAction = action;
      commPeerActionReady = true;
    }
  }
  resolveCommunicationHostTurn(now);

  CommunicationBattleTurnData turnPacket;
  if (role == COMM_ROLE_GUEST && communicationTakeBattleTurn(turnPacket)) {
    applyCommunicationBattleTurn(turnPacket);
  }

  CommunicationResultData result;
  if (role == COMM_ROLE_GUEST && communicationTakeResult(result)) {
    if (result.hostWon > 1) {
      commFailed = true;
      sfxPlay(SFX_DENY);
    } else if (result.mode == COMM_MODE_BATTLE) {
      commWon = result.hostWon == 0;
      commRounds = result.rounds;
      commReward = pet.applyCommunicationBattleReward(commWon, result.guestItemRoll);
      commDone = true;
    } else {
      commDone = true;
      startPairingEvent(commPeerData, result.guestShinyRoll);
    }
    communicationStop();
    cardDirty = true;
    hubDirty = true;
  }

  if (role == COMM_ROLE_HOST && commPendingResultReady &&
      (int32_t)(now - commPendingResultAt) >= 0) {
    if (communicationSendResult(commPendingResult)) {
      commPendingResultReady = false;
      commResultSentAt = now;
    } else {
      commPendingResultReady = false;
      commFailed = true;
    }
  }

  // Keep the host radio alive briefly after the final notification.  The
  // pre-battle room times out, but a player may take as long as needed per turn.
  if (commResultSentAt && now - commResultSentAt >= 1500UL) {
    communicationStop();
    cardDirty = true;
    hubDirty = true;
  } else if (commStartedAt && now - commStartedAt >= 60000UL && !commDone && !battleCommunication) {
    commFailed = true;
    communicationStop();
    cardDirty = true;
    hubDirty = true;
    sfxPlay(SFX_DENY);
  }
}

void renderCommunicationCard() {
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(3);
  const char *title = T(S_COMMUNICATION);
  gfx->setCursor(CX - gfx->textWidth(title) / 2, 44);
  gfx->print(title);

  if (commDone) {
    const char *message = commSelectedMode == COMM_MODE_PAIRING
                              ? T(S_COMM_SUCCESS)
                              : (commWon ? T(S_WIN) : T(S_LOSS));
    gfx->setTextColor(commWon || commSelectedMode == COMM_MODE_PAIRING ? UI_BAR_OK : UI_BAR_BAD);
    gfx->setTextSize(3);
    gfx->setCursor(CX - gfx->textWidth(message) / 2, 126);
    gfx->print(message);
    if (commSelectedMode == COMM_MODE_BATTLE) {
      char rounds[22];
      snprintf(rounds, sizeof(rounds), T(S_ROUNDS_FMT), commRounds);
      gfx->setTextColor(UI_INK);
      gfx->setTextSize(2);
      gfx->setCursor(CX - gfx->textWidth(rounds) / 2, 182);
      gfx->print(rounds);
      if (commReward.item != EXP_ITEM_NONE) {
        char reward[34];
        snprintf(reward, sizeof(reward), "%s x%u",
                 T(expeditionItemText(commReward.item)), commReward.amount);
        gfx->setCursor(CX - gfx->textWidth(reward) / 2, 226);
        gfx->print(reward);
      }
    }
    return;
  }

  if (commFailed) {
    gfx->setTextColor(UI_BAR_BAD);
    gfx->setTextSize(3);
    const char *failed = T(S_COMM_FAILED);
    gfx->setCursor(CX - gfx->textWidth(failed) / 2, 148);
    gfx->print(failed);
    gfx->setTextColor(UI_TRACK);
    gfx->setTextSize(1);
    const char *detail = communicationError();
    if (detail && detail[0]) {
      gfx->setCursor(CX - gfx->textWidth(detail) / 2, 194);
      gfx->print(detail);
    }
    return;
  }

  if (communicationState() != COMM_OFF) {
    CommunicationState state = communicationState();
    const char *status = state == COMM_ADVERTISING ? T(S_COMM_WAIT) : T(S_COMM_CONNECTING);
    gfx->setTextColor(0x4C98);
    gfx->setTextSize(3);
    gfx->setCursor(CX - gfx->textWidth(status) / 2, 150);
    gfx->print(status);
    gfx->setTextColor(UI_TRACK);
    gfx->setTextSize(2);
    const char *modeText = commSelectedMode == COMM_MODE_BATTLE ? T(S_COMM_BATTLE) : T(S_COMM_PAIRING);
    gfx->setCursor(CX - gfx->textWidth(modeText) / 2, 206);
    gfx->print(modeText);
    return;
  }

  if (commSelectedMode == COMM_MODE_NONE) {
    gfx->fillRoundRect(58, 126, 350, 76, 15, UI_BAR_BAD);
    gfx->fillRoundRect(58, 220, 350, 76, 15,
                       pet.ageMinutes >= FAREWELL_AGE_MIN ? C565(0xf0, 0x9a, 0xc2) : UI_TRACK);
    gfx->setTextColor(UI_INK);
    gfx->setTextSize(3);
    gfx->setCursor(CX - gfx->textWidth(T(S_COMM_BATTLE)) / 2, 150);
    gfx->print(T(S_COMM_BATTLE));
    gfx->setCursor(CX - gfx->textWidth(T(S_COMM_PAIRING)) / 2, 244);
    gfx->print(T(S_COMM_PAIRING));
    if (pet.ageMinutes < FAREWELL_AGE_MIN) {
      gfx->setTextColor(UI_TRACK);
      gfx->setTextSize(2);
      const char *locked = T(S_COMM_PAIR_LOCKED);
      gfx->setCursor(CX - gfx->textWidth(locked) / 2, 308);
      gfx->print(locked);
    }
  } else {
    gfx->setTextColor(UI_INK);
    gfx->setTextSize(2);
    const char *modeText = commSelectedMode == COMM_MODE_BATTLE ? T(S_COMM_BATTLE) : T(S_COMM_PAIRING);
    gfx->setCursor(CX - gfx->textWidth(modeText) / 2, 104);
    gfx->print(modeText);
    gfx->fillRoundRect(58, 142, 350, 76, 15, UI_BAR_OK);
    gfx->fillRoundRect(58, 236, 350, 76, 15, 0x4C98);
    gfx->setTextColor(UI_INK);
    gfx->setTextSize(3);
    gfx->setCursor(CX - gfx->textWidth(T(S_COMM_HOST)) / 2, 166);
    gfx->print(T(S_COMM_HOST));
    gfx->setCursor(CX - gfx->textWidth(T(S_COMM_JOIN)) / 2, 260);
    gfx->print(T(S_COMM_JOIN));
  }
}

void communicationCardTap(int16_t x, int16_t y) {
  if (y >= 396) {
    resetCommunicationUi();
    hubOpen = false;
    markUiDirty();
    lockTouchBrief();
    sfxPlay(SFX_TAP);
    return;
  }
  if (commDone || commFailed) {
    resetCommunicationUi();
    sfxPlay(SFX_TAP);
    return;
  }
  if (communicationState() != COMM_OFF) {
    if (y >= 310) resetCommunicationUi();
    return;
  }
  if (commSelectedMode == COMM_MODE_NONE) {
    if (x >= 58 && x <= 408 && y >= 126 && y <= 202) {
      commSelectedMode = COMM_MODE_BATTLE;
      hubDirty = true;
      sfxPlay(SFX_MENU);
    } else if (x >= 58 && x <= 408 && y >= 220 && y <= 296) {
      if (pet.ageMinutes >= FAREWELL_AGE_MIN) {
        commSelectedMode = COMM_MODE_PAIRING;
        hubDirty = true;
        sfxPlay(SFX_MENU);
      } else sfxPlay(SFX_DENY);
    }
    return;
  }
  if (x >= 58 && x <= 408 && y >= 142 && y <= 218) beginCommunicationRole(COMM_ROLE_HOST);
  else if (x >= 58 && x <= 408 && y >= 236 && y <= 312) beginCommunicationRole(COMM_ROLE_GUEST);
  else {
    commSelectedMode = COMM_MODE_NONE;
    hubDirty = true;
  }
  lockTouchBrief();
}

void renderBattleHub() {
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(4);
  const char *title = T(S_WILD_BATTLE);
  gfx->setCursor(CX - gfx->textWidth(title) / 2, 52);
  gfx->print(title);

  char wl[28], streak[28];
  snprintf(wl, sizeof(wl), T(S_WL_FMT), pet.battleWins, pet.battleLosses);
  snprintf(streak, sizeof(streak), "%s %u", T(S_RECORDS), pet.bestBattleStreak);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(3);
  gfx->setCursor(CX - gfx->textWidth(wl) / 2, 108);
  gfx->print(wl);
  gfx->setCursor(CX - gfx->textWidth(streak) / 2, 146);
  gfx->print(streak);

  gfx->fillRoundRect(72, 190, 322, 78, 16, UI_BAR_BAD);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(4);
  const char *start = "배틀 시작";
  gfx->setCursor(CX - gfx->textWidth(start) / 2, 212);
  gfx->print(start);
  gfx->setTextColor(UI_TRACK);
  gfx->setTextSize(3);
  const char *hint = "야생 포켓몬과 겨룹니다";
  gfx->setCursor(CX - gfx->textWidth(hint) / 2, 300);
  gfx->print(hint);
}

void drawPageFooter(uint8_t page, uint8_t count) {
  int dotsX = CX - ((count - 1) * 24) / 2;
  for (uint8_t i = 0; i < count; i++) {
    if (i == page) gfx->fillCircle(dotsX + i * 24, 400, 5, UI_INK);
    else gfx->drawCircle(dotsX + i * 24, 400, 4, UI_INK);
  }
  gfx->setTextColor(UI_TRACK);
  gfx->setTextSize(2);
  gfx->setCursor(CX - gfx->textWidth(T(S_BACK)) / 2, 420);
  gfx->print(T(S_BACK));
}

void renderHub() {
  hubDirty = false;
  gfx->fillScreen(UI_BG_DAY);
  if (hubPage == 0) renderCardDaily();
  else if (hubPage == 1) renderItemsHub();
  else if (hubPage == 2) renderBattleHub();
  else renderCommunicationCard();
  drawPageFooter(hubPage, HUB_COUNT);
  gfx->flush();
}

void renderCard() {
  cardDirty = false;
  gfx->fillScreen(UI_BG_DAY);
  if (cardPage == 0) renderCardProfile();
  else if (cardPage == 1) renderCardPersonality();
  else if (cardPage == 2) renderCardStats();
  else if (cardPage == 3) renderCardMoves();
  else if (cardPage == 4) renderCardProgress();
  else renderCardMedals();
  drawPageFooter(cardPage, CARD_COUNT);
  gfx->flush();
}

// ---------- teclado para renombrar ----------

static const char KB_KEYS[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZ.-";  // 28 + DEL + OK = 30
#define KB_COLS 6
#define KB_X 40
#define KB_Y 150
#define KB_W 64
#define KB_H 52

void openKeyboard() {
  kbOpen = true;
  keyboardDirty = true;
  lockTouchBrief();
  strncpy(nameBuf, pet.nick, sizeof(nameBuf) - 1);
  nameBuf[sizeof(nameBuf) - 1] = 0;
  nameLen = strlen(nameBuf);
  sfxPlay(SFX_MENU);
}

void renderKeyboard() {
  keyboardDirty = false;
  gfx->fillScreen(UI_BG_DAY);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(2);
  gfx->setCursor(CX - gfx->textWidth(T(S_NAME)) / 2, 56);
  gfx->print(T(S_NAME));
  // buffer actual
  gfx->fillRoundRect(83, 84, 300, 40, 8, UI_WHITE);
  gfx->drawRoundRect(83, 84, 300, 40, 8, UI_INK);
  gfx->setTextSize(3);
  gfx->setCursor(95, 94);
  gfx->print(nameLen ? nameBuf : "_");

  for (int i = 0; i < 30; i++) {
    int x = KB_X + (i % KB_COLS) * KB_W, y = KB_Y + (i / KB_COLS) * KB_H;
    bool special = (i >= 28);
    gfx->fillRoundRect(x, y, KB_W - 6, KB_H - 6, 6, special ? UI_BAR_WARN : UI_WHITE);
    gfx->drawRoundRect(x, y, KB_W - 6, KB_H - 6, 6, UI_INK);
    gfx->setTextColor(UI_INK);
    gfx->setTextSize(2);
    if (i < 28) {
      gfx->setCursor(x + KB_W / 2 - 9, y + KB_H / 2 - 10);
      gfx->print(KB_KEYS[i]);
    } else {
      const char *lab = (i == 28) ? "<-" : "OK";
      gfx->setCursor(x + KB_W / 2 - 15, y + KB_H / 2 - 10);
      gfx->print(lab);
    }
  }
  gfx->flush();
}

void keyboardTap(int16_t x, int16_t y) {
  int col = (x - KB_X) / KB_W, row = (y - KB_Y) / KB_H;
  if (col < 0 || col >= KB_COLS || row < 0 || row >= 5) return;
  int i = row * KB_COLS + col;
  if (i >= 30) return;
  sfxPlay(SFX_TAP);
  if (i == 28) {  // borrar
    if (nameLen) nameBuf[--nameLen] = 0;
    keyboardDirty = true;
  } else if (i == 29) {  // OK
    pet.rename(nameBuf);
    kbOpen = false;
    cardDirty = true;
    lockTouchBrief();
  } else if (nameLen < sizeof(nameBuf) - 1) {
    nameBuf[nameLen++] = KB_KEYS[i];
    nameBuf[nameLen] = 0;
    keyboardDirty = true;
  }
}

// ---------- galeria pokedex ----------

#define GAL_X 73
#define GAL_Y 92
#define GAL_CELL 80

bool galleryDexVisible(int16_t dex) {
  if (dex < 1 || dex > 151) return false;
  if (galleryFilter == 1) return pet.isRegistered(dex);
  if (galleryFilter == 2) return pet.isCaught(dex);
  if (galleryFilter == 3) return pet.isLinked(dex);
  return true;
}

uint16_t galleryFilteredCount() {
  if (galleryFilter == 0) return 151;
  uint16_t count = 0;
  for (int16_t dex = 1; dex <= 151; dex++)
    if (galleryDexVisible(dex)) count++;
  return count;
}

int galleryPageCount() {
  uint16_t count = galleryFilteredCount();
  int pages = (count + 15) / 16;
  return pages > 0 ? pages : 1;
}

int16_t galleryDexAt(uint16_t index) {
  for (int16_t dex = 1; dex <= 151; dex++) {
    if (!galleryDexVisible(dex)) continue;
    if (index == 0) return dex;
    index--;
  }
  return 0;
}

// dibuja una miniatura centrada en su celda; sil=true la pinta en tinta
void drawThumb(const uint8_t *b, int x, int y, int s, bool sil) {
  uint8_t w = b[0], h = b[1], n = b[2];
  const uint8_t *pal = b + 3;
  const uint8_t *d = pal + n * 2;
  int ox = x + (GAL_CELL - w * s) / 2;
  int oy = y + (GAL_CELL - h * s) / 2;
  for (int r = 0; r < h; r++) {
    for (int c = 0; c < w; c++) {
      uint8_t idx = d[r * w + c];
      if (idx == 0xFF) continue;
      uint16_t col = sil ? INK_K : (uint16_t)(pal[idx * 2] | (pal[idx * 2 + 1] << 8));
      gfx->fillRect(ox + c * s, oy + r * s, s, s, col);
    }
  }
}

void renderGallery() {
  if (galleryDetail) {  // vista detalle: se redibuja siempre (animada)
    gfx->fillScreen(UI_BG_DAY);
    const DexEntry &d = DEX_TBL[galleryDetail];
    bool reg = pet.isRegistered(galleryDetail);
    bool caught = pet.isCaught(galleryDetail);
    bool linked = pet.isLinked(galleryDetail);
    bool known = pet.isKnown(galleryDetail);
    char head[24];
    snprintf(head, sizeof(head), "N.%03d %s%s", galleryDetail,
             galleryShowShiny ? "*" : "", known ? dexName(galleryDetail) : "???");
    gfx->setTextColor(known ? d.accent : UI_INK);
    gfx->setTextSize(3);
    int gts = gfx->textWidth(head) <= 234 ? 3 : 2;  // 긴 이름은 자동 축소
    gfx->setTextSize(gts);
    gfx->setCursor(CX - gfx->textWidth(head) / 2, gts == 3 ? 56 : 60);
    gfx->print(head);
    if (known) {
      char types[24];
      typeText(types, sizeof(types), d);
      gfx->setTextColor(battleTypeColor(d.type1));
      gfx->setTextSize(2);
      gfx->setCursor(CX - gfx->textWidth(types) / 2, 94);
      gfx->print(types);
    }
    if (galleryPmd.loaded) {
      // animado y a color si se conoce; silueta estatica si no (estilo "?")
      drawPmdActM(galleryPmd, PMD_IDLE, CX, 300, known ? millis() : 0, true, !known, 6);
    } else {
      const uint8_t *t = thumbs.get(galleryDetail);
      if (t) drawThumb(t, CX - GAL_CELL, 135, 4, !known);
    }
    int markCount = (reg ? 1 : 0) + (caught ? 1 : 0) + (linked ? 1 : 0);
    int markY = 366 - (markCount - 1) * 11;
    auto drawGalleryMark = [&](StrId id, uint16_t color) {
      const char *mark = T(id);
      gfx->setTextColor(color);
      gfx->setTextSize(2);
      gfx->setCursor(CX - gfx->textWidth(mark) / 2, markY);
      gfx->print(mark);
      markY += 22;
    };
    if (reg) drawGalleryMark(S_RAISED_MARK, UI_BAR_OK);
    if (caught) drawGalleryMark(S_CAUGHT_MARK, UI_BAR_WARN);
    if (linked) drawGalleryMark(S_LINKED_MARK, 0x4C98);
    gfx->setTextColor(UI_INK);
    gfx->setTextSize(2);
    gfx->setCursor(CX - gfx->textWidth(T(S_DETAIL_BACK)) / 2, 408);
    gfx->print(T(S_DETAIL_BACK));
    gfx->flush();
    return;
  }

  if (!galleryDirty) return;  // la rejilla es estatica
  galleryDirty = false;

  gfx->fillScreen(UI_BG_DAY);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(3);
  gfx->setCursor(CX - 7 * 9, 28);
  gfx->print("POKEDEX");

  char head[38];
  snprintf(head, sizeof(head), "R:%u C:%u L:%u", pet.registeredCount(), pet.caughtCount(), pet.linkedCount());
  gfx->setTextSize(2);
  gfx->setCursor(CX - gfx->textWidth(head) / 2, 54);
  gfx->print(head);

  const char *filters[4] = { T(S_FILTER_ALL), T(S_RAISED_MARK), T(S_CAUGHT_MARK), T(S_LINKED_MARK) };
  for (int i = 0; i < 4; i++) {
    int fx = 52 + i * 91;
    uint16_t fill = (galleryFilter == i) ? UI_INK : UI_WHITE;
    uint16_t text = (galleryFilter == i) ? UI_WHITE : UI_INK;
    gfx->fillRoundRect(fx, 72, 86, 20, 6, fill);
    gfx->drawRoundRect(fx, 72, 86, 20, 6, UI_INK);
    gfx->setTextColor(text);
    gfx->setTextSize(1);
    gfx->setCursor(fx + (86 - gfx->textWidth(filters[i])) / 2, 74);
    gfx->print(filters[i]);
  }

  for (int r = 0; r < 4; r++) {
    for (int c = 0; c < 4; c++) {
      int16_t dex = galleryDexAt(galleryPage * 16 + r * 4 + c);
      if (dex <= 0) continue;
      int x = GAL_X + c * GAL_CELL, y = GAL_Y + r * GAL_CELL;
      const uint8_t *t = thumbs.get(dex);
      if (t) {
        bool reg = pet.isRegistered(dex);
        bool caught = pet.isCaught(dex);
        bool linked = pet.isLinked(dex);
        bool known = pet.isKnown(dex);
        drawThumb(t, x, y, 2, !known);
        if (pet.isShinyRegistered(dex)) {
          gfx->setTextColor(UI_BAR_WARN);
          gfx->setTextSize(2);
          gfx->setCursor(x + 62, y + 4);
          gfx->print("*");
        } else if ((caught || linked) && !reg) {
          gfx->setTextColor(caught ? UI_BAR_WARN : 0x4C98);
          gfx->setTextSize(1);
          gfx->setCursor(x + 60, y + 6);
          gfx->print(caught ? "C" : "L");
        }
      } else {
        char num[6];
        snprintf(num, sizeof(num), "%d", dex);
        gfx->setTextColor(UI_TRACK);
        gfx->setTextSize(2);
        gfx->setCursor(x + 24, y + 32);
        gfx->print(num);
      }
    }
  }
  // puntos de pagina
  int pages = galleryPageCount();
  int dotX = CX - (pages - 1) * 7;
  for (int i = 0; i < pages; i++) {
    if (i == galleryPage) gfx->fillCircle(dotX + i * 14, 436, 4, UI_INK);
    else gfx->drawCircle(dotX + i * 14, 436, 3, UI_INK);
  }
  gfx->flush();
}

void galleryTap(int16_t x, int16_t y) {
  if (galleryDetail) {
    // 이로치를 얻은 포켓몬은 중앙 스프라이트를 터치하면 전환한다.
    if (pet.isShinyRegistered(galleryDetail) &&
        x >= 105 && x <= 361 && y >= 112 && y <= 340) {
      galleryShowShiny = !galleryShowShiny;
      galleryPmd.unload();
      galleryPmd.load(galleryDetail, galleryShowShiny);
      sfxPlay(SFX_MENU);
      return;
    }
    // 스프라이트 바깥을 터치하면 목록으로 돌아간다.
    galleryDetail = 0;
    galleryShowShiny = false;
    galleryPmd.unload();
    galleryDirty = true;
    sfxPlay(SFX_TAP);
    return;
  }
  if (y < 46) {  // tocar la cabecera = salir
    galleryOpen = false;
    galleryPmd.unload();
    sfxPlay(SFX_TAP);
    return;
  }
  if (y >= 68 && y < GAL_Y) {
    int f = (x - 52) / 91;
    if (f >= 0 && f < 4 && x >= 52 + f * 91 && x <= 138 + f * 91) {
      galleryFilter = (uint8_t)f;
      galleryPage = 0;
      galleryDirty = true;
      sfxPlay(SFX_TAP);
      return;
    }
  }
  if (x < GAL_X || y < GAL_Y) return;
  int c = (x - GAL_X) / GAL_CELL, r = (y - GAL_Y) / GAL_CELL;
  if (c < 0 || c > 3 || r < 0 || r > 3) return;
  int16_t dex = galleryDexAt(galleryPage * 16 + r * 4 + c);
  if (dex <= 0) return;
  galleryDetail = dex;
  galleryShowShiny = false;
  galleryPmd.load(dex, false);
  sfxPlay(SFX_MENU);
  if (pet.isKnown(dex)) speciesChirpPlay(dex);
}

void drawBattery() {
  int pc = batPercent();
  if (pc < 0) return;  // sin bateria conectada
  int x = CX - 14, y = 12, w = 24, h = 11;
  bool charging = batCharging();
  uint16_t col = charging ? UI_BAR_OK
                 : (pc >= 40) ? inkColor()
                 : (pc >= 15) ? UI_BAR_WARN
                              : UI_BAR_BAD;
  gfx->drawRoundRect(x, y, w, h, 2, col);
  gfx->fillRect(x + w, y + 3, 3, 5, col);  // borne
  if (charging) {
    // rayo de carga (zigzag) en vez de la barra de nivel
    uint16_t bolt = C565(0xff, 0xd9, 0x4a);
    int bx = x + w / 2;
    gfx->fillTriangle(bx + 3, y + 1, bx - 4, y + 6, bx + 1, y + 6, bolt);
    gfx->fillTriangle(bx - 1, y + 5, bx + 4, y + 5, bx - 3, y + 10, bolt);
  } else {
    int fw = (w - 4) * pc / 100;
    if (fw > 0) gfx->fillRect(x + 2, y + 2, fw, h - 4, col);
  }
}

void drawHeader(const char *name, uint16_t nameColor, const char *msg) {
  drawBattery();
  gfx->setTextColor(nameColor);
  gfx->setTextSize(3);
  gfx->setCursor(CX - gfx->textWidth(name) / 2, 52);
  gfx->print(name);
  gfx->setTextColor(inkColor());
  gfx->setTextSize(2);
  gfx->setCursor(CX - gfx->textWidth(msg) / 2, 90);
  gfx->print(msg);
}

// animacion de la ceremonia (10s): despedida = reverencia con corazones y se
// aleja caminando; escapada = se asusta y sale corriendo. Sustituye al idle.
void drawCeremony() {
  if (!pmd.loaded) { drawPet(); return; }  // respaldo si no hay sprite PMD
  uint32_t now = millis();
  float t = pet.ceremonyT();               // 0..1 a lo largo de los 10s
  bool panic = (pet.ceremony == CER_RUNAWAY);
  int x = CX, y = PET_GROUND;
  uint8_t act = PMD_IDLE;

  if (panic) {
    // final triste: penumbra azulada + lluvia
    for (int i = 0; i < 46; i++) {
      int rx = (i * 47 + now / 3) % 466;
      int ry = (i * 91 + now / 2) % 470;
      gfx->drawLine(rx, ry, rx - 3, ry + 12, C565(0x6a, 0x84, 0xb0));
    }
    bool fade = false;
    if (t < 0.30f) {                       // cabizbajo, temblando
      act = pmd.has(PMD_HURT) ? PMD_HURT : PMD_IDLE;
      x = CX + (int)(4 * sinf(now * 0.04f));
    } else {                               // se aleja despacio y se desvanece
      act = pmd.has(PMD_WALKL) ? PMD_WALKL : PMD_IDLE;
      x = CX - (int)(((t - 0.30f) / 0.70f) * (CX + 120));
      fade = (t > 0.6f) && ((now / 160) % 2 == 0);  // parpadea hacia la silueta
    }
    drawPmdAct(act, x, y, now, true, fade, 5);  // fade=silueta: se difumina al irse
    // lagrima cayendo del bicho
    if (t < 0.55f) {
      int ty = y - 150 + (int)((now / 6) % 40);
      gfx->fillRect(x + 6, ty, 3, 6, C565(0x9a, 0xc4, 0xe8));
    }
    return;
  }

  // despedida epica: halo dorado pulsante + chispas y corazones que ascienden
  int gcy = PET_GROUND - 96;
  for (int k = 0; k < 4; k++) {
    int r = 60 + k * 34 + (int)(10 * sinf(now * 0.02f));
    gfx->drawCircle(CX, gcy, r, C565(0xff, 0xdf, 0x8a));
  }
  for (int i = 0; i < 16; i++) {
    int px = (i * 71 + 28) % 466;
    int py = 410 - (int)((now / 8 + i * 70) % 360);   // suben y reaparecen abajo
    if (py < 30) continue;
    if (i % 4 == 0) drawMap(SPR_HEART, 32, px - 8, py - 8, 1, false);  // corazoncito
    else gfx->fillRect(px, py, 4, 4, (i % 2) ? C565(0xff, 0xe7, 0x9f) : C565(0xff, 0x9a, 0xc0));
  }

  if (t < 0.45f) {                         // reverencia / pose de despedida
    act = pmd.has(PMD_POSE) ? PMD_POSE : (pmd.has(PMD_NOD) ? PMD_NOD : PMD_IDLE);
  } else {                                 // se aleja por la derecha
    act = pmd.has(PMD_WALKR) ? PMD_WALKR : PMD_IDLE;
    x = CX + (int)(((t - 0.45f) / 0.55f) * (CX + 140));
  }
  drawPmdAct(act, x, y, now, true, false, 5);
  if (pet.showHeart())                     // corazon grande siguiendo al bicho
    drawMap(SPR_HEART, 32, x + 50, y - 190, 2, false);
}

// dialogo de decision (2 botones apilados): evolucionar/mantener o despedirse/quedaros
void drawChoiceDialog() {
  const char *q, *o1, *o2;
  uint16_t c1, c2, t1, t2;
  if (choiceKind == 1) {  // evolucion
    q = T(S_EVO_Q); o1 = T(S_EVO_TAP); o2 = T(S_EVO_KEEP);
    c1 = UI_BAR_BAD; t1 = UI_INK; c2 = UI_TRACK; t2 = UI_WHITE;
  } else {                // despedida
    q = T(S_FAR_Q); o1 = T(S_FAR_GO); o2 = T(S_FAR_STAY);
    c1 = UI_BAR_WARN; t1 = UI_INK; c2 = UI_BAR_OK; t2 = UI_INK;
  }
  gfx->fillRoundRect(73, 156, 320, 188, 16, UI_WHITE);
  gfx->drawRoundRect(73, 156, 320, 188, 16, UI_INK);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(2);
  gfx->setCursor(CX - gfx->textWidth(q) / 2, 176);
  gfx->print(q);
  gfx->fillRoundRect(93, 206, 280, 52, 12, c1);     // boton accion
  gfx->setTextColor(t1);
  gfx->setCursor(CX - gfx->textWidth(o1) / 2, 224);
  gfx->print(o1);
  gfx->fillRoundRect(93, 268, 280, 52, 12, c2);     // boton mantener/quedaros
  gfx->setTextColor(t2);
  gfx->setCursor(CX - gfx->textWidth(o2) / 2, 286);
  gfx->print(o2);
}

// boton-CTA rojo y grande para evolucionar (pulsa para llamar la atencion)
void drawEvolveButton() {
  uint32_t now = millis();
  int p = (int)(5 * sinf(now * 0.006f));  // late: -5..5
  int x = EVO_BTN_X - p, y = EVO_BTN_Y - p, w = EVO_BTN_W + 2 * p, h = EVO_BTN_H + 2 * p;
  gfx->fillRoundRect(x, y, w, h, 18, UI_BAR_BAD);
  gfx->drawRoundRect(x, y, w, h, 18, UI_WHITE);
  gfx->drawRoundRect(x + 2, y + 2, w - 4, h - 4, 16, UI_WHITE);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(3);
  const char *t = T(S_EVO_TAP);
  gfx->setCursor(CX - gfx->textWidth(t) / 2, y + h / 2 - 11);
  gfx->print(t);
}

// boton-CTA dorado de despedida: "<nombre> quiere decirte algo..."
void drawFarewellButton() {
  uint32_t now = millis();
  int p = (int)(4 * sinf(now * 0.005f));
  int x = FAR_BTN_X - p, y = FAR_BTN_Y - p, w = FAR_BTN_W + 2 * p, h = FAR_BTN_H + 2 * p;
  gfx->fillRoundRect(x, y, w, h, 16, UI_BAR_WARN);
  gfx->drawRoundRect(x, y, w, h, 16, UI_INK);
  char buf[52];
  const char *nm = pet.nick[0] ? pet.nick : dexName(pet.speciesId);
  snprintf(buf, sizeof(buf), T(S_FAREWELL_BTN), nm);
  gfx->setTextColor(UI_INK);
  gfx->setTextSize(2);
  gfx->setCursor(CX - gfx->textWidth(buf) / 2, y + h / 2 - 8);
  gfx->print(buf);
}

// boton-CTA sombrio de escapada por abandono: "<nombre> se siente abandonado..."
// (final triste: azul-gris oscuro, latido lento y apagado)
void drawRunawayButton() {
  uint32_t now = millis();
  int p = (int)(3 * sinf(now * 0.003f));
  int x = FAR_BTN_X - p, y = FAR_BTN_Y - p, w = FAR_BTN_W + 2 * p, h = FAR_BTN_H + 2 * p;
  gfx->fillRoundRect(x, y, w, h, 16, C565(0x3a, 0x44, 0x5a));
  gfx->drawRoundRect(x, y, w, h, 16, C565(0x70, 0x80, 0x98));
  char buf[52];
  const char *nm = pet.nick[0] ? pet.nick : dexName(pet.speciesId);
  snprintf(buf, sizeof(buf), T(S_RUNAWAY_BTN), nm);
  gfx->setTextColor(C565(0xc8, 0xd2, 0xe0));
  gfx->setTextSize(2);
  gfx->setCursor(CX - gfx->textWidth(buf) / 2, y + h / 2 - 8);
  gfx->print(buf);
}

// animacion epica de evolucion: halo radial + rayos giratorios + parpadeo del
// sprite acelerando + chispas que salen disparadas + fogonazo final
void drawEvolveFX(uint32_t now) {
  float t = pet.evolveT();          // 0..1
  int cx = CX, cy = PET_GROUND - 96;

  // halo radial que crece y pulsa
  int halo = 36 + (int)(t * 150) + (int)(8 * sinf(now * 0.02f));
  for (int k = 0; k < 4; k++) {
    int r = halo - k * 7;
    if (r > 0) gfx->drawCircle(cx, cy, r, UI_WHITE);
  }
  // rayos giratorios desde el centro del bicho
  float base = now * 0.004f;
  for (int i = 0; i < 12; i++) {
    float a = base + i * (float)(PI / 6);
    int len = 90 + (int)(70 * (0.5f + 0.5f * sinf(now * 0.012f + i)));
    gfx->drawLine(cx, cy, cx + (int)(cosf(a) * len), cy + (int)(sinf(a) * len), UI_WHITE);
  }
  // parpadeo entre la forma ANTERIOR y la NUEVA (siluetas), acelerando; al
  // final (t>0.9) se queda fija en la nueva para el fogonazo de revelado
  int period = 60 + (int)(220 * (1.0f - t));
  bool showOld = t < 0.9f && evoPmd.loaded && ((now / period) % 2) == 0;
  if (showOld) drawPmdActM(evoPmd, PMD_IDLE, cx, PET_GROUND, 0, true, true, 5);
  else drawPmdAct(PMD_IDLE, cx, PET_GROUND, 0, true, true, 5);
  // chispas que salen disparadas
  for (int i = 0; i < 10; i++) {
    float a = i * (float)(PI / 5) + t * 4.0f;
    int d = (int)((now / 14 + i * 33) % 200);
    int sx = cx + (int)(cosf(a) * d), sy = cy + (int)(sinf(a) * d);
    gfx->fillRect(sx - 2, sy - 2, 5, 5, (i & 1) ? C565(0xff, 0xe0, 0x70) : UI_WHITE);
  }
  // fogonazo final antes de revelar la forma nueva
  if (t > 0.9f) gfx->fillCircle(cx, cy, (int)(300 * (t - 0.9f) / 0.1f), UI_WHITE);
}

void drawPet() {
  if (pmd.loaded) {
    drawPetPMD();
    return;
  }
  if (mon.loaded) {
    drawPetSD();
    return;
  }
  // The public firmware contains no embedded Pokémon artwork. A missing or
  // corrupt downloaded sprite therefore gets a neutral diagnostic placeholder.
  gfx->setTextColor(inkColor());
  gfx->setTextSize(6);
  gfx->setCursor(CX - 18, PET_CY - 80);
  gfx->print("?");
  gfx->setTextSize(2);
  const char *l1 = T(S_NO_SPRITES);
  gfx->setCursor(CX - gfx->textWidth(l1) / 2, PET_CY - 4);
  gfx->print(l1);
  const char *l2 = T(S_LOAD_SPRITES);
  gfx->setCursor(CX - gfx->textWidth(l2) / 2, PET_CY + 20);
  gfx->print(l2);
}

// ---------- escena de bano ----------

void startBath() {
  if (pet.isEgg() || pet.sleeping || pet.ceremony || bathUntil) return;
  bathUntil = millis() + 3000;
  bathPending = true;
  sfxPlay(SFX_EVENT_SPARKLE);
  int cx = (int)beh.x;
  for (auto &b : bubbles) {
    b.x = cx - 70 + random(140);
    b.y = PET_GROUND - random(150);
    b.r = 8 + random(16);
    b.ph = random(64);
  }
}

void drawBath() {
  uint32_t now = millis();
  if (now > bathUntil) {
    bathUntil = 0;
    if (bathPending) {
      bathPending = false;
      pet.clean();
      // pose de alegria al quedar limpio
      if (pmd.has(PMD_POSE)) {
        beh.mode = 2;
        beh.act = PMD_POSE;
        beh.t0 = now;
        beh.until = now + pmdActTotalMs(pmd.acts[PMD_POSE]) * 2;
      }
    }
    return;
  }
  uint32_t left = bathUntil - now;
  if (left > 800) {
    // espuma: pompas meciendose y subiendo poco a poco
    float t = now / 220.0f;
    for (auto &b : bubbles) {
      int bx = b.x + (int)(sinf(t + b.ph) * 6);
      int by = b.y - (int)((3000 - left) / 90);
      gfx->fillCircle(bx, by, b.r, UI_WHITE);
      gfx->drawCircle(bx, by, b.r, 0x7E3D);
      gfx->fillCircle(bx - b.r / 3, by - b.r / 3, b.r / 4, UI_BG_DAY);
    }
  } else {
    // las pompas revientan: destellos
    for (int i = 0; i < 8; i++) {
      auto &b = bubbles[i];
      int sx = b.x + (i % 3) * 6 - 6, sy = b.y - 18;
      uint16_t col = (i % 2) ? UI_BAR_WARN : UI_WHITE;
      gfx->fillRect(sx - 6, sy - 1, 13, 3, col);
      gfx->fillRect(sx - 1, sy - 6, 3, 13, col);
    }
  }
}

// ---------- mascota PMD: comportamiento ----------

uint32_t pmdActTotalMs(const PmdAct &a) {
  uint32_t t = 0;
  for (uint8_t i = 0; i < a.frames; i++) t += a.ms[i];
  return t ? t : 100;
}

uint8_t pmdFrameAt(const PmdAct &a, uint32_t t, bool loop) {
  uint32_t total = pmdActTotalMs(a);
  if (!loop && t >= total) return a.frames - 1;
  t %= total;
  uint8_t i = 0;
  while (t >= a.ms[i]) {
    t -= a.ms[i];
    i = (i + 1) % a.frames;
  }
  return i;
}

// dibuja una accion anclada por la base (centro-x, suelo) y devuelve su escala
// dibuja una accion de un PmdMon concreto (m); drawPmdAct usa el global pmd
void drawPmdActM(PmdMon &m, uint8_t actId, int cx, int groundY, uint32_t t, bool loop, bool sil, uint8_t maxS) {
  const PmdAct &a = m.acts[actId];
  if (!a.frames) return;
  uint8_t sBase = m.acts[PMD_IDLE].h ? 170 / m.acts[PMD_IDLE].h : 5;
  if (sBase < 2) sBase = 2;
  if (sBase > maxS) sBase = maxS;
  uint8_t s = sBase;
  // TPK3 actions share a movement canvas that can be much larger than the
  // visible cropped frames. Mr. Mime's attack canvas is 72x88 while the actor
  // itself is only about 21x28; scaling by the transparent canvas made it
  // shrink from 3x to 2x every time it attacked. Use the largest visible frame
  // so an action keeps the same apparent Pokemon size throughout.
  uint8_t visibleW = 0, visibleH = 0;
  for (uint8_t i = 0; i < a.frames; i++) {
    if (a.frame[i].w > visibleW) visibleW = a.frame[i].w;
    if (a.frame[i].h > visibleH) visibleH = a.frame[i].h;
  }
  while (s > 2 && ((uint16_t)visibleH * s > 250 ||
                    (uint16_t)visibleW * s > 250)) s--;
  uint8_t fi = pmdFrameAt(a, t, loop);
  const PmdFrame &fr = a.frame[fi];
  // anclar por los pies (a.base), no por el alto del lienzo: asi las acciones
  // con padding distinto (Hurt, Eat...) quedan todas a la misma altura de suelo
  int x0 = cx - a.w * s / 2 + fr.x * s;
  int y0 = groundY - (a.base ? a.base : a.h) * s + fr.y * s;
  for (int r = 0; r < fr.h; r++) {
    const uint8_t *row = fr.data + r * fr.w;
    for (int c = 0; c < fr.w; c++) {
      uint8_t idx = row[c];
      if (idx == 0xFF) continue;
      gfx->fillRect(x0 + c * s, y0 + r * s, s, s, sil ? INK_K : m.pal[idx]);
    }
  }
}
void drawPmdAct(uint8_t actId, int cx, int groundY, uint32_t t, bool loop, bool sil, uint8_t maxS) {
  drawPmdActM(pmd, actId, cx, groundY, t, loop, sil, maxS);
}

void drawPmdActRatioM(PmdMon &m, uint8_t actId, int cx, int groundY,
                      uint32_t t, bool loop, uint8_t numerator, uint8_t denominator) {
  const PmdAct &a = m.acts[actId];
  if (!a.frames || denominator == 0) return;
  uint8_t fi = pmdFrameAt(a, t, loop);
  const PmdFrame &fr = a.frame[fi];
  int canvasW = (int)a.w * numerator / denominator;
  int xBase = cx - canvasW / 2;
  int yBase = groundY - (int)(a.base ? a.base : a.h) * numerator / denominator;
  for (int r = 0; r < fr.h; r++) {
    const uint8_t *row = fr.data + r * fr.w;
    int y0 = yBase + (fr.y + r) * numerator / denominator;
    int y1 = yBase + (fr.y + r + 1) * numerator / denominator;
    for (int c = 0; c < fr.w; c++) {
      uint8_t idx = row[c];
      if (idx == 0xFF) continue;
      int x0 = xBase + (fr.x + c) * numerator / denominator;
      int x1 = xBase + (fr.x + c + 1) * numerator / denominator;
      gfx->fillRect(x0, y0, x1 - x0, y1 - y0, m.pal[idx]);
    }
  }
}

// elige el siguiente capricho del bicho cuando esta contento
void behNext() {
  uint32_t now = millis();
  beh.t0 = now;
  int r = random(100);
  if (r < 35 && (pmd.has(PMD_WALKL) || pmd.has(PMD_WALKR))) {
    beh.mode = 1;  // paseo
    beh.targetX = pet.sitterActive(pet.lastSeenEpoch) ? 110 + random(146) : 150 + random(176);
    beh.until = now + 15000;
  } else if (r < 60) {
    // gesto aleatorio entre los disponibles
    // (Hop fuera: salta demasiado alto; Sit fuera: mira hacia atras)
    static const uint8_t flair[] = { PMD_POSE, PMD_NOD, PMD_BREATH };
    uint8_t pick[3], n = 0;
    for (uint8_t f : flair)
      if (pmd.has(f)) pick[n++] = f;
    if (n) {
      beh.mode = 2;
      beh.act = pick[random(n)];
      beh.until = now + pmdActTotalMs(pmd.acts[beh.act]);
      return;
    }
    beh.mode = 0;
    beh.until = now + 2000 + random(3000);
  } else {
    beh.mode = 0;  // mirar al frente
    beh.until = now + 2000 + random(3000);
  }
}

void drawPetPMD() {
  uint32_t now = millis();

  // Keep the raised Pokemon on the left while Chansey occupies the right.
  if (pet.sitterActive(pet.lastSeenEpoch)) {
    if (beh.x > 260) beh.x = 260;
    if (beh.x < 105) beh.x = 105;
    if (beh.targetX > 260 || beh.targetX < 105) beh.targetX = 110 + random(146);
  }

  if (pet.evolving()) {
    drawEvolveFX(now);
    return;
  }
  if (evoPmd.loaded) evoPmd.unload();  // termino la evolucion: libera la forma anterior

  PetMood m = pet.mood();
  uint8_t act;
  bool loop = true;
  if (m == MOOD_SLEEPING && pmd.has(PMD_SLEEP)) {
    act = PMD_SLEEP;
    beh.mode = 0;
  } else if (m == MOOD_EATING && pmd.has(PMD_EAT)) {
    act = PMD_EAT;
    beh.t0 = 0;
  } else if (m == MOOD_SAD && pmd.has(PMD_HURT)) {
    act = PMD_HURT;
  } else {
    // contento: el planificador decide (idle / paseo / gesto)
    if (now > beh.until) behNext();
    if (beh.mode == 1) {
      float d = beh.targetX - beh.x;
      if (fabsf(d) < 4) {
        behNext();
        act = PMD_IDLE;
      } else {
        beh.x += (d > 0 ? 3.0f : -3.0f);
        act = (d > 0) ? PMD_WALKR : PMD_WALKL;
      }
    } else {
      act = (beh.mode == 2) ? beh.act : PMD_IDLE;
      loop = false;
    }
    if (!pmd.has(act)) act = PMD_IDLE;
  }

  drawPmdAct(act, (int)beh.x, PET_GROUND, now - beh.t0, loop || act == PMD_IDLE, false, 5);

  if (pet.showHeart()) drawMap(SPR_HEART, 32, (int)beh.x + 50, PET_GROUND - 190, 2, false);
}

// sprite TPK1 heredado: zoom entero por pixel, frames a su ritmo
void drawPetSD() {
  int s = mon.scale;
  int w = mon.w * s, h = mon.h * s;
  int x = CX - w / 2;
  int y = PET_CY - h / 2;

  bool sil = false;
  if (pet.evolving()) {
    sil = (millis() / 300) % 2;
  } else if (pet.mood() == MOOD_HAPPY && (millis() / 500) % 2) {
    y -= 6;  // saltito
  }

  uint16_t fm = mon.frameMs ? mon.frameMs : 100;
  uint16_t fi = pet.sleeping ? 0 : (millis() / fm) % mon.frames;
  const uint8_t *fr = mon.data + (uint32_t)fi * mon.w * mon.h;
  for (int r = 0; r < mon.h; r++) {
    const uint8_t *row = fr + r * mon.w;
    for (int c = 0; c < mon.w; c++) {
      uint8_t idx = row[c];
      if (idx == 0xFF) continue;
      gfx->fillRect(x + c * s, y + r * s, s, s, sil ? INK_K : mon.pal[idx]);
    }
  }

  // emotes en vez de expresiones (los sprites importados no tienen anclas)
  if (pet.showHeart()) drawMap(SPR_HEART, 32, x + w - 30, y - 50, 2, false);
}

void drawPoops() {
  for (int i = 0; i < pet.poops; i++) {
    drawMap(SPR_POOP, 32, 36 + i * 46, 244, 2, false);
  }
}

void drawBars() {
  drawBar(78, 318, T(S_BAR_FOOD), pet.fullness);
  drawBar(244, 318, T(S_BAR_JOY), pet.joy);
  drawBar(78, 346, T(S_BAR_ENE), pet.energy);
  drawBar(244, 346, T(S_BAR_HYG), pet.hygiene);
}

void drawBar(int x, int y, const char *label, uint8_t val) {
  gfx->setTextColor(inkColor());
  gfx->setTextSize(2);
  gfx->setCursor(x, y);
  gfx->print(label);
  int bx = x + 48, bw = 100, bh = 15;  // +48: deja sitio a etiquetas de 4 letras (EN)
  uint16_t fill = (val >= 50) ? UI_BAR_OK : (val >= 25) ? UI_BAR_WARN : UI_BAR_BAD;
  gfx->fillRoundRect(bx, y, bw, bh, 4, UI_TRACK);
  int fw = (bw - 4) * val / 100;
  if (fw > 0) gfx->fillRoundRect(bx + 2, y + 2, fw, bh - 4, 3, fill);
}

void drawButtons() {
  for (int i = 0; i < 4; i++) {
    bool off = (pet.sleeping || pet.sitterActive(pet.lastSeenEpoch)) && i != 2;
    int bx = buttons[i].cx - BTN_HALF, by = buttons[i].cy - BTN_HALF;
    if (!pet.sleeping) gfx->fillRoundRect(bx, by, 2 * BTN_HALF, 2 * BTN_HALF, 14, UI_WHITE);
    gfx->drawRoundRect(bx, by, 2 * BTN_HALF, 2 * BTN_HALF, 14, inkColor());
    if (!off) drawMap(buttons[i].icon, 16, buttons[i].cx - 16, buttons[i].cy - 16, 2, false);
  }
}

const char *eggMsg() {
  switch (pet.eggCracks()) {
    case 0: return T(S_EGG_TOUCH);
    case 1: return T(S_EGG_MOVES);
    default: return T(S_EGG_ALMOST);
  }
}

const char *statusMsg() {
  if (statusNoticeUntil && (int32_t)(statusNoticeUntil - millis()) > 0) {
    return statusNoticeMsg;
  }
  statusNoticeUntil = 0;
  if (pet.evolving()) return T(S_EVOLVING);
  if (pet.sitterActive(pet.lastSeenEpoch)) return T(S_SITTER_ACTIVE);
  if (bathUntil) return "깨끗하게 씻자~";
  if (pet.sleeping) return "Zzz...";
  if (pet.eating()) return T(S_EATING);
  if (pet.showHeart()) return T(S_LIKES);
  if (pet.fullness < 25) return T(S_HUNGRY);
  if (pet.hygiene < 25) return T(S_NEEDS_BATH);
  if (pet.energy < 25) return T(S_EXHAUSTED);
  if (pet.joy < 25) return T(S_SAD);
  if (pet.weight > 60) return T(S_CHUBBY);
  if (pet.shiny && pet.ageMinutes < 15) return T(S_IS_SHINY);
  return T(S_HAPPY);
}

// dibuja un mapa de n x n pixeles escalado; silhouette=true lo pinta en tinta
void drawMap(const char *const *map, int n, int x, int y, int s, bool silhouette) {
  for (int r = 0; r < n; r++) {
    for (int c = 0; c < n; c++) {
      char ch = map[r][c];
      if (ch == '.') continue;
      gfx->fillRect(x + c * s, y + r * s, s, s, silhouette ? INK_K : spriteColor(ch));
    }
  }
}

#pragma once
#include <Arduino.h>
#include <Preferences.h>

// 1 tick = 1 minuto de juego. Baja este valor para probar mas rapido
// (p. ej. 5000UL = las estadisticas caen 12x mas rapido).
#define PET_TICK_MS 60000UL
// La edad avanza cada minuto, pero el nivel sube cada 2 horas reales.
#define MINUTES_PER_LEVEL 120
#define AWAKE_NEED_INTERVAL_MIN 3           // necesidades normales: cada 3 min
#define SLEEP_NEED_INTERVAL_DAY_MIN 4       // 달 수면: 09:00~22:00
#define SLEEP_NEED_INTERVAL_NIGHT_MIN 5     // 달 수면: 22:00~09:00
#define SLEEP_NEED_NIGHT_FLOOR 30           // 밤에는 이 아래로 내려가지 않음
#define EAT_ANIM_MS 2500UL
#define HEART_MS 1500UL
#define EVOLVE_ANIM_MS 5200UL              // animacion de evolucion (mas larga = mas epica)
#define CEREMONY_MS 10000UL                // duracion de la despedida en pantalla
#define ADULT_AGE_MIN (3UL * 24 * 60)       // adulto y apto para crianza a los 3 dias
#define FAREWELL_AGE_MIN ADULT_AGE_MIN      // alias conservado para compatibilidad
#define FINAL_FORM_FAREWELL_DELAY_MIN (24UL * 60)  // un dia completo tras alcanzar forma final
#define RUNAWAY_TICKS 60                   // se escapa tras 1 h con TODO a cero

// 1분 동안 실수 없이 플레이했을 때 도달 가능한 보수적인 MAX 기준.
// 잠만보만 30초이며 게임 자체의 만점과 동일하다.
static constexpr uint8_t MINIGAME_MAX_RUNNER = 50;
static constexpr uint8_t MINIGAME_MAX_SNORLAX = 100;
static constexpr uint8_t MINIGAME_MAX_EEVEE = 75;
static constexpr uint8_t MINIGAME_MAX_DIGLETT = 40;
static constexpr uint8_t MINIGAME_MAX_MAGIKARP = 30;

// ceremonias de fin de ciclo
enum : uint8_t { CER_NONE = 0, CER_FAREWELL, CER_RUNAWAY, CER_RELEASE };

enum PetMood : uint8_t { MOOD_HAPPY, MOOD_SAD, MOOD_EATING, MOOD_SLEEPING };
enum PetEventType : uint8_t { PET_EVENT_BERRY = 0, PET_EVENT_HEART, PET_EVENT_SPARKLE };
// Tamagotchi-style care calls.  A non-NONE need remains locked until that
// exact stat has recovered above the red gauge range (danger is <= 24).
static constexpr uint8_t TAMAGOTCHI_CARE_DANGER_MAX = 24;
enum CareNeed : uint8_t {
  CARE_NEED_NONE = 0,
  CARE_NEED_FULLNESS,
  CARE_NEED_HYGIENE,
  CARE_NEED_ENERGY,
  CARE_NEED_JOY,
};
enum PetPersonality : uint8_t {
  PERS_BALANCED = 0,
  PERS_PLAYFUL,
  PERS_BRAVE,
  PERS_CALM,
  PERS_LAZY,
};

enum PetInteractResult : uint8_t {
  PET_INTERACT_NONE = 0,
  PET_INTERACT_JOY = 1 << 0,
  PET_INTERACT_BOND = 1 << 1,
  PET_INTERACT_ENERGY = 1 << 2,
};

enum DailyGoalType : uint8_t {
  DAILY_GOAL_CARE = 0,
  DAILY_GOAL_PLAY,
  DAILY_GOAL_BATTLE,
  DAILY_GOAL_ITEM,
  DAILY_GOAL_WALK,
};
#define DAILY_GOAL_COUNT 3

enum ExpeditionItem : uint8_t {
  EXP_ITEM_SNACK = 0,
  EXP_ITEM_ENERGY,
  EXP_ITEM_CARE,
  EXP_ITEM_TRAIN,
  EXP_ITEM_MOVE,
  EXP_ITEM_NONE = 0xFF,
};
#define EXP_ITEM_COUNT 5
#define EXP_ITEM_MAX 15

enum TrainingStat : int8_t {
  TRAIN_STAT_ATK = 0,
  TRAIN_STAT_DEF,
  TRAIN_STAT_SPE,
};

// medallas del individuo (bitmask)
enum : uint16_t {
  MED_LV10 = 1 << 0, MED_LV25 = 1 << 1, MED_LV50 = 1 << 2,
  MED_BERRY = 1 << 3, MED_STREAK7 = 1 << 4, MED_BOND = 1 << 5,
  MED_FINAL = 1 << 6, MED_FIT = 1 << 7,
};
#define MED_COUNT 8

enum BattleRewardStat : uint8_t {
  BATTLE_REWARD_NONE = 0,
  BATTLE_REWARD_ATK,
  BATTLE_REWARD_DEF,
  BATTLE_REWARD_SPE,
};

struct BattleReward {
  BattleRewardStat stat = BATTLE_REWARD_NONE;
  uint8_t amount = 0;
};

struct CommunicationReward {
  ExpeditionItem item = EXP_ITEM_NONE;
  uint8_t amount = 0;
};

#define WALK_REWARD_MAX 5
struct WalkReward {
  ExpeditionItem items[WALK_REWARD_MAX] = {
    EXP_ITEM_NONE, EXP_ITEM_NONE, EXP_ITEM_NONE, EXP_ITEM_NONE, EXP_ITEM_NONE
  };
  uint8_t count = 0;
  uint8_t tier = 0;
  bool dailyMoveDisc = false;
};

class Pet {
public:
  // Estadisticas 0..100
  uint8_t fullness = 80;  // comida
  uint8_t joy = 80;       // felicidad
  uint8_t energy = 80;    // energia
  uint8_t hygiene = 100;  // limpieza
  uint8_t poops = 0;      // cacas en pantalla (max 3)
  uint8_t weight = 0;     // 0-100: las chuches engordan, el minijuego quema
  // genes (90-110%, se tiran al eclosionar) y entrenamiento (0-100)
  uint8_t geneAtk = 100, geneDef = 100, geneSpe = 100;
  uint8_t trAtk = 0, trDef = 0, trSpe = 0;
  bool berryKnown = false;  // ya descubrio su baya favorita
  bool shiny = false;       // variante de color rara (se sortea en el huevo)
  uint32_t ageMinutes = 0;
  int16_t speciesId = -1;      // numero de Pokedex (1-151), -1 = huevo
  int16_t prevSpeciesId = -1;  // para la animacion de evolucion
  uint8_t careMistakes = 0;   // descuidos: cada uno retrasa la evolucion 1 nivel
  bool sleeping = false;
  uint32_t lastSeenEpoch = 0;   // ultima hora RTC vista (para progresion offline)
  uint32_t rtcGuardEpoch = 0;   // ultimo RTC confiable guardado junto al estado
  uint8_t ceremony = CER_NONE;  // despedida/escapada/liberacion en curso
  uint8_t lastEnd = CER_NONE;   // como acabo la anterior (afecta al huevo)
  uint8_t dexReg[19] = { 0 };       // pokedex de criados (bitmap 151 bits)
  uint8_t dexShinyReg[19] = { 0 };  // criados en version shiny
  uint8_t dexCaught[19] = { 0 };    // pokedex de salvajes capturados
  uint8_t dexLinked[19] = { 0 };    // especies vistas por batalla/crianza BLE
  // racha de cuidado diario (del jugador: persiste entre crianzas)
  uint16_t streak = 0, bestStreak = 0;
  uint32_t lastCareDay = 0;
  // vinculo (del bicho: sube lento con cuidado, se resetea al nacer otro)
  uint8_t bond = 0;
  char nick[12] = "";    // apodo (vacio = nombre de especie)
  // medallas: del individuo + contador acumulado entre todas las crianzas
  uint16_t medals = 0, totalMedals = 0;
  uint16_t newMedal = 0;   // recien conseguida(s), para celebrar
  uint16_t lastMilestone = 0;  // hito de racha ya celebrado
  uint16_t gameHi = 0;     // record del minijuego (del jugador)
  uint16_t catchHi = 0;    // record de capturas del minijuego catch
  uint16_t memoHi = 0;     // record de rondas del minijuego memo
  uint16_t diglettHi = 0;  // record del minijuego Whack-a-Diglett
  uint16_t typeHi = 0;     // record del minijuego type match
  uint16_t battleWins = 0, battleLosses = 0;
  uint16_t battleStreak = 0, bestBattleStreak = 0;
  uint8_t collectionFrame = 0;  // 0=Basis, weitere Rahmen ueber Dex-Meilensteine
  uint32_t lastPetInteractMinute = 0;
  uint8_t dexRewardMask = 0;
  uint32_t dailyGoalDay = 0;
  uint8_t dailyGoalType[DAILY_GOAL_COUNT] = { DAILY_GOAL_CARE, DAILY_GOAL_PLAY, DAILY_GOAL_BATTLE };
  uint8_t dailyGoalProgress[DAILY_GOAL_COUNT] = { 0, 0, 0 };
  uint8_t dailyGoalDone = 0;
  uint8_t itemCounts[EXP_ITEM_COUNT] = { 0 };
  uint16_t battleMove1 = 0;  // persistent random common move for this individual
  uint32_t walkDiscDay = 0;
  uint32_t sitterUntilEpoch = 0;  // Chansey protects needs until 19:00 today
  bool saveLoadedFromNvs = false;
  bool saveCreatedThisBoot = false;

  void begin();                 // carga estado de NVS (o crea el primer huevo)
  void update(uint32_t nowMs);  // llamar en cada loop()

  // Acciones (botones tactiles)
  void feed();              // baya roja (compatibilidad)
  void feedBerry(uint8_t color);  // 0 roja, 1 azul, 2 verde
  void feedCandy();
  bool lovesBerry(uint8_t color) const {
    return !isEgg() && (speciesId % 3) == color;  // gusto oculto por especie
  }
  void playResult(uint8_t score);  // recompensa del minijuego (entrena VEL)
  uint8_t applyCatchResult(uint8_t score);
  uint8_t applyMemoResult(uint8_t rounds);
  uint8_t applyDiglettResult(uint8_t score);
  uint8_t applyTypeResult(uint8_t score);
  void playWithFriend();
  bool applyPetEvent(uint8_t eventType);
  uint8_t interactPet(bool eveningBonus);
  PetPersonality personality() const;
  void ensureDailyGoals();
  uint8_t dailyGoalTarget(uint8_t goalType) const;
  bool dailyGoalComplete(uint8_t index) const;
  BattleReward applyBattleWin(int16_t wildDex, bool closeWin);
  void applyBattleLoss();

  bool expeditionInventoryFull() const;
  bool useExpeditionItem(ExpeditionItem item, int8_t trainingStat = -1);
  bool canStartWalk() const;
  bool beginWalk();
  void restoreWalkPause();
  void endWalkPause();
  WalkReward applyWalkReward(uint16_t steps, uint8_t itemRoll);
  CommunicationReward applyCommunicationBattleReward(bool won, uint8_t itemRoll);
  void createCommunicationEgg(uint8_t shinyRoll);
  uint16_t signatureMove() const;
  bool canRelearnMove1() const;
  uint16_t previewMove1(uint32_t seed) const;
  bool learnMove1(uint16_t moveId);
  bool keepMove1();
  bool awardMoveDisc(uint8_t chancePct, uint8_t luckRoll);

  // Babysitter: age and evolution time continue, while needs, dirt and care
  // calls are paused. The end timestamp is persisted so rebooting cannot
  // extend or cancel the service accidentally.
  bool sitterActive(uint32_t nowEpoch) const;
  bool canStartSitter(uint32_t nowEpoch) const;
  bool startSitter(uint32_t nowEpoch);
  void stopSitter();
  bool expireSitterIfNeeded(uint32_t nowEpoch);

  // stats de combate: base real de gen 1 x genes + nivel + entrenamiento
  uint16_t atkStat() const;
  uint16_t defStat() const;
  uint16_t speStat() const;
  uint8_t weightEnergyPenalty() const;
  void play();
  void toggleLight();  // dormir / despertar
  void clean();
  void caress();  // tocar al bicho
  void eggTap();  // tocar el huevo: 3 toques y eclosiona
  void newEgg();   // empezar de cero con un inicial aleatorio
  void release();  // soltar (pulsacion larga + confirmar)
  void syncClock(uint32_t nowEpoch);  // aplica el tiempo transcurrido apagado
  void setClock(uint32_t nowEpoch);   // fija la hora sin aplicar progresion
  void startFarewell();  // tambien usable desde la consola serie (BYE)
  void startRunaway();   // tambien usable desde la consola serie (RUN)

  // Modos de descuido:
  // - Tamagotchi: llamada cada 15 min y un unico fallo por necesidad bloqueada.
  // - PokeTama: conserva el sistema historico de cooldown de 30 min.
  bool tamagotchiModeEnabled() const { return tamagotchiMode; }
  void setTamagotchiMode(bool enabled);
  CareNeed pendingCareNeed() const { return careNeed; }
  bool careCallMissed() const { return careNeed != CARE_NEED_NONE && careMissed; }
  uint32_t careCallDeadline() const { return careDueEpoch; }
  CareNeed startCareCallIfNeeded();
  void armCareCallDeadline(uint32_t nowEpoch);
  bool resolveCareCallIfRecovered();
  bool applyDueCareMiss(uint32_t nowEpoch);

  bool isEgg() const { return speciesId < 0; }
  uint8_t eggCracks() const { return eggTaps; }
  bool eating() const { return millis() < eatUntil; }
  bool showHeart() const { return millis() < heartUntil; }
  bool evolving() const { return millis() < evolveUntil; }
  float evolveT() const {     // progreso de la animacion de evolucion 0..1
    uint32_t n = millis();
    uint32_t left = evolveUntil > n ? evolveUntil - n : 0;
    return 1.0f - (float)left / (float)EVOLVE_ANIM_MS;
  }
  bool canEvolveNow() const;  // condiciones de evolucion cumplidas (lista)
  void evolve();              // dispara la transformacion (la llama un toque del usuario)
  bool canFarewellNow() const;  // adulto + forma final durante 24 h: despedida lista
  bool canRunawayNow() const;   // abandono total 1h: lista para escaparse (boton triste)
  // Rechazar una decision la silencia hasta las 00:00 del dia siguiente.
  bool wantEvolveButton(uint32_t nowEpoch = 0) const;
  bool wantFarewellButton(uint32_t nowEpoch = 0) const;
  void declineEvolve(uint32_t nowEpoch = 0);
  void declineFarewell(uint32_t nowEpoch = 0);
  // primera partida: el jugador elige inicial (Bulbasaur/Charmander/Squirtle)
  bool awaitingStarter() const { return starterPick; }
  void chooseStarter(int16_t dex) { eggTarget = dex; starterPick = false; save(); }
  bool initialClockPending() const { return firstClockPending; }
  void confirmInitialClock() {
    firstClockPending = false;
    prefs.putBool("timeok", true);
  }
  void factoryReset() { prefs.clear(); }  // borra la NVS (test: comando serie WIPE)
  void dbgRunawayReady() { fullness = joy = energy = hygiene = 0; neglectTicks = RUNAWAY_TICKS; }  // test
  void dbgSetAgeMinutes(uint32_t minutes) {
    ageMinutes = minutes;
    lastTick = millis();
    save();
  }
  uint8_t level() const {
    const uint32_t value = 1UL + ageMinutes / MINUTES_PER_LEVEL;
    return value > 255UL ? 255 : (uint8_t)value;
  }
  bool isRegistered(int16_t dex) const {
    return dex >= 1 && dex <= 151 && (dexReg[(dex - 1) >> 3] & (1 << ((dex - 1) & 7)));
  }
  bool isCaught(int16_t dex) const {
    return dex >= 1 && dex <= 151 && (dexCaught[(dex - 1) >> 3] & (1 << ((dex - 1) & 7)));
  }
  bool isLinked(int16_t dex) const {
    return dex >= 1 && dex <= 151 && (dexLinked[(dex - 1) >> 3] & (1 << ((dex - 1) & 7)));
  }
  bool isKnown(int16_t dex) const { return isRegistered(dex) || isCaught(dex) || isLinked(dex); }
  bool friendPlayUnlocked() const { return knownDexCount() >= 2; }
  bool isShinyRegistered(int16_t dex) const {
    return dex >= 1 && dex <= 151 && (dexShinyReg[(dex - 1) >> 3] & (1 << ((dex - 1) & 7)));
  }
  uint16_t registeredCount() const;
  uint16_t caughtCount() const;
  uint16_t linkedCount() const;
  uint16_t knownDexCount() const;
  uint8_t collectionRank() const;
  uint8_t unlockedCollectionFrameCount() const;
  bool setCollectionFrame(uint8_t frame);
  void registerCaught(int16_t dex);
  void registerLinked(int16_t dex);
  uint8_t nextDexGoal() const;
  uint8_t applyDexRewards();
  uint8_t catchChanceForWild(int16_t wildDex, uint8_t wildLevel, uint8_t petLevel, bool closeWin) const;
  uint8_t respectCatchChanceForWild(int16_t wildDex, uint8_t wildLevel, uint8_t petLevel) const;
  bool tryCatchWild(int16_t wildDex, uint8_t wildLevel, uint8_t petLevel, bool closeWin, uint8_t luckRoll);
  bool tryRespectCatchWild(int16_t wildDex, uint8_t wildLevel, uint8_t petLevel, uint8_t luckRoll);
  bool lineHasUnregistered(int16_t base) const;
  uint8_t eggRarity() const;       // rareza del huevo actual (sin revelar especie)
  int16_t pickEggSpecies();        // publica para poder simular tiradas (EGGS)
  uint8_t lowestStat() const { return min(min(fullness, joy), min(energy, hygiene)); }
  PetMood mood() const;
  // progreso de la ceremonia de despedida/escapada, 0..1 (para animarla)
  float ceremonyT() const {
    if (ceremony == CER_NONE) return 0.0f;
    uint32_t n = millis();
    uint32_t left = ceremonyUntil > n ? ceremonyUntil - n : 0;
    return 1.0f - (float)left / (float)CEREMONY_MS;
  }

  // racha / vinculo / medallas / nombre
  void rename(const char *name);
  bool hasMedal(uint16_t m) const { return medals & m; }
  bool showMedal() const { return millis() < medalUntil; }
  bool showMilestone() const { return millis() < milestoneUntil; }
  bool showDexReward() const { return false; }
  uint8_t lastDexRewardGoal() const { return lastDexReward; }
  int careBonus() const;  // mejora del huevo por racha + vinculo

  // guardado periodico diferido: tick() marca pendiente y el loop lo vuelca
  // cuando la pantalla esta atenuada/apagada (la escritura a flash congela
  // ~1s ambos cores: asi no se ve ni corta el tactil)
  bool savePending() const { return pendingSave; }
  void flushSave();

private:
  Preferences prefs;
  uint32_t lastTick = 0;
  uint32_t eatUntil = 0;
  uint32_t heartUntil = 0;
  uint32_t evolveUntil = 0;
  int16_t eggTarget = 1;       // dex oculto que saldra del huevo
  bool eggShiny = false;       // sorpresa sorteada al crear el huevo
  uint8_t eggTaps = 0;
  uint8_t mistakeCooldown = 0;
  bool tamagotchiMode = true;
  CareNeed careNeed = CARE_NEED_NONE;
  bool careMissed = false;
  uint32_t careDueEpoch = 0;
  uint8_t sleepNeedMinutes = 0;  // 달 수면 중 다음 능력치 하락까지 누적된 분
  bool walkCarePaused = false;   // 산책 중에는 생활 능력치 감소만 일시 정지
  uint8_t ticksSinceSave = 0;
  bool pendingSave = false;     // guardado periodico pendiente de volcar
  uint32_t evoDeclinedDay = 0;  // sello de dia RTC en que se rechazo evolucionar
  uint32_t farDeclinedDay = 0;  // sello de dia RTC en que se rechazo despedirse
  uint32_t finalFormAgeMinutes = 0;  // edad a la que alcanzo la forma final
  bool starterPick = false;     // primera partida: esperando que el jugador elija inicial
  bool firstClockPending = false;  // fresh save must confirm local time before starter choice
  uint8_t neglectTicks = 0;
  uint16_t goodTicks = 0;  // racha bien cuidado: forja la DEF
  uint32_t ceremonyUntil = 0;
  uint8_t bondToday = 0;       // tope diario de subida de vinculo
  uint32_t medalUntil = 0;     // celebracion de medalla en pantalla
  uint32_t milestoneUntil = 0; // celebracion de hito de racha
  uint32_t dexRewardUntil = 0;
  uint8_t lastDexReward = 0;

  uint32_t today() const { return lastSeenEpoch ? lastSeenEpoch / 86400 : 0; }
  uint32_t dayStamp(uint32_t nowEpoch = 0) const;
  void registerCare();   // primer cuidado del dia: racha + vinculo
  uint8_t careNeedValue(CareNeed need) const;
  void clearCareCall();
  void addCareMistake();
  void applySleepingMinute(uint32_t minuteEpoch);
  void addBond(uint8_t amt);
  void noteDailyGoal(uint8_t goalType, uint8_t amount);
  void applyDailyReward();
  void checkMedals();
  void tick();
  void hatch();
  void registerSpecies(int16_t dex);
  bool canReceiveExpeditionItem(ExpeditionItem item) const;
  void save();
  void load();
  static uint8_t clamp100(int v) { return v < 0 ? 0 : (v > 100 ? 100 : v); }
};

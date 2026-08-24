#include "pet.h"
#include "dex.h"
#include "audio.h"
#include "moves.h"

#include <cstring>

void Pet::begin() {
  prefs.begin("tamapoke", false);
  bool hadSave = prefs.getBool("init", false);
  saveLoadedFromNvs = hadSave;
  saveCreatedThisBoot = !hadSave;
  if (!hadSave) {
    prefs.putBool("init", true);
    prefs.putBool("timeok", false);
    firstClockPending = true;
    newEgg();
  } else {
    load();
    // Saves made before the onboarding clock existed are already established
    // games, so migrate them without interrupting the player after an update.
    if (prefs.isKey("timeok")) {
      firstClockPending = !prefs.getBool("timeok", false);
    } else {
      firstClockPending = false;
      prefs.putBool("timeok", true);
    }
  }
  lastTick = millis();
}

void Pet::newEgg() {
  ceremony = CER_NONE;
  neglectTicks = 0;
  weight = 0;
  speciesId = -1;
  prevSpeciesId = -1;
  battleMove1 = MOVE_NONE;
  eggTarget = pickEggSpecies();  // especie oculta segun rareza y pokedex
  starterPick = (registeredCount() == 0);  // primera partida: el jugador elige inicial
  // sorteo shiny: 1/48 base, mejor con despedida y con racha/vinculo altos
  int shinyBase = (lastEnd == CER_FAREWELL ? 24 : 48) - careBonus();
  if (shinyBase < 8) shinyBase = 8;
  eggShiny = (random(shinyBase) == 0);
  eggTaps = 0;
  fullness = 80;
  joy = 80;
  energy = 80;
  hygiene = 100;
  poops = 0;
  ageMinutes = 0;
  careMistakes = 0;
  mistakeCooldown = 0;
  careNeed = CARE_NEED_NONE;
  careMissed = false;
  careDueEpoch = 0;
  sleepNeedMinutes = 0;
  sitterUntilEpoch = 0;
  evoDeclinedDay = 0;
  farDeclinedDay = 0;
  finalFormAgeMinutes = 0;
  sleeping = false;
  save();
}

// Progresion offline: aplica los mismos pulsos de 3/4 min. PokeTama conserva
// un suelo de 15; Tamagotchi puede entrar en peligro para generar llamadas.
static uint8_t dropTo(uint8_t v, uint8_t d, uint8_t fl) {
  if (v <= fl) return v;
  return (v - fl > d) ? v - d : fl;
}

static uint8_t minU8(uint8_t a, uint8_t b) {
  return a < b ? a : b;
}

static uint8_t minigamePerformance(uint8_t score, uint8_t maxScore) {
  if (!maxScore) return 0;
  uint16_t percent = (uint16_t)score * 100U / maxScore;
  return percent > 100U ? 100U : (uint8_t)percent;
}

static uint8_t minigameRewardTier(uint8_t performance) {
  return performance >= 80 ? 3 : performance >= 50 ? 2 : performance >= 20 ? 1 : 0;
}

static bool isMoonSleepNight(uint32_t epoch) {
  if (!epoch) return false;  // RTC 미설정 시에는 주간 규칙을 사용
  uint8_t hour = (uint8_t)((epoch / 3600UL) % 24UL);
  return hour >= 22 || hour < 9;
}

void Pet::applySleepingMinute(uint32_t minuteEpoch) {
  energy = clamp100(energy + 6);
  if (weight > 0 && ageMinutes % 3 == 0) weight--;

  const bool night = isMoonSleepNight(minuteEpoch);
  const uint8_t interval = night ? SLEEP_NEED_INTERVAL_NIGHT_MIN
                                 : SLEEP_NEED_INTERVAL_DAY_MIN;
  if (sleepNeedMinutes < interval) sleepNeedMinutes++;
  if (sleepNeedMinutes < interval) return;

  sleepNeedMinutes = 0;
  const uint8_t floor = night ? SLEEP_NEED_NIGHT_FLOOR : 0;
  fullness = dropTo(fullness, 1, floor);
  joy = dropTo(joy, 1, floor);
  hygiene = dropTo(hygiene, 1, floor);
}

void Pet::setClock(uint32_t nowEpoch) {
  lastSeenEpoch = nowEpoch;
  if (nowEpoch) save();  // persiste ya: un corte de luz no pierde la referencia
}

void Pet::syncClock(uint32_t nowEpoch) {
  // En light sleep millis() tambien avanza. Reiniciar el ancla evita aplicar
  // los mismos minutos una segunda vez en update() al volver al loop activo.
  lastTick = millis();
  uint32_t seen = prefs.getUInt("seen", 0);
  lastSeenEpoch = nowEpoch;
  if (nowEpoch == 0) return;
  uint32_t mins = (seen && nowEpoch > seen) ? (nowEpoch - seen) / 60 : 0;
  if (mins < 2 || ceremony != CER_NONE) {
    save();  // primera vez o sin tiempo que aplicar: solo persistir la hora
    return;
  }
  if (mins > 14UL * 24 * 60) mins = 14UL * 24 * 60;  // tope: 2 semanas

  for (uint32_t i = 0; i < mins; i++) {
    ageMinutes++;
    if (isEgg()) {
      if (ageMinutes >= 3) hatch();  // eclosiona en tu ausencia
      continue;
    }
    // A walk protects the four care gauges, but age, level and hatch/evolution
    // clocks continue. The flag is restored from the walk checkpoint before
    // syncClock() runs, so sleep and reboot cannot re-apply the paused decay.
    if (walkCarePaused) continue;
    // Chansey pauses only care needs. Age/levels and egg timing still advance.
    // Test the end of each elapsed minute so progression resumes precisely at
    // the persisted 19:00 boundary even after a reboot.
    uint32_t minuteEpoch = seen + (i + 1UL) * 60UL;
    if (sitterActive(minuteEpoch)) continue;
    if (sleeping) {
      // 달 수면에만 시간대별 완화 규칙을 적용한다. 밤에는 5분 주기와
      // 최저 30, 낮에는 4분 주기로 0까지 내려간다.
      applySleepingMinute(minuteEpoch);
      continue;
    }
    // PokeTama conserva el suelo offline historico. En Tamagotchi las barras
    // deben poder entrar en peligro mientras el dispositivo reposa; de otro
    // modo las comprobaciones de 15 minutos nunca podrian generar una llamada.
    uint8_t offlineFloor = tamagotchiMode ? 0 : 15;
    if (ageMinutes % AWAKE_NEED_INTERVAL_MIN == 0) {
      fullness = dropTo(fullness, 1, offlineFloor);
      energy = dropTo(energy, 1, offlineFloor);
      if (fullness > 40 && poops < 3 && random(100) < 15) poops++;
      hygiene = dropTo(hygiene, 1 + 4 * poops, offlineFloor);
      energy = dropTo(energy, weightEnergyPenalty(), offlineFloor);
      if (weight > 0) weight--;

      uint8_t joyDrop = 1;
      if (fullness < 30) joyDrop += 2;
      if (hygiene < 30) joyDrop += 2;
      joy = dropTo(joy, joyDrop, offlineFloor);
    }
  }
  if (!isEgg()) {
    // la evolucion NO se aplica offline: queda lista y la dispara el usuario
    // tocando al bicho cuando vuelve (para que vea la transformacion)
  }
  if (sitterUntilEpoch && nowEpoch >= sitterUntilEpoch) sitterUntilEpoch = 0;
  Serial.printf("offline: %u min aplicados (nv.%u)\n", mins, level());
  save();
}

void Pet::update(uint32_t nowMs) {
  // Time correction and RTC recovery can refresh lastTick a few milliseconds
  // after loop() captured nowMs.  Unsigned subtraction would otherwise look
  // like an entire millis() wrap (about 49.7 days) and fast-forward the pet by
  // roughly 71,582 minutes in one frame.
  if ((int32_t)(nowMs - lastTick) < 0) {
    lastTick = nowMs;
  }
  // fin de ceremonia: la criatura se va y queda un huevo nuevo
  if (ceremony != CER_NONE && millis() > ceremonyUntil) {
    newEgg();
    return;
  }
  while (nowMs - lastTick >= PET_TICK_MS) {
    lastTick += PET_TICK_MS;
    tick();
  }
  resolveCareCallIfRecovered();
}

uint8_t Pet::careNeedValue(CareNeed need) const {
  switch (need) {
    case CARE_NEED_FULLNESS: return fullness;
    case CARE_NEED_HYGIENE: return hygiene;
    case CARE_NEED_ENERGY: return energy;
    case CARE_NEED_JOY: return joy;
    default: return 100;
  }
}

void Pet::addCareMistake() {
  if (careMistakes < 255) careMistakes++;
  bond = bond >= 3 ? bond - 3 : 0;
}

void Pet::clearCareCall() {
  careNeed = CARE_NEED_NONE;
  careMissed = false;
  careDueEpoch = 0;
}

void Pet::setTamagotchiMode(bool enabled) {
  if (tamagotchiMode == enabled) return;
  tamagotchiMode = enabled;
  mistakeCooldown = 0;
  clearCareCall();
  save();
}

CareNeed Pet::startCareCallIfNeeded() {
  if (!tamagotchiMode || isEgg() || ceremony != CER_NONE ||
      sitterActive(lastSeenEpoch) || careNeed != CARE_NEED_NONE ||
      lowestStat() > TAMAGOTCHI_CARE_DANGER_MAX) {
    return CARE_NEED_NONE;
  }

  // Menor valor primero; en empate: hambre, limpieza, energia, felicidad.
  CareNeed selected = CARE_NEED_FULLNESS;
  uint8_t selectedValue = fullness;
  if (hygiene < selectedValue) { selected = CARE_NEED_HYGIENE; selectedValue = hygiene; }
  if (energy < selectedValue) { selected = CARE_NEED_ENERGY; selectedValue = energy; }
  if (joy < selectedValue) selected = CARE_NEED_JOY;
  careNeed = selected;
  careMissed = false;
  careDueEpoch = 0;
  save();
  return careNeed;
}

void Pet::armCareCallDeadline(uint32_t nowEpoch) {
  if (!tamagotchiMode || careNeed == CARE_NEED_NONE || careMissed || nowEpoch == 0) return;
  careDueEpoch = nowEpoch + 15UL * 60UL;
  save();
}

bool Pet::resolveCareCallIfRecovered() {
  if (careNeed == CARE_NEED_NONE ||
      careNeedValue(careNeed) <= TAMAGOTCHI_CARE_DANGER_MAX) return false;
  clearCareCall();
  save();
  return true;
}

bool Pet::applyDueCareMiss(uint32_t nowEpoch) {
  if (!tamagotchiMode || careNeed == CARE_NEED_NONE) return false;
  if (sitterActive(nowEpoch)) return false;
  if (resolveCareCallIfRecovered()) return false;
  if (careMissed || careDueEpoch == 0 || nowEpoch < careDueEpoch) return false;
  addCareMistake();
  careMissed = true;
  careDueEpoch = 0;
  save();
  return true;
}

void Pet::tick() {
  if (ceremony != CER_NONE) return;  // el tiempo se detiene en la despedida
  ageMinutes++;

  if (isEgg()) {
    if (ageMinutes >= 3) hatch();  // si no lo tocas, eclosiona solo a los 3 min
    return;
  }

  if (walkCarePaused) {
    checkMedals();
    if (++ticksSinceSave >= 5) pendingSave = true;
    return;
  }

  if (sitterActive(lastSeenEpoch)) {
    checkMedals();
    if (++ticksSinceSave >= 5) pendingSave = true;
    return;
  }

  // 달 수면은 체력을 회복한다. 22:00~09:00에는 다른 욕구가 5분마다
  // 감소하되 30에서 멈추고, 그 밖에는 4분마다 0까지 감소한다.
  // Despierto, el pulso normal ocurre cada 3 min y los debuffs siguen fuertes.
  // El peso aun se quema; la racha de buen cuidado (goodTicks) queda en pausa.
  if (sleeping) {
    applySleepingMinute(lastSeenEpoch);
    checkMedals();  // aun puede cruzar un nivel por edad mientras duerme
    if (++ticksSinceSave >= 5) pendingSave = true;
    return;
  }

  if (ageMinutes % MINUTES_PER_LEVEL == 0) sfxPlay(SFX_LEVEL);  // subio de nivel (despierto)

  // Ritmo relajado: las necesidades normales se actualizan cada 3 minutos.
  // Los debuffs conservan toda su fuerza dentro de ese pulso de cuidado.
  if (ageMinutes % AWAKE_NEED_INTERVAL_MIN == 0) {
    fullness = clamp100(fullness - 1);
    energy = clamp100(energy - 1);
    if (fullness > 40 && poops < 3 && random(100) < 15) poops++;

    hygiene = clamp100(hygiene - 1 - 4 * poops);
    // el sobrepeso da pereza: la energia cae el doble
    energy = dropTo(energy, weightEnergyPenalty(), 0);
    if (weight > 0) weight--;

    int dJoy = -1;
    if (fullness < 30) dJoy -= 2;
    if (hygiene < 30) dJoy -= 2;
    joy = clamp100(joy + dJoy);
  }

  // la disciplina forja la defensa: 12 h seguidas bien cuidado = +1 DEF
  if (lowestStat() >= 40) {
    if (++goodTicks >= 720) {
      goodTicks = 0;
      if (trDef < 100) trDef++;
    }
  } else {
    goodTicks = 0;
  }

  // Descuido: dejar una estadistica por los suelos cuenta como error de
  // cuidado (con enfriamiento para no contar el mismo descuido cada minuto)
  if (!tamagotchiMode && mistakeCooldown > 0) mistakeCooldown--;
  if (!tamagotchiMode && lowestStat() <= 10 && mistakeCooldown == 0) {
    // PokeTama conserva literalmente el comportamiento historico.
    careMistakes++;
    mistakeCooldown = 30;
    if (bond > 3) bond -= 3;
  }

  checkMedals();  // la evolucion la dispara el usuario (canEvolveNow + tap), no el tick

  // abandono total: con TODO a cero durante una hora queda lista para escaparse;
  // NO se va sola, la dispara el usuario con el boton (final triste, lo presencia)
  if (fullness == 0 && joy == 0 && energy == 0 && hygiene == 0) {
    if (neglectTicks < RUNAWAY_TICKS) neglectTicks++;
  } else {
    neglectTicks = 0;  // un solo cuidado la salva
  }

  // ciclo completo: adulto + 24 h en forma final; la despedida NO salta sola
  // lista (canFarewellNow) y la dispara el usuario con el boton, para que la vea

  // autoguardado periodico: NO escribir a flash aqui (corre dentro del loop,
  // mientras se anima); solo marcar y dejar que el loop lo vuelque al atenuar
  if (++ticksSinceSave >= 5) pendingSave = true;
}

// vuelca el guardado periodico pendiente (lo llama el loop en un momento sin
// animacion para que el paron de la escritura a flash no se vea)
void Pet::flushSave() {
  if (pendingSave) save();
}

// quedan miembros sin registrar en la linea evolutiva de esta base?
bool Pet::lineHasUnregistered(int16_t base) const {
  int16_t cur = base;
  for (int guard = 0; cur >= 1 && cur <= 151 && guard < 6; guard++) {
    if (!isRegistered(cur)) return true;
    if (cur == DEX_EEVEE) {
      for (int16_t b = 134; b <= 136; b++)
        if (!isRegistered(b)) return true;
      return false;
    }
    cur = DEX_TBL[cur].evolvesTo;
  }
  return false;
}

uint8_t Pet::eggRarity() const {
  return (eggTarget >= 1 && eggTarget <= 151) ? DEX_TBL[eggTarget].rarity : R_COMUN;
}

// elige la especie del huevo: tirada de rareza (mejorada por una despedida
// completa, castigada por una escapada) y sesgo hacia lineas incompletas
int16_t Pet::pickEggSpecies() {
  // primera partida: inicial clasico
  if (registeredCount() == 0) {
    return CLASSIC_DEX[random(NUM_CLASSIC_DEX)];
  }

  uint8_t tier = R_COMUN;
  if (lastEnd != CER_RUNAWAY) {
    bool blessed = (lastEnd == CER_FAREWELL);
    int rare = (blessed ? 45 : 27) + careBonus();
    int leg = (registeredCount() >= 25) ? (blessed ? 10 : 3) + careBonus() / 3 : 0;
    int r = random(100);
    if (r < leg) tier = R_LEGENDARIO;
    else if (r < leg + rare) tier = R_RARO;
  }

  // candidatos del tier con linea incompleta; si no hay, baja de tier;
  // si la pokedex del tier esta completa, vale cualquiera del tier
  for (int pass = 0; pass < 2; pass++) {
    for (int t = tier; t >= R_COMUN; t--) {
      int16_t cand[80];
      int n = 0;
      for (int16_t d = 1; d <= 151 && n < 80; d++) {
        if (DEX_TBL[d].rarity != t) continue;
        if (pass == 0 && !lineHasUnregistered(d)) continue;
        cand[n++] = d;
      }
      if (n > 0) return cand[random(n)];
    }
  }
  return CLASSIC_DEX[random(NUM_CLASSIC_DEX)];  // inalcanzable, por si acaso
}

void Pet::registerSpecies(int16_t dex) {
  if (dex < 1 || dex > 151) return;
  dexReg[(dex - 1) >> 3] |= (1 << ((dex - 1) & 7));
  if (shiny) dexShinyReg[(dex - 1) >> 3] |= (1 << ((dex - 1) & 7));
}

// la racha y el vinculo mejoran el sorteo del huevo (0..~14)
int Pet::careBonus() const {
  int s = streak > 30 ? 30 : streak;
  return s / 3 + bond / 25;
}

uint8_t Pet::dailyGoalTarget(uint8_t goalType) const {
  switch (goalType) {
    case DAILY_GOAL_CARE: return 2;
    case DAILY_GOAL_PLAY: return 2;
    case DAILY_GOAL_BATTLE: return 1;
    case DAILY_GOAL_ITEM: return 1;
    case DAILY_GOAL_WALK: return 150;
    default: return 1;
  }
}

bool Pet::dailyGoalComplete(uint8_t index) const {
  return index < DAILY_GOAL_COUNT && (dailyGoalDone & (1 << index));
}

void Pet::ensureDailyGoals() {
  if (isEgg() || ceremony != CER_NONE) return;
  uint32_t d = today();
  if (d == 0 || d == dailyGoalDay) return;
  static const uint8_t POOL[] = {
    DAILY_GOAL_CARE, DAILY_GOAL_PLAY, DAILY_GOAL_BATTLE, DAILY_GOAL_ITEM, DAILY_GOAL_WALK
  };
  uint8_t seed = (uint8_t)((d + (speciesId > 0 ? speciesId : 0)) % 5);
  for (uint8_t i = 0; i < DAILY_GOAL_COUNT; i++) {
    dailyGoalType[i] = POOL[(seed + i) % 5];
    dailyGoalProgress[i] = 0;
  }
  dailyGoalDone = 0;
  dailyGoalDay = d;
  save();
}

void Pet::applyDailyReward() {
  joy = clamp100((int)joy + 4);
  addBond(1);
}

void Pet::noteDailyGoal(uint8_t goalType, uint8_t amount) {
  if (isEgg() || ceremony != CER_NONE || amount == 0) return;
  ensureDailyGoals();
  if (dailyGoalDay == 0) return;
  bool changed = false;
  for (uint8_t i = 0; i < DAILY_GOAL_COUNT; i++) {
    if (dailyGoalType[i] != goalType || dailyGoalComplete(i)) continue;
    uint8_t target = dailyGoalTarget(goalType);
    uint16_t next = (uint16_t)dailyGoalProgress[i] + amount;
    dailyGoalProgress[i] = next > target ? target : next;
    changed = true;
    if (dailyGoalProgress[i] >= target) {
      dailyGoalDone |= (1 << i);
      applyDailyReward();
      heartUntil = millis() + HEART_MS;
      sfxPlay(SFX_DAILY_GOAL);
    }
  }
  if (changed) save();
}

// primer cuidado del dia: avanza la racha y afianza el vinculo
void Pet::registerCare() {
  if (isEgg() || ceremony != CER_NONE) return;
  uint32_t d = today();
  if (d == 0 || d == lastCareDay) return;  // sin reloj, o ya conto hoy
  if (lastCareDay == 0 || d == lastCareDay + 1) {
    streak++;
  } else {
    streak = 1;        // hubo un hueco de dias
    lastMilestone = 0;
  }
  lastCareDay = d;
  bondToday = 0;
  if (streak > bestStreak) bestStreak = streak;
  bond = clamp100(bond + 4);
  uint16_t ms = (streak >= 100) ? 100 : (streak >= 30) ? 30
              : (streak >= 7)   ? 7   : (streak >= 3)  ? 3 : 0;
  if (ms > lastMilestone) {
    lastMilestone = ms;
    milestoneUntil = millis() + 4500;
  }
  checkMedals();
  save();
}

void Pet::addBond(uint8_t amt) {
  if (bondToday >= 8) return;  // tope diario: el vinculo no se farmea
  bond = clamp100(bond + amt);
  bondToday += amt;
}

void Pet::checkMedals() {
  if (isEgg()) return;
  uint16_t before = medals;
  if (level() >= 10) medals |= MED_LV10;
  if (level() >= 25) medals |= MED_LV25;
  if (level() >= 50) medals |= MED_LV50;
  if (berryKnown) medals |= MED_BERRY;
  if (streak >= 7) medals |= MED_STREAK7;
  if (bond >= 100) medals |= MED_BOND;
  if (DEX_TBL[speciesId].evolvesTo == 0) medals |= MED_FINAL;
  if (weight == 0 && level() >= 5 && careMistakes == 0) medals |= MED_FIT;
  uint16_t gained = medals & ~before;
  if (gained) {
    for (uint16_t m = gained; m; m &= (m - 1)) totalMedals++;
    newMedal = gained;
    medalUntil = millis() + 4000;
    if (!sleeping) sfxPlay(SFX_MEDAL);
    save();
  }
}

void Pet::rename(const char *name) {
  strncpy(nick, name, sizeof(nick) - 1);
  nick[sizeof(nick) - 1] = 0;
  save();
}

static uint16_t calcStat(uint8_t base, uint8_t gene, uint8_t lvl, uint8_t tr) {
  return (uint16_t)base * gene / 100 + lvl + tr;
}

uint16_t Pet::atkStat() const {
  return isEgg() ? 0 : calcStat(DEX_TBL[speciesId].bAtk, geneAtk, level(), trAtk);
}
uint16_t Pet::defStat() const {
  return isEgg() ? 0 : calcStat(DEX_TBL[speciesId].bDef, geneDef, level(), trDef);
}
uint16_t Pet::speStat() const {
  return isEgg() ? 0 : calcStat(DEX_TBL[speciesId].bSpe, geneSpe, level(), trSpe);
}

uint8_t Pet::weightEnergyPenalty() const {
  if (weight >= 80) return 2;
  if (weight >= 60) return 1;
  return 0;
}

uint16_t Pet::registeredCount() const {
  uint16_t n = 0;
  for (int i = 1; i <= 151; i++)
    if (isRegistered(i)) n++;
  return n;
}

uint16_t Pet::caughtCount() const {
  uint16_t n = 0;
  for (int i = 1; i <= 151; i++)
    if (isCaught(i)) n++;
  return n;
}

uint16_t Pet::linkedCount() const {
  uint16_t n = 0;
  for (int i = 1; i <= 151; i++)
    if (isLinked(i)) n++;
  return n;
}

uint16_t Pet::knownDexCount() const {
  uint16_t n = 0;
  for (int i = 1; i <= 151; i++)
    if (isKnown(i)) n++;
  return n;
}

uint8_t Pet::collectionRank() const {
  uint16_t known = knownDexCount();
  if (known >= 151) return 5;
  if (known >= 100) return 4;
  if (known >= 50) return 3;
  if (known >= 25) return 2;
  if (known >= 10) return 1;
  return 0;
}

uint8_t Pet::unlockedCollectionFrameCount() const {
  return (uint8_t)(collectionRank() + 1);
}

bool Pet::setCollectionFrame(uint8_t frame) {
  if (frame >= unlockedCollectionFrameCount()) return false;
  if (collectionFrame == frame) return true;
  collectionFrame = frame;
  save();
  return true;
}

uint8_t Pet::nextDexGoal() const {
  static const uint8_t GOALS[] = { 10, 25, 50, 100, 151 };
  uint16_t known = knownDexCount();
  for (uint8_t i = 0; i < sizeof(GOALS); i++)
    if (known < GOALS[i]) return GOALS[i];
  return 151;
}

uint8_t Pet::applyDexRewards() {
  // Collection frames were removed from the Korean firmware UI. Keep this
  // compatibility entry point for old saves and simulator code, but never
  // create a hidden frame unlock or its celebration again.
  return 0;
}

void Pet::registerCaught(int16_t dex) {
  if (dex < 1 || dex > 151) return;
  dexCaught[(dex - 1) >> 3] |= (1 << ((dex - 1) & 7));
  save();
}

uint8_t Pet::catchChanceForWild(int16_t wildDex, uint8_t wildLevel, uint8_t petLevel, bool closeWin) const {
  if (wildDex < 1 || wildDex > DEX_COUNT) return 0;
  const DexEntry &wild = DEX_TBL[wildDex];
  if (wild.rarity == R_LEGENDARIO) return 0;
  int chance = wild.rarity == R_RARO ? 28 : 55;
  int levelGap = (int)wildLevel - (int)(petLevel ? petLevel : 1);
  if (levelGap > 0) chance -= levelGap * 4;
  else if (levelGap < 0) chance += (-levelGap) * 2;
  if (closeWin) chance += 8;
  chance += bond / 20;
  if (wild.rarity == R_RARO && chance > 60) chance = 60;
  if (chance > 75) chance = 75;
  if (chance < 10) chance = 10;
  return (uint8_t)chance;
}

uint8_t Pet::respectCatchChanceForWild(int16_t wildDex, uint8_t wildLevel, uint8_t petLevel) const {
  uint8_t normal = catchChanceForWild(wildDex, wildLevel, petLevel, true);
  if (normal == 0) return 0;
  uint8_t chance = (uint8_t)((uint16_t)normal * 40 / 100);
  if (chance < 5) chance = 5;
  if (chance > 25) chance = 25;
  return chance;
}

bool Pet::tryCatchWild(int16_t wildDex, uint8_t wildLevel, uint8_t petLevel, bool closeWin, uint8_t luckRoll) {
  uint8_t chance = catchChanceForWild(wildDex, wildLevel, petLevel, closeWin);
  if (chance == 0) return false;
  if ((luckRoll % 100) < chance) {
    registerCaught(wildDex);
    joy = clamp100((int)joy + 4);
    addBond(1);
    save();
    return true;
  }
  return false;
}

bool Pet::tryRespectCatchWild(int16_t wildDex, uint8_t wildLevel, uint8_t petLevel, uint8_t luckRoll) {
  uint8_t chance = respectCatchChanceForWild(wildDex, wildLevel, petLevel);
  if (chance == 0) return false;
  if ((luckRoll % 100) < chance) {
    registerCaught(wildDex);
    save();
    return true;
  }
  return false;
}

// Adulto, forma final y 24 h completas desde que alcanzo esa forma.
bool Pet::canFarewellNow() const {
  return !isEgg() && !sleeping && ceremony == CER_NONE &&
         DEX_TBL[speciesId].evolvesTo == 0 && ageMinutes >= ADULT_AGE_MIN &&
         ageMinutes >= finalFormAgeMinutes + FINAL_FORM_FAREWELL_DELAY_MIN;
}

uint32_t Pet::dayStamp(uint32_t nowEpoch) const {
  uint32_t epoch = nowEpoch ? nowEpoch : lastSeenEpoch;
  // +1 reserva el cero para "nunca rechazado". Sin RTC, usa el dia de edad.
  return epoch ? epoch / 86400UL + 1UL : ageMinutes / 1440UL + 1UL;
}

bool Pet::wantEvolveButton(uint32_t nowEpoch) const {
  return canEvolveNow() && evoDeclinedDay != dayStamp(nowEpoch);
}

bool Pet::wantFarewellButton(uint32_t nowEpoch) const {
  return canFarewellNow() && farDeclinedDay != dayStamp(nowEpoch);
}

void Pet::declineEvolve(uint32_t nowEpoch) {
  evoDeclinedDay = dayStamp(nowEpoch);
  save();
}

void Pet::declineFarewell(uint32_t nowEpoch) {
  farDeclinedDay = dayStamp(nowEpoch);
  save();
}

// abandono total durante 1h: lista para escaparse. La dispara el usuario con el
// boton (final triste); cuidarla un solo tick la salva (neglectTicks se resetea)
bool Pet::canRunawayNow() const {
  return !isEgg() && !sleeping && ceremony == CER_NONE && neglectTicks >= RUNAWAY_TICKS;
}

void Pet::startFarewell() {
  if (isEgg() || ceremony != CER_NONE) return;
  lastEnd = CER_FAREWELL;
  ceremony = CER_FAREWELL;
  ceremonyUntil = millis() + CEREMONY_MS;
  heartUntil = ceremonyUntil;  // corazones durante toda la despedida
  sfxPlay(SFX_BYE);
  save();
}

void Pet::startRunaway() {
  if (isEgg() || ceremony != CER_NONE) return;
  lastEnd = CER_RUNAWAY;
  ceremony = CER_RUNAWAY;
  ceremonyUntil = millis() + CEREMONY_MS;
  sfxPlay(SFX_BYE);
  save();
}

void Pet::release() {
  if (isEgg() || ceremony != CER_NONE) return;
  lastEnd = CER_RELEASE;
  ceremony = CER_RELEASE;
  ceremonyUntil = millis() + CEREMONY_MS;
  heartUntil = ceremonyUntil;
  sfxPlay(SFX_BYE);
  save();
}

void Pet::hatch() {
  speciesId = eggTarget;
  shiny = eggShiny;
  // genes del individuo: 90-110% por stat (cada crianza es unica)
  geneAtk = 90 + random(21);
  geneDef = 90 + random(21);
  geneSpe = 90 + random(21);
  battleMove1 = chooseCommonMoveForSpecies(
      speciesId, (uint32_t)random(0x7fffffffL) ^ ((uint32_t)speciesId << 16));
  trAtk = trDef = trSpe = 0;
  berryKnown = false;
  bond = 0;          // vinculo, medallas y nombre son del individuo
  bondToday = 0;
  medals = 0;
  newMedal = 0;
  nick[0] = 0;
  finalFormAgeMinutes = 0;
  registerSpecies(speciesId);  // criado = registrado en la pokedex
  checkMedals();     // por si nace ya en forma final (legendario)
  sfxPlay(SFX_HATCH);
  save();
}

// ¿se dan ya las condiciones para evolucionar? Cada descuido retrasa la
// evolucion 1 nivel, y ademas tiene que estar bien cuidado en ese momento
// (ninguna estadistica por debajo de 40). NO evoluciona sola: la dispara el
// usuario tocando al bicho (evolve()), para que vea la transformacion.
bool Pet::canEvolveNow() const {
  if (isEgg() || sleeping || ceremony != CER_NONE) return false;
  const DexEntry &d = DEX_TBL[speciesId];
  if (d.evolvesTo == 0) return false;
  return level() >= (uint8_t)(d.evolveLevel + careMistakes) && lowestStat() >= 40;
}

void Pet::evolve() {
  if (!canEvolveNow()) return;
  const DexEntry &d = DEX_TBL[speciesId];
  prevSpeciesId = speciesId;
  int16_t next = d.evolvesTo;
  if (speciesId == DEX_EEVEE) {
    // rama de Eevee: prefiere la evolucion que falte en la pokedex
    int16_t opts[3];
    int n = 0;
    for (int16_t b = 134; b <= 136; b++)
      if (!isRegistered(b)) opts[n++] = b;
    next = n > 0 ? opts[random(n)] : (int16_t)(134 + random(3));
  }
  speciesId = next;
  evoDeclinedDay = 0;
  if (DEX_TBL[speciesId].evolvesTo == 0) finalFormAgeMinutes = ageMinutes;
  registerSpecies(speciesId);
  sfxPlay(SFX_EVOLVE);
  evolveUntil = millis() + EVOLVE_ANIM_MS;
  save();
}

void Pet::feed() {
  feedBerry(0);
}

void Pet::feedBerry(uint8_t color) {
  if (ceremony != CER_NONE) return;
  if (isEgg() || sleeping) return;
  if (fullness >= 100) return;
  if (lovesBerry(color)) {
    fullness = clamp100(fullness + 35);
    joy = clamp100(joy + 10);
    heartUntil = millis() + HEART_MS;  // "le encanta!"
    berryKnown = true;                 // descubierto: se muestra en la ficha
    addBond(2);
  } else {
    fullness = clamp100(fullness + 25);
  }
  eatUntil = millis() + EAT_ANIM_MS;
  registerCare();
  noteDailyGoal(DAILY_GOAL_CARE, 1);
  save();
}

void Pet::feedCandy() {
  if (ceremony != CER_NONE) return;
  if (isEgg() || sleeping) return;
  fullness = clamp100(fullness + 10);
  joy = clamp100(joy + 20);
  energy = clamp100(energy + 25);
  weight = clamp100(weight + 12);  // las chuches pasan factura
  eatUntil = millis() + EAT_ANIM_MS;
  registerCare();
  noteDailyGoal(DAILY_GOAL_CARE, 1);
  save();
}

void Pet::playResult(uint8_t score) {
  if (ceremony != CER_NONE || isEgg()) return;
  uint8_t performance = minigamePerformance(score, MINIGAME_MAX_RUNNER);
  uint8_t gain = minigameRewardTier(performance);
  trSpe = clamp100((int)trSpe + gain);
  joy = clamp100((int)joy + 4 + performance * 11 / 100);
  energy = dropTo(energy, 5, 5);
  fullness = dropTo(fullness, 2, 5);
  int burn = (int)weight - performance * 5 / 100;
  weight = burn > 0 ? burn : 0;
  if (performance >= 20) heartUntil = millis() + HEART_MS;
  if (score > gameHi) gameHi = score;  // nuevo record
  addBond(2);
  registerCare();
  noteDailyGoal(DAILY_GOAL_PLAY, 1);
  save();
}

uint8_t Pet::applyCatchResult(uint8_t score) {
  if (ceremony != CER_NONE || isEgg()) return 0;
  if (score > 100) score = 100;
  uint8_t performance = minigamePerformance(score, MINIGAME_MAX_SNORLAX);
  uint8_t gain = minigameRewardTier(performance);
  trDef = clamp100((int)trDef + gain);
  energy = clamp100((int)energy + 4 + performance * 14 / 100);
  joy = clamp100((int)joy + 4 + performance * 8 / 100);
  if (performance >= 20) heartUntil = millis() + HEART_MS;
  if (score > catchHi) catchHi = score;
  addBond(1);
  registerCare();
  noteDailyGoal(DAILY_GOAL_PLAY, 1);
  save();
  return gain;
}

uint8_t Pet::applyMemoResult(uint8_t rounds) {
  if (ceremony != CER_NONE || isEgg()) return 0;
  uint8_t performance = minigamePerformance(rounds, MINIGAME_MAX_EEVEE);
  uint8_t gain = minigameRewardTier(performance);
  joy = clamp100((int)joy + 4 + performance * 11 / 100);
  fullness = clamp100((int)fullness + 4 + performance * 16 / 100);
  int burn = (int)weight - performance * 5 / 100;
  weight = burn > 0 ? burn : 0;
  if (performance >= 20) heartUntil = millis() + HEART_MS;
  if (rounds > memoHi) memoHi = rounds;
  addBond(2);
  registerCare();
  noteDailyGoal(DAILY_GOAL_PLAY, 1);
  save();
  return gain;
}

uint8_t Pet::applyDiglettResult(uint8_t score) {
  if (ceremony != CER_NONE || isEgg()) return 0;
  uint8_t performance = minigamePerformance(score, MINIGAME_MAX_DIGLETT);
  uint8_t gain = minigameRewardTier(performance);
  trAtk = clamp100((int)trAtk + gain);
  joy = clamp100((int)joy + 4 + performance * 11 / 100);
  energy = dropTo(energy, 5, 8);
  fullness = dropTo(fullness, 2, 5);
  if (performance >= 20) heartUntil = millis() + HEART_MS;
  if (score > diglettHi) diglettHi = score;
  addBond(1);
  registerCare();
  noteDailyGoal(DAILY_GOAL_PLAY, 1);
  save();
  return gain;
}

uint8_t Pet::applyTypeResult(uint8_t score) {
  if (ceremony != CER_NONE || isEgg()) return 0;
  uint8_t performance = minigamePerformance(score, MINIGAME_MAX_MAGIKARP);
  uint8_t gain = minigameRewardTier(performance);
  joy = clamp100((int)joy + 4 + performance * 11 / 100);
  energy = clamp100((int)energy + 4 + performance * 14 / 100);
  fullness = dropTo(fullness, 2, 5);
  int burn = (int)weight - performance * 5 / 100;
  weight = burn > 0 ? burn : 0;
  if (performance >= 20) heartUntil = millis() + HEART_MS;
  if (score > typeHi) typeHi = score;
  addBond(1);
  registerCare();
  noteDailyGoal(DAILY_GOAL_PLAY, 1);
  save();
  return gain;
}

void Pet::playWithFriend() {
  if (ceremony != CER_NONE || isEgg() || sleeping) return;
  joy = clamp100((int)joy + 18);
  energy = dropTo(energy, 5, 8);
  fullness = dropTo(fullness, 2, 5);
  heartUntil = millis() + HEART_MS;
  addBond(2);
  registerCare();
  noteDailyGoal(DAILY_GOAL_PLAY, 1);
  save();
}

void Pet::registerLinked(int16_t dex) {
  if (dex < 1 || dex > 151 || isLinked(dex)) return;
  dexLinked[(dex - 1) >> 3] |= (1 << ((dex - 1) & 7));
  save();
}

bool Pet::applyPetEvent(uint8_t eventType) {
  if (ceremony != CER_NONE || isEgg()) return false;
  if (eventType == PET_EVENT_BERRY) {
    fullness = clamp100((int)fullness + 10);
    joy = clamp100((int)joy + 4);
  } else if (eventType == PET_EVENT_HEART) {
    joy = clamp100((int)joy + 6);
    addBond(1);
  } else if (eventType == PET_EVENT_SPARKLE) {
    joy = clamp100((int)joy + 5);
    if (energy <= hygiene) energy = clamp100((int)energy + 3);
    else hygiene = clamp100((int)hygiene + 3);
  } else {
    return false;
  }
  heartUntil = millis() + HEART_MS;
  registerCare();
  noteDailyGoal(DAILY_GOAL_CARE, 1);
  save();
  return true;
}

uint8_t Pet::interactPet(bool eveningBonus) {
  if (ceremony != CER_NONE || isEgg() || sleeping) return PET_INTERACT_NONE;
  uint32_t nowMinute = ageMinutes ? ageMinutes : 1;
  if (lastPetInteractMinute && nowMinute < lastPetInteractMinute + 10) {
    heartUntil = millis() + HEART_MS;
    return PET_INTERACT_NONE;
  }
  lastPetInteractMinute = nowMinute;
  uint8_t result = PET_INTERACT_JOY;
  PetPersonality p = personality();
  int joyGain = (p == PERS_PLAYFUL) ? 4 : 2;
  joy = clamp100((int)joy + joyGain);
  if (p == PERS_LAZY) {
    energy = clamp100((int)energy + 2);
    result |= PET_INTERACT_ENERGY;
  }
  bool bondGain = eveningBonus || p == PERS_CALM || (p == PERS_BRAVE && battleWins > 0);
  if (bondGain) {
    uint8_t before = bond;
    addBond(1);
    if (bond > before) result |= PET_INTERACT_BOND;
  }
  heartUntil = millis() + HEART_MS;
  registerCare();
  noteDailyGoal(DAILY_GOAL_CARE, 1);
  save();
  return result;
}

PetPersonality Pet::personality() const {
  if (isEgg()) return PERS_BALANCED;
  if (weight >= 72 || energy <= 20) return PERS_LAZY;
  if (battleWins >= 8 || bestBattleStreak >= 4) return PERS_BRAVE;
  if (catchHi >= 18 || memoHi >= 8 || gameHi >= 24 || trSpe >= 55) return PERS_PLAYFUL;
  if ((bond >= 45 && careMistakes <= 1) || (streak >= 5 && careMistakes == 0)) return PERS_CALM;
  return PERS_BALANCED;
}

BattleReward Pet::applyBattleWin(int16_t wildDex, bool closeWin) {
  BattleReward reward;
  if (ceremony != CER_NONE || isEgg()) return reward;
  if (wildDex < 1 || wildDex > DEX_COUNT) wildDex = 1;
  const DexEntry &wild = DEX_TBL[wildDex];
  reward.amount = (wild.rarity == R_RARO) ? 2 : 1;
  if (closeWin) reward.amount++;
  if (wild.bAtk >= wild.bDef && wild.bAtk >= wild.bSpe) {
    reward.stat = BATTLE_REWARD_DEF;
    trDef = clamp100((int)trDef + reward.amount);
  } else if (wild.bDef >= wild.bAtk && wild.bDef >= wild.bSpe) {
    reward.stat = BATTLE_REWARD_ATK;
    trAtk = clamp100((int)trAtk + reward.amount);
  } else {
    reward.stat = BATTLE_REWARD_SPE;
    trSpe = clamp100((int)trSpe + reward.amount);
  }
  battleWins++;
  battleStreak++;
  if (battleStreak > bestBattleStreak) bestBattleStreak = battleStreak;
  joy = clamp100((int)joy + 8 + (closeWin ? 4 : 0));
  energy = dropTo(energy, 8, 20);
  fullness = dropTo(fullness, 3, 10);
  addBond(closeWin ? 3 : 2);
  registerCare();
  noteDailyGoal(DAILY_GOAL_BATTLE, 1);
  save();
  return reward;
}

void Pet::applyBattleLoss() {
  if (ceremony != CER_NONE || isEgg()) return;
  battleLosses++;
  battleStreak = 0;
  joy = dropTo(joy, 12, 20);
  energy = dropTo(energy, 18, 20);
  fullness = dropTo(fullness, 4, 10);
  save();
}

bool Pet::canReceiveExpeditionItem(ExpeditionItem item) const {
  if (item >= EXP_ITEM_COUNT || itemCounts[item] >= EXP_ITEM_MAX) return false;
  return item != EXP_ITEM_TRAIN || trAtk < 100 || trDef < 100 || trSpe < 100;
}

bool Pet::expeditionInventoryFull() const {
  for (uint8_t i = 0; i < EXP_ITEM_COUNT; i++) {
    if (canReceiveExpeditionItem((ExpeditionItem)i)) return false;
  }
  return true;
}

bool Pet::useExpeditionItem(ExpeditionItem item, int8_t trainingStat) {
  if (item >= EXP_ITEM_COUNT || itemCounts[item] == 0) return false;
  if (item == EXP_ITEM_SNACK) {
    fullness = clamp100((int)fullness + 25);
    joy = clamp100((int)joy + 5);
  } else if (item == EXP_ITEM_ENERGY) {
    energy = clamp100((int)energy + 30);
  } else if (item == EXP_ITEM_CARE) {
    hygiene = clamp100((int)hygiene + 30);
    if (poops > 0) poops--;
  } else if (item == EXP_ITEM_TRAIN) {
    if (trainingStat == TRAIN_STAT_ATK && trAtk < 100) trAtk = clamp100((int)trAtk + 2);
    else if (trainingStat == TRAIN_STAT_DEF && trDef < 100) trDef = clamp100((int)trDef + 2);
    else if (trainingStat == TRAIN_STAT_SPE && trSpe < 100) trSpe = clamp100((int)trSpe + 2);
    else return false;
  } else {
    // Move discs are consumed only by learnMove1(), after the replacement
    // candidate has been shown in the profile UI.
    return false;
  }
  itemCounts[item]--;
  noteDailyGoal(DAILY_GOAL_ITEM, 1);
  save();
  return true;
}

bool Pet::sitterActive(uint32_t nowEpoch) const {
  // 달 수면과 시터가 동시에 보이는 상태는 허용하지 않는다. 과거 버전에서
  // 남은 만료 시각이 RTC 역행 뒤 되살아나더라도 수면 장면이 우선한다.
  return !sleeping && nowEpoch != 0 && sitterUntilEpoch != 0 && nowEpoch < sitterUntilEpoch;
}

bool Pet::canStartSitter(uint32_t nowEpoch) const {
  if (nowEpoch == 0 || isEgg() || sleeping || ceremony != CER_NONE || sitterActive(nowEpoch)) {
    return false;
  }
  // Chansey cannot be used as an emergency escape from an already critical
  // care state. The player must restore every need above 15 first.
  if (fullness <= 15 || joy <= 15 || energy <= 15 || hygiene <= 15) return false;
  return (nowEpoch % 86400UL) < 19UL * 3600UL;
}

bool Pet::startSitter(uint32_t nowEpoch) {
  if (!canStartSitter(nowEpoch)) return false;
  sitterUntilEpoch = nowEpoch - (nowEpoch % 86400UL) + 19UL * 3600UL;
  sleeping = false;
  clearCareCall();
  save();
  return true;
}

bool Pet::canStartWalk() const {
  if (isEgg() || sleeping || ceremony != CER_NONE || careNeed != CARE_NEED_NONE) return false;
  if (fullness <= TAMAGOTCHI_CARE_DANGER_MAX ||
      joy <= TAMAGOTCHI_CARE_DANGER_MAX ||
      hygiene <= TAMAGOTCHI_CARE_DANGER_MAX) return false;
  // The fixed eight-point cost must never push energy into the red gauge.
  return energy >= TAMAGOTCHI_CARE_DANGER_MAX + 9;
}

bool Pet::beginWalk() {
  if (!canStartWalk()) return false;
  energy = clamp100((int)energy - 8);
  walkCarePaused = true;
  save();
  return true;
}

void Pet::restoreWalkPause() {
  walkCarePaused = true;
}

void Pet::endWalkPause() {
  if (!walkCarePaused) return;
  walkCarePaused = false;
  save();
}

WalkReward Pet::applyWalkReward(uint16_t steps, uint8_t itemRoll) {
  WalkReward reward;
  if (steps < 150 || isEgg() || ceremony != CER_NONE) return reward;

  reward.tier = steps >= 1000 ? 4 : steps >= 600 ? 3 : steps >= 300 ? 2 : 1;
  uint32_t rng = (uint32_t)itemRoll + steps * 1103515245UL + 12345UL;
  auto nextRoll = [&]() -> uint8_t {
    rng = rng * 1664525UL + 1013904223UL;
    return (uint8_t)(rng >> 24);
  };
  auto addItem = [&](ExpeditionItem preferred, bool commonOnly) {
    if (reward.count >= WALK_REWARD_MAX) return;
    uint8_t span = commonOnly ? 3 : 4;
    for (uint8_t i = 0; i < span; i++) {
      ExpeditionItem candidate = (ExpeditionItem)(((uint8_t)preferred + i) % span);
      if (!canReceiveExpeditionItem(candidate)) continue;
      itemCounts[candidate]++;
      reward.items[reward.count++] = candidate;
      return;
    }
  };

  // 누적 보상: 1단계 한 개에 2단계 보상이 차례로 더해진다.
  addItem((ExpeditionItem)(nextRoll() % 3), true);
  if (reward.tier >= 2) addItem((ExpeditionItem)(nextRoll() % 4), false);
  if (reward.tier >= 3) addItem((ExpeditionItem)(nextRoll() % 4), false);

  if (reward.tier >= 4) {
    uint32_t day = today();
    bool firstThousandToday = day != 0 && day > walkDiscDay;
    if (firstThousandToday) {
      walkDiscDay = day;
      if (canReceiveExpeditionItem(EXP_ITEM_MOVE)) {
        itemCounts[EXP_ITEM_MOVE]++;
        reward.items[reward.count++] = EXP_ITEM_MOVE;
        reward.dailyMoveDisc = true;
      } else {
        addItem((ExpeditionItem)(nextRoll() % 4), false);
        addItem((ExpeditionItem)(nextRoll() % 4), false);
      }
    } else {
      addItem((ExpeditionItem)(nextRoll() % 4), false);
      addItem((ExpeditionItem)(nextRoll() % 4), false);
    }
  }

  joy = clamp100((int)joy + reward.tier * 3);
  addBond(reward.tier > 3 ? 3 : reward.tier);
  registerCare();
  noteDailyGoal(DAILY_GOAL_WALK, steps > 255 ? 255 : (uint8_t)steps);
  save();
  return reward;
}

CommunicationReward Pet::applyCommunicationBattleReward(bool won, uint8_t itemRoll) {
  CommunicationReward reward;
  if (isEgg() || ceremony != CER_NONE) return reward;
  uint8_t moveChance = won ? 10 : 4;
  bool moveDisc = itemRoll % 100 < moveChance && canReceiveExpeditionItem(EXP_ITEM_MOVE);
  ExpeditionItem preferred = moveDisc ? EXP_ITEM_MOVE : (ExpeditionItem)(itemRoll % 4);
  for (uint8_t i = 0; i < EXP_ITEM_COUNT; i++) {
    ExpeditionItem candidate = moveDisc ? EXP_ITEM_MOVE : (ExpeditionItem)((preferred + i) % 4);
    if (canReceiveExpeditionItem(candidate)) {
      reward.item = candidate;
      break;
    }
  }
  if (reward.item == EXP_ITEM_NONE) return reward;

  uint8_t wanted = moveDisc ? 1 : (won ? 2 : 1);
  uint8_t room = EXP_ITEM_MAX - itemCounts[reward.item];
  reward.amount = wanted < room ? wanted : room;
  itemCounts[reward.item] += reward.amount;
  if (won) {
    battleWins++;
    battleStreak++;
    if (battleStreak > bestBattleStreak) bestBattleStreak = battleStreak;
    joy = clamp100((int)joy + 10);
    addBond(3);
  } else {
    battleLosses++;
    battleStreak = 0;
    joy = dropTo(joy, 4, 20);
    addBond(1);
  }
  energy = dropTo(energy, 10, 15);
  fullness = dropTo(fullness, 4, 10);
  registerCare();
  noteDailyGoal(DAILY_GOAL_BATTLE, 1);
  save();
  return reward;
}

void Pet::createCommunicationEgg(uint8_t shinyRoll) {
  if (isEgg() || ceremony != CER_NONE || ageMinutes < FAREWELL_AGE_MIN) return;
  lastEnd = CER_FAREWELL;
  newEgg();
  // Link pairing has a deliberately high 1/4 shiny chance.
  eggShiny = (shinyRoll % 4U) == 0;
  save();
}

uint16_t Pet::signatureMove() const {
  return isEgg() ? MOVE_NONE : signatureMoveForSpecies(speciesId);
}

bool Pet::canRelearnMove1() const {
  if (isEgg() || ceremony != CER_NONE || itemCounts[EXP_ITEM_MOVE] == 0) return false;
  uint8_t count = commonMoveCountForSpecies(speciesId);
  for (uint8_t i = 0; i < count; i++)
    if (commonMoveForSpecies(speciesId, i) != battleMove1) return true;
  return false;
}

uint16_t Pet::previewMove1(uint32_t seed) const {
  if (!canRelearnMove1()) return MOVE_NONE;
  uint8_t count = commonMoveCountForSpecies(speciesId);
  uint8_t alternatives = 0;
  for (uint8_t i = 0; i < count; i++)
    if (commonMoveForSpecies(speciesId, i) != battleMove1) alternatives++;
  if (!alternatives) return MOVE_NONE;
  uint8_t pick = (uint8_t)(seed % alternatives);
  for (uint8_t i = 0; i < count; i++) {
    uint16_t candidate = commonMoveForSpecies(speciesId, i);
    if (candidate == battleMove1) continue;
    if (pick-- == 0) return candidate;
  }
  return MOVE_NONE;
}

bool Pet::learnMove1(uint16_t moveId) {
  if (!canRelearnMove1() || moveId == MOVE_NONE || moveId == battleMove1) return false;
  bool valid = false;
  uint8_t count = commonMoveCountForSpecies(speciesId);
  for (uint8_t i = 0; i < count; i++)
    if (commonMoveForSpecies(speciesId, i) == moveId) { valid = true; break; }
  if (!valid) return false;
  itemCounts[EXP_ITEM_MOVE]--;
  battleMove1 = moveId;
  save();
  return true;
}

bool Pet::keepMove1() {
  if (itemCounts[EXP_ITEM_MOVE] == 0) return false;
  itemCounts[EXP_ITEM_MOVE]--;
  save();
  return true;
}

bool Pet::awardMoveDisc(uint8_t chancePct, uint8_t luckRoll) {
  if (isEgg() || chancePct == 0 || luckRoll >= chancePct ||
      itemCounts[EXP_ITEM_MOVE] >= EXP_ITEM_MAX) return false;
  itemCounts[EXP_ITEM_MOVE]++;
  save();
  return true;
}

void Pet::stopSitter() {
  if (!sitterUntilEpoch) return;
  sitterUntilEpoch = 0;
  save();
}

bool Pet::expireSitterIfNeeded(uint32_t nowEpoch) {
  if (!sitterUntilEpoch || !nowEpoch || nowEpoch < sitterUntilEpoch) return false;
  sitterUntilEpoch = 0;
  save();
  return true;
}

void Pet::play() {
  if (ceremony != CER_NONE) return;
  if (isEgg() || sleeping) return;
  joy = clamp100(joy + 25);
  energy = clamp100(energy - 10);
  fullness = clamp100(fullness - 5);
  heartUntil = millis() + HEART_MS;
  addBond(2);
  registerCare();
  save();
}

void Pet::toggleLight() {
  if (ceremony != CER_NONE) return;
  if (isEgg()) return;
  sleeping = !sleeping;
  if (sleeping) sitterUntilEpoch = 0;
  sleepNeedMinutes = 0;
  save();
}

void Pet::clean() {
  if (ceremony != CER_NONE) return;
  poops = 0;
  hygiene = 100;
  addBond(1);
  registerCare();
  noteDailyGoal(DAILY_GOAL_CARE, 1);
  save();
}

void Pet::caress() {
  if (ceremony != CER_NONE) return;
  if (isEgg() || sleeping) return;
  joy = clamp100(joy + 5);
  heartUntil = millis() + HEART_MS;
  addBond(1);
  registerCare();
  noteDailyGoal(DAILY_GOAL_CARE, 1);
  save();
}

void Pet::eggTap() {
  if (!isEgg()) return;
  if (++eggTaps >= 3) hatch();
  else save();
}

PetMood Pet::mood() const {
  if (sleeping) return MOOD_SLEEPING;
  if (eating()) return MOOD_EATING;
  if (lowestStat() < 25) return MOOD_SAD;
  return MOOD_HAPPY;
}

void Pet::save() {
  ticksSinceSave = 0;
  pendingSave = false;
  prefs.putUChar("full", fullness);
  prefs.putUChar("joy", joy);
  prefs.putUChar("ene", energy);
  prefs.putUChar("hyg", hygiene);
  prefs.putUChar("poop", poops);
  prefs.putUChar("wgt", weight);
  prefs.putUChar("gatk", geneAtk);
  prefs.putUChar("gdef", geneDef);
  prefs.putUChar("gspe", geneSpe);
  prefs.putUChar("tatk", trAtk);
  prefs.putUChar("tdef", trDef);
  prefs.putUChar("tspe", trSpe);
  prefs.putBool("bk", berryKnown);
  prefs.putBool("shy", shiny);
  prefs.putBool("eshy", eggShiny);
  prefs.putBool("stpk", starterPick);
  prefs.putBytes("dexsh", dexShinyReg, sizeof(dexShinyReg));
  prefs.putUInt("age", ageMinutes);
  prefs.putShort("dexn", speciesId);
  prefs.putShort("eggT2", eggTarget);
  prefs.putUChar("crack", eggTaps);
  prefs.putUChar("mist", careMistakes);
  prefs.putBool("tamamode", tamagotchiMode);
  prefs.putUChar("cneed", (uint8_t)careNeed);
  prefs.putBool("cmissed", careMissed);
  prefs.putUInt("cdue", careDueEpoch);
  prefs.putBool("sleep", sleeping);
  prefs.putUChar("slneed", sleepNeedMinutes);
  prefs.putUChar("lend", lastEnd);
  if (lastSeenEpoch) {
    prefs.putUInt("seen", lastSeenEpoch);
    // 별도 검증값은 기존 게임 저장과 같은 트랜잭션에서만 갱신한다.
    // 부팅 시 RTC 판정이 끝나기 전에는 save()를 부르지 않아 이전의
    // 신뢰 가능한 기준시각이 보존된다.
    prefs.putUInt("rtcguard", lastSeenEpoch);
    rtcGuardEpoch = lastSeenEpoch;
  }
  prefs.putBytes("dexreg", dexReg, sizeof(dexReg));
  prefs.putBytes("dexcgt", dexCaught, sizeof(dexCaught));
  prefs.putBytes("dexlnk", dexLinked, sizeof(dexLinked));
  prefs.putUShort("strk", streak);
  prefs.putUShort("bstrk", bestStreak);
  prefs.putUInt("cday", lastCareDay);
  prefs.putUChar("bond", bond);
  prefs.putUShort("medal", medals);
  prefs.putUShort("tmedal", totalMedals);
  prefs.putUShort("mstone", lastMilestone);
  prefs.putUShort("ghi", gameHi);
  prefs.putUShort("chi", catchHi);
  prefs.putUShort("mhi", memoHi);
  prefs.putUShort("dghi", diglettHi);
  prefs.putUShort("tyhi", typeHi);
  prefs.putUShort("bwin", battleWins);
  prefs.putUShort("bloss", battleLosses);
  prefs.putUShort("bstk", battleStreak);
  prefs.putUShort("bbstk", bestBattleStreak);
  prefs.putUChar("cfrm", collectionFrame);
  prefs.putUInt("pimin", lastPetInteractMinute);
  prefs.putUChar("dxrew", dexRewardMask);
  prefs.putUInt("dgday", dailyGoalDay);
  prefs.putBytes("dgtype", dailyGoalType, sizeof(dailyGoalType));
  prefs.putBytes("dgprog", dailyGoalProgress, sizeof(dailyGoalProgress));
  prefs.putUChar("dgdone", dailyGoalDone);
  prefs.putBytes("items", itemCounts, sizeof(itemCounts));
  prefs.putUShort("bmove1", battleMove1);
  prefs.putUInt("wdiscday", walkDiscDay);
  prefs.putUInt("situntil", sitterUntilEpoch);
  prefs.putUInt("evoday", evoDeclinedDay);
  prefs.putUInt("farday", farDeclinedDay);
  prefs.putUInt("finage", finalFormAgeMinutes);
  prefs.putString("nick", nick);
}

void Pet::load() {
  fullness = prefs.getUChar("full", 80);
  joy = prefs.getUChar("joy", 80);
  energy = prefs.getUChar("ene", 80);
  hygiene = prefs.getUChar("hyg", 100);
  poops = prefs.getUChar("poop", 0);
  weight = prefs.getUChar("wgt", 0);
  geneAtk = prefs.getUChar("gatk", 0);
  geneDef = prefs.getUChar("gdef", 0);
  geneSpe = prefs.getUChar("gspe", 0);
  if (geneAtk == 0) {  // mascota anterior a los genes: tirada unica ahora
    geneAtk = 90 + random(21);
    geneDef = 90 + random(21);
    geneSpe = 90 + random(21);
  }
  trAtk = prefs.getUChar("tatk", 0);
  trDef = prefs.getUChar("tdef", 0);
  trSpe = prefs.getUChar("tspe", 0);
  berryKnown = prefs.getBool("bk", false);
  shiny = prefs.getBool("shy", false);
  eggShiny = prefs.getBool("eshy", false);
  starterPick = prefs.getBool("stpk", false);
  prefs.getBytes("dexsh", dexShinyReg, sizeof(dexShinyReg));
  ageMinutes = prefs.getUInt("age", 0);
  rtcGuardEpoch = prefs.getUInt("rtcguard", prefs.getUInt("seen", 0));
  if (prefs.isKey("dexn")) {
    speciesId = prefs.getShort("dexn", -1);
    eggTarget = prefs.getShort("eggT2", 4);
  } else {
    // migracion desde la version con indices de flash (0-8)
    static const uint8_t OLD2DEX[9] = { 4, 5, 6, 1, 2, 3, 7, 8, 9 };
    int8_t old = prefs.getChar("spec", -1);
    speciesId = (old >= 0 && old < 9) ? OLD2DEX[old] : -1;
    int8_t oldT = prefs.getChar("eggT", 0);
    eggTarget = (oldT >= 0 && oldT < 9) ? OLD2DEX[oldT] : 4;
  }
  eggTaps = prefs.getUChar("crack", 0);
  careMistakes = prefs.getUChar("mist", 0);
  tamagotchiMode = prefs.getBool("tamamode", true);
  uint8_t savedCareNeed = prefs.getUChar("cneed", CARE_NEED_NONE);
  careNeed = savedCareNeed <= CARE_NEED_JOY ? (CareNeed)savedCareNeed : CARE_NEED_NONE;
  careMissed = prefs.getBool("cmissed", false);
  careDueEpoch = prefs.getUInt("cdue", 0);
  if (!tamagotchiMode || careNeed == CARE_NEED_NONE) clearCareCall();
  sleeping = prefs.getBool("sleep", false);
  sleepNeedMinutes = prefs.getUChar("slneed", 0);
  if (sleepNeedMinutes >= SLEEP_NEED_INTERVAL_NIGHT_MIN) sleepNeedMinutes = 0;
  lastEnd = prefs.getUChar("lend", CER_NONE);
  prefs.getBytes("dexreg", dexReg, sizeof(dexReg));
  prefs.getBytes("dexcgt", dexCaught, sizeof(dexCaught));
  if (prefs.getBytes("dexlnk", dexLinked, sizeof(dexLinked)) != sizeof(dexLinked))
    memset(dexLinked, 0, sizeof(dexLinked));
  streak = prefs.getUShort("strk", 0);
  bestStreak = prefs.getUShort("bstrk", 0);
  lastCareDay = prefs.getUInt("cday", 0);
  bond = prefs.getUChar("bond", 0);
  medals = prefs.getUShort("medal", 0);
  totalMedals = prefs.getUShort("tmedal", 0);
  lastMilestone = prefs.getUShort("mstone", 0);
  gameHi = prefs.getUShort("ghi", 0);
  catchHi = prefs.getUShort("chi", 0);
  if (catchHi > 100) catchHi = 100;
  memoHi = prefs.getUShort("mhi", 0);
  diglettHi = prefs.getUShort("dghi", 0);
  typeHi = prefs.getUShort("tyhi", 0);
  battleWins = prefs.getUShort("bwin", 0);
  battleLosses = prefs.getUShort("bloss", 0);
  battleStreak = prefs.getUShort("bstk", 0);
  bestBattleStreak = prefs.getUShort("bbstk", 0);
  collectionFrame = prefs.getUChar("cfrm", 0);
  if (collectionFrame >= unlockedCollectionFrameCount()) collectionFrame = 0;
  lastPetInteractMinute = prefs.getUInt("pimin", 0);
  dexRewardMask = prefs.getUChar("dxrew", 0);
  dailyGoalDay = prefs.getUInt("dgday", 0);
  size_t gotTypes = prefs.getBytes("dgtype", dailyGoalType, sizeof(dailyGoalType));
  size_t gotProg = prefs.getBytes("dgprog", dailyGoalProgress, sizeof(dailyGoalProgress));
  if (gotTypes != sizeof(dailyGoalType)) {
    dailyGoalType[0] = DAILY_GOAL_CARE;
    dailyGoalType[1] = DAILY_GOAL_PLAY;
    dailyGoalType[2] = DAILY_GOAL_BATTLE;
  }
  if (gotProg != sizeof(dailyGoalProgress)) {
    dailyGoalProgress[0] = dailyGoalProgress[1] = dailyGoalProgress[2] = 0;
  }
  dailyGoalDone = prefs.getUChar("dgdone", 0);
  size_t gotItems = prefs.getBytes("items", itemCounts, sizeof(itemCounts));
  if (gotItems < 4) memset(itemCounts, 0, sizeof(itemCounts));
  else if (gotItems < sizeof(itemCounts))
    memset(itemCounts + gotItems, 0, sizeof(itemCounts) - gotItems);
  for (uint8_t i = 0; i < EXP_ITEM_COUNT; i++)
    if (itemCounts[i] > EXP_ITEM_MAX) itemCounts[i] = EXP_ITEM_MAX;
  battleMove1 = prefs.getUShort("bmove1", MOVE_NONE);
  walkDiscDay = prefs.getUInt("wdiscday", 0);
  if (speciesId >= 1 && battleMove1 == MOVE_NONE)
    battleMove1 = chooseCommonMoveForSpecies(
        speciesId, (uint32_t)speciesId * 65537UL + geneAtk * 257UL + geneSpe);
  sitterUntilEpoch = prefs.getUInt("situntil", 0);
  // A moon-sleep save and an active sitter are mutually exclusive. Clear
  // inconsistent data left by older RTC rollback behavior before the UI can
  // briefly show Chansey over a sleeping Pokemon.
  if (sleeping) sitterUntilEpoch = 0;
  evoDeclinedDay = prefs.getUInt("evoday", 0);
  farDeclinedDay = prefs.getUInt("farday", 0);
  if (prefs.isKey("finage")) {
    finalFormAgeMinutes = prefs.getUInt("finage", 0);
  } else if (speciesId >= 1 && DEX_TBL[speciesId].evolvesTo == 0) {
    // Migracion: los finales nacidos de huevo cuentan desde el nacimiento;
    // una forma final evolucionada en un guardado antiguo espera 24 h seguras.
    finalFormAgeMinutes = DEX_TBL[speciesId].rarity == R_EVO ? ageMinutes : 0;
  } else {
    finalFormAgeMinutes = 0;
  }
  prefs.getString("nick", nick, sizeof(nick));
  // siembra: la mascota actual cuenta como criada (guardados antiguos)
  if (speciesId >= 1) registerSpecies(speciesId);
}

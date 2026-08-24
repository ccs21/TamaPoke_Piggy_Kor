#include "battle.h"

#include "dex.h"
#include "moves.h"

namespace {
constexpr uint8_t MAX_BATTLE_ROUNDS = 50;
constexpr uint8_t MAX_TURN_ROUNDS = 20;
constexpr uint8_t SKILL1_MAX_USES = 3;
constexpr uint8_t SKILL2_MAX_USES = 2;
constexpr uint8_t RECOVER_MAX_USES = 2;

uint8_t clampRoll(uint8_t roll) { return roll > 99 ? 99 : roll; }

uint16_t hpFor(const BattleStats &stats) {
  if (stats.hp > 0) return stats.hp;
  uint8_t level = stats.level ? stats.level : 1;
  uint32_t scaled = 30 + (uint32_t)level * 5 + stats.def;
  return scaled > 65535 ? 65535 : (uint16_t)scaled;
}

int8_t typeRelation(uint8_t attackType, uint8_t defendType) {
  if (attackType == TYPE_NONE || defendType == TYPE_NONE) return 0;
  switch (attackType) {
    case TYPE_NORMAL:
      if (defendType == TYPE_ROCK || defendType == TYPE_STEEL) return -1;
      if (defendType == TYPE_GHOST) return -2;
      return 0;
    case TYPE_FIRE:
      if (defendType == TYPE_BUG || defendType == TYPE_STEEL || defendType == TYPE_GRASS || defendType == TYPE_ICE) return 1;
      if (defendType == TYPE_ROCK || defendType == TYPE_FIRE || defendType == TYPE_WATER || defendType == TYPE_DRAGON) return -1;
      return 0;
    case TYPE_WATER:
      if (defendType == TYPE_GROUND || defendType == TYPE_ROCK || defendType == TYPE_FIRE) return 1;
      if (defendType == TYPE_WATER || defendType == TYPE_GRASS || defendType == TYPE_DRAGON) return -1;
      return 0;
    case TYPE_ELECTRIC:
      if (defendType == TYPE_FLYING || defendType == TYPE_WATER) return 1;
      if (defendType == TYPE_GRASS || defendType == TYPE_ELECTRIC || defendType == TYPE_DRAGON) return -1;
      if (defendType == TYPE_GROUND) return -2;
      return 0;
    case TYPE_GRASS:
      if (defendType == TYPE_GROUND || defendType == TYPE_ROCK || defendType == TYPE_WATER) return 1;
      if (defendType == TYPE_FLYING || defendType == TYPE_POISON || defendType == TYPE_BUG ||
          defendType == TYPE_STEEL || defendType == TYPE_FIRE || defendType == TYPE_GRASS ||
          defendType == TYPE_DRAGON) return -1;
      return 0;
    case TYPE_ICE:
      if (defendType == TYPE_FLYING || defendType == TYPE_GROUND || defendType == TYPE_GRASS || defendType == TYPE_DRAGON) return 1;
      if (defendType == TYPE_STEEL || defendType == TYPE_FIRE || defendType == TYPE_WATER || defendType == TYPE_ICE) return -1;
      return 0;
    case TYPE_FIGHTING:
      if (defendType == TYPE_NORMAL || defendType == TYPE_ROCK || defendType == TYPE_STEEL ||
          defendType == TYPE_ICE || defendType == TYPE_DARK) return 1;
      if (defendType == TYPE_FLYING || defendType == TYPE_POISON || defendType == TYPE_BUG ||
          defendType == TYPE_PSYCHIC || defendType == TYPE_FAIRY) return -1;
      if (defendType == TYPE_GHOST) return -2;
      return 0;
    case TYPE_POISON:
      if (defendType == TYPE_GRASS || defendType == TYPE_FAIRY) return 1;
      if (defendType == TYPE_POISON || defendType == TYPE_GROUND || defendType == TYPE_ROCK || defendType == TYPE_GHOST) return -1;
      if (defendType == TYPE_STEEL) return -2;
      return 0;
    case TYPE_GROUND:
      if (defendType == TYPE_POISON || defendType == TYPE_ROCK || defendType == TYPE_STEEL ||
          defendType == TYPE_FIRE || defendType == TYPE_ELECTRIC) return 1;
      if (defendType == TYPE_BUG || defendType == TYPE_GRASS) return -1;
      if (defendType == TYPE_FLYING) return -2;
      return 0;
    case TYPE_FLYING:
      if (defendType == TYPE_FIGHTING || defendType == TYPE_BUG || defendType == TYPE_GRASS) return 1;
      if (defendType == TYPE_ROCK || defendType == TYPE_STEEL || defendType == TYPE_ELECTRIC) return -1;
      return 0;
    case TYPE_PSYCHIC:
      if (defendType == TYPE_FIGHTING || defendType == TYPE_POISON) return 1;
      if (defendType == TYPE_STEEL || defendType == TYPE_PSYCHIC) return -1;
      if (defendType == TYPE_DARK) return -2;
      return 0;
    case TYPE_BUG:
      if (defendType == TYPE_GRASS || defendType == TYPE_PSYCHIC || defendType == TYPE_DARK) return 1;
      if (defendType == TYPE_FIGHTING || defendType == TYPE_FLYING || defendType == TYPE_POISON ||
          defendType == TYPE_GHOST || defendType == TYPE_STEEL || defendType == TYPE_FIRE ||
          defendType == TYPE_FAIRY) return -1;
      return 0;
    case TYPE_ROCK:
      if (defendType == TYPE_FLYING || defendType == TYPE_BUG || defendType == TYPE_FIRE || defendType == TYPE_ICE) return 1;
      if (defendType == TYPE_FIGHTING || defendType == TYPE_GROUND || defendType == TYPE_STEEL) return -1;
      return 0;
    case TYPE_GHOST:
      if (defendType == TYPE_GHOST || defendType == TYPE_PSYCHIC) return 1;
      if (defendType == TYPE_DARK) return -1;
      if (defendType == TYPE_NORMAL) return -2;
      return 0;
    case TYPE_DRAGON:
      if (defendType == TYPE_DRAGON) return 1;
      if (defendType == TYPE_STEEL) return -1;
      if (defendType == TYPE_FAIRY) return -2;
      return 0;
    case TYPE_DARK:
      if (defendType == TYPE_GHOST || defendType == TYPE_PSYCHIC) return 1;
      if (defendType == TYPE_FIGHTING || defendType == TYPE_DARK || defendType == TYPE_FAIRY) return -1;
      return 0;
    case TYPE_STEEL:
      if (defendType == TYPE_ROCK || defendType == TYPE_ICE || defendType == TYPE_FAIRY) return 1;
      if (defendType == TYPE_STEEL || defendType == TYPE_FIRE || defendType == TYPE_WATER || defendType == TYPE_ELECTRIC) return -1;
      return 0;
    case TYPE_FAIRY:
      if (defendType == TYPE_FIGHTING || defendType == TYPE_DRAGON || defendType == TYPE_DARK) return 1;
      if (defendType == TYPE_POISON || defendType == TYPE_STEEL || defendType == TYPE_FIRE) return -1;
      return 0;
  }
  return 0;
}

uint16_t effectPctFor(uint8_t attackType, uint8_t defendType1, uint8_t defendType2) {
  if (attackType == TYPE_NONE) return 100;
  uint16_t pct = 100;
  const uint8_t types[2] = {defendType1, defendType2};
  for (uint8_t i = 0; i < 2; i++) {
    if (types[i] == TYPE_NONE) continue;
    int8_t relation = typeRelation(attackType, types[i]);
    if (relation > 0) pct = (uint16_t)(pct * 2);
    else if (relation == -1) pct = (uint16_t)(pct / 2);
    else if (relation == -2) return 0;
  }
  return pct;
}

bool isAttack(BattleAction action) {
  return action == BATTLE_BASIC || action == BATTLE_SKILL1 || action == BATTLE_SKILL2;
}

uint16_t moveFor(const BattleStats &stats, BattleAction action) {
  if (action == BATTLE_SKILL1) return stats.move1;
  if (action == BATTLE_SKILL2) return stats.move2;
  return MOVE_NONE;
}

uint8_t powerFor(BattleAction action) {
  if (action == BATTLE_SKILL1) return 120;
  if (action == BATTLE_SKILL2) return 135;
  return 100;
}

uint8_t moveTypeFor(const BattleStats &stats, BattleAction action) {
  uint16_t moveId = moveFor(stats, action);
  return moveId == MOVE_NONE ? TYPE_NONE : moveDef(moveId).type;
}

bool hasUses(const BattleRuntime &battle, bool player, BattleAction action) {
  if (action == BATTLE_SKILL1)
    return player ? battle.skill1UsesLeft > 0 : battle.enemySkill1UsesLeft > 0;
  if (action == BATTLE_SKILL2)
    return player ? battle.skill2UsesLeft > 0 : battle.enemySkill2UsesLeft > 0;
  return true;
}

void consumeUse(BattleRuntime &battle, bool player, BattleAction action) {
  if (action == BATTLE_SKILL1) {
    uint8_t &uses = player ? battle.skill1UsesLeft : battle.enemySkill1UsesLeft;
    if (uses) uses--;
  } else if (action == BATTLE_SKILL2) {
    uint8_t &uses = player ? battle.skill2UsesLeft : battle.enemySkill2UsesLeft;
    if (uses) uses--;
  }
}

bool actionAvailable(const BattleRuntime &battle, bool player, BattleAction action) {
  const BattleStats &stats = player ? battle.player : battle.enemy;
  return isAttack(action) && battleActionUnlocked(stats, action) && hasUses(battle, player, action);
}

uint16_t damageFor(const BattleStats &attacker, const BattleStats &defender,
                   uint16_t defenderMaxHp, BattleAction action, uint8_t roll,
                   uint16_t &typePct) {
  int rate = 16 + ((int)attacker.atk - (int)defender.def) / 12;
  if (rate < 11) rate = 11;
  if (rate > 23) rate = 23;
  typePct = effectPctFor(moveTypeFor(attacker, action), defender.type1, defender.type2);
  if (typePct == 0) return 0;
  uint32_t damage = (uint32_t)defenderMaxHp * (uint32_t)rate / 100;
  if (damage < 1) damage = 1;
  damage = damage * powerFor(action) / 100;
  damage = damage * typePct / 100;
  damage = damage * (90 + (clampRoll(roll) % 21)) / 100;
  uint16_t cap = (uint16_t)((uint32_t)defenderMaxHp * 45 / 100);
  if (cap < 1) cap = 1;
  if (damage > cap) damage = cap;
  return damage ? (uint16_t)damage : 1;
}

void applyHit(uint16_t &hp, uint16_t damage, uint16_t &total, uint16_t &reported) {
  reported = damage > hp ? hp : damage;
  hp -= reported;
  total += reported;
}

bool finished(const BattleRuntime &battle) {
  return battle.playerHp == 0 || battle.enemyHp == 0 || battle.round >= MAX_TURN_ROUNDS;
}

bool winner(const BattleRuntime &battle, uint8_t roll) {
  if (battle.enemyHp == 0 && battle.playerHp > 0) return true;
  if (battle.playerHp == 0 && battle.enemyHp > 0) return false;
  if (battle.playerHp != battle.enemyHp) return battle.playerHp > battle.enemyHp;
  if (battle.player.spe != battle.enemy.spe) return battle.player.spe > battle.enemy.spe;
  return clampRoll(roll) >= 50;
}

bool playerFirst(const BattleRuntime &battle, uint8_t roll) {
  if (battle.player.spe != battle.enemy.spe) return battle.player.spe > battle.enemy.spe;
  return clampRoll(roll) >= 50;
}

uint16_t recover(uint16_t &hp, uint16_t maxHp) {
  uint16_t amount = (uint16_t)((uint32_t)maxHp * 28 / 100);
  if (amount < 6) amount = 6;
  uint16_t missing = maxHp - hp;
  if (amount > missing) amount = missing;
  hp += amount;
  return amount;
}

BattleAction chooseEnemyAction(const BattleRuntime &battle, uint8_t roll) {
  BattleAction best = BATTLE_BASIC;
  uint32_t bestScore = 10000;
  const BattleAction skills[2] = {BATTLE_SKILL1, BATTLE_SKILL2};
  for (BattleAction action : skills) {
    if (!actionAvailable(battle, false, action)) continue;
    uint16_t pct = effectPctFor(moveTypeFor(battle.enemy, action), battle.player.type1, battle.player.type2);
    uint32_t score = (uint32_t)powerFor(action) * pct;
    if (score > bestScore) {
      best = action;
      bestScore = score;
    }
  }
  // Wild Pokemon usually use their best move, but occasionally fall back to
  // the unlimited basic attack so every encounter is not scripted identically.
  return best != BATTLE_BASIC && (roll % 100) < 78 ? best : BATTLE_BASIC;
}

void attack(BattleRuntime &battle, bool byPlayer, BattleAction action, uint8_t evadeRoll,
            uint8_t damageRoll, BattleTurnResult &turn) {
  BattleStats &attacker = byPlayer ? battle.player : battle.enemy;
  BattleStats &defender = byPlayer ? battle.enemy : battle.player;
  uint16_t &targetHp = byPlayer ? battle.enemyHp : battle.playerHp;
  uint16_t targetMax = byPlayer ? battle.enemyMaxHp : battle.playerMaxHp;
  uint16_t &total = byPlayer ? battle.playerDamageTotal : battle.enemyDamageTotal;
  uint16_t &reported = byPlayer ? turn.playerDamage : turn.enemyDamage;
  bool dodged = evadeRoll < battleEvasionChance(attacker, defender);
  if (byPlayer) turn.enemyDodged = dodged;
  else turn.playerDodged = dodged;
  if (dodged) return;
  uint16_t typePct = 100;
  uint16_t damage = damageFor(attacker, defender, targetMax, action, damageRoll, typePct);
  if (byPlayer) turn.playerTypePct = typePct;
  else turn.enemyTypePct = typePct;
  applyHit(targetHp, damage, total, reported);
}

}  // namespace

uint16_t battleTypeEffectPct(uint8_t attackType, uint8_t defendType1, uint8_t defendType2) {
  return effectPctFor(attackType, defendType1, defendType2);
}

uint8_t battleEvasionChance(const BattleStats &attacker, const BattleStats &defender) {
  int chance = 10 + ((int)defender.spe - (int)attacker.spe) * 2 / 5;
  if (chance < 5) chance = 5;
  if (chance > 50) chance = 50;
  return (uint8_t)chance;
}

uint8_t battleRunChance(const BattleStats &player, const BattleStats &enemy) {
  int chance = 40 + ((int)player.spe - (int)enemy.spe) / 2;
  if (chance < 25) chance = 25;
  if (chance > 90) chance = 90;
  return (uint8_t)chance;
}

bool battleActionUnlocked(const BattleStats &stats, BattleAction action) {
  if (action == BATTLE_SKILL1) return stats.level >= 10 && stats.move1 != MOVE_NONE;
  if (action == BATTLE_SKILL2) return stats.level >= 20 && stats.move2 != MOVE_NONE;
  return true;
}

bool canStartWildBattle(bool isEgg, bool sleeping, uint8_t ceremony) {
  return !isEgg && !sleeping && ceremony == 0;
}

uint8_t wildLevelFor(uint8_t petLevel, uint8_t luckRoll) {
  int base = petLevel ? petLevel : 1;
  int delta;
  if (luckRoll < 55) delta = (int)(luckRoll % 3) - 1;
  else if (luckRoll < 85) delta = -2 - (int)(luckRoll % 3);
  else delta = 2 + (int)(luckRoll % 2);
  int level = base + delta;
  if (level < 1) level = 1;
  return level > 100 ? 100 : (uint8_t)level;
}

int16_t pickWildSpecies(uint8_t roll) {
  int16_t pool[DEX_COUNT];
  int count = 0;
  uint8_t targetRarity = (roll % 100) < 25 ? R_RARO : R_COMUN;
  for (int16_t dex = 1; dex <= DEX_COUNT; dex++)
    if (DEX_TBL[dex].rarity == targetRarity) pool[count++] = dex;
  if (count == 0 && targetRarity == R_RARO)
    for (int16_t dex = 1; dex <= DEX_COUNT; dex++)
      if (DEX_TBL[dex].rarity == R_COMUN) pool[count++] = dex;
  return count ? pool[roll % count] : 1;
}

BattleStats wildBattleStats(int16_t dex, uint8_t level) {
  if (dex < 1 || dex > DEX_COUNT) dex = 1;
  const DexEntry &entry = DEX_TBL[dex];
  uint8_t lvl = level ? level : 1;
  BattleStats stats = {};
  stats.atk = entry.bAtk + lvl;
  stats.def = entry.bDef + lvl;
  stats.spe = entry.bSpe + lvl;
  stats.level = lvl;
  stats.type1 = entry.type1;
  stats.type2 = entry.type2;
  stats.move1 = chooseCommonMoveForSpecies(dex, (uint32_t)dex * 131UL + lvl * 17UL);
  stats.move2 = signatureMoveForSpecies(dex);
  return stats;
}

BattleRuntime beginBattleRuntime(const BattleStats &player, const BattleStats &enemy) {
  BattleRuntime battle = {};
  battle.player = player;
  battle.enemy = enemy;
  battle.playerMaxHp = hpFor(player);
  battle.enemyMaxHp = hpFor(enemy);
  battle.playerHp = battle.playerMaxHp;
  battle.enemyHp = battle.enemyMaxHp;
  battle.restUsesLeft = RECOVER_MAX_USES;
  battle.enemyRestUsesLeft = RECOVER_MAX_USES;
  battle.skill1UsesLeft = SKILL1_MAX_USES;
  battle.skill2UsesLeft = SKILL2_MAX_USES;
  battle.enemySkill1UsesLeft = SKILL1_MAX_USES;
  battle.enemySkill2UsesLeft = SKILL2_MAX_USES;
  return battle;
}

BattleTurnResult stepBattle(BattleRuntime &battle, BattleAction action, uint8_t luckRoll) {
  BattleTurnResult turn = {};
  turn.playerTypePct = turn.enemyTypePct = 100;
  turn.enemyAction = BATTLE_BASIC;
  if (finished(battle)) {
    turn.battleEnded = true;
    turn.playerWon = winner(battle, luckRoll);
    return turn;
  }
  const uint8_t luck = clampRoll(luckRoll);

  if (isAttack(action) && !actionAvailable(battle, true, action)) {
    turn.moveUnavailable = true;
    return turn;
  }
  if (action == BATTLE_RECOVER) {
    bool fullyCharged = battle.skill1UsesLeft == SKILL1_MAX_USES && battle.skill2UsesLeft == SKILL2_MAX_USES;
    if (battle.restUsesLeft == 0 || (battle.playerHp == battle.playerMaxHp && fullyCharged)) {
      turn.recoveryFailed = true;
      return turn;
    }
  }

  battle.round++;
  turn.playerMoveId = moveFor(battle.player, action);
  if (action == BATTLE_RUN) {
    turn.enemyAction = chooseEnemyAction(battle, (uint8_t)(luck * 37U + 9U));
    turn.enemyMoveId = moveFor(battle.enemy, turn.enemyAction);
    if (luck < battleRunChance(battle.player, battle.enemy)) {
      turn.playerRan = true;
      turn.battleEnded = true;
      return turn;
    }
    turn.runFailed = true;
    turn.playerActedFirst = false;
    consumeUse(battle, false, turn.enemyAction);
    attack(battle, false, turn.enemyAction, (uint8_t)((luck * 53U + 17U) % 100U),
           (uint8_t)((luck * 29U + 41U) % 100U), turn);
    turn.enemyActed = true;
  } else if (action == BATTLE_RECOVER) {
    turn.playerRecovered = true;
    battle.restUsesLeft--;
    turn.playerHeal = recover(battle.playerHp, battle.playerMaxHp);
    battle.skill1UsesLeft = SKILL1_MAX_USES;
    battle.skill2UsesLeft = SKILL2_MAX_USES;
    turn.enemyAction = chooseEnemyAction(battle, (uint8_t)(luck * 37U + 9U));
    turn.enemyMoveId = moveFor(battle.enemy, turn.enemyAction);
    consumeUse(battle, false, turn.enemyAction);
    attack(battle, false, turn.enemyAction, (uint8_t)((luck * 53U + 17U) % 100U),
           (uint8_t)((luck * 29U + 41U) % 100U), turn);
    turn.enemyActed = true;
  } else {
    turn.enemyAction = chooseEnemyAction(battle, (uint8_t)(luck * 37U + 9U));
    turn.enemyMoveId = moveFor(battle.enemy, turn.enemyAction);
    turn.playerActedFirst = playerFirst(battle, luck);
    consumeUse(battle, true, action);
    consumeUse(battle, false, turn.enemyAction);
    if (turn.playerActedFirst) {
      attack(battle, true, action, (uint8_t)((luck * 31U + 7U) % 100U),
             (uint8_t)((luck * 47U + 13U) % 100U), turn);
      if (battle.enemyHp > 0) {
        attack(battle, false, turn.enemyAction, (uint8_t)((luck * 53U + 17U) % 100U),
               (uint8_t)((luck * 29U + 41U) % 100U), turn);
        turn.enemyActed = true;
      }
    } else {
      attack(battle, false, turn.enemyAction, (uint8_t)((luck * 53U + 17U) % 100U),
             (uint8_t)((luck * 29U + 41U) % 100U), turn);
      turn.enemyActed = true;
      if (battle.playerHp > 0)
        attack(battle, true, action, (uint8_t)((luck * 31U + 7U) % 100U),
               (uint8_t)((luck * 47U + 13U) % 100U), turn);
    }
  }

  turn.battleEnded = finished(battle);
  turn.playerWon = turn.battleEnded ? winner(battle, luck) : false;
  return turn;
}

LinkBattleTurnResult stepLinkBattle(BattleRuntime &battle, BattleAction hostAction,
                                    BattleAction guestAction, uint8_t luckRoll) {
  LinkBattleTurnResult out = {};
  BattleTurnResult &host = out.host;
  BattleTurnResult &guest = out.guest;
  host.playerTypePct = host.enemyTypePct = guest.playerTypePct = guest.enemyTypePct = 100;
  host.enemyAction = guestAction;
  guest.enemyAction = hostAction;
  if (finished(battle)) {
    bool hostWon = winner(battle, luckRoll);
    host.battleEnded = guest.battleEnded = true;
    host.playerWon = hostWon;
    guest.playerWon = !hostWon;
    return out;
  }

  if (hostAction == BATTLE_RUN || guestAction == BATTLE_RUN) {
    battle.round++;
    bool hostWon = guestAction == BATTLE_RUN && hostAction != BATTLE_RUN;
    if (hostAction == BATTLE_RUN && guestAction == BATTLE_RUN) hostWon = clampRoll(luckRoll) >= 50;
    host.playerForfeited = hostAction == BATTLE_RUN;
    guest.playerForfeited = guestAction == BATTLE_RUN;
    host.battleEnded = guest.battleEnded = true;
    host.playerWon = hostWon;
    guest.playerWon = !hostWon;
    return out;
  }

  if (isAttack(hostAction) && !actionAvailable(battle, true, hostAction)) {
    host.moveUnavailable = true;
    hostAction = BATTLE_BASIC;
  }
  if (isAttack(guestAction) && !actionAvailable(battle, false, guestAction)) {
    guest.moveUnavailable = true;
    guestAction = BATTLE_BASIC;
  }
  bool hostFullyCharged = battle.skill1UsesLeft == SKILL1_MAX_USES && battle.skill2UsesLeft == SKILL2_MAX_USES;
  bool guestFullyCharged = battle.enemySkill1UsesLeft == SKILL1_MAX_USES && battle.enemySkill2UsesLeft == SKILL2_MAX_USES;
  if (hostAction == BATTLE_RECOVER && (battle.restUsesLeft == 0 || (battle.playerHp == battle.playerMaxHp && hostFullyCharged))) {
    host.recoveryFailed = true;
    hostAction = BATTLE_BASIC;
  }
  if (guestAction == BATTLE_RECOVER && (battle.enemyRestUsesLeft == 0 || (battle.enemyHp == battle.enemyMaxHp && guestFullyCharged))) {
    guest.recoveryFailed = true;
    guestAction = BATTLE_BASIC;
  }

  battle.round++;
  const uint8_t luck = clampRoll(luckRoll);
  host.enemyAction = guestAction;
  guest.enemyAction = hostAction;
  host.playerMoveId = moveFor(battle.player, hostAction);
  host.enemyMoveId = moveFor(battle.enemy, guestAction);
  guest.playerMoveId = host.enemyMoveId;
  guest.enemyMoveId = host.playerMoveId;

  if (hostAction == BATTLE_RECOVER) {
    host.playerRecovered = true;
    battle.restUsesLeft--;
    host.playerHeal = recover(battle.playerHp, battle.playerMaxHp);
    battle.skill1UsesLeft = SKILL1_MAX_USES;
    battle.skill2UsesLeft = SKILL2_MAX_USES;
  }
  if (guestAction == BATTLE_RECOVER) {
    guest.playerRecovered = true;
    battle.enemyRestUsesLeft--;
    guest.playerHeal = recover(battle.enemyHp, battle.enemyMaxHp);
    battle.enemySkill1UsesLeft = SKILL1_MAX_USES;
    battle.enemySkill2UsesLeft = SKILL2_MAX_USES;
  }

  if (isAttack(hostAction)) consumeUse(battle, true, hostAction);
  if (isAttack(guestAction)) consumeUse(battle, false, guestAction);
  bool hostFirst = playerFirst(battle, luck);
  host.playerActedFirst = hostFirst;
  guest.playerActedFirst = !hostFirst;

  auto hostAttack = [&]() {
    attack(battle, true, hostAction, (uint8_t)((luck * 31U + 7U) % 100U),
           (uint8_t)((luck * 47U + 13U) % 100U), host);
  };
  auto guestAttack = [&]() {
    BattleTurnResult mirror = {};
    mirror.playerTypePct = mirror.enemyTypePct = 100;
    attack(battle, false, guestAction, (uint8_t)((luck * 53U + 17U) % 100U),
           (uint8_t)((luck * 29U + 41U) % 100U), mirror);
    host.enemyDamage = mirror.enemyDamage;
    host.playerDodged = mirror.playerDodged;
    host.enemyTypePct = mirror.enemyTypePct;
    host.enemyActed = true;
  };

  if (isAttack(hostAction) && isAttack(guestAction)) {
    if (hostFirst) {
      hostAttack();
      if (battle.enemyHp > 0) guestAttack();
    } else {
      guestAttack();
      if (battle.playerHp > 0) hostAttack();
    }
  } else if (isAttack(hostAction)) {
    hostAttack();
  } else if (isAttack(guestAction)) {
    guestAttack();
  }

  guest.playerDamage = host.enemyDamage;
  guest.enemyDamage = host.playerDamage;
  guest.enemyDodged = host.playerDodged;
  guest.playerDodged = host.enemyDodged;
  guest.playerTypePct = host.enemyTypePct;
  guest.enemyTypePct = host.playerTypePct;
  guest.enemyActed = isAttack(hostAction) && battle.playerHp > 0;

  bool ended = finished(battle);
  bool hostWon = ended ? winner(battle, luck) : false;
  host.battleEnded = guest.battleEnded = ended;
  host.playerWon = hostWon;
  guest.playerWon = ended ? !hostWon : false;
  return out;
}

BattleResult resolveBattle(const BattleStats &player, const BattleStats &enemy,
                           const BattleOptions &options) {
  BattleRuntime runtime = beginBattleRuntime(player, enemy);
  BattleTurnResult turn = {};
  uint8_t round = 0;
  while (!turn.battleEnded && round++ < MAX_BATTLE_ROUNDS)
    turn = stepBattle(runtime, BATTLE_BASIC, (uint8_t)((options.luckRoll + round * 17U) % 100U));
  BattleResult result = {};
  result.playerWon = turn.playerWon;
  result.rounds = runtime.round;
  result.playerDamage = runtime.playerDamageTotal;
  result.enemyDamage = runtime.enemyDamageTotal;
  result.playerHpLeft = runtime.playerHp;
  result.enemyHpLeft = runtime.enemyHp;
  return result;
}

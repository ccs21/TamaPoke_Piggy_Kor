#pragma once

#include <stdint.h>

struct BattleStats {
  uint16_t hp;
  uint16_t atk;
  uint16_t def;
  uint16_t spe;
  uint8_t level;
  uint8_t type1 = 0;
  uint8_t type2 = 0;
  uint16_t move1 = 0;
  uint16_t move2 = 0;
};

struct BattleOptions {
  // 0..99. Keeps ties and damage variation deterministic in tests.
  uint8_t luckRoll;
};

struct BattleResult {
  bool playerWon;
  uint8_t rounds;
  uint16_t playerDamage;
  uint16_t enemyDamage;
  uint16_t playerHpLeft;
  uint16_t enemyHpLeft;
};

enum BattleAction : uint8_t {
  BATTLE_BASIC = 0,
  BATTLE_SKILL1,
  BATTLE_SKILL2,
  BATTLE_RECOVER,
  BATTLE_RUN,
};

// Presentation stages are shared by the firmware and the Windows simulator.
// Battle calculation still happens atomically in stepBattle(); these stages
// reveal that result in the same order a player sees the attacks land.
enum BattleAnimationStage : uint8_t {
  BATTLE_ANIM_NONE = 0,
  BATTLE_ANIM_PLAYER_ATTACK,
  BATTLE_ANIM_ENEMY_REACT,
  BATTLE_ANIM_PLAYER_REST,
  BATTLE_ANIM_ENEMY_ATTACK,
  BATTLE_ANIM_PLAYER_REACT,
  BATTLE_ANIM_STATUS,
};

struct BattleRuntime {
  BattleStats player;
  BattleStats enemy;
  uint16_t playerHp;
  uint16_t enemyHp;
  uint16_t playerMaxHp;
  uint16_t enemyMaxHp;
  uint8_t round;
  uint16_t playerDamageTotal;
  uint16_t enemyDamageTotal;
  uint8_t restUsesLeft;
  uint8_t enemyRestUsesLeft;
  uint8_t skill1UsesLeft;
  uint8_t skill2UsesLeft;
  uint8_t enemySkill1UsesLeft;
  uint8_t enemySkill2UsesLeft;
};

struct BattleTurnResult {
  uint16_t playerDamage;
  uint16_t enemyDamage;
  uint16_t playerHeal;
  bool playerDodged;
  bool enemyDodged;
  bool playerRecovered;
  bool recoveryFailed;
  bool moveUnavailable;
  bool playerRan;
  bool runFailed;
  bool playerForfeited;
  bool playerActedFirst;
  bool enemyActed;
  bool battleEnded;
  bool playerWon;
  uint16_t playerTypePct;
  uint16_t enemyTypePct;
  uint16_t playerMoveId;
  uint16_t enemyMoveId;
  BattleAction enemyAction;
};

// A link battle resolves both players' choices at the same time.  The host
// owns the authoritative runtime and sends both mirrored views to the guest,
// so the two devices can never disagree about HP or the winner.
struct LinkBattleTurnResult {
  BattleTurnResult host;
  BattleTurnResult guest;
};

bool canStartWildBattle(bool isEgg, bool sleeping, uint8_t ceremony);
uint8_t wildLevelFor(uint8_t petLevel, uint8_t luckRoll);
int16_t pickWildSpecies(uint8_t roll);
BattleStats wildBattleStats(int16_t dex, uint8_t level);
uint16_t battleTypeEffectPct(uint8_t attackType, uint8_t defendType1, uint8_t defendType2);
uint8_t battleEvasionChance(const BattleStats &attacker, const BattleStats &defender);
uint8_t battleRunChance(const BattleStats &player, const BattleStats &enemy);
bool battleActionUnlocked(const BattleStats &stats, BattleAction action);
BattleRuntime beginBattleRuntime(const BattleStats &player, const BattleStats &enemy);
BattleTurnResult stepBattle(BattleRuntime &battle, BattleAction action, uint8_t luckRoll);
LinkBattleTurnResult stepLinkBattle(BattleRuntime &battle,
                                    BattleAction hostAction,
                                    BattleAction guestAction,
                                    uint8_t luckRoll);

BattleResult resolveBattle(const BattleStats &player,
                           const BattleStats &enemy,
                           const BattleOptions &options);

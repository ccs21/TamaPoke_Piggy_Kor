#pragma once
#include <stdint.h>

#include "dex.h"

enum MoveEffect : uint8_t {
  MOVE_FX_IMPACT = 0,
  MOVE_FX_NORMAL, MOVE_FX_FIRE, MOVE_FX_WATER, MOVE_FX_ELECTRIC,
  MOVE_FX_GRASS, MOVE_FX_ICE, MOVE_FX_FIGHTING, MOVE_FX_POISON,
  MOVE_FX_GROUND, MOVE_FX_FLYING, MOVE_FX_PSYCHIC, MOVE_FX_BUG,
  MOVE_FX_ROCK, MOVE_FX_GHOST, MOVE_FX_DRAGON, MOVE_FX_DARK,
  MOVE_FX_STEEL, MOVE_FX_FAIRY, MOVE_FX_SPEED, MOVE_FX_SLASH,
};

struct MoveDef {
  const char *name;
  uint8_t type;
  uint8_t effect;
};

constexpr uint16_t MOVE_NONE = 0;
const MoveDef &moveDef(uint16_t moveId);
uint16_t signatureMoveForSpecies(int16_t dex);
uint8_t commonMoveCountForSpecies(int16_t dex);
uint16_t commonMoveForSpecies(int16_t dex, uint8_t index);
uint16_t chooseCommonMoveForSpecies(int16_t dex, uint32_t seed);

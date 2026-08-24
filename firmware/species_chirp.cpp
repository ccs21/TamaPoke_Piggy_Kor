#include "species_chirp.h"

#include "dex.h"

static uint16_t clampPitch(int value) {
  if (value < 140) return 140;
  if (value > 2600) return 2600;
  return (uint16_t)value;
}

static int typePitchOffset(uint8_t type) {
  switch (type) {
    case TYPE_ELECTRIC: return 210;
    case TYPE_FIRE: return 105;
    case TYPE_FLYING: return 150;
    case TYPE_ICE: return 170;
    case TYPE_PSYCHIC: return 125;
    case TYPE_WATER: return -35;
    case TYPE_GRASS: return -65;
    case TYPE_BUG: return -85;
    case TYPE_ROCK: return -200;
    case TYPE_GROUND: return -170;
    case TYPE_STEEL: return -130;
    case TYPE_GHOST: return -110;
    default: return 0;
  }
}

bool speciesChirpProfile(int16_t dex, SpeciesChirpProfile *out) {
  if (!out || dex < 1 || dex > DEX_COUNT) return false;

  const DexEntry &entry = DEX_TBL[dex];
  const uint8_t seed = (uint8_t)(dex * 53 + entry.bAtk * 3 + entry.bSpe);
  const int base = 430 + entry.bSpe * 5 + typePitchOffset(entry.type1);
  const int direction = (seed & 0x80) ? 1 : -1;
  const int span = 70 + (seed & 0x5F);

  out->count = 3;
  out->notes[0] = {
    clampPitch(base + (int)(seed & 0x1F) * 5),
    (uint16_t)(42 + (seed & 0x1F)),
    (int16_t)(direction * span),
    (uint8_t)(52 + ((seed >> 2) & 0x1F)),
    (uint8_t)(seed & 0x03),
  };
  out->notes[1] = {
    clampPitch(base + direction * (35 + ((seed >> 3) & 0x7F))),
    (uint16_t)(34 + ((seed >> 5) & 0x1F)),
    (int16_t)(-direction * (span / 2)),
    (uint8_t)(46 + ((seed >> 1) & 0x27)),
    (uint8_t)((seed >> 3) & 0x03),
  };
  // Die dritte Frequenz enthaelt bewusst die Dexnummer direkt. Dadurch hat
  // jede der 151 Spezies ein eindeutig anderes, eigenes Klangprofil.
  out->notes[2] = {
    (uint16_t)(220 + dex * 11),
    (uint16_t)(48 + ((seed >> 4) & 0x1F)),
    (int16_t)(direction * (30 + entry.bSpe)),
    (uint8_t)(48 + ((seed >> 4) & 0x1F)),
    (uint8_t)((seed + entry.type2) & 0x03),
  };
  return true;
}

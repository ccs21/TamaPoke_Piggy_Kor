#pragma once
#include <stdint.h>

// GENERADO por tools/sprites.py - edita alli y ejecuta:
//   python3 tools/sprites.py emit

#define SPRITE_W 32
#define SPRITE_H 32

#define UI_BG_DAY 0xF77C  // #f2efe1
#define UI_BG_NIGHT 0x10C5  // #141828
#define UI_INK 0x0000  // #000000 -- high-contrast daytime text and outlines
#define UI_INK_NIGHT 0xDEFE  // #d8dcf0
#define UI_TRACK 0x632D  // #62666f -- dark neutral gray for secondary/disabled UI
#define UI_BAR_OK 0x5DCD  // #58b868
#define UI_BAR_WARN 0xED07  // #e8a23c
#define UI_BAR_BAD 0xEA87  // #e8503a
#define UI_WHITE 0xFFFF  // #ffffff

static inline uint16_t spriteColor(char ch) {
  switch (ch) {
    case 'k': return 0x18C4;  // #1b1b25
    case 'w': return 0xFFFF;  // #ffffff
    case 'y': return 0xFED2;  // #f8d990
    case 'Y': return 0xE5CC;  // #e0b860
    case 'o': return 0xF427;  // #f5863d
    case 'O': return 0xD2E5;  // #d65f28
    case 'r': return 0xEA87;  // #e8503a
    case 'R': return 0xB184;  // #b53224
    case 'f': return 0xFECB;  // #ffd95e
    case 't': return 0x8EB6;  // #8fd6b4
    case 'T': return 0x5D71;  // #5fae8c
    case 'g': return 0x5DCD;  // #58b868
    case 'G': return 0x3C49;  // #3c8a4c
    case 'd': return 0x3BEC;  // #3f7e62
    case 'p': return 0xF454;  // #f08aa4
    case 'P': return 0xC2F0;  // #c75f80
    case 'b': return 0x7E3D;  // #7cc4ea
    case 'B': return 0x4C98;  // #4f93c4
    case 'N': return 0x3B74;  // #3a6fa0
    case 'M': return 0x2A8F;  // #2a5278
    case 'c': return 0xB3C8;  // #b07a45
    case 'C': return 0x7AA6;  // #7e5530
    case 'l': return 0x9D5C;  // #9aa9e0
    case 'L': return 0x6BF7;  // #6f7cb8
    case 's': return 0xAD97;  // #aab0bc
    case 'S': return 0x7BF1;  // #787e8c
    default: return 0;
  }
}

static const char* const SPR_ICON_FOOD[16] = {  // 16x16
  "................",
  "................",
  "...........k....",
  "........k.kk....",
  "........k.......",
  ".......krk......",
  ".....kkrrrkk....",
  "....krwrrrrrk...",
  "....kwrrrrrrk...",
  "...krrrrrrrrRk..",
  "...krrrrrrrrRk..",
  "....krrrrrrrk...",
  "....krrrrrrRk...",
  ".....kkRRRkk....",
  ".......kkk......",
  "................",
};
static const char* const SPR_ICON_PLAY[16] = {  // 16x16
  "................",
  "................",
  ".......kkk......",
  ".....kkrrrkk....",
  "....krrrrrrrk...",
  "...krrrrrrrrrk..",
  "...krrrrrrrrrk..",
  "..krrrrkkkrrrRk.",
  "..kkkkkkwkkkkkk.",
  "..krrwwkkkwwrRk.",
  "...kwwwwwwwwwk..",
  "...kwwwwwwwwYk..",
  "...kwwwwwwwwYk..",
  "....kwwwwwwYk...",
  ".....kkkkkkk....",
  "................",
};
static const char* const SPR_ICON_LIGHT[16] = {  // 16x16
  "................",
  "................",
  "................",
  "......kk....f...",
  "....kkk.........",
  "....kfk......f..",
  "...kffk.........",
  "...kffk.........",
  "...kfffk........",
  "...kffffk.......",
  "...kfffffkk.kk..",
  "....kffffffkk...",
  "....kkfffffkk...",
  "......kkkkk.....",
  "................",
  "................",
};
static const char* const SPR_ICON_CLEAN[16] = {  // 16x16
  "................",
  "................",
  "................",
  "...kk......kkk..",
  "...kk.....kwbbk.",
  "...kk.....kbbbk.",
  "......kkk..kkk..",
  "....kkbbbkk.....",
  "...kbwbbbbbk....",
  "...kbbwbbbBk....",
  "...kbbbbbbbk....",
  "...kbbbbbbBk....",
  "...kbbbbbbBk....",
  "....kkBBBkk.....",
  "......kkk.......",
  "................",
};

static const char* const SPR_ICON_BERRY_B[16] = {  // 16x16
  "................",
  "................",
  "...........k....",
  "........k.k.....",
  "........k.......",
  ".......kbk......",
  ".....kkbbbkk....",
  "....kbwbbbbbk...",
  "....kwbbbbbbk...",
  "...kbbbbbbbbBk..",
  "...kbbbbbbbbBk..",
  "....kbbbbbbbk...",
  "....kbbbbbbBk...",
  ".....kkBBBkk....",
  ".......kkk......",
  "................",
};

static const char* const SPR_ICON_BERRY_G[16] = {  // 16x16
  "................",
  "................",
  "...........k....",
  "........k.k.....",
  "........k.......",
  ".......kgk......",
  ".....kkgggkk....",
  "....kgwgggggk...",
  "....kwggggggk...",
  "...kggggggggGk..",
  "...kggggggggGk..",
  "....kgggggggk...",
  "....kggggggGk...",
  ".....kkGGGkk....",
  ".......kkk......",
  "................",
};

static const char* const SPR_ICON_CANDY[16] = {  // 16x16
  "................",
  "................",
  "................",
  "................",
  "................",
  "......kkkkk.....",
  "..k..kpwpppk.k..",
  "...kkpwpppppk...",
  "..kpppppPppPpk..",
  "...kkppppPpPk...",
  "..k..kppppPk.k..",
  "......kkkkk.....",
  "................",
  "................",
  "................",
  "................",
};

static const char* const SPR_EGG[32] = {  // 32x32
  "................................",
  "................................",
  "................................",
  "................................",
  "................................",
  "................................",
  "................k...............",
  ".............kkkykkk............",
  "............kyyyyyyyk...........",
  "...........kyyyyyyyyyk..........",
  "..........kyyyyyyyyyyyk.........",
  ".........kyyggyyyyyyyyyk........",
  ".........kyyggyyyyyyyyyk........",
  "........kyyygyyyyyyyyyyYk.......",
  "........kyyyyyyyyyyyyyYYk.......",
  "........kyyyyyyyyyyyyyYYk.......",
  "........kyyyyyyyyyyggyYYk.......",
  "........kyyyyyyyyyyggyYYk.......",
  "........kyyyyyyyyyyygyYYk.......",
  "........kyyyyyyyyyyyyyYYk.......",
  "........kyyyyyyyyyyyyyYYk.......",
  "........kyyyyyyyyyyyyYYYk.......",
  ".........kyyyggyyyyyyYYk........",
  ".........kyyyggyyyyyYYYk........",
  "..........kyyyyyyyYYYYk.........",
  "...........kyyYYYYYYYk..........",
  "............kYYYYYYYk...........",
  ".............kkkYkkk............",
  "................k...............",
  "................................",
  "................................",
  "................................",
};
static const char* const SPR_POOP[32] = {  // 32x32
  "................................",
  "................................",
  "................................",
  "................................",
  "................................",
  "................................",
  "................................",
  "................................",
  "................................",
  "................k...............",
  ".................k..............",
  "...............kkk..............",
  "..............kccck.............",
  ".............kccccck............",
  "..............kccck.............",
  "..............kCCCk.............",
  "............kkccccckk...........",
  "............kccccccck...........",
  "...........kccccccccCk..........",
  "............kccccccck...........",
  "............kcccccCCk...........",
  "...........kccCCCCCcck..........",
  "..........kccccccccccck.........",
  "..........kccccccccccCk.........",
  ".........kcccccccccccCCk........",
  "..........kccccccccccCk.........",
  "..........kcccccccccCCk.........",
  "...........kkkcCCCCkkk..........",
  "..............kkkkk.............",
  "................................",
  "................................",
  "................................",
};

static const char* const SPR_HEART[32] = {  // 32x32
  "................................",
  "................................",
  "................................",
  "................................",
  "................................",
  "................................",
  "........krrrrk....krrrrk........",
  ".......krrrrrrk..krrrrrrk.......",
  "......krrrrrrrrrrrrrrrrrrk......",
  ".....krrrrrrrrrrrrrrrrrrrrk.....",
  ".....krrwwrrrrrrrrrrrrrrRrk.....",
  ".....krwwrrrrrrrrrrrrrrRRrk.....",
  "......krrrrrrrrrrrrrrrrrrk......",
  ".......krrrrrrrrrrrrrrrrk.......",
  "........krrrrrrrrrrrrrrk........",
  ".........krrrrrrrrrrrrk.........",
  "..........krrrrrrrrrrk..........",
  "...........krrrrrrrrk...........",
  "............krrrrrrk............",
  ".............krrrrk.............",
  "..............krrk..............",
  "...............kk...............",
  "................................",
  "................................",
  "................................",
  "................................",
  "................................",
  "................................",
  "................................",
  "................................",
  "................................",
  "................................",
};
// End of the built-in fallback sprite table.

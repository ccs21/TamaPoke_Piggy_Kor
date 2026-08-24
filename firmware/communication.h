#pragma once

#include <Arduino.h>

enum CommunicationMode : uint8_t {
  COMM_MODE_NONE = 0,
  COMM_MODE_BATTLE = 1,
  COMM_MODE_PAIRING = 2,
};

enum CommunicationRole : uint8_t {
  COMM_ROLE_NONE = 0,
  COMM_ROLE_HOST = 1,
  COMM_ROLE_GUEST = 2,
};

enum CommunicationState : uint8_t {
  COMM_OFF = 0,
  COMM_ADVERTISING,
  COMM_SCANNING,
  COMM_CONNECTING,
  COMM_CONNECTED,
  COMM_ERROR,
};

#pragma pack(push, 1)
struct CommunicationPetData {
  uint16_t magic = 0x5450;  // "PT" little-endian
  uint8_t version = 6;
  uint8_t kind = 1;
  uint8_t mode = COMM_MODE_NONE;
  // 1세대 도감 번호(1~151)와 기술 번호(0~136)는 한 바이트면 충분하다.
  // HP를 별도로 보내면서도 BLE 포켓몬 패킷을 22바이트로 유지한다.
  uint8_t species = 0;
  uint8_t level = 1;
  uint16_t atk = 1;
  uint16_t def = 1;
  uint16_t spe = 1;
  uint16_t hp = 1;
  uint8_t move1 = 0;
  uint8_t ageDays = 0;
  uint8_t flags = 0;
  uint32_t nonce = 0;
};

enum CommunicationPetFlags : uint8_t {
  COMM_PET_SHINY = 1 << 0,
};

struct CommunicationResultData {
  uint16_t magic = 0x5450;
  uint8_t version = 3;
  uint8_t kind = 2;
  uint8_t mode = COMM_MODE_NONE;
  uint8_t hostWon = 0;
  uint8_t hostItemRoll = 0;
  uint8_t guestItemRoll = 0;
  uint8_t hostShinyRoll = 1;
  uint8_t guestShinyRoll = 1;
  uint8_t rounds = 0;
  uint32_t transaction = 0;
};

struct CommunicationBattleActionData {
  uint16_t magic = 0x5450;
  uint8_t version = 3;
  uint8_t kind = 3;
  uint8_t mode = COMM_MODE_BATTLE;
  uint8_t round = 0;
  uint8_t action = 0;
  uint32_t transaction = 0;
};

enum CommunicationBattleFlags : uint8_t {
  COMM_TURN_HOST_DODGED = 1 << 0,
  COMM_TURN_GUEST_DODGED = 1 << 1,
  COMM_TURN_HOST_FIRST = 1 << 2,
  COMM_TURN_HOST_FORFEIT = 1 << 3,
  COMM_TURN_GUEST_FORFEIT = 1 << 4,
  COMM_TURN_HOST_ACTED = 1 << 5,
  COMM_TURN_ENDED = 1 << 6,
  COMM_TURN_HOST_WON = 1 << 7,
};

struct CommunicationBattleTurnData {
  uint16_t magic = 0x5450;
  uint8_t version = 3;
  uint8_t kind = 4;
  uint8_t mode = COMM_MODE_BATTLE;
  uint8_t round = 0;
  uint8_t hostAction = 0;
  uint8_t guestAction = 0;
  uint8_t flags = 0;
  uint8_t hostRestLeft = 0;
  uint8_t guestRestLeft = 0;
  uint8_t hostSkill1Left = 0;
  uint8_t hostSkill2Left = 0;
  uint8_t guestSkill1Left = 0;
  uint8_t guestSkill2Left = 0;
  uint16_t hostTypePct = 100;
  uint16_t guestTypePct = 100;
  uint16_t hostHp = 0;
  uint16_t guestHp = 0;
  uint16_t hostDamage = 0;
  uint16_t guestDamage = 0;
  uint16_t hostHeal = 0;
  uint16_t guestHeal = 0;
  uint32_t transaction = 0;
};
#pragma pack(pop)

static_assert(sizeof(CommunicationPetData) == 22, "BLE pet packet must stay exactly 22 bytes");
static_assert(sizeof(CommunicationBattleTurnData) <= 61, "BLE turn packet must fit the requested MTU");

bool communicationStartHost(CommunicationMode mode, const CommunicationPetData &local);
bool communicationStartGuest(CommunicationMode mode, const CommunicationPetData &local);
void communicationPoll();
void communicationStop();
CommunicationState communicationState();
CommunicationRole communicationRole();
bool communicationTakePeer(CommunicationPetData &peer);
bool communicationSendBattleAction(const CommunicationBattleActionData &action);
bool communicationTakeBattleAction(CommunicationBattleActionData &action);
bool communicationSendBattleTurn(const CommunicationBattleTurnData &turn);
bool communicationTakeBattleTurn(CommunicationBattleTurnData &turn);
bool communicationSendResult(const CommunicationResultData &result);
bool communicationTakeResult(CommunicationResultData &result);
const char *communicationError();

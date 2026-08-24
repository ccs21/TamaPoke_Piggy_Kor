#include "communication.h"

#include <BLE2902.h>
#include <BLEAdvertisedDevice.h>
#include <BLEClient.h>
#include <BLEDevice.h>
#include <BLEScan.h>
#include <BLEServer.h>

#include <cstring>

namespace {
BLEUUID serviceUuid("7d2a0001-6b7f-4e65-8c95-504f4b455441");
BLEUUID linkUuid("7d2a0002-6b7f-4e65-8c95-504f4b455441");

CommunicationState state = COMM_OFF;
CommunicationRole role = COMM_ROLE_NONE;
CommunicationMode mode = COMM_MODE_NONE;
CommunicationPetData localData = {};
CommunicationPetData peerData = {};
CommunicationResultData resultData = {};
CommunicationBattleActionData actionData = {};
CommunicationBattleTurnData turnData = {};
volatile bool peerReady = false;
// `peerReady` is cleared when the UI consumes the packet. Keep a separate
// latch so the host can recover a missed BLE write callback without treating
// the same characteristic value as a new opponent on every loop.
volatile bool peerReceived = false;
volatile bool resultReady = false;
volatile bool actionReady = false;
volatile bool turnReady = false;
volatile bool foundHost = false;
char errorText[40] = "";

BLEServer *server = nullptr;
BLECharacteristic *serverLink = nullptr;
BLEClient *client = nullptr;
BLERemoteCharacteristic *remoteLink = nullptr;
BLEAdvertisedDevice *foundDevice = nullptr;

bool validPet(const CommunicationPetData &packet) {
  return packet.magic == 0x5450 && packet.version == 6 && packet.kind == 1 &&
         packet.mode == mode && packet.species >= 1 && packet.species <= 151;
}

bool validResult(const CommunicationResultData &packet) {
  return packet.magic == 0x5450 && packet.version == 3 && packet.kind == 2 &&
         packet.mode == mode;
}

bool validAction(const CommunicationBattleActionData &packet) {
  return packet.magic == 0x5450 && packet.version == 3 && packet.kind == 3 &&
         mode == COMM_MODE_BATTLE && packet.mode == mode && packet.action <= 4;
}

bool validTurn(const CommunicationBattleTurnData &packet) {
  return packet.magic == 0x5450 && packet.version == 3 && packet.kind == 4 &&
         mode == COMM_MODE_BATTLE && packet.mode == mode &&
         packet.hostAction <= 4 && packet.guestAction <= 4;
}

void setError(const char *message) {
  strncpy(errorText, message, sizeof(errorText) - 1);
  errorText[sizeof(errorText) - 1] = '\0';
  state = COMM_ERROR;
}

class ServerEvents final : public BLEServerCallbacks {
  void onConnect(BLEServer *) override { state = COMM_CONNECTED; }
  void onDisconnect(BLEServer *s) override {
    if (role == COMM_ROLE_HOST && state != COMM_OFF) {
      state = COMM_ADVERTISING;
      s->startAdvertising();
    }
  }
};

class LinkWrites final : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic *characteristic) override {
    String value = characteristic->getValue();
    if (value.length() == sizeof(CommunicationPetData)) {
      CommunicationPetData packet;
      memcpy(&packet, value.c_str(), sizeof(packet));
      if (validPet(packet)) {
        peerData = packet;
        peerReceived = true;
        peerReady = true;
      }
    } else if (value.length() == sizeof(CommunicationBattleActionData)) {
      CommunicationBattleActionData packet;
      memcpy(&packet, value.c_str(), sizeof(packet));
      if (validAction(packet)) {
        actionData = packet;
        actionReady = true;
      }
    }
  }
};

class ClientEvents final : public BLEClientCallbacks {
  void onConnect(BLEClient *) override { state = COMM_CONNECTED; }
  void onDisconnect(BLEClient *) override {
    if (state != COMM_OFF) setError("연결이 끊어졌습니다");
  }
};

void notifyPacket(BLERemoteCharacteristic *, uint8_t *data, size_t length, bool) {
  if (length == sizeof(CommunicationResultData)) {
    CommunicationResultData packet;
    memcpy(&packet, data, sizeof(packet));
    if (!validResult(packet)) return;
    resultData = packet;
    resultReady = true;
  } else if (length == sizeof(CommunicationBattleTurnData)) {
    CommunicationBattleTurnData packet;
    memcpy(&packet, data, sizeof(packet));
    if (!validTurn(packet)) return;
    turnData = packet;
    turnReady = true;
  }
}

class ScanEvents final : public BLEAdvertisedDeviceCallbacks {
  void onResult(BLEAdvertisedDevice advertised) override {
    if (!advertised.haveServiceUUID() || !advertised.isAdvertisingService(serviceUuid)) return;
    BLEDevice::getScan()->stop();
    if (foundDevice) delete foundDevice;
    foundDevice = new BLEAdvertisedDevice(advertised);
    foundHost = true;
  }
};

ServerEvents serverEvents;
LinkWrites linkWrites;
ClientEvents clientEvents;
ScanEvents scanEvents;

void resetState() {
  peerReady = false;
  peerReceived = false;
  resultReady = false;
  actionReady = false;
  turnReady = false;
  foundHost = false;
  errorText[0] = '\0';
  server = nullptr;
  serverLink = nullptr;
  client = nullptr;
  remoteLink = nullptr;
  if (foundDevice) {
    delete foundDevice;
    foundDevice = nullptr;
  }
}

bool beginCommon(CommunicationMode requestedMode, const CommunicationPetData &local,
                 CommunicationRole requestedRole) {
  communicationStop();
  resetState();
  mode = requestedMode;
  role = requestedRole;
  localData = local;
  localData.mode = requestedMode;
  BLEDevice::init("PokeTama");
  BLEDevice::setMTU(64);
  return true;
}
}  // namespace

bool communicationStartHost(CommunicationMode requestedMode, const CommunicationPetData &local) {
  if (!beginCommon(requestedMode, local, COMM_ROLE_HOST)) return false;
  server = BLEDevice::createServer();
  if (!server) { setError("BLE 서버 생성 실패"); return false; }
  server->setCallbacks(&serverEvents);
  BLEService *service = server->createService(serviceUuid);
  serverLink = service->createCharacteristic(
      linkUuid, BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_WRITE |
                    BLECharacteristic::PROPERTY_NOTIFY);
  serverLink->setCallbacks(&linkWrites);
  serverLink->addDescriptor(new BLE2902());
  serverLink->setValue(reinterpret_cast<const uint8_t *>(&localData), sizeof(localData));
  service->start();
  BLEAdvertising *advertising = BLEDevice::getAdvertising();
  advertising->addServiceUUID(serviceUuid);
  advertising->setScanResponse(true);
  advertising->start();
  state = COMM_ADVERTISING;
  return true;
}

bool communicationStartGuest(CommunicationMode requestedMode, const CommunicationPetData &local) {
  if (!beginCommon(requestedMode, local, COMM_ROLE_GUEST)) return false;
  BLEScan *scan = BLEDevice::getScan();
  scan->setAdvertisedDeviceCallbacks(&scanEvents, false);
  scan->setActiveScan(true);
  scan->setInterval(110);
  scan->setWindow(70);
  if (!scan->start(0, nullptr, false)) { setError("BLE 검색 시작 실패"); return false; }
  state = COMM_SCANNING;
  return true;
}

void communicationPoll() {
  // The guest's first write normally arrives through LinkWrites::onWrite().
  // Some ESP32-S3 links acknowledge that write but fail to dispatch the
  // callback. The characteristic still contains the packet, so recover it
  // here. The nonce check excludes the host's initial readable value.
  if (role == COMM_ROLE_HOST && state == COMM_CONNECTED && !peerReceived && serverLink) {
    String value = serverLink->getValue();
    if (value.length() == sizeof(CommunicationPetData)) {
      CommunicationPetData packet;
      memcpy(&packet, value.c_str(), sizeof(packet));
      if (packet.nonce != localData.nonce && validPet(packet)) {
        peerData = packet;
        peerReceived = true;
        peerReady = true;
      }
    }
    return;
  }

  if (role != COMM_ROLE_GUEST || !foundHost || state == COMM_CONNECTED || state == COMM_ERROR) return;
  foundHost = false;
  state = COMM_CONNECTING;
  client = BLEDevice::createClient();
  if (!client) { setError("BLE 클라이언트 생성 실패"); return; }
  client->setClientCallbacks(&clientEvents);
  if (!client->connect(foundDevice)) { setError("상대 기기에 연결하지 못했습니다"); return; }
  BLERemoteService *service = client->getService(serviceUuid);
  if (!service) { setError("통신 서비스를 찾지 못했습니다"); return; }
  remoteLink = service->getCharacteristic(linkUuid);
  if (!remoteLink) { setError("통신 채널을 찾지 못했습니다"); return; }
  String hostValue = remoteLink->readValue();
  if (hostValue.length() != sizeof(CommunicationPetData)) {
    setError("상대 포켓몬 정보를 읽지 못했습니다");
    return;
  }
  CommunicationPetData hostPacket;
  memcpy(&hostPacket, hostValue.c_str(), sizeof(hostPacket));
  if (!validPet(hostPacket)) {
    setError("호환되지 않는 통신 상대입니다");
    return;
  }
  peerData = hostPacket;
  peerReceived = true;
  peerReady = true;
  if (remoteLink->canNotify()) remoteLink->registerForNotify(notifyPacket);
  if (!remoteLink->writeValue(reinterpret_cast<uint8_t *>(&localData), sizeof(localData), true)) {
    setError("내 포켓몬 정보를 보내지 못했습니다");
    return;
  }
  state = COMM_CONNECTED;
}

void communicationStop() {
  if (state == COMM_OFF) return;
  if (BLEDevice::getScan()->isScanning()) BLEDevice::getScan()->stop();
  if (client && client->isConnected()) client->disconnect();
  if (server) BLEDevice::getAdvertising()->stop();
  BLEDevice::deinit(false);
  state = COMM_OFF;
  role = COMM_ROLE_NONE;
  mode = COMM_MODE_NONE;
  resetState();
}

CommunicationState communicationState() { return state; }
CommunicationRole communicationRole() { return role; }

bool communicationTakePeer(CommunicationPetData &peer) {
  if (!peerReady) return false;
  peer = peerData;
  peerReady = false;
  return true;
}

bool communicationSendBattleAction(const CommunicationBattleActionData &action) {
  if (role != COMM_ROLE_GUEST || state != COMM_CONNECTED || !remoteLink) return false;
  CommunicationBattleActionData packet = action;
  return remoteLink->writeValue(reinterpret_cast<uint8_t *>(&packet), sizeof(packet), true);
}

bool communicationTakeBattleAction(CommunicationBattleActionData &action) {
  if (!actionReady) return false;
  action = actionData;
  actionReady = false;
  return true;
}

bool communicationSendBattleTurn(const CommunicationBattleTurnData &turn) {
  if (role != COMM_ROLE_HOST || state != COMM_CONNECTED || !serverLink) return false;
  serverLink->setValue(reinterpret_cast<const uint8_t *>(&turn), sizeof(turn));
  serverLink->notify();
  return true;
}

bool communicationTakeBattleTurn(CommunicationBattleTurnData &turn) {
  if (!turnReady) return false;
  turn = turnData;
  turnReady = false;
  return true;
}

bool communicationSendResult(const CommunicationResultData &result) {
  if (role != COMM_ROLE_HOST || state != COMM_CONNECTED || !serverLink) return false;
  serverLink->setValue(reinterpret_cast<const uint8_t *>(&result), sizeof(result));
  serverLink->notify();
  return true;
}

bool communicationTakeResult(CommunicationResultData &result) {
  if (!resultReady) return false;
  result = resultData;
  resultReady = false;
  return true;
}

const char *communicationError() { return errorText; }

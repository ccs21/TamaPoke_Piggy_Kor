#pragma once

#include <Arduino.h>

enum WifiTimeState : uint8_t {
  WIFI_TIME_OFF = 0,
  WIFI_TIME_SCANNING,
  WIFI_TIME_READY,
  WIFI_TIME_CONNECTING,
  WIFI_TIME_NTP,
  WIFI_TIME_SUCCESS,
  WIFI_TIME_ERROR,
};

enum TimePanel : uint8_t {
  TIME_PANEL_SETTINGS = 0,
  TIME_PANEL_CHOICE,
  TIME_PANEL_AP_LIST,
  TIME_PANEL_PASSWORD,
  TIME_PANEL_CONNECTING,
  TIME_PANEL_MANUAL,
};

enum TimeCorrectionReason : uint8_t {
  TIME_REASON_SETTINGS = 0,
  TIME_REASON_FIRST_BOOT,
  TIME_REASON_RTC_RECOVERY,
};

struct WifiTimeAp {
  char ssid[33];
  int32_t rssi;
  bool secure;
};

void wifiTimeStartScan();
void wifiTimeStartConnect(const char *ssid, const char *password);
void wifiTimeUpdate();
void wifiTimeStop();
WifiTimeState wifiTimeState();
uint8_t wifiTimeApCount();
const WifiTimeAp *wifiTimeAp(uint8_t index);
uint32_t wifiTimeEpoch();          // 한국 현지시각을 wall-clock epoch로 반환
const char *wifiTimeError();

bool wifiTimeLoadPassword(const char *ssid, char *out, size_t outSize);
bool wifiTimeLoadCredentials(char *ssid, size_t ssidSize, char *password,
                             size_t passwordSize);
void wifiTimeRemember(const char *ssid, const char *password);
void wifiTimeClearCredentials();

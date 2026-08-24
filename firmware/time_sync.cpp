#include "time_sync.h"

#include <Preferences.h>
#include <cstring>
#include <ctime>

namespace {

constexpr uint8_t MAX_APS = 8;
constexpr uint32_t CONNECT_TIMEOUT_MS = 20000UL;
constexpr uint32_t NTP_TIMEOUT_MS = 15000UL;
constexpr uint32_t KST_OFFSET_SECONDS = 9UL * 3600UL;
constexpr uint32_t MIN_VALID_UTC_EPOCH = 1735689600UL;  // 2025-01-01 UTC

WifiTimeState state = WIFI_TIME_OFF;
WifiTimeAp aps[MAX_APS] = {};
uint8_t apCount = 0;
uint32_t stageStartedAt = 0;
uint32_t syncedEpoch = 0;
char selectedSsid[33] = {};
char selectedPassword[65] = {};
char errorText[80] = {};

void setError(const char *message) {
  std::strncpy(errorText, message ? message : "연결에 실패했습니다.", sizeof(errorText) - 1);
  errorText[sizeof(errorText) - 1] = 0;
  state = WIFI_TIME_ERROR;
}

void copyCredential(char *target, size_t size, const char *source) {
  if (!target || size == 0) return;
  std::strncpy(target, source ? source : "", size - 1);
  target[size - 1] = 0;
}

}  // namespace

#ifdef _WIN32

void wifiTimeStartScan() {
  apCount = 0;
  syncedEpoch = 0;
  errorText[0] = 0;
  stageStartedAt = millis();
  state = WIFI_TIME_SCANNING;
}

void wifiTimeStartConnect(const char *ssid, const char *password) {
  copyCredential(selectedSsid, sizeof(selectedSsid), ssid);
  copyCredential(selectedPassword, sizeof(selectedPassword), password);
  syncedEpoch = 0;
  errorText[0] = 0;
  stageStartedAt = millis();
  state = WIFI_TIME_CONNECTING;
}

void wifiTimeUpdate() {
  const uint32_t now = millis();
  if (state == WIFI_TIME_SCANNING && now - stageStartedAt >= 700UL) {
    static const WifiTimeAp demo[] = {
        {"TamaPoke_2G", -42, true},
        {"HOME_WIFI", -58, true},
        {"OPEN_GUEST", -72, false},
    };
    apCount = sizeof(demo) / sizeof(demo[0]);
    std::memcpy(aps, demo, sizeof(demo));
    state = WIFI_TIME_READY;
  } else if (state == WIFI_TIME_CONNECTING && now - stageStartedAt >= 700UL) {
    if (std::strcmp(selectedPassword, "fail") == 0) {
      setError("비밀번호를 확인해 주세요.");
    } else {
      state = WIFI_TIME_NTP;
      stageStartedAt = now;
    }
  } else if (state == WIFI_TIME_NTP && now - stageStartedAt >= 900UL) {
    std::time_t utc = std::time(nullptr);
    if (utc < static_cast<std::time_t>(MIN_VALID_UTC_EPOCH)) utc = MIN_VALID_UTC_EPOCH;
    syncedEpoch = static_cast<uint32_t>(utc) + KST_OFFSET_SECONDS;
    state = WIFI_TIME_SUCCESS;
  }
}

void wifiTimeStop() {
  state = WIFI_TIME_OFF;
  apCount = 0;
  syncedEpoch = 0;
}

#else

#include <WiFi.h>

void wifiTimeStartScan() {
  syncedEpoch = 0;
  errorText[0] = 0;
  apCount = 0;
  WiFi.mode(WIFI_STA);
  WiFi.disconnect(false, false);
  WiFi.scanDelete();
  const int16_t result = WiFi.scanNetworks(true, true);
  if (result == WIFI_SCAN_FAILED) {
    setError("주변 Wi-Fi 검색을 시작하지 못했습니다.");
    return;
  }
  stageStartedAt = millis();
  state = WIFI_TIME_SCANNING;
}

void wifiTimeStartConnect(const char *ssid, const char *password) {
  copyCredential(selectedSsid, sizeof(selectedSsid), ssid);
  copyCredential(selectedPassword, sizeof(selectedPassword), password);
  syncedEpoch = 0;
  errorText[0] = 0;
  WiFi.scanDelete();
  WiFi.mode(WIFI_STA);
  WiFi.begin(selectedSsid, selectedPassword);
  stageStartedAt = millis();
  state = WIFI_TIME_CONNECTING;
}

void wifiTimeUpdate() {
  const uint32_t now = millis();
  if (state == WIFI_TIME_SCANNING) {
    const int16_t result = WiFi.scanComplete();
    if (result == WIFI_SCAN_RUNNING) return;
    if (result < 0) {
      setError("주변 Wi-Fi를 검색하지 못했습니다.");
      return;
    }
    apCount = 0;
    for (int16_t i = 0; i < result && apCount < MAX_APS; ++i) {
      const String ssid = WiFi.SSID(i);
      if (ssid.length() == 0) continue;
      bool duplicate = false;
      for (uint8_t j = 0; j < apCount; ++j) {
        if (std::strncmp(aps[j].ssid, ssid.c_str(), sizeof(aps[j].ssid)) == 0) {
          duplicate = true;
          if (WiFi.RSSI(i) > aps[j].rssi) aps[j].rssi = WiFi.RSSI(i);
          break;
        }
      }
      if (duplicate) continue;
      copyCredential(aps[apCount].ssid, sizeof(aps[apCount].ssid), ssid.c_str());
      aps[apCount].rssi = WiFi.RSSI(i);
      aps[apCount].secure = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
      ++apCount;
    }
    WiFi.scanDelete();
    state = WIFI_TIME_READY;
    return;
  }

  if (state == WIFI_TIME_CONNECTING) {
    if (WiFi.status() == WL_CONNECTED) {
      configTime(0, 0, "kr.pool.ntp.org", "pool.ntp.org", "time.google.com");
      stageStartedAt = now;
      state = WIFI_TIME_NTP;
    } else if (now - stageStartedAt >= CONNECT_TIMEOUT_MS) {
      setError("Wi-Fi에 연결하지 못했습니다.");
    }
    return;
  }

  if (state == WIFI_TIME_NTP) {
    const std::time_t utc = std::time(nullptr);
    // 이전 동기화값을 즉시 재사용하지 않도록 잠깐 기다린 뒤 수용한다.
    if (now - stageStartedAt >= 1200UL && utc >= static_cast<std::time_t>(MIN_VALID_UTC_EPOCH)) {
      syncedEpoch = static_cast<uint32_t>(utc) + KST_OFFSET_SECONDS;
      state = WIFI_TIME_SUCCESS;
    } else if (now - stageStartedAt >= NTP_TIMEOUT_MS) {
      setError("온라인 시간 정보를 받지 못했어요.");
    }
  }
}

void wifiTimeStop() {
  WiFi.scanDelete();
  WiFi.disconnect(true, false);
  WiFi.mode(WIFI_OFF);
  state = WIFI_TIME_OFF;
  apCount = 0;
  syncedEpoch = 0;
}

#endif

WifiTimeState wifiTimeState() { return state; }
uint8_t wifiTimeApCount() { return apCount; }

const WifiTimeAp *wifiTimeAp(uint8_t index) {
  return index < apCount ? &aps[index] : nullptr;
}

uint32_t wifiTimeEpoch() { return syncedEpoch; }
const char *wifiTimeError() { return errorText; }

bool wifiTimeLoadPassword(const char *ssid, char *out, size_t outSize) {
  if (!out || outSize == 0 || !ssid) return false;
  out[0] = 0;
  Preferences prefs;
  prefs.begin("tamawifi", true);
  char savedSsid[33] = {};
  char savedPassword[65] = {};
  prefs.getString("ssid", savedSsid, sizeof(savedSsid));
  prefs.getString("pass", savedPassword, sizeof(savedPassword));
  prefs.end();
  if (std::strcmp(savedSsid, ssid) != 0) return false;
  copyCredential(out, outSize, savedPassword);
  return true;
}

bool wifiTimeLoadCredentials(char *ssid, size_t ssidSize, char *password,
                             size_t passwordSize) {
  if (!ssid || ssidSize == 0 || !password || passwordSize == 0) return false;
  ssid[0] = 0;
  password[0] = 0;
  Preferences prefs;
  prefs.begin("tamawifi", true);
  char savedSsid[33] = {};
  char savedPassword[65] = {};
  prefs.getString("ssid", savedSsid, sizeof(savedSsid));
  prefs.getString("pass", savedPassword, sizeof(savedPassword));
  prefs.end();
  if (!savedSsid[0]) return false;
  copyCredential(ssid, ssidSize, savedSsid);
  copyCredential(password, passwordSize, savedPassword);
  return true;
}

void wifiTimeRemember(const char *ssid, const char *password) {
  Preferences prefs;
  prefs.begin("tamawifi", false);
  prefs.putString("ssid", ssid ? ssid : "");
  prefs.putString("pass", password ? password : "");
  prefs.end();
}

void wifiTimeClearCredentials() {
  Preferences prefs;
  prefs.begin("tamawifi", false);
  prefs.clear();
  prefs.end();
}

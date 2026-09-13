#ifndef _WIN32
#include "power_log.h"
#include "audio.h"
#include "rtcbat.h"
#include "pin_config.h"
#include "sdmon.h"
#include <LittleFS.h>
#include <Wire.h>
#include <esp_timer.h>
#include <esp_system.h>
#include <esp_random.h>
#include <cstring>
#include <esp_wifi.h>
#include <esp_bt.h>

// Two rotating CSV files, <=64 KiB each. NVS/game saves are untouched.
// Logging reuses existing wake-ups (no new sleep timer). Each sleep sample
// briefly mounts LittleFS only for this append and immediately unmounts it.
static constexpr size_t FILE_LIMIT = 64 * 1024;
static const char *CURRENT = "/power-log.csv";
static const char *PREVIOUS = "/power-log-prev.csv";
static const char *HEADER =
  "kind,device,board,firmware,boot,epoch,uptime_ms,event,scene,screen_off,walk_rest,"
  "imu_requested,display_pct,pmu_pct,battery_mv,usb_cached,status0,status1,adc30,"
  "dcdc80,ldo90,ldo91,amp_pin,audio_flags,sound_mode,codec0d,codec0e,codec12,"
  "codec01,wifi_mode,bt_status,cpu_mhz,sleep_calls,sleep_errors,sleep_us,"
  "last_sleep_error,last_wake_cause,reset_reason,log_errors";
static uint32_t bootId = 0, logErrors = 0;
static uint32_t sleepCalls = 0, sleepErrors = 0;
static uint64_t sleepUs = 0, lastSampleUs = 0;
static int lastSleepError = 0, lastWakeCause = 0;
static int lastState = -1;
static bool logEnabled = true;

static int readReg(uint8_t address, uint8_t reg) {
  Wire.beginTransmission(address);
  Wire.write(reg);
  if (Wire.endTransmission(false) != 0) return -1;
  if (Wire.requestFrom(address, (uint8_t)1) != 1) return -1;
  return Wire.read();
}
static bool openStorage(bool &owned) {
  owned = !sdReady;
  if (owned && !LittleFS.begin(false, "/littlefs", 10, "sprites")) {
    ++logErrors;
    return false;  // NEVER auto-format an asset filesystem.
  }
  return true;
}
static void closeStorage(bool owned) { if (owned) LittleFS.end(); }

void powerLogSleepResult(uint64_t elapsedUs, int error, int wakeCause) {
  ++sleepCalls;
  if (error) ++sleepErrors;
  else sleepUs += elapsedUs;
  lastSleepError = error;
  lastWakeCause = wakeCause;
}

void powerLogRecord(const char *event, const char *scene, bool screenOff,
                    bool walkRest, bool imuRequested, int displayedPercent,
                    bool usbCached) {
  if (!logEnabled) return;
  const uint64_t nowUs = esp_timer_get_time();
  const int state = (screenOff ? 1 : 0) | (walkRest ? 2 : 0) |
                    (imuRequested ? 4 : 0) | (usbCached ? 8 : 0);
  const bool periodic = strcmp(event, "tick") == 0;
  if (periodic && lastSampleUs &&
      (nowUs - lastSampleUs < 5000000ULL ||
       (state == lastState && nowUs - lastSampleUs < 300000000ULL))) return;
  if (periodic && audioBusy()) return; // avoid interrupting a short sound
  lastSampleUs = nowUs;
  lastState = state;
  if (!bootId) bootId = esp_random() | 1U;

  const int status0 = readReg(0x34, 0x00);
  const int status1 = readReg(0x34, 0x01);
  const int adc = readReg(0x34, 0x30);
  const bool batteryConnected = status0 >= 0 && (status0 & 0x08);
  int pct = batteryConnected ? readReg(0x34, 0xA4) : -1;
  if (pct > 100) pct = -1;
  int mv = -1;
  // Report unavailable ADC as -1; never enable a sensor just to log it.
  if (batteryConnected && adc >= 0 && (adc & 0x01)) {
    int hi = readReg(0x34, 0x34), lo = readReg(0x34, 0x35);
    if (hi >= 0 && lo >= 0) {
      mv = ((hi & 0x1f) << 8) | lo;
      if (mv == 0 || mv == 0x1fff) mv = -1;
    }
  }
  const int dc = readReg(0x34, 0x80), ldo0 = readReg(0x34, 0x90);
  const int ldo1 = readReg(0x34, 0x91);
  int codec[4] = {-1, -1, -1, -1};
  if (!audioBusy()) {
    const uint8_t regs[4] = {0x0d, 0x0e, 0x12, 0x01};
    for (int i = 0; i < 4; ++i) codec[i] = readReg(0x18, regs[i]);
  }
  wifi_mode_t mode = WIFI_MODE_NULL;
  if (esp_wifi_get_mode(&mode) != ESP_OK) mode = WIFI_MODE_NULL;
  const uint64_t device = ESP.getEfuseMac();
  char row[768];
  int length = snprintf(row, sizeof(row),
    "PL,%04lx%08lx,%s,1.48.4-ko,%08lx,%lu,%llu,%s,%s,%d,%d,%d,%d,%d,%d,%d,"
    "%d,%d,%d,%d,%d,%d,%d,%u,%u,%d,%d,%d,%d,%d,%d,%u,%lu,%lu,%llu,%d,%d,%d,%lu\n",
    (unsigned long)(device >> 32), (unsigned long)(uint32_t)device,
    TAMAPOKE_BOARD_NAME, (unsigned long)bootId, (unsigned long)rtcEpoch(),
    (unsigned long long)(nowUs / 1000), event, scene, screenOff, walkRest,
    imuRequested, displayedPercent, pct, mv, usbCached, status0, status1, adc,
    dc, ldo0, ldo1, digitalRead(PA), audioDiagnosticFlags(), audioMode(),
    codec[0], codec[1], codec[2], codec[3], (int)mode,
    (int)esp_bt_controller_get_status(), (unsigned)getCpuFrequencyMhz(),
    (unsigned long)sleepCalls, (unsigned long)sleepErrors,
    (unsigned long long)sleepUs, lastSleepError, lastWakeCause,
    (int)esp_reset_reason(), (unsigned long)logErrors);
  if (length <= 0 || length >= (int)sizeof(row)) { ++logErrors; return; }
  bool owned = false;
  if (!openStorage(owned)) return;
  File f = LittleFS.open(CURRENT, FILE_READ);
  size_t size = f ? f.size() : 0;
  if (f) f.close();
  if (size + (size_t)length > FILE_LIMIT) {
    if (LittleFS.exists(PREVIOUS) && !LittleFS.remove(PREVIOUS)) {
      ++logErrors; closeStorage(owned); return;
    }
    if (!LittleFS.rename(CURRENT, PREVIOUS)) {
      ++logErrors; closeStorage(owned); return;
    }
  }
  if (LittleFS.totalBytes() - LittleFS.usedBytes() < 16384) {
    ++logErrors; closeStorage(owned); return;
  }
  f = LittleFS.open(CURRENT, FILE_APPEND);
  if (!f || f.write((const uint8_t *)row, (size_t)length) != (size_t)length)
    ++logErrors;
  if (f) f.close();
  closeStorage(owned);
}

bool powerLogCommand(const String &line) {
  if (line != "POWERLOG" && line != "POWERLOG OFF" &&
      line != "POWERLOG ON" && line != "POWERLOG CLEAR") return false;
  if (line == "POWERLOG OFF" || line == "POWERLOG ON") {
    logEnabled = line == "POWERLOG ON";
    Serial.println(logEnabled ? "POWERLOG enabled" : "POWERLOG disabled");
    return true;
  }
  bool owned = false;
  if (!openStorage(owned)) { Serial.println("POWERLOG ERROR storage"); return true; }
  if (line == "POWERLOG CLEAR") {
    bool ok = true;
    if (LittleFS.exists(CURRENT)) ok = LittleFS.remove(CURRENT) && ok;
    if (LittleFS.exists(PREVIOUS)) ok = LittleFS.remove(PREVIOUS) && ok;
    Serial.println(ok ? "POWERLOG cleared" : "POWERLOG ERROR clear");
  } else {
    // The normal timeout is zero; diagnostics use bounded back-pressure so
    // a complete CSV reaches the PC instead of silently losing USB packets.
    Serial.setTxTimeoutMs(1000);
    Serial.println("POWERLOG BEGIN v1");
    Serial.println(HEADER);
    const char *paths[] = {PREVIOUS, CURRENT};
    uint8_t chunk[256];
    bool complete = true;
    const uint32_t started = millis();
    for (const char *path : paths) {
      File f = LittleFS.open(path, FILE_READ);
      if (!f) continue;
      while (f.available()) {
        if (!Serial || millis() - started > 30000UL) { complete = false; break; }
        size_t count = f.read(chunk, sizeof(chunk));
        if (!count || Serial.write(chunk, count) != count) { complete = false; break; }
        delay(1);
      }
      f.close();
      if (!complete) break;
    }
    Serial.println(complete ? "POWERLOG END" : "POWERLOG ERROR transfer");
    Serial.setTxTimeoutMs(0);
  }
  closeStorage(owned);
  return true;
}
#endif

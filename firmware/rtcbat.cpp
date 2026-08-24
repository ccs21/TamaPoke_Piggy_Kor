#include "rtcbat.h"
#include "pin_config.h"  // define XPOWERS_CHIP_AXP2101
#include <Wire.h>
#include <time.h>
#include <XPowersLib.h>

#if TAMAPOKE_BOARD_175C
#include <esp_attr.h>
#include <esp_rtc_time.h>
#include <esp_system.h>
#else
#include <SensorPCF85063.hpp>
#endif

#if TAMAPOKE_BOARD_175C
// The 1.75C schematic has no external calendar RTC.  Keep the wall-clock
// anchor in RTC no-init memory and measure elapsed time with the ESP32-S3 RTC
// counter.  Both survive light sleep and CPU/watchdog/USB resets.  A real loss
// of power resets the counter (or invalidates this checked record), so setup
// still requests NTP/manual correction after a complete discharge.
static constexpr uint32_t SOFT_RTC_MAGIC = 0x54524B43UL;  // "TRKC"
struct SoftRtcRetained {
  uint32_t magic;
  uint32_t baseEpoch;
  uint64_t baseRtcUs;
  uint32_t check;
};

RTC_NOINIT_ATTR SoftRtcRetained retainedSoftRtc;
static uint32_t softRtcBaseEpoch = 0;
static uint64_t softRtcBaseUs = 0;

static uint32_t softRtcCheck(uint32_t epoch, uint64_t rtcUs) {
  return SOFT_RTC_MAGIC ^ epoch ^ (uint32_t)rtcUs ^ (uint32_t)(rtcUs >> 32) ^ 0xA57C39E1UL;
}

static bool retainedSoftRtcValid(uint64_t nowRtcUs) {
  if (retainedSoftRtc.magic != SOFT_RTC_MAGIC || retainedSoftRtc.baseEpoch < 1735689600UL)
    return false;
  if (retainedSoftRtc.check != softRtcCheck(retainedSoftRtc.baseEpoch, retainedSoftRtc.baseRtcUs))
    return false;
  // A real power loss restarts the RTC counter near zero.  Never reuse an
  // anchor from a previous powered session if its counter lies in the future.
  return nowRtcUs >= retainedSoftRtc.baseRtcUs;
}

static void retainSoftRtc(uint32_t epoch, uint64_t rtcUs) {
  retainedSoftRtc.magic = 0;
  retainedSoftRtc.baseEpoch = epoch;
  retainedSoftRtc.baseRtcUs = rtcUs;
  retainedSoftRtc.check = softRtcCheck(epoch, rtcUs);
  retainedSoftRtc.magic = SOFT_RTC_MAGIC;
}
#else
static SensorPCF85063 rtc;
#endif
static XPowersPMU pmu;
static bool rtcOk = false;
static bool pmuOk = false;

static void configureBatteryCharger() {
  if (!pmuOk) return;
  pmu.setVbusCurrentLimit(XPOWERS_AXP2101_VBUS_CUR_LIM_900MA);
  pmu.setChargerConstantCurr(XPOWERS_AXP2101_CHG_CUR_500MA);
  pmu.setPrechargeCurr(XPOWERS_AXP2101_PRECHARGE_100MA);
  pmu.setChargerTerminationCurr(XPOWERS_AXP2101_CHG_ITERM_100MA);
  pmu.enableChargerTerminationLimit();
  pmu.setChargeTargetVoltage(XPOWERS_AXP2101_CHG_VOL_4V2);
  pmu.enableCellbatteryCharge();
  Serial.printf("AXP2101 charger: vbusLim=%u chgCur=%u target=%u\n",
                pmu.getVbusCurrentLimit(), pmu.getChargerConstantCurr(),
                pmu.getChargeTargetVoltage());
}

bool rtcBegin() {
#if TAMAPOKE_BOARD_175C
  const uint64_t nowRtcUs = esp_rtc_get_time_us();
  const esp_reset_reason_t resetReason = esp_reset_reason();
  if (retainedSoftRtcValid(nowRtcUs)) {
    softRtcBaseEpoch = retainedSoftRtc.baseEpoch;
    softRtcBaseUs = retainedSoftRtc.baseRtcUs;
  } else {
    softRtcBaseEpoch = 0;
    softRtcBaseUs = nowRtcUs;
    retainedSoftRtc.magic = 0;
  }
  rtcOk = true;
  Serial.printf("1.75C software RTC: reset=%d retained=%d rtc_us=%llu\n",
                (int)resetReason, softRtcBaseEpoch != 0,
                (unsigned long long)nowRtcUs);
#else
  rtcOk = rtc.begin(Wire, IIC_SDA, IIC_SCL);
  if (!rtcOk) Serial.println("PCF85063 no detectado");
#endif
  return rtcOk;
}

uint32_t rtcEpoch() {
#if TAMAPOKE_BOARD_175C
  if (!rtcOk || !softRtcBaseEpoch) return 0;
  const uint64_t nowUs = esp_rtc_get_time_us();
  const uint64_t elapsedSeconds = nowUs >= softRtcBaseUs
                                      ? (nowUs - softRtcBaseUs) / 1000000ULL
                                      : 0;
  const uint64_t epoch = (uint64_t)softRtcBaseEpoch + elapsedSeconds;
  return epoch <= UINT32_MAX ? (uint32_t)epoch : UINT32_MAX;
#else
  if (!rtcOk) return 0;
  RTC_DateTime t = rtc.getDateTime();
  if (t.getYear() < 2025 || t.getYear() > 2120) return 0;  // sin hora valida
  struct tm tmv = {};
  tmv.tm_year = t.getYear() - 1900;
  tmv.tm_mon = t.getMonth() - 1;
  tmv.tm_mday = t.getDay();
  tmv.tm_hour = t.getHour();
  tmv.tm_min = t.getMinute();
  tmv.tm_sec = t.getSecond();
  time_t e = mktime(&tmv);  // TZ por defecto = UTC, consistente con gmtime_r
  return e > 0 ? (uint32_t)e : 0;
#endif
}

void rtcSetEpoch(uint32_t e) {
#if TAMAPOKE_BOARD_175C
  if (!rtcOk || !e) return;
  const uint64_t nowRtcUs = esp_rtc_get_time_us();
  softRtcBaseEpoch = e;
  softRtcBaseUs = nowRtcUs;
  retainSoftRtc(e, nowRtcUs);
#else
  if (!rtcOk) return;
  time_t tt = e;
  struct tm tmv;
  gmtime_r(&tt, &tmv);
  rtc.setDateTime(RTC_DateTime(tmv.tm_year + 1900, tmv.tm_mon + 1, tmv.tm_mday,
                               tmv.tm_hour, tmv.tm_min, tmv.tm_sec));
#endif
}

bool batBegin() {
  pmuOk = pmu.begin(Wire, AXP2101_SLAVE_ADDRESS, IIC_SDA, IIC_SCL);
  if (!pmuOk) Serial.println("AXP2101 no detectado");
  else configureBatteryCharger();
  return pmuOk;
}

void pmuEnablePanel() {
  pmuOk = pmu.begin(Wire, AXP2101_SLAVE_ADDRESS, IIC_SDA, IIC_SCL);
  if (!pmuOk) {
    Serial.println("AXP2101 no detectado (pmuEnablePanel)");
    return;
  }
#if !TAMAPOKE_BOARD_175C
  // Original 1.75: BLDO1 is the 3.3V OLED VDD rail.
  pmu.setBLDO1Voltage(3300);
  pmu.enableBLDO1();
#endif
}

bool rtcClockIntegrityValid() {
#if TAMAPOKE_BOARD_175C
  return rtcOk && softRtcBaseEpoch != 0;
#else
  return rtcOk && rtc.isClockIntegrityGuaranteed();
#endif
}

void pmuDisablePanel() {
#if !TAMAPOKE_BOARD_175C
  if (pmuOk) pmu.disableBLDO1();
#endif
  // 1.75C does not use the original BLDO1 panel rail. Keep it untouched.
}

// el estado de energia (I2C) se cachea ~2 s: leerlo en cada frame del loop
// metia trafico I2C inutil y podia oscilar (parpadeo de brillo)
static uint32_t powerCacheT = 0;
static uint32_t powerCacheMs = 2000;
static int cachedPct = -1;
static bool cachedCharging = false, cachedUsb = true;

static void refreshPower() {
  uint32_t now = millis();
  if (powerCacheT && now - powerCacheT < powerCacheMs) return;
  powerCacheT = now ? now : 1;
  if (!pmuOk) { cachedPct = -1; cachedCharging = false; cachedUsb = true; return; }
  cachedPct = pmu.isBatteryConnect() ? pmu.getBatteryPercent() : -1;
  cachedCharging = pmu.isCharging();
  cachedUsb = pmu.isVbusIn();
}

int batPercent() { refreshPower(); return cachedPct; }
bool batCharging() { refreshPower(); return cachedCharging; }
bool usbPresent() { refreshPower(); return cachedUsb; }

void setPowerCacheInterval(uint32_t ms) {
  if (ms < 500) ms = 500;
  if (ms > 30000) ms = 30000;
  powerCacheMs = ms;
}

void pwrSetup() {
  if (!pmuOk) return;
  // A los 5 s el firmware duerme. Si se bloquea por completo, a los 10 s el
  // AXP2101 reinicia la placa en vez de apagarla definitivamente.
  pmu.setOffLevel(3);
  pmu.setLongPressRestart();
  pmu.enableLongPressShutdown();
  pmu.disableIRQ(XPOWERS_AXP2101_ALL_IRQ);
  pmu.enableIRQ(XPOWERS_AXP2101_PKEY_SHORT_IRQ |
                XPOWERS_AXP2101_PKEY_LONG_IRQ |
                XPOWERS_AXP2101_PKEY_NEGATIVE_IRQ |
                XPOWERS_AXP2101_PKEY_POSITIVE_IRQ);
  pmu.clearIrqStatus();
}

uint8_t pwrEvents() {
  if (!pmuOk) return PWR_EVENT_NONE;
  pmu.getIrqStatus();
  uint8_t events = PWR_EVENT_NONE;
  if (pmu.isPekeyShortPressIrq()) events |= PWR_EVENT_SHORT;
  if (pmu.isPekeyNegativeIrq()) events |= PWR_EVENT_PRESS;
  if (pmu.isPekeyPositiveIrq()) events |= PWR_EVENT_RELEASE;
  if (pmu.isPekeyLongPressIrq()) events |= PWR_EVENT_LONG;
  if (events) pmu.clearIrqStatus();
  return events;
}

bool pwrShortPressed() {
  return (pwrEvents() & PWR_EVENT_SHORT) != 0;
}

bool pwrShutdown() {
  if (!pmuOk) return false;
  pmu.shutdown();
  return true;
}

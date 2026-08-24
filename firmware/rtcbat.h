#pragma once
#include <Arduino.h>

// 1.75: PCF85063 calendar RTC. 1.75C: software wall clock backed by the
// ESP32-S3 RTC counter and checked RTC memory. It survives ordinary sleep and
// CPU resets, then requests correction after a true RTC-domain power loss.
bool rtcBegin();
uint32_t rtcEpoch();             // segundos unix; 0 si el RTC no es valido
void rtcSetEpoch(uint32_t e);
bool rtcClockIntegrityValid();   // false if hardware stopped or soft clock is unset

// PMU AXP2101: estado de la bateria
bool batBegin();
void pmuEnablePanel();           // enciende BLDO1 (OLED VDD 3.3V); llamar antes de gfx->begin()
void pmuDisablePanel();          // corta BLDO1 durante el reposo profundo del juego
int batPercent();                // 0-100, -1 si no hay bateria conectada
bool batCharging();
bool usbPresent();
void setPowerCacheInterval(uint32_t ms);

enum : uint8_t {
  PWR_EVENT_NONE = 0,
  PWR_EVENT_SHORT = 1 << 0,
  PWR_EVENT_PRESS = 1 << 1,
  PWR_EVENT_RELEASE = 1 << 2,
  PWR_EVENT_LONG = 1 << 3,
};

// El apagado fisico queda sustituido por un reinicio de emergencia a los 10 s.
void pwrSetup();
uint8_t pwrEvents();
bool pwrShortPressed();  // sondear en el loop
bool pwrShutdown();      // cuts PMU rails; false only if the PMU is unavailable

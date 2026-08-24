#pragma once

// One codebase supports both Waveshare board revisions. Build-All.ps1 sets
// TAMAPOKE_BOARD_175C for each output; Arduino IDE builds default to 1.75.
#ifndef TAMAPOKE_BOARD_175C
#define TAMAPOKE_BOARD_175C 0
#endif

#define XPOWERS_CHIP_AXP2101

// 466x466 CO5300 AMOLED over QSPI (common pins)
#define LCD_SDIO0 4
#define LCD_SDIO1 5
#define LCD_SDIO2 6
#define LCD_SDIO3 7
#define LCD_SCLK 38
#define LCD_CS 12
#define LCD_WIDTH 466
#define LCD_HEIGHT 466

// CST9217 and AXP2101 share this I2C bus. The original 1.75 also has a
// PCF85063 here; the 1.75C has no external calendar RTC.
#define IIC_SDA 15
#define IIC_SCL 14
#define TP_INT 11

// ES8311 common pins
#define I2S_BCK_IO 9
#define I2S_DI_IO 10
#define I2S_WS_IO 45
#define I2S_DO_IO 8
#define PA 46

#if TAMAPOKE_BOARD_175C
  #define TAMAPOKE_BOARD_NAME "1.75C"
  #define LCD_RESET 2
  #define TP_RESET 2
  #define I2S_MCK_IO 16
  #define PANEL_TOUCH_SHARE_RESET 1
#else
  #define TAMAPOKE_BOARD_NAME "1.75"
  #define LCD_RESET 39
  #define TP_RESET 40
  #define I2S_MCK_IO 42
  #define PANEL_TOUCH_SHARE_RESET 0
#endif

// Both final builds store TPK3 assets in an internal LittleFS partition.

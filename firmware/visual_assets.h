#pragma once

#include <Arduino.h>
#include "Arduino_GFX_Library.h"

// TVR1 files are row-bounded RGB565 runs stored in LittleFS.  They keep
// optional artwork out of the public firmware binary while remaining cheap
// enough to draw on the ESP32-S3.
bool drawVisualAsset(Arduino_GFX *target, const char *path,
                     int16_t originX, int16_t originY, uint8_t scale = 1,
                     int8_t tilt = 0);

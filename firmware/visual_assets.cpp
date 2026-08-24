#include "visual_assets.h"
#include "sdmon.h"

#include <LittleFS.h>
#include <cstring>

namespace {
struct CachedVisual {
  const char *path = nullptr;
  uint16_t width = 0;
  uint16_t height = 0;
  uint32_t runCount = 0;
  uint8_t *runs = nullptr;
  bool attempted = false;
};

constexpr uint8_t kMaxCachedVisuals = 16;
CachedVisual gVisuals[kMaxCachedVisuals];

uint16_t readU16(const uint8_t *p) {
  return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
}

uint32_t readU32(const uint8_t *p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
         ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

CachedVisual *slotFor(const char *path) {
  CachedVisual *freeSlot = nullptr;
  for (auto &item : gVisuals) {
    if (item.path && strcmp(item.path, path) == 0) return &item;
    if (!item.path && !freeSlot) freeSlot = &item;
  }
  if (freeSlot) freeSlot->path = path;
  return freeSlot;
}

bool loadVisual(CachedVisual &item) {
  if (item.attempted) return item.runs != nullptr;
  item.attempted = true;
  if (!sdReady) return false;
  File file = LittleFS.open(item.path, FILE_READ);
  if (!file) return false;
  uint8_t header[12];
  if (file.read(header, sizeof(header)) != sizeof(header) ||
      memcmp(header, "TVR1", 4) != 0) {
    file.close();
    return false;
  }
  item.width = readU16(header + 4);
  item.height = readU16(header + 6);
  item.runCount = readU32(header + 8);
  const uint32_t bytes = item.runCount * 6UL;
  if (!item.width || !item.height || item.width > 512 || item.height > 512 ||
      !item.runCount || item.runCount > 300000UL ||
      bytes > 2UL * 1024 * 1024 || file.size() != 12UL + bytes) {
    file.close();
    return false;
  }
  item.runs = (uint8_t *)ps_malloc(bytes);
  if (!item.runs || file.read(item.runs, bytes) != bytes) {
    if (item.runs) free(item.runs);
    item.runs = nullptr;
    file.close();
    return false;
  }
  file.close();
  return true;
}
}  // namespace

bool drawVisualAsset(Arduino_GFX *target, const char *path,
                     int16_t originX, int16_t originY, uint8_t scale,
                     int8_t tilt) {
  CachedVisual *item = slotFor(path);
  if (!item || !loadVisual(*item)) return false;
  uint16_t x = 0, y = 0;
  const int16_t anchor = item->height * 9 / 10;
  const uint8_t *p = item->runs;
  for (uint32_t index = 0; index < item->runCount && y < item->height; index++, p += 6) {
    const uint16_t length = readU16(p);
    const uint16_t color = readU16(p + 2);
    const bool visible = readU16(p + 4) != 0;
    if (!length || x + length > item->width) return false;
    if (visible) {
      const int16_t rowShift = ((int16_t)y - anchor) * tilt / 100;
      target->fillRect(originX + rowShift + x * scale,
                       originY + y * scale, length * scale, scale, color);
    }
    x += length;
    if (x == item->width) { x = 0; y++; }
  }
  return x == 0 && y == item->height;
}

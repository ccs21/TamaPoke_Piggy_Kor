#include "sdmon.h"
#include "pin_config.h"
#include <FS.h>
#include <LittleFS.h>

bool sdReady = false;
bool sdDirty = false;
SdThumbs thumbs;

bool PmdMon::load(uint8_t dexNum, bool shiny) {
  unload();
  if (!sdReady) return false;

  char path[28];
  snprintf(path, sizeof(path), "/mons/p%s%03u.bin", shiny ? "s" : "", dexNum);
  File f = LittleFS.open(path, FILE_READ);
  if (!f && shiny) {  // sin shiny PMD: usa el normal
    snprintf(path, sizeof(path), "/mons/p%03u.bin", dexNum);
    f = LittleFS.open(path, FILE_READ);
  }
  if (!f) return false;

  uint32_t size = f.size();
  if (size < 7 || size > 3UL * 1024 * 1024) { f.close(); return false; }
  blob = (uint8_t *)ps_malloc(size);
  if (!blob || f.read(blob, size) != size ||
      (memcmp(blob, "TPK3", 4) != 0 && memcmp(blob, "TPK2", 4) != 0)) {
    if (blob) { free(blob); blob = nullptr; }
    f.close();
    return false;
  }
  f.close();

  bool cropped = memcmp(blob, "TPK3", 4) == 0;
  uint8_t nActs = blob[4];
  memcpy(&palCount, blob + 5, 2);
  if (palCount > 256 || (uint32_t)7 + palCount * 2 > size) { unload(); return false; }
  memcpy(pal, blob + 7, palCount * 2);

  const uint8_t *p = blob + 7 + palCount * 2;
  const uint8_t *end = blob + size;
  for (uint8_t i = 0; i < nActs && p + 4 <= end; i++) {
    uint8_t id = p[0], w = p[1], h = p[2], nf = p[3];
    p += 4;
    if (id >= PMD_NACTS || nf > 24) { unload(); return false; }
    if (w == 0 || h == 0 || nf == 0) { unload(); return false; }
    PmdAct &a = acts[id];
    a.w = w;
    a.h = h;
    a.frames = nf;
    uint8_t base = 1;

    if (cropped) {
      // TPK3: [ms:u16, x:u8, y:u8, w:u8, h:u8, pixels...] por frame.
      for (uint8_t k = 0; k < nf; k++) {
        if (p + 6 > end) { unload(); return false; }
        a.ms[k] = p[0] | (p[1] << 8);
        PmdFrame &fr = a.frame[k];
        fr.x = p[2]; fr.y = p[3]; fr.w = p[4]; fr.h = p[5];
        p += 6;
        uint32_t pixelBytes = (uint32_t)fr.w * fr.h;
        if ((uint16_t)fr.x + fr.w > w || (uint16_t)fr.y + fr.h > h ||
            p + pixelBytes > end) { unload(); return false; }
        fr.data = p;
        p += pixelBytes;
        for (int r = fr.h - 1; r >= 0; r--) {
          bool any = false;
          for (int c = 0; c < fr.w && !any; c++)
            if (fr.data[r * fr.w + c] != 0xFF) any = true;
          if (any) {
            uint8_t frameBase = fr.y + r + 1;
            if (frameBase > base) base = frameBase;
            break;
          }
        }
      }
    } else {
      // TPK2 compatibility is useful when comparing a source file on PC.
      uint32_t bytes = (uint32_t)nf * 2 + (uint32_t)w * h * nf;
      if (p + bytes > end) { unload(); return false; }
      for (uint8_t k = 0; k < nf; k++) {
        a.ms[k] = p[0] | (p[1] << 8);
        p += 2;
      }
      for (uint8_t k = 0; k < nf; k++) {
        PmdFrame &fr = a.frame[k];
        fr.x = fr.y = 0; fr.w = w; fr.h = h; fr.data = p;
        p += (uint32_t)w * h;
        for (int r = h - 1; r >= 0; r--) {
          bool any = false;
          for (int c = 0; c < w && !any; c++)
            if (fr.data[r * w + c] != 0xFF) any = true;
          if (any) { if (r + 1 > base) base = r + 1; break; }
        }
      }
    }
    a.base = base;
  }
  loaded = true;
  Serial.printf("cargado %s (%u KB)\n", path, size / 1024);
  return true;
}

void PmdMon::unload() {
  if (blob) {
    free(blob);
    blob = nullptr;
  }
  for (auto &a : acts) {
    a = {};
  }
  loaded = false;
}

bool SdThumbs::load() {
  if (!sdReady) return false;
  File f = LittleFS.open("/mons/thumbs.bin", FILE_READ);
  if (!f) {
    Serial.println("sin thumbs.bin (galeria sin miniaturas)");
    return false;
  }
  uint32_t size = f.size();
  data = (uint8_t *)ps_malloc(size);
  if (!data || f.read(data, size) != size || memcmp(data, "TPTH", 4) != 0) {
    Serial.println("thumbs.bin invalido");
    if (data) { free(data); data = nullptr; }
    f.close();
    return false;
  }
  f.close();
  memcpy(&count, data + 4, 2);
  loaded = true;
  Serial.printf("miniaturas cargadas: %u (%u KB)\n", count, size / 1024);
  return true;
}

const uint8_t *SdThumbs::get(int16_t dex) const {
  if (!loaded || dex < 1 || dex > count) return nullptr;
  uint32_t off;
  memcpy(&off, data + 6 + 4 * (dex - 1), 4);
  return data + off;
}

bool sdBegin() {
  // Never format on failure: the factory-flashed sprite image must not be
  // erased just because a mount was interrupted.
  sdReady = LittleFS.begin(false, "/littlefs", 10, "sprites");
  if (sdReady) {
    Serial.printf("LittleFS sprites: %u/%u KB\n",
                  (unsigned)(LittleFS.usedBytes() / 1024),
                  (unsigned)(LittleFS.totalBytes() / 1024));
    LittleFS.mkdir("/mons");
  } else {
    Serial.println("LittleFS de sprites no disponible");
  }
  return sdReady;
}

void sdEnd() {
  if (!sdReady) return;
  LittleFS.end();
  sdReady = false;
}

bool SdMon::load(uint8_t dexNum, bool shiny) {
  unload();
  if (!sdReady) return false;

  char path[24];
  snprintf(path, sizeof(path), "/mons/%s%03u.bin", shiny ? "s" : "", dexNum);
  File f = LittleFS.open(path, FILE_READ);
  if (!f && shiny) {  // sin variante shiny: usa la normal
    snprintf(path, sizeof(path), "/mons/%03u.bin", dexNum);
    f = LittleFS.open(path, FILE_READ);
  }
  if (!f) {
    Serial.printf("no existe %s\n", path);
    return false;
  }

  char magic[4];
  uint16_t header[4];
  if (f.read((uint8_t *)magic, 4) != 4 || memcmp(magic, "TPK1", 4) != 0 ||
      f.read((uint8_t *)header, 8) != 8) {
    f.close();
    return false;
  }
  w = header[0];
  h = header[1];
  frames = header[2];
  frameMs = header[3];
  // acota dimensiones: evita size desbordado o absurdo con archivo corrupto
  if (f.read((uint8_t *)&palCount, 2) != 2 || palCount > 256 ||
      w == 0 || w > 256 || h == 0 || h > 256 || frames == 0 || frames > 64) {
    f.close();
    return false;
  }
  if (f.read((uint8_t *)pal, palCount * 2) != palCount * 2) {
    f.close();
    return false;
  }

  uint32_t size = (uint32_t)w * h * frames;
  data = (uint8_t *)ps_malloc(size);
  if (!data) {
    Serial.println("sin PSRAM para el sprite");
    f.close();
    return false;
  }
  uint32_t got = f.read(data, size);
  f.close();
  if (got != size) {
    Serial.printf("%s truncado (%u de %u)\n", path, got, size);
    unload();
    return false;
  }

  // zoom entero para que el bicho mida ~200 px de alto en pantalla
  scale = 200 / h;
  if (scale < 2) scale = 2;
  if (scale > 5) scale = 5;

  Serial.printf("cargado %s: %ux%u x%u frames @%ums, escala %u\n",
                path, w, h, frames, frameMs, scale);
  loaded = true;
  return true;
}

void SdMon::unload() {
  if (data) {
    free(data);
    data = nullptr;
  }
  loaded = false;
}

// ---------------------------------------------------------------------------
// Protocolo de mantenimiento por USB para la NOR interna:
//   PUT <ruta> <bytes>\n  + datos crudos   -> "OK" ... "DONE"
//   LS\n                                   -> listado de /mons
// Usar con tools/send_sd.py
// ---------------------------------------------------------------------------

bool sdSerialCommand(const String &line) {
  if (line.startsWith("PUT ")) {
    int sp = line.lastIndexOf(' ');
    String path = line.substring(4, sp);
    uint32_t size = line.substring(sp + 1).toInt();
    if (!sdReady || size == 0 || size > 4 * 1024 * 1024) {
      Serial.println("ERR");
      return true;
    }
    if (!path.startsWith("/")) path = "/" + path;
    LittleFS.remove(path);
    File f = LittleFS.open(path, FILE_WRITE);
    if (!f) {
      Serial.println("ERR");
      return true;
    }
    Serial.println("OK");
    static uint8_t buf[2048];
    uint32_t remaining = size;
    Serial.setTimeout(5000);
    while (remaining > 0) {
      size_t want = remaining > sizeof(buf) ? sizeof(buf) : remaining;
      size_t n = Serial.readBytes(buf, want);
      if (n == 0) break;  // timeout
      f.write(buf, n);
      remaining -= n;
      Serial.println("#");  // ack: listo para el siguiente bloque
    }
    f.close();
    Serial.setTimeout(1000);
    sdDirty = (remaining == 0);
    Serial.println(remaining == 0 ? "DONE" : "ERR");
    return true;
  } else if (line == "LS") {
    File dir = LittleFS.open("/mons");
    if (dir) {
      File e;
      while ((e = dir.openNextFile())) {
        Serial.printf("%s %u\n", e.name(), (uint32_t)e.size());
        e.close();
      }
      dir.close();
    }
    Serial.println("DONE");
    return true;
  }
  return false;
}

#include "korean_canvas.h"

#include "generated/korean_font_data.h"

namespace {

bool decodeUtf8Byte(uint8_t value, uint32_t &codepoint, uint8_t &remaining) {
  if (remaining == 0) {
    if (value < 0x80) {
      codepoint = value;
      return true;
    }
    if ((value & 0xE0) == 0xC0) {
      codepoint = value & 0x1F;
      remaining = 1;
      return false;
    }
    if ((value & 0xF0) == 0xE0) {
      codepoint = value & 0x0F;
      remaining = 2;
      return false;
    }
    if ((value & 0xF8) == 0xF0) {
      codepoint = value & 0x07;
      remaining = 3;
      return false;
    }
    codepoint = '?';
    return true;
  }

  if ((value & 0xC0) != 0x80) {
    codepoint = '?';
    remaining = 0;
    return true;
  }
  codepoint = (codepoint << 6) | (value & 0x3F);
  remaining--;
  return remaining == 0;
}

}  // namespace

uint8_t KoreanCanvas::koreanPixelSize() const {
  if (textsize_y <= 1) return 12;
  if (textsize_y == 2) return 16;
  if (textsize_y == 3) return 24;
  return 32;
}

int16_t KoreanCanvas::findGlyph(uint32_t codepoint) const {
  int16_t low = 0;
  int16_t high = KO_GLYPH_COUNT - 1;
  while (low <= high) {
    const int16_t middle = low + (high - low) / 2;
    const uint32_t value = pgm_read_dword(&KO_CODEPOINTS[middle]);
    if (value == codepoint) return middle;
    if (value < codepoint) low = middle + 1;
    else high = middle - 1;
  }
  return -1;
}

void KoreanCanvas::drawCodepoint(uint32_t codepoint) {
  const int16_t glyphIndex = findGlyph(codepoint);
  if (glyphIndex < 0) {
    Arduino_GFX::write('?');
    return;
  }

  const uint8_t size = koreanPixelSize();
  const uint8_t *bitmaps = nullptr;
  uint16_t bytesPerGlyph = 0;
  switch (size) {
    case 12:
      bitmaps = KO_FONT_12_BITMAPS;
      bytesPerGlyph = KO_FONT_12_BYTES;
      break;
    case 16:
      bitmaps = KO_FONT_16_BITMAPS;
      bytesPerGlyph = KO_FONT_16_BYTES;
      break;
    case 24:
      bitmaps = KO_FONT_24_BITMAPS;
      bytesPerGlyph = KO_FONT_24_BYTES;
      break;
    default:
      bitmaps = KO_FONT_32_BITMAPS;
      bytesPerGlyph = KO_FONT_32_BYTES;
      break;
  }

  if (wrap && cursor_x + size - 1 > _max_text_x) {
    cursor_x = _min_text_x;
    cursor_y += size;
  }
  if (textcolor != textbgcolor) {
    writeFillRect(cursor_x, cursor_y, size, size, textbgcolor);
  }

  const uint32_t bitOffset = static_cast<uint32_t>(glyphIndex) * bytesPerGlyph * 8U;
  for (uint8_t y = 0; y < size; ++y) {
    int8_t runStart = -1;
    for (uint8_t x = 0; x <= size; ++x) {
      bool set = false;
      if (x < size) {
        const uint32_t bitIndex = bitOffset + static_cast<uint32_t>(y) * size + x;
        const uint8_t byte = pgm_read_byte(bitmaps + bitIndex / 8U);
        set = (byte & (0x80U >> (bitIndex % 8U))) != 0;
      }
      if (set && runStart < 0) runStart = static_cast<int8_t>(x);
      if (!set && runStart >= 0) {
        writeFastHLine(cursor_x + runStart, cursor_y + y, x - runStart, textcolor);
        runStart = -1;
      }
    }
  }
  cursor_x += size;
}

size_t KoreanCanvas::write(uint8_t value) {
  if (utf8Remaining_ == 0 && value < 0x80) {
    return Arduino_GFX::write(value);
  }

  if (decodeUtf8Byte(value, utf8Codepoint_, utf8Remaining_)) {
    if (utf8Codepoint_ < 0x80) Arduino_GFX::write(static_cast<uint8_t>(utf8Codepoint_));
    else drawCodepoint(utf8Codepoint_);
    utf8Codepoint_ = 0;
  }
  return 1;
}

int16_t KoreanCanvas::textWidth(const char *text) const {
  if (!text) return 0;
  int16_t lineWidth = 0;
  int16_t maxWidth = 0;
  uint32_t codepoint = 0;
  uint8_t remaining = 0;
  for (const uint8_t *cursor = reinterpret_cast<const uint8_t *>(text); *cursor; ++cursor) {
    if (!decodeUtf8Byte(*cursor, codepoint, remaining)) continue;
    if (codepoint == '\n') {
      if (lineWidth > maxWidth) maxWidth = lineWidth;
      lineWidth = 0;
    } else if (codepoint != '\r') {
      lineWidth += codepoint < 0x80 ? 6 * textsize_x : koreanPixelSize();
    }
    codepoint = 0;
  }
  if (lineWidth > maxWidth) maxWidth = lineWidth;
  return maxWidth;
}

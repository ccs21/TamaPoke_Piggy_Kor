#pragma once

#include "Arduino_GFX_Library.h"

class KoreanCanvas : public Arduino_Canvas {
 public:
  KoreanCanvas(int16_t w, int16_t h, Arduino_G *output, int16_t outputX = 0,
               int16_t outputY = 0, uint8_t rotation = 0)
      : Arduino_Canvas(w, h, output, outputX, outputY, rotation) {}

  // Text size 2 is the smallest size that remains legible on the 1.75-inch
  // round AMOLED. Legacy screens may still request size 1 to fit a label, but
  // no user-facing text may be smaller than the communication-card footer.
  void setTextSize(uint8_t size) {
    Arduino_Canvas::setTextSize(size < 2 ? 2 : size);
  }


  // Every positioned text run starts a new UTF-8 sequence. If a formatted
  // buffer was truncated in the middle of a Korean glyph, carrying that
  // decoder state into the next label turns its first three bytes into "???".
  void setCursor(int16_t x, int16_t y) {
    utf8Codepoint_ = 0;
    utf8Remaining_ = 0;
    Arduino_Canvas::setCursor(x, y);
  }

  size_t write(uint8_t value) override;
  int16_t textWidth(const char *text) const;
  uint8_t koreanPixelSize() const;

 private:
  uint32_t utf8Codepoint_ = 0;
  uint8_t utf8Remaining_ = 0;

  void drawCodepoint(uint32_t codepoint);
  int16_t findGlyph(uint32_t codepoint) const;
};

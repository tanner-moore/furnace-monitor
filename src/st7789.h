// Minimal ST7789 driver on Adafruit GFX's SPITFT base. Written here instead of
// using Adafruit's ST7735/ST7789 library because that library pulls in the
// Arduino SD library, which shadows the ESP32 core's SD and breaks the build.
#pragma once

#include <Adafruit_SPITFT.h>

class St7789 : public Adafruit_SPITFT {
 public:
  St7789(SPIClass* spi, int8_t cs, int8_t dc, int8_t rst) : Adafruit_SPITFT(240, 320, spi, cs, dc, rst) {}

  // Resets and starts a panel of the given size (240x240, 135x240, 170x320, ...).
  void init(uint16_t width, uint16_t height);

  void begin(uint32_t freq = 0) override;
  void setAddrWindow(uint16_t x, uint16_t y, uint16_t w, uint16_t h) override;
  void setRotation(uint8_t r) override;

 private:
  uint16_t panelW_ = 240, panelH_ = 320;
  uint8_t colStart_ = 0, colStart2_ = 0, rowStart_ = 0, rowStart2_ = 0;
  uint16_t xStart_ = 0, yStart_ = 0;
};

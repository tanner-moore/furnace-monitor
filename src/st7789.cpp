#include "st7789.h"

// Commands (ST7789 datasheet).
static constexpr uint8_t kSwReset = 0x01, kSleepOut = 0x11, kNormalOn = 0x13, kInvertOn = 0x21, kDisplayOn = 0x29,
                         kColAddr = 0x2A, kRowAddr = 0x2B, kRamWrite = 0x2C, kMadctl = 0x36, kColMode = 0x3A;
static constexpr uint8_t kMadctlMY = 0x80, kMadctlMX = 0x40, kMadctlMV = 0x20;

void St7789::begin(uint32_t freq) {
  if (!freq) freq = 40000000;
  _freq = freq;
  invertOnCommand = kInvertOn;
  invertOffCommand = 0x20;
  initSPI(freq);  // also pulses the reset pin
}

void St7789::init(uint16_t width, uint16_t height) {
  begin();
  // The controller has 240x320 of RAM; smaller panels sit at an offset in it.
  // Same placement as Adafruit's ST7789 library.
  if (width == 240 && height == 240) {
    rowStart_ = 320 - height;
    rowStart2_ = 0;
    colStart_ = colStart2_ = 0;
  } else {
    rowStart_ = rowStart2_ = (320 - height) / 2;
    colStart_ = (240 - width + 1) / 2;
    colStart2_ = (240 - width) / 2;
  }
  panelW_ = width;
  panelH_ = height;

  sendCommand(kSwReset);
  delay(150);
  sendCommand(kSleepOut);
  delay(10);
  uint8_t mode = 0x55;  // 16-bit colour
  sendCommand(kColMode, &mode, 1);
  delay(10);
  sendCommand(kInvertOn);  // these IPS panels need inversion for true colours
  delay(10);
  sendCommand(kNormalOn);
  delay(10);
  sendCommand(kDisplayOn);
  delay(10);
  setRotation(0);
}

void St7789::setRotation(uint8_t r) {
  rotation = r & 3;
  uint8_t madctl = 0;
  switch (rotation) {
    case 0:
      madctl = kMadctlMX | kMadctlMY;
      xStart_ = colStart_, yStart_ = rowStart_;
      _width = panelW_, _height = panelH_;
      break;
    case 1:
      madctl = kMadctlMY | kMadctlMV;
      xStart_ = rowStart_, yStart_ = colStart2_;
      _width = panelH_, _height = panelW_;
      break;
    case 2:
      madctl = 0;
      xStart_ = colStart2_, yStart_ = rowStart2_;
      _width = panelW_, _height = panelH_;
      break;
    case 3:
      madctl = kMadctlMX | kMadctlMV;
      xStart_ = rowStart2_, yStart_ = colStart_;
      _width = panelH_, _height = panelW_;
      break;
  }
  sendCommand(kMadctl, &madctl, 1);
}

void St7789::setAddrWindow(uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
  x += xStart_;
  y += yStart_;
  writeCommand(kColAddr);
  SPI_WRITE32(((uint32_t)x << 16) | (x + w - 1));
  writeCommand(kRowAddr);
  SPI_WRITE32(((uint32_t)y << 16) | (y + h - 1));
  writeCommand(kRamWrite);
}

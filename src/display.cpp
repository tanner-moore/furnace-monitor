#include "display.h"

#include <Adafruit_GFX.h>
#include <SPI.h>

#include "co16_io.h"
#include "config.h"
#include "monitor.h"
#include "net.h"
#include "pins.h"
#include "st7789.h"

namespace display {

// RGB565 colours.
static constexpr uint16_t kBg = 0x0000, kInk = 0xFFFF, kMuted = 0x8410, kOn = 0xFC60, kBad = 0xF800,
                          kOk = 0x07E0;

static St7789* tft;
static GFXcanvas16* canvas;  // drawn off-screen, then sent in one go (no flicker)

static const char* phaseLabel(furnace::Phase p) {
  switch (p) {
    case furnace::Phase::Idle: return "Idle";
    case furnace::Phase::FanOnly: return "Fan only";
    case furnace::Phase::HeatCall: return "Heat call";
    case furnace::Phase::Ignition: return "Ignition";
    case furnace::Phase::LowFire: return "Low fire";
    case furnace::Phase::HighFire: return "High fire";
    case furnace::Phase::PostPurge: return "Post-purge";
    case furnace::Phase::BlowerOverrun: return "Cooling down";  // "Blower overrun" is too wide at size 3
    case furnace::Phase::Lockout: return "LOCKOUT";
  }
  return "?";
}

static void tempLine(GFXcanvas16& c, int16_t y, const char* label, float tempC, bool isRise = false) {
  c.setTextSize(2);
  c.setTextColor(kMuted);
  c.setCursor(8, y);
  c.print(label);
  c.setTextColor(kInk);
  c.setCursor(c.width() / 2, y);
  if (isnan(tempC)) {
    c.print("--");
  } else if (config.tempUnit == "F") {
    c.printf("%.0f F", isRise ? tempC * 9 / 5 : tempC * 9 / 5 + 32);
  } else {
    c.printf("%.1f C", tempC);
  }
}

static void draw(const monitor::Snapshot& s) {
  GFXcanvas16& c = *canvas;
  const bool alert = s.st.alerts != 0;
  c.fillScreen(kBg);

  // Phase banner.
  const bool firing = s.in.mvl || s.in.mvh;
  c.fillRect(0, 0, c.width(), 44, alert ? kBad : firing ? kOn : 0x2104);
  c.setTextColor(kInk);
  c.setTextSize(3);
  c.setCursor(8, 11);
  // Without the input chip the phase is the last one read, not the current one.
  c.print(s.inputsOk ? phaseLabel(s.st.phase) : "Unknown");

  int16_t y = 56;
  tempLine(c, y, "Supply", s.in.supplyC);
  tempLine(c, y += 24, "Return", s.in.returnC);
  tempLine(c, y += 24, "Rise", s.st.deltaTC, true);
  tempLine(c, y += 24, "Flue", s.in.flueC);

  // Alerts, or today's figures when all is well.
  y += 32;
  c.setTextSize(2);
  if (alert) {
    // One alert per line; the longest name fits a line at this size. Three lines fit above the
    // address, so with more than three the last line counts the rest.
    char names[160];
    furnace::alertNames(s.st.alerts, names, sizeof names);
    char* list[11];
    int n = 0;
    for (char* name = strtok(names, ","); name && n < 11; name = strtok(nullptr, ",")) list[n++] = name;
    const int shown = n > 3 ? 2 : n;
    c.setTextColor(kBad);
    for (int i = 0; i < shown; i++) {
      for (char* p = list[i]; *p; p++)
        if (*p == '_') *p = ' ';
      c.setCursor(8, y + 18 * i);
      c.print(list[i]);
    }
    if (n > shown) {
      c.setCursor(8, y + 18 * shown);
      c.printf("+%d more", n - shown);
    }
  } else {
    c.setTextColor(kOk);
    c.setCursor(8, y);
    c.printf("%lu cycles %lu min", (unsigned long)s.st.cyclesToday, (unsigned long)(s.st.burnerSecondsToday / 60));
    if (s.filterDirty) {
      c.setCursor(8, y + 22);
      c.setTextColor(kBad);
      c.print("Filter dirty");
    } else if (config.filterLifeHours) {
      const float left = 1.0f - s.st.totals.filterBlowerSeconds / (3600.0f * config.filterLifeHours);
      c.setCursor(8, y + 22);
      c.setTextColor(left <= 0 ? kBad : kMuted);
      c.printf("Filter %d%% left", (int)constrain(lroundf(left * 100), 0, 100));
    }
  }

  // Address at the bottom so the web page is easy to find.
  c.setTextSize(1);
  c.setTextColor(kMuted);
  c.setCursor(8, c.height() - 12);
  c.printf("%s %s", net::linkName(), net::ipString().c_str());
}

static void task(void*) {
  for (;;) {
    draw(monitor::snapshot());
    co16::spiLock();
    tft->drawRGBBitmap(0, 0, canvas->getBuffer(), canvas->width(), canvas->height());
    co16::spiUnlock();
    vTaskDelay(pdMS_TO_TICKS(1000));
  }
}

void begin() {
  if (!config.display) return;
  const bool portrait = config.displayRotation % 2 == 0;
  const uint16_t w = portrait ? config.displayWidth : config.displayHeight;
  const uint16_t h = portrait ? config.displayHeight : config.displayWidth;
  canvas = new GFXcanvas16(w, h);
  if (!canvas->getBuffer()) {
    log_e("no memory for the display buffer");
    return;
  }

  pinMode(PIN_LCD_BACKLIGHT, OUTPUT);
  digitalWrite(PIN_LCD_BACKLIGHT, HIGH);
  tft = new St7789(&SPI, PIN_LCD_CS, PIN_LCD_DC, PIN_LCD_RST);
  co16::spiLock();
  tft->init(config.displayWidth, config.displayHeight);
  tft->setRotation(config.displayRotation);
  tft->fillScreen(kBg);
  co16::spiUnlock();

  xTaskCreatePinnedToCore(task, "display", 6144, nullptr, 1, nullptr, 0);
}

}  // namespace display

#include "monitor.h"

#include <Preferences.h>
#include <esp_heap_caps.h>
#include <esp_sntp.h>
#include <stdarg.h>

#include "co16_io.h"
#include "config.h"
#include "flash_decoder.h"
#include "rtc_ds3231.h"

namespace monitor {

using furnace::FlashDecoder;
using furnace::Model;

// History: one sample every 5 s for 24 h, kept in PSRAM.
static constexpr uint32_t kSampleEveryMs = 5000;
static constexpr size_t kHistoryLen = 24 * 3600 / 5;

struct Sample {
  uint32_t epoch;
  int16_t supply10, return10, flue10;  // 0.1 C, INT16_MIN = no reading
  uint8_t inducerA10, blowerA10;       // 0.1 A, 255 = no reading
  uint8_t flags;                       // see kFlag*
  uint8_t phase;
};
enum : uint8_t { kFlagW1 = 1, kFlagW2 = 2, kFlagG = 4, kFlagMvl = 8, kFlagMvh = 16, kFlagFlame = 32, kFlagBlower = 64 };

static constexpr size_t kEventLen = 200;

static SemaphoreHandle_t lock;
static Snapshot snap;
static Model model;
static FlashDecoder led;

static Sample* history;
static size_t histHead, histCount;
static Event events[kEventLen];
static uint32_t eventSeq;

static Preferences prefs;

// --- helpers --------------------------------------------------------------

static int16_t pack10(float v) { return isnan(v) ? INT16_MIN : (int16_t)lroundf(v * 10); }
static float unpack10(int16_t v) { return v == INT16_MIN ? NAN : v / 10.0f; }
static uint8_t packAmps(float v) { return isnan(v) ? 255 : (uint8_t)constrain(lroundf(v * 10), 0, 254); }

static bool inputOn(uint16_t bits, uint8_t input) { return input >= 1 && input <= 16 && (bits >> (input - 1)) & 1; }

static float rtdOrNan(uint8_t ch, float offset) {
  if (ch == 0) return NAN;
  const float t = co16::readRtdC(ch);
  return isnan(t) ? NAN : t + offset;
}

static time_t nowEpoch() {
  time_t t = time(nullptr);
  return t > 1700000000 ? t : 0;
}

static void addEventLocked(const char* text) {
  Event& e = events[eventSeq % kEventLen];
  e.seq = ++eventSeq;
  e.epoch = nowEpoch();
  strlcpy(e.text, text, sizeof e.text);
}

void logEvent(const char* fmt, ...) {
  char buf[72];
  va_list ap;
  va_start(ap, fmt);
  vsnprintf(buf, sizeof buf, fmt, ap);
  va_end(ap);
  xSemaphoreTake(lock, portMAX_DELAY);
  addEventLocked(buf);
  xSemaphoreGive(lock);
}

void applySettings() {
  xSemaphoreTake(lock, portMAX_DELAY);
  model.setSettings(config.model);
  xSemaphoreGive(lock);
}

// --- acquisition task -----------------------------------------------------

static void saveDayCounters(int yday) {
  const auto& st = model.state();
  prefs.putInt("yday", yday);
  prefs.putUInt("cycles", st.cyclesToday);
  prefs.putUInt("burnerS", st.burnerSecondsToday);
  prefs.putUInt("highS", st.highFireSecondsToday);
}

static void task(void*) {
  furnace::Inputs in;
  float spare = NAN, ledV = NAN;
  uint16_t raw = 0;
  bool inputsOk = false;
  uint8_t rtdStep = 0;
  uint32_t lastAnalogMs = 0, lastRtdMs = 0, lastSampleMs = 0, lastSaveMs = 0;
  int lastYday = prefs.getInt("yday", -1);
  bool rtcWritten = false;
  char buf[96];

  for (;;) {
    const uint32_t now = millis();

    // Digital inputs every pass (50 ms).
    uint16_t bits;
    inputsOk = co16::readInputs(bits);
    if (inputsOk) raw = bits;
    in.w1 = inputOn(raw, config.diW1);
    in.w2 = inputOn(raw, config.diW2);
    in.g = inputOn(raw, config.diG);
    in.mvl = inputOn(raw, config.diMvl);
    in.mvh = inputOn(raw, config.diMvh);

    // LED light sensor every pass, so flashes are not missed.
    if (config.aiLed) {
      ledV = co16::readAnalogVolts(config.aiLed);
      if (!isnan(ledV)) led.sample(ledV >= config.ledOnVolts, now);
      in.boardCode = led.code();
    }

    // Motor currents once a second.
    if (now - lastAnalogMs >= 1000) {
      lastAnalogMs = now;
      in.inducerAmps = config.aiInducer ? co16::readAnalogVolts(config.aiInducer) * config.inducerAmpsPerVolt : NAN;
      in.blowerAmps = config.aiBlower ? co16::readAnalogVolts(config.aiBlower) * config.blowerAmpsPerVolt : NAN;
    }

    // One PT100 channel every 500 ms (each read blocks ~90 ms).
    if (now - lastRtdMs >= 500) {
      lastRtdMs = now;
      switch (rtdStep++ % 4) {
        case 0: in.supplyC = rtdOrNan(config.rtdSupply, config.offsetSupply); break;
        case 1: in.returnC = rtdOrNan(config.rtdReturn, config.offsetReturn); break;
        case 2: in.flueC = rtdOrNan(config.rtdFlue, config.offsetFlue); break;
        case 3: spare = rtdOrNan(config.rtdSpare, config.offsetSpare); break;
      }
    }

    in.sensorFault = !inputsOk || (config.rtdSupply && isnan(in.supplyC)) || (config.rtdReturn && isnan(in.returnC)) ||
                     (config.rtdFlue && isnan(in.flueC)) || (config.aiInducer && isnan(in.inducerAmps)) ||
                     (config.aiBlower && isnan(in.blowerAmps));

    const time_t epoch = nowEpoch();
    xSemaphoreTake(lock, portMAX_DELAY);

    // New local day: reset counters.
    if (epoch) {
      struct tm lt;
      localtime_r(&epoch, &lt);
      if (lastYday >= 0 && lt.tm_yday != lastYday) model.newDay();
      if (lt.tm_yday != lastYday) {
        lastYday = lt.tm_yday;
        saveDayCounters(lastYday);
      }
    }

    const furnace::Phase before = model.state().phase;
    const uint32_t ev = model.update(in, now);
    const auto& st = model.state();

    if (ev & (1u << (int)furnace::Event::PhaseChanged)) {
      snprintf(buf, sizeof buf, "%s -> %s", furnace::phaseName(before), furnace::phaseName(st.phase));
      addEventLocked(buf);
    }
    if (ev & (1u << (int)furnace::Event::CycleEnded)) {
      snprintf(buf, sizeof buf, "burner cycle ended after %lu s", (unsigned long)st.lastCycleSeconds);
      addEventLocked(buf);
    }
    if (model.newAlerts()) {
      char names[160];
      furnace::alertNames(model.newAlerts(), names, sizeof names);
      snprintf(buf, sizeof buf, "alert: %s", names);
      addEventLocked(buf);
    }

    snap.uptimeS = now / 1000;
    snap.epoch = epoch;
    snap.in = in;
    snap.st = st;
    snap.spareC = spare;
    snap.ledVolts = ledV;
    snap.rawInputs = raw;
    snap.inputsOk = inputsOk;

    if (history && now - lastSampleMs >= kSampleEveryMs) {
      lastSampleMs = now;
      Sample& s = history[histHead];
      s.epoch = (uint32_t)epoch;
      s.supply10 = pack10(in.supplyC);
      s.return10 = pack10(in.returnC);
      s.flue10 = pack10(in.flueC);
      s.inducerA10 = packAmps(in.inducerAmps);
      s.blowerA10 = packAmps(in.blowerAmps);
      s.flags = (in.w1 ? kFlagW1 : 0) | (in.w2 ? kFlagW2 : 0) | (in.g ? kFlagG : 0) | (in.mvl ? kFlagMvl : 0) |
                (in.mvh ? kFlagMvh : 0) | (st.flame ? kFlagFlame : 0) | (st.blowerOn ? kFlagBlower : 0);
      s.phase = (uint8_t)st.phase;
      histHead = (histHead + 1) % kHistoryLen;
      if (histCount < kHistoryLen) histCount++;
    }

    // Persist today's counters every 10 minutes so a reboot loses little.
    if (now - lastSaveMs >= 600000 && lastYday >= 0) {
      lastSaveMs = now;
      saveDayCounters(lastYday);
    }
    xSemaphoreGive(lock);

    // Keep the RTC in step once NTP has set the clock.
    if (epoch && !rtcWritten && sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
      rtcWritten = rtc::writeUtc(epoch);
    }

    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

void begin() {
  lock = xSemaphoreCreateMutex();
  model.setSettings(config.model);

  history = (Sample*)heap_caps_calloc(kHistoryLen, sizeof(Sample), MALLOC_CAP_SPIRAM);
  if (!history) log_e("no PSRAM for history");

  co16::begin();

  // Start with the RTC time until NTP answers.
  const time_t t = rtc::readUtc();
  if (t) {
    struct timeval tv = {t, 0};
    settimeofday(&tv, nullptr);
  }

  prefs.begin("furnace", false);
  const time_t epoch = nowEpoch();
  if (epoch) {
    struct tm lt;
    localtime_r(&epoch, &lt);
    if (prefs.getInt("yday", -1) == lt.tm_yday)
      model.restoreDay(prefs.getUInt("cycles", 0), prefs.getUInt("burnerS", 0), prefs.getUInt("highS", 0));
  }

  xTaskCreatePinnedToCore(task, "acquire", 8192, nullptr, 3, nullptr, 1);
}

Snapshot snapshot() {
  xSemaphoreTake(lock, portMAX_DELAY);
  Snapshot s = snap;
  xSemaphoreGive(lock);
  return s;
}

static void putNum(JsonObject o, const char* key, float v, int decimals = 1) {
  if (isnan(v)) {
    o[key] = nullptr;
  } else {
    const float p = decimals == 2 ? 100.0f : 10.0f;
    o[key] = roundf(v * p) / p;
  }
}

void stateJson(const Snapshot& s, JsonObject o) {
  char alerts[200];
  o["phase"] = furnace::phaseName(s.st.phase);
  o["heat_call"] = s.in.w1;
  o["heat_call_2"] = s.in.w2;
  o["fan_call"] = s.in.g;
  o["burner"] = s.in.mvl || s.in.mvh;
  o["high_fire"] = s.in.mvh;
  o["flame"] = s.st.flame;
  o["inducer"] = s.st.inducerOn;
  o["inducer_high"] = s.st.inducerHigh;
  o["blower"] = s.st.blowerOn;
  putNum(o, "t_supply", s.in.supplyC);
  putNum(o, "t_return", s.in.returnC);
  putNum(o, "delta_t", s.st.deltaTC);
  putNum(o, "t_flue", s.in.flueC);
  putNum(o, "t_spare", s.spareC);
  putNum(o, "i_inducer", s.in.inducerAmps, 2);
  putNum(o, "i_blower", s.in.blowerAmps, 2);
  o["cycles_today"] = s.st.cyclesToday;
  o["burner_min_today"] = s.st.burnerSecondsToday / 60;
  o["high_fire_min_today"] = s.st.highFireSecondsToday / 60;
  o["last_cycle_s"] = s.st.lastCycleSeconds;
  o["problem"] = s.st.alerts != 0;
  o["alerts"] = furnace::alertNames(s.st.alerts, alerts, sizeof alerts);
  o["board_code"] = s.in.boardCode;
  o["board_status"] = FlashDecoder::describe(s.in.boardCode);
  o["inputs"] = s.rawInputs;
  o["uptime_s"] = s.uptimeS;
  o["time"] = (uint32_t)s.epoch;
}

size_t eventsAfter(uint32_t afterSeq, Event* out, size_t max) {
  xSemaphoreTake(lock, portMAX_DELAY);
  uint32_t first = afterSeq + 1;
  if (eventSeq >= kEventLen && first <= eventSeq - kEventLen) first = eventSeq - kEventLen + 1;
  size_t n = 0;
  for (uint32_t seq = first; seq <= eventSeq && n < max; seq++) out[n++] = events[(seq - 1) % kEventLen];
  xSemaphoreGive(lock);
  return n;
}

void historyJson(uint32_t hours, size_t maxPoints, JsonObject o) {
  xSemaphoreTake(lock, portMAX_DELAY);
  const size_t want = min((size_t)(hours * 3600 / 5), histCount);
  const size_t step = max((size_t)1, (want + maxPoints - 1) / max((size_t)1, maxPoints));
  JsonArray t = o["t"].to<JsonArray>(), sup = o["supply"].to<JsonArray>(), ret = o["return"].to<JsonArray>(),
            flue = o["flue"].to<JsonArray>(), ind = o["inducer"].to<JsonArray>(), blo = o["blower"].to<JsonArray>(),
            flags = o["flags"].to<JsonArray>();
  for (size_t i = want; i >= step; i -= step) {
    const Sample& s = history[(histHead + kHistoryLen - i) % kHistoryLen];
    t.add(s.epoch);
    auto add10 = [](JsonArray a, int16_t v) {
      if (v == INT16_MIN) a.add(nullptr); else a.add(unpack10(v));
    };
    add10(sup, s.supply10);
    add10(ret, s.return10);
    add10(flue, s.flue10);
    if (s.inducerA10 == 255) ind.add(nullptr); else ind.add(s.inducerA10 / 10.0f);
    if (s.blowerA10 == 255) blo.add(nullptr); else blo.add(s.blowerA10 / 10.0f);
    flags.add(s.flags);
  }
  o["flag_bits"] = "1=W1 2=W2 4=G 8=MVL 16=MVH 32=flame 64=blower";
  xSemaphoreGive(lock);
}

}  // namespace monitor

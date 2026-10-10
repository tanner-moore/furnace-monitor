#include "monitor.h"

#include <Preferences.h>
#include <esp_heap_caps.h>
#include <esp_sntp.h>
#include <stdarg.h>

#include "co16_io.h"
#include "config.h"
#include "flash_decoder.h"
#include "rtc_ds3231.h"
#include "sdlog.h"

namespace monitor {

using furnace::FlashDecoder;
using furnace::Model;

// Fine history: one sample every 5 s for 24 h. Coarse history: one-minute
// averages for 30 days, reloaded from the SD card at boot. Both in PSRAM.
static constexpr uint32_t kSampleEveryMs = 5000;
static constexpr uint32_t kFinePeriodS = 5;
static constexpr size_t kFineLen = 24 * 3600 / kFinePeriodS;
static constexpr uint32_t kCoarsePeriodS = 60;
static constexpr size_t kCoarseLen = 30 * 24 * 60;

static constexpr uint32_t kBenchHoldMs = 15000;

struct Sample {
  uint32_t epoch;
  int16_t supply10, return10, flue10;  // 0.1 C, INT16_MIN = no reading
  uint8_t inducerA10, blowerA10;       // 0.1 A, 255 = no reading
  uint8_t flags;                       // see kFlag*
  uint8_t phase;
};
enum : uint8_t { kFlagW1 = 1, kFlagW2 = 2, kFlagG = 4, kFlagMvl = 8, kFlagMvh = 16, kFlagFlame = 32, kFlagBlower = 64 };

struct Ring {
  Sample* buf = nullptr;
  size_t len = 0, head = 0, count = 0;
  uint32_t periodS = 0;

  void push(const Sample& s) {
    if (!buf) return;
    buf[head] = s;
    head = (head + 1) % len;
    if (count < len) count++;
  }
  // i = 1 is the newest sample.
  const Sample& back(size_t i) const { return buf[(head + len - i) % len]; }
};

static constexpr size_t kEventLen = 200;
static constexpr size_t kCycleLen = 100;

static SemaphoreHandle_t lock;
static Snapshot snap;
static Model model;
static FlashDecoder led;

static Ring fine, coarse;
static Event events[kEventLen];
static uint32_t eventSeq;
static CycleRecord cycles[kCycleLen];
static size_t cycleCount;
static uint32_t cycleSeq;

static Bench benchData;
static volatile uint32_t benchUntilMs;
static volatile bool filterResetPending;

static Preferences prefs;

// --- helpers --------------------------------------------------------------

static int16_t pack10(float v) { return isnan(v) ? INT16_MIN : (int16_t)lroundf(v * 10); }
static float unpack10(int16_t v) { return v == INT16_MIN ? NAN : v / 10.0f; }
static uint8_t packAmps(float v) { return isnan(v) ? 255 : (uint8_t)constrain(lroundf(v * 10), 0, 254); }

static bool inputOn(uint16_t bits, uint8_t input) { return input >= 1 && input <= 16 && (bits >> (input - 1)) & 1; }

static time_t nowEpoch() {
  time_t t = time(nullptr);
  return t > 1700000000 ? t : 0;
}

static void storeEventLocked(time_t epoch, const char* text) {
  Event& e = events[eventSeq % kEventLen];
  e.seq = ++eventSeq;
  e.epoch = epoch;
  strlcpy(e.text, text, sizeof e.text);
}

static void addEventLocked(const char* text) {
  const time_t epoch = nowEpoch();
  storeEventLocked(epoch, text);
  if (config.sdLogging) sdlog::logEvent((uint32_t)epoch, text);
}

static void storeCycleLocked(uint32_t seq, uint32_t epoch, const furnace::CycleStats& c) {
  CycleRecord& r = cycles[cycleCount % kCycleLen];
  r.seq = seq;
  r.epoch = epoch;
  r.stats = c;
  cycleCount++;
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

void resetFilter() { filterResetPending = true; }

Bench bench() {
  benchUntilMs = millis() + kBenchHoldMs;
  xSemaphoreTake(lock, portMAX_DELAY);
  Bench b = benchData;
  xSemaphoreGive(lock);
  return b;
}

// --- persistence ----------------------------------------------------------

static void saveDayCounters(int yday) {
  const auto& st = model.state();
  prefs.putInt("yday", yday);
  prefs.putUInt("cycles", st.cyclesToday);
  prefs.putUInt("burnerS", st.burnerSecondsToday);
  prefs.putUInt("highS", st.highFireSecondsToday);
}

static void saveTotals() {
  const furnace::Totals t = model.state().totals;
  prefs.putBytes("totals", &t, sizeof t);
  prefs.putUInt("filterAt", snap.filterChangedEpoch);
}

// --- one-minute averages --------------------------------------------------

struct MinuteAcc {
  uint32_t minute = 0;  // epoch / 60
  int n = 0;
  float sum[5];
  int cnt[5];
  uint8_t flags = 0, phase = 0;

  void add(float v, int i) {
    if (isnan(v)) return;
    sum[i] += v;
    cnt[i]++;
  }
  float avg(int i) const { return cnt[i] ? sum[i] / cnt[i] : NAN; }
  void reset(uint32_t m) {
    minute = m;
    n = 0;
    flags = 0;
    for (int i = 0; i < 5; i++) sum[i] = 0, cnt[i] = 0;
  }
};

static Sample toSample(const sdlog::MinuteRow& r) {
  Sample s;
  s.epoch = r.epoch;
  s.supply10 = pack10(r.supplyC);
  s.return10 = pack10(r.returnC);
  s.flue10 = pack10(r.flueC);
  s.inducerA10 = packAmps(r.inducerAmps);
  s.blowerA10 = packAmps(r.blowerAmps);
  s.flags = r.flags;
  s.phase = r.phase;
  return s;
}

// --- acquisition task -----------------------------------------------------

static bool rtdUsed(uint8_t ch) {
  return ch == config.rtdSupply || ch == config.rtdReturn || ch == config.rtdFlue || ch == config.rtdSpare;
}

static void task(void*) {
  furnace::Inputs in;
  float spare = NAN, ledV = NAN;
  uint16_t raw = 0;
  bool inputsOk = false;
  uint8_t rtdStep = 0, benchAi = 0;
  uint32_t lastAnalogMs = 0, lastRtdMs = 0, lastSampleMs = 0, lastSaveMs = 0;
  int lastYday = prefs.getInt("yday", -1);
  bool rtcWritten = false, filterDueWas = false;
  MinuteAcc acc;
  acc.reset(0);
  char buf[96];

  Bench b;
  for (int i = 0; i < 16; i++) b.aiVolts[i] = NAN;
  for (int i = 0; i < 4; i++) b.rtdOhms[i] = b.rtdC[i] = NAN, b.rtdFault[i] = 0;

  for (;;) {
    const uint32_t now = millis();
    const bool benchOn = (int32_t)(benchUntilMs - now) > 0;

    // Digital inputs every pass (50 ms).
    uint16_t bits;
    inputsOk = co16::readInputs(bits);
    if (inputsOk) raw = bits;
    in.w1 = inputOn(raw, config.diW1);
    in.w2 = inputOn(raw, config.diW2);
    in.g = inputOn(raw, config.diG);
    in.mvl = inputOn(raw, config.diMvl);
    in.mvh = inputOn(raw, config.diMvh);
    in.coAlarm = config.diCo && inputOn(raw, config.diCo) != config.coOnOpen;

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

    // Bench mode: one more analog input per pass, all 16 in under a second.
    if (benchOn) {
      b.aiVolts[benchAi] = co16::readAnalogVolts(benchAi + 1);
      benchAi = (benchAi + 1) % 16;
    }

    // One PT100 channel every 500 ms (each read blocks ~90 ms). Channels not
    // assigned to anything are only read in bench mode.
    if (now - lastRtdMs >= 500) {
      lastRtdMs = now;
      for (int tries = 0; tries < 4; tries++) {
        const uint8_t ch = rtdStep++ % 4 + 1;
        if (!benchOn && !rtdUsed(ch)) continue;
        co16::RtdReading r;
        co16::readRtd(ch, r);
        b.rtdOhms[ch - 1] = r.ohms;
        b.rtdC[ch - 1] = r.tempC;
        b.rtdFault[ch - 1] = r.fault;
        if (ch == config.rtdSupply) in.supplyC = r.tempC + config.offsetSupply;
        if (ch == config.rtdReturn) in.returnC = r.tempC + config.offsetReturn;
        if (ch == config.rtdFlue) in.flueC = r.tempC + config.offsetFlue;
        if (ch == config.rtdSpare) spare = r.tempC + config.offsetSpare;
        break;
      }
    }
    if (!config.rtdSupply) in.supplyC = NAN;
    if (!config.rtdReturn) in.returnC = NAN;
    if (!config.rtdFlue) in.flueC = NAN;
    if (!config.rtdSpare) spare = NAN;

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

    if (filterResetPending) {
      filterResetPending = false;
      model.resetFilter();
      snap.filterChangedEpoch = (uint32_t)epoch;
      filterDueWas = false;
      saveTotals();
      addEventLocked("filter changed");
    }

    const furnace::Phase before = model.state().phase;
    const uint32_t ev = model.update(in, now);
    const auto& st = model.state();

    if (ev & (1u << (int)furnace::Event::PhaseChanged)) {
      snprintf(buf, sizeof buf, "%s -> %s", furnace::phaseName(before), furnace::phaseName(st.phase));
      addEventLocked(buf);
    }
    if (ev & (1u << (int)furnace::Event::TrialFailed)) {
      snprintf(buf, sizeof buf, "ignition trial failed (%lu this call)", (unsigned long)st.failedTrialsThisCall);
      addEventLocked(buf);
    }
    if (ev & (1u << (int)furnace::Event::CycleEnded)) {
      snprintf(buf, sizeof buf, "burner cycle ended after %lu s", (unsigned long)st.lastCycleSeconds);
      addEventLocked(buf);
      storeCycleLocked(++cycleSeq, (uint32_t)epoch, st.lastCycle);
      snap.lastCycleEpoch = (uint32_t)epoch;
      if (config.sdLogging) sdlog::logCycle({(uint32_t)epoch, st.lastCycle});
      saveTotals();
    }
    if (model.newAlerts()) {
      char names[160];
      furnace::alertNames(model.newAlerts(), names, sizeof names);
      snprintf(buf, sizeof buf, "alert: %s", names);
      addEventLocked(buf);
    }
    const bool filterDue = config.filterLifeHours && st.totals.filterBlowerSeconds >= config.filterLifeHours * 3600u;
    if (filterDue && !filterDueWas) addEventLocked("filter change due");
    filterDueWas = filterDue;

    snap.uptimeS = now / 1000;
    snap.epoch = epoch;
    snap.in = in;
    snap.st = st;
    snap.spareC = spare;
    snap.ledVolts = ledV;
    snap.rawInputs = raw;
    snap.inputsOk = inputsOk;
    snap.sdOk = config.sdLogging && sdlog::ok();

    b.inputs = raw;
    b.inputsOk = inputsOk;
    b.ledVolts = ledV;
    b.ledLit = !isnan(ledV) && ledV >= config.ledOnVolts;
    b.ledOnMs = led.lastOnMs();
    b.ledOffMs = led.lastOffMs();
    b.boardCode = in.boardCode;
    benchData = b;

    if (now - lastSampleMs >= kSampleEveryMs) {
      lastSampleMs = now;
      Sample s;
      s.epoch = (uint32_t)epoch;
      s.supply10 = pack10(in.supplyC);
      s.return10 = pack10(in.returnC);
      s.flue10 = pack10(in.flueC);
      s.inducerA10 = packAmps(in.inducerAmps);
      s.blowerA10 = packAmps(in.blowerAmps);
      s.flags = (in.w1 ? kFlagW1 : 0) | (in.w2 ? kFlagW2 : 0) | (in.g ? kFlagG : 0) | (in.mvl ? kFlagMvl : 0) |
                (in.mvh ? kFlagMvh : 0) | (st.flame ? kFlagFlame : 0) | (st.blowerOn ? kFlagBlower : 0);
      s.phase = (uint8_t)st.phase;
      fine.push(s);

      // Fold the 5 s samples into one-minute averages (needs the clock).
      if (epoch) {
        const uint32_t minute = (uint32_t)epoch / kCoarsePeriodS;
        if (minute != acc.minute) {
          if (acc.n) {
            sdlog::MinuteRow r;
            r.epoch = acc.minute * kCoarsePeriodS;
            r.supplyC = acc.avg(0);
            r.returnC = acc.avg(1);
            r.flueC = acc.avg(2);
            r.inducerAmps = acc.avg(3);
            r.blowerAmps = acc.avg(4);
            r.flags = acc.flags;
            r.phase = acc.phase;
            coarse.push(toSample(r));
            if (config.sdLogging) sdlog::logMinute(r);
          }
          acc.reset(minute);
        }
        acc.n++;
        acc.add(in.supplyC, 0);
        acc.add(in.returnC, 1);
        acc.add(in.flueC, 2);
        acc.add(in.inducerAmps, 3);
        acc.add(in.blowerAmps, 4);
        acc.flags |= s.flags;
        acc.phase = s.phase;
      }
    }

    // Persist counters every 10 minutes so a reboot loses little.
    if (now - lastSaveMs >= 600000 && lastYday >= 0) {
      lastSaveMs = now;
      saveDayCounters(lastYday);
      saveTotals();
    }
    xSemaphoreGive(lock);

    // Keep the RTC in step once NTP has set the clock.
    if (epoch && !rtcWritten && sntp_get_sync_status() == SNTP_SYNC_STATUS_COMPLETED) {
      rtcWritten = rtc::writeUtc(epoch);
    }

    vTaskDelay(pdMS_TO_TICKS(50));
  }
}

static bool allocRing(Ring& r, size_t len, uint32_t periodS) {
  r.buf = (Sample*)heap_caps_calloc(len, sizeof(Sample), MALLOC_CAP_SPIRAM);
  r.len = len;
  r.periodS = periodS;
  return r.buf != nullptr;
}

void begin() {
  lock = xSemaphoreCreateMutex();
  model.setSettings(config.model);

  if (!allocRing(fine, kFineLen, kFinePeriodS) || !allocRing(coarse, kCoarseLen, kCoarsePeriodS))
    log_e("no PSRAM for history");

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
  furnace::Totals totals;
  if (prefs.getBytesLength("totals") == sizeof totals) {
    prefs.getBytes("totals", &totals, sizeof totals);
    model.restoreTotals(totals);
  }
  snap.filterChangedEpoch = prefs.getUInt("filterAt", 0);

  // Read recent history, cycles and events back from the SD card. Nothing else
  // is running yet, so the rings need no lock here.
  if (config.sdLogging && sdlog::begin()) {
    const uint32_t t0 = millis();
    if (epoch) sdlog::readMinutes((uint32_t)epoch - kCoarseLen * kCoarsePeriodS, [](const sdlog::MinuteRow& r) {
      coarse.push(toSample(r));
    });
    sdlog::readCycles(kCycleLen, [](const sdlog::CycleRow& r) { storeCycleLocked(0, r.epoch, r.stats); });
    sdlog::readEvents(kEventLen, [](uint32_t e, const char* text) { storeEventLocked(e, text); });
    log_i("restored %u minutes, %u cycles, %u events from SD in %u ms", (unsigned)coarse.count, (unsigned)cycleCount,
          (unsigned)eventSeq, (unsigned)(millis() - t0));
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

static float hours(uint32_t seconds) { return seconds / 3600.0f; }

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
  o["failed_trials"] = s.st.failedTrialsThisCall;

  // The last completed burner cycle (null until one ends after boot).
  const furnace::CycleStats& c = s.st.lastCycle;
  const bool haveCycle = s.st.lastCycleSeconds > 0;
  if (haveCycle) o["cycle_ignition_s"] = c.ignitionSeconds; else o["cycle_ignition_s"] = nullptr;
  putNum(o, "cycle_rise_low", c.riseLowC);
  putNum(o, "cycle_rise_high", c.riseHighC);
  putNum(o, "cycle_flue_max", c.flueMaxC);
  putNum(o, "cycle_inducer_a", c.inducerAmps, 2);
  putNum(o, "cycle_blower_a", c.blowerAmps, 2);

  const furnace::Totals& t = s.st.totals;
  o["total_cycles"] = t.cycles;
  o["total_failed_ignitions"] = t.failedTrials;
  putNum(o, "total_burner_h", hours(t.burnerSeconds));
  putNum(o, "total_high_fire_h", hours(t.highFireSeconds));
  putNum(o, "total_blower_h", hours(t.blowerSeconds));

  const float filterH = hours(t.filterBlowerSeconds);
  putNum(o, "filter_h", filterH);
  o["filter_life_h"] = config.filterLifeHours;
  if (config.filterLifeHours) {
    o["filter_pct"] = (int)constrain(lroundf(100.0f * (1.0f - filterH / config.filterLifeHours)), 0, 100);
    o["filter_due"] = filterH >= config.filterLifeHours;
  } else {
    o["filter_pct"] = nullptr;
    o["filter_due"] = false;
  }
  o["filter_changed"] = s.filterChangedEpoch;

  o["co_alarm"] = s.in.coAlarm;
  o["co_fitted"] = config.diCo != 0;
  o["problem"] = s.st.alerts != 0;
  o["alerts"] = furnace::alertNames(s.st.alerts, alerts, sizeof alerts);
  o["board_code"] = s.in.boardCode;
  o["board_status"] = FlashDecoder::describe(s.in.boardCode);
  o["inputs"] = s.rawInputs;
  o["sd"] = s.sdOk;
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

static void cycleJson(const CycleRecord& r, JsonObject e) {
  const furnace::CycleStats& c = r.stats;
  e["time"] = r.epoch;
  e["seconds"] = c.seconds;
  e["high_fire_s"] = c.highFireSeconds;
  e["ignition_s"] = c.ignitionSeconds;
  putNum(e, "rise_low", c.riseLowC);
  putNum(e, "rise_high", c.riseHighC);
  putNum(e, "flue_max", c.flueMaxC);
  putNum(e, "inducer_a", c.inducerAmps, 2);
  putNum(e, "blower_a", c.blowerAmps, 2);
}

void cyclesJson(size_t max, JsonArray a) {
  xSemaphoreTake(lock, portMAX_DELAY);
  const size_t n = min(max, min(cycleCount, kCycleLen));
  for (size_t i = 1; i <= n; i++) cycleJson(cycles[(cycleCount - i) % kCycleLen], a.add<JsonObject>());
  xSemaphoreGive(lock);
}

size_t cyclesAfter(uint32_t afterSeq, CycleRecord* out, size_t max) {
  xSemaphoreTake(lock, portMAX_DELAY);
  const size_t held = min(cycleCount, kCycleLen);
  size_t n = 0;
  for (size_t i = held; i >= 1 && n < max; i--) {
    const CycleRecord& r = cycles[(cycleCount - i) % kCycleLen];
    if (r.seq > afterSeq) out[n++] = r;
  }
  xSemaphoreGive(lock);
  return n;
}

void historyJson(uint32_t hrs, size_t maxPoints, JsonObject o) {
  xSemaphoreTake(lock, portMAX_DELAY);
  // Use the 5 s samples when they cover the request (or cover as much as the
  // minute averages do, e.g. right after a boot without an SD card).
  const uint32_t wantS = hrs * 3600;
  const uint32_t fineS = fine.count * fine.periodS, coarseS = coarse.count * coarse.periodS;
  const Ring& r = (hrs <= 24 && fineS >= min(wantS, coarseS)) ? fine : coarse;

  const size_t want = min((size_t)(wantS / r.periodS), r.count);
  const size_t step = max((size_t)1, (want + maxPoints - 1) / max((size_t)1, maxPoints));
  JsonArray t = o["t"].to<JsonArray>(), sup = o["supply"].to<JsonArray>(), ret = o["return"].to<JsonArray>(),
            flue = o["flue"].to<JsonArray>(), ind = o["inducer"].to<JsonArray>(), blo = o["blower"].to<JsonArray>(),
            flags = o["flags"].to<JsonArray>();
  for (size_t i = want; i >= step; i -= step) {
    // Over a step of several samples, keep any flag seen (so short burns show).
    uint8_t f = 0;
    for (size_t j = 0; j < step; j++) f |= r.back(i - j).flags;
    const Sample& s = r.back(i);
    t.add(s.epoch);
    auto add10 = [](JsonArray a, int16_t v) {
      if (v == INT16_MIN) a.add(nullptr); else a.add(unpack10(v));
    };
    add10(sup, s.supply10);
    add10(ret, s.return10);
    add10(flue, s.flue10);
    if (s.inducerA10 == 255) ind.add(nullptr); else ind.add(s.inducerA10 / 10.0f);
    if (s.blowerA10 == 255) blo.add(nullptr); else blo.add(s.blowerA10 / 10.0f);
    flags.add(f);
  }
  o["period_s"] = r.periodS * step;
  o["flag_bits"] = "1=W1 2=W2 4=G 8=MVL 16=MVH 32=flame 64=blower";
  xSemaphoreGive(lock);
}

}  // namespace monitor

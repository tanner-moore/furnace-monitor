#include "sdlog.h"

#include <FS.h>
#include <SD.h>
#include <SPI.h>

#include "co16_io.h"
#include "pins.h"

namespace sdlog {

static constexpr uint32_t kSpiHz = 20000000;
static constexpr uint32_t kRemountEveryMs = 60000;
static const char* kCyclesPath = "/cycles.csv";
static const char* kEventsPath = "/events.csv";

enum class Kind : uint8_t { Minute, Cycle, Event };

struct Item {
  Kind kind;
  MinuteRow minute;
  CycleRow cycle;
  uint32_t epoch;
  char text[72];
};

static QueueHandle_t queue;
static bool mounted = false;
static uint32_t lastMountMs = 0;

// --- formatting ---------------------------------------------------------------

static void putNum(String& out, float v, int decimals) {
  out += ',';
  if (!isnan(v)) out += String(v, decimals);
}

static void dayPath(uint32_t epoch, char* buf, size_t len) {
  const time_t t = epoch;
  struct tm lt;
  localtime_r(&t, &lt);
  snprintf(buf, len, "/log/%04d-%02d-%02d.csv", lt.tm_year + 1900, lt.tm_mon + 1, lt.tm_mday);
}

// Splits a CSV line in place. Returns the number of fields.
static int split(char* line, char** fields, int max) {
  int n = 0;
  char* p = line;
  while (n < max) {
    fields[n++] = p;
    char* comma = strchr(p, ',');
    if (!comma) break;
    *comma = '\0';
    p = comma + 1;
  }
  return n;
}

static float num(const char* s) { return *s ? strtof(s, nullptr) : NAN; }

// --- mounting and writing -----------------------------------------------------

static bool mount() {
  lastMountMs = millis();
  co16::spiLock();
  SD.end();
  mounted = SD.begin(PIN_SD_CS, SPI, kSpiHz);
  if (mounted && !SD.exists("/log")) SD.mkdir("/log");
  co16::spiUnlock();
  return mounted;
}

static bool append(const char* path, const char* header, const String& line) {
  File f = SD.open(path, FILE_APPEND, true);
  if (!f) return false;
  if (f.size() == 0) f.print(header);
  const bool ok = f.print(line) == line.length();
  f.close();
  return ok;
}

static void write(const Item& it) {
  String line;
  char path[32];
  const char* header;
  switch (it.kind) {
    case Kind::Minute: {
      const MinuteRow& r = it.minute;
      dayPath(r.epoch, path, sizeof path);
      header = "time,supply_c,return_c,flue_c,inducer_a,blower_a,flags,phase\n";
      line = String(r.epoch);
      putNum(line, r.supplyC, 1);
      putNum(line, r.returnC, 1);
      putNum(line, r.flueC, 1);
      putNum(line, r.inducerAmps, 2);
      putNum(line, r.blowerAmps, 2);
      line += ',' + String(r.flags) + ',' + String(r.phase) + '\n';
      break;
    }
    case Kind::Cycle: {
      const furnace::CycleStats& c = it.cycle.stats;
      strlcpy(path, kCyclesPath, sizeof path);
      header = "time,seconds,high_fire_s,ignition_s,rise_low_c,rise_high_c,flue_max_c,inducer_a,blower_a\n";
      line = String(it.cycle.epoch) + ',' + String(c.seconds) + ',' + String(c.highFireSeconds) + ',' +
             String(c.ignitionSeconds);
      putNum(line, c.riseLowC, 1);
      putNum(line, c.riseHighC, 1);
      putNum(line, c.flueMaxC, 1);
      putNum(line, c.inducerAmps, 2);
      putNum(line, c.blowerAmps, 2);
      line += '\n';
      break;
    }
    case Kind::Event:
      strlcpy(path, kEventsPath, sizeof path);
      header = "time,text\n";
      line = String(it.epoch) + ',' + it.text + '\n';
      break;
  }

  co16::spiLock();
  const bool ok = append(path, header, line);
  co16::spiUnlock();
  if (!ok) {
    log_w("SD write to %s failed", path);
    mounted = false;
  }
}

static void task(void*) {
  Item it;
  for (;;) {
    if (xQueueReceive(queue, &it, portMAX_DELAY) != pdTRUE) continue;
    if (!mounted && millis() - lastMountMs >= kRemountEveryMs) mount();
    if (mounted) write(it);
  }
}

static void enqueue(const Item& it) {
  if (queue) xQueueSend(queue, &it, 0);
}

bool begin() {
  mount();
  if (!mounted) log_w("no SD card");
  queue = xQueueCreate(32, sizeof(Item));
  xTaskCreatePinnedToCore(task, "sdlog", 6144, nullptr, 1, nullptr, 0);
  return mounted;
}

bool ok() { return mounted; }

void logMinute(const MinuteRow& r) {
  Item it{};
  it.kind = Kind::Minute;
  it.minute = r;
  enqueue(it);
}

void logCycle(const CycleRow& r) {
  Item it{};
  it.kind = Kind::Cycle;
  it.cycle = r;
  enqueue(it);
}

void logEvent(uint32_t epoch, const char* text) {
  Item it{};
  it.kind = Kind::Event;
  it.epoch = epoch;
  strlcpy(it.text, text, sizeof it.text);
  for (char* p = it.text; *p; p++)
    if (*p == '\n' || *p == '\r') *p = ' ';
  enqueue(it);
}

// --- reading back at boot -----------------------------------------------------

// Calls fn for each complete line of the file, oldest first. With maxRows set,
// only the last maxRows lines are read (the file is read from near its end).
template <typename Fn>
static void eachLine(const char* path, size_t maxRows, Fn fn) {
  File f = SD.open(path, FILE_READ);
  if (!f) return;
  size_t start = 0;
  if (maxRows) {
    const size_t want = maxRows * 160;  // generous bytes per row
    if (f.size() > want) start = f.size() - want;
  }
  f.seek(start);
  if (start) f.readStringUntil('\n');  // skip the partial line

  String* ring = maxRows ? new String[maxRows] : nullptr;
  size_t count = 0;
  char buf[200];
  while (f.available()) {
    const size_t n = f.readBytesUntil('\n', buf, sizeof buf - 1);
    buf[n] = '\0';
    if (n == 0 || !isdigit((unsigned char)buf[0])) continue;  // header or blank
    if (ring) {
      ring[count % maxRows] = buf;
      count++;
    } else {
      fn(buf);
    }
  }
  f.close();
  if (ring) {
    const size_t first = count > maxRows ? count - maxRows : 0;
    for (size_t i = first; i < count; i++) {
      strlcpy(buf, ring[i % maxRows].c_str(), sizeof buf);
      fn(buf);
    }
    delete[] ring;
  }
}

void readMinutes(uint32_t sinceEpoch, void (*fn)(const MinuteRow&)) {
  if (!mounted || !sinceEpoch) return;
  const time_t now = time(nullptr);
  // Step through local days at noon so DST changes never skip or repeat a day.
  time_t t = sinceEpoch;
  struct tm day;
  localtime_r(&t, &day);
  day.tm_hour = 12;
  day.tm_min = day.tm_sec = 0;
  co16::spiLock();
  for (int guard = 0; guard < 400; guard++) {
    day.tm_isdst = -1;
    const time_t noon = mktime(&day);
    if (noon - 12 * 3600 > now) break;
    char path[32];
    dayPath(noon, path, sizeof path);
    eachLine(path, 0, [&](char* line) {
      char* f[8];
      if (split(line, f, 8) < 8) return;
      MinuteRow r;
      r.epoch = strtoul(f[0], nullptr, 10);
      if (r.epoch < sinceEpoch) return;
      r.supplyC = num(f[1]);
      r.returnC = num(f[2]);
      r.flueC = num(f[3]);
      r.inducerAmps = num(f[4]);
      r.blowerAmps = num(f[5]);
      r.flags = atoi(f[6]);
      r.phase = atoi(f[7]);
      fn(r);
    });
    day.tm_mday++;
  }
  co16::spiUnlock();
}

void readCycles(size_t maxRows, void (*fn)(const CycleRow&)) {
  if (!mounted) return;
  co16::spiLock();
  eachLine(kCyclesPath, maxRows, [&](char* line) {
    char* f[9];
    if (split(line, f, 9) < 9) return;
    CycleRow r;
    r.epoch = strtoul(f[0], nullptr, 10);
    r.stats.seconds = strtoul(f[1], nullptr, 10);
    r.stats.highFireSeconds = strtoul(f[2], nullptr, 10);
    r.stats.ignitionSeconds = strtoul(f[3], nullptr, 10);
    r.stats.riseLowC = num(f[4]);
    r.stats.riseHighC = num(f[5]);
    r.stats.flueMaxC = num(f[6]);
    r.stats.inducerAmps = num(f[7]);
    r.stats.blowerAmps = num(f[8]);
    fn(r);
  });
  co16::spiUnlock();
}

void readEvents(size_t maxRows, void (*fn)(uint32_t epoch, const char* text)) {
  if (!mounted) return;
  co16::spiLock();
  eachLine(kEventsPath, maxRows, [&](char* line) {
    char* comma = strchr(line, ',');
    if (!comma) return;
    *comma = '\0';
    fn(strtoul(line, nullptr, 10), comma + 1);
  });
  co16::spiUnlock();
}

}  // namespace sdlog

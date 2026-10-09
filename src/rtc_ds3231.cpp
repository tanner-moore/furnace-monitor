#include "rtc_ds3231.h"

#include <Wire.h>

#include "pins.h"

namespace rtc {

static uint8_t bcd2bin(uint8_t v) { return (v & 0x0F) + 10 * (v >> 4); }
static uint8_t bin2bcd(uint8_t v) { return ((v / 10) << 4) | (v % 10); }

// timegm() equivalent (days-from-civil), independent of the TZ setting.
static time_t utcFromTm(const struct tm& tm) {
  int y = tm.tm_year + 1900;
  const int m = tm.tm_mon + 1;
  y -= m <= 2;
  const int era = (y >= 0 ? y : y - 399) / 400;
  const int yoe = y - era * 400;
  const int doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + tm.tm_mday - 1;
  const int doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
  const long days = (long)era * 146097 + doe - 719468;
  return (time_t)days * 86400 + tm.tm_hour * 3600 + tm.tm_min * 60 + tm.tm_sec;
}

time_t readUtc() {
  Wire.beginTransmission(I2C_ADDR_RTC);
  Wire.write((uint8_t)0x00);
  if (Wire.endTransmission() != 0) return 0;
  if (Wire.requestFrom((uint8_t)I2C_ADDR_RTC, (uint8_t)7) != 7) return 0;
  uint8_t r[7];
  for (auto& b : r) b = Wire.read();
  struct tm tm = {};
  tm.tm_sec = bcd2bin(r[0] & 0x7F);
  tm.tm_min = bcd2bin(r[1] & 0x7F);
  tm.tm_hour = bcd2bin(r[2] & 0x3F);  // 24-hour mode
  tm.tm_mday = bcd2bin(r[4] & 0x3F);
  tm.tm_mon = bcd2bin(r[5] & 0x1F) - 1;
  tm.tm_year = bcd2bin(r[6]) + 100;  // years since 1900, RTC holds 2000-2099
  if (tm.tm_year < 125) return 0;   // before 2025: never set
  return utcFromTm(tm);
}

bool writeUtc(time_t t) {
  struct tm tm;
  gmtime_r(&t, &tm);
  uint8_t buf[8] = {0x00,
                    bin2bcd(tm.tm_sec),
                    bin2bcd(tm.tm_min),
                    bin2bcd(tm.tm_hour),
                    bin2bcd(tm.tm_wday + 1),
                    bin2bcd(tm.tm_mday),
                    bin2bcd(tm.tm_mon + 1),
                    bin2bcd(tm.tm_year - 100)};
  Wire.beginTransmission(I2C_ADDR_RTC);
  Wire.write(buf, sizeof buf);
  return Wire.endTransmission() == 0;
}

}  // namespace rtc

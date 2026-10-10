#include "co16_io.h"

#include <Adafruit_ADS1X15.h>
#include <Adafruit_MAX31865.h>
#include <SPI.h>
#include <Wire.h>

#include "pins.h"

namespace co16 {

static Adafruit_MAX31865 rtd(PIN_RTD_CS, &SPI);
static Adafruit_ADS1115 adc[4];
static bool adcOk[4];
static const uint8_t kAdcAddr[4] = {I2C_ADDR_ADC_1_4, I2C_ADDR_ADC_5_8, I2C_ADDR_ADC_9_12, I2C_ADDR_ADC_13_16};

static ChipStatus chips;
static SemaphoreHandle_t spiMutex;

static constexpr float kRtdNominal = 100.0f;  // PT100
static constexpr float kRtdRef = 400.0f;      // CO16 reference resistor

static bool i2cWrite(uint8_t addr, const uint8_t* data, size_t len) {
  Wire.beginTransmission(addr);
  Wire.write(data, len);
  return Wire.endTransmission() == 0;
}

// The relays are active low: writing 1 to every PCF8575 pin turns them all off.
// This is the only place the firmware ever addresses the relay expander.
static void forceRelaysOff() {
  const uint8_t allOff[2] = {0xFF, 0xFF};
  chips.relays = i2cWrite(I2C_ADDR_RELAYS, allOff, sizeof allOff);
  if (!chips.relays) log_w("relay expander did not answer");
}

void spiLock() { xSemaphoreTakeRecursive(spiMutex, portMAX_DELAY); }
void spiUnlock() { xSemaphoreGiveRecursive(spiMutex); }

ChipStatus chipStatus() { return chips; }

void begin() {
  spiMutex = xSemaphoreCreateRecursiveMutex();
  Wire.begin(PIN_I2C_SDA, PIN_I2C_SCL, 100000);
  forceRelaysOff();

  // XL9555: make sure both ports are inputs (configuration registers 6 and 7).
  const uint8_t cfg[3] = {0x06, 0xFF, 0xFF};
  chips.inputs = i2cWrite(I2C_ADDR_INPUTS, cfg, sizeof cfg);
  if (!chips.inputs) log_e("input expander did not answer");

  for (int i = 0; i < 4; i++) {
    adcOk[i] = chips.adc[i] = adc[i].begin(kAdcAddr[i], &Wire);
    if (adcOk[i]) {
      adc[i].setGain(GAIN_ONE);  // +/-4.096 V full scale
      adc[i].setDataRate(RATE_ADS1115_860SPS);
    } else {
      log_w("ADS1115 0x%02x missing", kAdcAddr[i]);
    }
  }

  pinMode(PIN_RTD_MUX_S1, OUTPUT);
  pinMode(PIN_RTD_MUX_S2, OUTPUT);
  pinMode(PIN_LCD_CS, OUTPUT);  // keep the other SPI devices deselected
  digitalWrite(PIN_LCD_CS, HIGH);
  pinMode(PIN_SD_CS, OUTPUT);
  digitalWrite(PIN_SD_CS, HIGH);
  SPI.begin(PIN_SPI_SCK, PIN_SPI_MISO, PIN_SPI_MOSI);
  chips.rtd = rtd.begin(MAX31865_3WIRE);
  if (!chips.rtd) log_e("MAX31865 init failed");
}

bool readInputs(uint16_t& bits) {
  const uint8_t reg = 0x00;  // input port 0, auto-increments to port 1
  if (!i2cWrite(I2C_ADDR_INPUTS, &reg, 1)) return false;
  if (Wire.requestFrom((uint8_t)I2C_ADDR_INPUTS, (uint8_t)2) != 2) return false;
  const uint8_t p0 = Wire.read();
  const uint8_t p1 = Wire.read();
  bits = (uint16_t)~(p0 | (p1 << 8));  // inputs pull low when the contact closes
  return true;
}

float readAnalogVolts(uint8_t input) {
  if (input < 1 || input > 16) return NAN;
  const int chip = (input - 1) / 4;
  const int ch = (input - 1) % 4;
  if (!adcOk[chip]) return NAN;
  const int16_t raw = adc[chip].readADC_SingleEnded(ch);
  float v = adc[chip].computeVolts(raw) * ADC_TERMINAL_SCALE;
  if (v < 0.0025f * ADC_TERMINAL_SCALE) v = 0.0f;
  return v;
}

bool readRtd(uint8_t channel, RtdReading& out) {
  out = RtdReading();
  if (channel < 1 || channel > 4) return false;
  spiLock();
  // Mux select: channel 1 = S1 low, S2 low; S3 is tied low on the board.
  // Mapping still to be confirmed against a probe on each channel.
  const uint8_t sel = channel - 1;
  digitalWrite(PIN_RTD_MUX_S1, sel & 1);
  digitalWrite(PIN_RTD_MUX_S2, (sel >> 1) & 1);
  delay(20);
  const uint16_t raw = rtd.readRTD();
  out.fault = rtd.readFault();
  if (out.fault) rtd.clearFault();
  spiUnlock();
  out.ohms = raw * kRtdRef / 32768.0f;
  if (out.fault) return false;
  const float t = rtd.calculateTemperature(raw, kRtdNominal, kRtdRef);
  if (t < -50.0f || t > 450.0f) return false;
  out.tempC = t;
  return true;
}

}  // namespace co16

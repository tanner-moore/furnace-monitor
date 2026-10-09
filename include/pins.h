// KinCony CO16 pin and bus map (ESP32-S3-WROOM-1U N16R8).
// Source: KinCony forum "CO16 ESP32-S3 I/O pin define" (showthread.php?tid=9778).
#pragma once

// I2C bus: relays, inputs, ADCs, RTC, EEPROM.
#define PIN_I2C_SDA 8
#define PIN_I2C_SCL 18

#define I2C_ADDR_RELAYS 0x22   // PCF8575, relays 1-16 (active low). NEVER energized.
#define I2C_ADDR_INPUTS 0x24   // XL9555, dry-contact inputs 1-16 (active low)
#define I2C_ADDR_EEPROM 0x50   // 24C02
#define I2C_ADDR_RTC 0x68      // DS3231
// ADS1115 per group of four analog inputs. Note AI9-12 and AI13-16 are swapped.
#define I2C_ADDR_ADC_1_4 0x48
#define I2C_ADDR_ADC_5_8 0x49
#define I2C_ADDR_ADC_9_12 0x4B
#define I2C_ADDR_ADC_13_16 0x4A

// Shared SPI bus: MAX31865, ST7789 display, SD card.
#define PIN_SPI_SCK 11
#define PIN_SPI_MOSI 10
#define PIN_SPI_MISO 12
#define PIN_RTD_CS 14     // MAX31865
#define PIN_RTD_MUX_S1 7  // NX3L4051 select bit 0
#define PIN_RTD_MUX_S2 21 // NX3L4051 select bit 1 (also wired to MAX31865 DRDY; we drive it, so poll instead)
#define PIN_LCD_CS 4
#define PIN_SD_CS 9

// W5500 Ethernet on its own SPI bus.
#define PIN_ETH_SCK 1
#define PIN_ETH_MOSI 2
#define PIN_ETH_MISO 41
#define PIN_ETH_CS 42
#define PIN_ETH_INT 43
#define PIN_ETH_RST 44

// RS485 (spare).
#define PIN_RS485_RX 38
#define PIN_RS485_TX 39

// Terminal scaling: the analog front end divides 0-10 V down for the ADS1115.
// Factor from KinCony's ESPHome config.
#define ADC_TERMINAL_SCALE 5.16696f

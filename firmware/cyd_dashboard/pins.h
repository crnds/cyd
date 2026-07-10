#pragma once

// Pin mapping for the ESP32-2432S028R "Cheap Yellow Display" (2.8" ILI9341,
// resistive XPT2046 touch, shared VSPI bus). Capacitive (GT911) variants use
// different pins and a different LovyanGFX panel/touch config.

#define CYD_TFT_MOSI 13
#define CYD_TFT_MISO 12
#define CYD_TFT_SCLK 14
#define CYD_TFT_CS   15
#define CYD_TFT_DC   2
#define CYD_TFT_RST  -1
#define CYD_TFT_BL   21

// The XPT2046 touch controller is NOT on the display's SPI bus — it has its
// own dedicated pins on this board (classic CYD gotcha).
#define CYD_TOUCH_SCLK 25
#define CYD_TOUCH_MOSI 32
#define CYD_TOUCH_MISO 39
#define CYD_TOUCH_CS   33
#define CYD_TOUCH_IRQ  36

// The onboard microSD slot is NOT on the display's VSPI bus either — like
// touch, it has its own dedicated SPI pins (confirmed on hardware: mounting
// failed when sharing the display's remapped VSPI signals). Driven as a
// second hardware SPI bus (HSPI) in cyd_dashboard.ino.
#define CYD_SD_SCLK  18
#define CYD_SD_MISO  19
#define CYD_SD_MOSI  23
#define CYD_SD_CS    5

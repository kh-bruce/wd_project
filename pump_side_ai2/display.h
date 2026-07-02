// =====================================================================
// display.h — optional 0.96" I2C OLED (SSD1306 128x64) status screen.
//
// displayInit() runs once in setup(). It draws a boot splash and then spawns a
// dedicated FreeRTOS task that owns the panel and drives all subsequent redraws
// — so the ~100ms I2C flush blocks that task, never loop() (which would starve
// the MQTT keepalive). loop() no longer calls into the display at all. If no
// panel is found at boot, no task is spawned and the controller runs headless.
//
// The U8g2 / Wire dependency lives entirely in display.cpp; this header keeps
// the rest of the project (and the host test harness) free of it.
// =====================================================================
#ifndef DISPLAY_H
#define DISPLAY_H

#include <Arduino.h>

void displayInit();       // Wire.begin + panel begin(); draws splash + spawns the OLED task. Call once in setup().
void displayCycleStyle(); // advance OLED style 1->..->5->1 and persist to NVS. Call from loop thread (button).

#endif // DISPLAY_H

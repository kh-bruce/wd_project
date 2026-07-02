// =====================================================================
// display.h — optional 0.96" I2C OLED (SSD1306 128x64) status screen.
//
// Both functions run on the loop thread ONLY (called from setup()/loop()),
// preserving the "loop is the sole hardware owner" model. The display is
// strictly optional: if no panel is found at boot, displayTick() is a no-op
// and the controller runs identically headless.
//
// The U8g2 / Wire dependency lives entirely in display.cpp; this header keeps
// the rest of the project (and the host test harness) free of it.
// =====================================================================
#ifndef DISPLAY_H
#define DISPLAY_H

#include <Arduino.h>

void displayInit();   // Wire.begin + panel begin(); latches "present" flag. Call once in setup().
void displayTick();   // throttled redraw; no-op if the OLED is absent. Call every loop().

#endif // DISPLAY_H

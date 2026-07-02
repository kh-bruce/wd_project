// =====================================================================
// button.h — on-board BOOT button (GPIO0) as a UI button.
//
// Runs on the loop thread ONLY (buttonInit in setup, buttonTick every loop),
// matching the "loop is the sole hardware/state owner" model. Actions never
// touch hardware directly — a long press enqueues a pump command, a short
// press calls the display's style switch. Non-blocking: no delay()/while.
//
//   short press (released < BUTTON_LONGPRESS_MS) -> cycle OLED style
//   long  press (held    >= BUTTON_LONGPRESS_MS) -> toggle pump run/stop
//
// GPIO0 is a strapping pin, so presses are ignored for the first
// BUTTON_BOOT_IGNORE_MS after boot (a held button at reset means flash mode).
// =====================================================================
#ifndef BUTTON_H
#define BUTTON_H

void buttonInit();   // pinMode(INPUT_PULLUP). Call once in setup().
void buttonTick();   // poll + debounce + short/long dispatch. Call every loop().

#endif // BUTTON_H

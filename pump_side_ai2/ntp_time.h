// =====================================================================
// ntp_time.h — background (non-blocking) NTP sync + cached local time.
// Time is synced from loop() via timer ticks; never blocks setup() (that
// caused the boot watchdog reboot). Local time is UTC+8 from cached epoch.
// =====================================================================
#ifndef NTP_TIME_H
#define NTP_TIME_H

#include <Arduino.h>

void ntpInit();          // start background fast-poll; call once in setup()
void ntpTick();          // call every loop()

// Local time (UTC+8) from cached NTP values; false if no sync yet.
bool getLocalTimeFromCache(int &hoursOut, String &formattedOut);
String formatTimeFromEpoch(unsigned long epoch);
bool isTimeInRange();    // true if within prefill window
bool isBadTime();        // true if within quiet hours

extern unsigned long lastTimeSyncMs;  // 0 if never synced

#endif // NTP_TIME_H

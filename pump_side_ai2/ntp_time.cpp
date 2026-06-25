#include "ntp_time.h"
#include "config.h"
#include "logging.h"
#include <WiFiUdp.h>
#include <NTPClient.h>
#include <arduino-timer.h>

static WiFiUDP ntpUDP;
static NTPClient timeClient(ntpUDP, "time.nist.gov");

static const char* ntpServers[] = {
  "pool.ntp.org", "time.nist.gov", "time.google.com", "time.windows.com"
};
// Direct IPs to avoid DNS issues
static const char* ntpServersIpFallback[] = {
  "129.6.15.28",    // time-a-g.nist.gov
  "216.239.35.0",   // time.google.com (anycast)
  "133.243.238.244" // ntp.nict.jp
};

unsigned long lastTimeSyncMs = 0;
static unsigned long lastTimeEpoch = 0;
static int ntpAttemptIndex = 0;

static auto timer_ntp = timer_create_default();
static Timer<>::Task ntpTask;

static bool ntpFastPoll(void *);
static bool ntpSlowPoll(void *);
static void scheduleNtpFast();
static void scheduleNtpSlow();

String formatTimeFromEpoch(unsigned long epoch) {
  if (epoch == 0) return "time not synced";
  unsigned long secDay = epoch % 86400;
  int h = secDay / 3600;
  int m = (secDay % 3600) / 60;
  int s = secDay % 60;
  char buf[9];
  snprintf(buf, sizeof(buf), "%02d:%02d:%02d", h, m, s);
  return String(buf);
}

bool getLocalTimeFromCache(int &hoursOut, String &formattedOut) {
  if (lastTimeEpoch == 0 || lastTimeSyncMs == 0) return false;
  unsigned long nowMs = millis();
  unsigned long nowEpoch = lastTimeEpoch + ((nowMs - lastTimeSyncMs) / 1000);
  hoursOut = (nowEpoch % 86400) / 3600;
  formattedOut = formatTimeFromEpoch(nowEpoch);
  return true;
}

bool isTimeInRange() {
  int hours = 0; String t;
  if (!getLocalTimeFromCache(hours, t)) return false;
  return (hours >= cfg::PREFILL_HOUR_START && hours < cfg::PREFILL_HOUR_END);
}

bool isBadTime() {
  int hours = 0; String t;
  if (!getLocalTimeFromCache(hours, t)) return false;
  // quiet hours wrap midnight: 23:00–06:00
  return (hours >= cfg::QUIET_HOUR_START || hours < cfg::QUIET_HOUR_END);
}

// One NTP attempt, no in-function delay — keeps each loop tick short so it
// can't approach the watchdog. The timer spaces out retries.
static bool syncTimeOnce(const char* server) {
  timeClient.end();
  timeClient = NTPClient(ntpUDP, server, 28800, 60000); // UTC+8 offset
  timeClient.begin();
  if (timeClient.forceUpdate()) {
    lastTimeSyncMs = millis();
    lastTimeEpoch  = timeClient.getEpochTime();
    Serial.print("NTP synced via "); Serial.print(server);
    Serial.print(": "); Serial.println(timeClient.getFormattedTime());
    return true;
  }
  return false;
}

static bool pollOnce() {
  const int nameCount = sizeof(ntpServers) / sizeof(ntpServers[0]);
  const int ipCount   = sizeof(ntpServersIpFallback) / sizeof(ntpServersIpFallback[0]);
  const int total = nameCount + ipCount;
  if (total == 0) return false;
  int idx = ntpAttemptIndex % total;
  ntpAttemptIndex++;
  const char* server = (idx < nameCount) ? ntpServers[idx]
                                         : ntpServersIpFallback[idx - nameCount];
  return syncTimeOnce(server);
}

static bool ntpFastPoll(void *) {
  if (pollOnce()) { scheduleNtpSlow(); return false; } // stop fast; slow continues
  logError("NTP fast poll failed");
  return true;
}

static bool ntpSlowPoll(void *) {
  pollOnce(); // keep correcting drift hourly; failures are harmless
  return true;
}

static void scheduleNtpFast() {
  timer_ntp.cancel(ntpTask);
  ntpTask = timer_ntp.every(2000, ntpFastPoll);
}
static void scheduleNtpSlow() {
  timer_ntp.cancel(ntpTask);
  ntpTask = timer_ntp.every(60UL * 60 * 1000, ntpSlowPoll);
}

void ntpInit() { scheduleNtpFast(); }
void ntpTick() { timer_ntp.tick(); }

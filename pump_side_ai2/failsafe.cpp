#include "failsafe.h"
#include "config.h"
#include "commands.h"
#include "pump_control.h"
#include "logging.h"

bool bad_conn_mode = false;
int  bad_conn_count = 0;

void failsafeInit() {
  // Treat boot as "just heard from the tower" so we don't trip during the
  // first BAD_CONN_DELAY_MS while waiting for the first message.
  portENTER_CRITICAL(&cmdMux);
  lastWaterMs = millis();
  portEXIT_CRITICAL(&cmdMux);
  bad_conn_mode = false;
}

void serviceFailsafe() {
  unsigned long waterMs;
  portENTER_CRITICAL(&cmdMux);
  waterMs = lastWaterMs;
  portEXIT_CRITICAL(&cmdMux);

  bool stale = (millis() - waterMs > cfg::BAD_CONN_DELAY_MS);

  if (stale && !bad_conn_mode) {
    // TRIP: no fresh water for the whole window -> force-stop, latch.
    bad_conn_mode = true;
    bad_conn_count++;
    logError("Bad connection mode activated - pump force stopped");
    pump_stop();
    setBlinkInterval(cfg::BLINK_BADCONN_MS);
  } else if (!stale && bad_conn_mode) {
    // RECOVER: fresh data resumed -> clear latch, back to normal.
    bad_conn_mode = false;
    logWarning("Bad connection mode recovered");
    setBlinkInterval(cfg::BLINK_NORMAL_MS);
  }
}

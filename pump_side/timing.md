# ESP32 Water Project: Timer & Scheduling Reference

This document lists **all scheduled and timed tasks** in the ESP32 water project, with details on their intervals, triggers, and purposes.

---

## 1. NTP Polling (Time Synchronization)

### Fast Poll (ntpFastPoll)
- **Interval:** Every 2 seconds (when active)
- **Setup:** `scheduleNtpFast()` → `timer_ntp.every(2000, ntpFastPoll)`
- **Purpose:**
  - Rapidly attempts to synchronize time with NTP servers after boot or when time is not yet synced.
  - Cycles through all configured NTP servers and fallback IPs.
  - Stops when a successful sync occurs (then switches to slow poll).

### Slow Poll (ntpSlowPoll)
- **Interval:** Every 5 minutes (after initial sync)
- **Setup:** `scheduleNtpSlow()` → `timer_ntp.every(5 * 60 * 1000, ntpSlowPoll)`
- **Purpose:**
  - Periodically re-syncs time with NTP servers to maintain accurate time.
  - Cycles through all servers in a round-robin fashion.

---

## 2. Pump & Overheat Timers

### Auto Pump Start (fill_up)
- **Interval:** Once, after 31 seconds (WDT_TIMEOUT + 1)
- **Setup:** `timer_1.in(timer_1_delay, fill_up)` (currently commented out)
- **Purpose:**
  - Automatically starts the pump after boot (if enabled).

### Overheat Protection (overheat)
- **Interval:** Every 20 minutes (while pump is running)
- **Setup:** `timer_2.in(timer_2_interval, overheat)`
- **Purpose:**
  - Triggers pump overheat protection, forcing the pump to stop if running too long.

### Overheat Recovery (recover_from_overheat)
- **Interval:** Every 10 minutes (after overheat protection)
- **Setup:** `timer_3.in(timer_3_interval, recover_from_overheat)`
- **Purpose:**
  - Allows the pump to recover from overheat protection after a cooldown period.

### Manual Pump Auto-Stop (stop_manual_pump)
- **Interval:** Once, after 5 minutes
- **Setup:** `timer_manual_pump.in(manual_pump_duration_ms, stop_manual_pump)`
- **Purpose:**
  - Automatically stops the pump after a manual run command.

---

## 3. Frontdoor Relay Timer

### Relay Reset (relay_reset)
- **Interval:** Once, after 300 ms
- **Setup:** `timer_relay.in(relay_open_interval, relay_reset)`
- **Purpose:**
  - Resets all frontdoor relay GPIOs to HIGH after a button press (up/down/stop), simulating a momentary relay action.

---

## 4. Bad Connection Detection

### Bad Connection Mode (go_bad_conn_mode)
- **Interval:** Once, after 1 minute (if not reset)
- **Setup:** `timer_bad_connection.in(timer_bad_connection_delay, go_bad_conn_mode)`
- **Purpose:**
  - If the system does not receive expected updates, enters "bad connection mode" and forces the pump to stop for safety.

---

## 5. Status LED Blinking

### Blink LED (blink_f)
- **Interval:**
  - Normal: Every 1000 ms
  - Overheat: Every 250 ms
  - Bad Connection: Every 50 ms
- **Setup:** `timer_blink.every(interval, blink_f)` (interval set by `set_timer_blink_interval_to()`)
- **Purpose:**
  - Provides visual feedback of system state via onboard LED.

---

## 6. SSE Status Push (Web UI Updates)

### Debounced Status Push (sendStatusSse)
- **Interval:** Debounced, max once per second
- **Setup:** `timer_events.in(delayMs, statusDeferred)` (in `scheduleStatusPush()`)
- **Purpose:**
  - Sends status and log updates to the web UI via Server-Sent Events (SSE), but never more than once per second to avoid flooding.

---


## 7. Pre-fill Water Level

### Pre-fill Check (isTimeInRange)
- **Interval:** Every 10 minutes
- **Setup:** `timer_ntp.every(timer_ntp_interval, isTimeInRange)`
- **Purpose:**
  - Checks if the current time is within the pre-fill window and triggers water level checks accordingly.
  - Now active in the code.

### Auto Pump Start After Boot (fill_up)
- **Interval:** Once, after 31 seconds (WDT_TIMEOUT + 1)
- **Setup:** `timer_1.in(timer_1_delay, fill_up)`
- **Purpose:**
  - Automatically starts the pump after boot (now active).

----

## Summary Table

| Interval/Delay      | Function/Task             | Purpose/Trigger                        |
|---------------------|--------------------------|----------------------------------------|
| every 2 sec         | ntpFastPoll              | Fast NTP retry until success           |
| every 5 min         | ntpSlowPoll              | Periodic NTP resync                    |
| after 31 sec        | fill_up                  | Auto pump start after boot             |
| every 20 min        | overheat                 | Pump overheat protection               |
| every 10 min        | recover_from_overheat    | Recover from overheat                  |
| after 5 min         | stop_manual_pump         | Stop manual pump                       |
| after 300 ms        | relay_reset              | Reset frontdoor relay                  |
| after 1 min         | go_bad_conn_mode         | Enter bad connection mode              |
| every X ms          | blink_f                  | Blink status LED (interval varies)     |
| max 1/sec           | sendStatusSse            | SSE status/log push (debounced)        |
| every 10 min        | isTimeInRange            | Pre-fill check                         |

----

**Note:** All timers use the [arduino-timer](https://github.com/contrem/arduino-timer) library for non-blocking scheduling.

# wd_project — self-hosted MQTT + Home Assistant (Docker)

Stands up **Mosquitto** (MQTT broker) and **Home Assistant Container** on your Linux/Docker
box (`zz0004.local`) to replace Blynk. Both ESP32 boards and HA talk to this broker.

```
deploy/
├── docker-compose.yml
├── mosquitto/
│   ├── config/mosquitto.conf      # broker config (auth on, anon off)
│   ├── config/passwd              # created by you (step 2) — NOT committed
│   ├── data/                      # broker persistence (gitignored)
│   └── log/                       # broker logs (gitignored)
└── homeassistant/                 # HA /config volume (created on first run)
```

---

## 0. Copy this folder to the Linux device

From your Mac (in the repo root):

```bash
rsync -av deploy/ youruser@zz0004.local:~/wd_project/deploy/
# or: scp -r deploy youruser@zz0004.local:~/wd_project/
```

Then SSH in and `cd ~/wd_project/deploy`.

---

## 1. Pull images

```bash
docker compose pull
```

## 2. Create the MQTT user (`wd_mqtt`)

The broker requires a username/password (anonymous is disabled). Create the password
file **before** the first full start. Run the password tool inside a throwaway
Mosquitto container so you don't need it installed on the host:

```bash
# creates mosquitto/config/passwd with user "wd_mqtt" (you'll be prompted for a password)
docker run --rm -it \
  -v "$PWD/mosquitto/config:/mosquitto/config" \
  eclipse-mosquitto:2 \
  mosquitto_passwd -c /mosquitto/config/passwd wd_mqtt
```

> The file is created owned by the container's mosquitto user. If the broker later logs
> a permissions warning about `passwd`, run: `chmod 0700 mosquitto/config/passwd`.

To add more users later, drop the `-c` flag:
`... mosquitto_passwd /mosquitto/config/passwd anotheruser`

## 3. Start everything

```bash
docker compose up -d
docker compose logs -f mosquitto      # confirm "mosquitto version 2.x running"
docker compose logs -f homeassistant  # first boot takes a minute
```

## 4. Verify the broker (from the Linux box)

```bash
# subscribe to everything (leave running in one terminal)
docker exec -it wd-mosquitto mosquitto_sub -u wd_mqtt -P 'YOUR_PASSWORD' -t 'wd/#' -v

# publish a test message from another terminal
docker exec -it wd-mosquitto mosquitto_pub -u wd_mqtt -P 'YOUR_PASSWORD' -t 'wd/test' -m 'hello'
```

You should see `wd/test hello` in the subscriber. Once the ESP32s are flashed you'll see
`wd/tower/state/water` (~1 Hz) and `wd/pump/state/status` here.

## 5. Configure Home Assistant

1. Open `http://zz0004.local:8123` (or `http://<host-ip>:8123`) and create your HA account.
2. Settings → Devices & Services → **Add Integration** → **MQTT**.
3. Broker: `localhost`  Port: `1883`  Username: `wd_mqtt`  Password: *(your password)*.
   (HA uses `network_mode: host`, so the broker is reachable at `localhost:1883`.)
4. MQTT discovery is on by default (prefix `homeassistant`). Once the boards are flashed,
   two devices — **WD Tower (4F)** and **WD Pump (1F)** — appear automatically with their
   entities grouped.

---

## ESP32 connection settings (`arduino_secrets.h` on each board)

```cpp
#define SECRET_MQTT_HOST  "192.168.1.xx"   // ← use the NUMERIC IP of zz0004, not the .local name
#define SECRET_MQTT_PORT  1883
#define SECRET_MQTT_USER  "wd_mqtt"
#define SECRET_MQTT_PASS  "YOUR_PASSWORD"
```

> **Important — use the numeric IP, not `zz0004.local`, for the ESP32 boards.**
> ESP32 + PubSubClient does not resolve mDNS (`.local`) hostnames by default, so point the
> firmware at the host's numeric LAN IP. Find it on the Linux box with `hostname -I`.
> (HA ↔ broker uses `localhost` and is unaffected.)

---

## Resetting / tearing down

```bash
docker compose down            # stop, keep data
docker compose down -v         # stop and remove anonymous volumes (config/data on disk persist)
```

Broker data and HA config live in the bind-mounted folders, so they survive recreation.

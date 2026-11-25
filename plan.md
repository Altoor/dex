# dex — firmware plan (v2)

`dex` is firmware for the **Waveshare ESP32-S3-Touch-ePaper-1.54** (V1 and V2).
This milestone turns the device from a bare "hello world" screen into a small
always-on companion for the **Rolodex** backend: it shows the next todo, records
voice notes with the BOOT button, posts them to a configurable dynamic API, and
queues everything while offline. Power and e-paper refresh are treated as scarce
resources.

The old milestone (hello world + Wi-Fi provisioning) is complete and remains the
foundation.

## 1. Goals

1. **Next todo** on the home screen, fetched from `GET /api/v1/todos`.
2. **Checkmark** on the home screen marks the todo done with a debounced
   `PATCH /api/v1/todos/:id`, leaving a short window to undo.
3. **Voice notes:** a BOOT-button tap starts recording (max 60 s); another tap
   stops and posts the recording to the configured dynamic API. PWR cancels.
4. **Offline queue:** recordings made without Wi-Fi are stored and sent in the
   order they were made as soon as the network returns. Queue draining does not
   wait for a previous request's final status.
5. **Status:** a static hourglass in the top row while a call is in flight or a
   request is still pending; the final result is shown on the web settings page.
6. **Configurable Rolodex connection:** server URL, standard API key, dynamic
   API name, dynamic API key, and the `state` string.
7. **Battery:** show a battery indicator in the top row while charging and
   always show battery voltage/percentage on the web settings page.
8. **Power:** stay on, but idle cheaply. No background loop runs more often than
   every 2 minutes; API work happens right after actions or on demand.
9. **QR/settings on demand:** the SoftAP, captive DNS and HTTP settings server
   are only up while the QR/settings screen is visible.
10. **Power button:** single click refreshes the screen and runs checks (API
    first, then render); double click toggles the QR/settings screen. A long
    press powers the device off, and a long press while off powers it on.

## 2. Input map

| Input | Idle | Recording | Settings/QR | Factory reset |
| --- | --- | --- | --- | --- |
| **PWR** single click | run checks, then redraw | cancel recording | close QR/settings | — |
| **PWR** double click | open QR/settings | — | close QR/settings | — |
| **PWR** long hold (>3 s) | power off | cancel + power off | power off | part of combo |
| **BOOT** single click | start recording | stop + enqueue | — | part of combo |
| **Both** held 15 s | factory reset | factory reset | factory reset | — |
| **Touch** gear | open QR/settings | cancel? (no) | close | — |
| **Touch** checkmark | toggle todo (debounced) | — | — | — |

The side buttons stay primary; touch is a shortcut. BOOT is no longer a settings
button. The 15-second combo is unchanged.

Single vs double PWR is resolved in the button task with a ~350 ms window, so a
single click is emitted only after the double-click window expires.

## 3. Rolodex API contract

Base URL is user-configured, e.g. `http://192.168.1.40:4000` (never hard-coded to
localhost). All requests send `Authorization: Bearer <key>`.

### Todos (standard key)

```
GET   <base>/api/v1/todos?done=false&limit=1&includeArchived=false
      -> 200 { "items": [ { "id": 12, "name": "...", "description": "...",
                            "done": false, "doByDate": "...", ... } ],
               "pagination": { ... } }

PATCH <base>/api/v1/todos/12
      Content-Type: application/json
      { "done": true }
      -> 200 { "todo": { ... } }
      -> 404 when the todo is gone
```

The device shows `items[0]`. A PATCH is sent after a 3 s debounce; toggling the
checkmark again inside that window cancels the PATCH.

### Dynamic API (per-endpoint key)

```
POST  <base>/api/hooks/<slug>
      Content-Type: multipart/form-data; boundary=...
      state=<configured state>
      files=@recording.wav;type=audio/wav
      -> 202 { "accepted": true, "requestId": "...", "runId": "...",
               "statusUrl": "<base>/api/hooks/<slug>/requests/<id>",
               "websocketUrl": "..." }

GET   <base>/api/hooks/<slug>/requests/<requestId>
      -> 200 { "requestId", "runId",
               "status": "accepted|running|awaiting_input|completed|failed|cancelled",
               "result": <any JSON or null>, "error": <string|null>,
               "steps": [ ... ], ... }
```

Limits from the backend: 25 MB per file, 20 files, 5 MB JSON. Recordings are
mono 16 kHz 16-bit WAV, so a 60 s note is ~1.9 MB, well inside the limit.
`requestId` is stored and `statusUrl` is rebuilt from the configured base URL
(the server's `Host` header may be a proxy/localhost value).

## 4. Hardware

| Function | Pins |
| --- | --- |
| E-paper | PWR 6 (active low), BUSY 8, RST 9, DC 10, CS 11, SCK 12, MOSI 13 |
| Buttons | BOOT 0, PWR 18 |
| Touch (FT6336) | SDA 47, SCL 48, RST 7, INT 21, addr 0x38 |
| ES8311 codec | I2C addr 0x18 on SDA 47 / SCL 48 |
| I2S mic | MCLK 14, BCLK 15, WS 38, DIN 16 (DOUT 45 unused) |
| Audio power | GPIO 42 (active low); PA_CTRL GPIO 46 |
| microSD (1-bit SDMMC) | CLK 39, CMD 41, D0 40 |
| Battery ADC | GPIO 4 (200K/200K divider, `VBAT = 2 × VADC`) |
| Battery latch | GPIO 17 (HIGH = stay powered) |

Audio is initialized with Espressif's `espressif/esp_codec_dev` managed component
(ES8311 + I2S). The recording path configures 16 kHz, 16-bit, stereo I2S and
extracts the left channel to mono PCM.

## 5. Component architecture

```
main/
  app_main.c          boot order, power latch, task startup
components/
  board/              pins, power rails, button task (single/double/long)
  battery/            ADC oneshot + charging/USB detection
  i2c_bus/            one shared I2C master for touch + codec
  storage/            mount microSD at /sdcard, report availability
  recorder/           ES8311 + I2S capture, WAV, SD-first queue + RAM fallback
  net_client/         HTTP calls, FIFO sender, status polling
  app_config/         NVS settings (server/keys/dynamic API) + last result
  web_server/         HTTP API + settings page; start/stop on demand
  wifi_manager/       STA/AP, DNS; AP only while settings are shown
  ui/                 screens, refresh policy, debounce logic
  epaper/, gfx/, qrcodegen/, touch_panel/   (existing)
```

### Boot order

1. `board_init()`: configure rails and **immediately raise GPIO 17** to latch
   battery power.
2. `nvs_flash_init`, `app_config_init`, `storage_init` (SD optional),
   `battery_init`, `epd_init`.
3. `i2c_bus_init`, then `wifi_manager_init` (STA only; **no automatic AP**),
   `recorder_init`, `net_client_init`.
4. `web_server_init` (creates the server handle but does not start listening),
   then `ui_start`.
5. First refresh: run API checks (todo + status), then render the home screen.
   If nothing is configured, render the "configure me" state.

## 6. Settings and storage

`app_config` owns NVS namespace `dexcfg`:

| Key | Meaning | Default |
| --- | --- | --- |
| `srv_url` | base URL, no trailing slash | empty → configure |
| `std_key` | standard API key | empty |
| `dyn_slug` | dynamic API name (path segment) | empty |
| `dyn_key` | dynamic API key | empty |
| `dyn_state` | multipart `state` value | `"Voice note from dex"` |

Wi-Fi credentials stay in the existing `dex` NVS namespace. `POST /api/settings`
treats an empty key field as "keep the existing key", so the page can render
masked values (`ak_H3…oKdw`) without ever round-tripping secrets. The user can
clear a key with an explicit `clear_*` boolean.

Factory reset erases the whole NVS partition, as today.

## 7. Recording pipeline

- **Format:** WAV, PCM, mono, 16 kHz, 16-bit (44-byte header + PCM).
- **Max length:** 60 s, enforced by the recorder task.
- **SD first (durability):** when an SD card is mounted, the recorder writes
  directly to `/sdcard/dex/rec-<seq>.wav.part` from the first sample. The file is
  finalised (header + rename to `.wav`) when recording stops. This keeps queued
  recordings across power loss and resets.
- **RAM fallback:** only when no SD card is mounted does the recorder capture
  into a growable internal-RAM buffer (starting at 64 KB, growing while heap is
  available). RAM recordings survive until reboot and are uploaded/discarded as
  usual; a power loss loses them, which cannot be avoided without SD.
- **Ordering:** sequence numbers in the filename (`rec-000123.wav`), allocated
  by scanning the directory at boot and then incrementing. FIFO drain = lowest
  number first. RAM items are appended after SD items in creation order.
- **Delete:** after the POST returns **202**, the file/buffer is deleted. The
  user explicitly asked to free memory/space once sent; final processing status
  is tracked by `requestId` independently.
- **Cancel:** PWR cancels an in-progress recording, discards the buffer, and
  deletes a partial SD file. PWR does not cancel already-queued recordings.

### Audio init notes

- `BOARD_AUDIO_PIN_PWR` (42) is driven low before codec init (active-low rail).
- The shared I2C bus is created once by `i2c_bus` and reused by touch and codec.
- `esp_codec_dev` opens the ES8311 in ADC mode with 16 kHz / 16-bit / 2 channels;
  the recorder converts each stereo frame to one mono sample before writing.
- I2S RX uses MCLK 14 / BCLK 15 / WS 38 / DIN 16 with MCLK = 256 × fs.
- If the codec is absent (non-Touch variant or init failure), recording is
  disabled and the UI says so; all other features keep working.

## 8. Upload queue, status and polling

`net_client` owns a worker task. It exposes non-blocking "kick" calls; the UI
never waits on the network.

**Sending recordings (FIFO):**

1. A kick is raised on: Wi-Fi `GOT_IP`, recording stop, PWR single click, and the
   2-minute timer.
2. If online and configured, the worker takes the oldest queued recording and
   POSTs it. On **202**, it stores `requestId`, deletes the file, and immediately
   continues with the next recording **without waiting for that request's final
   status**.
3. On network/server error, it stops draining and retries on the next kick; the
   recording stays queued.

**Status polling:**

- Outstanding request IDs are kept in a small ring (max 8).
- The worker polls `GET .../requests/<id>` at most once every 2 minutes, and
  immediately after an action (post-success, PWR refresh). It never polls in a
  tight loop.
- Terminal states: `completed`, `failed`, `cancelled`. The latest terminal
  result/error is stored in `app_config` (truncated) and exposed to the web page.
- The home-screen hourglass reflects `inflight_http || outstanding_requests`. It
  is a static glyph; the e-paper is redrawn only when this boolean changes.

**Todo:**

- Fetch on boot, on Wi-Fi connect, on PWR single click, after a PATCH, and on the
  2-minute timer.
- PATCH is debounced 3 s in the UI; toggling again cancels it.
- A failed PATCH restores the previous todo state and shows a short error line.

## 9. Scheduling and power

- **No tight loops.** The only periodic work is a 2-minute timer (network
  refresh/status) and the existing 15–50 ms input polling/recording I2S reads.
- **Event-driven UI:** redraw only on state transitions (screen change, todo
  change, recording start/stop, hourglass on/off, battery bucket change, Wi-Fi
  status change). No animated spinner and no timed full refresh.
- **Wi-Fi:** `WIFI_PS_MIN_MODEM` power save while connected.
- **CPU:** enable dynamic frequency scaling (`esp_pm_configure`, max 160 MHz,
  min 40 MHz). Automatic light sleep is intentionally left off initially because
  it can interfere with the USB-Serial/JTAG console and recording latency; it can
  be enabled later if the board behaves.
- **Settings power-down:** leaving the QR screen stops the HTTP server and, if it
  was started just for settings, stops the SoftAP and captive DNS.
- **Power latch:** GPIO 17 is raised in `board_init`. A PWR-only hold (>3 s)
  shows `Power off`, lowers GPIO 17 and enters deep sleep with PWR as the wake
  source (on battery this cuts power; on USB it parks in deep sleep). Holding
  PWR wakes/reboots and the normal boot flow runs checks again.
- **Battery:** read GPIO 4 with the ADC oneshot driver (curve-fitting
  calibration, 12 dB attenuation), `VBAT = 2 × VADC`, map 3.3–4.2 V to 0–100 %.
  "Charging" is inferred from `usb_serial_jtag_is_connected()` and a high/rising
  pack voltage. Top-row icon appears while charging; the web page always shows
  voltage, percent and charging state. Battery is sampled on the 2-minute tick,
  on PWR refresh, and when opening settings.

## 10. Screens

### Home (200 × 200)

```
[gear]  dex                         [hourglass] [battery]
────────────────────────────────────────────────────────
  NEXT TODO
  <todo name, wrapped, up to ~4 lines at scale 2>
  <due date / "no due date">
                                        [ ✓ ]  <- checkmark box
────────────────────────────────────────────────────────
  BOOT: record   PWR: sync / 2× QR
  <status line: recording / uploading / queued / error>
```

- No todo: `All clear` plus a hint.
- Not configured: `Set server + key` and `2× PWR for setup`.
- Recording: `Recording 0:12 / 1:00` and `BOOT stop · PWR cancel`.
- Queued/offline: `N queued · will send`.
- Hourglass: two stacked triangles drawn with `gfx_fill_rect`, static.
- Checkmark: 24 × 24 hitbox; filled when the todo is optimistically done.
- Battery: small 16 × 8 outline with fill bars; only while charging/USB.

### Settings/QR

Keep the existing QR layout. Add a short footer showing the queue count and the
last upload status (`sent`, `done`, `failed`). The SoftAP/DNS/server come up when
this screen opens and stop when it closes.

### Reset

Unchanged.

## 11. Web settings page

The page gains a **Rolodex** section and a **Device status** section:

- Server URL (e.g. `http://192.168.1.40:4000`).
- Standard API key (masked, blank keeps existing).
- Dynamic API name/slug.
- Dynamic API key (masked, blank keeps existing).
- `state` string.
- Save via `POST /api/settings`.
- Read-only status: Wi-Fi, IP, battery %, charging, SD mounted, queued
  recordings, last request id/status/result/error (pretty-printed and
  truncated). Poll `/api/status` every 10 s while the page is open.

New endpoints (added to the existing ones):

```
GET  /api/settings   -> masked config + configured flags + defaults
POST /api/settings   -> { server_url, standard_key, dynamic_slug,
                          dynamic_key, state, clear_standard_key,
                          clear_dynamic_key }
GET  /api/status     -> existing Wi-Fi fields + battery, sd, queue, last_result
```

The page is only reachable while the QR screen is up, as requested.

## 12. Build and config

- `main/idf_component.yml`: `espressif/esp_codec_dev: "^1.3.4"`.
- `sdkconfig.defaults` additions:
  - `CONFIG_FATFS_LFN_HEAP=y`, `CONFIG_FATFS_MAX_LFN=255`
  - `CONFIG_PM_ENABLE=y` (DFS only; light sleep stays off)
  - `CONFIG_MBEDTLS_CERTIFICATE_BUNDLE=y` for user-supplied HTTPS servers
  - `CONFIG_ESP_HTTP_CLIENT_ENABLE_HTTPS=y`
- Tick rate stays at the IDF default (100 Hz); recording and debounce use
  millisecond timers, so no finer tick is needed.
- Keep the 4 MB flash layout and 2.9 MB app partition. Recordings live on SD,
  not flash.
- `components/*/CMakeLists.txt` updated with new requirements.

## 13. Milestones

- **M1 — plumbing:** `app_config`, `i2c_bus`, `storage`, `battery`; board power
  latch + new button events; web server start/stop; Wi-Fi AP/DNS start/stop.
- **M2 — todos:** `net_client` todo fetch/PATCH, home-screen todo + checkmark,
  debounce/undo, hourglass.
- **M3 — audio:** `recorder` (ES8311/I2S/WAV, SD-first queue, RAM fallback, 60 s cap, cancel),
  BOOT start/stop, queue listing.
- **M4 — hooks:** multipart POST, FIFO drain, request status polling, last-result
  storage, web settings section.
- **M5 — power/UI polish:** PWR single/double/long, battery indicator, refresh
  policy, PWR-triggered checks (API first, then screen), README/plan polish.

## 14. Validation checklist

1. Builds for `esp32s3` with no warnings. **Verified.**
2. Boot with no config: home shows "Not configured"; no AP/server until QR.
3. Double-click PWR opens QR; AP + server appear; page loads; closing stops both.
4. Save server/key/dynamic API; settings survive reboot.
5. Online: home shows the next todo; checkmark PATCHes after 3 s; undo cancels.
6. BOOT records; second BOOT posts multipart; hourglass appears/disappears.
7. Result appears on the web status section (immediately as `accepted`, then the
   terminal status/result after polling).
8. Offline: record 2–3 notes; reconnect; they upload in order and files vanish.
9. PWR single click runs checks and redraws only after the API result.
10. PWR long press powers off; holding it again boots and refreshes.
11. Battery % on the web page; top-row icon while USB/charging.
12. Both buttons 15 s still factory-reset.

Items 2–12 require the board and are not exercisable in this environment.

## 15. Risks / out of scope

- **No hardware in this environment:** ES8311 init, SD mount, ADC and power-latch
  behavior cannot be exercised here; only compilation is verified.
- **Long recordings without SD:** internal RAM caps a note at a few seconds.
  SD is required for the full 60 s. This is documented in the UI and README.
- **Charging detection** is inferred (USB Serial/JTAG + voltage) because the
  ETA6098 `STAT` line is not exposed as a GPIO.
- **WebSocket status** is not used; polling matches the "no more than every
  2 minutes" rule and is simpler. The HTTP status endpoint is equivalent.
- **PSRAM** stays disabled so one binary works on V1 and V2. Audio uses internal
  RAM plus SD.
- Out of scope: OTA, mDNS, WPA2 SoftAP, playback/TTS, partial e-paper refresh.

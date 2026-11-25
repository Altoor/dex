# dex

Firmware for the **Waveshare ESP32-S3-ePaper-1.54** (V1 and V2).

The firmware is an always-on companion for the **Rolodex** backend: it shows the
next todo on a 200 × 200 e-paper screen, records voice notes with the BOOT
button, posts them to a configurable dynamic API, and queues everything while
offline. Power and e-paper refreshes are treated as scarce resources.

## What it does

- Home screen shows the next open todo from `GET /api/v1/todos`, with a
  checkmark that marks it done (debounced, with a short undo window).
- **BOOT** tap starts a voice recording (max 60 s); another tap stops it and
  uploads it. **PWR** cancels a recording.
- Recordings are queued and sent in the order they were made. Uploading the next
  recording does not wait for the previous request to finish processing.
- Recordings are written to **microSD** first so they survive power loss; without
  a card they fall back to RAM (roughly 12 s of audio).
- Files are deleted from the card/RAM once the upload has been accepted.
- **PWR** single click runs all checks (API first), then redraws. **PWR** double
  click toggles the QR/settings screen. Touch: gear opens/closes settings, the
  checkmark on the home screen completes the todo.
- Settings page configures server URL, standard API key, dynamic API name,
  dynamic API key, and the `state` string sent with each recording. It also
  shows device status, battery, queue length, and the last dynamic API result.
- The SoftAP, captive DNS and settings HTTP server run **only** while the
  QR/settings screen is visible.
- Battery indicator in the top row while charging; battery voltage/percentage
  always shown on the settings page.
- Hold **PWR for 3 s** to power off; hold it again while off to power on
  (on battery the power latch cuts the board completely).
- Hold **BOOT + PWR for 15 seconds** to erase all settings and reboot.

See [plan.md](plan.md) for the design and roadmap.

## Hardware

Waveshare ESP32-S3-Touch-ePaper-1.54 (the firmware also works on the non-touch
variant and disables touch when the FT6336 does not answer). The firmware does
not need PSRAM and uses a 4 MB flash layout, so the same binary works on V1 and
V2 boards.

A **microSD card (FAT32)** is recommended: it makes the recording queue durable
across resets and power loss, and holds the full 60-second recordings.

| Signal | GPIO |
| --- | --- |
| EPD_PWR | 6 |
| EPD_BUSY | 8 |
| EPD_RST | 9 |
| EPD_DC | 10 |
| EPD_CS | 11 |
| EPD_SCK | 12 |
| EPD_MOSI | 13 |
| PWR button | 18 |
| BOOT button | 0 |
| Touch SDA / SCL | 47 / 48 |
| Touch RST / INT | 7 / 21 |
| Codec I²C (ES8311) | 0x18 on SDA 47 / SCL 48 |
| I²S MCLK / BCLK / WS / DIN | 14 / 15 / 38 / 16 |
| Audio power / PA | 42 (active low) / 46 |
| microSD CLK / CMD / D0 | 39 / 41 / 40 (1-bit SDMMC) |
| Battery ADC | 4 (200K/200K divider) |
| Power latch | 17 (held high) |


## Build

Install **ESP-IDF 5.5.x** for the `esp32s3` target, then activate the ESP-IDF
environment and build. Pick the snippet for your shell:

```bash
# bash / zsh
. $HOME/esp/esp-idf/export.sh
idf.py set-target esp32s3
idf.py build
```

```fish
# fish
source $HOME/esp/esp-idf/export.fish
idf.py set-target esp32s3
idf.py build
```

`export.sh` is a bash/zsh script; sourcing it from fish fails with
`Unsupported use of '='`. Use `export.fish` in fish instead.

**Troubleshooting activation:** if the export step fails with
`openocd-esp32 ... libusb-1.0.so.0: cannot open shared object file`, install the
libusb runtime and retry:

```bash
sudo apt install libusb-1.0-0   # Debian/Ubuntu
```

## Using the device

1. Power on. The screen shows the next todo, or a setup hint if the device has
   no Rolodex settings yet.
2. Double-click **PWR** (or tap the gear) to open the settings screen with a QR
   code.
3. If the device has no Wi-Fi yet, join the open **`dex-XXXX`** network shown on
   the screen. The captive portal usually opens the settings page; otherwise
   scan the QR code or browse to **http://192.168.4.1/**.
4. Pick a network, enter the password, and tap **Connect**. The device reboots
   and joins the network.
5. On the same settings page, fill in the **Rolodex** section: server URL (for
   example `http://192.168.1.40:4000`), standard API key, dynamic API name
   (slug), dynamic API key, and the state string. Save.
6. Close the settings screen (double-click **PWR**). The device fetches the next
   todo and shows it on the home screen.
7. Tap **BOOT** to record, tap it again to stop and upload. Tap the checkmark to
   complete the current todo; you have about three seconds to tap again and
   undo before the update is sent.
8. Single-click **PWR** to run a manual refresh (drains the queue, polls pending
   statuses, then re-fetches the todo before redrawing).
9. To wipe everything, hold **BOOT + PWR together for 15 seconds**. The screen
   shows a factory-reset message and the device reboots.

## Repository layout

| Path | Purpose |
| --- | --- |
| `main/` | Application entry point and boot order |
| `components/board/` | Pins, power rails/latch, button gestures |
| `components/epaper/` | SSD1681 SPI driver and refresh LUT |
| `components/gfx/` | 1-bpp canvas, text, shapes, QR, hourglass, battery |
| `components/touch_panel/` | FT6336 I²C touch controller |
| `components/i2c_bus/` | Shared I²C master bus (touch + codec) |
| `components/storage/` | microSD mount (optional, 1-bit SDMMC) |
| `components/battery/` | Battery ADC and charging/USB detection |
| `components/recorder/` | ES8311 + I²S capture, WAV, queue (SD first, RAM fallback) |
| `components/app_config/` | NVS settings and last API result |
| `components/net_client/` | Todos, PATCH, multipart upload, status polling |
| `components/qrcodegen/` | Nayuki QR Code generator (MIT) |
| `components/wifi_manager/` | NVS credentials, STA/AP, scan, captive DNS |
| `components/web_server/` | HTTP API and embedded mobile settings page |
| `components/ui/` | Screen state machine, debounce, power/QR lifecycle |

## Licenses

Project code is MIT licensed (see `LICENSE`). Third-party notices:

- `components/qrcodegen/` — Nayuki QR Code generator, MIT.
- `components/gfx/font5x7.h` — Adafruit GFX classic 5×7 font, BSD.

## Flash the device

1. Connect the board over USB-C.
2. Find the serial port it appeared as:

   **Linux** — list the serial devices, then unplug/replug the board and run it
   again to see which one appears:

   ```sh
   find /dev -maxdepth 1 \( -name 'ttyACM*' -o -name 'ttyUSB*' \) -ls
   ```

   This works in bash, zsh, and fish. Native USB shows up as `/dev/ttyACM0`; a
   USB-UART bridge as `/dev/ttyUSB0`.

   > **fish users:** `ls /dev/ttyACM* /dev/ttyUSB*` fails with `No matches for
   > wildcard` when a pattern matches nothing, because fish treats unmatched
   > globs as an error. Add fish's null-glob qualifier, e.g.
   > `ls -l /dev/ttyACM*(N) /dev/ttyUSB*(N)`, or use the `find` command above.

   If `idf.py` reports `Path '/dev/ttyACM0' is not readable`, you are not in the
   `dialout` group. Check with `id -nG | grep dialout`, then add yourself:

   ```bash
   sudo usermod -aG dialout "$USER"
   ```

   Start a fresh login session afterwards (log out and back in, or reboot; a new
   terminal alone may not pick up the group). To avoid logging out, run
   `newgrp dialout` in the current shell, then source the export script again:

   ```fish
   newgrp dialout
   source $HOME/esp/esp-idf/export.fish
   idf.py -p /dev/ttyACM0 flash monitor
   ```

   **macOS** — list the callout devices and look for `usbmodem` or `usbserial`:

   ```bash
   ls /dev/cu.*
   ```

   Use the matching path, e.g. `/dev/cu.usbmodem1101`.

   **Windows (PowerShell)** — list the COM ports:

   ```powershell
   Get-CimInstance Win32_SerialPort | Select-Object DeviceID, Description
   ```

   Or check **Device Manager → Ports (COM & LPT)**. Use the number you see,
   e.g. `COM5`.

3. Flash and open the monitor using the port from step 2:

   ```bash
   # bash / zsh
   . $HOME/esp/esp-idf/export.sh
   idf.py -p /dev/ttyACM0 flash monitor
   ```

   ```fish
   # fish
   source $HOME/esp/esp-idf/export.fish
   idf.py -p /dev/ttyACM0 flash monitor
   ```

   Replace `/dev/ttyACM0` above with your device (Linux `/dev/ttyUSB0`,
   macOS `/dev/cu.usbmodemXXXX`, Windows `COM5`, …). You can also let ESP-IDF
   guess with `idf.py flash monitor` if only one board is connected.
4. If flashing fails, hold **BOOT**, tap **RST**, release **BOOT** to enter the
   download mode, then retry.
5. Quit the monitor with `Ctrl+]`.

On WSL2, the board must be attached to WSL first, then it appears as
`/dev/ttyACM0` or `/dev/ttyUSB0` inside WSL. List and attach it from PowerShell:

```powershell
usbipd list
usbipd attach --wsl --busid <BUSID>
```

Run `usbipd list` again after attaching to confirm the state is `Attached`, then
use the same `find` command in WSL to locate the device path:

```sh
find /dev -maxdepth 1 \( -name 'ttyACM*' -o -name 'ttyUSB*' \) -ls
```

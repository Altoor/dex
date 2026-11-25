# dex

Firmware for the **Waveshare ESP32-S3-ePaper-1.54** (V1 and V2).

The first milestone renders a clean home screen, exposes a settings screen from
the PWR button, and lets a phone configure Wi-Fi through a QR code and a
locally-hosted web page.

## What it does

- 200 × 200 e-paper home screen with **hello world** and a gear icon.
- Short press **PWR** (or **BOOT**) toggles the settings screen; tapping the
  gear on the touch panel does the same.
- Settings screen shows a QR code for the device's settings page.
- With no saved Wi-Fi, the device hosts an open **`dex-XXXX`** SoftAP with a
  captive portal at **http://192.168.4.1/**.
- The settings page scans nearby networks, saves credentials to NVS, and
  reboots the device onto the selected network.
- Once connected, the QR code points at the device's LAN IP instead.
- Hold **BOOT + PWR for 15 seconds** to erase all settings and reboot into
  setup mode.

See [plan.md](plan.md) for the design and roadmap.

## Hardware

Waveshare ESP32-S3-Touch-ePaper-1.54 (the firmware also works on the non-touch
variant and disables touch when the FT6336 does not answer). The firmware does
not need PSRAM and uses a 4 MB flash layout, so the same binary works on V1 and
V2 boards.

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
| Touch SDA | 47 |
| Touch SCL | 48 |
| Touch RST | 7 |
| Touch INT | 21 |

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

1. Power on. The screen shows `hello` / `world` and a gear.
2. Short-press **PWR** (or tap the gear) to open the settings screen with a QR
   code.
3. If the device has no Wi-Fi yet, join the open **`dex-XXXX`** network shown on
   the screen. The captive portal usually opens the settings page; otherwise
   scan the QR code or browse to **http://192.168.4.1/**.
4. Pick a network, enter the password, and tap **Connect**.
5. The device reboots, joins the network, and shows the SSID and IP on the home
   screen.
6. Later, press **PWR** and scan the QR code while on the same LAN to reopen
   settings. Use **Forget network** to return to AP mode.
7. To wipe everything, hold **BOOT + PWR together for 15 seconds**. The screen
   shows a factory-reset message and the device reboots into AP mode.

## Repository layout

| Path | Purpose |
| --- | --- |
| `main/` | Application entry point and boot order |
| `components/board/` | Pins, power rails, debounced button task |
| `components/epaper/` | SSD1681 SPI driver and refresh LUT |
| `components/gfx/` | 1-bpp canvas, 5×7 text, shapes, gear, QR drawing |
| `components/touch_panel/` | FT6336 I²C touch controller |
| `components/qrcodegen/` | Nayuki QR Code generator (MIT) |
| `components/wifi_manager/` | NVS credentials, STA/AP, scan, captive DNS |
| `components/web_server/` | HTTP API and embedded mobile settings page |
| `components/ui/` | Screen state machine and rendering |

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

# ESP32-S3 Camera Firmware for M5Stack Unit CamS3-5MP

Custom ESP-IDF firmware for the **M5Stack Unit CamS3-5MP** board. Streams MJPEG
video over HTTP, integrates with [Frigate NVR](https://frigate.video) and
[Home Assistant](https://www.home-assistant.io) via MQTT, and supports
over-the-air firmware updates triggered from an MQTT broker.

[![Build](https://github.com/hbentel/M5Stack-Unit-CamS3-5MP/actions/workflows/build.yml/badge.svg)](https://github.com/hbentel/M5Stack-Unit-CamS3-5MP/actions/workflows/build.yml)
[![License](https://img.shields.io/badge/license-Apache%202.0-blue.svg)](LICENSE)

---

## About this fork

This fork adds compatibility with [DuetWebControl](https://github.com/Duet3D/DuetWebControl)'s
[Duet Tool Align](https://github.com/jaysuk/duet-tool-align) plugin, which expects both a live
MJPEG stream and a single-JPEG snapshot from the *same* origin/port:

- `capture_handler` (the existing snapshot logic behind `GET /`) now also sends
  `Cache-Control: no-store, no-cache, must-revalidate` and `Pragma: no-cache`, and is additionally
  registered at **`GET /snapshot`**.
- The existing `/stream` handler (already async — it hands off to a per-client worker task and
  returns immediately, see `mjpeg_client_worker_task`) is now also registered on the **port-80**
  server, alongside the original port-81 stream server. Both routes are unaffected.
- The broadcaster's frame copy into the shared pool buffer is now bounds-checked (was an
  unchecked `memcpy`, harmless in practice at every resolution this board actually produces, but
  a real overflow risk if a frame ever exceeded the pool's buffer size).
- **Brightness/contrast/saturation/white-balance are now on `/setup`** (previously MQTT-only, so
  tuning image quality required running a broker). Also corrected `apply_camera_settings()`'s
  defaults, which were quietly applying the sensor's *darkest*/least-saturated setting at every
  boot rather than neutral — see the commit for why (mega_ccm.c register indices, not the generic
  OV-sensor `-2..2` convention).
- Those four fields have their own **`POST /setup/image`**, wired to a second submit button
  ("Apply Now") on the same form: applies live with no reboot, since unlike cam_res/jpeg_qual
  they don't need `esp_camera_init()` to run again -- and deliberately doesn't persist to NVS on
  its own, since a flash write must never happen while the camera's DMA pipeline is running (see
  `config_mgr.h`). Included in the next full "Save & Restart" regardless of which form set them
  last, including from MQTT (previously an MQTT-set value could vanish on the next reboot).

With this, point the plugin's "Camera bridge URL" at `http://<device-ip>` (or `http://<device-id>.local`)
with no port suffix. All upstream features (MQTT, OTA, Frigate/Home Assistant integration, BLE
provisioning) are untouched — see below.

Everything else in this README describes the upstream project as-is.

---

## Quick Start

1. **Flash** — `bash build.sh && bash flash.sh`
2. **Provision Wi-Fi** — on first boot the device advertises as `PROV_unitcams3` over BLE; open the [Espressif BLE Provisioning](https://apps.apple.com/app/esp-ble-provisioning/id1473590141) app, enter your Wi-Fi credentials
3. **Configure** — open `http://<device-ip>/setup` in a browser; set your MQTT broker URL and device ID, then submit to save and reboot
4. **Stream** — point Frigate (or any MJPEG client) at `http://<device-ip>:81/stream`

The device IP is printed to the serial monitor on boot and is shown on the `/setup` page.

---

## Hardware

**Supported board: M5Stack Unit CamS3-5MP**

- SoC: ESP32-S3 (dual-core Xtensa LX7 @ 240 MHz)
- Sensor: GalaxyCore PY260 (2MP / 5MP output via JPEG compression)
- Flash: 16 MB QIO
- PSRAM: 8 MB Octal (OPI) — 40 MHz

> **Other boards are not supported without pin remapping.** The pin constants in
> `main/main.c` are hardcoded for the M5Stack Unit CamS3-5MP schematic.

---

## Features

- **Zero-copy MJPEG streaming** over HTTP (port 81 `/stream`) — up to 5 simultaneous
  clients; atomic reference counting means all workers share one PSRAM buffer with no
  per-client copy; pull-based pacing so each client runs at its own natural rate
- **Optimized snapshots** over HTTP (port 80 `/`) — reuses the live stream frame when
  the broadcaster is active; zero hardware contention with the stream
- **MQTT telemetry** — RSSI, uptime, heap, PSRAM, FPS, active stream count, error counters every 10 s
- **Production reliability** — internal SRAM task stacks, 16KB main stack, and
  automatic Watchdog recovery (30s timeout)
- **Home Assistant auto-discovery** — sensor, number, and button entities on connect
- **Camera image controls** via MQTT — brightness, contrast, saturation, white balance
- **URL-based OTA** — publish firmware URL to MQTT; optional token auth and SHA-256 integrity verification before flashing
- **Recovery manager** — NVS boot-loop detection, 2-minute health timer, OTA rollback
- **Core dump to flash** — download crash dumps via `GET /api/coredump`
- **Web log viewer** — `GET /api/logs` returns the full boot and runtime log from a 16 KB PSRAM ring buffer
- **mDNS** — device is reachable at `<device-id>.local` (e.g. `unitcams3.local`) in addition to its IP
- **BLE Wi-Fi provisioning** — no hardcoded credentials; first-boot BLE setup

---

## Prerequisites

- **ESP-IDF v5.3.2 LTS** *(stable, recommended)* — run `bash setup_idf.sh` to install automatically,
  or follow the [manual installation guide](https://docs.espressif.com/projects/esp-idf/en/v5.3.2/esp32s3/get-started/)
  **or ESP-IDF v6.0** *(beta)* — must be installed manually to `~/esp/esp-idf-v6`; see [IDF v6 build notes](#idf-v60-beta) below (`setup_idf.sh` handles v5 only)
- Python 3.10+ (v5 scripts), Python 3.11 (v6 managed component toolchain)
- **Espressif BLE Provisioning** app — [iOS](https://apps.apple.com/app/esp-ble-provisioning/id1473590141) / [Android](https://play.google.com/store/apps/details?id=com.espressif.provble)
- An MQTT broker on your local network (e.g., Mosquitto)

---

## Pre-built Firmware

Each [GitHub Release](https://github.com/hbentel/M5Stack-Unit-CamS3-5MP/releases)
includes firmware binaries built by GitHub Actions directly from the tagged source:

| File | Use |
|------|-----|
| `unitcams3_merged.bin` | **Recommended** — single file, flash at offset `0x0` |
| `unitcams3_firmware.bin` | App partition only (OTA updates) |
| `bootloader.bin`, `partition-table.bin`, `ota_data_initial.bin` | Individual regions |

### Flash the merged binary (no build required)

Put the device in [Download Mode](#2-download-mode-m5stack-unit-cams3-5mp), then:

```bash
pip install esptool
esptool.py --chip esp32s3 -b 460800 \
  --before default_reset --after hard_reset \
  write_flash --flash_mode dio --flash_freq 80m --flash_size 16MB \
  0x0 unitcams3_merged.bin
```

### Verify provenance (optional)

Every release binary is cryptographically attested by GitHub Actions. To confirm
the binary you downloaded was built from the published source — not modified by
anyone — install the [GitHub CLI](https://cli.github.com/) and run:

```bash
gh attestation verify unitcams3_merged.bin \
  --repo hbentel/M5Stack-Unit-CamS3-5MP
```

A passing result means the binary is provably linked to a specific Actions run
and source commit. No trust in the uploader required.

---

## Build & Flash

### 1. Clone

```bash
git clone https://github.com/hbentel/M5Stack-Unit-CamS3-5MP.git esp32camera
cd esp32camera
```

### 2. Download Mode (M5Stack Unit CamS3-5MP)

This board has **no BOOT or RESET buttons**. A normally-running device can be
flashed without manual intervention — esptool resets it automatically. However,
if the device is crash-looping, brand new, or otherwise unresponsive, you must
enter ROM download mode manually using the 1×3 pin header on the PCB
(labelled **G / G0 / 3V3**):

1. **Unplug the USB-C cable.**
2. Bridge **G0** to **G** (GND) with a jumper wire.
3. **Plug in the USB-C cable** — the device powers on in download mode.
4. Remove the jumper wire.
5. Run the flash command (see below).
6. **Power-cycle after flashing** (unplug and replug USB-C). The native USB
   `hard-reset` does **not** re-sample the GPIO0 strapping pin, so the device
   stays in download mode until power is fully removed.

### 3. IDF v5.3.2 (stable, recommended)

Prerequisites: ESP-IDF v5.3.2 installed at `~/esp/esp-idf`.

```bash
bash build.sh          # build only
bash build.sh clean    # full clean rebuild
bash flash.sh          # flash (auto-detects /dev/cu.usbmodem*)
bash monitor.sh        # serial monitor — open in a separate terminal
```

The scripts activate the v5.3.2 environment automatically via `idf_env.sh`.

**Never pipe the flash script through `head`, `tail`, or any pipe.** esptool
erases all flash regions before writing any of them. If the process is killed
mid-write the partition table is lost and the device will not boot. Re-run
`bash flash.sh` immediately to recover.

### 4. IDF v6.0 (beta) <a name="idf-v60-beta"></a>

Prerequisites: ESP-IDF v6.0 installed at `~/esp/esp-idf-v6`; Python 3.11 venv
at `~/.espressif/python_env/idf6.0_py3.11_env`.

```bash
bash build_v6.sh          # build only
bash build_v6.sh clean    # full clean rebuild (removes build/ and sdkconfig)
bash monitor.sh           # serial monitor — open in a separate terminal
```

`build_v6.sh` activates the v6 environment internally, so no manual `source
export.sh` is needed. Flash with `idf.py` after the build (see below for the
flash command; `flash.sh` re-activates v5 and must not be used).

> **Switching between v5 and v6** cleans automatically if you pass `clean`, but
> for a quick switch just delete `build/` and `sdkconfig` manually — both are
> version-specific:
> ```bash
> rm -rf build sdkconfig && bash build.sh       # back to v5
> rm -rf build sdkconfig && bash build_v6.sh    # back to v6
> ```

**Flashing a crash-looping device with v6 esptool:**

If the device is crash-looping and cannot be caught by a normal reset, first
enter download mode (step 2 above), then flash with `--before no-reset
--no-stub`:

```bash
PYTHON=~/.espressif/python_env/idf6.0_py3.11_env/bin/python
PORT=$(ls /dev/cu.usbmodem* 2>/dev/null | head -1)

$PYTHON -m esptool --chip esp32s3 -p "$PORT" -b 460800 \
  --before no-reset --after hard-reset \
  write-flash --flash-mode dio --flash-freq 80m --flash-size 16MB \
  0x0     build/bootloader/bootloader.bin \
  0x8000  build/partition_table/partition-table.bin \
  0x11000 build/ota_data_initial.bin \
  0x20000 build/unitcams3_firmware.bin

# Unplug and replug USB-C to exit download mode.
```

`--no-stub` is required when the device is crash-looping (the stub loader
upload fails; the ROM bootloader handles flash directly).

### 5. Configure via browser

After flashing, no recompile is needed to configure the device. Open a browser and
navigate to:

```
http://<device-ip>/setup
```

| Field | Description | Default |
|-------|-------------|---------|
| MQTT URL | `mqtt://host` or `mqtt://host:port` | compile-time default |
| MQTT Username | Leave blank if broker has no auth | _(empty)_ |
| MQTT Password | Leave blank if broker has no auth | _(empty)_ |
| Device ID | MQTT topic prefix, HA entity prefix, and mDNS hostname (`<id>.local`) | `unitcams3` |
| OTA Token | If set, MQTT `ota/set` requires JSON `{"url":"...","token":"<this>"}` | _(empty — auth disabled)_ |
| Coredump Token | If set, `GET /api/coredump` requires `Authorization: Bearer <this>` | _(empty — auth disabled)_ |
| Enable MQTT | Toggle MQTT on/off | enabled |
| Camera Resolution | QVGA / VGA / HD / UXGA | VGA (640×480) |
| JPEG Quality | 1 (best) – 63 (lowest) | 12 |

Submit the form — the device saves the values to NVS and reboots. Settings persist
across power cycles and OTA updates.

> **Compile-time defaults** for MQTT URL, username, password, and device ID can be
> set via `idf.py menuconfig` → *UnitCamS3 Firmware Configuration*. These are used
> as fallbacks when no value has been saved via `/setup`.

---

## Wi-Fi Provisioning

On first boot (or after NVS erase) the device starts BLE advertising as
**`PROV_unitcams3`**.

1. Open the **Espressif BLE Provisioning** app on your phone(Available in the Apple App Store and Google Play Store).
2. Scan for `PROV_unitcams3` and tap it.
3. Enter your Wi-Fi credentials.
4. The device connects, stores credentials in NVS, and reboots — BLE is not active on subsequent boots.

To re-provision: erase NVS (see Troubleshooting below).

---

## mDNS

Once connected to Wi-Fi the device advertises itself via mDNS. The hostname is
the **Device ID** configured on the `/setup` page (default: `unitcams3`):

```
http://unitcams3.local/         # snapshot
http://unitcams3.local:81/stream  # MJPEG stream
```

Most home routers and operating systems support mDNS out of the box (macOS,
Windows 10+, iOS, Android, most Linux distros with Avahi). If you have two
cameras on the same network they must each have a unique Device ID — configure
one of them at `/setup` before mounting it.

---

## Frigate Integration

Add this camera block to your Frigate `config.yml`:

```yaml
cameras:
  unitcams3:
    ffmpeg:
      inputs:
        - path: http://<device-ip>:81/stream
          roles:
            - detect
            - record
      input_args: -f mjpeg         # must override global input_args
      hwaccel_args: []             # must override global hwaccel (QSV etc.)
    detect:
      width: 640
      height: 480
      fps: 5
```

> The `/stats` endpoint reports actual camera FPS — call it twice 5–10 s apart
> for an accurate reading.

### go2rtc restream (recommended)

Routing the camera through go2rtc enables Frigate's timeline/VOD playback
(VOD requires HLS segments from go2rtc, not a direct MJPEG stream):

```yaml
go2rtc:
  streams:
    unitcams3:
      - "ffmpeg:http://<device-ip>:81/stream#video=h264"

cameras:
  unitcams3:
    ffmpeg:
      hwaccel_args: []
      inputs:
        - path: rtsp://127.0.0.1:8554/unitcams3
          input_args: preset-rtsp-restream
          roles:
            - detect
            - record
    detect:
      width: 640
      height: 480
      fps: 5
```

---

## MQTT & Home Assistant

The firmware publishes Home Assistant MQTT discovery messages on every connect.
All topics use the **Device ID** as a prefix (default: `unitcams3`).

### Telemetry topics (published every 10 s)

| Topic | Value |
|-------|-------|
| `unitcams3/status` | `ON` / `OFF` (LWT) |
| `unitcams3/rssi` | Wi-Fi signal in dBm |
| `unitcams3/uptime` | Uptime in seconds |
| `unitcams3/heap` | Free internal heap in bytes |
| `unitcams3/psram_free` | Free PSRAM in bytes |
| `unitcams3/fps` | Camera FPS (delta since last publish) |
| `unitcams3/no_soi` | Cumulative NO-SOI frame errors |
| `unitcams3/jpeg_drops` | Cumulative dropped frames |
| `unitcams3/recovery_count` | Cumulative recovery events |
| `unitcams3/streams` | Active MJPEG stream clients |
| `unitcams3/ota_status` | `idle` / `pending_reboot` |

### Command topics (subscribe)

| Topic | Payload | Effect |
|-------|---------|--------|
| `unitcams3/brightness/set` | `0` to `8` | Set sensor brightness |
| `unitcams3/contrast/set` | `0` to `6` | Set sensor contrast |
| `unitcams3/saturation/set` | `0` to `6` | Set sensor saturation |
| `unitcams3/wb_mode/set` | `0`–`4` | White balance (0=Auto, 1=Sun, 2=Cloud, 3=Office, 4=Home) |
| `unitcams3/fps_cap/set` | `0`–`15` | Broadcaster FPS cap (0 = unlimited); not persisted |
| `unitcams3/led/set` | `1` / `0` | LED on / off |
| `unitcams3/restart` | _(any)_ | Trigger `esp_restart()` |
| `unitcams3/reprovision` | _(any)_ | Wipe Wi-Fi credentials and reboot into BLE provisioning |
| `unitcams3/ota/set` | JSON (see OTA section) | Trigger OTA update |

---

## HTTP Endpoints

| Port | Path | Description |
|------|------|-------------|
| 80 | `GET /` | Single JPEG snapshot |
| 80 | `GET /setup` | Browser configuration page |
| 80 | `POST /setup` | Save configuration and reboot |
| 80 | `GET /health` | JSON: uptime, heap, PSRAM, JPEG drops, `app_sha256`, `reset_reason` |
| 80 | `GET /stats` | JSON: camera FPS counters, Wi-Fi RSSI, memory |
| 80 | `GET /api/coredump` | Download last crash dump (ELF format); requires `Authorization: Bearer <token>` if Coredump Token is configured |
| 80 | `GET /api/logs` | Full boot + runtime log as plain text (16 KB PSRAM ring buffer) |
| 81 | `GET /stream` | MJPEG stream (used by Frigate) |

### Setup Page
<img width="910" height="821" alt="Screenshot 2026-03-06 at 9 30 31 AM" src="https://github.com/user-attachments/assets/1c5bb01d-a9c8-4c9a-956a-bdb8f7a98845" />

```bash
curl http://<device-ip>/health
curl http://<device-ip>/stats
curl -o /tmp/snap.jpg http://<device-ip>/
curl http://<device-ip>/api/logs    # full boot + runtime log
```

The `reset_reason` field in `/health` shows why the device last rebooted:
`power_on`, `software`, `panic`, `task_watchdog`, etc. Useful for diagnosing
unexpected restarts without needing a serial monitor.

---

## OTA Updates

### Serve the firmware binary

```bash
# In the build directory, serve the binary on port 8080
cd build && python3 -m http.server 8080
```

### Trigger OTA via MQTT

**Without a token configured** (default — open):
```bash
mosquitto_pub -h <broker-ip> -q 1 \
  -t unitcams3/ota/set \
  -m "http://<your-ip>:8080/unitcams3_firmware.bin"
```

**With a token configured** (recommended):
```bash
SHA=$(shasum -a 256 build/unitcams3_firmware.bin | cut -c1-64)
mosquitto_pub -h <broker-ip> -q 1 \
  -t unitcams3/ota/set \
  -m "{\"url\":\"http://<your-ip>:8080/unitcams3_firmware.bin\",\"token\":\"<your-token>\",\"sha256\":\"$SHA\"}"
```

Use `-q 1` (QoS 1) so the broker retries delivery if the device briefly
disconnects at the moment of publish. QoS 0 messages are silently lost on reconnect.

The `sha256` field is optional but recommended — the device verifies the full firmware
image against it before writing to flash, catching truncated downloads or MITM attempts.

The device saves the URL and hash to RTC RAM (plain SRAM writes — safe while camera DMA
is running), publishes `unitcams3/ota_status: pending_reboot`, then reboots.
On the next boot, OTA runs *before* the camera initializes. If the download or
verification fails, the device returns to normal operation; retrigger via MQTT.

### Verify the flash succeeded

```bash
BUILD_SHA=$(shasum -a 256 build/unitcams3_firmware.elf | cut -c1-64)
DEVICE_SHA=$(curl -s http://<device-ip>/health | \
  python3 -c "import sys,json; print(json.load(sys.stdin)['app_sha256'])")
[ "$BUILD_SHA" = "$DEVICE_SHA" ] && echo "MATCH" || echo "MISMATCH"
```

`/health` embeds the SHA-256 of the ELF at build time via `esp_app_get_description()`.

---


## Troubleshooting

### Cannot flash / `No serial data received` / device crash-looping
**Symptom:** esptool prints `Failed to connect: No serial data received` or
the USB port disappears and reappears rapidly.
**Cause:** The device is crash-looping and the USB CDC interface is unstable.
esptool cannot catch it during the ~1–2 s window before the next crash.
**Fix:** Enter ROM download mode manually — see [Download Mode](#2-download-mode-m5stack-unit-cams3-5mp) above.
Use `--before no-reset --no-stub` when flashing in this state.
After flashing, unplug and replug USB-C; the device will not exit download mode
from a software reset alone.

### NO-SOI flood (camera produces corrupt frames)
**Symptom:** `no_soi` counter climbs rapidly in `/stats` or MQTT.
**Cause:** XCLK frequency is too high. The PY260 vertical front porch is ~3 μs at
10 MHz; higher XCLK causes the ISR to miss the sync window.
**Fix:** XCLK is already hardcoded to 10 MHz in `main/main.c`. Do not increase it.
If you see NO-SOI errors, check that `CAM_XCLK_FREQ_HZ` is `10000000`.

### Device not advertising BLE / already provisioned
**Symptom:** App cannot find `PROV_unitcams3`.
**Cause:** Wi-Fi credentials are stored in NVS from a previous provisioning.
**Fix:** Erase NVS — connect to serial monitor and hold the reset button, or:
```bash
idf.py erase-flash    # erases everything; re-flash after
```
Or erase only the `nvs` partition if you want to keep the firmware:
```bash
esptool.py --chip esp32s3 erase_region 0x9000 0x8000
```

### OTA not triggering / device ignores MQTT message
**Symptom:** Published URL to `unitcams3/ota/set` but device did not reboot.
**Cause:** MQTT may not be connected, or the device ID is different.
**Fix:** Check the serial monitor (`bash monitor.sh`) for the OTA log line:
`OTA requested — saving URL to RTC RAM and rebooting`.
Verify `unitcams3/status` is `ON` in your broker.

### Device enters safe mode after repeated OTA attempts
**Symptom:** `recovery_mgr` logs `BOOT LOOP DETECTED` and services don't start.
**Cause:** Previously, every reboot (including OTA-triggered ones) incremented the
NVS boot counter. This is fixed — `ota_mgr_start_url()` now calls
`recovery_mgr_signal_planned_reboot()` before rebooting, which resets the counter
on the next boot via an RTC NOINIT flag.

---

## Architecture Notes

### XCLK 10 MHz ceiling
The PY260's vertical front porch is approximately 3 μs at 10 MHz. At 16 MHz it
shrinks to ~1.875 μs, causing double-exception crashes; at 20 MHz it is ~1.5 μs
(96% NO-SOI flood). **10 MHz is a physical sensor ceiling — do not increase it.**

### Core 1 ISR requirement
The VSYNC ISR must run on Core 1. Wi-Fi IRQs run on Core 0 and can delay an ISR
assigned there past the front porch, flooding the stream with NO-SOI errors. The
ISR is installed via `esp_ipc_call_blocking(1, ...)` in `cam_hal.c`.

### Flash write safety rule
Any flash write (NVS commit, OTA partition write) disables the OPI PSRAM cache.
If the camera GDMA is writing to PSRAM at that moment, a cache-disabled memory
access causes an ExcCause=7 panic. **Never write flash while the camera is
running.** OTA runs before `esp_camera_init()` for this reason. The OTA URL is
passed across the reboot via `RTC_NOINIT_ATTR` (plain SRAM, no flash).

### RTC NOINIT vs RTC DATA
`RTC_NOINIT_ATTR` places data in `.rtc_noinit` — a `(NOLOAD)` section the
bootloader skips on every reset. Values survive `esp_restart()`.
`RTC_DATA_ATTR` (`.rtc.data`) is re-copied from the flash image on every
non-deep-sleep reset and is only suitable for deep-sleep persistence.

---

## Attribution

The `components/esp32-camera/` component is a fork of
[espressif/esp32-camera](https://github.com/espressif/esp32-camera)
(Apache License 2.0, Copyright 2019 Espressif Systems), modified to support
the PY260 sensor and to pin the VSYNC ISR to Core 1.

---

## License

Apache License 2.0 — see [LICENSE](LICENSE).

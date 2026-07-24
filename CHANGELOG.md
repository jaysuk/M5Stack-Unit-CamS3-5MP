# Changelog

All notable changes to this project will be documented here.
Format follows [Keep a Changelog](https://keepachangelog.com/en/1.1.0/).

---

## [v0.3.0] — 2026-07-24

**This release adds compatibility with [RepRapFirmware](https://github.com/Duet3D/RepRapFirmware)
toolchangers via [DuetWebControl](https://github.com/Duet3D/DuetWebControl)'s
[Duet Tool Align](https://github.com/jaysuk/duet-tool-align) plugin** — this board can now serve
as the plugin's camera source for automated, in-browser XY tool-offset alignment. Everything
upstream (Frigate/Home Assistant/MQTT, OTA, BLE provisioning) is untouched; this is additive.

### Added

- **`GET /snapshot`** — same handler as `GET /`, registered at the path the Duet Tool Align plugin
  (and `duet-webcam-bridge` before it) expects from a camera bridge, with
  `Cache-Control: no-store, no-cache, must-revalidate` + `Pragma: no-cache` added.
- **`GET /stream` on port 80** — the existing async stream handler (already hands off to a
  per-client worker task rather than blocking, see `mjpeg_client_worker_task`) is now also served
  on the main port-80 server, alongside the original port-81 stream server. This means Duet Tool
  Align only needs one bridge URL, no port suffix, for both endpoints.
- **Brightness/Contrast/Saturation/White Balance on `/setup`** — previously reachable only via
  MQTT, so tuning image quality for machine-vision use required running a broker.
- **`POST /setup/image`** — a second, no-reboot submit button for just those four fields. Applies
  immediately (same live sensor writes MQTT uses); deliberately does not persist to NVS on its own
  since a flash write must never happen while the camera's DMA/capture pipeline is running (see
  `config_mgr.h`) — included in the next full "Save & Restart" regardless of which path set it
  last (`/setup/image`, `/setup`, or MQTT).

### Fixed

- **Unbounded `memcpy` in the broadcaster** — the frame copy into the shared pool buffer had no
  capacity check. Not hit in practice (measured JPEG sizes are well within the 512KB buffers at
  every supported resolution), but nothing stopped an unusually large frame from overflowing the
  buffer and corrupting adjacent PSRAM heap. Now dropped with a log line instead.
- **Wrong "neutral" defaults** — `apply_camera_settings()` applied
  `brightness = contrast = saturation = 0` at every boot as if that were neutral. For this sensor
  (mega_ccm.c register indices, not the generic OV-sensor `-2..2` range), `0` is the
  *darkest/least-saturated* setting — every boot was quietly running close to the dimmest, flattest
  image the sensor could produce. Neutral is 4/3/3.
- **MQTT-set image values reverting** — brightness/contrast/saturation/wb_mode set via MQTT were
  applied live but never remembered anywhere, so they'd silently revert to whatever was last saved
  via `/setup` on the next reboot. Now updates `config_mgr`'s in-memory state too (same safe,
  no-flash-write pattern as `/setup/image` above).

---

## [v0.2.7] — 2026-06-23

### Fixed

- **OTA rollback-confirmation crash** — `esp_ota_mark_app_valid_cancel_rollback()`
  was deferred via a 2-minute health timer, by which point Wi-Fi/MQTT were already
  active. The resulting `otadata` flash erase disables the OPI PSRAM cache; two
  coredumps confirmed the Wi-Fi task itself was corrupted by the cache-disable
  race, even with the camera not yet initialized — proving the hazard isn't
  camera-GDMA-specific. Fix: moved the confirmation into `recovery_mgr_init()`,
  in the same pre-Wi-Fi window the boot-loop NVS write has always used safely.
- **OTA SHA-256 hashing failure** — one-shot `psa_hash_compute()` over the
  ~1.3 MB firmware image failed with `PSA_ERROR_INSUFFICIENT_MEMORY` (only
  ~77 KB internal heap free at OTA time). Fix: hash incrementally in 16 KB
  chunks via `psa_hash_setup`/`update`/`finish`.
- **`/setup` POST body truncation** — `httpd_req_recv()` returned partial reads
  for bodies spanning multiple TCP segments, silently truncating form fields.
  Fix: loop until the full (capped) body is received.
- **Frame pool init leak** — a failed allocation partway through
  `frame_pool_init()` leaked the mutex/semaphore/already-allocated PSRAM
  buffers. Fix: clean up everything allocated so far on failure.
- **JPEG EOI search UB** — the backward `0xFF 0xD9` search in `cam_hal.c` built
  a pointer that could decrement past the start of the buffer when no marker
  was found. Fix: rewritten with bounded unsigned index arithmetic.

### Security

- **`/setup` XSS hardening** — MQTT URL/user/pass and device ID are now
  HTML-escaped before being embedded in the `/setup` page, preventing a
  malicious config value from injecting markup.

### Changed

- **`config_mgr` validation** — resolution/quality are now validated at every
  setter, not just NVS load; `config_mgr_save()` aborts without committing if
  any `nvs_set_*` call fails.
- **README** — go2rtc example switched to a direct passthrough restream
  (needed for Frigate VOD/timeline) instead of the ffmpeg transpose re-encode
  pipeline.

---

## [v0.2.6] — 2026-04-12

### Fixed

- **Frame pool exhaustion under concurrent snapshot + stream** — with 3 pool slots,
  a simultaneous `GET /` snapshot (holding a ref while sending ~150 KB over HTTP)
  combined with a worker mid-send on the previous frame exhausted all slots. The
  broadcaster stalled for up to ~5 s, causing Frigate's 20 s ffmpeg watchdog to fire
  and restart the capture thread. Fix: pool size increased from 3 → 5 slots
  (2.5 MB PSRAM). With 5 slots, snapshot(1) + worker(1) + broadcaster(1) still
  leaves 2 free slots, eliminating the exhaustion window entirely.

---

## [v0.2.5] — 2026-04-11

### Fixed

- **Recovery manager crash loop** — `recovery_mgr_init()` was called after Wi-Fi
  and MQTT were already running. Its `nvs_commit()` disables the OPI PSRAM cache;
  concurrent PSRAM access by the Wi-Fi task caused ExcCause=7 → crash → boot loop.
  Fix: moved `recovery_mgr_init()` to before `wifi_init_sta()`, while no
  PSRAM-accessing tasks are alive. Root cause confirmed via coredump analysis.
- **IDF v6 CI build failure** — `sha256_buffer()` used the legacy
  `mbedtls_sha256_*` context API removed in mbedTLS 4.0 (IDF v6). Replaced with
  `psa_hash_compute()` from the PSA Crypto API, compatible with both IDF v5
  (mbedTLS 3.x) and IDF v6 (mbedTLS 4.0).

---

## [v0.2.4] — 2026-04-10

### Security

- **OTA token authentication** — `unitcams3/ota/set` now accepts a JSON payload
  `{"url":"...","token":"...","sha256":"..."}`. If an OTA token is configured (via
  `/setup` or `CONFIG_UNITCAMS3_OTA_TOKEN`), the token field is required and
  verified before the OTA is initiated; mismatched or missing tokens are silently
  rejected. Empty token keeps legacy bare-URL behavior.
- **OTA SHA-256 verification** — the optional `sha256` JSON field (64 hex chars)
  is saved to `RTC_NOINIT_ATTR` alongside the URL. On the next boot, after the
  full firmware image is downloaded into a PSRAM buffer, its SHA-256 is computed
  and compared against the saved hash before any flash write occurs. Mismatch
  aborts the update.
- **Coredump Bearer auth** — `GET /api/coredump` now requires an
  `Authorization: Bearer <token>` header when a Coredump Token is configured (via
  `/setup` or `CONFIG_UNITCAMS3_COREDUMP_TOKEN`). Unauthenticated requests receive
  `401 Unauthorized` with a `WWW-Authenticate` challenge. Empty token leaves
  the endpoint open (default).
- **`/setup` token management** — OTA Token and Coredump Token fields added to the
  browser configuration page. Token values are never reflected in the HTML response
  (placeholder shows "(saved)" instead of the actual value). Submitting the form
  without touching a token field preserves the existing value.

### Added

- **Configurable broadcaster FPS cap** — `unitcams3/fps_cap/set` MQTT command
  (payload `0`–`15`; `0` = unlimited). Broadcaster sleeps the remainder of each
  frame interval; `CAMERA_GRAB_LATEST` discards stale frames while sleeping.
  HA `number` entity. Current value reflected in `/stats` as `fps_cap`.
- **LED control via MQTT** — GPIO 14 exposed as `unitcams3/led/set` (`1`/`0`).
  HA `light` entity. GPIO access kept in `main.c` via registered callback.
- **Wi-Fi re-provisioning via MQTT** — `unitcams3/reprovision` command clears NVS
  provisioning data and reboots into BLE provisioning mode.
- **`broadcast_fps` in `/stats`** — delta of broadcaster frame counter; distinct
  from `fps` (ISR VSYNC rate).
- **`frames_delivered` counter** — cumulative frames sent to all MJPEG clients since
  boot; exposed in `/stats`.
- **Long-term soak metrics** — `internal_min` and `psram_min` (minimum free since
  boot) tracked in `/stats`; `heap_min` published to MQTT every 10 s.
- **Task stack HWM** — broadcaster and HTTP task HWMs reported in `/stats` as
  `stack_hwm.broadcaster_words` / `http_task_words`; per-worker minimum in
  `worker_min_words`.

### Fixed

- **IDF v6 re-provisioning API** — `wifi_v6.c` updated to use
  `network_prov_mgr_reset_wifi_provisioning()` (v6 renamed from
  `network_prov_mgr_reset_provisioning()`); fixes v6 CI build failure.
- **IPC task stack overflow** — `CONFIG_ESP_IPC_TASK_STACK_SIZE` raised from 1280
  to 2048 bytes. The previous size caused a double-exception crash during
  `nvs_commit()` at boot when GPIO was initialized inside `mqtt_mgr`.
- **`esp_driver_gpio` removed from `mqtt_mgr`** — GPIO access moved to a
  registered callback in `main.c`, eliminating the dependency that changed binary
  layout and triggered the IPC stack overflow.

---

## [v0.2.3] — 2026-04-05

### Added

- **Zero-copy MJPEG delivery** — refactored `frame_pool` to use C11 atomic
  reference counting. Multiple clients now stream from the same PSRAM buffer
  simultaneously, reducing bus load by ~80%.
- **Worker reliability** — implemented `SO_SNDTIMEO` (10s) on stream sockets.
  Stalled or slow clients now fail gracefully without hanging worker tasks.
- **Optimized snapshots** — the `/` endpoint now grabs a reference to the active
  broadcaster frame if available, eliminating hardware contention during streams.

### Fixed

- **Watchdog safety** — re-enabled Task WDT Panic for production auto-recovery.
- **Broadcaster stability** — added Task WDT registration and removed artificial
  frame pacing; broadcaster now runs at max camera speed with pull-based worker pacing.
- **Memory safety** — increased main task stack to 16KB and system event stack
  to 4096B; forced all task stacks to internal SRAM to prevent flash-write crashes.

## [v0.2.3] — 2026-04-07

### Added

- **Zero-copy MJPEG streaming** — `frame_pool` now uses C11 `<stdatomic.h>` atomic
  reference counting. The broadcaster captures one frame; all worker tasks share a
  reference to the same PSRAM buffer. Eliminates 5 concurrent `memcpy` calls per
  frame, reducing PSRAM bus load by ~80%.
- **Pull-based pacing** — removed the artificial 100 ms broadcaster delay. The
  broadcaster runs at full camera speed (~9.4 fps); each client worker's TCP send
  latency acts as its own rate limiter. A slow Frigate client always gets the latest
  available frame via binary semaphore coalescing.
- **Unified snapshot** (`GET /`) — when the broadcaster is active, the snapshot
  endpoint grabs a reference to the current stream frame instead of calling
  `esp_camera_fb_get()` independently. Zero hardware contention with the stream.
- **`SO_SNDTIMEO` on worker sockets** (10 s) — a stalled client causes
  `httpd_resp_send_chunk` to time out and the worker to exit cleanly, freeing its
  slot and pool reference without hanging the device.
- **Broadcaster Task WDT** — `mjpeg_broadcaster_task` registered with Task Watchdog;
  triggers 30 s panic if the broadcaster hangs.
- **`IP_EVENT_STA_LOST_IP` handler** — device now reconnects if IP is lost silently
  due to DHCP failure (previously only reconnected on explicit disconnect events).
- **Stress test script** — `test_stream_stress.py` opens N simultaneous streams,
  reports per-client FPS, bad JPEG count, max frame gap, and device-side stats.

### Fixed

- **Wi-Fi TX power cap removed** — `esp_wifi_set_max_tx_power(32)` (8 dBm) was the
  primary cause of 105 Wi-Fi disconnects. Removing it restored full 20 dBm TX power.
  RSSI at AP improved from −56 dBm to −27 dBm; disconnect count dropped to 0.
- **Disconnect reason logged** — `WIFI_EVENT_STA_DISCONNECTED` now logs the reason
  code (`beacon_timeout`, `auth_expire`, etc.) for field diagnostics.
- **Reconnect backoff** — 1 s delay before `esp_wifi_connect()` retry prevents
  rapid reconnect storms against the AP.
- **OTA MQTT subscription QoS 0 → 1** — `unitcams3/ota/set` subscription now uses
  QoS 1 so the broker retries delivery after a brief MQTT reconnect; QoS 0 messages
  published during a disconnect window were silently lost.
- **Frame pool reduced 8 → 3 slots** — with atomic ref counting, peak simultaneous
  usage is 2 slots (broadcaster current + workers mid-send, all sharing one ref).
  3rd slot covers the transitional swap. Frees 2.5 MB PSRAM (4 MB → 1.5 MB).
- **Dead `STREAM_FRAME_INTERVAL_MS` define removed** — unused constant left over
  from pre-pull-pacing design.
- **Stale comment in `ota_mgr.c`** — updated frame pool size reference from
  `4×512KB` to `3×512KB`.

---

## [v0.2.2] — 2026-04-05

### Fixed

- **Production hardening** — re-enabled Task Watchdog Panic (`CONFIG_ESP_TASK_WDT_PANIC=y`) for auto-recovery from hangs.
- **Main task stack** — increased `main_task` stack size to 16KB to prevent startup overflows during heavy initialization.
- **Documentation** — formalized Memory Segregation rules in ARCHITECTURE.md.

## [v0.2.1] — 2026-04-05

### Added

- **Multi-client MJPEG streaming** — support for up to 5 simultaneous stream clients
  (e.g., Frigate `detect` + `record` + browser view) using asynchronous HTTP
  handlers and a dedicated broadcaster task.
- **`active_streams` metric** — added to `/stats` JSON and MQTT telemetry;
  includes Home Assistant auto-discovery as a new sensor.
- **Broadcaster idling** — `mjpeg_broadcaster_task` now idles when 0 clients are
  connected, preserving PSRAM bandwidth and reducing heat.
- **Python test script** — `test_mjpeg_concurrency.py` added for verifying
  multi-client performance.

### Fixed

- **Internal RAM for stacks** — forced all task stacks to internal SRAM to prevent
  Double Exception crashes during flash writes (NVS/OTA).
- **Startup stability** — increased main task stack to 16KB and system event stack
  to 4096B to prevent overflows during mDNS and MQTT initialization.
- **mDNS unique instances** — assigned unique instance names ("HTTP", "Stream")
  to prevent "Service already exists" errors on port 81.
- **Race condition fix** — delayed telemetry task until MQTT is connected to
  ensure all mutexes are initialized before use.
- **Thread-safe camera recovery** — added a mutex to `camera_reinit()` to prevent
  concurrent driver de-initialization crashes.
- **Frame pool capacity** — increased from 4 to 8 buffers (4MB total PSRAM) to
  smoothly handle multiple simultaneous consumers.

## [v0.2.0] — 2026-04-04

### Added

- **mDNS** — device advertises at `<device-id>.local` using the espressif/mdns
  managed component. Hostname tracks the Device ID set on `/setup`.
- **Web log viewer** (`GET /api/logs`) — 16 KB PSRAM ring buffer captures all
  `ESP_LOG*` output from boot onward; returned as plain text with ANSI colour codes.
  Implemented via `esp_log_set_vprintf()` hook with ISR-safe spinlock. Gracefully
  disabled if PSRAM is unavailable at boot.
- **`reset_reason` in `/health`** — JSON field showing why the device last rebooted
  (`power_on`, `software`, `panic`, `task_watchdog`, etc.).
- **Device ID tooltip on `/setup`** — explains that Device ID controls the MQTT
  topic prefix, Home Assistant entity prefix, and mDNS hostname (`<id>.local`).

### Fixed

- **Resolution enum mismatch** — `/setup` dropdown values now map to PY260-native
  framesizes only: QVGA (6), VGA (10), HD (13), UXGA (15). Previously the list
  included CIF and HVGA which silently fail on this sensor.
- **Invalid NVS cam_res recovery** — `config_mgr_init()` validates the stored
  resolution on boot and resets to VGA if the value is not in the supported set,
  preventing boot loops from stale NVS written by older firmware.
- **`mega_ccm` unsupported framesize** — added an `else` branch that logs
  `ESP_LOGE` and returns `-1` instead of silently succeeding.
- **MQTT HA discovery ranges** — corrected brightness to `0–8`, contrast and
  saturation to `0–6` to match the PY260 driver limits.
- **`assert()` in `ll_cam.c`** replaced with `ESP_LOGE` + `ESP_ERR_INVALID_ARG`
  return (assertions are compiled out in release builds).
- **Deinit ordering in `cam_hal.c`** — `ll_cam_deinit()` (frees ISR) now called
  before `vQueueDelete()`.
- **`CONFIG_CAMERA_TASK_STACK_SIZE` Kconfig** — default raised to 8192; now
  wired to `CAM_TASK_STACK` in `cam_hal.c`.
- **GDMA DMA buffer size range** extended to 65536 in Kconfig.
- **`DEFAULT_CAM_RES`** corrected from 8 (CIF) to 10 (VGA) in `config_mgr.c`.
- **`RECOVERY_ERR_RTSP_SEND` renamed** to `RECOVERY_ERR_STREAM_SEND`.
- **Orphaned `CONFIG_APP_UPDATE=y`** removed from `sdkconfig.defaults`.

### Build / CI

- GitHub Actions now builds both IDF v5.3.2 and IDF v6.0 on every push.
- Release artifacts include `unitcams3_merged.bin` (single-file flash) with
  build provenance attestation via `actions/attest-build-provenance`.

---

## [v0.1.0] — 2026-03-03

Initial public release.

### Features

- **Camera driver** — Forked ESP32-S3 camera driver tuned for the PY260 (`mega_ccm`)
  sensor on the M5Stack Unit CamS3-5MP. JPEG-only capture; all RAW/RGB/YUV paths removed.
- **ISR stability** — VSYNC ISR pinned to Core 1 via `esp_ipc_call_blocking`; full
  GDMA reset sequence prevents spurious double-VSYNC that truncated frames to 4096 bytes.
  XCLK hard-limited to 10 MHz (PY260 front-porch constraint).
- **Frame pool** — 4 × 512 KB PSRAM ring buffer decouples camera capture from HTTP delivery.
- **JPEG validation** — SOI/EOI byte-level check with atomic drop counters exposed via `/stats`.
- **HTTP server** (port 80) — `GET /` snapshot, `GET /health` (JSON, includes ELF SHA-256),
  `GET /stats`, `GET /api/coredump` (ELF coredump download).
- **MJPEG stream** (port 81) — `GET /stream` multipart MJPEG, compatible with Frigate NVR
  (`input_args: -f mjpeg`).
- **MQTT + Home Assistant discovery** — Telemetry every 10 s (RSSI, heap, FPS, drop counters);
  image controls (brightness, contrast, saturation, WB mode); reboot command.
- **BLE Wi-Fi provisioning** — First-boot BLE provisioning via `wifi_provisioning`; credentials
  stored in NVS. No hardcoded Wi-Fi credentials.
- **Runtime config (`/setup` page)** — Browser form to change MQTT URL/credentials, device ID,
  MQTT enable toggle, camera resolution, JPEG quality. Values persist in NVS; device restarts
  to apply.
- **OTA updates** — URL-based OTA triggered by MQTT (`unitcams3/ota/set`). Downloads full
  firmware to PSRAM, stops Wi-Fi, then flashes — avoids OPI cache-disable crash during
  concurrent camera DMA.
- **Recovery manager** — NVS boot-loop detection (threshold = 3), safe mode, 2-minute health
  timer, OTA rollback confirmation via `esp_ota_mark_app_valid_cancel_rollback`.
- **IDF v5 and v6 support** — `build.sh` targets IDF v5.3.2 LTS; `build_v6.sh` targets
  IDF v6 beta. Both auto-detect version mismatch via `CMakeCache.txt` and clean before rebuild.
- **CI** — GitHub Actions workflow builds firmware with IDF v5.3.2 on every push and PR.

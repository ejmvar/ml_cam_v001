# ML CAM v001: safe bootstrap

This repository now contains a conservative camera foundation for the
identified classic ESP32 silicon. After station-mode Wi-Fi obtains an IP, it
starts a local-LAN HTTP checkpoint server for fresh JPEG capture and a bounded
three-frame temporal analysis window. It does not stream video, use SD, write
flash evidence, act as a gateway, or enable ESP-NOW.

Periodic capture startup is deliberately ordered: `ml_camera_init()` creates
the camera and periodic mutexes but does not create the periodic task; after
Wi-Fi obtains an IP, the application starts HTTP first and then calls the
idempotent periodic-task start API. This prevents periodic capture from
competing with camera/HTTP startup. `/info` reports `periodic_started` and the
startup order. If task creation fails, fresh capture and HTTP remain usable and
`/capture/periodic/latest.jpg` returns a bounded 503 or 404 response.

## Quick path

1. Source the repository ESP-IDF environment: `source ./00-SOURCE-this.sh`.
2. Select the confirmed target: `idf.py set-target esp32`.
3. Build only: `idf.py build`.
4. Provision Wi-Fi only with `./scripts/provision-wifi-nvs.sh` after the
   hardware review is complete. The script flashes only the NVS partition.

### Live JPEG checkpoint

1. Application-flash only, reset, and monitor for the bounded
   `Wi-Fi connected and IP acquired` line. Copy only the device IP from that
   line; do not retain raw serial output.
2. Check readiness without secrets:

   ```bash
   curl --fail --silent --show-error --connect-timeout 3 --max-time 10 \
      'http://DEVICE_IP/health?mode=normal&quality=15'
   ```

    Expected JSON: `{"status":"ready","mode":"normal","quality":15}`.
3. Obtain and validate one fresh JPEG entirely through a pipe. This does not
   save the image in the repository:

   ```bash
   curl --fail --silent --show-error --connect-timeout 3 --max-time 10 \
      'http://DEVICE_IP/capture.jpg?mode=normal&quality=15' \
     | uv run python scripts/check-jpeg-http.py
   ```

    Expected response: `JPEG checkpoint passed: ... bytes; dimensions 320x240`.

### One-shot flash, reset, and HTTP verification

Run the complete bounded verification from the repository root:

```bash
./scripts/flash-reset-verify-http.sh
```

The script defaults to the stable FTDI path recorded below. Override only the
serial device when needed: `PORT=/dev/ttyUSB0 ./scripts/flash-reset-verify-http.sh`.
`POST_FLASH_TIMEOUT=5`, `READINESS_TIMEOUT=30`, `HTTP_TIMEOUT=10`,
`TCP_READINESS_TIMEOUT=30`, `TCP_READINESS_POLL_INTERVAL=1`, and
`READINESS_SETTLE_DELAY=1` are the defaults; all are bounded and configurable
within safe limits. After extracting the approved readiness marker, the script
performs a non-HTTP TCP connect gate to port 80. The gate polls at the bounded
interval until `TCP_READINESS_TIMEOUT` (maximum 60 seconds), suppressing raw
network errors. Only after TCP accepts a connection does the script apply the
small post-TCP grace delay before the first HTTP request. This separates the
serial marker from actual socket readiness without adding HTTP retries.

The sequence is intentionally one-shot: it performs one non-erasing full
`idf.py flash`, waits, performs one explicit esptool hard reset, then captures
only filtered safe readiness markers in a temporary file. The device IPv4
address is accepted only from the approved `Image checkpoint URL:
http://.../capture.jpg` marker. The marker is emitted only after the HTTP
server starts successfully. The temporary file is removed on exit.

After readiness, `wget` makes one request to each of `/health`,
`/capture.jpg`, `/analysis`, `/analysis/prev.jpg`, `/analysis/current.jpg`, and
`/analysis/next.jpg`. Every URL uses the explicit `mode=normal&quality=15`
query. `/health` must return the readiness JSON; `/analysis` is checked by
the JSON contract checker; every image body is piped to the JPEG checker and
never saved. This proves, respectively, HTTP readiness, the bounded temporal
JSON contract, and valid in-memory JPEG responses for the capture and all three
retained analysis frames. A failure reports its primary stage (flash,
reset/readiness, or wget endpoint) and does not retry blindly. If the approved
marker is absent or TCP port 80 never becomes ready, the primary failure stage
is `reset/readiness`, no endpoint request is issued, and the script stops. A TCP
gate success is not an HTTP response check; the six subsequent URLs still each
receive exactly one bounded `wget` request.

The `/capture.jpg` handler captures one frame in RAM, sends it, and releases
the camera frame even when the HTTP send fails. No SD, flash, gateway, or NVS
write is performed. The server starts only after Wi-Fi has acquired an IP.
This checkpoint has no authentication and is intended only for a trusted local
LAN; it is not production-safe and must not be exposed to an untrusted network.

The shared toolchain convention comes from `/W/NVT/espnow-master-slave`. The
repository wrapper delegates to `scripts/idf-env.sh`, which selects the local
ESP-IDF v6.0.2 environment when available.

### Image endpoint contract

Every endpoint is listed below. All image and analysis URLs accept only
`quality=10..30`; invalid values return HTTP 400. `/health` accepts the same
range and reports the selected mode and quality without capturing.

| Method and URL | Response | Mode | Expected dimensions | Quality |
| --- | --- | --- | --- | --- |
| `GET /health` | JSON readiness | query-bound | n/a | 10..30 |
| `GET /capture.jpg` | fresh-first JPEG; cache fallback for continuity | normal | 320x240 | 10..30 |
| `GET /capture/qqvga.jpg` | fresh-first JPEG; cache fallback for continuity | normal | 160x120 | 10..30 |
| `GET /capture/hqvga.jpg` | fresh-first JPEG; cache fallback for continuity | normal | 240x176 | 10..30 |
| `GET /capture/qvga.jpg` | fresh-first JPEG; cache fallback for continuity | normal | 320x240 | 10..30 |
| `GET /capture/qqvga/latest.jpg` | cache-only JPEG | normal | 160x120 | 10..30 |
| `GET /capture/hqvga/latest.jpg` | cache-only JPEG | normal | 240x176 | 10..30 |
| `GET /capture/qvga/latest.jpg` | cache-only JPEG | normal | 320x240 | 10..30 |
| `GET /capture/latest.jpg` | cache-only JPEG alias | normal | 320x240 | 10..30 |
| `GET /capture/periodic/latest.jpg` | latest RAM-only periodic JPEG | normal | 320x240 | task-dependent |
| `GET /analysis` | analysis JSON | query-bound | retained JPEGs: normal 320x240; transformed 40x24; decoded grid 40x30 | 10..30 |
| `GET /analysis/prev.jpg` | retained JPEG | query-bound | normal 320x240; transformed 40x24 | 10..30 |
| `GET /analysis/current.jpg` | retained JPEG | query-bound | normal 320x240; transformed 40x24 | 10..30 |
| `GET /analysis/next.jpg` | retained JPEG | query-bound | normal 320x240; transformed 40x24 | 10..30 |
| `GET /ml/grayscale.jpg` | JPEG | grayscale | 40x24 | 10..30 |
| `GET /ml/reduced-colors.jpg` | JPEG | reduced_colors | 40x24 | 10..30 |
| `GET /ml/grayscale/analysis` | analysis JSON | grayscale | retained JPEGs 40x24; decoded grid 40x30 | 10..30 |
| `GET /ml/reduced-colors/analysis` | analysis JSON | reduced_colors | retained JPEGs 40x24; decoded grid 40x30 | 10..30 |
| `GET /ml/grayscale/analysis/prev.jpg` | retained JPEG | grayscale | 40x24 | 10..30 |
| `GET /ml/grayscale/analysis/current.jpg` | retained JPEG | grayscale | 40x24 | 10..30 |
| `GET /ml/grayscale/analysis/next.jpg` | retained JPEG | grayscale | 40x24 | 10..30 |
| `GET /ml/reduced-colors/analysis/prev.jpg` | retained JPEG | reduced_colors | 40x24 | 10..30 |
| `GET /ml/reduced-colors/analysis/current.jpg` | retained JPEG | reduced_colors | 40x24 | 10..30 |
| `GET /ml/reduced-colors/analysis/next.jpg` | retained JPEG | reduced_colors | 40x24 | 10..30 |

For query-bound endpoints, `mode=normal|reduced_colors|grayscale` is accepted;
the default is `mode=normal&quality=15`. Exact examples:

```text
http://DEVICE_IP/capture.jpg?mode=normal&quality=15
http://DEVICE_IP/capture.jpg?mode=grayscale&quality=20
http://DEVICE_IP/analysis?mode=reduced_colors&quality=15
http://DEVICE_IP/analysis/prev.jpg?mode=reduced_colors&quality=15
```

`/health` never captures, analyzes, or processes an image. `/analysis` captures
three sequential frames and commits the retained window only after all three
complete. Retained image URLs require the matching successful analysis call
first, with the same mode and quality; otherwise they return HTTP 409 instead
of silently returning the wrong image.

Normal capture endpoints always try a bounded fresh acquisition first. A failed
fresh acquisition may return the matching validated cache entry with HTTP 200,
`X-Image-Source: cache-fallback`, `X-Image-Age-Ms`, and a bounded
`X-Capture-Fallback-Reason`; fallback responses do not include fresh capture
timing. Without a matching cache entry they preserve HTTP 503 JSON. The
`/capture/<resolution>/latest.jpg` endpoints (and `/capture/latest.jpg` alias)
are explicitly cache-only and never trigger a camera capture.

`normal` returns the camera's QVGA JPEG. `grayscale` decodes a bounded 1:8
40x30 representation, converts it to luminance-valued RGB565 pixels, and
re-encodes a block-aligned 40x24 grayscale-valued RGB565 JPEG for this target.
It does not use native grayscale pixel-format encoding. `reduced_colors` applies
deterministic RGB565 channel quantization to that low-resolution representation
and re-encodes a 40x24 JPEG. Both
transforms may return HTTP 503 with an explicit error if decoding, encoding, or
bounded internal RAM is unavailable; they never silently claim a mode. No-PSRAM
ESP32 memory limits explain the low-resolution transform and possible 503.
Normal QVGA capture does not depend on either transform. Every fresh normal
endpoint changes the sensor frame size under the camera mutex, captures at
request time, validates SOI/EOI, size, and JPEG SOF dimensions, and never
returns a frame from the previous sensor resolution. When the requested
resolution changes, the camera discards exactly one bounded transition frame
before making at most two validated-frame attempts. A transition or validation
failure is logged with a safe stage and returns a bounded 503; there are no
unbounded retries. `/capture.jpg` is the fresh QVGA alias and never falls back
to cache. Fresh responses include `X-Image-Source: fresh` and
`X-Capture-Duration-Ms`.

The three normal-resolution cache slots retain only the last valid JPEG for
each resolution and its quality (one 8192-byte-or-less copy per resolution,
bounded to three copies total). Cache URLs never capture; before a matching
quality has succeeded they return HTTP 404 with bounded JSON. Successful cache
responses include `X-Image-Source: cache` and `X-Image-Age-Ms`. Cache reads
copy the selected JPEG under a cache mutex and send that copy after releasing
the mutex, so a concurrent fresh capture cannot free response data. Cache data
is volatile RAM only and is never written to SD, flash, NVS, or files.

Transformed JPEGs use `fmt2jpg_cb()` with an explicitly allocated 8192-byte
internal-DRAM output buffer. The callback copies only in-order chunks that fit
the buffer and rejects overflow; the buffer and exact size are returned only
after successful encoding and SOI/EOI validation. On this classic ESP32 target,
the installed `fmt2jpg()` implementation is incompatible with the no-PSRAM
path because it unconditionally allocates a 128 KiB JPEG buffer. This bounded
callback path is a target-specific memory safeguard, not a portability claim
for every esp32-camera release or hardware configuration.

### Dedicated ML-mode endpoint rules

The dedicated URLs in the endpoint table bind the mode on the server, which
makes captures unambiguous for automation. They accept only `quality=10..30`; a conflicting `mode` query returns
HTTP 400. A transform failure returns HTTP 503 with a bounded generic JSON error,
and the server validates JPEG SOI/EOI markers before returning success. The
host validator additionally parses JPEG markers and rejects malformed bodies or
wrong SOF dimensions. Retained-frame URLs require a successful analysis for the
same mode and quality first, otherwise they return HTTP 409.

JPEG `quality` is the camera quality parameter: a lower number means higher
quality. The contract verifies that the requested value is accepted and echoed
by `/health` and `/analysis`; exact visual quality is verified by comparing
same-mode captures at different quality values, not inferred from file size
alone. File size varies with image content and is not a quality measurement.

### Diagnostic-only verification

This does not flash or reset hardware. It makes exactly one request per URL and
keeps response bodies in memory. Set `DEVICE_IP` when it is already known; when it
is absent, the script reads only the approved readiness marker through the existing
safe serial filter. `MODE` may be `both`, `grayscale`, or `reduced_colors`.
`QUALITY` may be `10..30`; lower values request higher JPEG quality.

```bash
source ./00-SOURCE-this.sh
DEVICE_IP=192.0.2.10 MODE=both QUALITY=15 HTTP_TIMEOUT=10 ./scripts/diagnose-http-modes.sh
# Verify another quality contract without flashing:
DEVICE_IP=192.0.2.10 MODE=both QUALITY=10 ./scripts/diagnose-http-modes.sh
# Or obtain the IP from the bounded approved marker:
PORT=/dev/serial/by-id/... MODE=both QUALITY=30 ./scripts/diagnose-http-modes.sh
```

Measure fresh normal capture timing with exactly one bounded request per
resolution; responses remain in memory and are dimension-validated:

```bash
uv run python scripts/diagnose-http-resolutions.py DEVICE_IP --quality 15 --timeout 10
```

The report includes HTTP status, JPEG dimensions, and elapsed milliseconds for
QQVGA, HQVGA, and QVGA. A failure stops the procedure; cached URLs must not be
substituted for continuity or timing measurements.
Resolution changes include one discarded transition frame, so the first fresh
request after a change can be substantially slower than a steady-resolution
request. The live QQVGA transition measured 9086 ms; this is diagnostic timing,
not permission to extend the bounded two-attempt capture limit.

The checker classifies failures as transport (DNS/ARP/TCP/timeout), HTTP status,
JSON contract, or JPEG contract. A successful run proves `/health`, both dedicated
captures, both dedicated analyses, and all six retained-frame URLs. It does not
prove semantic ML inference.

### Safe verification sequence and stop conditions

1. Build without hardware changes: `source ./00-SOURCE-this.sh && idf.py build`.
2. Run the mandatory one-shot sequence, unchanged:
   `./scripts/flash-reset-verify-http.sh`.
3. After that passes, run the diagnostic-only script above for the selected mode.
4. Stop immediately on flash/reset/readiness failure, missing approved IP marker,
   any transport failure, HTTP status other than 2xx, invalid JSON, invalid JPEG,
   or a dedicated endpoint reporting HTTP 503. Do not retry blindly or erase NVS.

The outputs provide bounded, mode-labelled JPEGs and temporal change metrics for
later grain-curtain ML fault detection: they can establish capture availability,
mode consistency, frame validity, and a deterministic temporal baseline. They are
not an ML model, a fault decision, or semantic inference.

The three retained JPEG copies are capped at 8192 bytes each. Transform
workspace is capped at 8192 RGB565 pixels (16384 bytes) plus a 4096-byte decoder
workspace; returned JPEGs are capped at 8192 bytes. Camera and retained-window mutex waits
are capped at 1000 ms, and HTTP send/receive waits at 5 seconds. Capture,
processing, locking, and allocation failures return bounded HTTP 503 errors;
there are no unbounded retries. `/analysis` still returns HTTP 200 when only
decoded-pixel metrics fail, reporting `decoded_pixels.status: "unavailable"`.

## Scope and confirmed facts

| Area | Decision or evidence |
| --- | --- |
| Silicon | Confirmed classic `ESP32-D0WD-V3`, revision 3.1, 40 MHz crystal. |
| Target | ESP-IDF target `esp32`. |
| Wi-Fi network | SSID is `WNinternet_3dfe`. The password is never stored here. |
| Gateway | Explicitly out of scope for this bootstrap. |
| Evidence destinations | SD, flash, and gateway evidence policy are future work. |
| Sensor | OV3660, identified by repository evidence and the user's hardware verification. |
| Camera wiring | Standard ESP32-CAM / AI-Thinker-compatible map, verified by the user. |
| Camera photos | Generic publication photos; they are not used as pin-map evidence. |
| Camera mode | One conservative QVGA JPEG frame, DRAM frame buffer, one buffer. |

The chip and sensor/module evidence is recorded in the existing
`ESP32CAM OV3660 MAC ...md` file. The user's direct verification establishes
the camera-to-programmer pin alignment. The generic publication photos do not
prove that map. PSRAM presence, size, and flash capacity remain unverified.

## Exact hardware uncertainty

The remaining hardware facts are:

1. PSRAM presence, size, timing, and reliable runtime configuration;
2. flash size and the final evidence-storage layout;
3. runtime validation that the attached module responds as the identified
   OV3660 using the verified wiring.

The code reports physical flash size with `esp_flash_get_physical_size()` and
reports PSRAM only when the runtime heap reports a nonzero
`heap_caps_get_total_size(MALLOC_CAP_SPIRAM)`. Therefore the log is
hardware/runtime evidence, not an inference from `sdkconfig`. It still uses one QVGA JPEG frame
buffer in internal DRAM. `esp_camera_init()` must detect the sensor and allocate
the required buffers; otherwise initialization fails clearly and the application
continues to the Wi-Fi path.

## Phase 1: what, how, and why

### 1. Establish an ESP-IDF project

**What:** Add the root and `main/` CMake entry points, a custom partition table,
and an `app_main` component.

**How:** The root CMake follows the existing ESP-IDF project convention and
enables a minimal build. `main/idf_component.yml` declares the managed
`espressif/esp32-camera` dependency, and the component declares it alongside
`esp_event`, `esp_netif`, `esp_wifi`, and `nvs_flash`. `sdkconfig.defaults`
selects the classic `esp32` target and custom partition table.

**Why:** A buildable firmware boundary is useful now, while avoiding camera
hardware assumptions and avoiding gateway dependencies.

### 2. Initialize NVS without destructive recovery

**What:** Start NVS and fail closed when the partition is exhausted or has an
unexpected version.

**How:** `main/app_main.c` calls `nvs_flash_init()`. It reports maintenance is
needed instead of erasing NVS automatically.

**Why:** Automatic erase could destroy provisioned configuration and is unsafe
for a first bootstrap.

### 3. Apply the Phase 1 flags

**What:** Read three boolean flags from the `bootstrap` NVS namespace.

**How:** The keys are `use_sd`, `use_espnow`, and `snd_to_gw`; a missing key
defaults to false. The firmware logs only the boolean state.

**Why:** Feature activation must be explicit and persistent, not inferred from
hardware guesses or compile-time secrets.

| NVS key | Phase 1 default | Meaning in this slice |
| --- | --- | --- |
| `use_sd` | `false` | No SD initialization or writes. |
| `use_espnow` | `false` | No ESP-NOW initialization, callbacks, or traffic. |
| `snd_to_gw` | `false` | No gateway behavior or sends. |

### 4. Provide a Wi-Fi foundation with secret separation

**What:** Support station-mode Wi-Fi only after credentials exist in NVS.

**How:** The firmware reads `wifi_ssid` and `wifi_password` from the same
`bootstrap` namespace. It does not print either value. Missing or incomplete
credentials leave the device idle. The intended network SSID is
`WNinternet_3dfe`.

**Why:** Firmware cannot safely read a host filesystem after flashing, and the
password must not enter source, documentation, logs, shell history, or normal
build reports.

The sole permitted source for the Wi-Fi credentials is:
`/home/qq/bin/SECRETS/WIFI-WNinternet/wifi.env`.

This repository does not inspect, copy, commit, or embed that file. The reviewed
provisioning workflow is `scripts/provision-wifi-nvs.sh`.

#### Secure provisioning procedure

**What:** Generate the `bootstrap` NVS namespace with `wifi_ssid` and
`wifi_password`, then write only the NVS partition at `0x9000` (size `0x6000`)
through the stable FTDI device path.

**How:** The script sources the approved file at runtime with shell tracing
disabled, accepts the existing common SSID/password variable names, requires the
SSID to equal `WNinternet_3dfe`, and invokes the ESP-IDF NVS partition generator.
The CSV and binary are created under a mode-700 temporary directory with mode
600 files. A cleanup trap removes the directory on exit, including failure.

**Why:** Credentials must reach the device without entering repository files,
source, shell arguments, history, logs, documentation, or ordinary build
artifacts. Missing or ambiguous variable names fail closed; values are never
printed. The flash command has one address/data pair and does not erase flash.

After a successful provisioning run, validate only the firmware's
`Wi-Fi connected and IP acquired` message with a serial monitor bounded to 30
seconds. Do not save or report the monitor output.

### 5. Initialize and exercise the camera foundation

**What:** `main/camera.c` provides initialization plus a one-frame capture and
release API.

**How:** It uses the user-verified standard ESP32-CAM / AI-Thinker-compatible
map: `PWDN=32`, `XCLK=0`, `SIOD=26`, `SIOC=27`, data pins `D0=5`, `D1=18`,
`D2=19`, `D3=21`, `D4=36`, `D5=39`, `D6=34`, `D7=35`, `VSYNC=25`, `HREF=23`,
and `PCLK=22`. Reset is not connected (`-1`). The esp32-camera driver performs
sensor detection; the configuration requests JPEG output for the identified
OV3660, QVGA size, quality 15, one frame buffer, and internal DRAM.

**Why:** This is the smallest useful camera slice without assuming PSRAM or
introducing storage, streaming, ESP-NOW, or gateway behavior. `app_main`
attempts initialization and one capture, releases the frame immediately, and
continues Wi-Fi/NVS behavior if the camera is unavailable.

On the first call to the one-frame API, it performs exactly one bounded warm-up
capture, logs its size, returns that frame immediately, and then performs the
requested capture. A null warm-up or requested frame is an error; there is no
unbounded retry. The discard lets the sensor/driver's first JPEG output settle
while preserving the caller's one requested frame result.

### 6. Runtime memory diagnostics

**What:** Log physical flash capacity and runtime PSRAM initialization/size once
after NVS initialization.

**How:** Use the initialized default flash chip with
`esp_flash_get_physical_size()`. Query the runtime heap with
`heap_caps_get_total_size(MALLOC_CAP_SPIRAM)`; report unavailable when it is
zero.

**Why:** Flash capacity and PSRAM must be established from safe runtime APIs,
not configuration claims, before choosing future frame-buffer or evidence
storage layouts. These diagnostics do not write or erase flash and do not expose
credentials.

### 7. Verify live JPEG availability over Wi-Fi

**What:** Start the official ESP-IDF `esp_http_server` component only after the
station receives an IP. Provide `GET /health` and `GET /capture.jpg`.

**How:** The health endpoint returns plain-text `ready`. The capture endpoint
captures one new JPEG through the existing bounded camera API, sends it with
`Content-Type: image/jpeg`, and releases the frame on every path.

**Why:** This proves the camera result is obtainable over Wi-Fi before any
storage or gateway behavior is designed. The endpoint is RAM-only and does not
write SD, flash, gateway state, or NVS.

The endpoint is deliberately unauthenticated for this local-LAN checkpoint.
Anyone who can reach the device can request an image; this is not a production
security boundary.

### Bounded temporal JPEG analysis

1. Run one analysis operation. It captures exactly three sequential JPEGs,
   named `prev`, `current`, and `next`, and retains bounded RAM copies:

   ```bash
   curl --fail --silent --show-error --connect-timeout 3 --max-time 15 \
     http://DEVICE_IP/analysis
   ```

   The JSON reports each frame's nonzero size and bounded FNV-1a32 digest,
   plus byte-change counts and per-mille metrics for both adjacent pairs.
2. Inspect the retained temporal window directly, without saving files:

   ```bash
    for frame in prev current next; do
      wget --quiet --no-verbose --tries=1 --connect-timeout=3 --timeout=10 -O - \
        "http://DEVICE_IP/analysis/${frame}.jpg" \
        | uv run python scripts/check-jpeg-http.py --expected-width 320 --expected-height 240
    done
    ```

    A browser can inspect the same images at
    `http://DEVICE_IP/analysis/prev.jpg`,
    `http://DEVICE_IP/analysis/current.jpg`, and
    `http://DEVICE_IP/analysis/next.jpg`.

    The three JPEG endpoints expose the copies from the most recent run that
    captured all three JPEGs. A capture failure does not replace the prior
    window. JPEG availability is independent from decoded-pixel analysis: if
    decoding fails, `/analysis` still returns HTTP 200 with the compressed-JPEG
    metrics and `decoded_pixels.status: "unavailable"`.

The analysis capacity is bounded at 8192 bytes per JPEG and uses RAM only;
frames are never written to SD, flash, NVS, a gateway, or the repository.
The reported change metric is explicitly provisional: it compares compressed
JPEG bytes, not decoded pixels. It is a temporal JPEG-signature checkpoint,
not semantic object detection or final vectorized image analysis.

### Bounded decoded-pixel comparison

The same `/analysis` request attempts to decode each retained JPEG with the managed
`espressif/esp_jpeg` component at scale 1:8 into RGB565. The JSON adds a
`decoded_pixels` object while preserving the compressed-JPEG fields and the
three retained JPEG endpoints. At the configured QVGA input this is a 40x30
representation; decoded analysis output remains separately capped at 4096 pixels
and 8192 bytes.

When decoding succeeds, `decoded_pixels.status` is `"available"` and the
decoded metrics are present. When it fails, the status is `"unavailable"` and
the response contains only a bounded `failure_stage` such as `jpeg_decode`;
the three captured JPEGs and their compressed metrics remain available. This
supports manual quality and resolution review even when RGB565 processing is
unavailable. The separate endpoint transforms are bounded and report an
explicit 503 if their decoder or encoder cannot run.

For each adjacent pair, `changed_pixels` counts RGB565 samples whose complete
16-bit value differs at the same reduced-grid coordinate. `compared_pixels` is
the common width multiplied by the common height, and:

```text
change_per_mille = changed_pixels * 1000 / compared_pixels
```

The JSON reports `format`, `scale`, dimensions, counts, and
`threshold_status: "not_defined"`. This is a deterministic diagnostic metric,
not a production transient decision; no threshold has been invented or
enabled.

The previous implementation allocated three 8192-byte RGB565 buffers and a
4096-byte decoder working buffer simultaneously. On this no-PSRAM ESP32,
`ml_analysis_run()` had a confirmed high-risk `ESP_ERR_NO_MEM` failure path,
which the `/analysis` handler correctly exposed as HTTP 503 `Analysis
unavailable`.
 The implementation now allocates two 8192-byte RGB565 buffers and one
 4096-byte working buffer. It decodes `prev` and `current`, compares them,
 reuses the `prev` buffer for `next`, and then compares `current` and `next`.
 This preserves both adjacent-pair metrics while reducing the peak decoded
 workspace by 8192 bytes. Allocation, JPEG-info, and decode failures are
 logged with bounded sizes and error names; no image bytes are logged.
Every decoded buffer and the shared working buffer is released on success and
failure. No PSRAM is required, and decoded pixels are transient: they are
compared and freed before `/analysis` returns. JPEG retention remains capped
at three copies of 8192 bytes in application RAM.

The post-change mandatory validation records whether decoded pixels were
available separately from JPEG endpoint validity. A decoded failure is not an
HTTP failure when the three-frame capture completed; only capture, locking, or
response-contract failures remain request failures.

Transform failures emit only the safe serial marker `ML transform failure:` with
the mode, bounded failure stage, ESP error name, derived scaled dimensions,
encoded dimensions, output bytes, and free internal heap. Transformed JPEG
encoding uses `scaled.width & ~7U` and `scaled.height & ~7U`, validates that the
result is nonzero and within the existing bounds, and passes exactly the cropped
RGB565 byte count to `fmt2jpg`. For `JPEG_IMAGE_SCALE_1_8`, the installed
ESP-IDF decoder reports the source JPEG dimensions in `info.width`/`info.height`
while `info.output_len` is the scaled RGB565 buffer size. The implementation
derives scaled dimensions with floor division by 8, treats `output_len` as the
allocation authority, and bounds both the RGB565 byte count and pixel count.
The serial checkpoint filters this marker alongside the
approved readiness markers; it never prints raw serial lines. The diagnostic
workflow reports captured transform markers without retaining the serial log.

This remains limited by JPEG decoding and 1:8 quantization: it is not optical
flow, motion estimation, object detection, or semantic analysis. A threshold
requires later hardware/data calibration.

## Dormant ESP-NOW command-handler design

This is a design boundary, not an enabled radio implementation.

When a later reviewed configuration sets `use_espnow=true`, the handler should:

1. initialize ESP-NOW only after Wi-Fi channel and security policy are known;
2. accept a versioned, authenticated command envelope with a bounded payload;
3. reject malformed, unauthenticated, replayed, or unsupported commands;
4. dispatch `USE_SD` as an idempotent request to enable SD behavior;
5. require an explicit acknowledgement containing the resulting flag state;
6. persist the accepted state in NVS before acknowledging success; and
7. remain quiet and perform no retries while `use_espnow=false`.

The `USE_SD` command must not itself invent camera pins, mount an unknown card,
or enable gateway sends. Authentication, replay protection, peer admission,
wire format, and recovery behavior are deferred until the protocol is reviewed.

## Deferred phases / TODO

- [x] Identify the camera module and OV3660 sensor from repository evidence.
- [x] Verify the standard camera/programmer pin alignment with the user.
- [x] Verify PSRAM presence, size, timing, and ESP-IDF configuration: this
      module reports no runtime PSRAM.
- [x] Verify physical flash size: 4 MiB. The final evidence-storage layout is
      still deferred.
- [x] Define and execute a reviewed, secret-safe Wi-Fi NVS provisioning workflow
      using only `/home/qq/bin/SECRETS/WIFI-WNinternet/wifi.env`.
- [x] Validate OV3660 runtime detection and one-frame capture on hardware.
- [ ] Validate the bounded three-frame RAM-only analysis window on hardware.
- [ ] Define SD detection, filesystem, quotas, and evidence retention.
- [ ] Define flash evidence storage and recovery semantics.
- [ ] Implement and review the authenticated dormant ESP-NOW command protocol.
- [ ] Decide whether and how `USE_SD` may be changed remotely.
- [ ] Define gateway enrollment, transport, and `SND_TO_GW` behavior.
- [ ] Add host tests and hardware validation for each enabled phase.
- [x] Flash and validate the camera-neutral Phase 1 firmware after the
      provisioning and pin-map reviews were complete.

## Validation checklist

The safe validation for this slice is a target build, application-only flash,
and a serial monitor bounded to 30 seconds after sourcing the ESP-IDF
environment. A successful capture after the single warm-up indicates that the
existing wiring, sensor detection, and bounded capture path still work. A
`NO-SOI - JPEG start marker missing` warning means the driver rejected a JPEG
candidate; its absence is preferable, but a successful requested frame is the
capture result that matters. Any warning that remains must be recorded for
follow-up rather than hidden by retries.

- [ ] `idf.py --version` reports the expected local ESP-IDF installation.
- [ ] `idf.py set-target esp32` completes in this project root.
- [ ] `idf.py build` completes with the camera component and dormant ESP-NOW.
- [ ] No secret file contents appear in terminal output, source, docs, or logs.
- [x] After provisioning, a bounded monitor reports `Wi-Fi connected and IP
       acquired` without recording serial output.
- [x] After application-only flash, `/health` returns plain-text `ready`.
- [x] After application-only flash, `/capture.jpg` returns a JPEG with valid
       SOI/EOI markers through a RAM-only host pipe.
- [x] After application-only flash, `/analysis` returns the bounded temporal
        JSON shape and all three retained JPEG endpoints have valid markers.

## Hardware validation record

The application-only image was flashed through the ESP32-CAM-MB stable device
path:

`/dev/serial/by-id/usb-FTDI_FT232R_USB_UART_A5069RR4-if00-port0`

The bounded serial validation confirmed:

- ESP32-D0WD-V3 revision 3.1.
- Physical flash size reported by the runtime API: `4194304` bytes (4 MiB).
- PSRAM runtime status: unavailable; the runtime heap reported no SPIRAM
  capacity. The firmware does not infer PSRAM from `sdkconfig`.
- `USE_SD=false`, `USE_ESPNOW=false`, and `SND_TO_GW=false`.
- OV3660 detected with PID `0x3660` at SCCB address `0x3c`.
- Camera initialization succeeded.
- One warm-up JPEG was discarded (`2428` bytes), followed by one requested
  JPEG captured successfully (`2456` bytes).
- Wi-Fi credentials were absent from NVS; no credential values were printed.

That first record was captured before provisioning. After the secure NVS
provisioning script ran, one explicit hard reset and one bounded monitor
confirmed `Wi-Fi connected and IP acquired`. The monitor was filtered in
memory to approved diagnostic categories and no serial output was saved.

No `NO-SOI - JPEG start marker missing` warnings appeared during the bounded
post-flash monitor. The earlier two warnings are therefore not reproduced by
this bounded warm-up/discard run, but repeated validation is still future work.

The live Wi-Fi checkpoint was then validated after the application-only flash:
the filtered bounded monitor reported `USE_SD=false`, `USE_ESPNOW=false`,
`SND_TO_GW=false`, acquired `192.168.100.83`, and reported the image URL.
Host requests returned `ready` from `/health`; `/capture.jpg` returned 2518 bytes
and then 2529 bytes in two checks, with JPEG SOI/EOI markers present. The image
 bytes were piped to the validator and were not saved to the repository.

The temporal-analysis checkpoint was subsequently validated at the same current
IP. `/analysis` returned HTTP 200 with valid bounded JSON and three nonzero
frames, each no larger than 8192 bytes. The retained endpoints returned:

- `prev.jpg`: 2548 bytes, valid SOI/EOI markers;
- `current.jpg`: 2543 bytes, valid SOI/EOI markers;
- `next.jpg`: 2555 bytes, valid SOI/EOI markers.

The response bytes were checked in memory and not saved to disk.

The decoded-pixel firmware was then rebuilt and fully reflashed without
erasing or writing NVS. A bounded readiness checkpoint did not show boot
markers, camera readiness, or an IP address. Consequently, the decoded-pixel
JSON fields are implemented but not yet hardware-validated on this firmware;
the current blocker is boot/readiness, not a confirmed HTTP failure.

The corrected graceful-degradation firmware was then validated with the
mandatory one-shot verifier. Flash, explicit reset, readiness, and IP
extraction passed for `192.168.100.83`. The verifier confirmed:

- `/health`: passed;
- `/capture.jpg`: 2575 bytes, valid JPEG;
- `/analysis`: HTTP 200 with accepted `decoded_pixels.status=unavailable` and
  `failure_stage=decoded_bounds`;
- `/analysis/prev.jpg`: 2589 bytes, valid JPEG;
- `/analysis/current.jpg`: 2579 bytes, valid JPEG;
- `/analysis/next.jpg`: 2582 bytes, valid JPEG.

This proves image access is independent of decoded-pixel availability. The
images are now available for manual quality, resolution, and grayscale
 experiments. The decoded-pixel bound remains a separate implementation issue.

## Approved Phase 1 periodic and motion pipeline

The periodic pipeline is deliberately RAM-only and bounded:

| Item | Contract |
| --- | --- |
| Periodic interval | Compile-time `ML_PERIODIC_CAPTURE_INTERVAL_SECONDS`, clamped to 1..60 seconds; default 1 second. |
| Periodic source | Validated QVGA (320x240) JPEG, captured only after camera initialization. |
| Periodic ownership | Two fixed 8192-byte slots; the producer writes the unpublished slot while HTTP copies the published slot under a separate mutex. No flash, NVS, SD, or file writes. |
| Capture task | `xTaskCreatePinnedToCore(..., 1)` on dual-core builds; the unicore fallback uses `xTaskCreate`. |
| HTTP task | `httpd_config_t.task_core_id = 0` where exposed by ESP-IDF; unicore also uses core 0. This is scheduling affinity, not isolation: Wi-Fi/camera interrupts and drivers still share system resources. |
| Periodic endpoint | `GET /capture/periodic/latest.jpg`; returns bounded 404 JSON before the first valid frame, then reports source, age, interval, dimensions, and producer core headers. |

`/capture.jpg` and the existing resolution-specific fresh/cache endpoints keep
their existing semantics. The periodic endpoint is explicit and never replaces a
fresh request-triggered capture.

### Bounded `motion_edges` mode

`motion_edges` is independent from the normal cache. It captures a QQVGA JPEG,
decodes it at 1:8 to a bounded 20x15 RGB565 grid, computes integer horizontal
and vertical luminance gradients, thresholds them, and encodes a block-aligned
16x8 black/white JPEG. The threshold is the bounded compile-time
`ML_MOTION_EDGE_THRESHOLD` value (default 48, clamped to 1..255).

- `GET /ml/motion-edges.jpg?mode=motion_edges&quality=15`
- `GET /ml/motion-edges/latest.jpg?mode=motion_edges&quality=15`

The latest path has its own two-slot cache and mutex, so it cannot expose a
partially written edge frame or corrupt the normal capture cache. The `/info`
endpoint lists the verified QQVGA, HQVGA, and QVGA resolutions, active interval,
implemented modes, and all implemented endpoints. Deferred ideas are listed
under `nice_to_have`, not as available endpoints.

NICE-TO-HAVE, not available in this phase: OpenCV, MJPEG streaming, SVG output,
USB UVC, semantic ML inference, and higher resolutions.

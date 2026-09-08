# ML CAM v001: safe bootstrap

This repository now contains a conservative camera foundation for the
identified classic ESP32 silicon. After station-mode Wi-Fi obtains an IP, it
starts a local-LAN HTTP checkpoint server so one fresh JPEG can be obtained
from RAM before any storage is introduced. It does not stream video, use SD,
write flash evidence, act as a gateway, or enable ESP-NOW.

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
     http://DEVICE_IP/health
   ```

   Expected response: `ready`.
3. Obtain and validate one fresh JPEG entirely through a pipe. This does not
   save the image in the repository:

   ```bash
   curl --fail --silent --show-error --connect-timeout 3 --max-time 10 \
     http://DEVICE_IP/capture.jpg \
     | uv run python scripts/check-jpeg-http.py
   ```

   Expected response: `JPEG checkpoint passed: ... bytes; SOI/EOI markers
   present`.

The `/capture.jpg` handler captures one frame in RAM, sends it, and releases
the camera frame even when the HTTP send fails. No SD, flash, gateway, or NVS
write is performed. The server starts only after Wi-Fi has acquired an IP.
This checkpoint has no authentication and is intended only for a trusted local
LAN; it is not production-safe and must not be exposed to an untrusted network.

The shared toolchain convention comes from `/W/NVT/espnow-master-slave`. The
repository wrapper delegates to `scripts/idf-env.sh`, which selects the local
ESP-IDF v6.0.2 environment when available.

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

# Project instructions

## Tooling and workflow

- Use `mise` for project tooling.
- Use `uv` for the Python environment.
- Always run Python as `uv run python`.
- Run Bash and Python work from file-based scripts under `scripts/` (or
  `script-helpers/` for a deliberately one-use helper); do not paste ad-hoc
  scripts into shell commands.
- Keep repeatable tasks documented and exposed through `make`, `just`, or
  `mise` tasks.
- Keep commits small and semantic. Do not commit unless explicitly requested.
- Load the ESP-IDF environment with `source ./00-SOURCE-this.sh`; the shared
  environment convention is maintained under `/W/NVT/espnow-master-slave`.

## ESP32-CAM rules

- The confirmed silicon target is the classic `esp32` target (ESP32-D0WD-V3).
- Do not claim camera support or add camera GPIO assignments until the camera
  board, sensor, pin map, and PSRAM are identified from hardware evidence.
- Keep gateway behavior out of the current bootstrap slice.
- Phase 1 defaults are `USE_SD=false`, `USE_ESPNOW=false`, and `SND_TO_GW=false`.
- ESP-NOW must remain dormant unless explicitly enabled by a later, reviewed
  configuration change.

## Secret handling

- Wi-Fi credentials may be read only from
  `/home/qq/bin/SECRETS/WIFI-WNinternet/wifi.env`.
- Never copy, print, log, commit, embed in source, documentation, test data,
  shell history, or generated reports any Wi-Fi password or other secret.
- Do not inspect the contents of the secret file during repository work.
- Treat build artifacts, serial output, and NVS images as sensitive whenever
  they may contain provisioned credentials.
- Do not modify, overwrite, or delete secret files.

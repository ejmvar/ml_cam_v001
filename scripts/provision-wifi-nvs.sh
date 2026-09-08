#!/usr/bin/env bash
set -Eeuo pipefail

# This script deliberately accepts no credential arguments. The approved
# secret file is sourced only at runtime and all derived material is temporary.
if [[ $- == *x* ]]; then
    set +x
fi
umask 077

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
SECRET_FILE="/home/qq/bin/SECRETS/WIFI-WNinternet/wifi.env"
PORT="/dev/serial/by-id/usb-FTDI_FT232R_USB_UART_A5069RR4-if00-port0"
NVS_OFFSET="0x9000"
NVS_SIZE="0x6000"
NVS_GENERATOR="${IDF_PATH:-${HOME}/.espressif/v6.0.2/esp-idf}/components/nvs_flash/nvs_partition_generator/nvs_partition_gen.py"

die() {
    printf '[provision-wifi-nvs] %s\n' "$1" >&2
    exit 1
}

[[ -f "${ROOT_DIR}/partitions.csv" ]] || die 'partitions.csv is missing'
[[ -f "${SECRET_FILE}" ]] || die 'approved secret file is missing'
[[ -x "${PORT}" || -c "${PORT}" ]] || die 'stable serial device is unavailable'
[[ -f "${NVS_GENERATOR}" ]] || die 'ESP-IDF NVS generator is unavailable; source ./00-SOURCE-this.sh first'
command -v esptool >/dev/null 2>&1 || die 'esptool is unavailable; source ./00-SOURCE-this.sh first'

TEMP_DIR="$(mktemp -d "${TMPDIR:-/tmp}/ml-cam-wifi-nvs.XXXXXX")"
chmod 700 "${TEMP_DIR}"
trap 'rm -rf -- "${TEMP_DIR}"' EXIT HUP INT TERM
CSV_FILE="${TEMP_DIR}/nvs.csv"
IMAGE_FILE="${TEMP_DIR}/nvs.bin"
chmod 600 "${CSV_FILE}" "${IMAGE_FILE}" 2>/dev/null || true

# Source output is discarded, and no values from the file are ever printed.
set +u
source "${SECRET_FILE}" >/dev/null 2>&1
set -u

ssid_name=''
ssid_value=''
for name in WIFI_SSID SSID; do
    if [[ -v "${name}" && -n "${!name}" ]]; then
        ssid_name="${name}"
        ssid_value="${!name}"
        break
    fi
done
[[ -n "${ssid_name}" ]] || die 'missing Wi-Fi SSID variable (expected WIFI_SSID or SSID)'
[[ "${ssid_value}" == 'WNinternet_3dfe' ]] || die 'Wi-Fi SSID variable does not match the required network'

password_name=''
password_value=''
for name in WIFI_PASSWORD WIFI_PASS WIFI_PASSWD WIFI_PSK PASSWORD; do
    if [[ -v "${name}" && -n "${!name}" ]]; then
        password_name="${name}"
        password_value="${!name}"
        break
    fi
done
[[ -n "${password_name}" ]] || die 'missing Wi-Fi password variable (expected WIFI_PASSWORD, WIFI_PASS, WIFI_PASSWD, WIFI_PSK, or PASSWORD)'

[[ "${ssid_value}" != *$'\r'* && "${ssid_value}" != *$'\n'* ]] || die 'Wi-Fi SSID contains a line break'
[[ "${password_value}" != *$'\r'* && "${password_value}" != *$'\n'* ]] || die 'Wi-Fi password contains a line break'
[[ "${#ssid_value}" -le 32 ]] || die 'Wi-Fi SSID exceeds the ESP32 limit'
[[ "${#password_value}" -le 64 ]] || die 'Wi-Fi password exceeds the ESP32 limit'

csv_quote() {
    local value="$1"
    value="${value//\"/\"\"}"
    printf '"%s"' "${value}"
}

{
    printf 'key,type,encoding,value\n'
    printf 'bootstrap,namespace,,\n'
    printf 'wifi_ssid,data,string,'
    csv_quote "${ssid_value}"
    printf '\n'
    printf 'wifi_password,data,string,'
    csv_quote "${password_value}"
    printf '\n'
} >"${CSV_FILE}"
chmod 600 "${CSV_FILE}"

python "${NVS_GENERATOR}" generate "${CSV_FILE}" "${IMAGE_FILE}" "${NVS_SIZE}" >/dev/null
chmod 600 "${IMAGE_FILE}"

# This is intentionally a single-partition write: no erase-all and no other
# address/data pair are permitted by this invocation.
esptool --port "${PORT}" write-flash "${NVS_OFFSET}" "${IMAGE_FILE}"
printf '%s\n' '[provision-wifi-nvs] NVS partition flashed successfully'

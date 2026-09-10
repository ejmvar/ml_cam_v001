#!/usr/bin/env bash
set -Eeuo pipefail

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
PORT="${PORT:-/dev/serial/by-id/usb-FTDI_FT232R_USB_UART_A5069RR4-if00-port0}"
DEVICE_IP="${DEVICE_IP:-}"
MODE="${MODE:-both}"
QUALITY="${QUALITY:-15}"
READINESS_TIMEOUT="${READINESS_TIMEOUT:-30}"
HTTP_TIMEOUT="${HTTP_TIMEOUT:-10}"

die() { printf '[diagnose-http] %s\n' "$1" >&2; exit 1; }
bounded() { [[ "$1" =~ ^[1-9][0-9]*$ ]] && ((10#$1 <= "$2")); }
bounded "$READINESS_TIMEOUT" 300 || die 'READINESS_TIMEOUT must be an integer from 1 through 300.'
bounded "$HTTP_TIMEOUT" 60 || die 'HTTP_TIMEOUT must be an integer from 1 through 60.'
[[ "$QUALITY" =~ ^(1[0-9]|20|2[1-9]|30)$ ]] || die 'QUALITY must be an integer from 10 through 30.'
case "$MODE" in
    both) MODES=(grayscale reduced_colors) ;;
    grayscale|reduced_colors) MODES=("$MODE") ;;
    *) die 'MODE must be both, grayscale, or reduced_colors.' ;;
esac

valid_ip() {
    local part
    IFS=. read -r -a parts <<< "$1"
    (( ${#parts[@]} == 4 )) || return 1
    for part in "${parts[@]}"; do [[ "$part" =~ ^[0-9]+$ ]] && ((10#$part <= 255)) || return 1; done
}

if [[ -z "$DEVICE_IP" ]]; then
    [[ -e "$PORT" ]] || die "transport setup failure: serial port is not available: $PORT"
    marker_file="$(mktemp)"
    cleanup() { rm -f -- "$marker_file"; }
    trap cleanup EXIT
    cd "$ROOT_DIR"
    source ./00-SOURCE-this.sh >/dev/null
    if ! timeout --foreground "$((READINESS_TIMEOUT + 5))s" \
        "$IDF_PYTHON_ENV_PATH/bin/python" scripts/serial-checkpoint.py \
        "$PORT" "$READINESS_TIMEOUT" no-reset >"$marker_file"; then
        die 'transport setup failure: bounded readiness marker read failed.'
    fi
    while IFS= read -r line; do
        if [[ "$line" =~ Image\ checkpoint\ URL:\ http://([0-9]+\.[0-9]+\.[0-9]+\.[0-9]+)/capture\.jpg ]]; then
            candidate="${BASH_REMATCH[1]}"
            if valid_ip "$candidate"; then DEVICE_IP="$candidate"; break; fi
        fi
    done < "$marker_file"
    [[ -n "$DEVICE_IP" ]] || die 'transport setup failure: approved Image checkpoint URL marker did not provide an IPv4 address.'
fi
valid_ip "$DEVICE_IP" || die 'transport setup failure: DEVICE_IP is not a valid IPv4 address.'

transform_marker_file=""
transform_monitor_pid=""
if [[ -e "$PORT" ]]; then
    transform_marker_file="$(mktemp)"
    cleanup_transform_monitor() {
        if [[ -n "$transform_monitor_pid" ]]; then
            kill "$transform_monitor_pid" 2>/dev/null || true
            wait "$transform_monitor_pid" 2>/dev/null || true
        fi
        [[ -z "$transform_marker_file" ]] || rm -f -- "$transform_marker_file"
        [[ -z "${marker_file:-}" ]] || rm -f -- "$marker_file"
    }
    trap cleanup_transform_monitor EXIT
    cd "$ROOT_DIR"
    source ./00-SOURCE-this.sh >/dev/null
    timeout --foreground "${HTTP_TIMEOUT}s" \
        "$IDF_PYTHON_ENV_PATH/bin/python" scripts/serial-checkpoint.py \
        "$PORT" "$HTTP_TIMEOUT" no-reset >"$transform_marker_file" &
    transform_monitor_pid=$!
fi

cd "$ROOT_DIR"
BASE_URL="http://${DEVICE_IP}"
check() {
    local label="$1" url="$2" kind="$3" mode="$4"
    printf '[diagnose-http] %s\n' "$label"
        if ! uv run python scripts/check-http-contract.py "$url" "$kind" --mode "$mode" --quality "$QUALITY" --timeout "$HTTP_TIMEOUT"; then
        die "failed at $label"
    fi
}

for mode in "${MODES[@]}"; do
    slug="$mode"
    [[ "$mode" == reduced_colors ]] && slug='reduced-colors'
    check "/health?mode=$mode&quality=$QUALITY" "$BASE_URL/health?mode=$mode&quality=$QUALITY" health "$mode"
    check "/ml/$slug.jpg" "$BASE_URL/ml/$slug.jpg?quality=$QUALITY" jpeg "$mode"
    check "/ml/$slug/analysis" "$BASE_URL/ml/$slug/analysis?quality=$QUALITY" analysis "$mode"
    for frame in prev current next; do
        check "/ml/$slug/analysis/$frame.jpg" "$BASE_URL/ml/$slug/analysis/$frame.jpg?quality=$QUALITY" jpeg "$mode"
    done
done
if [[ -n "$transform_marker_file" && -s "$transform_marker_file" ]]; then
    while IFS= read -r line; do
        [[ "$line" == *"ML transform failure:"* ]] && printf '[diagnose-http] %s\n' "$line"
    done < "$transform_marker_file"
fi
printf '[diagnose-http] passed: transport, HTTP status, JSON, and JPEG contracts for %s\n' "$MODE"

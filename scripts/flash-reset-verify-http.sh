#!/usr/bin/env bash
set -Eeuo pipefail

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
PORT="${PORT:-/dev/serial/by-id/usb-FTDI_FT232R_USB_UART_A5069RR4-if00-port0}"
POST_FLASH_TIMEOUT="${POST_FLASH_TIMEOUT:-5}"
READINESS_TIMEOUT="${READINESS_TIMEOUT:-30}"
HTTP_TIMEOUT="${HTTP_TIMEOUT:-10}"
TCP_READINESS_TIMEOUT="${TCP_READINESS_TIMEOUT:-30}"
TCP_READINESS_POLL_INTERVAL="${TCP_READINESS_POLL_INTERVAL:-1}"
READINESS_SETTLE_DELAY="${READINESS_SETTLE_DELAY:-1}"

die() {
    printf '[flash-reset-verify] %s\n' "$1" >&2
    exit 1
}

is_bounded_seconds() {
    [[ "$1" =~ ^[1-9][0-9]*$ ]] && ((10#$1 <= 300))
}

is_bounded_http_timeout() {
    [[ "$1" =~ ^[1-9][0-9]*$ ]] && ((10#$1 <= 60))
}

is_bounded_settle_delay() {
    [[ "$1" =~ ^[1-9][0-9]*$ ]] && ((10#$1 <= 10))
}

is_bounded_tcp_timeout() {
    [[ "$1" =~ ^[1-9][0-9]*$ ]] && ((10#$1 <= 60))
}

is_bounded_tcp_poll_interval() {
    [[ "$1" =~ ^[1-9][0-9]*$ ]] && ((10#$1 <= 5))
}

is_bounded_seconds "$POST_FLASH_TIMEOUT" || die 'POST_FLASH_TIMEOUT must be an integer from 1 through 300.'
is_bounded_seconds "$READINESS_TIMEOUT" || die 'READINESS_TIMEOUT must be an integer from 1 through 300.'
is_bounded_http_timeout "$HTTP_TIMEOUT" || die 'HTTP_TIMEOUT must be an integer from 1 through 60.'
is_bounded_tcp_timeout "$TCP_READINESS_TIMEOUT" || die 'TCP_READINESS_TIMEOUT must be an integer from 1 through 60.'
is_bounded_tcp_poll_interval "$TCP_READINESS_POLL_INTERVAL" || die 'TCP_READINESS_POLL_INTERVAL must be an integer from 1 through 5.'
is_bounded_settle_delay "$READINESS_SETTLE_DELAY" || die 'READINESS_SETTLE_DELAY must be an integer from 1 through 10.'
[[ -e "$PORT" ]] || die "stable serial port is not available: $PORT"

cd "$ROOT_DIR"
# shellcheck source=/dev/null
source ./00-SOURCE-this.sh >/dev/null

printf '[flash-reset-verify] flash: idf.py --port %s flash (non-erasing)\n' "$PORT"
if ! idf.py --port "$PORT" flash; then
    die 'primary failure stage: flash.'
fi

printf '[flash-reset-verify] waiting %ss after flash\n' "$POST_FLASH_TIMEOUT"
sleep "$POST_FLASH_TIMEOUT"

printf '[flash-reset-verify] reset: one explicit esptool hard reset\n'
if ! timeout --foreground 15s python -m esptool --port "$PORT" --connect-attempts 1 --after hard-reset run >/dev/null; then
    die 'primary failure stage: reset/readiness (explicit hard reset failed).'
fi

LOG_FILE="$(mktemp)"
cleanup() {
    rm -f -- "$LOG_FILE"
}
trap cleanup EXIT

printf '[flash-reset-verify] readiness: bounded %ss log checkpoint\n' "$READINESS_TIMEOUT"
# serial-checkpoint enforces the exact readiness window; the extra grace lets it
# close its serial handle and return its marker-missing status cleanly.
if ! timeout --foreground "$((READINESS_TIMEOUT + 5))s" \
    "$IDF_PYTHON_ENV_PATH/bin/python" scripts/serial-checkpoint.py \
    "$PORT" "$READINESS_TIMEOUT" no-reset >"$LOG_FILE"; then
    die 'primary failure stage: reset/readiness (bounded log checkpoint failed).'
fi

DEVICE_IP=''
while IFS= read -r line; do
    if [[ "$line" =~ Image\ checkpoint\ URL:\ http://([0-9]+\.[0-9]+\.[0-9]+\.[0-9]+)/capture\.jpg ]]; then
        candidate="${BASH_REMATCH[1]}"
        IFS=. read -r octet1 octet2 octet3 octet4 <<< "$candidate"
        if ((octet1 <= 255 && octet2 <= 255 && octet3 <= 255 && octet4 <= 255)); then
            DEVICE_IP="$candidate"
            break
        fi
    fi
done < "$LOG_FILE"
[[ -n "$DEVICE_IP" ]] || die 'primary failure stage: reset/readiness (approved Image checkpoint URL marker did not provide an IPv4 address).'
printf '[flash-reset-verify] readiness passed: image checkpoint IP %s\n' "$DEVICE_IP"

wait_for_tcp_port() {
    local deadline=$((SECONDS + TCP_READINESS_TIMEOUT))
    while ((SECONDS < deadline)); do
        # Bash /dev/tcp performs only a TCP connect; it sends no HTTP request.
        if timeout --foreground 1s bash -c 'exec 3<>"/dev/tcp/$1/80"' _ "$DEVICE_IP" 2>/dev/null; then
            return 0
        fi
        sleep "$TCP_READINESS_POLL_INTERVAL"
    done
    return 1
}

printf '[flash-reset-verify] readiness: bounded TCP port 80 gate (%ss timeout)\n' "$TCP_READINESS_TIMEOUT"
if ! wait_for_tcp_port; then
    die "primary failure stage: reset/readiness (TCP port 80 was not ready within ${TCP_READINESS_TIMEOUT}s)."
fi
printf '[flash-reset-verify] TCP port 80 accepted a connection\n'
printf '[flash-reset-verify] post-TCP grace before first request: %ss\n' "$READINESS_SETTLE_DELAY"
sleep "$READINESS_SETTLE_DELAY"

wget_once() {
    wget --quiet --no-verbose --tries=1 \
        --connect-timeout="$HTTP_TIMEOUT" --timeout="$HTTP_TIMEOUT" -O - "$1"
}

BASE_URL="http://${DEVICE_IP}"
printf '[flash-reset-verify] wget /health\n'
if ! wget_once "${BASE_URL}/health?mode=normal&quality=15" | uv run python scripts/check-health-json.py; then
    die 'primary failure stage: wget endpoint (/health validation failed).'
fi

printf '[flash-reset-verify] wget /capture.jpg\n'
if ! wget_once "${BASE_URL}/capture.jpg?mode=normal&quality=15" | uv run python scripts/check-jpeg-http.py --expected-width 320 --expected-height 240; then
    die 'primary failure stage: wget endpoint (/capture.jpg JPEG validation failed).'
fi

printf '[flash-reset-verify] wget /analysis\n'
if ! wget_once "${BASE_URL}/analysis?mode=normal&quality=15" | uv run python scripts/check-analysis-json.py; then
    die 'primary failure stage: wget endpoint (/analysis JSON validation failed).'
fi

for image in /analysis/prev.jpg /analysis/current.jpg /analysis/next.jpg; do
    printf '[flash-reset-verify] wget %s\n' "$image"
    if ! wget_once "${BASE_URL}${image}?mode=normal&quality=15" | uv run python scripts/check-jpeg-http.py --expected-width 320 --expected-height 240; then
        die "primary failure stage: wget endpoint (${image} JPEG validation failed)."
    fi
done

printf '%s\n' '[flash-reset-verify] validation passed: reset/readiness and all six HTTP URLs verified.'

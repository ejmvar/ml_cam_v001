#!/usr/bin/env bash

# Source this file to make the repository's ESP-IDF toolchain available.
# It intentionally does not install tools or activate a repository Python venv.

idf_env_setup() {
    local default_idf_path="${HOME}/.espressif/v6.0.2/esp-idf"
    local idf_path="${IDF_PATH:-${default_idf_path}}"
    local export_script="${idf_path}/export.sh"
    local idf_series python_env_root python_env idf_version_dir
    local configured_python_env="${ESPNOW_IDF_PYTHON_ENV_PATH:-}"
    local -a python_env_candidates=()

    if [[ -n "${VIRTUAL_ENV:-}" && "${VIRTUAL_ENV}" != "${IDF_PYTHON_ENV_PATH:-}" ]]; then
        printf '%s\n' "[idf-env] A non-ESP-IDF Python virtual environment is active: ${VIRTUAL_ENV}" >&2
        printf '%s\n' '[idf-env] Remediation: run deactivate, then source ./00-SOURCE-this.sh.' >&2
        return 1
    fi

    if [[ ! -d "${idf_path}" ]]; then
        printf '%s\n' "[idf-env] ESP-IDF checkout not found: ${idf_path}" >&2
        if [[ -n "${IDF_PATH:-}" ]]; then
            printf '%s\n' "[idf-env] Remediation: export IDF_PATH=/absolute/path/to/esp-idf (current override: ${IDF_PATH})." >&2
        else
            printf '%s\n' "[idf-env] Remediation: git clone --branch v6.0.2 --recursive https://github.com/espressif/esp-idf.git \"${default_idf_path}\"" >&2
        fi
        return 1
    fi

    if [[ ! -f "${export_script}" ]]; then
        printf '%s\n' "[idf-env] ESP-IDF export script not found: ${export_script}" >&2
        printf '%s\n' "[idf-env] Remediation: install or repair the checkout at ${idf_path}, then rerun: source ./00-SOURCE-this.sh" >&2
        return 1
    fi

    # ESP-IDF v6 checkouts expose their version through idf.py rather than a
    # repository-root version.txt. The installed checkout convention includes
    # the version in its directory name, which is enough to select its venv;
    # idf-check validates the authoritative idf.py version after export.
    idf_version_dir="$(basename -- "$(dirname -- "${idf_path}")")"
    if [[ "${idf_version_dir}" =~ ^v([0-9]+\.[0-9]+) ]]; then
        idf_series="${BASH_REMATCH[1]}"
    elif [[ "${configured_python_env}" =~ /idf([0-9]+\.[0-9]+)_py ]]; then
        idf_series="${BASH_REMATCH[1]}"
    else
        printf '%s\n' "[idf-env] Cannot determine the ESP-IDF major/minor version from ${idf_path}." >&2
        return 1
    fi

    # ESP-IDF's export.sh honors a pre-existing IDF_PYTHON_ENV_PATH, which can
    # leave a shell pinned to an old Python environment. Select a valid
    # environment on every source instead; an explicit repository override is
    # available without trusting that stale ESP-IDF variable.
    python_env_root="${IDF_TOOLS_PATH:-${HOME}/.espressif}/python_env"
    if [[ -n "${configured_python_env}" ]]; then
        python_env="${configured_python_env}"
    else
        shopt -s nullglob
        python_env_candidates=("${python_env_root}/idf${idf_series}_py"*_env)
        shopt -u nullglob
        if ((${#python_env_candidates[@]} == 0)); then
            printf '%s\n' "[idf-env] No ESP-IDF Python environment found for ESP-IDF ${idf_series} under ${python_env_root}." >&2
            printf '%s\n' "[idf-env] Remediation: ${idf_path}/install.sh esp32s3, then source ./00-SOURCE-this.sh" >&2
            return 1
        fi
        # Globbing is deterministic; the final candidate is the highest
        # installed Python version for this ESP-IDF series.
        python_env="${python_env_candidates[${#python_env_candidates[@]}-1]}"
    fi

    if [[ ! -x "${python_env}/bin/python" ]]; then
        printf '%s\n' "[idf-env] ESP-IDF Python executable not found: ${python_env}/bin/python" >&2
        printf '%s\n' "[idf-env] Remediation: ${idf_path}/install.sh esp32s3, then source ./00-SOURCE-this.sh" >&2
        return 1
    fi

    export IDF_PATH="${idf_path}"
    export IDF_PYTHON_ENV_PATH="${python_env}"
    # Put the interpreter selected by ESP-IDF before mise/system shims. This
    # also makes idf.py's /usr/bin/env python shebang deterministic.
    export PATH="${python_env}/bin:${PATH}"
    export PYTHON="${python_env}/bin/python"
    # shellcheck source=/dev/null
    source "${export_script}"
    hash -r 2>/dev/null || true

    if ! command -v idf.py >/dev/null 2>&1; then
        printf '%s\n' "[idf-env] idf.py is unavailable after sourcing ${IDF_PATH}/export.sh." >&2
        printf '%s\n' "[idf-env] Remediation: ${IDF_PATH}/install.sh esp32s3 && source \"${IDF_PATH}/export.sh\"" >&2
        return 1
    fi

    export ESP_IDF_ENV_INITIALIZED_PATH="${IDF_PATH}"
    return 0
}

idf_env_setup
unset -f idf_env_setup

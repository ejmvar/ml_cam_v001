#!/usr/bin/env bash
set -Eeuo pipefail

ROOT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/.." && pwd)"
source "${ROOT_DIR}/scripts/idf-env.sh"

IDF_PYTHON="${IDF_PYTHON_ENV_PATH}/bin/python"
if ! IDF_PYTHON_ACTUAL="$(${IDF_PYTHON} -c 'import sys; print(sys.executable)')"; then
    printf '%s\n' "[idf-check] Cannot execute the ESP-IDF Python: ${IDF_PYTHON}" >&2
    printf '%s\n' "[idf-check] Remediation: ${IDF_PATH}/install.sh esp32s3, then rerun the check." >&2
    exit 1
fi
IDF_PYTHON_VERSION="$(${IDF_PYTHON} --version 2>&1)"
if ! IDF_VERSION="$(idf.py --version 2>&1)" || [[ ! "${IDF_VERSION}" =~ ^ESP-IDF\ v[0-9]+\.[0-9]+\.[0-9]+ ]]; then
    printf '%s\n' '[idf-check] idf.py did not report a valid ESP-IDF version.' >&2
    printf '%s\n' "[idf-check] Remediation: repair ${IDF_PATH} and its selected Python environment, then rerun the check." >&2
    exit 1
fi
if ! IDF_PY_COMPONENT="$(${IDF_PYTHON} -c 'import idf_component_manager; print(idf_component_manager.__file__)')"; then
    printf '%s\n' "[idf-check] idf_component_manager is not importable from ${IDF_PYTHON}." >&2
    printf '%s\n' "[idf-check] Remediation: ${IDF_PATH}/install.sh esp32s3, then rerun the check." >&2
    exit 1
fi

[[ "${IDF_PATH}" == /* && -f "${IDF_PATH}/export.sh" && -f "${IDF_PATH}/tools/idf.py" ]] || {
    printf '%s\n' "[idf-check] Invalid IDF_PATH: ${IDF_PATH}" >&2
    exit 1
}
[[ "$(command -v idf.py)" == "${IDF_PATH}/tools/idf.py" ]] || {
    printf '%s\n' "[idf-check] idf.py is not selected from ${IDF_PATH}/tools." >&2
    printf '%s\n' '[idf-check] Remediation: source ./00-SOURCE-this.sh in this shell.' >&2
    exit 1
}
[[ "${IDF_PYTHON_ACTUAL}" == "${IDF_PYTHON}" ]] || {
    printf '%s\n' "[idf-check] ESP-IDF selected Python mismatch: ${IDF_PYTHON_ACTUAL}" >&2
    exit 1
}

printf '[idf-check] IDF_PATH=%s\n' "${IDF_PATH}"
printf '[idf-check] idf.py=%s\n' "$(command -v idf.py)"
printf '[idf-check] ESP-IDF=%s\n' "${IDF_VERSION}"
printf '[idf-check] Python=%s (%s)\n' "${IDF_PYTHON_ACTUAL}" "${IDF_PYTHON_VERSION}"
printf '[idf-check] idf_component_manager=%s\n' "${IDF_PY_COMPONENT}"

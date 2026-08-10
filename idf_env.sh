#!/usr/bin/env bash
# Common ESP-IDF environment setup for all scripts.
# Bypasses Python 3.9 importlib.metadata namespace package bug
# that prevents export.sh from completing.
# Usage: source idf_env.sh

IDF_DIR="$HOME/esp/esp-idf"
ESPRESSIF_DIR="$HOME/.espressif"

if [ ! -f "$IDF_DIR/export.sh" ]; then
    echo "ERROR: ESP-IDF not found at $IDF_DIR"
    echo "Run ./setup_idf.sh first."
    return 1 2>/dev/null || exit 1
fi

# Git Bash (MSYS2) always sets MSYSTEM, and idf.py's own __main__ guard no-ops
# entirely when it sees MSYSTEM (see esp-idf/tools/idf.py: it prints a warning
# and never calls main()). `unset MSYSTEM` in this shell does not help either --
# MSYS2's runtime re-injects MSYSTEM into any native (non-MSYS) child process
# bash spawns, regardless of the parent shell's env table. The only reliable
# fix is routing the actual idf.py invocation through a native Python
# subprocess.call (not bash's own exec), which does not get the reinjection.
#
# This must run unconditionally, ABOVE the "already configured" early-return
# below: a child shell that inherits an already-exported IDF_PATH (e.g. a
# script that itself sources this file after a caller already did) hits that
# early return before ever defining this function otherwise. Shell functions
# are not inherited across a `bash script.sh` child process the way exported
# env vars are, so every subshell needs this block to actually run again.
if [ -n "${MSYSTEM:-}" ]; then
    idf.py() {
        python - "$@" <<'PYEOF'
import os
import subprocess
import sys

env = dict(os.environ)
env.pop("MSYSTEM", None)
idf_py = os.path.join(env["IDF_PATH"], "tools", "idf.py")
raise SystemExit(subprocess.call([sys.executable, idf_py] + sys.argv[1:], env=env))
PYEOF
    }
fi

# If the v5 environment is already active (IDF_PATH points to the v5 dir and
# idf.py is in PATH) skip the manual PATH construction.
# Do NOT early-return if a different version (e.g. v6) is active — we need to
# overwrite IDF_PATH, IDF_PYTHON_ENV_PATH, and PATH with v5 values.
if [ "${IDF_PATH:-}" = "$IDF_DIR" ] && command -v idf.py >/dev/null 2>&1; then
    return 0 2>/dev/null || true
fi

# --- IDF 5.x fallback: manual PATH setup (bypasses Python importlib.metadata bug) ---

# Use Python 3.14 to bypass the Python 3.9 importlib.metadata bug
if [ -x /Library/Frameworks/Python.framework/Versions/3.14/bin/python3 ]; then
    export PATH="/Library/Frameworks/Python.framework/Versions/3.14/bin:$PATH"
elif [ -x /usr/bin/python3 ]; then
    # Fallback just in case
    export PATH="/usr/bin:$PATH"
fi

export IDF_PATH="$IDF_DIR"

# Set up Python venv
PY_VER=$(python3 -c "import sys; print(f'{sys.version_info.major}.{sys.version_info.minor}')")
IDF_VER_HEADER="$IDF_DIR/components/esp_common/include/esp_idf_version.h"
IDF_MAJOR=$(grep "ESP_IDF_VERSION_MAJOR" "$IDF_VER_HEADER" | grep -oE '[0-9]+$')
IDF_MINOR=$(grep "ESP_IDF_VERSION_MINOR" "$IDF_VER_HEADER" | grep -oE '[0-9]+$')
VENV_DIR="$ESPRESSIF_DIR/python_env/idf${IDF_MAJOR}.${IDF_MINOR}_py${PY_VER}_env"
if [ -d "$VENV_DIR" ]; then
    export IDF_PYTHON_ENV_PATH="$VENV_DIR"
fi

# Build PATH with all ESP-IDF tools directly (bypasses check-python-dependencies)
_IDF_TOOL_PATHS=""
for tool_dir in "$ESPRESSIF_DIR"/tools/*/; do
    for ver_dir in "$tool_dir"*/; do
        if [ -d "$ver_dir" ]; then
            # Find bin directories or the directory itself
            if [ -d "${ver_dir}bin" ]; then
                _IDF_TOOL_PATHS="${_IDF_TOOL_PATHS:+${_IDF_TOOL_PATHS}:}${ver_dir}bin"
            else
                # Some tools have nested dirs (e.g. xtensa-esp-elf/esp-13.2.0/xtensa-esp-elf/bin)
                _found_nested_bin=0
                for nested in "$ver_dir"*/; do
                    if [ -d "${nested}bin" ]; then
                        _IDF_TOOL_PATHS="${_IDF_TOOL_PATHS:+${_IDF_TOOL_PATHS}:}${nested}bin"
                        _found_nested_bin=1
                    fi
                done
                # Some tools (ninja, ccache, dfu-util, idf-exe on Windows) ship the
                # executable directly in the version dir with no bin/ at all.
                if [ "$_found_nested_bin" = 0 ]; then
                    _IDF_TOOL_PATHS="${_IDF_TOOL_PATHS:+${_IDF_TOOL_PATHS}:}${ver_dir%/}"
                fi
                unset _found_nested_bin
            fi
        fi
    done
done

# venvs created by `python -m venv` put executables in bin/ on macOS/Linux but
# Scripts/ on Windows; VENV_DIR itself is cross-platform (installed by the IDF
# tools installer), so just pick whichever executable dir actually exists.
if [ -d "${VENV_DIR}/Scripts" ]; then
    VENV_BIN_DIR="${VENV_DIR}/Scripts"
else
    VENV_BIN_DIR="${VENV_DIR}/bin"
fi

export PATH="${VENV_BIN_DIR}:${_IDF_TOOL_PATHS}:${IDF_DIR}/components/espcoredump:${IDF_DIR}/components/partition_table:${IDF_DIR}/components/app_update:${IDF_DIR}/tools:${PATH}"
unset _IDF_TOOL_PATHS PY_VER VENV_DIR VENV_BIN_DIR IDF_VER_HEADER IDF_MAJOR IDF_MINOR

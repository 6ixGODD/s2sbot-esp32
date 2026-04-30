#!/usr/bin/env bash
# ============================================================================
# cmake-format pre-commit hook
# ============================================================================
# Description: Format staged CMakeLists.txt files using cmake-format (via uv).
#              Reformats in-place and re-stages the file.  Fails if any file
#              was modified.
# ============================================================================
set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
. "${SCRIPT_DIR}/common.sh"

# Git hooks run with a stripped-down PATH; extend it to include the locations
# where uv is typically installed (official installer → ~/.local/bin).
export PATH="${HOME}/.local/bin:/usr/local/bin:${PATH}"

# Require uv so we can invoke cmake-format from the project virtualenv.
require uv

ROOT="$(repo_root)"
CONFIG="${ROOT}/.cmake-format.yml"

if ! file_exists "$CONFIG"; then
    die ".cmake-format.yml not found at repo root: ${CONFIG}"
fi

# Collect all staged CMakeLists.txt / *.cmake files passed by pre-commit.
# pre-commit passes filenames as positional arguments.
if [ "$#" -eq 0 ]; then
    log_info "No CMake files to format."
    exit 0
fi

CHANGED=0
for f; do
    if ! file_exists "$f"; then
        log_debug "Skipping missing file: $f"
        continue
    fi

    log_step "cmake-format: $f"

    # Snapshot checksum before formatting.
    BEFORE="$(cksum "$f")"

    if ! uv run cmake-format --config-files "$CONFIG" --in-place "$f"; then
        log_error "cmake-format failed on: $f"
        exit 1
    fi

    if [ "$(cksum "$f")" != "$BEFORE" ]; then
        log_warn "Reformatted: $f"
        CHANGED=1
    fi
done

if [ "$CHANGED" -eq 1 ]; then
    log_warn "cmake-format made changes — stage the formatted files and commit again."
    exit 1
fi

log_success "All CMake files are properly formatted."

#!/bin/sh
# ============================================================================
# cmake-format pre-commit hook
# ============================================================================
# Description: Format staged CMakeLists.txt files using cmake-format (via uv).
#              Reformats in-place and re-stages the file.  Fails if any file
#              was changed, so the user sees the diff before committing.
# ============================================================================
set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
. "${SCRIPT_DIR}/common.sh"

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
    if ! uv run cmake-format --config-files "$CONFIG" --in-place "$f"; then
        log_error "cmake-format failed on: $f"
        exit 1
    fi

    # Re-stage the file so the formatted version is what gets committed.
    git add "$f"

    # Track whether any file was actually modified.
    if ! git diff --quiet HEAD -- "$f" 2>/dev/null; then
        log_warn "Reformatted: $f"
        CHANGED=1
    fi
done

if [ "$CHANGED" -eq 1 ]; then
    log_warn "cmake-format made changes. Review the diff and commit again."
    exit 1
fi

log_success "All CMake files are properly formatted."

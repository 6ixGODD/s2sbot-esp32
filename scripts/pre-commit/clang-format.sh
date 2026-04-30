#!/bin/sh
# ============================================================================
# clang-format pre-commit hook
# ============================================================================
# Description: Format staged C/C++ source files using clang-format.
#              Reformats in-place (using the project .clang-format) and
#              re-stages the file.  Fails if any file was changed so the
#              user sees the diff before committing.
# ============================================================================
set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
. "${SCRIPT_DIR}/common.sh"

require clang-format

ROOT="$(repo_root)"
CONFIG="${ROOT}/.clang-format"

if ! file_exists "$CONFIG"; then
    die ".clang-format not found at repo root: ${CONFIG}"
fi

if [ "$#" -eq 0 ]; then
    log_info "No C/C++ files to format."
    exit 0
fi

CHANGED=0
for f; do
    if ! file_exists "$f"; then
        log_debug "Skipping missing file: $f"
        continue
    fi

    log_step "clang-format: $f"
    if ! clang-format --style=file:"$CONFIG" -i "$f"; then
        log_error "clang-format failed on: $f"
        exit 1
    fi

    git add "$f"

    if ! git diff --quiet HEAD -- "$f" 2>/dev/null; then
        log_warn "Reformatted: $f"
        CHANGED=1
    fi
done

if [ "$CHANGED" -eq 1 ]; then
    log_warn "clang-format made changes. Review the diff and commit again."
    exit 1
fi

log_success "All C/C++ files are properly formatted."

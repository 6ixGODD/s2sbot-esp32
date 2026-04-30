#!/usr/bin/env bash
# ============================================================================
# clang-format pre-commit hook (manual / CI use)
# ============================================================================
# Description: Format C/C++ source files using the system clang-format.
#              Reformats in-place and re-stages the file.  Fails if any file
#              was modified.
# Usage: sh scripts/pre-commit/clang-format.sh <file> [<file> ...]
# ============================================================================
set -eu

SCRIPT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
. "${SCRIPT_DIR}/common.sh"

# Git hooks run with a stripped-down PATH; extend it to cover common locations
# where clang-format is installed (apt → /usr/bin, brew → /usr/local/bin).
export PATH="/usr/local/bin:/usr/bin:/bin:${HOME}/.local/bin:${PATH}"

ROOT="$(repo_root)"
CONFIG="${ROOT}/.clang-format"

if ! file_exists "$CONFIG"; then
    die ".clang-format not found at repo root: ${CONFIG}"
fi

# Locate clang-format; accept any installed version.
CLANG_FORMAT=""
for candidate in clang-format clang-format-19 clang-format-18 clang-format-17; do
    if command_exists "$candidate"; then
        CLANG_FORMAT="$candidate"
        break
    fi
done
if [ -z "$CLANG_FORMAT" ]; then
    die "clang-format is not installed. Run: apt install clang-format"
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

    log_step "clang-format ($CLANG_FORMAT): $f"
    BEFORE="$(cksum "$f")"

    if ! "$CLANG_FORMAT" --style=file:"$CONFIG" -i "$f"; then
        log_error "clang-format failed on: $f"
        exit 1
    fi

    if [ "$(cksum "$f")" != "$BEFORE" ]; then
        log_warn "Reformatted: $f"
        CHANGED=1
    fi
done

if [ "$CHANGED" -eq 1 ]; then
    log_warn "clang-format made changes — stage the formatted files and commit again."
    exit 1
fi

log_success "All C/C++ files are properly formatted."

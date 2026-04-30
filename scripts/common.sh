#!/usr/bin/env bash
# ============================================================================
# Common Utilities Library
# ============================================================================
# Description: Shared functions and utilities for all s2sbot scripts.
# Usage: . "$(dirname "$0")/common.sh"
# ============================================================================

# ---------------------------------------------------------------------------
# Terminal colours (disabled when not a TTY or NO_COLOR is set)
# ---------------------------------------------------------------------------
if [ -t 1 ] && [ -z "${NO_COLOR:-}" ]; then
    GREEN='\033[0;32m'
    RED='\033[0;31m'
    YELLOW='\033[1;33m'
    BLUE='\033[0;34m'
    CYAN='\033[0;36m'
    NC='\033[0m'
else
    GREEN='' RED='' YELLOW='' BLUE='' CYAN='' NC=''
fi

# ---------------------------------------------------------------------------
# Logging
# ---------------------------------------------------------------------------
log_info() {
    printf "${GREEN}[INFO]${NC}  %s\n" "$1"
}

log_warn() {
    printf "${YELLOW}[WARN]${NC}  %s\n" "$1"
}

log_error() {
    printf "${RED}[ERROR]${NC} %s\n" "$1" >&2
}

log_step() {
    printf "${BLUE}[STEP]${NC}  %s\n" "$1"
}

log_success() {
    printf "${GREEN}[OK]${NC}    %s\n" "$1"
}

log_debug() {
    if [ "${DEBUG:-0}" = "1" ]; then
        printf "${CYAN}[DEBUG]${NC} %s\n" "$1"
    fi
}

# ---------------------------------------------------------------------------
# Control flow
# ---------------------------------------------------------------------------

# die MSG [EXIT_CODE]
#   Print an error and exit.
die() {
    log_error "$1"
    exit "${2:-1}"
}

# require CMD
#   Exit with a helpful message if CMD is not found on PATH.
require() {
    command_exists "$1" || die "'$1' is required but not found. Please install it first."
}

# confirm PROMPT
#   Ask for y/N confirmation. Returns 0 for yes, 1 for no.
confirm() {
    _prompt="${1:-Are you sure?}"
    printf "${YELLOW}%s${NC} (y/N): " "$_prompt"
    read -r _response
    case "$_response" in
        [yY][eE][sS] | [yY]) return 0 ;;
        *) return 1 ;;
    esac
}

# ---------------------------------------------------------------------------
# Predicates
# ---------------------------------------------------------------------------

command_exists() {
    command -v "$1" >/dev/null 2>&1
}

dir_exists() {
    [ -d "$1" ]
}

file_exists() {
    [ -f "$1" ]
}

# ---------------------------------------------------------------------------
# Filesystem helpers
# ---------------------------------------------------------------------------

# ensure_dir DIR
#   Create DIR (and parents) if it does not exist.
ensure_dir() {
    dir_exists "$1" || mkdir -p "$1" || die "Failed to create directory: $1"
}

# repo_root
#   Print the absolute path of the git repository root, or die.
repo_root() {
    git rev-parse --show-toplevel 2>/dev/null || die "Not inside a git repository."
}

# ---------------------------------------------------------------------------
# Formatting
# ---------------------------------------------------------------------------

print_separator() {
    printf '%s\n' '============================================================'
}

# print_header TITLE
#   Print a prominent section header.
print_header() {
    echo ""
    print_separator
    printf '  %s\n' "$1"
    print_separator
    echo ""
}

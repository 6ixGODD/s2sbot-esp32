#!/bin/sh
# ============================================================================
# Setup Dev Environment
# ============================================================================
# Description: Bootstrap a Python dev environment for s2sbot tooling.
#              Installs uv (if absent), syncs the virtualenv, and installs
#              pre-commit hooks.
# Usage: sh scripts/setup_dev.sh
# ============================================================================
set -eu

# Locate the repo root so the script works from any working directory.
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
. "${SCRIPT_DIR}/common.sh"

ROOT="$(repo_root)"
cd "$ROOT"

# ---------------------------------------------------------------------------
# 1. Install uv
# ---------------------------------------------------------------------------
print_header "Step 1 — Install uv"

UV_INSTALL_URL="https://astral.sh/uv/install.sh"

if command_exists uv; then
    log_info "uv is already installed: $(uv --version)"
else
    log_step "Downloading and running the uv installer..."
    _uv_tmp="$(mktemp)"
    if command_exists curl; then
        curl -LsSf "$UV_INSTALL_URL" -o "$_uv_tmp" || die "Failed to download uv installer."
    elif command_exists wget; then
        wget -qO "$_uv_tmp" "$UV_INSTALL_URL" || die "Failed to download uv installer."
    else
        rm -f "$_uv_tmp"
        die "Neither curl nor wget is available. Please install one and retry."
    fi
    sh "$_uv_tmp"
    rm -f "$_uv_tmp"

    # The installer places uv in ~/.local/bin; add it to PATH for this session.
    export PATH="${HOME}/.local/bin:${PATH}"

    command_exists uv || die "uv installation succeeded but 'uv' is still not on PATH. \
Open a new shell or add ~/.local/bin to your PATH."

    log_success "uv installed: $(uv --version)"
fi

# ---------------------------------------------------------------------------
# 2. Sync the virtualenv
# ---------------------------------------------------------------------------
print_header "Step 2 — uv sync"

log_step "Syncing dependencies from pyproject.toml / uv.lock..."
uv sync
log_success "Virtualenv synced."

# ---------------------------------------------------------------------------
# 3. Install pre-commit hooks
# ---------------------------------------------------------------------------
print_header "Step 3 — Install pre-commit hooks"

log_step "Running pre-commit install..."
uv run pre-commit install
log_success "pre-commit hooks installed."

# ---------------------------------------------------------------------------
# Done
# ---------------------------------------------------------------------------
print_header "Dev environment ready"
log_info "Activate the virtualenv with:  source .venv/bin/activate"
log_info "Or prefix commands with:       uv run <command>"

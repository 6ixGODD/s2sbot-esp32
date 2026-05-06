# Copilot Instructions

## Project Overview

s2sbot is an ESP32 firmware project implementing a speech-to-speech bot. It is built with **ESP-IDF** (Espressif IoT Development Framework) and communicates with a remote speech-to-speech server over WebSocket using **Protocol Buffers** (nanopb). The full pipeline: microphone → AFE/wakeword → WebSocket stream (ASR → LLM → TTS) → speaker.

## Build, Flash & Monitor

This project uses the ESP-IDF CMake build system. All firmware commands require the ESP-IDF environment to be activated.

```sh
# Build
idf.py build

# Flash to device
idf.py flash

# Serial monitor
idf.py monitor

# Combined (common during dev)
idf.py build flash monitor

# Configure Kconfig options (hardware pins, codec selection, wakeword engine, etc.)
idf.py menuconfig
```

## Dev Environment Setup

```sh
# Bootstrap Python tooling (installs uv, syncs venv, installs pre-commit hooks, clang-format)
sh scripts/setup-dev.sh

# After setup, run pre-commit manually
uv run pre-commit run --all-files

# Python linting/formatting (via ruff)
uv run ruff check .
uv run ruff format .
```

## Architecture

```
main/           — app_main entry point; starts up all components
components/
  s2s/          — core speech-to-speech session logic (WebSocket client + protocol state)
  audio/        — audio pipeline: AFE, I2S codec drivers (ES8311, no-codec), wakeword, OGG demux
  net/          — network layer: net.c (Wi-Fi/IP init), websocket.c, http.c, mqtt.c
  mqtt/         — MQTT client component
  proto/        — Protobuf schema (s2s.proto) + nanopb-generated bindings (s2s.pb.c/h)
  nanopb/       — vendored nanopb embedded protobuf library
  provisioning/ — Wi-Fi provisioning (BLE/SoftAP)
  cert/         — TLS certificate management
  state/        — application state machine
  led/          — LED strip control
  piezoelectric/— buzzer driver
  sysinfo/      — device info
  ota/          — OTA firmware updates
tools/          — Python dev tools package (s2sbot-tools, CLI entry point: s2sbot-tools)
scripts/        — shell scripts: setup-dev.sh, pre-commit hooks (cmake-format, clang-format)
```

## Wire Protocol (WebSocket + Protobuf)

The device communicates over WebSocket binary frames. Each frame carries exactly one serialised protobuf message (no additional framing header).

- **Client → Server**: `ClientMessage` with `oneof { AudioChunk, FinishAudio }`
- **Server → Client**: `ServerMessage` with `oneof { ASRStartEvent, ASRResultEvent, ASRDoneEvent, LLMDeltaEvent, LLMDoneEvent, TTSDeltaEvent, TTSDoneEvent, ErrorEvent }`

To regenerate protobuf bindings after editing `components/proto/s2s.proto`:
```sh
python scripts/gen_proto.py
```

## Partition Layout

Custom partition table (`partitions.csv`):
- `nvs` — NVS storage
- `phy_init` — PHY calibration
- `factory` — main app (2.5 MB)
- `model` — SPIFFS for wakeword models (1.4 MB)

## C Code Conventions

- **Style**: based on Google C++ style via clang-format (`.clang-format`), with Stroustrup brace style, 88-column limit, 4-space indent.
- **Return type placement**: top-level function return types go on their own line (`AlwaysBreakAfterReturnType: TopLevel`).
- **Pointer alignment**: right (`int *p`, not `int* p`).
- **Include ordering** (strictly enforced by clang-format):
  1. C standard library (`<stdio.h>`, etc.)
  2. Other angle-bracket system/third-party headers
  3. FreeRTOS (`"freertos/..."`)
  4. ESP-IDF core (`"esp_*.h"`)
  5. ESP-IDF drivers (`"driver/..."`)
  6. ESP-IDF lower-level (`"hal/..."`, `"soc/..."`, `"rom/..."`)
  7. Managed components (esp_codec_dev, esp-sr, audio pipeline ecosystem)
  8. Nanopb / generated protobuf (`"pb*.h"`, `"*.pb.h"`)
  9. Project public headers (`"s2sbot/..."`)
  10. Other quoted headers (component-local private headers)
- **Public project headers** live under `components/<name>/include/s2sbot/<name>/` and are included as `"s2sbot/<name>/..."`.
- **ESP-IDF error macros** (`ESP_ERROR_CHECK`, `ESP_RETURN_ON_ERROR`, `ESP_GOTO_ON_ERROR`, `ESP_RETURN_ON_FALSE`, `ESP_GOTO_ON_FALSE`) are declared as `StatementMacros` in clang-format — do not wrap their arguments unnecessarily.
- Use `ESP_LOGI`/`ESP_LOGE`/`ESP_LOGW` with a `#define TAG "COMPONENT_NAME"` at the top of each `.c` file.

## Python Code Conventions

- Python ≥ 3.12, managed with **uv** (`uv sync`, `uv run`).
- Linted and formatted with **ruff** (120-char line length, double quotes, Google docstring convention).
- Import sections in order: future → stdlib → third-party → first-party (`s2s`) → local. Each import on its own line.

## IDF Component Dependencies

Managed via `idf_component.yml`. Current managed components:
- `espressif/esp-sr` — wakeword/ASR models (WakeNet, Multinet)
- `espressif/led_strip` — addressable LED strip
- `espressif/esp_websocket_client` — WebSocket transport
- `espressif/esp_codec_dev` — codec abstraction (ES8311)

## Kconfig / Hardware Configuration

Hardware-specific settings (GPIO pin assignments, codec selection, wakeword engine, AFE/AEC enable) are all Kconfig options configured via `idf.py menuconfig`. Do not hardcode GPIO numbers or hardware parameters in source files — always add a `Kconfig` entry.

---
name: esp32-gui-dev
description: >-
  Provides architectural context, guidelines, and workflows for developing the ESP32_GUI project, 
  which runs on a Waveshare ESP32-S3-POE-ETH-8DI-8DO board with a web dashboard, MQTT, and Telegram integration.
---

# ESP32_GUI Development Skill

This skill provides essential context and instructions for developing, debugging, and modifying the **ESP32_GUI** project.

## 1. Project Overview

The project is an industrial IoT dashboard and control system firmware designed for the **Waveshare ESP32-S3-POE-ETH-8DI-8DO** hardware. 
It features:
- **Digital I/O**: 8 Digital Inputs (DI) via optocouplers and 8 Digital Outputs (DO) controlled via a PCF8574 I2C expander.
- **Web Dashboard**: An interactive UI (HTML/CSS/JS) served by the ESP32 from the LittleFS `data/` folder.
- **Cloud/MQTT Integration**: A WebSocket bridge wrapper (`WsClientBridge`) over `WebSocketsClient` passing data to `PubSubClient` for secure cloud MQTT (e.g., Replit backend).
- **Telegram Bot**: Responds to commands (`/status`, `/health`, `/metrics`, `/do on N`) and sends alerts.
- **OTA Updates**: Features a System Update section on the Web GUI allowing over-the-air firmware flashes directly via the browser.
- **Logic Rules Engine**: User-defined rules linking DI states to DO triggers, saved to `do_rules.json`.

## 2. Directory Structure

- `ESP32_GUI.ino`: The monolithic main firmware file (C++) containing all backend logic, web server endpoints, and loop tasks.
- `data/`: Contains the frontend assets flashed to the ESP32's LittleFS.
  - `index.html`: Main dashboard UI.
  - `iot.html`: MQTT configuration page.
  - `update.html`: System Update / OTA page.
  - `telegram.html`: Telegram bot settings.
  - `style.css`: Vanilla CSS for the web interface.
  - `*.json`: Default config files (`config.json`, `users.json`, `variables.json`).

## 3. Core Development Guidelines

### 3.1 Modifying the Firmware (`ESP32_GUI.ino`)
- **Mutexes**: The project uses FreeRTOS tasks and `SemaphoreHandle_t`. When modifying I/O (`outputState`), ensure you respect `ioMutex` to prevent race conditions.
- **Digital Outputs**: Always use the `setOutput(int ch, bool state)` function rather than directly writing to the PCF8574. This ensures MQTT events are published and state is tracked.
- **Asynchronous Loop**: The `loop()` must not block. Use `millis()` for timing instead of `delay()`, except inside dedicated FreeRTOS tasks (like `telegramTask`).
- **Memory Management**: When adding JSON serialization/deserialization with `ArduinoJson`, carefully manage `StaticJsonDocument` sizes to avoid heap fragmentation and stack overflows, especially given the memory constraints of the web server routes.

### 3.2 Modifying the Frontend (`data/`)
- **Vanilla JS/CSS**: The frontend is built without heavy frameworks. Ensure all DOM manipulation and state updates continue to use standard JavaScript.
- **Endpoint Structure**: The JS fetches real-time data via specific endpoints (e.g., `/status`, `/getHealth`, `/getDoRules`). If you add UI elements requiring new backend data, implement the corresponding `server.on("/...", ...)` handler in the `.ino` file.
- **File Limits**: Keep frontend files lightweight. Avoid importing large external libraries unless absolutely necessary.

## 4. Key JSON Configuration Files (Stored in LittleFS)

When the ESP32 boots, it loads configurations from the following files:
- `config.json`: Network and MQTT defaults.
- `do_rules.json`: The dynamic logic rules (AND/OR/IF) linking inputs to outputs.
- `telegram.json`: Bot token, chat ID, and notification thresholds.
- `metrics.json`: Persists the machine state, runtime, downtime, and trigger counts to survive reboots.
- `users.json`: Credentials for the web server basic auth.

## 5. Troubleshooting & Debugging

- **Network Fallback**: The system prefers Ethernet (`eth_connected`). If modifying network code, account for both WiFi and ETH edge cases.
- **Telegram Polling**: The Telegram task polls every `TG_POLL_MS` (2.5s). If the bot misses messages, check the `tgTaskHandle` stack and ensure network connectivity.
- **MQTT Bridge**: The custom `WsClientBridge` converts binary WebSocket payloads into a stream `PubSubClient` can read. If MQTT fails to connect via WSS, inspect this bridge's buffer (`WSBRIDGE_RX_SIZE`).

## 6. Project Rules

For coding standards, GitHub workflows, and frontend UI design patterns (including the SIB Color Palette), please refer to the `rules.md` file located in `.agents/rules/`.

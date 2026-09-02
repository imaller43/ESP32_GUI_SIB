---
description: Global project rules for ESP32_GUI_SIB, including GitHub workflows, language requirements, and coding standards.
---

# Project Rules

The following rules MUST be followed at all times when working on this project:

## 1. AI Workflow & GitHub
- **Auto-commit and Push:** Every time you (the AI) make a modification to the code, you MUST automatically `git commit` and `git push origin main` (or the active branch) to GitHub once the changes are completed and verified. Do not wait for the user to ask you to push.

## 2. Language Requirement
- **English Only:** All code variables, function names, and code comments MUST be written in English.

## 3. Firmware Coding Standards
- **Digital Outputs:** Always use the `setOutput(int ch, bool state)` function rather than directly writing to the PCF8574 expander. This ensures MQTT events are published and state is correctly tracked.

## 4. Frontend & UI Design Rules
- **Color Palette:** The Sapura Industrial Berhad (SIB) Color Palette MUST be used for UI changes:
  - Smooth Orange (Primary/Accents): `#f92d16`
  - Dark Blue (Header/Dark Bg): `#20203b`
  - Smooth Grey (Text/Borders/Dark Login Box): `#4a4b54`
  - Smooth White (Light Bg/Cards): `#f0f2f5` / `#ffffff`
- **Form Layouts:** Use `display: grid` for complex horizontal form rows (like DI/DO topic rows). Avoid `display: flex` for these elements to prevent text truncation or squishing.
- **Dynamic Widths:** For long prefix strings in forms, use percentage-based max-widths (e.g., `max-width: 55%;`) instead of hard-coded pixels.
- **Strict Blanking Logic:** When loading configs into the UI, explicitly check for factory default combinations and render inputs as empty/blank, forcing the user to make a deliberate choice.
- **Fetch API Guarding & Reboots:** ESP32 reboots cause abrupt network drops. Standard `fetch()` calls will hang endlessly. ALWAYS wrap frontend fetch polling with an `AbortController` (e.g., `fetchTimeout`). Moreover, separate the `fetch()` call from `.json()` parsing. If the ESP32 reboots, it destroys the user session token in RAM, meaning reconnect attempts will be redirected (HTTP 302) to the `/login` HTML page. Catch `res.redirected` to automatically reload the page and prevent silent `SyntaxError` crashes that freeze the UI offline.

## 5. System Update (OTA) Architecture
- **Isolated OTA Branch**: Compiled binaries (`firmware.bin`, `littlefs.bin`) and `version.json` are automatically pushed to an isolated `OTA-Update` branch via GitHub Actions. The ESP32 MUST fetch `version.json` from the `/OTA-Update/` URL path, not `/main/`.
- **LittleFS RAM Caching:** Flashing `littlefs.bin` (`U_SPIFFS`) completely wipes the partition. To prevent the ESP32 from "losing its memory" and network identity, all stateful/private JSON files (`config.json`, `telegram.json`, `do_rules.json`, `metrics.json`, `users.json`) MUST be read into RAM strings via `backupConfigs()` before `Update.begin()`, and written back via `restoreConfigs()` after `Update.end()`. 
- **Static vs Stateful:** Only backup stateful files to RAM. Static files (`variables.json`, `.html`, `.css`, images) must NOT be backed up to RAM, as we want them to be overwritten by the latest developer UI updates.
- **Float Versioning Check:** Firmware and LittleFS versions are parsed as floats. Due to C++ float precision errors, never compare them directly (`==`). Always use a tolerance threshold: `(newFwVer - (float)FIRMWARE_VERSION) > 0.001`.
- **MQTT Broadcasting:** Always mirror critical Telegram status updates (like OTA steps) to MQTT (e.g., `mqtt.publish("esp32/ota/status", ...)`). This ensures the Node-RED dashboard stays in sync in real-time.

## 6. FreeRTOS Safety & Memory
- **Task Handle Validation:** Never use `eTaskGetState(handle) == eDeleted` to check if a task has finished. Once a task executes `vTaskDelete(NULL)`, its memory is freed and the handle becomes invalid, causing memory corruption. Use a `volatile bool` flag (e.g., `otaRunning`) to track task states.
- **Queue Flushing Before Reboot:** Telegram messages are queued in RAM. When calling `ESP.restart()`, always use a `while` loop to check `uxQueueMessagesWaiting(tgOutQueue) > 0` (with a timeout) to give the background task time to transmit final messages before the RAM is erased.
- **JSON Deserialization Safety:** Always allocate ample memory for `StaticJsonDocument` (e.g., `512` or `1024`), especially when parsing long strings (like GitHub URLs). If memory is too tight, `deserializeJson` will fail silently, causing catastrophic logic breaks without throwing an exception.

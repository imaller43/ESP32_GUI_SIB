---
name: esp32-architecture-guide
description: >-
  Detailed architectural guide for the ESP32_GUI project, focusing on the REST API HTTP polling mechanism, 
  LittleFS file management, and hardware interaction flow.
---

# ESP32 Dashboard Architecture & Workflow

This skill provides a deep dive into how the ESP32_GUI project connects its C++ backend firmware with its HTML/JS frontend, and how it manages persistent state memory.

## 1. The HTTP Polling Mechanism (No WebSockets for UI)
Unlike many ESP32 projects that use WebSockets for real-time UI updates, this dashboard relies on a lightweight **RESTful API** combined with **HTTP Short-Polling**.

- **Frontend (`index.html` & `app.js`)**: Uses JavaScript `setInterval()` loops to continuously send `fetch()` GET requests to the ESP32.
  - `/status` (Every 500ms): Fetches live DI/DO pin states.
  - `/getMetrics` (Every 1000ms): Fetches runtime, downtime, cycle time, and reject counts.
  - `/getHealth` (Every 2000ms): Fetches chip temperature, free heap, CPU frequency, and Wi-Fi RSSI.
- **Smart Polling**: The frontend uses `document.visibilityState !== 'hidden'` to stop polling when the browser tab is not active, saving ESP32 processing power and bandwidth.
- **Backend (`ESP32_GUI.ino`)**: The `WebServer` listens on port 80 and maps endpoints to handler functions (e.g., `server.on("/status", handleStatus)`). These functions build a JSON string and return it with a `200 OK` HTTP status.

## 2. Hardware Control Flow (Activating a DO)
When a user clicks a button to toggle a Digital Output on the dashboard, the following sequence occurs:
1. **Button Click**: JavaScript triggers `toggle(ch)`.
2. **API Request**: JS executes `fetch('/toggle?ch=' + ch)`.
3. **Firmware Intercept**: The ESP32's `WebServer` intercepts the `/toggle` endpoint and triggers `handleToggle()`.
4. **Hardware Update**: `handleToggle()` calculates the new state and calls `setOutput(ch, state)`.
5. **I2C Command**: `setOutput()` updates the internal state and writes to the **PCF8574** I2C expander chip (at address `0x20`) to physically trigger the relay/output.

## 3. LittleFS Memory Management
The project utilizes the ESP32's onboard flash memory (formatted as LittleFS) to cleanly separate backend logic from frontend presentation and state.

### A. Serving Frontend Web Files
All web assets are stored as files in the `data/` folder and flashed to LittleFS. The `WebServer` uses a custom `serveStatic()` function to read these files (`index.html`, `style.css`, `app.js`, `.png` logos) and serve them to the browser.

### B. Persistent State & Configuration
Because RAM clears on reboot, LittleFS acts as a hard drive to save crucial state and configuration data via JSON files:
- `config.json`: Network and IoT/MQTT parameters.
- `telegram.json`: Telegram bot credentials and alert thresholds.
- `do_rules.json`: Custom automation logic rules (IF/AND/OR).
- `metrics.json`: Crucial for the dashboard to resume accurate metrics (runtime, downtime) after a power failure.
- `users.json`: Login credentials.

## 4. Why this Architecture Matters
- **Stability**: Using HTTP polling instead of WebSockets for the UI ensures the ESP32 does not run out of Free Heap memory by holding multiple persistent TCP connections open. Connections are opened, data is served, and connections are immediately closed.
- **Maintainability**: Separating HTML/CSS/JS into LittleFS means the web design can be updated via OTA without recompiling the monolithic C++ firmware.

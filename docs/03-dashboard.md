# 03 — Dashboard Pages

Once logged in, navigate between pages using the top navigation bar.

---

## Dashboard

The main overview page. Shows everything at a glance:

- **Live clock** — updates every second in the browser
- **Machine state** — RUNNING (green), STOPPED (red), or UNKNOWN (grey)
- **Network info** — current IP address and whether the board is on Ethernet or Wi-Fi
- **Process metrics** — Runtime, Downtime, Cycle Time, Reject Count, Efficiency
- **ESP32 health** — chip temperature, free RAM, Wi-Fi RSSI (or "Ethernet"), CPU speed, uptime
- **Digital Inputs (DI1–DI8)** — live ON/OFF badges, updated every 500 ms
- **Digital Outputs (DO1–DO8)** — live state badges with **Toggle** buttons
- **DO Logic Rules** — Automatically drive outputs based on input state conditions (AND/OR logic)

Clicking a **Toggle** button immediately flips that output. The badge updates on the next poll (within 500 ms).

**Efficiency** defaults to `Runtime ÷ (Runtime + Downtime) × 100%`. You can override this with a custom formula — save one in the custom efficiency section and it will be used here automatically.

### DO Logic Rules 

Automatically control outputs based on input states — no PLC required. Each rule defines conditions based on DI states and drives a DO output when those conditions are met.

**How it works:**
- Each rule targets one **Digital Output** (DO1–DO8)
- The rule has one or more **conditions** on DI states (ON or OFF)
- Conditions are chained with **AND** or **OR** logic
- When the overall condition evaluates to true, the output turns ON; when false, it turns OFF
- Rules are re-evaluated automatically every time any DI state changes

**Example rule:**
```
IF DI2 = ON  AND  DI3 = OFF  →  DO1 = ON
```
This turns on DO1 (e.g. a green indicator light) whenever the Start button (DI2) is pressed and the Reset button (DI3) is not active.

Up to **16 rules** can be defined, each with up to **8 conditions**.

> **Note:** Rules run automatically and will override manual toggles from the dashboard whenever a DI state changes. To stop a rule from controlling an output, disable it with the toggle or delete it.

Click **Add Rule**, configure the output, add conditions, then **Save Rules**. Rules take effect immediately.

---

## `/iot` - IoT Config

All connectivity and automation configuration lives on this single page for **MQTT** with Prefix Base Topic. 

---

### MQTT

Connect the ESP32 to an MQTT broker to publish DI/DO state changes and RS485 register values, and receive remote DO control commands.

<table>
<tr>
<td valign="top" width="60%">

**Connection mode:**


| Mode | Use when |
|---|---|
| **Local TCP** | Connecting to a broker on your LAN (e.g. Mosquitto on port 1883) |
| **Local TLS** | Same as above but with TLS encryption (port 8883) |
| **Cloud WS** | Connecting to a cloud broker over WebSocket (e.g. HiveMQ) |
| **Cloud WSS** | Cloud broker over secure WebSocket — port 8084, path `/mqtt` |

</td>
<td valign="top" width="40%" align="center">

<img src="https://github.com/user-attachments/assets/b228a2bd-5838-476c-aaa5-f50edfd95191" alt="image" width="100%" />

</td>
</tr>
</table>

**DI/DO Topics:**

Each DI and DO channel has two fields:
- **Topic** — the MQTT topic string for that channel
- **Heartbeat (ms)** — if set to a value greater than 0, the ESP32 will publish the current state on that topic at this interval in milliseconds, even if nothing changed. Set to `0` for on-change-only publishing.

> Example: set DI1 topic to `factory/di1` and heartbeat to `5000` — the ESP32 will publish `"1"` or `"0"` to `factory/di1` every 5 seconds, and also immediately on any state change.

Changes take effect immediately after clicking **Save** — no reboot required.
<p align="center"><img src="https://github.com/user-attachments/assets/fd362b15-0472-4858-a44f-8065c705f2e6" alt="image" width="400" /></p>

---

## `/telegram` — Telegram Bot

Configure the Telegram bot for push alerts and remote commands. See [04 — Telegram Setup](04-telegram-setup.md) for full setup instructions.

- Shows bot status: inactive / active / failed
- Bot token and Chat ID entry fields
- Reject alert threshold (send an alert every N rejects; set to 0 to disable)
- **Send Test Message** button to verify the bot is working

<p align="center"><img src="https://github.com/user-attachments/assets/14acb418-4a95-4ec2-a04d-66e62f4a7687" alt="image" width="400" /></p>

---

## `/update` — System Update (OTA)

Perform Over-The-Air (OTA) firmware updates directly from the browser without needing a USB cable.

- **Firmware Update**: Upload a compiled `.bin` file to update the main ESP32 firmware.
- **Filesystem Update**: Upload a LittleFS `.bin` file to update the web dashboard assets (`data/` folder).

**How to use:**
1. Click **Choose File** and select your compiled `.bin` file.
2. Click **Update Firmware** (or **Update Filesystem**).
3. Wait for the progress bar to reach 100%. The ESP32 will automatically reboot and apply the update.

> **Warning:** Do not close the browser tab or power off the ESP32 while the update is in progress, as this may corrupt the firmware.

---
## Next Step
→ [04 — Telegram Setup](04-telegram-setup.md)

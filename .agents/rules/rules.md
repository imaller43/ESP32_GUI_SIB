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

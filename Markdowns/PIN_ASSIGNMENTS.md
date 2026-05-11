# Mira — Pin Assignments Reference

Arduino Nano ESP32-S3 (ABX00083). All logic is 3.3 V.

---

## Active Assignments

### VEML7700 Lux Sensor (I2C via Stemma QT cable)

| Signal | Arduino Pin | GPIO | Stemma QT wire color |
|--------|-------------|------|----------------------|
| SDA | A4 | GPIO11 | Blue |
| SCL | A5 | GPIO12 | Yellow |
| VIN | 3.3V | — | Red |
| GND | GND | — | Black |

**Notes:**
- Connected via Stemma QT JST connector → jumper wire cable. No soldering.
- Breakout has onboard I2C pullup resistors — no external resistors needed.
- I2C address: `0x10`
- A4/A5 are the hardware I2C pins on this board. These are permanently reserved.

---

### Buttons (active LOW, internal pull-up)

Two tactile buttons. For each, one leg to the assigned Arduino pin, other leg to GND. No external resistors needed.

| Function | Arduino Pin | GPIO | Short press | Long press (700 ms) |
|----------|-------------|------|-------------|---------------------|
| BTN_MODE  | D9  | GPIO18 | If NORMAL → SOFT_PAUSE; else → NORMAL | Hard off toggle (HARD_OFF ↔ LOCKED_OUT) |
| BTN_CYCLE | D10 | GPIO21 | Advance to next state in enum order via `forceState()` | — (long press not used) |

**Firmware setup:**
```cpp
pinMode(BTN_MODE,  INPUT_PULLUP);
pinMode(BTN_CYCLE, INPUT_PULLUP);
// Pressed = LOW, released = HIGH
```

Both buttons are polled every 50 ms inside the non-blocking wait loop between lux ticks via `pollButton()`; events are dispatched by `handleButtonEvents()` (BTN_MODE) and `handleCycleButton()` (BTN_CYCLE).

---

## Full Pin Map (all header pins)

| Arduino Pin | GPIO | Status | Role |
|-------------|------|--------|------|
| D0 / RX0 | GPIO44 | ⛔ Reserved | UART RX — used by Serial monitor |
| D1 / TX0 | GPIO43 | ⛔ Reserved | UART TX — used by Serial monitor |
| D2 | GPIO5 | 🔲 Free | |
| D3 | GPIO6 | 🔲 Free | |
| D4 | GPIO7 | 🔲 Free | |
| D5 | GPIO8 | ⛔ Excluded | Shared with onboard RGB LED green channel |
| D6 | GPIO9 | 🔲 Free | |
| D7 | GPIO10 | 🔲 Free | |
| D8 | GPIO17 | 🔲 Free | |
| D9 | GPIO18 | ✅ In use | BTN_MODE |
| D10 | GPIO21 | ✅ In use | BTN_CYCLE |
| D11 | GPIO38 | 🔲 Free | |
| D12 | GPIO47 | 🔲 Free | (SPI CIPO — available if SPI not used) |
| D13 | GPIO48 | ⚠️ Avoid | LED_BUILTIN + SPI SCK — driving it toggles the built-in LED |
| A0 / D17 | GPIO1 | 🔲 Free | |
| A1 / D18 | GPIO2 | 🔲 Free | |
| A2 / D19 | GPIO3 | 🔲 Free | |
| A3 / D20 | GPIO4 | 🔲 Free | |
| A4 / D21 | GPIO11 | ⛔ Reserved | I2C SDA — VEML7700 |
| A5 / D22 | GPIO12 | ⛔ Reserved | I2C SCL — VEML7700 |
| A6 / D23 | GPIO13 | 🔲 Free | |
| A7 / D24 | GPIO14 | 🔲 Free | |
| B0 | GPIO46 | ⚠️ Avoid | Shared with onboard RGB LED red channel |
| B1 | GPIO8 | ⛔ Excluded | Same GPIO as D5 — RGB LED green (see D5) |
| 3.3V | — | ⛔ Reserved | VEML7700 VIN |
| VBUS | — | 🔲 Free | |
| VIN | — | 🔲 Free | External power input |
| GND | — | ✅ In use | Common ground rail |

---

## Excluded / Avoided Pins — Reasoning

### D0 / D1 (GPIO44 / GPIO43) — UART
Used by the hardware UART for the Serial monitor over USB. Driving these as GPIO during development would corrupt serial output and could interfere with flashing.

### D5 (GPIO8) — onboard RGB LED green
GPIO8 is physically wired to both the D5 header pin and the green channel of the onboard RGB LED. Using D5 as GPIO would cause the green LED to flicker. Skipped in favor of other pins.

### D13 (GPIO48) — LED_BUILTIN + SPI SCK
D13 is used for the built-in LED (LED_BUILTIN) and SPI clock. Driving it as a general output will blink the onboard LED alongside normal use. Not a hard conflict, but avoided to keep behavior clean.

### B0 (GPIO46) — onboard RGB LED red
Same situation as D5/GPIO8. GPIO46 is the red channel of the onboard RGB LED and also exposed on the left header as B0.

### A4 / A5 (GPIO11 / GPIO12) — I2C bus
Reserved for the VEML7700 lux sensor. Must not be reassigned.

---

## Abandoned Hardware

### HD44780 16×2 LCD (attempted 4/20/2026 — abandoned)

ESP32-S3 outputs 3.3V logic. HD44780 at 5V VDD requires VIH ≥ 3.5V (0.7 × VCC).
Signal levels fall below the input threshold regardless of supply rail used. Level shifting
was not pursued. No display is planned for the current design.

### 3-Button Scheme (abandoned alongside LCD)

A Mode / Confirm / Cycle button scheme was originally designed for a config screen that depended on the LCD. With the LCD abandoned, the config screen was dropped. The current design uses two buttons (BTN_MODE for NORMAL/pause/hard-off, BTN_CYCLE to walk through the state enum) — see the Buttons table above. Per-weekday DAY_TYPES configuration was also dropped; a single `WAKE_RAMP_TICKS` is used instead.

---

*Last updated: May 2026 (added BTN_CYCLE on D10/GPIO21)*

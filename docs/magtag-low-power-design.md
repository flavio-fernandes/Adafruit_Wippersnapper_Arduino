# MagTag Low-Power Display Mode Design

This document describes a local WipperSnapper v1 feature for making the
Adafruit MagTag use much less battery by avoiding routine eInk refreshes and
sleeping for long periods.

The goal is intentionally practical: keep the normal WipperSnapper behavior as
the default, then let a MagTag owner opt into a quiet, battery-first mode from
`secrets.json`.

## Background

The current MagTag display path draws a status bar/header and refreshes it while
the device is running. That is useful for interactive debugging, but it has two
costs for battery-powered eInk use:

- The status bar reserves part of the screen.
- The firmware periodically refreshes the eInk display only to update status
  information such as WiFi and MQTT state.

The request is based on these upstream discussions:

- <https://github.com/adafruit/Adafruit_Wippersnapper_Arduino/issues/928>
- <https://github.com/adafruit/Adafruit_Wippersnapper_Arduino/issues/928#issuecomment-4672976838>
- <https://github.com/adafruit/Adafruit_Wippersnapper_Arduino/issues/877>

Issue #928 identifies the current status bar path:

- `src/components/display/controller.cpp`
  - `DisplayController::Handle_Display_AddOrReplace()` calls
    `display->drawStatusBar(WS._config.aio_user)`.
  - `DisplayController::update()` periodically calls
    `hw_instance->updateStatusBar(rssi, 100, is_connected)`.
- `src/components/display/hardware.cpp`
  - `DisplayHardware::drawStatusBar()` forwards to the active display driver.
  - `DisplayHardware::updateStatusBar()` forwards to the active display driver.
- MagTag eInk drivers:
  - `src/components/display/drivers/dispDrvThinkInkGrayscale4T5.h`
  - `src/components/display/drivers/dispDrvThinkInkGrayscale4Eaamfgn.h`

Issue #877 asks generally for disabling status bars on displays. The MagTag
low-power mode uses that same idea as its first battery-saving step.

## Proposed User Configuration

Add an optional `magtag_low_power` object to `secrets.json`:

```json
{
  "magtag_low_power": {
    "enabled": true,
    "display_status_bar": false,
    "sleep_interval_minutes": 180,
    "awake_window_minutes": 10,
    "sleep_message": "zzz"
  }
}
```

Defaults:

- `enabled`: `false`
- `display_status_bar`: `true` unless low-power mode is enabled, then `false`
- `sleep_interval_minutes`: `180`
- `awake_window_minutes`: `10`
- `sleep_message`: `"zzz"`

The defaults preserve existing behavior unless the user explicitly opts in.

The values should be parsed into `secretsConfig` from:

- `src/provisioning/Config.h`
- `src/provisioning/ConfigJson.cpp`
- `src/provisioning/ConfigJson.h`

## Part 1: Hide The Status Bar

When `magtag_low_power.enabled` is true and
`magtag_low_power.display_status_bar` is false:

1. Do not call `drawStatusBar()` after the splash screen.
2. Do not call `updateStatusBar()` from the 60-second display update loop.
3. Let MagTag `writeMessage()` use the full eInk display height.

The display driver should own the layout detail. A small driver-level flag is
cleaner than scattering `STATUS_BAR_HEIGHT` checks through the controller.

Recommended shape:

- Add a `setStatusBarEnabled(bool enabled)` method to `DisplayHardware`.
- Add a virtual `setStatusBarEnabled(bool enabled)` method to `dispDrvBase`.
- Store `_status_bar_enabled` in the MagTag eInk drivers.
- In `writeMessage()`, use:
  - `top = _status_bar_enabled ? STATUS_BAR_HEIGHT : 0`
  - `y = _status_bar_enabled ? STATUS_BAR_HEIGHT + 4 : 4`

This keeps the default display behavior unchanged and makes the full-screen
layout local to the drivers that reserve the status bar.

## Part 2: Long Deep Sleep

When MagTag low-power mode is enabled, the firmware should spend most of its
life in ESP32-S2 deep sleep.

High-level behavior:

1. Boot or wake.
2. Connect to WiFi and Adafruit IO as usual.
3. Stay awake for the configured awake window.
4. During that awake window, run normal WipperSnapper processing so button
   presses can be sent to Adafruit IO.
5. Before sleeping, draw the configured sleep marker, such as `zzz`, as a small
   non-intrusive overlay on the eInk display.
6. Enter deep sleep for the configured interval.
7. Wake early if any MagTag front button is pressed.
8. On a button wake, queue the active-low button press so it can be published
   after MQTT is connected.
9. On wake, resume normal WipperSnapper processing for another awake window.
   The next display update can repaint over the small marker.

The first implementation should be MagTag-only:

```cpp
#if defined(ARDUINO_MAGTAG29_ESP32S2)
```

This avoids changing sleep behavior on other WipperSnapper boards.

## Wake Sources

Use ESP32-S2 wake sources:

- Timer wake for the long sleep interval.
- GPIO wake for the MagTag front buttons.

The exact MagTag button pin constants should come from the board variant when
available. If the variant does not expose stable names, add MagTag-only
constants in a small board-power helper and document the mapping there.

Button wake is also treated as a queued active-low button event. On
`ESP_SLEEP_WAKEUP_EXT1`, the firmware reads `esp_sleep_get_ext1_wakeup_status()`
and maps the GPIO bit in the wake mask back to the matching MagTag front-button
pin. The queued event uses that exact pin, the normal WipperSnapper `pin_event`
payload, and publishes a `LOW` value because the MagTag buttons use pull-ups and
wake on low.

Before enabling EXT1 wake, configure the front-button pins in the RTC GPIO
domain as input-only with RTC pull-ups enabled and pulldowns disabled. The
normal Arduino `INPUT_PULLUP` configuration is enough while the firmware is
awake, but it does not reliably hold the pins during ESP32-S2 deep sleep. In
physical testing, omitting RTC pull-ups allowed Button D/GPIO11 to float low and
cause an immediate false EXT1 wake before the intended Button B press.

The queued event is sent only after MQTT is connected. If Adafruit IO reports a
throttle window before the queued event is sent, the firmware waits for the
throttle to clear and retries with exponential backoff. The queued event expires
after 2 minutes; after that, it is dropped and the firmware continues normal
low-power behavior instead of keeping the MagTag awake indefinitely.

## Display Behavior Around Sleep

The eInk display should not refresh periodically in low-power mode.

Refreshes are allowed only when:

- A new display message is received from Adafruit IO.
- The firmware is about to enter deep sleep and draws the small sleep marker.
- A new display message arrives after wake and naturally repaints over the
  marker area.

The sleep marker should be deliberately small and simple. The firmware draws a
tiny white `zzz` chip at the bottom center of the display without clearing the
existing display buffer. The rest of the eInk content remains visible while the
MagTag is in deep sleep.

Sleep entry must wait for that marker refresh before turning off network state
or entering deep sleep. The MagTag driver's `drawSleepMarker()` calls the
ThinkInk/eInk `display()` method, and the low-power sleep path logs
`WS_MAGTAG_LOW_POWER_SLEEP_MARKER_BEGIN` before that blocking refresh and
`WS_MAGTAG_LOW_POWER_SLEEP_MARKER_DONE` after it returns. Only after the marker
is done does the firmware pause briefly, disconnect, configure wake sources, and
call `esp_deep_sleep_start()`. This ordering keeps the eInk refresh from being
cut short by power-state changes.

## Serial Test Backdoor

The MagTag build includes a small serial-only test backdoor so the workbench can
exercise the low-power code without waiting 180 minutes.

Commands:

```text
WSLP STATUS
WSLP BUTTON
WSLP AWAKE <seconds>
WSLP SLEEP [seconds]
```

Behavior:

- `WSLP STATUS` prints the current low-power state, timers, ESP sleep wake
  cause, EXT1 wake mask, and live A/B/C/D button levels.
- `WSLP BUTTON` emulates a button wake while the firmware is awake. This resets
  the awake window, logs the same high-level wake reason used by the low-power
  flow, and queues a `BUTTON_A` active-low pin event for publish after MQTT is
  connected and any throttle window has cleared.
- `WSLP AWAKE <seconds>` overrides the current boot's awake window, which is
  useful for testing automatic sleep without waiting 10 minutes.
- `WSLP SLEEP [seconds]` forces the normal sleep-entry path immediately. When a
  value is supplied, that value is used as the timer wake interval for the test.
  The sleep-entry path also logs A/B/C/D button levels immediately before EXT1
  wake is enabled, so a held or floating wake pin can be diagnosed from serial
  output.

Serial cannot press a physical GPIO while the ESP32-S2 is already in deep
sleep, so `WSLP BUTTON` is a logic-path emulator rather than a hardware wake
source. The real deep-sleep wake sources are still the timer and the four MagTag
front buttons.

## Placement In The Code

Suggested files:

- `src/provisioning/Config.h`
  - Add `magtagLowPowerConfig`.
  - Add it as a field on `secretsConfig`.
- `src/provisioning/ConfigJson.cpp`
  - Parse `magtag_low_power`.
  - Apply conservative defaults.
- `src/components/display/controller.cpp`
  - Skip initial and periodic status bar work when disabled.
- `src/components/display/hardware.h`
- `src/components/display/hardware.cpp`
  - Forward status-bar configuration to the driver.
  - Optionally expose a helper for writing the sleep marker.
- `src/components/display/drivers/dispDrvBase.h`
  - Add the virtual status-bar toggle.
- `src/components/display/drivers/dispDrvThinkInkGrayscale4T5.h`
- `src/components/display/drivers/dispDrvThinkInkGrayscale4Eaamfgn.h`
  - Make message layout conditional on the status-bar flag.
- `src/Wippersnapper_demo.ino`
  - Add MagTag-only low-power lifecycle calls around `wipper.run()`.
  - Start the awake timer before `wipper.connect()` so registration or
    configuration waits cannot keep a MagTag awake past the configured window.
  - Publish queued wake-button events through the same `pin_event` protobuf used
    by normal digital input handling.
- `src/Wippersnapper.cpp`
  - Call a weak application background hook from blocking network, throttle, and
    configuration waits so the MagTag sleep deadline can still be enforced.

If the sleep logic grows beyond a few small functions, add a MagTag-specific
helper instead of turning `Wippersnapper_demo.ino` into a power-management file.

## Awake Window Timing

The awake window starts after boot/wake setup has completed enough for
WipperSnapper to run normally. A simple first rule is:

- Start the awake timer after `wipper.connect()` returns.
- Keep calling `wipper.run()` until `awake_window_minutes` expires.
- Then draw the sleep marker and enter deep sleep.

The default awake window is 10 minutes because it gives a user time to press a
button and lets WipperSnapper publish state without keeping WiFi up all day.

## Open Questions Before Implementation

The feature is clear enough to implement, but these choices should be confirmed
before making it permanent:

- Should the default sleep interval be 3 hours, or do you want a different
  value?
- Should button wake apply to all four front buttons, or only selected buttons?
- Should this remain a local fork feature only, or should the status-bar config
  be shaped to resemble the future WipperSnapper v2 device setting?

## Validation Plan

Build:

```sh
tools/magtag-build
```

Flash app-only to SLOT1:

```sh
tools/magtag-flash-workbench --app-only --yes
```

Monitor serial:

```sh
tools/magtag-monitor-workbench
```

Expected validation:

- Firmware boots and connects normally.
- With low-power mode disabled, current display/status behavior is unchanged.
- With low-power mode enabled, the status bar is not drawn.
- Display messages can use the full MagTag eInk area.
- The serial log reports entering deep sleep.
- The serial log reports `WS_MAGTAG_LOW_POWER_SLEEP_MARKER_BEGIN` followed by
  `WS_MAGTAG_LOW_POWER_SLEEP_MARKER_DONE` before the board enters deep sleep.
- The screen shows the configured sleep marker as a small overlay, with the
  existing display content still visible.
- Timer wake resumes the firmware.
- A front-button press wakes the device before the timer expires.
- After a front-button wake, the serial log reports
  `WS_MAGTAG_LOW_POWER_BUTTON_QUEUED` and then either
  `WS_MAGTAG_LOW_POWER_BUTTON_PUBLISHED` or
  `WS_MAGTAG_LOW_POWER_BUTTON_DROPPED reason=timeout`.
- After a button wake, the firmware remains awake for 10 minutes by default.
- Private `secrets.json` and `config/workbench.env` remain ignored by git.

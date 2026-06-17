# MagTag Button Wake Test Notes

Date: 2026-06-17

## Current repo state

- Branch: `flavio-magtag-local-test`
- Pushed commit: `6957dfc4 Fix MagTag low-power wake and workbench monitor`
- The WipperSnapper repo was clean after the failed diagnostic attempt was
  reverted.
- The attempted diagnostic edit to `src/Wippersnapper_demo.ino` was not flashed
  successfully and was removed before stopping.

## What is already proven

- The latest pushed firmware builds successfully for
  `adafruit_magtag29_esp32s2`.
- App-only flashing to the MagTag on workbench `SLOT1` succeeded earlier, with
  flash hash verification.
- Forced deep sleep via `WSLP SLEEP 10` worked:
  - Serial printed `WS_MAGTAG_LOW_POWER_SLEEP seconds=10`.
  - Serial printed `WS_MAGTAG_LOW_POWER_SLEEP_MARKER_BEGIN`.
  - Serial printed `WS_MAGTAG_LOW_POWER_SLEEP_MARKER_DONE`.
  - RFC2217 serial disconnected immediately afterward, consistent with deep
    sleep.
  - The camera captured the eInk sleep marker at
    `artifacts/magtag-deep-sleep-latest.jpg`.
  - After the 10-second timer, SLOT1 recovered and serial became reachable.
- The queued button emulator path works while awake:
  - `WSLP BUTTON` queues `BUTTON_A`.
  - The firmware published `WS_MAGTAG_LOW_POWER_BUTTON_PUBLISHED pin=15`.

## Important configuration observation

- App-only flashing does not rewrite the MagTag filesystem config.
- The physical board still reported:
  - `awake_window_seconds=1200`
- That means the board's persisted `secrets.json` still has the old 20-minute
  awake window, even though source defaults and examples now use 10 minutes.
- This does not affect explicit serial tests like `WSLP SLEEP 10` or
  `WSLP SLEEP 600`, but it does affect the normal automatic awake window on the
  physical board until the filesystem/config is updated.

## Button B physical wake test attempt

Goal:

- Put the MagTag to sleep for 10 minutes with `WSLP SLEEP 600`.
- Press physical Button B.
- Verify that wake happened before the 10-minute timer.
- Verify exact Button B was queued and published after MQTT connected and any
  throttle cleared.

Known MagTag button mapping from the board variant:

- `BUTTON_A = GPIO 15`
- `BUTTON_B = GPIO 14`
- `BUTTON_C = GPIO 12`
- `BUTTON_D = GPIO 11`

What happened:

- `WSLP SLEEP 600` was sent.
- Serial printed:
  - `WS_MAGTAG_LOW_POWER_SLEEP seconds=600`
  - `WS_MAGTAG_LOW_POWER_SLEEP_MARKER_BEGIN`
  - `WS_MAGTAG_LOW_POWER_SLEEP_MARKER_DONE`
- RFC2217 serial disconnected, which initially looked like deep sleep.
- When monitoring was started before the planned Button B press, serial
  connected immediately.
- The board reported:
  - `WS_MAGTAG_LOW_POWER_STATUS enabled=1 awake_seconds=63 awake_window_seconds=1200 sleep_interval_minutes=180 queued_button=0`
- This means the board was already awake about 63 seconds into that boot. It had
  woken far earlier than the 10-minute timer and before the intended physical
  Button B press window.
- The logs showed awake-mode Button B activity:
  - `Executing state-based event on D14`
  - `Encoding pinEvent...Encoded!`
  - `Publishing pinEvent...Published!`
- Since `D14` is Button B, the app did observe Button B while awake, but this
  did not prove the deep-sleep queued wake path.
- The required low-power wake evidence was not captured:
  - No `WS_MAGTAG_LOW_POWER_WAKE reason=button mask=...`
  - No `WS_MAGTAG_LOW_POWER_BUTTON_QUEUED pin=14`
  - No `WS_MAGTAG_LOW_POWER_BUTTON_PUBLISHED pin=14` from the queued wake path

Camera observation:

- The follow-up camera capture at `artifacts/magtag-unexpected-awake.jpg`
  showed the normal awake display and no visible sleep marker.
- The user also did not see the `zzz` marker on the physical screen during that
  attempt.

## Likely explanations for the failed Button B test

The evidence says the test did not remain in deep sleep until the planned Button
B press. Possible causes:

- A wake pin may already have been held low or bounced low soon after sleep
  entry.
- The workbench serial/recovery path may have reset or otherwise disturbed the
  board.
- The board may have entered deep sleep briefly, then immediately woke due to
  EXT1 before monitoring began.
- The display marker may have been drawn but later overwritten by the normal
  awake display after the early wake.

The logs are not sufficient to distinguish these yet because `WSLP STATUS` did
not include wake cause, EXT1 mask, or live button pin levels in the pushed
firmware.

## Workbench state and issues

- Workbench `SLOT1` was healthy before the latest test.
- Later, a flash attempt for diagnostic firmware failed before writing:
  - esptool connected poorly and reported:
    - `Could not configure port: (5, 'Input/output error')`
- Recovery then degraded:
  - serial was not reachable on port `4001`
  - SSH accepted TCP intermittently but timed out during banner/session startup
  - API accepted TCP but recovery calls timed out
- This matches the previously observed workbench wedge behavior.
- The workbench likely needs a manual reboot before continuing.

## Diagnostic attempted but reverted

A temporary diagnostic was added locally but not kept:

- Print button levels before sleep.
- Print wake cause and EXT1 mask in `WSLP STATUS`.

The diagnostic build succeeded, but the flash failed before writing anything.
The edit was reverted so the repo returned to the pushed state.

This diagnostic is still a good next step if we want a crisp answer.

## Recommended next session plan

1. Manually reboot the workbench.
2. Confirm:
   - `tools/magtag-workbench-status`
   - host, SSH, API, and serial are reachable.
3. Re-add a small diagnostic before retesting:
   - `WSLP STATUS` should print:
     - wake cause integer or decoded wake reason
     - `esp_sleep_get_ext1_wakeup_status()`
     - current A/B/C/D digital levels
   - sleep entry should print pre-sleep A/B/C/D digital levels.
4. Build and flash the diagnostic firmware.
5. Confirm Button B is high before sleep:
   - Button B should read `1` when not pressed.
6. Send `WSLP SLEEP 600`.
7. Immediately capture camera evidence that the sleep marker is visible.
8. Start the serial wake monitor.
9. Press physical Button B.
10. Verify the post-wake log contains:
    - `WS_MAGTAG_LOW_POWER_WAKE reason=button mask=...`
    - mask includes GPIO 14
    - `WS_MAGTAG_LOW_POWER_BUTTON_QUEUED pin=14`
    - after MQTT connects and throttle clears:
      - `WS_MAGTAG_LOW_POWER_BUTTON_PUBLISHED pin=14`
11. If it wakes early again before pressing Button B, use the diagnostic output
    to identify whether the wake was EXT1 and which GPIO was in the mask.

## Commands used successfully

Build:

```sh
tools/magtag-build
```

Workbench status:

```sh
tools/magtag-workbench-status
```

Flash app-only:

```sh
ESPWB_SSH_KEY=/home/ff/.ssh/id_rsa \
ESPWB_KNOWN_HOSTS=/home/ff/.ssh/known_hosts \
  tools/magtag-flash-workbench --app-only --yes
```

Camera capture:

```sh
tools/workbench-camera-capture artifacts/magtag-deep-sleep-latest.jpg
```

## Bottom line

- Deep sleep itself and sleep-marker rendering were verified.
- The physical Button B queued-wake MQTT path is not yet verified.
- The last Button B attempt woke too early or was already awake before the
  intended press, so it cannot be counted as a successful queued wake test.
- Next attempt should include wake-cause, EXT1 mask, and button-level diagnostics
  before and after sleep.

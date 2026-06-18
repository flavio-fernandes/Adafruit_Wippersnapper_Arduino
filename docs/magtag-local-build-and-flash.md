# MagTag Local Build And Workbench Flash

This workflow builds the MagTag firmware with PlatformIO and flashes `.bin`
images through the ESP workbench pattern from
`flavio-fernandes/esp-codex-platform`. It avoids relying on the `MAGTAGBOOT`
UF2 drive, which can be inconvenient or unreliable to mount during repeated
local development.

## Target

- PlatformIO env: `adafruit_magtag29_esp32s2`
- Board: `adafruit_magtag29_esp32s2`
- CI board name: `magtag`
- Partition table: `tinyuf2-partitions-4MB-noota.csv`

The MagTag env is defined in `platformio.ini` with:

- `-DARDUINO_MAGTAG29_ESP32S2`
- `-DBOARD_HAS_PSRAM`
- `extra_scripts = pre:rename_usb_config.py, pre:generate_default_secrets.py`

## Setup

Install the PlatformIO extension in VS Code and open this repository. The
extension provides the PlatformIO CLI in its terminal environment. If you use a
separate shell, make sure `pio` is on `PATH`, or use the repo-local `.venv/bin/pio`
when present.

Local workbench settings may be placed in `config/workbench.env`. This file is
ignored by git. Keep hostnames, usernames, tokens, and generated firmware out of
commits.

For a one-off setup, point `ESPWB_CONFIG_FILE` at another env file. If this
variable is set, the helper scripts require that file to exist.

`ESPWB_SLOT` defaults to `SLOT1`.

This repo vendors the small, project-local helper layer used by
`esp-codex-platform`:

- `tools/lib/workbench-env` loads ignored local workbench config while allowing
  explicit environment variables to win.
- `tools/espwb-esptool` SSHs to the workbench and calls the reset-aware
  `/usr/local/bin/espwb-local-esptool` helper.
- `tools/espwb-monitor` opens DUT serial logs over RFC2217 and runs a recovery
  check on exit.
- `tools/workbench-camera-capture` and `tools/workbench-camera-sequence` capture
  visual evidence from the local V4L2 camera.

## Build

Run PlatformIO directly:

```sh
pio run -e adafruit_magtag29_esp32s2
```

Or use the helper:

```sh
tools/magtag-build
```

The helper defaults `PLATFORMIO_ENV` to `adafruit_magtag29_esp32s2`, uses
`pio` from `PATH` or `.venv/bin/pio`, and keeps PlatformIO's core cache under
`.pio/platformio-core` unless overridden.

## Local Default Secrets

For a custom local firmware build, create one of these private files:

```text
secrets.json
examples/secrets-examples/secrets.json
```

The MagTag build will embed that JSON as the default `/secrets.json` written
when WipperSnapper creates a fresh board filesystem. If neither private file is
present, the build uses the tracked template:

```text
examples/secrets-examples/secrets-wifi.json
```

You can also point at a different local file for one build:

```sh
WIPPERSNAPPER_SECRETS_JSON=/path/to/secrets.json tools/magtag-build
```

The generated header is written under `.pio/build/.../generated/` and may
contain private credentials. It is ignored with the rest of `.pio/`. The source
`secrets.json` files above are ignored by git and must stay private.

## Inspect Artifacts

```sh
tools/magtag-artifacts
```

After a successful build, the key files are under:

```text
.pio/build/adafruit_magtag29_esp32s2/
```

Expected files for flashing are:

- `firmware.bin`
- `bootloader.bin`
- `partitions.bin`
- framework `boot_app0.bin`

The framework `boot_app0.bin` comes from:

```text
.pio/platformio-core/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin
```

`firmware.elf` and `firmware.map` are useful for debugging and size inspection.
`firmware.uf2` is not required for this workflow and may not be produced.

## Flash

The default flashing path is the reset-aware workbench wrapper:

```sh
tools/espwb-esptool
```

Install or copy it from `esp-codex-platform` into `tools/espwb-esptool`. These
helpers do not call `pio upload`, and they do not use RFC2217 directly for
flashing.

Known ESP32-S2 native-USB caveat: repeated MagTag flash/recovery cycles through
RFC2217/esptool on the Raspberry Pi workbench are not reliable enough for normal
iteration. During issue #1 debugging, compressed stub writes stopped mid-transfer
or failed MD5 verification, and slower no-stub writes still eventually stopped
responding. Prefer TinyUF2 mass-storage flashing for MagTag workbench use. The
tracked follow-up is
<https://github.com/flavio-fernandes/Adafruit_Wippersnapper_Arduino/issues/1>.

When the MagTag is exposing `MAGTAGBOOT`, use the TinyUF2 workbench transport:

```sh
tools/magtag-flash-workbench --app-only --yes --tinyuf2-workbench
tools/magtag-flash-workbench --full --yes --tinyuf2-workbench
```

The helper generates a UF2, copies it to the workbench, writes it to the
`MAGTAGBOOT` volume, and verifies the requested UF2 blocks against
`CURRENT.UF2`. It stops `rfc2217-portal` only while the volume is mounted and
uses remote cleanup so the portal is restarted even if verification fails.

When TinyUF2 is not available, connect the MagTag directly to the workstation
USB, reset it into bootloader mode, and add `--direct-usb` to the flash helper.
Direct USB mode flashes the Espressif bootloader serial device with
`--before no-reset` and lets `--after hard-reset` return it to application USB.

### App-Only Flash

```sh
tools/magtag-flash-workbench --app-only
```

Direct workstation USB fallback:

```sh
tools/magtag-flash-workbench --app-only --direct-usb
```

TinyUF2 workbench transport:

```sh
tools/magtag-flash-workbench --app-only --yes --tinyuf2-workbench
```

This flashes:

```text
0x10000  .pio/build/adafruit_magtag29_esp32s2/firmware.bin
```

Use app-only flashing for normal repeated development when the board already has
a compatible bootloader and partition table. This mode assumes the existing
bootloader and partition table are correct for this MagTag build.

### Full Flash

```sh
tools/magtag-flash-workbench --full --yes
```

Direct workstation USB fallback:

```sh
tools/magtag-flash-workbench --full --yes --direct-usb
```

TinyUF2 workbench transport:

```sh
tools/magtag-flash-workbench --full --yes --tinyuf2-workbench
```

This flashes:

```text
0x1000   .pio/build/adafruit_magtag29_esp32s2/bootloader.bin
0x8000   .pio/build/adafruit_magtag29_esp32s2/partitions.bin
0xe000   .pio/platformio-core/packages/framework-arduinoespressif32/tools/partitions/boot_app0.bin
0x10000  .pio/build/adafruit_magtag29_esp32s2/firmware.bin
```

Full flash rewrites bootloader, partition table, boot app image, and firmware.
The helper requires `--yes` or an interactive `yes` confirmation before it runs.
It refuses to flash if any expected file is missing.

Direct USB mode defaults to the ESP32-S2 bootloader by-id path:

```sh
/dev/serial/by-id/usb-Espressif_ESP32-S2_0-if00
```

Override it for another local serial path:

```sh
MAGTAG_DIRECT_USB_PORT=/dev/ttyACM0 tools/magtag-flash-workbench --app-only --direct-usb
```

The app offset is verified from `tinyuf2-partitions-4MB-noota.csv`, where
`ota_0` starts at `0x10000`. The bootloader, partition table, and `boot_app0`
offsets match the Arduino-ESP32 PlatformIO framework image list for ESP32-S2
Arduino builds.

### Workbench Lessons For esp-codex-platform

The MagTag issue #1 debug session produced a few lessons that should be portable
to `flavio-fernandes/esp-codex-platform`:

- Remote operations that stop `rfc2217-portal` need a remote `EXIT` cleanup trap
  that unmounts volumes, removes temporary files, and restarts the portal.
- Fixed-slot recovery cannot infer a USB sysfs device from slot keys like
  `_fixed_SLOT1`; helpers should capture USB topology from the resolved devnode
  before stopping the portal.
- Native USB boards need transport-specific strategies. For MagTag ESP32-S2,
  TinyUF2 mass storage with `CURRENT.UF2` verification was more reliable than
  RFC2217/esptool writes over the Pi USB path.
- UF2 verification should compare requested bytes for final partial blocks,
  because `CURRENT.UF2` may expose full flash-aligned blocks.
- `ESPWB_KNOWN_HOSTS` support helps during recovery because `ssh-keyscan` can be
  unreliable while the workbench is degraded.
- RFC2217 proxy startup should retry when the proxy process starts but the TCP
  port is not listening yet; native USB boards can enumerate before the serial
  endpoint is ready for a stable proxy.

## Monitor

```sh
tools/magtag-monitor-workbench
```

The monitor helper delegates to the vendored `tools/espwb-monitor`, which reads
`ESP_PORT` from `config/workbench.env` and defaults to
`rfc2217://${WORKBENCH_IP}:4001?ign_set_control`.

If the monitor endpoint is refused after a flash or after closing a monitor,
`tools/magtag-monitor-workbench` runs one recovery attempt and retries the
monitor. You can also run the recovery helper directly:

```sh
tools/magtag-workbench-status
tools/magtag-recover-workbench
```

The recovery helper first tries the reset-aware
`tools/espwb-esptool flash-id` path documented by `esp-codex-platform`. If that
fails, it asks the workbench API to recover `SLOT1` and waits for the RFC2217
portal to become reachable. Workbench API calls are bounded by
`ESPWB_API_CONNECT_TIMEOUT`, `ESPWB_API_MAX_TIME`, and
`ESPWB_RECOVER_API_MAX_TIME` so a stale workbench service reports a failure
instead of hanging indefinitely. SSH-based helpers also honor
`ESPWB_SSH_CONNECT_TIMEOUT`, `ESPWB_SSH_SERVER_ALIVE_INTERVAL`, and
`ESPWB_SSH_SERVER_ALIVE_COUNT_MAX` for the same reason.

Use `tools/magtag-workbench-status` first when the failure mode is unclear. It
checks host reachability, SSH, the workbench API, the serial portal, and the
SLOT1 status reported by the API when available.

The status helper also prints the USB identity for the selected slot. Treat
`serial: reachable` as a transport check only. If `slot.usb` shows
`MagTag 2.9 Grayscale (239a:00e5)`, the MagTag is in TinyUF2 bootloader mode;
the workbench socket can accept connections, but firmware app commands such as
`WSLP STATUS` will not return anything until the WipperSnapper app is running.

If you need to bypass `tools/espwb-monitor`, set a local TCP monitor endpoint in
`config/workbench.env`:

```sh
ESPWB_MONITOR_HOST=your-workbench-host
ESPWB_MONITOR_PORT=4001
```

## Camera

Use the same local V4L2 camera helpers as `esp-codex-platform`:

```sh
tools/workbench-camera-capture
tools/workbench-camera-sequence 4 3
```

The default camera path comes from `config/workbench.env.example`. Override
`WORKBENCH_CAMERA_DEVICE` in ignored `config/workbench.env` when the host exposes
the camera under a different path, such as `/dev/video0`.

## Recovery Notes

If the board does not boot after an app-only flash, do a full flash so the
bootloader and partition table match the current PlatformIO environment. If full
flash fails, verify the workbench slot, cabling, board power, and that
`tools/espwb-esptool flash-id` works before flashing again.

Do not commit:

- `config/workbench.env`
- `secrets.json`
- `examples/secrets-examples/secrets.json`
- `artifacts/`
- `.pio/`
- generated `.bin`, `.elf`, `.map`, or `.uf2` files

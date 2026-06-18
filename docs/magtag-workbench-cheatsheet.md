# MagTag Workbench Cheatsheet

This is the shell-session version of the MagTag workflow: build, flash,
monitor serial logs, send low-power test commands, capture camera evidence, and
recover the workbench when the serial portal gets wedged.

Run these commands from the Linux host checkout:

```sh
cd Adafruit_Wippersnapper_Arduino.git
```

## Local Setup

Most helpers read ignored local settings from `config/workbench.env`. The
settings used by the commands below are:

```sh
export ESPWB_SLOT=SLOT1
export WORKBENCH_IP=<workbench-host-or-ip>
export ESPWB_MONITOR_PORT=4001
export ESPWB_SSH_KEY=${HOME}/.ssh/id_rsa
```

If those are already in `config/workbench.env`, you do not need to export them
for the repo helpers. Exporting them is still useful when running one-off raw
commands.

For raw serial snippets, this is the RFC2217 URL:

```sh
export ESPWB_MONITOR_PORT=${ESPWB_MONITOR_PORT:-4001}
export MAGTAG_SERIAL_URL="rfc2217://${WORKBENCH_IP:?set WORKBENCH_IP first}:${ESPWB_MONITOR_PORT}?ign_set_control"
printf 'MAGTAG_SERIAL_URL=%s\n' "$MAGTAG_SERIAL_URL"
```

If you change `WORKBENCH_IP`, run the `MAGTAG_SERIAL_URL=...` export again.
Shell variables are expanded when they are assigned, so an old URL can keep an
empty or stale host.

## Normal Build

Build the MagTag PlatformIO environment:

```sh
tools/magtag-build
```

Equivalent direct PlatformIO command:

```sh
pio run -e adafruit_magtag29_esp32s2
```

The helper uses the private default secrets file when present:

```text
examples/secrets-examples/secrets.json
```

That file must stay ignored and uncommitted.

## Check Workbench Health

Before flashing or monitoring, check SLOT1:

```sh
tools/magtag-workbench-status
```

Healthy output should show host/API/SSH/serial reachable and something like:

```text
slot: present=True running=True recovering=False state=idle tcp_port=4001
slot.usb: MagTag 2.9 Grayscale (239a:00e5)
```

`serial: reachable` only means the workbench RFC2217 socket is accepting
connections. It does not prove the WipperSnapper app is running. If
`slot.usb` shows `MagTag 2.9 Grayscale (239a:00e5)`, the board is in TinyUF2
bootloader mode; `WSLP ...` app commands will connect but return no status.

Quick raw port checks:

```sh
nc -z -w 3 "$WORKBENCH_IP" 22
nc -z -w 3 "$WORKBENCH_IP" 8080
nc -z -w 3 "$WORKBENCH_IP" "$ESPWB_MONITOR_PORT"
```

## Flash

Known ESP32-S2 native-USB caveat: repeated MagTag flash/recovery cycles through
the RFC2217 workbench path are not reliable enough for normal iteration on the
Raspberry Pi workbench. Prefer TinyUF2 workbench flashing when the board exposes
`MAGTAGBOOT`. The tracked follow-up is
<https://github.com/flavio-fernandes/Adafruit_Wippersnapper_Arduino/issues/1>.

For normal iteration, flash only the app image:

```sh
ESPWB_SSH_KEY=${HOME}/.ssh/id_rsa \
  tools/magtag-flash-workbench --app-only --yes
```

For a full image rewrite:

```sh
ESPWB_SSH_KEY=${HOME}/.ssh/id_rsa \
  tools/magtag-flash-workbench --full --yes
```

Reliable MagTag workbench path through TinyUF2:

```sh
ESPWB_SSH_KEY=${HOME}/.ssh/id_rsa \
  tools/magtag-flash-workbench --app-only --yes --tinyuf2-workbench

ESPWB_SSH_KEY=${HOME}/.ssh/id_rsa \
  tools/magtag-flash-workbench --full --yes --tinyuf2-workbench
```

Direct workstation USB fallback:

```sh
tools/magtag-flash-workbench --app-only --yes --direct-usb
tools/magtag-flash-workbench --full --yes --direct-usb
```

For direct USB, reset the MagTag into ESP32-S2 bootloader mode first. Set
`MAGTAG_DIRECT_USB_PORT` if your local bootloader serial path differs.

Low-level flash identity check:

```sh
ESPWB_SSH_KEY=${HOME}/.ssh/id_rsa tools/espwb-esptool flash-id
```

## Monitor Serial Logs

Preferred monitor:

```sh
tools/magtag-monitor-workbench
```

This uses the workbench RFC2217 serial portal and will try one recovery if the
monitor fails.

`tools/espwb-monitor` exits after `ESPWB_MONITOR_IDLE_TIMEOUT` seconds without
serial bytes, default `300`, so a quiet or wedged RFC2217 session still reaches
the post-monitor recovery path. Set `ESPWB_MONITOR_IDLE_TIMEOUT=0` only when an
unbounded live monitor is intentional. Set `ESPWB_MONITOR_MAX_TIME` to a
non-zero number of seconds when a test should have a hard total runtime cap.

Raw TCP monitor fallback:

```sh
nc "$WORKBENCH_IP" "$ESPWB_MONITOR_PORT"
```

## Send Serial Commands

Use the repo PlatformIO Python environment because it already has `pyserial`:

```sh
export PYTHON=.pio/platformio-core/penv/bin/python
```

Read low-power status:

```sh
$PYTHON -c 'import os, serial, time
workbench_ip=os.environ.get("WORKBENCH_IP", "")
port=os.environ.get("MAGTAG_SERIAL_URL") or "rfc2217://%s:%s?ign_set_control" % (workbench_ip, os.environ.get("ESPWB_MONITOR_PORT", "4001"))
if "://:" in port:
    raise SystemExit("MAGTAG_SERIAL_URL has no host; export WORKBENCH_IP first, then rebuild MAGTAG_SERIAL_URL")
ser=serial.serial_for_url(port, baudrate=115200, timeout=0.5)
time.sleep(0.8)
ser.write(b"WSLP STATUS\n")
deadline=time.time()+10
data=b""
while time.time()<deadline:
    chunk=ser.read(4096)
    if chunk:
        data += chunk
ser.close()
text=data.decode("utf-8", "replace")
print(text)
if "WS_MAGTAG_LOW_POWER_STATUS" not in text:
    raise SystemExit("No WSLP STATUS response. Check tools/magtag-workbench-status; if slot.usb is 239a:00e5, the board is in TinyUF2 bootloader mode, not the app.")'
```

Force a short deep sleep:

```sh
$PYTHON -c 'import os, serial, time
workbench_ip=os.environ.get("WORKBENCH_IP", "")
port=os.environ.get("MAGTAG_SERIAL_URL") or "rfc2217://%s:%s?ign_set_control" % (workbench_ip, os.environ.get("ESPWB_MONITOR_PORT", "4001"))
if "://:" in port:
    raise SystemExit("MAGTAG_SERIAL_URL has no host; export WORKBENCH_IP first, then rebuild MAGTAG_SERIAL_URL")
ser=serial.serial_for_url(port, baudrate=115200, timeout=0.5)
time.sleep(0.5)
ser.write(b"WSLP SLEEP 10\n")
deadline=time.time()+4
data=b""
while time.time()<deadline:
    try:
        chunk=ser.read(4096)
    except Exception as exc:
        data += ("\nSERIAL_EXCEPTION %s\n" % exc).encode()
        break
    if chunk:
        data += chunk
ser.close()
text=data.decode("utf-8", "replace")
print(text)
if "WS_MAGTAG_LOW_POWER_SLEEP" not in text and "SERIAL_EXCEPTION" not in text:
    raise SystemExit("No WSLP SLEEP response. Check tools/magtag-workbench-status; if slot.usb is 239a:00e5, the board is in TinyUF2 bootloader mode, not the app.")'
```

Expected forced-sleep output includes:

```text
WS_MAGTAG_LOW_POWER_SLEEP seconds=10
SERIAL_EXCEPTION connection failed
```

That serial exception is expected because the ESP32-S2 entered deep sleep and
the USB serial link disappeared.

Other useful low-power commands:

```text
WSLP STATUS
WSLP BUTTON
WSLP AWAKE 15
WSLP SLEEP 10
```

`WSLP BUTTON` emulates a button wake while the firmware is awake. It cannot wake
the board from true deep sleep; timer wake or a physical MagTag button does
that. It now queues a `BUTTON_A` active-low pin event and publishes it after
MQTT is connected and any Adafruit IO throttle window has cleared.

## Camera Capture

Capture one frame:

```sh
tools/workbench-camera-capture
```

Capture to a specific file:

```sh
tools/workbench-camera-capture artifacts/magtag-check.jpg
```

Capture a sequence, for example five frames three seconds apart:

```sh
tools/workbench-camera-sequence 5 3
```

The sequence command prints paths such as:

```text
artifacts/workbench-camera-YYYYMMDDTHHMMSSZ/frame-01.jpg
```

Use a sequence around `WSLP SLEEP 10` to confirm the eInk sleep marker is
visible while the MagTag is in deep sleep.

## Low-Power Verification Workflow

Build and flash:

```sh
tools/magtag-build
ESPWB_SSH_KEY=${HOME}/.ssh/id_rsa \
  tools/magtag-flash-workbench --app-only --yes
```

Confirm serial is back:

```sh
tools/magtag-workbench-status
```

Confirm firmware settings:

```sh
export WORKBENCH_IP=<workbench-host-or-ip>
export ESPWB_MONITOR_PORT=${ESPWB_MONITOR_PORT:-4001}
export MAGTAG_SERIAL_URL="rfc2217://${WORKBENCH_IP:?set WORKBENCH_IP first}:${ESPWB_MONITOR_PORT}?ign_set_control"
export PYTHON=.pio/platformio-core/penv/bin/python

$PYTHON -c 'import os, serial, time
workbench_ip=os.environ.get("WORKBENCH_IP", "")
port=os.environ.get("MAGTAG_SERIAL_URL") or "rfc2217://%s:%s?ign_set_control" % (workbench_ip, os.environ.get("ESPWB_MONITOR_PORT", "4001"))
if "://:" in port:
    raise SystemExit("MAGTAG_SERIAL_URL has no host; export WORKBENCH_IP first, then rebuild MAGTAG_SERIAL_URL")
ser=serial.serial_for_url(port, baudrate=115200, timeout=0.5)
time.sleep(0.8)
ser.write(b"WSLP STATUS\n")
deadline=time.time()+10
data=b""
while time.time()<deadline:
    chunk=ser.read(4096)
    if chunk:
        data += chunk
ser.close()
text=data.decode("utf-8", "replace")
print(text)
if "WS_MAGTAG_LOW_POWER_STATUS" not in text:
    raise SystemExit("No WSLP STATUS response. Check tools/magtag-workbench-status; if slot.usb is 239a:00e5, the board is in TinyUF2 bootloader mode, not the app.")'
```

Force sleep and capture the screen:

```sh
$PYTHON -c 'import os, serial, time
workbench_ip=os.environ.get("WORKBENCH_IP", "")
port=os.environ.get("MAGTAG_SERIAL_URL") or "rfc2217://%s:%s?ign_set_control" % (workbench_ip, os.environ.get("ESPWB_MONITOR_PORT", "4001"))
if "://:" in port:
    raise SystemExit("MAGTAG_SERIAL_URL has no host; export WORKBENCH_IP first, then rebuild MAGTAG_SERIAL_URL")
ser=serial.serial_for_url(port, baudrate=115200, timeout=0.5)
time.sleep(0.5)
ser.write(b"WSLP SLEEP 10\n")
deadline=time.time()+4
data=b""
while time.time()<deadline:
    try:
        chunk=ser.read(4096)
    except Exception as exc:
        data += ("\nSERIAL_EXCEPTION %s\n" % exc).encode()
        break
    if chunk:
        data += chunk
ser.close()
text=data.decode("utf-8", "replace")
print(text)
if "WS_MAGTAG_LOW_POWER_SLEEP" not in text and "SERIAL_EXCEPTION" not in text:
    raise SystemExit("No WSLP SLEEP response. Check tools/magtag-workbench-status; if slot.usb is 239a:00e5, the board is in TinyUF2 bootloader mode, not the app.")'

tools/workbench-camera-sequence 5 3
```

After the timer wake, confirm serial recovered:

```sh
tools/magtag-workbench-status
```

## Unwedge And Recover

Start with status:

```sh
tools/magtag-workbench-status
```

If serial is down but SSH/API are up:

```sh
ESPWB_SSH_KEY=${HOME}/.ssh/id_rsa tools/magtag-recover-workbench
```

If recovery is slow or timing out, give the helpers longer timeouts:

```sh
ESPWB_SSH_KEY=${HOME}/.ssh/id_rsa \
ESPWB_SSH_CONNECT_TIMEOUT=15 \
ESPWB_API_CONNECT_TIMEOUT=10 \
ESPWB_RECOVER_API_MAX_TIME=40 \
  tools/magtag-recover-workbench
```

If the SSH host-key pre-scan times out during recovery but the workbench key is
already trusted locally, point the helper at that file while keeping strict
host-key checking:

```sh
ESPWB_SSH_KEY=${HOME}/.ssh/id_rsa \
ESPWB_KNOWN_HOSTS=${HOME}/.ssh/known_hosts \
  tools/magtag-recover-workbench
```

If the monitor command fails, use the wrapper because it attempts recovery and
then retries:

```sh
tools/magtag-monitor-workbench
```

If you need to clear stale monitor/RFC2217 processes from the Linux host:

```sh
pgrep -af 'tools/espwb-monitor|rfc2217'
pkill -f "tools/espwb-monitor|rfc2217://${WORKBENCH_IP}:4001"
```

If the board is reachable enough for esptool but the monitor portal is not,
force a reset-aware probe:

```sh
ESPWB_SSH_KEY=${HOME}/.ssh/id_rsa tools/espwb-esptool flash-id
```

Then check status again:

```sh
tools/magtag-workbench-status
```

If `tools/magtag-workbench-status` says SSH and API are both unreachable, the
workbench itself is down or wedged. At that point, power-cycle the workbench and
then retry:

```sh
tools/magtag-workbench-status
ESPWB_SSH_KEY=${HOME}/.ssh/id_rsa tools/magtag-recover-workbench
```

## Useful Files

Build outputs:

```text
.pio/build/adafruit_magtag29_esp32s2/firmware.bin
.pio/build/adafruit_magtag29_esp32s2/bootloader.bin
.pio/build/adafruit_magtag29_esp32s2/partitions.bin
```

Ignored local configuration:

```text
config/workbench.env
examples/secrets-examples/secrets.json
```

Camera evidence:

```text
artifacts/workbench-camera-*/
```

Do not commit ignored private files or generated firmware artifacts.

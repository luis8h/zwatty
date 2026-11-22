# FTMS trainer simulator (ESP32)

An ESP32 that pretends to be a smart trainer (a KICKR Core-style Bluetooth FTMS
indoor bike) so you can test your cycling app without pedalling. You control the
virtual rider and inject faults from a web page, `curl`, or the serial console.
Everything runs offline: the ESP32 creates its own Wi-Fi network, no router,
internet or cloud involved.

```
 your cycling app  <--- BLE (FTMS) --->  ESP32  <--- Wi-Fi AP / serial --->  you
```

## What it simulates

**BLE: Fitness Machine Service (0x1826), Indoor Bike**

| Characteristic | Notes |
|---|---|
| Fitness Machine Feature (2ACC) | cadence, distance, elapsed time, power; targets: resistance, power, bike simulation |
| Indoor Bike Data (2AD2) | speed, cadence, distance, power, elapsed time, default 1 Hz |
| Training Status (2AD3) | idle / manual mode |
| Supported Power Range (2AD8) | 0 to 1800 W |
| Supported Resistance Range (2AD6) | 0.0 to 10.0 |
| Fitness Machine Control Point (2AD9) | see below |
| Fitness Machine Status (2ADA) | notifications after accepted commands |

**Control point opcodes**

| Opcode | Command | Behaviour |
|---|---|---|
| 0x00 | Request Control | grants control |
| 0x01 | Reset | clears targets, back to free ride |
| 0x04 | Set Target Resistance | free-ride with a resistance level |
| 0x05 | Set Target Power | **ERG mode** |
| 0x07 | Start / Resume | training status becomes manual mode |
| 0x08 | Stop / Pause | training status becomes idle |
| 0x11 | Set Indoor Bike Simulation Parameters | **SIM mode** (wind, grade, Crr, Cw) |

Responses follow the spec: `0x80 <opcode> <result>` with 0x01 success, 0x02 not
supported, 0x03 invalid parameter, 0x04 failed, 0x05 control not permitted.
Anything except Request Control is refused with 0x05 until the app has requested
control. Unknown opcodes get 0x02. Wind speed is treated as positive = headwind.

**Mode follows the app**, like a real trainer: Set Target Power switches to ERG,
Set Indoor Bike Simulation Parameters switches to SIM. There is no mode switch in
the web UI.

**Rider model**
- *ERG*: power moves toward the target with a first-order lag (`erg_lag`), capped
  by `max_power`.
- *SIM / free ride / resistance*: power is whatever `rider_power` says. Speed in SIM
  mode comes from a physics model (rolling resistance, grade, drag, wind, mass).
- Cadence, power noise and slow power drift are adjustable.

**Not included:** ANT+, Wahoo's proprietary service, Tacx FE-C over BLE, Cycling
Power / CSC services, simultaneous connections (one app at a time). A real KICKR has
quirks a spec-correct simulator does not reproduce (timing, odd command sequences).
If you capture them with nRF Connect, they can be added here.

## Hardware

Any ESP32 dev board with USB (tested design target: ZY-ESP32 MINI D1, original
ESP32). Nothing needs to be wired up.

## 1. Install the toolchain (Linux)

You need Python 3 and PlatformIO's CLI. No IDE. Internet is only needed once, to
download the toolchain and libraries; builds work offline afterwards.

Using `uv`:
```bash
uv tool install platformio
```

```bash
python3 -m venv ~/.venvs/pio
~/.venvs/pio/bin/pip install platformio
# optional: make `pio` available everywhere
mkdir -p ~/.local/bin && ln -sf ~/.venvs/pio/bin/pio ~/.local/bin/pio
```

(`pipx install platformio` works too.)

Give your user access to the serial port, then **log out and back in**:

```bash
sudo usermod -aG dialout $USER      # Arch/Fedora: the group may be called uucp or dialout
```

If `/dev/ttyUSB0` appears and immediately disappears on Ubuntu/Debian, the
`brltty` package is grabbing the CH340 chip: `sudo apt remove brltty`.

## 2. Configure (optional)

Edit `include/config.h`: BLE name, Wi-Fi SSID and password, power limits. The
defaults work as they are (SSID `FTMS-SIM`, password `simtrainer`).

## 3. Build and flash

Plug in the board, then from this directory:

```bash
pio run -t upload        # build + flash
pio device monitor       # serial console + log output (Ctrl+C to quit)
```

- Find the port with `ls /dev/ttyUSB* /dev/ttyACM*`. PlatformIO usually detects it;
  otherwise add `upload_port = /dev/ttyUSB0` to `platformio.ini`.
- If it hangs at "Connecting…", hold the **BOOT** button on the board until
  uploading starts. If it fails mid-upload, set `upload_speed = 115200`.
- To only build: `pio run`. To erase everything: `pio run -t erase`.

After boot, the monitor shows the BLE name and the Wi-Fi address.

## 4. Use it

1. On your phone or laptop, join the Wi-Fi network **FTMS-SIM** (password
   `simtrainer`). It has no internet; if your phone offers to disconnect, choose to stay connected.
2. Open **http://192.168.4.1**.
3. In your cycling app, scan for a trainer and pick **FTMS-SIM**.

If the app runs on the same phone as the web page, that works too: BLE and Wi-Fi are independent.

### Web UI

- **What your app sees**: live mode, power, cadence, speed, distance, ERG target, grade, wind, Crr and Cw, plus status pills (app connected, data subscribed, control granted, running).
- **Rider and trainer model**: sliders for every setting (table below).
- **Fault injection**: disconnect the app, stop data for 10 s, or send malformed or out-of-range data packets.
- **Activity**: every control point command received, and the response sent.

### HTTP API (for scripted tests)

```bash
BASE=http://192.168.4.1

curl $BASE/state                                   # full state + settings + log as JSON
curl $BASE/meta                                    # setting definitions (ranges, units)

curl "$BASE/set?cadence=95"                        # change settings (several at once is fine)
curl "$BASE/set?rider_power=250&power_noise=5&cadence_noise=3"
curl -d erg_lag=6 $BASE/set                        # POST works too

curl "$BASE/do?cmd=drop"                           # disconnect the app
curl "$BASE/do?cmd=stall&arg=15"                   # no data for 15 s
curl "$BASE/do?cmd=inject&arg=short"               # short | garbage | zero | max | negative
curl "$BASE/do?cmd=reset"                          # reset session state
curl "$BASE/do?cmd=defaults"                       # restore default settings
```

Replies are JSON; errors return HTTP 400 with a message. Join the AP from the test
machine (`nmcli device wifi connect FTMS-SIM password simtrainer`) and these calls
can be scripted from shell or Python.

### Serial console

`pio device monitor`, then type commands (`help` lists them):

```
cadence 90
rider_power 250
fail_next 2          # the next 2 control point commands fail
settings             # list all settings
state                # JSON state
drop
stall 10
inject garbage
```

## Settings

| Key | Meaning | Range | Default |
|---|---|---|---|
| `pedalling` | rider is pedalling (0 = coasting: power and cadence go to 0) | 0/1 | 1 |
| `cadence` | rpm | 0-150 | 85 |
| `cadence_noise` | rpm, std dev | 0-20 | 2 |
| `rider_power` | W, used in every mode except ERG | 0-1500 | 180 |
| `power_noise` | % of power, std dev | 0-30 | 2 |
| `power_drift` | % of power, slow oscillation | 0-30 | 0 |
| `erg_lag` | s, time constant toward the ERG target | 0-30 | 2 |
| `max_power` | W, the rider can't exceed this in ERG | 0-2000 | 1000 |
| `rider_mass` | kg, rider + bike (speed model) | 40-200 | 85 |
| `notify_ms` | Indoor Bike Data interval | 100-5000 | 1000 |
| `ctrl_delay_ms` | delay before control point responses and status updates | 0-10000 | 0 |
| `ctrl_silent` | never answer control point writes | 0/1 | 0 |
| `fail_next` | next N control point commands answered with `fail_code`, nothing applied | 0-20 | 0 |
| `fail_code` | result code used for failures | 2-5 | 4 |

Settings are not stored across reboots; defaults are restored on power-up.

## Test recipes

- **ERG lag**: send an ERG target from your app, set `erg_lag=8`, check the app copes with slow convergence.
- **Rider can't hold target**: `max_power=200`, then ask for 300 W.
- **Command failure handling**: `fail_next=1`, then change the ERG target. The app should see result code 0x04.
- **Slow or missing responses**: `ctrl_delay_ms=3000`, or `ctrl_silent=1`.
- **Data dropout**: *Stop data for 10 s* (the BLE link stays up, notifications pause).
- **Link loss**: *Disconnect app* and check reconnect handling.
- **Bad data**: *Send short / garbage / max / negative* packets.
- **Noisy rider**: `power_noise=10`, `cadence_noise=8`, `power_drift=10`.
- **Coasting**: `pedalling=0`.

## Troubleshooting

| Symptom | Fix |
|---|---|
| App doesn't find the trainer | Check the serial log says "BLE advertising". Make sure the app is scanning for FTMS trainers. Toggle phone Bluetooth. |
| Phone drops the Wi-Fi | Stay connected when it warns about no internet; disable "auto-switch to mobile data". |
| Can't open 192.168.4.1 | Confirm you joined `FTMS-SIM`, and that mobile data/VPN isn't capturing traffic. |
| `Permission denied: /dev/ttyUSB0` | Add yourself to `dialout` and log in again. |
| Upload hangs at "Connecting…" | Hold BOOT while it starts; try another USB cable (some are charge-only). |
| Garbage in monitor | Baud rate must be 115200 (the default in `platformio.ini`). |

## Project layout

```
platformio.ini      board, pinned platform and library versions
include/config.h    BLE name, Wi-Fi credentials, power limits
src/main.cpp        setup/loop
src/ftms.*          BLE: FTMS service, control point, data notifications, fault injection
src/sim.*           rider/trainer model and physics (shared state behind a mutex)
src/control.*       settings table, actions, JSON, serial command parser
src/web.*           Wi-Fi AP and HTTP server
src/web_ui.h        the web page (embedded in the firmware)
src/console.*       serial console
```

To add a setting: add a field to `Settings` in `sim.h` and one line to the table in
`control.cpp`. It then appears in the web UI, the API and the console.

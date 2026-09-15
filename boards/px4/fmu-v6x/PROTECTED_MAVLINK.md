# Protected MAVLink bring-up (v0.2.1)

`px4_fmu-v6x_protected-mavlink` adds userspace MAVLink to the `v0.2`
diagnostic baseline. The original `px4_fmu-v6x_protected` remains the minimal
diagnostic target. Both use the same protected NuttX configuration; the
`nuttx-config/protected-mavlink` symlink selects `protected`.

This first communications patch enables manual UART startup, parameter
transport and version replies. Mission/fence/rally transfers are disabled with
`CONFIG_MAVLINK_MISSION=n`, avoiding a dataman client when no dataman service is
running. The new option defaults to enabled for existing configurations.
Logger, dataman, commander, sensors and physical outputs remain disabled.

The startup script marks MAVLink boot complete so parameter handling and
transmission are allowed when an instance is started later. It starts no UART
instance automatically. Native USB NSH continues to own `/dev/ttyACM0`.

## Build

```sh
make px4_fmu-v6x_protected-mavlink

python3 boards/px4/fmu-v6x/tools/verify_protected.py build/px4_fmu-v6x_protected-mavlink
```

The upload artifact is
`build/px4_fmu-v6x_protected-mavlink/px4_fmu-v6x_protected-mavlink.px4`.
Build the two targets sequentially: they share generated NuttX source state.
The artifact checker checks MAVLink's user builtin and ELF placement, mission
exclusion, and the existing protected memory/allocator/USB/diagnostic checks.

## Manual UART connection

Use a separate serial connection between PC/QGroundControl and TELEM1. The
board's USB COM port remains the NSH console. FMU DEBUG's UART is the boot
console, not TELEM1. Board configuration maps the ports as follows:

| Board connector | NuttX device |
| --- | --- |
| TELEM1 | `/dev/ttyS6` |
| TELEM2 | `/dev/ttyS4` |
| TELEM3 | `/dev/ttyS1` |

From USB NSH, start a single instance on TELEM1 at 57600 baud with hardware
flow control disabled (`-Z`). Select the matching PC serial port and baud rate
in QGroundControl.

```sh
mavlink start -d /dev/ttyS6 -b 57600 -m minimal -r 2000 -Z

mavlink status

free

ps
```

`minimal` selects a small stream set; it is not a separate protocol or an
isolation boundary. Check reception/transmission counters, QGC parameter
listing and USB NSH response. `MAV_SYS_ID` can be read from both interfaces;
parameter persistence across reboot is a later validation item.

To stop the manually started instances:

```sh
mavlink stop-all

mavlink status
```

No commander or sensor topics are synthesized. A heartbeat can be sent with
`MAV_STATE_UNINIT`; this configuration is not a working flight stack. Version
metadata replies no longer depend on a published `vehicle_status`. Mission
and commander-dependent control capability flags are omitted when their
features are excluded. Existing protected userspace UUID/GUID helpers are
zero-valued stubs, so MAVLink's unique ID is not yet the physical board ID
printed by kernel `ver`.

## Validation status

The local GNU GCC 14.2.1 protected-MAVLink build and artifact checks passed.
MAVLink is present only in the user ELF/builtin table and no mission manager
is linked. The updated checker also passed on the existing minimal-target
artifacts. ARM syntax checks of `mavlink_main.cpp`, `mavlink_receiver.cpp` and
`mavlink_mission.cpp` with mission/commander enabled passed; this is not a full
flat/default firmware build. Sizes and the binary SHA-256 are in the generated
`protected-artifact-check.json` beside each build's firmware.

Hardware UART/QGC connection, parameter exchange and MAVLink task lifecycle
have not yet been verified on this new image. Previous `v0.2` hardware results
apply to that diagnostic baseline, not to this larger image.

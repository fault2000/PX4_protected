# FMUv6X protected firmware bring-up

`px4_fmu-v6x_protected` is a minimal STM32H753II firmware target using NuttX
protected mode and the MPU to separate kernel and user memory. It does not
include the full PX4 flight stack or isolate individual user modules from one
another. Successful hardware startup and MPU behavior have not yet been
validated; see the startup BusFault correction below.

The source baseline is:

| Repository | Required commit |
| --- | --- |
| PX4 v1.17 | `d6f12ad1c4f70ad3230afd7d86e971421e02fef4` plus the protected support changes |
| NuttX | `5ef31ffdf1a29202aca2c76c9727d663b49c0c51` (STM32H7 heap and MPU fixes on `fb2fadf6f599c1406f052db013efd00a2518e72c`) |
| NuttX apps | `e37940d8535f603a16b8f6f21c21edaf584218aa` |

The heap fix in `platforms/nuttx/NuttX/nuttx/arch/arm/src/stm32h7/stm32_allocateheap.c`
keeps the protected user heap within AXI SRAM, consistent with the kernel heap
calculation. The NuttX submodule pins this fix and the STM32H7 MPU correction
described below in the `fault2000/nuttX` repository; recursive checkout
retrieves both fixes automatically.

This repository starts with an exact source snapshot of PX4 v1.17.0 from
[upstream commit `d6f12ad1c4`](https://github.com/PX4/PX4-Autopilot/commit/d6f12ad1c4f70ad3230afd7d86e971421e02fef4).
The local `v1.17.0` tag identifies this imported snapshot. Its commit ID differs
from upstream because this repository starts a new history at that release.
Original licenses and copyright notices are retained. The initial two
follow-up commits separate protected-mode bring-up from USB NSH support. Inspect
those changes with `git diff v1.17.0..main`.

For a new checkout:

```sh
git clone --recursive https://github.com/fault2000/PX4_protected.git
cd PX4_protected
```

When updating an existing checkout, synchronize and initialize the submodules
after switching to this branch:

```sh
git submodule sync --recursive
git submodule update --init --recursive
```

From the PX4 repository root, with the ARM toolchain and normal PX4 build
dependencies installed:

```sh
make px4_fmu-v6x_protected
```

The active target does not require `CMAKE_POLICY_VERSION_MINIMUM=3.5` with
CMake 4. `CCACHE_DISABLE=1` can be used when the local compiler cache is not
writable; it is not required by protected mode.

The target is configured to link separate kernel and user ELF files and package
them into `build/px4_fmu-v6x_protected/px4_fmu-v6x_protected.px4`. The original
bootloader reservation is maintained; the user-image start address is fixed at
`0x08100000`. Address
ranges below use an exclusive end:

| Region | Start | End | Capacity |
| --- | --- | --- | --- |
| Bootloader reservation | `0x08000000` | `0x08020000` | 128 KiB |
| Kernel flash | `0x08020000` | `0x08100000` | 896 KiB |
| User flash | `0x08100000` | `0x08200000` | 1024 KiB |
| Kernel static data and idle stack | `0x24000000` | `0x24020000` | 128 KiB |
| User static data window | `0x24020000` | `0x24040000` | Up to 128 KiB |
| Kernel heap | User `_ebss` | `0x24060000` | At least 128 KiB |
| User heap | `0x24060000` | `0x24080000` | 128 KiB |

The user `_ebss` is aligned to 16 KiB to keep kernel heap memory outside user
MPU subregions. `CONFIG_MM_REGIONS=1` leaves other SRAM banks out of the general
allocator; SRAM4 remains available for the board's BDMA reservations. The linker
also checks the kernel static allocation plus a 1 KiB idle-stack allowance.

The configuration retains low-level board support for UART, SPI, I2C, SD, USB,
hardware identification and LEDs. Kernel and user PX4 services initialize
through `px4_entry`, then NSH runs `init/rc.protected` as
`/etc/init.d/rc.additional_init`. The script prints a banner and runs `ver all`
and `work_queue status`; it does not run the normal flight startup script.

Available diagnostics include `dmesg`, `hardfault_log`, `mft`, `mtd`, `param`,
`perf`, `reboot`, `top`, `listener`, `uorb`, `ver`, `work_queue`, and the protected
kernel-command launcher. Flight modules, PX4 sensor/output drivers, Ethernet,
Telnet and PX4 USB protocol autodetection are disabled. The `reboot` command
runs in the kernel because FMUv6X reset lockout controls GPIO. The broad PX4
`tests` command is not included.

## USB NSH console

The protected target enables NuttX's native USB NSH frontend with
`CONFIG_NSH_USBCONSOLE=y` and `CONFIG_NSH_USBCONDEV="/dev/ttyACM0"`.
NSH connects the CDC/ACM device through `boardctl`, executes the startup script,
then waits for a USB terminal. It does not need MAVLink, QGroundControl,
`SYS_USB_AUTO`, or a separate `sercon`/`nshterm` invocation.

After flashing this target, close QGroundControl and open the board's USB
serial port with a terminal application. On Linux, for example:

```sh
screen /dev/ttyACM0 115200
```

Use the actual device name assigned by the host. Press Enter three times to
start the session; this NuttX version waits for three consecutive CR or LF
characters before displaying `nsh>`. The USB frontend retries sessions after
disconnects, but enumeration and reconnect behavior still require hardware
validation.

Useful initial checks at the prompt are:

```sh
ver all
free
ps
work_queue status
dmesg
```

USART3 remains `/dev/console` at 57600 baud for early boot and startup-script
output. The initial interactive NSH session uses USB; this configuration does
not also start an interactive UART shell. USB NSH cannot show failures that
occur before USB initialization, and it does not replay the UART startup log.

## Startup BusFault correction (2026-09-15)

The first hardware UART capture showed a precise BusFault during
`px4_log_initialize()` while uORB registered `log_message`. Using the exact
GCC 13.2.1 kernel ELF uploaded to the board, `PC=0x08031c32` resolves to
`px4::atomic<unsigned long>::fetch_or()` and the instruction `ldrex r3, [r6]`.
`CFSR=0x8200` indicates PRECISERR and BFARVALID; `R6=BFAR=0x24062224` is inside
the configured user heap. These addresses identify that build only.

The previous NuttX pin (`3855de8c95`) mapped protected user SRAM as shareable.
The fix in `platforms/nuttx/NuttX/nuttx/arch/arm/src/stm32h7/stm32_mpuinit.c` maps
both user static RAM and user heaps as Normal, cacheable, non-shareable memory
so that LDREX/STREX use the CPU-local exclusive monitor. This backports
[Apache NuttX commit `519c9a4b8b`](https://github.com/apache/nuttx/commit/519c9a4b8ba06d382a2ce3778a872480c142feff)
to the older file structure. Memory ranges and access permissions are retained;
the common ARM MPU helper and flat build are unchanged. This makes user SRAM
effectively cacheable on Cortex-M7, so DMA buffers still need cache maintenance.

The corrected target built with GCC 14.2.1 on 2026-09-15 and passed the static
artifact checks below. The corrected firmware has not yet been tested on the
board. Preserve the original ELF and UART log, then capture a fresh power-on log
at 57600 baud and check that startup reaches USB NSH. After connecting to USB
and pressing Enter three times, run `ver all`, `free`, `ps`, `work_queue status`
and `dmesg`. Successful startup is the next acceptance gate; it does not complete
the remaining protected-mode validation.

Firmware generation alone does not establish working board startup, heap
isolation, cross-boundary callbacks, or real-time performance. These require
subsequent hardware validation before expanding this target into a flight
configuration.

Firmware generation was verified with GCC 14.2.1 and CMake 4.2.3 on 2026-09-14.
The `make` command above completed successfully with `CCACHE_DISABLE=1` in
the local environment. Static artifact
checks passed for ARM ELF load ranges, RAM bounds, reset vectors, userspace
header, flash padding, builtin tables, kernel placement of `reboot`, and the
`.px4` decompressed payload matching the combined binary. The user ELF includes
the native USB NSH frontend, and CDC/ACM initialization is linked in the kernel.

| Artifact measurement | Bytes |
| --- | ---: |
| Kernel flash image | 272,096 |
| User flash image | 96,704 |
| Combined binary, including flash gap | 1,014,208 |
| PX4 package | 364,499 |

These measurements describe the pre-publication working-tree build. Git
version metadata can change image sizes after committing the sources.

To repeat the artifact checks, use the versioned verification script. It
requires Python 3 and `arm-none-eabi-nm` from the ARM toolchain, and writes
`protected-artifact-check.json` into the selected build directory:

```sh
python3 boards/px4/fmu-v6x/tools/verify_protected.py build/px4_fmu-v6x_protected
```

USB enumeration, interactive commands and reconnect behavior have not yet
been tested on a board.

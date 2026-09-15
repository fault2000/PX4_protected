# FMUv6X protected firmware bring-up

`px4_fmu-v6x_protected` is a minimal STM32H753II firmware target using NuttX
protected mode and the MPU to separate kernel and user memory. It does not
include the full PX4 flight stack or isolate individual user modules from one
another. Initial hardware startup and USB NSH access/reconnection have been confirmed;
the basic protected HRT diagnostic has also passed twice on hardware.
Sustained runtime stability and MPU isolation validation remain in progress.

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

Research milestones use separate project tags: `v0.1` records the initial
hardware bring-up and USB NSH reconnection, and `v0.2` will mark completion and
validation of the next work package. The existing `v0.1.1` HRT tag is retained
as a historical reference. From the next patch onward, versions such as
`v0.1.2` appear in commit subjects and the roadmap change record, without a new
Git tag. A push publishes those commits; it has no separate version-message
field. See
[PROTECTED_ROADMAP.md](PROTECTED_ROADMAP.md) for the scope and versioning rules.
The PX4 numeric firmware version remains based on `v1.17.0`; version generation
excludes project tags matching `v0.*`. Firmware package identity may still
include the project tag and always records the source commit.

The `v0.1` hardware evidence comes from commit `03ca855b52`, documented below.
The milestone also includes the validation record, roadmap, and build-version
selection needed to support project tags; it introduces no HRT or uORB runtime
changes beyond that tested commit. A tag fixes the source and submodule commits,
but does not automatically archive generated firmware or the exact ELF used on
the board.

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

Available diagnostics include `dmesg`, `hardfault_log`, `hrt_smoke`, `mft`, `mtd`, `param`,
`perf`, `reboot`, `top`, `listener`, `uorb`, `ver`, `work_queue`, `work_queue_smoke`, and the protected
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
disconnects. Initial enumeration and shell entry have been confirmed on
Windows. The user also confirmed successful USB reconnection and NSH use;
the report did not record a repetition count.

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

## Startup BusFault corrections (2026-09-15)

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

With the MPU correction installed, the next hardware log reached the first
uORB buffer allocation. The exact GCC 13.2.1 ELF identified `PC=0x0802f00a`
as `ldr.w r4, [r0, #0x144]` in `mm_free_delaylist()`, called by
`mm_malloc()` / `mm_memalign()` from `DeviceNode::write()`. With `R0=0`, the
heap pointer was NULL and `BFAR=0x144`. This is a separate fault after the
earlier atomic registration operation.

The kernel link map exposed a userspace allocator archive pulled in by
`uORB_kernel`'s `nuttx_mm` dependency. Its `memalign()` used a second,
uninitialized `g_mmheap` in kernel BSS. The dependency now uses `nuttx_kmm`,
whose protected wrappers obtain the initialized user heap through
`USERSPACE->us_heap`. This preserves the allocation's user-heap ownership.

The artifact checker now rejects userspace `libmm.a` in the kernel map and a
duplicate kernel `g_mmheap`, and requires the protected `memalign()` wrapper
and the user-space heap pointer. It rejected the pre-fix image and passed on
the corrected image; disassembly also confirmed the corrected heap lookup.

The target with both corrections built with GCC 14.2.1 on 2026-09-15 and passed
the static artifact checks below.

## First hardware startup and USB shell confirmation (2026-09-15)

The user-built GCC 13.2.1 image reported PX4 commit
`03ca855b52ed8a21fde2ed82e565802c55d3bc10` and NuttX commit
`5ef31ffdf1a29202aca2c76c9727d663b49c0c51`. On the board identifying itself as
FMUM `0x003` / BASE `0x005`, the UART log reached `FMUv6X protected bring-up`,
`ver all`, and `work_queue status` without either previous HardFault.
Windows then enumerated the board USB as COM3, and pressing Enter three times
displayed `nsh>`. The debug adapter remained on COM4; COM numbers are
host-specific. This confirms initial startup and USB shell entry for that
image. The initial command results below extend this evidence; repeated
startup and sustained runtime stability remain unverified.

On 2026-09-15, the user confirmed that USB reconnection had also been tested
successfully. This corrects the earlier reconnect-pending status in the
baseline record and roadmap. The report does not specify a repetition count,
so it does not establish completion of the proposed ten-reconnect check or
the separate cold-start and sustained-runtime checks.

`Work Queue: 0 threads` reports an active userspace PX4 work-queue manager
with no worker queues created yet. The minimal configuration has not started
the flight modules that would request those queues. This count does not
include the manager task itself or kernel work queues. The fault-log lines
`state:1` and `Fault Log is Armed` describe an empty/rearmed crash slot, not
a new HardFault.

The user subsequently ran `help`, `free`, `ps`, `uorb status`, and
`listener log_message -n 1` successfully at the USB prompt. The heap snapshot
reported the following byte counts:

| Heap | Total | Used | Free | Largest free block |
| --- | ---: | ---: | ---: | ---: |
| Kernel | 245,408 | 36,560 | 208,848 | 207,920 |
| User | 130,720 | 20,096 | 110,624 | 110,112 |

`ps` showed separate kernel and user `wq:manager` tasks, a kernel
`wq:lp_default`, and the user `usr_hrt` dispatcher waiting on a semaphore.
`px4_entry` used 1,548 of 3,144 stack bytes (49.2%). These are live-state and
stack-usage observations, not proof of leak-free operation or callback timing.

`uorb status` reported `log_message` instance 0, queue length 4 and message
size 136. The user-space listener read the kernel's startup message
`initialized uORB logging`, severity 6 (info), timestamp 4,051 microseconds.
Its displayed age of approximately 755 seconds is consistent with reading a
message published once at startup. This confirms initial kernel publication
and a user subscription/read/unsubscribe, as well as a plausible time read
through the user HRT interface. The single-message listener path does not
exercise `poll()` notifications or periodic publication.

The `free` command's `Prog` row counts programmed/erased flash chunks rather
than runtime heap space. The pinned NuttX procfs implementation also counts
its terminal error result as one extra 32-byte page, explaining the displayed
2,097,184-byte total. Treat this as a reporting issue and use Kmem/Umem for
RAM observations; it does not indicate heap corruption.

Repeated cold starts, sustained heap trends, user work execution, uORB
notifications, and MPU isolation enforcement remain to be verified. Basic
HRT callback/cancellation results are recorded below. The static artifact checker does not infer hardware
validation from source revision; its generated hardware flags remain false
for locally built images.

Firmware generation alone does not establish working board startup, heap
isolation, cross-boundary callbacks, or real-time performance. These require
subsequent hardware validation before expanding this target into a flight
configuration.

## Build validation (v0.1.1)

The HRT patch built with GCC 14.2.1 and CMake 4.2.3 on 2026-09-15.
The `make` command above completed successfully with `CCACHE_DISABLE=1` in
the local environment. Static artifact
checks passed for ARM ELF load ranges, RAM bounds, reset vectors, userspace
header, flash padding, builtin tables, kernel placement of `reboot`, user
placement of `hrt_smoke`, and the
`.px4` decompressed payload matching the combined binary. The user ELF includes
the native USB NSH frontend, and CDC/ACM initialization is linked in the kernel.

| Artifact measurement | Bytes |
| --- | ---: |
| Kernel flash image | 272,352 |
| User flash image | 99,680 |
| Combined binary, including flash gap | 1,017,184 |
| PX4 package | 367,136 |

These measurements describe the pre-publication working-tree build. Git
version metadata can change image sizes after committing the sources.

To repeat the artifact checks, use the versioned verification script. It
requires Python 3, `arm-none-eabi-nm` and `arm-none-eabi-objdump` from the ARM toolchain, and writes
`protected-artifact-check.json` into the selected build directory:

```sh
python3 boards/px4/fmu-v6x/tools/verify_protected.py build/px4_fmu-v6x_protected
```

Initial USB enumeration, shell entry, the listed diagnostic commands, and USB
reconnection have been confirmed as described above. Quantitative repetition
records and sustained runtime validation remain pending.

## Protected HRT delivery and diagnostic command (v0.1.1)

The protected HRT bridge now retains pending timer notifications in FIFO
order, with an intrusive link separate from the kernel timer queue. It does
not allocate storage in the timer interrupt or impose the old three-entry
limit. There is at most one pending notification per timer; repeated periodic
expiries while it is pending are coalesced and counted. This preserves the
original FIFO position rather than producing a burst of overdue callbacks.
The wake semaphore is a binary notification. The receiver rechecks the queue
after each wake, including a wake left behind by cancellation.

Cancel and rearm remove the old kernel timer and any pending user notification
in one interrupt critical section. The single-core user dispatcher locks
scheduling before waiting for an event and keeps it locked through callback
completion. NuttX allows other tasks to run while that wait blocks, but resumes
the dispatcher with its scheduler lock intact. This prevents another task from
freeing a dequeued entry before its callback finishes. Interrupts remain
enabled. Helpers defer error logging until the dispatcher's outer unlock.

Callbacks must remain short and nonblocking: no sleep, lock waits, allocation,
or blocking I/O. Scheduling/cancellation and entry ownership are normal-task
or owning-callback operations, not asynchronous signal-handler APIs. Self-cancel
and self-rearm are supported; cancel before releasing entry/argument storage,
and serialize concurrent ownership changes. The implementation explicitly
rejects SMP builds. It does not claim arbitrary blocking callback support or
complete parity for every HRT API; the existing missing protected user
`hrt_call_delay()` wrapper remains a follow-up item.

`hrt_called()` still reports the kernel timer deadline state. It does not prove
that a deferred user callback has completed. NULL callbacks remain valid
deadline-only timers and do not enqueue a user notification.

The production bridge and dispatcher pass eight deterministic host lifecycle
checks, including ordinary FIFO delivery, pending cancellation/replacement,
NULL-callback expiry, periodic coalescing, and callback self-cancel/rearm:

```sh
python3 platforms/nuttx/src/px4/common/tests/run_hrt_host_tests.py
```

These checks model timer and OS entry points; they do not prove real NuttX
context switching, ARM privilege, hardware timing, or MPU isolation. See the
[host test notes](../../../platforms/nuttx/src/px4/common/tests/README.md).

After building and uploading this patch, run the following in USB NSH:

```sh
ver all
free
hrt_smoke run
hrt_smoke run
free
ps
```

Each successful run finishes with `hrt_smoke: PASS all checks`. The command
checks one-shot and absolute-time scheduling, cancellation before expiry,
NULL-callback deadline expiry, low-rate periodic callbacks followed by a quiet
period after cancellation, and reuse after cancellation. Callback count,
delay/interval, PID, `CONTROL`, and `IPSR` are printed outside the callback.
`CONTROL.nPRIV=1` provides the user privilege evidence; unprivileged IPSR reads
zero, so IPSR alone is not such evidence. Compare the callback PID with `usr_hrt`
in `ps`. Pending, peak pending, delivered and coalesced counts are also shown.
No new coalescing is expected in this low-load diagnostic; its broad timing
bounds are a smoke check, not a flight-loop latency requirement.

The command uses static storage and prevents concurrent invocations. If any
check fails, save the full output and reboot before retrying; the failed
session does not reuse its callback storage. Successful runs can be repeated
to observe heap and stack trends. Active/pending replacement and callback
self-cancel/rearm have host coverage but are not claimed as board-tested by
this command. The following record confirms the command's basic hardware
checks for `v0.1.1`; it does not establish all HRT behavior or the full `v0.2`
work package.

## First HRT hardware validation (2026-09-15)

The user supplied two consecutive successful `hrt_smoke run` results over USB
NSH. `ver all` reported PX4 commit
`eafec6aa20642f5548bf82a9010a910b56e05981` (the corrected `v0.1.1` tag),
NuttX commit `5ef31ffdf1a29202aca2c76c9727d663b49c0c51`, GCC 13.2.1, and build
time `Sep 15 2026 16:36:06`. The board identified itself as FMUM `0x003` /
BASE `0x005`. Both runs ended with `hrt_smoke: PASS all checks`.

| Observation | Run 1 | Run 2 |
| --- | ---: | ---: |
| Command PID | 12 | 13 |
| Callback PID | 7 | 7 |
| Relative one-shot elapsed (us) | 100,014 | 100,014 |
| Absolute-time rearm elapsed (us) | 100,009 | 100,008 |
| One-shot after periodic cancellation (us) | 100,015 | 100,013 |
| NULL-callback expiry observed (us) | 109,407 | 109,405 |
| Periodic callbacks before cancellation | 5 | 5 |
| Periodic interval min / max (us) | 99,999 / 100,000 | 100,000 / 100,000 |
| Notifications delivered during run | 8 | 8 |
| Pending notifications at end | 0 | 0 |
| Peak pending notifications | 1 | 1 |
| Coalesced notifications during run | 0 | 0 |

The three one-shot callbacks and five periodic callbacks account for all
eight deliveries per run; cumulative deliveries progressed `0 -> 8 -> 16`.
Cancellation before expiry produced no callback, and periodic cancellation
was followed by a 300 ms observation with no additional callback. The test
also successfully reused the same entry after completion/cancellation. The
`callback_pid=-1` / `CONTROL=0` output for the no-callback check is the initial
record value, as expected when nothing executes.

Executed callbacks reported `CONTROL=0x7` and `IPSR=0`; PID 7 matched `usr_hrt`
in the subsequent `ps` output. CONTROL bits `nPRIV=1`, `SPSEL=1`, and `FPCA=1`
indicate unprivileged execution using PSP with the floating-point context
active. IPSR reads zero from unprivileged code and is not independent proof of
privilege or MPU access enforcement. This verifies the basic kernel-timer to
user-dispatcher callback path for the reported image.

The one-shot observations were 8--15 us beyond the requested 100 ms. They
include scheduling/API, syscall and dispatch overhead; they are not isolated
interrupt-latency measurements. The NULL-callback result is the observation
time from `hrt_called()` polling every 10 ms, not callback delay. The periodic
range comes from four measured intervals in each run and is not a worst-case
jitter bound under load.

The `free` snapshots before and after both runs were identical:

| Heap | Total (B) | Used (B) | Free (B) | Largest free (B) | nused | nfree |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Kernel | 245,408 | 36,560 | 208,848 | 207,920 | 165 | 2 |
| User | 130,720 | 18,064 | 112,656 | 112,144 | 57 | 2 |

There was no net heap growth over these two runs. `usr_hrt` used 420 of 960
stack bytes (reported 43.7%), and `px4_entry` used 1,548 of 3,144 (49.2%). The
two diagnostic tasks had exited by the final `ps` snapshot. This short test
does not establish long-duration leak freedom or stack bounds for future
callbacks.

Hardware FIFO ordering across multiple timers, pending-event replacement,
callback self-cancel/rearm, coalescing under backlog, and behavior under
sustained load remain separate checks. The following patch adds a user
`ScheduledWorkItem` diagnostic for actual worker execution, stop/restart and
lifetime cleanup after HRT delivery. Its hardware validation and the remaining
uORB work are still required before `v0.2`.

## User work queue diagnostic (v0.1.2, no patch tag)

Protected user queues now use nested `sched_lock()` / `sched_unlock()` for
queue access. User tasks cannot mask interrupts using the inline ARM
BASEPRI/PRIMASK operations used by the original NuttX queue lock. The new path
requires one CPU and assumes queue producers execute in user task context,
including the nonblocking `usr_hrt` dispatcher. Asynchronous signal handlers
are outside this contract. Kernel queues continue to use IRQ exclusion.

The build compiles `px4_work_queue` for userspace and a separate
`px4_work_queue_kernel` with `__KERNEL__` defined. Each protected platform layer
selects its own archive; direct module dependencies are also mapped for kernel
modules. Seven sensor libraries and `follow_target_estimator` select the archive
and compile definition for their owning module's configured layer. Flat and
POSIX builds keep the original library and lock selection.
The artifact checker rejects either archive appearing in the wrong ELF map
and verifies scheduler calls in user `WorkQueue::Add()` and IRQ masking in
the kernel implementation.
This does not fix the separate uORB user-callback boundary described in the
roadmap.

`work_queue_smoke run` creates a dedicated user queue named `wq:usr_smoke` with
a requested 2,048-byte stack and priority 205. It checks immediate execution,
a 100 ms delayed run, cancellation before a 300 ms deadline, 100 ms periodic
execution, stop, and periodic restart on the same object. Each periodic phase
waits for at least five data runs, then observes 300 ms with no further data
run. The command prints worker PID, CONTROL/IPSR, counts and timing. CONTROL's
nPRIV bit must be set, and the worker PID must remain consistent and differ
from the command PID. Timing bounds are broad functional checks, not a
worst-case latency guarantee.

`ScheduleClear()` does not wait for an item already popped by the worker.
The diagnostic requests a control `Run()` on the same object to acknowledge
quiescence; control runs are counted separately from data runs. Final cleanup
detaches on the worker and waits until that worker PID no longer exists before
deleting the object. The full successful sequence normally has six control
runs. A failed check latches the session; uncertain cleanup retains the object
and static queue configuration until reboot rather than reusing their storage.

After uploading the new firmware, collect this USB NSH sequence:

```sh
ver all
free
work_queue status
work_queue_smoke run
work_queue_smoke run
free
work_queue status
ps
hrt_smoke run
```

Both work queue runs must finish with `PASS cleanup ... exited` and
`PASS all checks`. The private `wq:usr_smoke` task/queue should be absent after
successful cleanup. Record heap and stack observations with the exact firmware
hash and toolchain; background allocation or deferred task-stack reclamation
can affect immediate heap snapshots, so record a later snapshot if they differ.
The final HRT run checks the existing timer path after queue cleanup.

Local GCC 14.2.1 firmware build and artifact checks passed. The actual ARM
`WorkQueue.cpp` also compiled in protected user, protected kernel and flat
variants: user `Add()` calls scheduler locking, kernel/flat retain IRQ masking,
and a protected user SMP configuration is rejected. The existing eight HRT
host checks still passed. Six isolated CMake configurations using the eight
consumer libraries confirmed kernel/user, mixed-owner, flat and POSIX archive
selection and private compile definitions. These libraries remain disabled in
the minimal target; their full compiled runtime paths remain unverified.
These checks do not execute the new diagnostic on
hardware, and no full flat or other-board firmware build is claimed.

| Pre-publication artifact measurement | Bytes |
| --- | ---: |
| Kernel flash image | 272,352 |
| User flash image | 104,384 |
| Combined binary, including flash gap | 1,021,888 |
| Kernel static data | 60,192 |
| User static reservation | 16,384 |

Version metadata can change image sizes after committing. Hardware results
for this patch are pending. The previous `v0.1.1` HRT results
apply to their recorded image and do not validate this new queue path.

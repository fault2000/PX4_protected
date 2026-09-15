# FMUv6X protected firmware bring-up

`px4_fmu-v6x_protected` is a minimal STM32H753II firmware target using NuttX
protected mode and the MPU to separate kernel and user memory. It does not
include the full PX4 flight stack or isolate individual user modules from one
another. Initial hardware startup and USB NSH access/reconnection have been confirmed;
the basic protected HRT and user work queue diagnostics have also passed on hardware.
The `v0.1.3` uORB diagnostic has passed three hardware runs. The `v0.1.4`
waiting-subscriber diagnostic has also passed three runs across user/kernel
producers and poll/condition waits, followed by uORB, HRT and work-queue
regressions in the same image. Repeated startup, sustained runtime stability
and MPU isolation validation remain separate work.

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
hardware bring-up and USB NSH reconnection, and `v0.2` records the basic HRT,
user work-queue and uORB implementation and hardware validation package. The existing `v0.1.1` HRT tag is retained
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

For the separate userspace MAVLink/UART configuration introduced after `v0.2`,
see [PROTECTED_MAVLINK.md](PROTECTED_MAVLINK.md).

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
`perf`, `reboot`, `top`, `listener`, `uorb`, `uorb_smoke`, `uorb_wait_smoke`, `ver`, `work_queue`,
`work_queue_smoke`, and the protected kernel-command launcher. The kernel-only
`uorb_smoke_kernel` companion is invoked by `uorb_smoke`, and `uorb_wait_kernel`
by `uorb_wait_smoke`. Flight modules, PX4
sensor/output drivers, Ethernet, Telnet and PX4 USB protocol autodetection
are disabled. The `reboot` command
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

Repeated cold starts, sustained heap trends, uORB notifications, and MPU
isolation enforcement remain to be verified. Basic HRT callback/cancellation
and user work execution results are recorded below. The static artifact checker does not infer hardware
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
reconnection have been confirmed as described above. USB reconnection and
cold-start repetition counts and sustained runtime validation remain pending.

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
lifetime cleanup after HRT delivery. The following hardware record covers its
basic operation. The uORB records below add basic callback and waiting-subscriber
hardware acceptance. Additional repeated startup and idle observations were
skipped by user decision when closing `v0.2`; no pass is claimed for them.

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
That `v0.1.2` patch addressed queue locking and execution. The `v0.1.3` bridge
described below separately routes uORB callbacks to the user dispatcher.

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

Version metadata can change image sizes after committing. The previous
`v0.1.1` HRT results apply to their recorded image; the following record
provides separate hardware evidence for `v0.1.2`.

## User work queue hardware validation (2026-09-15)

The user reported four successful `work_queue_smoke run` invocations in two
captures without rebooting. The image identified PX4 commit
`963c07808e4d6abab26ac5aae3a2fd1e459475ec`, NuttX
`5ef31ffdf1a29202aca2c76c9727d663b49c0c51`, GCC 13.2.1, build time
`Sep 15 2026 17:01:09`, FMUM `0x003` and BASE `0x005`.

| Observation | Run 1 | Run 2 | Run 3 | Run 4 |
| --- | ---: | ---: | ---: | ---: |
| Command PID | 12 | 14 | 27 | 29 |
| Worker PID | 13 | 15 | 28 | 30 |
| Immediate elapsed (us) | 17 | 17 | 17 | 17 |
| 100 ms delayed elapsed (us) | 100,030 | 100,030 | 100,030 | 100,030 |
| Periodic min / max (us) | 99,999 / 100,000 | 99,999 / 100,000 | 100,000 / 100,000 | 99,999 / 100,000 |
| Restarted periodic min / max (us) | 99,999 / 100,000 | 100,000 / 100,000 | 100,000 / 100,000 | 100,000 / 100,000 |
| Data runs, each periodic phase | 5 | 5 | 5 | 5 |
| Control runs at cleanup | 6 | 6 | 6 | 6 |

Every run passed immediate/delayed execution, cancellation before expiry,
periodic stop/restart, and cleanup. Data runs reported `CONTROL=0x7`, `IPSR=0`
and a worker PID different from the command PID, confirming unprivileged user
worker execution. Cancellation's printed `CONTROL=0` is the reset data record;
the check intentionally produces no data run. Control runs remain separate.
The diagnostic also checked that neither data nor control counters increased
during the post-stop observation windows. The 17 us and 100,030 us values
include API and dispatch work and are not isolated interrupt latency measures;
the short periodic samples do not bound worst-case jitter under load.

All four runs reported `PASS cleanup ... exited` and `PASS all checks`.
Subsequent `work_queue status` showed zero user workers, and `ps` contained no
`wq:usr_smoke` task. The user queue manager remained PID 8, with stack use
612/1,232 B (49.6%); `usr_hrt` remained PID 7, using 420/960 B (43.7%). The
finished workers' own stack high-water marks were not captured.

After the first two queue runs, `hrt_smoke run` also passed all checks in the
same image. HRT callbacks ran as PID 7 with `CONTROL=0x7`; delivered increased
22 -> 30, pending ended at zero, and coalesced stayed zero. The initial 22 is
consistent with 11 timer deliveries per queue run (one delayed plus ten
periodic); the immediate/control runs do not use HRT scheduling.

### Heap observations and interpretation

| Snapshot | Kernel used (B) | User used (B) | Kernel nused | User nused |
| --- | ---: | ---: | ---: | ---: |
| Initial `free`, after `ver all` | 36,560 | 18,064 | 165 | 57 |
| After queue runs 1 and 2 | 36,848 | 20,128 | 168 | 58 |
| Follow-up after HRT run and `sleep 2` | 36,848 | 18,064 | 168 | 57 |
| After queue run 3 and `sleep 2` | 36,848 | 20,128 | 168 | 58 |
| After queue run 4 and `sleep 2` | 36,848 | 20,128 | 168 | 58 |

The kernel plateaued at +288 B / three allocations. User usage returned to
the earlier 18,064 B observation after the HRT command, and repeated queue
runs reproduced 20,128 B rather than accumulating another 2,064 B each time.
The repeated post-queue snapshots also matched in free/largest values:
kernel 208,560/207,728 B, user 110,592/108,896 B. No progressive heap growth was
observed over these four invocations.

The pinned NuttX source provides a consistent explanation, although no
allocation-address tracing was performed to prove ownership.
[`mm_free()`](../../../platforms/nuttx/NuttX/nuttx/mm/mm_heap/mm_free.c) can
put task-exit frees on `mm_delaylist`; a later kernel-side
[`mm_malloc()`](../../../platforms/nuttx/NuttX/nuttx/mm/mm_heap/mm_malloc.c)
for the same heap drains the list. The protected user allocator and a sleep
alone do not drain it.
`ver`, `hrt_smoke`, the queue command and its worker each request a 2,048-byte
stack; with the allocator header and alignment, one such allocation accounts
for 2,064 B. A single-command observation versus command-plus-worker can
therefore differ by one pending stack allocation. Worker PID disappearance
confirms its execution has ended, not that every deferred block is already
reflected as free in heap statistics.

NuttX's [signal-action allocator](../../../platforms/nuttx/NuttX/nuttx/sched/signal/sig_action.c)
allocates entries in blocks of four and returns used entries to a global reuse
list. Three additional 96-byte blocks match the
observed +288 B / three kernel allocations when the extra user worker is
introduced. The stable retained kernel allocation and the reversible user
delta support these explanations; they are not evidence of a per-run leak.
Long-duration, concurrent-worker and allocation-ownership checks remain
outside this short record. Basic `v0.1.2` work queue validation is complete.
The following `v0.1.3` implementation addresses the uORB notification/callback
boundary. Its basic hardware results are recorded below separately.

## Protected uORB callback boundary (v0.1.3, no patch tag)

Protected user callback registration now sends an opaque integer token to a
kernel broker; the user callback object and its virtual `call()` remain in the
user domain. Publication stores the message and queues notifications in the
kernel. The user `usr_uorb` task receives tokens, resolves them in its own
registration table, and invokes user callbacks. Native kernel callbacks retain
their synchronous kernel publication path. A user publication therefore no
longer causes the kernel to invoke the user's callback object directly.

There are 64 live user registration slots for this boot's shared user domain.
The broker scans that fixed table on publication, does not allocate notification
storage on the publish/interrupt path, and keeps pending registrations in FIFO
order. Each registration can have at most one pending notification; further
publications while pending increment the broker's coalesced counter without
moving the registration's FIFO position. Registration fails when capacity is exhausted. Tokens are not
reused during the boot, preventing an old notification from resolving to an
unrelated replacement object.

Notification coalescing is separate from each topic's message queue. A callback
is a request to inspect available data, not one callback guaranteed for every
publication. Topic queue depth and subscription generation still determine
which samples can be read. A depth-one topic yields its latest sample after a
burst; a queued topic must be drained through its normal subscription API.
`SubscriptionCallbackWorkItem` keeps its existing interface and schedules work
from `usr_uorb`, with the actual `Run()` on the user's selected work queue.

`usr_uorb` is started with the user uORB manager, requests a 1,536-byte stack,
and has priority 254. It persists for the boot, including when no callbacks
are registered. The user manager's `terminate()` returns false with `EBUSY`
once this service is started; there is no asynchronous dispatcher teardown.
`ps` should therefore retain `usr_uorb` after a successful smoke run.

### Callback and ownership contract

The dispatcher locks scheduling before waiting for a notification and retains
the lock through the short callback. NuttX permits other tasks to run while
the wait blocks and restores that task's lock state when it resumes. User
registration/state changes use the same nested scheduler exclusion; interrupts
remain enabled. This contract requires one CPU and normal user task context.
Callbacks must not allocate, sleep, print, wait for locks or perform blocking
I/O. Asynchronous signal handlers and SMP are outside this contract.

Unregister removes the user mapping and pending kernel notification. With the
single-core, nonblocking callback contract, an owner can then release callback
resources without racing an already dispatched callback. Self-unregister is
supported. Owners must unregister explicitly **before** destruction of derived
resources or the associated work item, and separately quiesce queued/in-flight
work before destroying it; `ScheduleClear()` alone is not that completion
barrier. The base callback destructor runs too late to protect resources
already destroyed by a derived destructor.

`SubscriptionCallback::unsubscribe()` now unregisters before unsubscribing,
and instance changes are serialized with callback state. The inherited
`SubscriptionInterval::unsubscribe()` is nonvirtual: calling it through a base
reference bypasses the callback-aware method. Such callers must explicitly
unregister first. Concurrent ownership changes still require one owner.

`SubscriptionBlocking::updatedBlocking()` now handles callback registration
failure rather than waiting on an unregistered subscription. The protected user
path also rechecks for an update while holding the condition mutex and the
scheduler guard, then retains the guard through NuttX's condition wait. This
closes the check-to-wait window within the same single-core contract. The
`v0.1.3` board diagnostic does not exercise a sleeping `SubscriptionBlocking`
waiter. The `v0.1.4` diagnostic below has passed three hardware runs of that
path, including finite timeout, periodic wake/read/re-wait and reuse. This
covers its 200 ms and 1 s wait arguments, not every timeout or load condition.

This change establishes a functional callback execution boundary. It does not
provide individual user-module isolation, validate every syscall argument, or
prove MPU access enforcement or a complete security boundary.

### uORB diagnostic and local validation

`uorb_smoke run` uses only the existing diagnostic topics `orb_test` and
`orb_multitest`, instance 0. Run it without another publisher on these topics.
It checks subscribe/copy, `poll()` readiness after a publication, then the
absence of readiness after copying that sample. This is **not** a test of
waking a subscriber already asleep inside `poll()` or `SubscriptionBlocking`.

The direct callback checks user PID/CONTROL and copied data, pending
unregister before dispatch, re-registration, an eight-publication burst with
one delivery and the latest value, and self-unregister. The burst holds the
single-core scheduler lock around ordinary publication to make the pending
state deterministic. It also traces the production
`SubscriptionCallbackWorkItem::call()` into a private `wq:uorb_smoke` user
worker, verifies actual `Run()` data/context, then verifies no further callback
or data run after unregister. Final cleanup detaches on that worker and waits
for its PID to disappear before deleting its work item.

The command invokes the fixed `uorb_smoke_kernel run` builtin through the
existing kernel launcher. That companion spawns an actual `uorb:k_smoke` kernel
thread; the launch ioctl's caller alone would not establish a separate kernel
producer task. The helper reads a fixed user request from `orb_multitest`,
registers a native kernel callback, publishes a fixed reply, validates the
native callback and copied reply, unregisters, and waits for kernel worker exit.
Together the sequence covers these basic paths:

| Direction | Diagnostic evidence required |
| --- | --- |
| User -> user | User publication read by user subscription/callback; user callback schedules a user work item |
| User -> kernel | Actual kernel worker copies the user's fixed request |
| Kernel -> kernel | Kernel publication invokes its native callback in the kernel worker's PID with `CONTROL.nPRIV=0` |
| Kernel -> user | User dispatcher copies the kernel worker's fixed reply with `CONTROL.nPRIV=1` |

The direct user callback uses `copy()` to read retained topic data, allowing
reply delivery after the helper unadvertises. `SubscriptionInterval::update()`
checks whether the topic is advertised, so it can reject such a final retained
sample; the diagnostic does not change or promise different semantics for that
API. Unprivileged `IPSR` reads zero and is not independent privilege evidence.
These short checks do not establish timing bounds or access-denial behavior.

A successful isolated run normally reports `delivered_delta=7` and
`coalesced_delta=7` for user notifications. The four direct callbacks, one
work-item callback, and request/reply callbacks account for the seven
deliveries. Registered and pending counts must return to their starting values;
peak pending and delivery/coalescing statistics are cumulative for the boot.
`wq:uorb_smoke` and `uorb:k_smoke` must exit; `usr_uorb` stays alive. A failed
check latches the diagnostic session and retains uncertain callback/work-item
storage. Save its full output and reboot before retrying after a failure.

The local GCC 14.2.1 protected firmware build and static artifact checker
passed. The checker verifies user placement of the dispatcher and `uorb_smoke`,
kernel placement of the broker and companion, and the existing image layout,
work-queue linkage, USB and allocator checks. Seven deterministic broker host
checks and the existing eight HRT host checks passed:

```sh
python3 platforms/common/uORB/test/protected_callbacks/run_host_tests.py
python3 platforms/nuttx/src/px4/common/tests/run_hrt_host_tests.py
python3 boards/px4/fmu-v6x/tools/verify_protected.py build/px4_fmu-v6x_protected
```

The broker checks cover FIFO/multiple registrations, coalescing, pending
unregister, stale wakeups, slot reuse, full capacity and token identity. They
compile production broker code with modeled semaphore/critical-section entry
points; they do not execute the user dispatcher or real ARM/NuttX scheduling.
See the [broker host test notes](../../../platforms/common/uORB/test/protected_callbacks/README.md).
No full flat or other-board firmware build is claimed.

| Pre-publication artifact measurement | Bytes |
| --- | ---: |
| Kernel flash image | 275,552 |
| User flash image | 111,072 |
| Combined binary, including flash gap | 1,028,576 |
| Kernel static data | 61,296 |
| User static reservation | 16,384 |

These are local working-tree build measurements; later source or Git-version
metadata changes can affect image sizes. The GCC 13.2.1 hardware record below
applies to commit `a69eb3fa3f`, not to this locally built GCC 14.2.1 binary.
Earlier `v0.1.1`/`v0.1.2` board results remain evidence for their recorded images.

### Hardware regression procedure

Build and upload the new image, then record `ver all` and the complete USB NSH
output. Use the first uORB run as warm-up before comparing repeated samples:

```sh
ver all

free

uorb_smoke run

sleep 2

free

uorb_smoke run

sleep 2

free

uorb_smoke run

sleep 2

free

work_queue status

ps

hrt_smoke run

work_queue_smoke run

sleep 2

free

work_queue status

ps
```

Require `PASS all checks` for each diagnostic and successful user/kernel worker
cleanup. Compare callback PID with `usr_uorb` in `ps`, and record stack usage
for that persistent dispatcher. The first use creates two persistent uORB
nodes/buffers, and NuttX can retain task-exit stacks for deferred reclamation
or reuse signal-pool allocations, as in the earlier work-queue observations.
Compare warmed-up repetitions and subsequent HRT/work-queue snapshots rather
than equating an immediate heap difference with a leak. A sleep alone is not
guaranteed to drain deferred frees. Record whether any increase accumulates.

The following record completes basic `v0.1.3` uORB hardware acceptance. The
later `v0.1.4` record adds real sleeping-subscriber wake checks. Concurrent-topic
load, sustained heap trends and independent MPU validation remain follow-up
work. The `v0.2` milestone records the completed basic validation scope.

## uORB hardware validation (2026-09-15)

The user supplied one capture containing three successful `uorb_smoke run`
invocations, then HRT and work-queue regressions, without a reported intervening
reboot. The image identified PX4
`a69eb3fa3fb6a530d5bf7a281736ebe8290dbba6`, NuttX
`5ef31ffdf1a29202aca2c76c9727d663b49c0c51`, GCC 13.2.1, build time
`Sep 15 2026 17:43:18`, FMUM `0x003`, BASE `0x005` and MCU revision V.

| Observation | Run 1 | Run 2 | Run 3 |
| --- | ---: | ---: | ---: |
| Command PID | 13 | 26 | 29 |
| User callback PID | 9 | 9 | 9 |
| User work-item PID | 14 | 27 | 30 |
| Kernel worker/native callback PID | 15 | 28 | 31 |
| User notification delivered delta | 7 | 7 | 7 |
| User notification coalesced delta | 7 | 7 | 7 |
| Final registered / pending | 0 / 0 | 0 / 0 | 0 / 0 |
| Peak pending | 1 | 1 | 1 |

All three runs passed setup, publication-before-poll readiness/copy, direct
user callback, pending unregister, re-registration, burst coalescing,
self-unregister, callback-to-work-item delivery and unregister quiet checks.
The controlled eight-publication burst produced one notification and seven
coalesces, copying the latest value 407 as intended for the depth-one topic.
The work-item callback's printed value 0 is its context-only trace; actual
`Run()` separately copied and checked value 601.

The user callbacks ran with `CONTROL=0x7`, including the kernel reply receiver.
PID 9 matched `usr_uorb` in `ps`, confirming user dispatcher execution with
nPRIV=1. User work-item runs also reported `CONTROL=0x7`. Each actual kernel
worker and its native callback shared a PID and reported `CONTROL=0x4`, whose
nPRIV bit is clear. The helper passed the user-request read and kernel reply,
and the user callback copied response 9320 (`0x2468`). These checks establish
the tested U->U, U->K, K->K and K->U functional paths. Both contexts printed
`IPSR=0`; privilege evidence comes from CONTROL.nPRIV, not IPSR alone.

Every user and kernel diagnostic worker reported successful exit. Each run
ended with `PASS callback-cleanup` and `PASS all checks`. The helper's
`released=1` means its publisher was successfully unadvertised; it is not a
claim that all deferred kernel allocations have returned to the heap.

After the third uORB run, `work_queue status` showed zero user workers and
`ps` showed no temporary uORB worker. The persistent task observations were:

| Task | PID | Priority | Stack used / reported size | Filled |
| --- | ---: | ---: | ---: | ---: |
| `usr_uorb` | 9 | 254 | 420 / 1,488 B | 28.2% |
| `usr_hrt` | 7 | 255 | 420 / 960 B | 43.7% |
| User `wq:manager` | 8 | 255 | 620 / 1,232 B | 50.3% |
| `px4_entry` | 4 | 100 | 1,516 / 3,144 B | 48.2% |

`usr_uorb` was waiting on a semaphore as expected with no pending notifications.
These are the post-uORB, pre-HRT/work-queue-regression observations; the capture
does not contain another `ps`/queue-status pair after the final regression.
The completed temporary workers' stack high-water marks were not collected.

### Heap observations

| Snapshot | Kernel used (B) | User used (B) | Kernel nused / nfree | User nused / nfree |
| --- | ---: | ---: | ---: | ---: |
| Initial, after `ver all` | 37,312 | 20,656 | 172 / 2 | 60 / 2 |
| After uORB run 1 and `sleep 2` | 37,776 | 22,928 | 179 / 3 | 66 / 3 |
| After uORB run 2 and `sleep 2` | 37,776 | 22,928 | 179 / 3 | 66 / 3 |
| After uORB run 3 and `sleep 2` | 37,776 | 22,928 | 179 / 3 | 66 / 3 |
| After HRT + work queue and `sleep 2` | 37,776 | 22,928 | 179 / 3 | 66 / 4 |

The first run increased kernel usage by 464 B and seven used blocks, and user
usage by 2,272 B and six used blocks. These are net changes in the snapshots,
not counts of allocation calls. Subsequent repetitions did not accumulate
more used memory. All four post-run snapshots retained kernel free/largest
207,632/206,784 B and user free/largest 107,792/105,920 B. The final user free
block count changed from three to four; the heap statistics are therefore not
identical in every field, although used/free/largest and used-block counts
remain unchanged. This is not evidence by itself of a leak or corruption.
There is no HRT-only heap snapshot to identify which regression changed the
free-block layout.

The persistent diagnostic topic nodes/buffers, deferred task-stack reclamation
and reusable signal allocations described above are relevant to first-use
versus repeated snapshots. This capture did not trace allocation addresses or
prove ownership of the retained bytes. It supports no observed cumulative
growth across these three uORB runs and the subsequent regressions, rather
than long-duration leak freedom or a peak-memory bound.

### HRT and work-queue regressions in the same image

`hrt_smoke run` (command PID 43) passed all checks with callback PID 7 and
`CONTROL=0x7`. Delivered increased 0 -> 8, pending ended at zero, max_pending
was one and coalesced stayed zero. The 100 ms one-shot observation was
100,053 us, absolute rearm 100,008 us, after-rearm 100,012 us, and periodic
intervals 99,999--100,000 us. NULL-callback expiry was observed at 109,408 us
through polling. These short observations do not establish worst-case latency.

`work_queue_smoke run` (command PID 44, worker PID 45) passed all checks with
`CONTROL=0x7`. Immediate execution took 17 us and the delayed run 100,030 us.
Periodic intervals were 99,999--100,000 us; after restart they were
99,999--100,001 us, with five data runs per phase. Six control runs and
`PASS cleanup worker_pid=45 exited` confirmed the diagnostic's own cleanup.
The final heap observation is included above.

Basic `v0.1.3` uORB validation is complete. The `v0.1.4` diagnostic below covers
a subscriber already waiting in `poll()` or `SubscriptionBlocking`, finite
timeout without publication, wake/read/re-wait cycles, and limited periodic
publication with sequence/timestamp observations. Its three successful hardware
runs are recorded below. Multi-subscriber load, long-duration operation,
repeated startup and independent MPU validation
remain separate items outside the basic validation scope used to close `v0.2`.

## Waiting-subscriber diagnostic (v0.1.4, no patch tag)

`uorb_wait_smoke run` exercises four combinations: `poll()` and the production
`SubscriptionBlocking<orb_test_s>`, each with a user producer and a kernel
producer. The command task is the consumer; all data uses the existing
`orb_test` instance 0. Run this command alone, without other diagnostics or
publishers using that topic.

The kernel-only `uorb_wait_kernel` helper uses the existing fixed builtin
launcher. At session start it records the caller's PID; it accepts no target
PID or executable address. The user supplies an aligned wait-identity object
in user BSS, and the helper validates its full range against the userspace
header. A short kernel critical section obtains the caller's TCB and compares
`TSTATE_WAIT_SEM` and its actual `waitsem` with the specific API's semaphore:

- For `poll()`, NuttX writes the semaphore pointer into the static `pollfd`.
  The helper compares that pointer without dereferencing the semaphore.
- For `SubscriptionBlocking`, a friend struct defined only in the diagnostic
  obtains the private condition semaphore's identity. Its object uses aligned
  static storage and runtime construction. The production wait and callback
  implementations, class layout and public API remain unchanged.

This comparison distinguishes the intended wait from an unrelated mutex or
allocation wait during API setup. The helper reports observations and context,
not kernel addresses. A new sequence is armed for each wait, so observations
from earlier waits cannot satisfy later checks.

For user production, a separate `uorb:usr_wait` task (requested stack 2,048 B,
priority 90) confirms the exact wait through the helper immediately before
ordinary uORB publication. The kernel `uorb:k_wait` task (requested stack
2,048 B, priority 100) observes the wait in all cases and also publishes in
kernel-producer cases. Each producer holds scheduling locked from the final
wait check through publication and accounting; interrupts remain enabled.
The command requests a 2,560-byte stack. Consumer and user producer check
CONTROL.nPRIV=1, while the kernel task checks nPRIV=0.

Each combination performs this finite sequence on the same subscription:

| Sequence | Operation |
| --- | --- |
| 1 | No publication: observe the exact wait and require a 200 ms timeout |
| 2--6 | Five samples at absolute 100 ms publication deadlines; observe a fresh wait, wake, copy and drain each sample |
| 7 | Another no-publication 200 ms timeout after periodic delivery |
| 8 | One final publication to verify reuse after that timeout |

Each case requires eight exact wait observations and six matching samples.
The full command therefore checks 32 waits and 24 samples. With no other user
callbacks active, the two blocking cases normally add 12 uORB callback
deliveries with no coalescing. Final registered and pending counts must return
to their initial values.

The timeout checks accept 180--500 ms of observed elapsed time; the current
NuttX tick is 1 ms. Wake waits use a 1 s API timeout but must return within
500 ms. That margin also rejects a publication arriving only after the timeout
interrupt, which scheduler locking alone cannot rule out. Publication lateness
is limited to 50 ms; exceeding that limit fails without shifting subsequent
absolute deadlines. These generous limits are functional checks, not promised
real-time performance.

The command prints missing/old sequence counts, publication interval,
publication lateness and publication-to-return latency ranges. `poll()` records
return time before copying; `updateBlocking()` records return time after its
internal copy. Those measurement points differ, so the values are not an
isolated or directly comparable scheduler-latency benchmark. The observer's
short sleeps control polling overhead and are not evidence that the consumer
entered its intended wait.

Cleanup stops and confirms exit of the user producer before stopping the
kernel observer. On normal completion the helper retains its publisher until
the consumer finishes, then unadvertises and confirms worker exit. Failure or
session expiry can end publication before the consumer's finite wait returns.
Only after safe exits are the
subscription, condition object and user publisher released. Uncertain cleanup
retains the static objects/handles and latches the command until reboot; no
task is forcibly killed. Each observer/producer loop checks a 10-second active
session deadline; cleanup/join has separate bounds. This is not a hard watchdog
or a bound on the whole command's execution time.
The helper's `released` result concerns unadvertising, not complete heap
reclamation.

The artifact checker also verifies both new builtin placements and the two
wait-identity storage symbols in user BSS. Existing uORB broker host checks
and HRT regressions remain separate from this diagnostic: they do not execute
these real NuttX waits. The local GCC 14.2.1 full protected build and artifact
checker passed, as did the existing seven broker and eight HRT host checks.
No full flat or other-board build is claimed. These local build/host checks
are separate from the GCC 13.2.1 hardware results below; the local GCC 14.2.1
binary has not been hardware-validated by that capture.

| Pre-publication artifact measurement | Bytes |
| --- | ---: |
| Kernel flash image | 279,168 |
| User flash image | 116,544 |
| Combined binary, including flash gap | 1,034,048 |
| Kernel static data | 61,432 |
| User static reservation | 16,384 |

These are local working-tree measurements; final Git metadata can change image
sizes. The following hardware record identifies its own source and toolchain;
these local sizes are not measurements of the user's GCC 13.2.1 image.

### Hardware regression procedure

After building and uploading, collect this USB NSH sequence. The first run
warms up persistent uORB/poll storage and reusable task resources; compare
subsequent heap samples rather than requiring the initial allocation count
to remain unchanged.

```sh
ver all
free

uorb_wait_smoke run
sleep 2
free

uorb_wait_smoke run
sleep 2
free

uorb_wait_smoke run
sleep 2
free

ps

uorb_smoke run
hrt_smoke run
work_queue_smoke run
sleep 2
free
work_queue status
ps
```

Require all four cases, observer/producer cleanup and the command to pass.
Record per-case timings, exact-wait counts, callback counts, heap changes and
remaining task stacks with the exact image hash and toolchain. Temporary
`uorb:usr_wait` and `uorb:k_wait` tasks should be absent after cleanup;
`usr_uorb` and `usr_hrt` remain. These checks do not replace multi-subscriber
stress or sustained heap/timing observations tracked as follow-up work.

### Waiting-subscriber hardware validation (2026-09-15)

The user supplied the complete sequence above from FMUv6X, FMUM `0x003`, BASE
`0x005`, MCU revision V. The image reported PX4
`8e05753feb2d7f6b96594bf0ad316bdec06c8ed7`, NuttX
`5ef31ffdf1a29202aca2c76c9727d663b49c0c51`, protected mode, GNU GCC
`13.2.1 20231009`, and build time `Sep 15 2026 18:51:28`. No reboot was reported
between these commands. All three `uorb_wait_smoke run` invocations passed all
four cases and cleanup. No fault or failed check appears in the capture.

| Result | Each invocation | Across three invocations |
| --- | ---: | ---: |
| Exact API semaphore wait observations | 32 | 96 |
| No-publication timeouts | 8 | 24 |
| Received samples | 24 | 72 |
| Periodic samples / final re-wake samples | 20 / 4 | 60 / 12 |
| Missed / duplicate or old samples | 0 / 0 | 0 / 0 |
| uORB callback deliveries / coalesced | 12 / 0 | 36 / 0 |
| Final registered / pending | 0 / 0 | 0 / 0 after each run |

Each case reported `observations=8 samples=6`, and every observer reported
`count=8 worker=8 check=0`. These last two counters identify which path first
observed each sequence: the worker observed all eight first. User producers
still called `check` immediately before publication, rechecking the exact wait;
`check=0` does not mean those checks were omitted. All 12 kernel workers
reported `exited=1 released=1 reason=none`, and all six user producers reported
`published=6 exited=1`. As above, `released` means successful unadvertising.

Command/consumer PIDs 13, 31 and 59 and user producer PIDs 15, 28, 43, 46, 61
and 74 reported `CONTROL=0x7 IPSR=0`. Kernel observer/producer PIDs 14, 26, 27,
30, 42, 44, 45, 58, 60, 62, 63 and 76 reported `CONTROL=0x4 IPSR=0`.
The nPRIV bit confirms the intended user/kernel execution privileges; the
recorded IPSR values were zero.

Timing ranges below combine all three runs; every value is in microseconds.
The interval column covers each five-sample periodic batch, while publication
lateness and receive latency also include the final re-wake sample.

| Case | First / second timeout | Publication lateness | Receive latency | Periodic publication interval |
| --- | ---: | ---: | ---: | ---: |
| poll / user | 200,766–200,767 / 200,936–200,940 | 988–1,990 | 15–18 | 99,998–100,001 |
| poll / kernel | 200,929–200,936 / 200,955 | 973–976 | 12–13 | 99,999–100,001 |
| SubscriptionBlocking / user | 200,767–200,769 / 200,916–200,918 | 991–993 | 40–42 | 99,999–100,000 |
| SubscriptionBlocking / kernel | 200,910–200,912 / 200,935 | 973–974 | 39–41 | 100,000–100,000 |

Publication lateness measures release relative to the planned deadline; it is
not subscriber wake latency. The roughly 1 ms release delays are consistent
with the producer's 1 ms sleep loop and scheduler tick, without identifying the
cause of every individual delay. Poll receive timing ends immediately after
`poll()` returns, before copying, whereas blocking receive timing includes
`updateBlocking()` and its copy. These measurements are not equivalent API
benchmarks. These short runs establish neither worst-case latency under load
nor sensor-to-control latency.

#### Heap and task observations

Each post-command heap snapshot followed `sleep 2`. Totals remained 245,408 B
for Kmem and 130,720 B for Umem. Values below are bytes except block counts.

| Snapshot | Kmem used / free / largest | Kmem nused / nfree | Umem used / free / largest | Umem nused / nfree |
| --- | ---: | ---: | ---: | ---: |
| Before first wait run | 37,312 / 208,096 / 207,168 | 172 / 2 | 20,656 / 110,064 / 109,552 | 60 / 2 |
| After wait run 1 | 37,680 / 207,728 / 203,280 | 177 / 4 | 23,344 / 107,376 / 105,824 | 64 / 3 |
| After wait run 2 | 37,680 / 207,728 / 203,280 | 177 / 4 | 23,344 / 107,376 / 105,824 | 64 / 3 |
| After wait run 3 | 37,680 / 207,728 / 203,280 | 177 / 4 | 23,344 / 107,376 / 105,824 | 64 / 3 |
| After uORB, HRT and work-queue regressions | 37,776 / 207,632 / 203,280 | 179 / 4 | 22,928 / 107,792 / 103,824 | 66 / 4 |

The first wait run added 368 B of kernel use and 2,688 B of user use. All
reported heap fields then matched across the three post-wait snapshots: no
accumulating increase was observed in these repetitions.

The final snapshot followed a different workload, with no intermediate heap
samples between the three regression commands. Compared with the wait plateau,
Kmem use increased 96 B and Umem use decreased 416 B; each gained two used
blocks. User free space increased while its largest free block decreased
2,000 B. This is not an identical final heap or evidence of per-run growth.
The wait command uses only `orb_test`; `uorb_smoke` additionally creates retained
`orb_multitest` storage. Its extra topic allocations and the 512 B difference
between the wait and work-queue command stack requests are consistent with
these changes when deferred stack reclamation is considered. This is a
source-consistent explanation, not allocation ownership traced on this board.
Long-term fragmentation and heap trends remain unmeasured.

| Persistent task | PID | Stack used / reported size after wait run 3 | After all regressions |
| --- | ---: | ---: | ---: |
| `usr_uorb` | 9 | 420 / 1,488 B (28.2%) | 420 / 1,488 B (28.2%) |
| `usr_hrt` | 7 | 420 / 960 B (43.7%) | 420 / 960 B (43.7%) |
| User `wq:manager` | 8 | 372 / 1,232 B (30.1%) | 628 / 1,232 B (50.9%) |
| `px4_entry` | 4 | 1,508 / 3,144 B (47.9%) | 1,548 / 3,144 B (49.2%) |

Both `ps` snapshots contained only the persistent PIDs 0–9; the temporary wait,
uORB and work-queue workers were absent. Final `work_queue status` reported zero
user work queues. Terminated diagnostic tasks' own stack high-water marks were
not captured, and PID disappearance does not prove immediate heap reclamation.

#### Existing diagnostics on the same image

- `uorb_smoke run` passed all checks. User callbacks ran in PID 9, the user
  WorkItem in PID 78 (`CONTROL=0x7`), and the kernel worker/native callback in
  PID 79 (`CONTROL=0x4`). Delivered/coalesced deltas were 7/7, final registered
  and pending were zero, and both temporary workers exited.
- `hrt_smoke run` passed all checks with callback PID 7, `CONTROL=0x7`.
  Delivered increased 0→8 with zero pending and zero coalesced at the end.
  Five periodic callbacks had 100,000 us intervals. One-shot observations were
  100,013, 100,008 and 100,012 us; NULL-callback expiry was detected at 109,408 us
  by polling, as in the earlier diagnostic.
- `work_queue_smoke run` passed with worker PID 92, `CONTROL=0x7`. Immediate
  work ran after 18 us and delayed work after 100,031 us. Both five-run periodic
  phases measured 99,999–100,000 us intervals. Six control runs completed and
  cleanup confirmed worker exit.

Basic `v0.1.4` waiting-subscriber hardware validation is complete. On 2026-09-15
the user chose to skip the additional ten cold starts and 30-minute idle
observation and move to the next work package. These checks are unperformed,
not passed. `v0.2` closes the implementation and basic hardware validation
scope with that explicit exclusion; no new patch tag is created. Multi-subscriber/load tests, longer
timing and memory observations, and independent MPU validation retain their
separate follow-up scope in [PROTECTED_ROADMAP.md](PROTECTED_ROADMAP.md).

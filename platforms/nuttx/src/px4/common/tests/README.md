# Protected HRT host regressions

Run from the repository root:

```sh
python3 platforms/nuttx/src/px4/common/tests/run_hrt_host_tests.py
```

Requirements: Python 3 and host C/C++ compilers (`cc`, `c++`, or `CC`/`CXX`).
The runner compiles the production `hrt_ioctl.c` and `usr_hrt.cpp`, using the
production HRT/boardctl declarations and NuttX queue layout. Build products and
small include wrappers exist only in a temporary directory. No firmware build,
generated NuttX configuration, or hardware connection is needed.

The harness supplies a deterministic timer and external OS entry points. It
checks FIFO delivery, cancellation before expiry and while pending, replacement
of a pending call by a new schedule, NULL-callback timeouts, periodic delivery,
coalescing of an already pending periodic event, binary wake synchronization,
and self-cancellation/rearming within the callback's scheduler lock. The
dispatcher runs the actual production loop; the harness stops it at a wait
boundary after the requested number of deliveries.

The timer model preserves the hardware driver's deadline/period semantics but
does not exercise the STM32 timer. Scheduler nesting and interrupt exclusion
are checked as API contracts; host stubs do not prove NuttX context-switch
behavior, callback privilege, real timing/jitter, or MPU isolation. Those checks
still require the FMUv6X diagnostic command and hardware validation.

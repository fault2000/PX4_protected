# Protected uORB callback broker host checks

From the repository root:

```sh
python3 platforms/common/uORB/test/protected_callbacks/run_host_tests.py
```

The runner compiles the production `uORBUserCallback.cpp` and its shared ABI
header, replacing only the platform critical-section and semaphore interfaces.
It checks FIFO delivery, multiple subscriptions, coalescing, pending unregister,
stale wakeups, slot reuse, full registration/pending capacity, and token identity.
The semaphore stub asserts that the doorbell has at most one outstanding token.

These deterministic checks do not model ARM privilege transitions, actual NuttX
scheduling, user callback execution, or timing. The `uorb_smoke` board diagnostic
and firmware/static checks cover the integration separately.

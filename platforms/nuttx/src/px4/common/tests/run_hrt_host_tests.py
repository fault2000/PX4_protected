#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Compile the production protected HRT transport with deterministic host stubs."""

import os
from pathlib import Path
import shlex
import subprocess
import tempfile


def main():
    tests = Path(__file__).resolve().parent
    root = tests.parents[5]
    nuttx = root / "platforms/nuttx/NuttX/nuttx"
    common = tests.parent
    wrappers = [
        "px4_platform_common/px4_config.h",
        "px4_platform_common/defines.h",
        "px4_platform_common/posix.h",
        "px4_platform_common/shutdown.h",
        "px4_platform_common/log.h",
        "px4_platform_common/sem.h",
        "px4_platform_common/time.h",
        "px4_platform/micro_hal.h",
        "board_config.h",
        "debug.h",
        "sys/boardctl.h",
    ]
    kernel_names = [
        "hrt_absolute_time", "hrt_call_after", "hrt_call_at", "hrt_call_every",
        "hrt_cancel", "get_latency", "reset_latency_counters",
    ]

    with tempfile.TemporaryDirectory(prefix="px4-protected-hrt-") as temp:
        build = Path(temp)
        for name in wrappers:
            header = build / name
            header.parent.mkdir(parents=True, exist_ok=True)
            header.write_text('#include "hrt_host_stubs.h"\n')
        # Include just the queue header, without putting NuttX's libc headers
        # ahead of the host compiler's system headers.
        (build / "queue.h").symlink_to(nuttx / "include/queue.h")
        includes = [
            f"-I{build}", f"-I{tests}", f"-I{root / 'src'}",
            f"-I{common / 'include'}",
            "-include", str(tests / "hrt_host_stubs.h"),
        ]
        warnings = ["-Wall", "-Wextra", "-Werror", "-Wno-unused-parameter"]
        cc = shlex.split(os.environ.get("CC", "cc"))
        cxx = shlex.split(os.environ.get("CXX", "c++"))
        kernel = build / "hrt_ioctl.o"
        user = build / "usr_hrt.o"
        executable = build / "hrt_host_test"
        commands = [
            cc + ["-std=gnu11"] + warnings + includes
            + [f"-D{name}=kernel_{name}" for name in kernel_names]
            + ["-c", str(common / "hrt_ioctl.c"), "-o", str(kernel)],
            cxx + ["-std=gnu++17"] + warnings + includes
            + ["-c", str(common / "usr_hrt.cpp"), "-o", str(user)],
            cxx + ["-std=gnu++17"] + warnings + includes
            + [str(tests / "hrt_host_test.cpp"), str(kernel), str(user),
               "-o", str(executable)],
            [str(executable)],
        ]
        for command in commands:
            subprocess.run(command, check=True, timeout=30)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Exercise the production kernel uORB callback broker with platform stubs."""

import os
from pathlib import Path
import shlex
import subprocess
import tempfile


def main():
    tests = Path(__file__).resolve().parent
    uorb = tests.parents[1]
    cxx = shlex.split(os.environ.get("CXX", "c++"))
    with tempfile.TemporaryDirectory(prefix="px4-protected-uorb-") as temp:
        build = Path(temp)
        for name in ("px4_config.h", "posix.h", "sem.h"):
            wrapper = build / "px4_platform_common" / name
            wrapper.parent.mkdir(parents=True, exist_ok=True)
            wrapper.write_text('#include "host_stubs.hpp"\n')
        executable = build / "protected_uorb_callback_test"
        command = cxx + [
            "-std=c++17", "-Wall", "-Wextra", "-Werror",
            f"-I{build}", f"-I{tests}", f"-I{uorb}",
            "-include", str(tests / "host_stubs.hpp"),
            str(uorb / "uORBUserCallback.cpp"), str(tests / "host_test.cpp"),
            "-o", str(executable),
        ]
        subprocess.run(command, check=True, timeout=30)
        subprocess.run([str(executable)], check=True, timeout=30)


if __name__ == "__main__":
    main()

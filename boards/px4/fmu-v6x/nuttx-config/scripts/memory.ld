/****************************************************************************
 * scripts/memory.ld
 *
 *   Copyright (C) 2016 Gregory Nutt. All rights reserved.
 *   Author: Gregory Nutt <gnutt@nuttx.org>
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in
 *    the documentation and/or other materials provided with the
 *    distribution.
 * 3. Neither the name NuttX nor the names of its contributors may be
 *    used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS
 * FOR A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE
 * COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING,
 * BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS
 * OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED
 * AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN
 * ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 *
 ****************************************************************************/

/* STM32H753II: the bootloader owns the first 128 KiB of the 2 MiB flash.
 * The user image starts at bank 2 and must match CONFIG_NUTTX_USERSPACE.
 *
 * AXI SRAM layout for the initial protected configuration:
 *   0x24000000..0x24020000: kernel data, bss and idle stack (128 KiB)
 *   0x24020000..0x24040000: user data and bss (at most 128 KiB)
 *   user _ebss..0x24060000: kernel heap (at least 128 KiB)
 *   0x24060000..0x24080000: user heap (128 KiB)
 *
 * This requires CONFIG_MM_KERNEL_HEAPSIZE=131072. User _ebss is aligned
 * to a 16 KiB MPU subregion boundary so the kernel heap remains protected.
 * CONFIG_MM_REGIONS=1 initially leaves the other SRAM banks out of the
 * allocator. SRAM4 remains available for the board's BDMA reservations.
 */

MEMORY
{
    kflash     (rx) : ORIGIN = 0x08020000, LENGTH = 896K
    uflash     (rx) : ORIGIN = 0x08100000, LENGTH = 1024K

    ITCM_RAM  (rwx) : ORIGIN = 0x00000000, LENGTH = 64K
    DTCM1_RAM (rwx) : ORIGIN = 0x20000000, LENGTH = 64K
    DTCM2_RAM (rwx) : ORIGIN = 0x20010000, LENGTH = 64K
    ksram     (rwx) : ORIGIN = 0x24000000, LENGTH = 128K
    usram     (rwx) : ORIGIN = 0x24020000, LENGTH = 128K
    SRAM1     (rwx) : ORIGIN = 0x30000000, LENGTH = 128K
    SRAM2     (rwx) : ORIGIN = 0x30020000, LENGTH = 128K
    SRAM3     (rwx) : ORIGIN = 0x30040000, LENGTH = 32K
    SRAM4     (rwx) : ORIGIN = 0x38000000, LENGTH = 64K
    BKPRAM    (rwx) : ORIGIN = 0x38800000, LENGTH = 4K
}

_uheap_end = 0x24080000;

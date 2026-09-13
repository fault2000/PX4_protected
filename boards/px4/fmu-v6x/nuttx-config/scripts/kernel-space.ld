/****************************************************************************
 * kernel-space.ld
 *
 *   Copyright (C) 2015 Gregory Nutt. All rights reserved.
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

/* Include memory.ld before this script. */

OUTPUT_ARCH(arm)
EXTERN(_vectors)
ENTRY(_stext)
EXTERN(abort)
EXTERN(_bootdelay_signature)
EXTERN(board_get_manifest)

SECTIONS
{
    .text : {
        _stext = ABSOLUTE(.);
        KEEP(*(.vectors))
        . = ALIGN(32);
        /* PX4 bootloader delay signature. */
        _bootdelay_signature = ABSOLUTE(.);
        FILL(0xffecc2925d7d05c5)
        . += 8;
        *(.text .text.*)
        *(.fixup)
        *(.gnu.warning)
        *(.rodata .rodata.*)
        *(.gnu.linkonce.t.*)
        *(.glue_7)
        *(.glue_7t)
        *(.got)
        *(.gcc_except_table)
        *(.gnu.linkonce.r.*)
        _etext = ABSOLUTE(.);
    } > kflash

    .init_section : {
        _sinit = ABSOLUTE(.);
        KEEP(*(.init_array .init_array.*))
        _einit = ABSOLUTE(.);
    } > kflash

    .ARM.extab : {
        *(.ARM.extab*)
    } > kflash

    .ARM.exidx : {
        __exidx_start = ABSOLUTE(.);
        *(.ARM.exidx*)
        __exidx_end = ABSOLUTE(.);
    } > kflash

    .data : {
        _sdata = ABSOLUTE(.);
        *(.data .data.*)
        *(.gnu.linkonce.d.*)
        CONSTRUCTORS
        . = ALIGN(4);
        _edata = ABSOLUTE(.);
    } > ksram AT > kflash

    _eronly = LOADADDR(.data);

    /* RAM functions must precede bss: startup places the idle stack at
     * _ebss and copies this section separately when ARCH_RAMFUNCS is set.
     */
    .ramfunc : {
        _sramfuncs = ABSOLUTE(.);
        *(.ramfunc .ramfunc.*)
        . = ALIGN(4);
        _eramfuncs = ABSOLUTE(.);
    } > ksram AT > kflash

    _framfuncs = LOADADDR(.ramfunc);

    /* STM32H7 flash programming uses complete 256-bit (32-byte) words. */
    .flashpad : {
        FILL(0xffffffff)
        BYTE(0xff)
        . = ALIGN(32);
    } > kflash

    .bss (NOLOAD) : {
        _sbss = ABSOLUTE(.);
        *(.bss .bss.*)
        *(.gnu.linkonce.b.*)
        *(COMMON)
        . = ALIGN(8);
        _ebss = ABSOLUTE(.);
    } > ksram

    /* The protected board configuration limits the idle stack to 1 KiB. */
    ASSERT(_ebss + 1024 <= ORIGIN(ksram) + LENGTH(ksram),
           "Kernel data and idle stack overlap user SRAM")

    .sram4_reserve (NOLOAD) : {
        *(.sram4 .sram4.*)
        . = ALIGN(4);
        _sram4_heap_start = ABSOLUTE(.);
    } > SRAM4

    .stab 0 : { *(.stab) }
    .stabstr 0 : { *(.stabstr) }
    .stab.excl 0 : { *(.stab.excl) }
    .stab.exclstr 0 : { *(.stab.exclstr) }
    .stab.index 0 : { *(.stab.index) }
    .stab.indexstr 0 : { *(.stab.indexstr) }
    .comment 0 : { *(.comment) }
    .debug_abbrev 0 : { *(.debug_abbrev) }
    .debug_info 0 : { *(.debug_info) }
    .debug_line 0 : { *(.debug_line) }
    .debug_pubnames 0 : { *(.debug_pubnames) }
    .debug_aranges 0 : { *(.debug_aranges) }
}

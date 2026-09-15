#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause

from pathlib import Path
import base64, hashlib, json, struct, subprocess, sys, zlib
import re

build = Path(sys.argv[1]).resolve()
cache = (build / 'CMakeCache.txt').read_text()
name = re.search(r'^PX4_CONFIG:STRING=(.+)$', cache, re.MULTILINE).group(1)
assert name in ('px4_fmu-v6x_protected', 'px4_fmu-v6x_protected-mavlink'), name

def inspect_elf(path):
    data = path.read_bytes()
    assert data[:6] == b'\x7fELF\x01\x01', path
    header = struct.unpack_from('<16sHHIIIIIHHHHHH', data)
    assert header[2] == 40, 'ELF must target ARM'
    _, _, _, _, entry, phoff, _, _, _, phentsize, phnum, *_ = header
    loads = []
    for index in range(phnum):
        ptype, offset, vaddr, paddr, filesz, memsz, flags, align = struct.unpack_from('<IIIIIIII', data, phoff + index * phentsize)
        if ptype == 1:
            loads.append(dict(offset=offset, vaddr=vaddr, paddr=paddr, filesz=filesz, memsz=memsz))
    output = subprocess.check_output(['arm-none-eabi-nm', '--defined-only', str(path)], text=True)
    symbols = {}
    for line in output.splitlines():
        fields = line.split()
        if len(fields) == 3:
            symbols[fields[2]] = int(fields[0], 16)
    return data, symbols, loads

kernel, ks, kloads = inspect_elf(build / (name + '_kernel.elf'))
user, us, uloads = inspect_elf(build / (name + '.elf'))
for loads, start, end in [(kloads, 0x08020000, 0x08100000), (uloads, 0x08100000, 0x08200000)]:
    for segment in loads:
        if segment['filesz']:
            assert start <= segment['paddr'] < segment['paddr'] + segment['filesz'] <= end, segment
assert ks['_stext'] == ks['_vectors'] == 0x08020000
assert us['userspace'] == us['_stext'] == 0x08100000
assert 0x24000000 <= ks['_sdata'] <= ks['_edata'] <= ks['_sbss'] <= ks['_ebss']
assert ks['_ebss'] + 1024 <= 0x24020000
assert 0x24020000 <= us['_sdata'] <= us['_edata'] <= us['_sbss'] <= us['_ebss'] <= 0x24040000
assert us['_ebss'] % 0x4000 == 0
assert us['_uheap_end'] == 0x24080000

binary = (build / (name + '.bin')).read_bytes()
assert len(binary) <= 0x1e0000 and len(binary) % 32 == 0
for elf, loads in [(kernel, kloads), (user, uloads)]:
    for segment in loads:
        if segment['filesz']:
            offset = segment['paddr'] - 0x08020000
            assert binary[offset:offset + segment['filesz']] == elf[segment['offset']:segment['offset'] + segment['filesz']], segment
kflash_end = max(s['paddr'] + s['filesz'] for s in kloads if s['filesz'])
assert binary[kflash_end - 0x08020000:0xe0000] == b'\xff' * (0x08100000 - kflash_end)
vector_sp, vector_reset = struct.unpack_from('<II', binary)
assert 0x24000000 <= vector_sp < 0x24020000
assert vector_reset & 1 and 0x08020000 <= (vector_reset & ~1) < 0x08100000
fields = ['px4_entry', '_stext', '_etext', '_eronly', '_sdata', '_edata', '_sbss', '_ebss', '_uheap_end']
values = struct.unpack_from('<9I', binary, 0xe0000)
for field, value in zip(fields, values):
    if field == 'px4_entry':
        assert value & 1 and (value & ~1) == (us[field] & ~1)
    else:
        assert value == us[field], (field, hex(value), hex(us[field]))

fw = json.loads((build / (name + '.px4')).read_text())
packaged = zlib.decompress(base64.b64decode(fw['image']))
assert fw['image_size'] == len(binary) and packaged == binary
config = (build / 'NuttX/nuttx/.config').read_text().splitlines()
for flag in ['CONFIG_BUILD_PROTECTED=y', 'CONFIG_BUILD_2PASS=y', 'CONFIG_ARM_MPU=y', 'CONFIG_MM_KERNEL_HEAP=y',
             'CONFIG_NSH_USBCONSOLE=y', 'CONFIG_NSH_USBCONDEV="/dev/ttyACM0"',
             'CONFIG_BOARDCTL_USBDEVCTRL=y', 'CONFIG_USART3_SERIAL_CONSOLE=y']:
    assert flag in config, flag
assert 'CONFIG_BUILD_FLAT=y' not in config
assert 'CONFIG_SMP=y' not in config, 'User HRT dispatch requires a single-core scheduler'
assert 'CONFIG_CDCACM_CONSOLE=y' not in config
assert (build / 'NuttX/kernel_builtin/kernel_builtin_list.h').read_bytes() == (build / 'NuttX/px4_kernel.bdat').read_bytes()
assert (build / 'NuttX/kernel_builtin/kernel_builtin_proto.h').read_bytes() == (build / 'NuttX/px4_kernel.pdat').read_bytes()
assert 'reboot_main' in ks and 'reboot_main' not in us
assert 'stm32_configgpio' in ks and 'stm32_configgpio' not in us
assert 'nsh_consolemain' in us and 'nsh_consolemain' not in ks
assert 'cdcacm_initialize' in ks and 'cdcacm_initialize' not in us
if name == 'px4_fmu-v6x_protected-mavlink':
    assert 'mavlink_main' in us and 'mavlink_main' not in ks, 'MAVLink must run in userspace'
    assert 'mavlink_main' in (build / 'NuttX/px4.bdat').read_text()
    assert 'mavlink_main' not in (build / 'NuttX/px4_kernel.bdat').read_text()
    board_config = (build / 'boardconfig').read_text().splitlines()
    assert 'CONFIG_USER_MAVLINK=y' in board_config
    assert '# CONFIG_MAVLINK_MISSION is not set' in board_config
    assert not any('MavlinkMissionManager' in symbol for symbol in us), 'Mission support is disabled'
else:
    assert 'mavlink_main' not in us and 'mavlink_main' not in ks
assert 'nsh_usbconsole.c' in (build / (name + '.map')).read_text()
assert 'hrt_smoke_main' in us and 'hrt_smoke_main' not in ks, 'HRT diagnostic must run in userspace'
assert 'hrt_smoke_main' in (build / 'NuttX/px4.bdat').read_text()
assert 'hrt_smoke_main' not in (build / 'NuttX/px4_kernel.bdat').read_text()
assert 'work_queue_smoke_main' in us and 'work_queue_smoke_main' not in ks, 'Work queue diagnostic must run in userspace'
assert 'work_queue_smoke_main' in (build / 'NuttX/px4.bdat').read_text()
assert 'work_queue_smoke_main' not in (build / 'NuttX/px4_kernel.bdat').read_text()
assert 'uorb_smoke_main' in us and 'uorb_smoke_main' not in ks, 'uORB diagnostic must run in userspace'
assert 'uorb_user_callback_dispatch' in us and 'uorb_user_callback_dispatch' not in ks, 'uORB dispatcher must run in userspace'
assert 'uorb_smoke_kernel_main' in ks and 'uorb_smoke_kernel_main' not in us, 'uORB companion must run in the kernel'
assert 'uorb_smoke_main' in (build / 'NuttX/px4.bdat').read_text()
assert 'uorb_smoke_main' not in (build / 'NuttX/px4_kernel.bdat').read_text()
assert 'uorb_smoke_kernel_main' in (build / 'NuttX/px4_kernel.bdat').read_text()
assert 'uorb_smoke_kernel_main' not in (build / 'NuttX/px4.bdat').read_text()
assert 'uorb_wait_smoke_main' in us and 'uorb_wait_smoke_main' not in ks, 'uORB wait diagnostic must run in userspace'
assert 'uorb_wait_kernel_main' in ks and 'uorb_wait_kernel_main' not in us, 'uORB wait observer must run in the kernel'
assert 'uorb_wait_smoke_main' in (build / 'NuttX/px4.bdat').read_text()
assert 'uorb_wait_smoke_main' not in (build / 'NuttX/px4_kernel.bdat').read_text()
assert 'uorb_wait_kernel_main' in (build / 'NuttX/px4_kernel.bdat').read_text()
assert 'uorb_wait_kernel_main' not in (build / 'NuttX/px4.bdat').read_text()
for symbol in ['uorb_wait_pollfd', 'uorb_wait_blocking_storage']:
    assert symbol in us and symbol not in ks, 'Wait diagnostic storage must belong to userspace'
    assert us['_sbss'] <= us[symbol] < us['_ebss'], 'Wait identity storage must reside in user BSS'

# The kernel must use the protected allocator wrappers from libkmm. Linking
# userspace libmm can make memalign use an uninitialized kernel g_mmheap copy.
kernel_map = (build / (name + '_kernel.map')).read_text()
user_map = (build / (name + '.map')).read_text()
# The user queue uses scheduler exclusion; the kernel queue retains IRQ
# exclusion. They must come from separately compiled archives.
assert 'libpx4_work_queue.a' not in kernel_map, 'Userspace work queue archive linked into kernel'
assert 'libpx4_work_queue_kernel.a(WorkQueue.cpp.obj)' in kernel_map, 'Kernel work queue archive missing'
assert 'libpx4_work_queue_kernel.a' not in user_map, 'Kernel work queue archive linked into userspace'
assert 'libpx4_work_queue.a(WorkQueue.cpp.obj)' in user_map, 'Userspace work queue archive missing'
assert 'libuORB_kernel.a(uORBUserCallback.cpp.obj)' in kernel_map, 'Kernel uORB notification broker missing'
assert 'uORBUserCallback.cpp.obj' not in user_map, 'Kernel uORB broker linked into userspace'

# Check the actual lock implementation in the two linked images, not just the
# archive names: protected user code must not attempt privileged IRQ masking.
queue_add = '_ZN3px49WorkQueue3AddEPNS_8WorkItemE'
def disassemble(path, symbol):
    return subprocess.check_output(['arm-none-eabi-objdump', '-d', '--disassemble=' + symbol, str(path)], text=True)

user_add = disassemble(build / (name + '.elf'), queue_add)
kernel_add = disassemble(build / (name + '_kernel.elf'), queue_add)
assert '<sched_lock>' in user_add and '<sched_unlock>' in user_add, 'User work queue scheduler locking missing'
irq_mask = r'\b(?:msr\s+(?:BASEPRI|PRIMASK)|cpsid)\b'
assert not re.search(irq_mask, user_add, re.IGNORECASE), 'User work queue attempts privileged IRQ masking'
assert re.search(irq_mask, kernel_add, re.IGNORECASE), 'Kernel work queue IRQ locking missing'
assert '<sched_lock>' not in kernel_add, 'Kernel work queue uses user scheduler locking'
assert 'libmm.a' not in kernel_map, 'Userspace allocator archive linked into kernel'
assert 'libkmm.a(umm_memalign.o)' in kernel_map, 'Kernel memalign wrapper missing'
assert 'g_mmheap' not in ks, 'Userspace heap pointer duplicated in kernel'
assert us['_sbss'] <= us['g_mmheap'] < us['_ebss'], 'User heap pointer outside user BSS'

report = {
    'checks': 'PASS: ARM ELF, load ranges, static RAM bounds, userspace header, reset vectors, binary padding, PX4 payload, protected configuration, builtin tables, reboot, HRT, work queue, uORB and waiting-subscriber diagnostic placement, user BSS wait identity storage, uORB user dispatcher and kernel broker placement, separate kernel/user work queue archives and lock instructions, USB NSH configuration, kernel/user placement and allocator linkage',
    'target': name,
    'mavlink_userspace_placement_checked': name == 'px4_fmu-v6x_protected-mavlink',
    'board_id': fw['board_id'],
    'kernel_flash_bytes': kflash_end - 0x08020000,
    'user_flash_bytes': max(s['paddr'] + s['filesz'] for s in uloads if s['filesz']) - 0x08100000,
    'combined_binary_bytes': len(binary),
    'px4_package_bytes': (build / (name + '.px4')).stat().st_size,
    'kernel_static_bytes': ks['_ebss'] - 0x24000000,
    'user_static_reserved_bytes': us['_ebss'] - 0x24020000,
    'binary_sha256': hashlib.sha256(binary).hexdigest(),
    'hardware_boot_verified': False,
    'usb_console_hardware_verified': False,
    'hrt_smoke_hardware_verified': False,
    'work_queue_smoke_hardware_verified': False,
    'uorb_smoke_hardware_verified': False,
    'uorb_wait_smoke_hardware_verified': False,
    'mavlink_hardware_verified': False,
}
print(json.dumps(report, indent=2))
(build / 'protected-artifact-check.json').write_text(json.dumps(report, indent=2) + '\n')

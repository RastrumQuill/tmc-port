#!/usr/bin/env python3
"""Find absolute ROM pointers hidden inside .incbin'd blobs.

Some data in the decomp is still included as raw binary extracted from the
ROM. Such blobs can contain absolute GBA addresses (0x08xxxxxx) of other data
or functions, which are meaningless in the PC build. This tool uses a
matching GBA build (tmc.elf + map) to find these words and writes a
relocation list for port/tools/asfilter.py, which then emits them as
symbolic '.4byte' expressions.

usage: find_rom_pointers.py BUILD_DIR(e.g. build/USA) ROM ELF OUT [--assets DIR]
"""
import os
import re
import subprocess
import sys
import bisect

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROM_BASE = 0x08000000


def run(cmd, **kw):
    return subprocess.run(cmd, check=True, stdout=subprocess.PIPE, **kw).stdout


def parse_map(path):
    """object file -> {section: address}"""
    objs = {}
    sec_re = re.compile(r'^\s+\.(rodata|text)\s+(0x[0-9a-f]+)\s+(0x[0-9a-f]+)\s+(\S+\.o)$')
    for line in open(path):
        m = sec_re.match(line.rstrip())
        if m and int(m.group(3), 16) > 0:
            objs.setdefault(m.group(4), {})['.' + m.group(1)] = int(m.group(2), 16)
    return objs


def blob_label(path):
    return '__ib_' + re.sub(r'[^A-Za-z0-9_]', '_', path)


def locate_blobs(build_dir, objs, assets, version_defs):
    """Assemble every asm object again with marked incbins: path -> (rom address, size)."""
    blobs = {}
    tmp = os.path.join(build_dir, 'ptrscan')
    os.makedirs(tmp, exist_ok=True)
    for obj, secs in sorted(objs.items()):
        src = obj[:-2] + '.s'
        if not (obj.startswith('data/') or obj.startswith('asm/')) or not os.path.isfile(src):
            continue
        pre = run(['tools/bin/preproc', 'tmc', src, '--', '-I', assets, '-I', os.path.join(build_dir, 'enum_include')])
        filt = run([sys.executable, os.path.join(TOOLS, 'asfilter.py'), '-I', assets,
                    '-I', os.path.join(build_dir, 'enum_include'), '--mark', '--keep-directives'], input=pre)
        out = os.path.join(tmp, re.sub(r'[^A-Za-z0-9_]', '_', obj))
        subprocess.run(['arm-none-eabi-as', '-mcpu=arm7tdmi'] + version_defs +
                       ['-I', '.', '-I', assets, '-I', os.path.join(build_dir, 'enum_include'), '-o', out],
                       input=filt, check=True)
        for line in run(['arm-none-eabi-objdump', '-t', out]).decode().splitlines():
            parts = line.split()
            if len(parts) >= 5 and parts[-1].startswith('__ib_'):
                sec = parts[-3]
                if sec not in secs:
                    continue
                addr = secs[sec] + int(parts[0], 16)
                label = parts[-1]
                blobs[label] = addr
    return blobs


def main():
    build_dir, rom_path, elf, out_path = sys.argv[1:5]
    assets = os.path.join(build_dir, 'assets')
    version_defs = ['--defsym', 'USA=1', '--defsym', 'REVISION=0', '--defsym', 'ENGLISH=1']
    rom = open(rom_path, 'rb').read()
    objs = parse_map(os.path.join(build_dir, 'tmc.map'))

    # incbin paths -> label
    paths = {}
    for root, _, files in os.walk('data'):
        pass
    label_addr = locate_blobs(build_dir, objs, assets, version_defs)

    # collect blob extents: label -> (addr, size, path)
    blob_list = []
    path_for_label = {}
    for src_root in ('data', 'asm'):
        for root, _, files in os.walk(src_root):
            for f in files:
                if not (f.endswith('.s') or f.endswith('.inc')):
                    continue
                for m in re.finditer(r'\.incbin\s+"([^"]+)"', open(os.path.join(root, f), errors='replace').read()):
                    path_for_label[blob_label(m.group(1))] = m.group(1)
    for label, addr in label_addr.items():
        path = path_for_label.get(label)
        if path is None:
            continue
        full = os.path.join(assets, path)
        if not os.path.isfile(full):
            full = path
        if not os.path.isfile(full):
            continue
        blob_list.append((addr, os.path.getsize(full), path, label))
    blob_list.sort()
    blob_starts = [b[0] for b in blob_list]

    # symbols of the GBA build (globals preferred; thumb code is at even addresses)
    syms = {}
    funcs = {}
    for line in run(['arm-none-eabi-nm', elf]).decode().splitlines():
        parts = line.split()
        if len(parts) != 3:
            continue
        addr = int(parts[0], 16)
        kind, name = parts[1], parts[2]
        if not (ROM_BASE <= addr < ROM_BASE + len(rom)) or name.startswith('$') or name.startswith('.'):
            continue
        is_global = kind.isupper()
        table = funcs if kind in 'Tt' else syms
        if addr not in table or (is_global and not table[addr][1]):
            table[addr] = (name, is_global)

    def blob_for(addr):
        i = bisect.bisect_right(blob_starts, addr) - 1
        if i >= 0:
            start, size, path, label = blob_list[i]
            if start <= addr < start + size:
                return blob_list[i]
        return None

    def resolve(v):
        if v & 1 and (v - 1) in funcs:
            name, is_global = funcs[v - 1]
            return name, 'func' if is_global else 'static-func'
        if v in syms:
            name, is_global = syms[v]
            return name, 'sym' if is_global else 'static-sym'
        b = blob_for(v)
        # only the start of a blob: pointers into the middle of graphics or
        # sample data are indistinguishable from random bytes
        if b is not None and v == b[0]:
            return b[3], 'blob'
        return None, None

    out = []
    stats = {}
    for start, size, path, label in blob_list:
        found = []
        first = (start + 3) & ~3
        for addr in range(first, start + size - 3, 4):
            v = int.from_bytes(rom[addr - ROM_BASE:addr - ROM_BASE + 4], 'little')
            if not (ROM_BASE <= v < ROM_BASE + len(rom)):
                continue
            expr, kind = resolve(v)
            if expr is None:
                continue
            found.append((addr - start, expr, kind))
        if found:
            stats[path] = found
    with open(out_path, 'w') as f:
        f.write('# generated by port/tools/find_rom_pointers.py: incbin path, offset, symbolic value\n')
        for path in sorted(stats):
            for off, expr, kind in stats[path]:
                f.write('%s 0x%x %s  # %s\n' % (path, off, expr, kind))
    total = sum(len(v) for v in stats.values())
    print('%d blobs located, %d contain %d pointers' % (len(blob_list), len(stats), total))


if __name__ == '__main__':
    main()

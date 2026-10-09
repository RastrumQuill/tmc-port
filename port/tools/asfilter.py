#!/usr/bin/env python3
"""Translate the decomp's ARM-flavoured GNU assembler *data* files into
something the host (x86) GNU assembler accepts.

Only data directives are expected (.byte/.2byte/.4byte/.incbin/macros...).
The translation is purely syntactic:
  * '@' and '//' comments are stripped (outside of string literals)
  * '.align N' (power of two on ARM) becomes '.p2align N'
  * '.word' (32 bit on ARM) becomes '.4byte'
  * 'label::' (global label shorthand) becomes '.global label' + 'label:'
  * ARM/Thumb mode directives are dropped
  * '.include' directives are expanded inline (recursively) so that the
    included macro files get the same treatment
  * with --mark TAG every '.incbin' gets a label __ib_TAG_N in front of it
    (used to locate blobs in the GBA build, see find_rom_pointers.py)
  * with --relocs FILE, blobs that contain absolute ROM pointers are split
    and the pointers are emitted as symbolic '.4byte' expressions
Usage: asfilter.py [-I dir]... [--mark] [--relocs FILE] < in.s > out.s
"""
import os
import re
import sys

DROP = re.compile(r'^\s*\.(syntax|thumb|arm|code|thumb_func|force_thumb|pool|ltorg|type|size|cpu|fpu)\b')
ALIGN = re.compile(r'^(\s*)\.align\b')
# '.word' is 32 bit on ARM but 16 bit on x86
WORD = re.compile(r'(^|[\s:])\.word\b')
GLABEL = re.compile(r'^(\s*)([A-Za-z_.$][\w.$]*)::')


def strip_comment(line):
    out = []
    in_str = False
    i = 0
    while i < len(line):
        c = line[i]
        if in_str:
            out.append(c)
            if c == '\\' and i + 1 < len(line):
                out.append(line[i + 1])
                i += 2
                continue
            if c == '"':
                in_str = False
        else:
            if c == '"':
                in_str = True
            elif c == '@':
                break
            elif c == '/' and line[i + 1:i + 2] == '/':
                break
            elif c == ';':
                # ARM gas uses ';' as statement separator too, keep it
                pass
            out.append(c)
        i += 1
    return ''.join(out).rstrip()


OPS = re.compile(r'\s*(<<|>>|[*/+|&^])\s*')
# Thumb function pointers carry the +1 interworking bit; native code must not.
THUMB_CALL = re.compile(r'^(\s*Call(?:WithArg)?\s+\w+)\+1\b')
MINUS = re.compile(r'\s+-\s+')
INCBIN = re.compile(r'^(\s*)\.incbin\s+"([^"]+)"\s*$')
INCLUDE = re.compile(r'^\s*\.include\s+"([^"]+)"')


def find_include(name, dirs):
    for d in dirs:
        path = os.path.join(d, name)
        if os.path.isfile(path):
            return path
    sys.exit('asfilter: cannot find include "%s"' % name)


class State:
    keep_directives = False
    mark = False
    relocs = {}


def blob_label(path):
    return '__ib_' + re.sub(r'[^A-Za-z0-9_]', '_', path)


def emit_incbin(indent, path, w):
    if State.mark:
        label = blob_label(path)
        w('%s.global %s\n%s:\n' % (indent, label, label))
    relocs = State.relocs.get(path)
    if not relocs:
        w('%s.incbin "%s"\n' % (indent, path))
        return
    pos = 0
    for off, expr in relocs:
        if off > pos:
            w('%s.incbin "%s", %d, %d\n' % (indent, path, pos, off - pos))
        w('%s.4byte %s\n' % (indent, expr))
        pos = off + 4
    w('%s.incbin "%s", %d\n' % (indent, path, pos))


def process(data, dirs, w, depth=0):
    if depth > 32:
        sys.exit('asfilter: includes nested too deeply')
    # strip /* */ block comments
    data = re.sub(r'/\*.*?\*/', lambda m: '\n' * m.group(0).count('\n'), data, flags=re.S)
    for line in data.split('\n'):
        inc = INCLUDE.match(line)
        if inc:
            path = find_include(inc.group(1), dirs)
            with open(path) as f:
                process(f.read(), dirs, w, depth + 1)
            continue
        line = strip_comment(line)
        ib = INCBIN.match(line)
        if ib:
            emit_incbin(ib.group(1), ib.group(2), w)
            continue
        if not State.keep_directives and DROP.match(line):
            w('\n')
            continue
        line = ALIGN.sub(r'\1.p2align', line)
        line = WORD.sub(r'\1.4byte', line)
        line = THUMB_CALL.sub(r'\1', line)
        # x86 gas splits macro arguments on whitespace, ARM gas does not:
        # glue binary operators to their operands ("A * 2" -> "A*2").
        if '"' not in line and not line.lstrip().startswith('.'):
            line = MINUS.sub('-', OPS.sub(r'\1', line))
        m = GLABEL.match(line)
        if m:
            w('%s.global %s\n' % (m.group(1), m.group(2)))
            line = m.group(1) + m.group(2) + ':' + line[m.end():]
        w(line + '\n')


def load_relocs(path):
    """lines: INCBIN_PATH OFFSET EXPR"""
    if not os.path.isfile(path):
        return
    for line in open(path):
        line = line.split('#', 1)[0].strip()
        if not line:
            continue
        label, off, expr = line.split(None, 2)
        State.relocs.setdefault(label, []).append((int(off, 0), expr))
    for v in State.relocs.values():
        v.sort()


def main():
    dirs = ['.']
    args = sys.argv[1:]
    while args:
        a = args.pop(0)
        if a == '-I':
            dirs.append(args.pop(0))
        elif a.startswith('-I'):
            dirs.append(a[2:])
        elif a == '--mark':
            State.mark = True
        elif a == '--keep-directives':
            State.keep_directives = True
        elif a == '--relocs':
            load_relocs(args.pop(0))
    out = []
    process(sys.stdin.read(), dirs, out.append)
    sys.stdout.write(''.join(out))


if __name__ == '__main__':
    main()

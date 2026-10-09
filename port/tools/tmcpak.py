#!/usr/bin/env python3
"""
Inspects and edits tmc_data.pak, the game data resource pack the PC port
creates from the ROM on its first start (format: port/src/resources.c).

  tmcpak.py list    PACK                      list the entries
  tmcpak.py extract PACK OUT_DIR [NAME...]    write entries as files (all, or the named ones)
  tmcpak.py replace PACK NAME FILE            replace an entry (the new data must have the same size)

Entry names are "<data file>:<section>", e.g. "data/sound/sounds:.rodata",
after the decompilation's data files. Replacing an entry with data of a
different size is not supported yet: the data contains pointers whose
positions are fixed in the executable.
"""
import os
import struct
import sys
import zlib

MAGIC = b"TMCPAK\x1a\x00"
VERSION = 1
MIN_MATCH = 4
TAIL = 8
HEADER = struct.Struct("<8sIIIII")


def lz_decompress(src, size):
    out = bytearray()
    i, n = 0, len(src)

    def length(i, value):
        while True:
            b = src[i]
            i += 1
            value += b
            if b != 255:
                return i, value

    while i < n:
        token = src[i]
        i += 1
        lit, match = token >> 4, token & 15
        if lit == 15:
            i, lit = length(i, lit)
        out += src[i:i + lit]
        i += lit
        if i >= n:
            break
        offset = src[i] | src[i + 1] << 8
        i += 2
        if match == 15:
            i, match = length(i, match)
        match += MIN_MATCH
        start = len(out) - offset
        if offset == 0 or start < 0:
            raise ValueError("bad match offset")
        for k in range(match):
            out.append(out[start + k])
    if len(out) != size:
        raise ValueError("bad size")
    return bytes(out)


def lz_compress(src):
    out = bytearray()
    n = len(src)
    table = {}
    ip = anchor = 0

    def put_length(value):
        while value >= 255:
            out.append(255)
            value -= 255
        out.append(value)

    def sequence(lit_start, lit_len, offset, match_len):
        m = match_len - MIN_MATCH if match_len else 0
        out.append((min(lit_len, 15) << 4) | min(m, 15))
        if lit_len >= 15:
            put_length(lit_len - 15)
        out.extend(src[lit_start:lit_start + lit_len])
        if match_len:
            out.extend((offset & 0xFF, offset >> 8))
            if m >= 15:
                put_length(m - 15)

    while n > TAIL + MIN_MATCH and ip + TAIL + MIN_MATCH <= n:
        key = src[ip:ip + 4]
        ref = table.get(key)
        table[key] = ip
        if ref is not None and ip - ref <= 0xFFFF:
            length = MIN_MATCH
            while ip + length < n - TAIL and src[ref + length] == src[ip + length]:
                length += 1
            sequence(anchor, ip - anchor, ip - ref, length)
            ip += length
            anchor = ip
        else:
            ip += 1
    sequence(anchor, n - anchor, 0, 0)
    return bytes(out)


def read_pack(path):
    data = open(path, "rb").read()
    magic, version, layout_hash, count, dir_pos, dir_size = HEADER.unpack_from(data)
    if magic != MAGIC or version != VERSION:
        raise SystemExit("%s: not a resource pack of this port" % path)
    entries = []
    pos = dir_pos
    for _ in range(count):
        name_len, = struct.unpack_from("<I", data, pos)
        name = data[pos + 4:pos + 4 + name_len].decode()
        offset, packed, size, crc = struct.unpack_from("<IIII", data, pos + 4 + name_len)
        entries.append(dict(name=name, offset=offset, packed=packed, size=size, crc=crc))
        pos += 20 + name_len
    return data, layout_hash, entries


def entry_data(data, e):
    raw = lz_decompress(data[e["offset"]:e["offset"] + e["packed"]], e["size"])
    if zlib.crc32(raw) != e["crc"]:
        raise SystemExit("%s: CRC mismatch" % e["name"])
    return raw


def write_pack(path, layout_hash, entries):
    """entries: dicts with name, size, crc and packed_data (compressed bytes)"""
    body = bytearray()
    directory = bytearray()
    pos = HEADER.size
    for e in entries:
        packed = e["packed_data"]
        name = e["name"].encode()
        directory += struct.pack("<I", len(name)) + name
        directory += struct.pack("<IIII", pos, len(packed), e["size"], e["crc"])
        body += packed
        pos += len(packed)
    header = HEADER.pack(MAGIC, VERSION, layout_hash, len(entries), pos, len(directory))
    tmp = path + ".tmp"
    with open(tmp, "wb") as f:
        f.write(header + body + directory)
    os.replace(tmp, path)


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    cmd, pack = sys.argv[1], sys.argv[2]
    data, layout_hash, entries = read_pack(pack)
    if cmd == "list":
        print("%-60s %10s %10s" % ("entry", "size", "packed"))
        for e in entries:
            print("%-60s %10d %10d" % (e["name"], e["size"], e["packed"]))
        print("%d entries, %d bytes, %d packed" % (len(entries), sum(e["size"] for e in entries),
                                                   sum(e["packed"] for e in entries)))
    elif cmd == "extract":
        out_dir = sys.argv[3]
        wanted = set(sys.argv[4:])
        for e in entries:
            if wanted and e["name"] not in wanted:
                continue
            path = os.path.join(out_dir, e["name"].replace(":", "/") + ".bin")
            os.makedirs(os.path.dirname(path), exist_ok=True)
            open(path, "wb").write(entry_data(data, e))
        print("extracted to %s" % out_dir)
    elif cmd == "replace":
        name, file = sys.argv[3], sys.argv[4]
        new = open(file, "rb").read()
        found = False
        for e in entries:
            if e["name"] == name:
                if len(new) != e["size"]:
                    raise SystemExit("%s is %d bytes, the new data is %d bytes; sizes must match"
                                     % (name, e["size"], len(new)))
                e["packed_data"] = lz_compress(new)
                e["crc"] = zlib.crc32(new)
                found = True
            else:
                e["packed_data"] = data[e["offset"]:e["offset"] + e["packed"]]
        if not found:
            raise SystemExit("no entry %s" % name)
        write_pack(pack, layout_hash, entries)
        print("replaced %s" % name)
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""
Creates the asset layout of the PC port from fully assembled data objects.

The game's data (graphics, maps, music, text, tables) is assembled from the
decompilation's data/*.s files and the assets extracted from the ROM. The
port does not ship any of it: for every data object this script records only
its layout - section sizes, symbols, pointer relocations - plus where each
run of bytes between two pointers lives in the ROM. The layout contains no
game content. At build time gen_asset_skeleton.py turns it back into
assembly (zero filled sections with the same symbols and pointers), and at
startup the game copies the runs from the player's ROM into place.

usage: make_asset_layout.py ROM OUT.json.gz BUILD_DIR OBJ...   (ELF32 i386 objects)
"""
import gzip
import json
import struct
import sys

SHT_SYMTAB, SHT_NOBITS, SHT_REL = 2, 8, 9
SHF_WRITE, SHF_ALLOC, SHF_EXECINSTR = 1, 2, 4
SHN_UNDEF, SHN_ABS, SHN_COMMON = 0, 0xFFF1, 0xFFF2
STB_LOCAL, STT_SECTION = 0, 3
R_386_32 = 1


class Elf:
    def __init__(self, path):
        d = open(path, "rb").read()
        if d[:4] != b"\x7fELF" or d[4] != 1:
            raise ValueError("%s: not an ELF32 object" % path)
        shoff, = struct.unpack_from("<I", d, 0x20)
        shentsize, shnum, shstrndx = struct.unpack_from("<HHH", d, 0x2E)
        self.sections = []
        for i in range(shnum):
            f = struct.unpack_from("<IIIIIIIIII", d, shoff + i * shentsize)
            self.sections.append(dict(name_off=f[0], type=f[1], flags=f[2], offset=f[4], size=f[5],
                                      link=f[6], info=f[7], align=f[8], entsize=f[9]))
        strtab = self.sections[shstrndx]
        for s in self.sections:
            s["name"] = self._str(d, strtab["offset"], s["name_off"])
            s["data"] = b"" if s["type"] == SHT_NOBITS else d[s["offset"]:s["offset"] + s["size"]]
        self.symbols = []
        for s in self.sections:
            if s["type"] == SHT_SYMTAB:
                names = self.sections[s["link"]]
                for i in range(s["size"] // 16):
                    name, value, size, info, other, shndx = struct.unpack_from("<IIIBBH", d, s["offset"] + i * 16)
                    self.symbols.append(dict(name=self._str(d, names["offset"], name), value=value,
                                             bind=info >> 4, type=info & 15, shndx=shndx))
        self.relocs = {}  # section index -> [(offset, symbol index, type)]
        for s in self.sections:
            if s["type"] == SHT_REL:
                rel = []
                for i in range(s["size"] // 8):
                    off, info = struct.unpack_from("<II", d, s["offset"] + i * 8)
                    rel.append((off, info >> 8, info & 0xFF))
                self.relocs[s["info"]] = rel

    @staticmethod
    def _str(d, base, off):
        end = d.index(b"\0", base + off)
        return d[base + off:end].decode()


def main():
    rom_path, out_path, build_dir = sys.argv[1:4]
    objs = sys.argv[4:]
    rom = open(rom_path, "rb").read()
    layout = {"rom_size": len(rom), "objects": []}
    stats = dict(objects=0, runs=0, rom_bytes=0, literal_bytes=0, zero_bytes=0)
    literals = []

    for obj in objs:
        elf = Elf(build_dir + "/" + obj)
        out_obj = {"name": obj, "sections": [], "abs": []}
        # local symbols referenced by relocations need labels in the skeleton
        for sym in elf.symbols:
            if sym["bind"] != STB_LOCAL and sym["shndx"] == SHN_ABS and sym["name"]:
                out_obj["abs"].append([sym["name"], sym["value"]])
            if sym["shndx"] == SHN_COMMON:
                raise ValueError("%s: common symbol %s" % (obj, sym["name"]))
        for idx, sec in enumerate(elf.sections):
            if not (sec["flags"] & SHF_ALLOC) or sec["type"] == SHT_REL:
                continue
            if sec["size"] == 0 and not any(s["shndx"] == idx for s in elf.symbols if s["name"]):
                continue
            flags = "a" + ("w" if sec["flags"] & SHF_WRITE else "") + ("x" if sec["flags"] & SHF_EXECINSTR else "")
            symbols = [[s["name"], s["value"], 0 if s["bind"] == STB_LOCAL else 1]
                       for s in elf.symbols
                       if s["shndx"] == idx and s["name"] and s["type"] != STT_SECTION]
            relocs = []
            for off, symidx, rtype in elf.relocs.get(idx, []):
                if rtype != R_386_32:
                    raise ValueError("%s: unsupported relocation type %d" % (obj, rtype))
                sym = elf.symbols[symidx]
                addend, = struct.unpack_from("<i", sec["data"], off)
                if sym["type"] == STT_SECTION:
                    target = ["section", sym["shndx"]]
                else:
                    target = ["symbol", sym["name"]]
                    if sym["bind"] == STB_LOCAL and sym["shndx"] not in (SHN_UNDEF, SHN_ABS):
                        # make sure the local label exists in its section of the skeleton
                        pass
                relocs.append([off, target, addend])
            relocs.sort()
            out_sec = {"index": idx, "name": sec["name"], "flags": flags, "nobits": sec["type"] == SHT_NOBITS,
                       "align": max(sec["align"], 1), "size": sec["size"], "symbols": symbols,
                       "relocs": relocs, "runs": [], "literal": []}
            if sec["type"] != SHT_NOBITS and sec["size"]:
                data = sec["data"]
                runs, pos = [], 0
                for off, _, _ in relocs:
                    if off > pos:
                        runs.append((pos, off - pos))
                    pos = off + 4
                if pos < len(data):
                    runs.append((pos, len(data) - pos))
                runs = [(o, n) for (o, n) in runs if any(data[o:o + n])]
                # one ROM base for the whole section, found from its longest run
                base = None
                if runs:
                    lo, ln = max(runs, key=lambda r: r[1])
                    needle = data[lo:lo + ln]
                    at = rom.find(needle)
                    while at >= 0:
                        cand = at - lo
                        if cand >= 0 and all(rom[cand + o:cand + o + n] == data[o:o + n] for o, n in runs):
                            base = cand
                            break
                        at = rom.find(needle, at + 1)
                for o, n in runs:
                    if base is not None:
                        out_sec["runs"].append([o, n, base + o])
                        stats["rom_bytes"] += n
                        continue
                    at = rom.find(data[o:o + n])
                    if at >= 0 and n >= 4:
                        out_sec["runs"].append([o, n, at])
                        stats["rom_bytes"] += n
                    else:
                        out_sec["literal"].append([o, data[o:o + n].hex()])
                        stats["literal_bytes"] += n
                        literals.append((obj, sec["name"], o, n))
                stats["runs"] += len(out_sec["runs"])
                stats["zero_bytes"] += len(data) - sum(n for _, n in runs) - 4 * len(relocs)
            out_obj["sections"].append(out_sec)
        # labels for local symbols used by relocations in other sections of this object are already
        # listed per section; references to sections become "section" targets
        layout["objects"].append(out_obj)
        stats["objects"] += 1

    with gzip.open(out_path, "wt") as f:
        json.dump(layout, f, separators=(",", ":"))
    print("layout: %(objects)d objects, %(runs)d ROM runs, %(rom_bytes)d bytes from the ROM, "
          "%(zero_bytes)d zero bytes, %(literal_bytes)d literal bytes" % stats)
    for l in literals[:20]:
        print("  literal (not found in the ROM): %s %s +0x%x, %d bytes" % l)


if __name__ == "__main__":
    main()

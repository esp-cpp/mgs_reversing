"""Pull the game's files out of a Metal Gear Solid disc image.

The disc is a raw 2352-byte-per-sector .bin (Mode 2 Form 1), so the 2048 bytes
of user data start 24 bytes into each sector. Reads the ISO9660 directory and
writes MGS/* out to a directory the port can point at.

STAGE.DIR is 71 MB and will not fit the board's 8 MB FAT partition, so it can
also be split per stage: its first sector is a table of (8-char name, start
sector) and each stage runs until the next one starts.

    python extract_disc.py <disc.bin> <outdir> [--stages s07a,brf,init]

With --stages, only those stage blocks are written (as MGS/STAGE/<name>.bin)
plus a rebuilt STAGE.DIR describing just them -- small enough to flash.

    python extract_disc.py <disc.bin> <outdir> --exe

writes only the disc's executable (SLUS_005.94 on the US disc) into outdir;
--text writes only MGS/TEXT.DAT, the item and weapon descriptions pulled
from that executable (the full extraction writes it as well).
"""
import os
import struct
import sys

SECTOR = 2352


def sniff_data_offset(fh):
    """Mode 1 keeps user data at +16, Mode 2 Form 1 at +24."""
    fh.seek(16 * SECTOR)
    raw = fh.read(SECTOR)
    at = raw.find(b"CD001")
    if at < 0:
        raise SystemExit("not an ISO9660 image")
    return at - 1


class Disc(object):
    def __init__(self, path):
        self.fh = open(path, "rb")
        self.off = sniff_data_offset(self.fh)

    def sector(self, n):
        self.fh.seek(n * SECTOR + self.off)
        return self.fh.read(2048)

    def read(self, lba, nbytes):
        out = bytearray()
        n = 0
        while len(out) < nbytes:
            out += self.sector(lba + n)
            n += 1
        return bytes(out[:nbytes])

    def listdir(self, lba, length):
        buf = b"".join(self.sector(lba + i)
                       for i in range((length + 2047) // 2048))
        out, i = [], 0
        while i < len(buf):
            rec_len = buf[i]
            if rec_len == 0:
                i = (i // 2048 + 1) * 2048
                if i >= len(buf):
                    break
                continue
            rec = buf[i:i + rec_len]
            name = rec[33:33 + rec[32]].decode("ascii", "replace")
            if name not in ("\x00", "\x01"):
                out.append((name.split(";")[0],
                            int.from_bytes(rec[2:6], "little"),
                            int.from_bytes(rec[10:14], "little"),
                            bool(rec[25] & 2)))
            i += rec_len
        return out


def find_text_tables(exe):
    """The item and weapon description tables of the executable.

    The decompilation carries Integral's Japanese descriptions; the port
    patches in the disc's own. Every description starts with the font's
    0xB0 0x14 control code and the tables are runs of pointers to them:
    24 items + 2 frozen items (one run of 26, followed in memory by the
    'mine detector unusable' string) and 10 weapons. Found by scanning so
    the executable's revision does not matter."""
    load = struct.unpack_from("<I", exe, 0x18)[0]
    base = 0x800
    strings = {}
    at = exe.find(b"\xb0\x14")
    while at >= 0:
        end = exe.find(b"\x00", at)
        s = exe[at:end]
        if 8 < len(s) < 400 and sum(32 <= c < 127 for c in s) > len(s) // 2:
            strings[load + at - base] = s
        at = exe.find(b"\xb0\x14", at + 2)
    words = struct.unpack_from("<%dI" % ((len(exe) - base) // 4), exe, base)
    runs = []
    k = 0
    while k < len(words):
        if words[k] in strings:
            j = k
            while j < len(words) and words[j] in strings:
                j += 1
            runs.append((k, j - k))
            k = j
        else:
            k += 1
    items = [r for r in runs if r[1] == 26]
    weapons = [r for r in runs if r[1] == 10]
    if len(items) != 1 or len(weapons) != 1:
        raise SystemExit("description tables not found in the executable (runs: %r)"
                         % [n for _, n in runs])
    k, _ = items[0]
    item_strs = [strings[words[k + t]] for t in range(24)]
    frozen_strs = [strings[words[k + 24 + t]] for t in range(2)]
    mine_addr = load + k * 4 + 26 * 4
    mine = strings.get(mine_addr)
    if mine is None:
        raise SystemExit("mine-detector string not after the item table")
    k, _ = weapons[0]
    weapon_strs = [strings[words[k + t]] for t in range(10)]
    return [(1, item_strs), (2, frozen_strs), (3, [mine]), (4, weapon_strs)]


def write_text(exe, outdir, exe_name):
    tables = [(0, [exe_name.encode("ascii")])] + find_text_tables(exe)
    os.makedirs(os.path.join(outdir, "MGS"), exist_ok=True)
    dst = os.path.join(outdir, "MGS", "TEXT.DAT")
    with open(dst, "wb") as f:
        f.write(b"MGST" + struct.pack("<II", 1, len(tables)))
        for ident, strs in tables:
            f.write(struct.pack("<II", ident, len(strs)))
            for s in strs:
                f.write(struct.pack("<H", len(s)) + s)
    print("  %-16s %12d bytes (%s)" % ("TEXT.DAT", os.path.getsize(dst),
          ", ".join("%d" % len(strs) for _, strs in tables)))


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    disc = Disc(sys.argv[1])
    outdir = sys.argv[2]
    want = None
    for a in sys.argv[3:]:
        if a.startswith("--stages="):
            want = a.split("=", 1)[1].split(",")

    pvd = disc.sector(16)
    root_lba = int.from_bytes(pvd[156 + 2:156 + 6], "little")
    root_len = int.from_bytes(pvd[156 + 10:156 + 14], "little")

    root = disc.listdir(root_lba, root_len)
    exe = None
    for name, lba, size, isdir in root:
        if not isdir and name.upper().startswith("SL") and "." in name:
            exe = (name, disc.read(lba, size))
    if "--text" in sys.argv:
        # only MGS/TEXT.DAT: the English menu descriptions from the executable
        if not exe:
            raise SystemExit("no executable on this disc")
        write_text(exe[1], outdir, exe[0])
        return
    if "--exe" in sys.argv:
        # the executable (SLUS_005.94, SLPM_862.47, ...) alone, into outdir:
        # the English menu text the port patches in comes from it
        for name, lba, size, isdir in root:
            if not isdir and name.upper().startswith("SL") and "." in name:
                os.makedirs(outdir, exist_ok=True)
                dst = os.path.join(outdir, name)
                with open(dst, "wb") as f:
                    f.write(disc.read(lba, size))
                print("  %-16s %12d bytes -> %s" % (name, size, dst))
        return

    mgs = [e for e in root if e[0] == "MGS"]
    if not mgs:
        raise SystemExit("no MGS directory on this disc")
    entries = disc.listdir(mgs[0][1], mgs[0][2])

    if "--list" in sys.argv:
        # the layout, with the gap to the next file: a file the game addresses
        # past its recorded size (RADIO.DAT's codec codes?) shows as a gap
        files = sorted((e for e in entries if not e[3]), key=lambda e: e[1])
        for i, (name, lba, size, isdir) in enumerate(files):
            nxt = files[i + 1][1] if i + 1 < len(files) else None
            gap = (nxt - lba) * 2048 - size if nxt else -1
            print("  %-16s lba %8d  size %12d (%7d sectors)  gap-to-next %d bytes"
                  % (name, lba, size, (size + 2047) // 2048, gap))
        return

    os.makedirs(os.path.join(outdir, "MGS"), exist_ok=True)
    for name, lba, size, isdir in entries:
        if isdir:
            continue
        if name == "STAGE.DIR" and want is not None:
            continue          # handled below
        dst = os.path.join(outdir, "MGS", name)
        print("  %-16s %12d bytes" % (name, size))
        with open(dst, "wb") as f:
            f.write(disc.read(lba, size))
    if exe:
        write_text(exe[1], outdir, exe[0])

    stage = [e for e in entries if e[0] == "STAGE.DIR"]
    if not stage:
        return
    lba, size = stage[0][1], stage[0][2]
    head = disc.sector(lba)
    table_size = struct.unpack("<i", head[0:4])[0]
    n = table_size // 12
    table = []
    for i in range(n):
        nm, off = struct.unpack("<8si", head[4 + i * 12:16 + i * 12])
        table.append((nm.decode("ascii", "replace").rstrip("\0"), off))
    print("\nSTAGE.DIR: %d stages, %d bytes total" % (n, size))

    if want is None:
        return

    os.makedirs(os.path.join(outdir, "MGS", "STAGE"), exist_ok=True)
    total = 0
    for i, (nm, off) in enumerate(table):
        if nm not in want:
            continue
        end = table[i + 1][1] if i + 1 < len(table) else (size // 2048)
        nsec = end - off
        blob = disc.read(lba + off, nsec * 2048)
        dst = os.path.join(outdir, "MGS", "STAGE", nm + ".bin")
        with open(dst, "wb") as f:
            f.write(blob)
        total += len(blob)
        print("  stage %-8s %8d bytes" % (nm, len(blob)))
    print("selected stages total: %d bytes (%.1f MB)" % (total, total / 1048576.0))


if __name__ == "__main__":
    main()

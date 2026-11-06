"""mku-play's disc checks, on small ISO images built here (no game data needed)."""
import hashlib
import importlib.machinery
import importlib.util
import os
import struct
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, "..", "..", "mku-play")
loader = importlib.machinery.SourceFileLoader("mku_play", TOOL)
spec = importlib.util.spec_from_loader("mku_play", loader)
mku = importlib.util.module_from_spec(spec)
loader.exec_module(mku)

SECTOR = 2048
fails = 0


def check(what, ok):
    global fails
    if not ok:
        print("FAIL", what)
        fails += 1


def both32(v):
    return struct.pack("<I", v) + struct.pack(">I", v)


def record(name, lba, size, is_dir):
    raw = name.encode()
    n = 33 + len(raw)
    n += n & 1
    r = bytes([n, 0]) + both32(lba) + both32(size) + bytes(7) + bytes([2 if is_dir else 0, 0, 0])
    r += struct.pack("<H", 1) + struct.pack(">H", 1) + bytes([len(raw)]) + raw
    return r.ljust(n, b"\0")


def make_iso(tree, padding_entries=0):
    """tree: {"DIR/SUB/FILE": bytes}. Directories get one sector each, or more if needed."""
    dirs = {"": []}
    for path in tree:
        parts = path.split("/")
        for i in range(len(parts) - 1):
            parent, d = "/".join(parts[:i]), "/".join(parts[:i + 1])
            if d not in dirs:
                dirs[d] = []
                dirs[parent].append((parts[i], d, True))
        dirs["/".join(parts[:-1])].append((parts[-1], path, False))
    dir_sectors = {d: 1 for d in dirs}
    if padding_entries:   # the file sits behind enough records to cross a sector boundary
        dir_sectors["PSP_GAME/SYSDIR"] = 2
    lba, locs = 18, {}
    for d in dirs:
        locs[d] = lba
        lba += dir_sectors[d]
    for path, data in tree.items():
        locs[path] = lba
        lba += max(1, (len(data) + SECTOR - 1) // SECTOR)
    img = bytearray(lba * SECTOR)
    for d, entries in dirs.items():
        body = bytearray()
        size = dir_sectors[d] * SECTOR
        recs = [record("\0", locs[d], SECTOR, True), record("\1", locs[d], SECTOR, True)]
        if d == "PSP_GAME/SYSDIR":
            recs += [record(f"PAD{i:05d}.BIN;1", 18, 0, False) for i in range(padding_entries)]
        for name, target, is_dir in entries:
            sz = dir_sectors[target] * SECTOR if is_dir else len(tree[target])
            recs.append(record(name if is_dir else name + ";1", locs[target], sz, is_dir))
        for r in recs:
            if len(body) // SECTOR != (len(body) + len(r) - 1) // SECTOR:   # never across a sector
                body.extend(bytes(SECTOR - len(body) % SECTOR))
            body.extend(r)
        img[locs[d] * SECTOR:locs[d] * SECTOR + len(body)] = body
    for path, data in tree.items():
        img[locs[path] * SECTOR:locs[path] * SECTOR + len(data)] = data
    pvd = bytearray(SECTOR)
    pvd[0:6] = b"\x01CD001"
    pvd[156:156 + 34] = record("\0", locs[""], SECTOR, True)
    img[16 * SECTOR:17 * SECTOR] = pvd
    return bytes(img)


def make_sfo(disc_id):
    key = b"DISC_ID\0"
    val = disc_id.encode() + b"\0"
    keys = 20 + 16
    vals = keys + len(key)
    return b"\0PSF" + struct.pack("<IIII", 0x101, keys, vals, 1) + \
        struct.pack("<HHIII", 0, 0x0204, len(val), len(val), 0) + key + val


def try_load(data):
    with tempfile.NamedTemporaryFile(suffix=".iso", delete=False) as f:
        f.write(data)
    try:
        return mku.load_game(f.name), None
    except mku.IsoError as e:
        return None, str(e)
    finally:
        os.unlink(f.name)


exe = b"\x7fELF fake executable " * 400
mku.SUPPORTED[hashlib.sha256(exe).hexdigest()] = "Test Game"
good = {"PSP_GAME/SYSDIR/BOOT.BIN": exe, "PSP_GAME/PARAM.SFO": make_sfo("ULUS10102"), "UMD_DATA.BIN": b"x"}

got, err = try_load(make_iso(good))
check(f"good image accepted ({err})", got and got[0] == exe and got[1] == "Test Game")

got, err = try_load(make_iso(good, padding_entries=70))
check(f"file behind a directory sector boundary ({err})", got and got[0] == exe)

other = dict(good)
other["PSP_GAME/SYSDIR/BOOT.BIN"] = b"\x7fELF some other game" * 50
other["PSP_GAME/PARAM.SFO"] = make_sfo("UCES00000")
_, err = try_load(make_iso(other))
check("other game refused, naming its disc id", err and "unsupported" in err and "UCES00000" in err)

blank = dict(good)
blank["PSP_GAME/SYSDIR/BOOT.BIN"] = bytes(8192)
_, err = try_load(make_iso(blank))
check("blanked BOOT.BIN explained", err and "blank" in err)

_, err = try_load(make_iso({"SOMETHING/ELSE.BIN": b"data"}))
check("not a PSP disc", err and "isn't a PSP game" in err)

_, err = try_load(b"CISO" + bytes(100000))
check("CSO explained", err and "CSO" in err)

_, err = try_load(bytes(100000))
check("not an ISO at all", err and "isn't an ISO" in err)

_, err = try_load(b"short")
check("tiny file", err and "isn't an ISO" in err)

try:
    mku.load_game("/nonexistent/game.iso")
    check("missing file", False)
except mku.IsoError as e:
    check("missing file", "can't be opened" in str(e))

check("sfo field", mku.sfo_field(make_sfo("ULUS10102"), "DISC_ID") == "ULUS10102")
check("sfo missing key", mku.sfo_field(make_sfo("ULUS10102"), "TITLE") is None)
check("sfo garbage", mku.sfo_field(b"garbage", "DISC_ID") is None and mku.sfo_field(None, "DISC_ID") is None)

if fails:
    print(f"test_mku_play: {fails} failed")
    sys.exit(1)
print("test_mku_play: ok")

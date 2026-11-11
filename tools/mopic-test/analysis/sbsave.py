"""Stellar Blade (PC) save files: read their progress offline and retarget them to another Steam account.

A save (StellarBladeSave00.sav, %LOCALAPPDATA%\\SB\\Saved\\SaveGames\\<SteamID64>\\) is
  b"EVAS" + uint32 1 | an uncompressed UE4 GVAS body | uint32 CRC32 of the body (bytes 8 .. end-4), little endian.
The owner's SteamID64 is a top-level struct property near the end:
  OnlineAccountInfo (StructProperty SBSaveGameData_OnlineAccountInfo)
    OnlineStoreType (EnumProperty ESBOnlineStoreType::Steam)
    OnlineAccountID (StrProperty "7656119...")
The game only loads a save whose OnlineAccountID is the logged-in account (community uploads say "put your own ID
in with a hex editor"); `retarget` writes another ID, fixes the three sizes around it and the CRC.

usage:
  sbsave.py info <save> [<save> ...] [--json]
  sbsave.py retarget <in> <out> --steamid <SteamID64>
  sbsave.py check <save>                      # exit 0 when the header and CRC are valid
  sbsave.py selftest --work <dir> [<real save> ...]   # synthetic saves (no game); leaves <dir>\\synthetic.sav
"""
import glob
import json
import os
import re
import struct
import subprocess
import sys
import zlib

MAGIC = b"EVAS"


class SaveError(Exception):
    pass


def read_bytes(path):
    with open(path, "rb") as f:
        return f.read()


def write_bytes(path, data):
    with open(path, "wb") as f:
        f.write(data)


def split(data):
    if len(data) < 16 or data[:4] != MAGIC or data[8:12] != b"GVAS":
        raise SaveError("not an EVAS/GVAS Stellar Blade save")
    body = data[8:-4]
    stored = struct.unpack_from("<I", data, len(data) - 4)[0]
    return data[:8], body, stored, zlib.crc32(body) & 0xFFFFFFFF


def join(head, body):
    return head + body + struct.pack("<I", zlib.crc32(body) & 0xFFFFFFFF)


def fstring(s):
    if s == "":
        return struct.pack("<i", 0)
    b = s.encode("ascii") + b"\0"
    return struct.pack("<i", len(b)) + b


def read_fstring(buf, pos):
    n = struct.unpack_from("<i", buf, pos)[0]
    pos += 4
    if n == 0:
        return "", pos
    if n < 0:  # UTF-16
        if n < -4096 or pos - 2 * n > len(buf):
            raise SaveError(f"implausible string length {n} at {pos}")
        try:
            return buf[pos:pos - 2 * n].decode("utf-16-le").rstrip("\0"), pos - 2 * n
        except UnicodeDecodeError as e:
            raise SaveError(f"bad UTF-16 string at {pos}: {e}") from None
    if n > 4096 or pos + n > len(buf):
        raise SaveError(f"implausible string length {n} at {pos}")
    return buf[pos:pos + n].rstrip(b"\0").decode("latin-1"), pos + n


def find_property(body, name, ptype, start=0):
    """Offset of the property tag `name` of type `ptype` (the FString length of its name)."""
    key = fstring(name) + fstring(ptype)
    i = body.find(key, start)
    return i


def account_info(body):
    """(struct_offset, id_offset, store_type, steam_id): the OnlineAccountInfo struct and its OnlineAccountID."""
    s = find_property(body, "OnlineAccountInfo", "StructProperty")
    if s < 0:
        raise SaveError("no OnlineAccountInfo struct")
    a = find_property(body, "OnlineAccountID", "StrProperty", s)
    if a < 0:
        raise SaveError("no OnlineAccountID in OnlineAccountInfo")
    store = None
    t = find_property(body, "OnlineStoreType", "EnumProperty", s)
    if 0 <= t < a:
        p = t + len(fstring("OnlineStoreType") + fstring("EnumProperty")) + 8
        _, p = read_fstring(body, p)  # enum type
        p += 1  # guid flag
        store, _ = read_fstring(body, p)
    p = a + len(fstring("OnlineAccountID") + fstring("StrProperty"))
    size = struct.unpack_from("<q", body, p)[0]
    value, end = read_fstring(body, p + 9)
    if end - (p + 9) != size:
        raise SaveError(f"OnlineAccountID size {size} does not match its string")
    return s, a, store, value


def retarget(data, steam_id):
    if not (steam_id.isascii() and steam_id.isdigit()):
        raise SaveError(f"SteamID64 must be digits: {steam_id!r}")
    head, body, stored, actual = split(data)
    if stored != actual:
        raise SaveError(f"CRC mismatch before retarget (stored {stored:08x}, body {actual:08x})")
    s, a, _store, old = account_info(body)
    # struct tag: name, type, int64 size at s+len(name+type)
    s_size_at = s + len(fstring("OnlineAccountInfo") + fstring("StructProperty"))
    a_size_at = a + len(fstring("OnlineAccountID") + fstring("StrProperty"))
    value_at = a_size_at + 9
    # the stored FString as it is (an uploader may have blanked or zeroed the digits): int32 length + bytes
    old_len = struct.unpack_from("<i", body, value_at)[0]
    if old_len < 0:
        raise SaveError("UTF-16 OnlineAccountID is not supported")
    old_value = body[value_at:value_at + 4 + old_len]
    new_value = fstring(steam_id)
    delta = len(new_value) - len(old_value)
    body = bytearray(body)
    body[value_at:value_at + len(old_value)] = new_value
    struct.pack_into("<q", body, a_size_at, struct.unpack_from("<q", body, a_size_at)[0] + delta)
    struct.pack_into("<q", body, s_size_at, struct.unpack_from("<q", body, s_size_at)[0] + delta)
    out = join(head, bytes(body))
    _, nbody, _, _ = split(out)
    if account_info(nbody)[3] != steam_id:
        raise SaveError("retarget did not read back")
    return out, old


def read_map(body, name):
    """A top-level MapProperty<NameProperty, Int/Float/Str/Name/Int64/Bool> as a dict (None when absent)."""
    i = find_property(body, name, "MapProperty")
    if i < 0:
        return None
    p = i + len(fstring(name) + fstring("MapProperty")) + 8
    key_type, p = read_fstring(body, p)
    value_type, p = read_fstring(body, p)
    p += 1  # guid flag
    _removed, count = struct.unpack_from("<ii", body, p)
    p += 8
    if key_type != "NameProperty" or count > 100000:
        return None
    out = {}
    for _ in range(count):
        key, p = read_fstring(body, p)
        if value_type == "IntProperty":
            out[key] = struct.unpack_from("<i", body, p)[0]; p += 4
        elif value_type == "Int64Property":
            out[key] = struct.unpack_from("<q", body, p)[0]; p += 8
        elif value_type == "FloatProperty":
            out[key] = round(struct.unpack_from("<f", body, p)[0], 3); p += 4
        elif value_type == "BoolProperty":
            out[key] = bool(body[p]); p += 1
        elif value_type in ("StrProperty", "NameProperty"):
            out[key], p = read_fstring(body, p)
        else:
            return {"_unparsed": value_type}
    return out


def read_name_array(body, name):
    i = find_property(body, name, "ArrayProperty")
    if i < 0:
        return None
    p = i + len(fstring(name) + fstring("ArrayProperty")) + 8
    inner, p = read_fstring(body, p)
    p += 1
    if inner != "NameProperty":
        return None
    count = struct.unpack_from("<i", body, p)[0]
    p += 4
    out = []
    for _ in range(min(count, 1000)):
        v, p = read_fstring(body, p)
        out.append(v)
    return out


def info(path):
    data = read_bytes(path)
    head, body, stored, actual = split(data)
    r = {"path": path, "size": len(data), "crc_ok": stored == actual}
    ver = body.find(b"++UE4+")
    if ver >= 0:
        r["engine"] = body[ver:body.find(b"\0", ver)].decode("latin-1")
    try:
        _, _, store, sid = account_info(body)
        r["store"], r["steam_id"] = store, sid
    except SaveError as e:
        r["account_error"] = str(e)
    ints = read_map(body, "DataMap_int32") or {}
    for k in ("Version", "GameDifficulty", "NewGamePlusPlayCount", "PlayTime"):
        if k in ints:
            r[k] = ints[k]
    strs = read_map(body, "DataMap_FString") or {}
    for k in ("MapName", "CampAlias"):
        if k in strs:
            r[k] = strs[k]
    tracking = read_name_array(body, "TrackingQuestAliases")
    if tracking is not None:
        r["tracking_quests"] = tracking
    epic = sorted(set(m.decode() for m in re.findall(rb"LV_Quest_Epic_[0-9A-Za-z_]+", body)))
    r["epic_flags"] = epic
    if "PlayTime" in r:
        r["play_h"] = round(r["PlayTime"] / 3600, 2)
    return r


def _tag(name, ptype, size):
    return fstring(name) + fstring(ptype) + struct.pack("<q", size)


def synthetic(steam_id="76561190000000001", account_id_raw=None):
    """A save laid out like the game's (the properties sbsave reads, in the same encoding). account_id_raw: the
    OnlineAccountID FString bytes as an uploader may have left them (b"\\0\\0\\0\\0" = blanked)."""
    def int_map(name, items):
        payload = struct.pack("<ii", 0, len(items)) + b"".join(fstring(k) + struct.pack("<i", v) for k, v in items.items())
        return _tag(name, "MapProperty", len(payload)) + fstring("NameProperty") + fstring("IntProperty") + b"\0" + payload

    def str_map(name, items):
        payload = struct.pack("<ii", 0, len(items)) + b"".join(fstring(k) + fstring(v) for k, v in items.items())
        return _tag(name, "MapProperty", len(payload)) + fstring("NameProperty") + fstring("StrProperty") + b"\0" + payload

    def name_array(name, values):
        payload = struct.pack("<i", len(values)) + b"".join(fstring(v) for v in values)
        return _tag(name, "ArrayProperty", len(payload)) + fstring("NameProperty") + b"\0" + payload

    store = fstring("ESBOnlineStoreType::Steam")
    sid = account_id_raw if account_id_raw is not None else fstring(steam_id)
    inner = (_tag("OnlineStoreType", "EnumProperty", len(store)) + fstring("ESBOnlineStoreType") + b"\0" + store
             + _tag("OnlineAccountID", "StrProperty", len(sid)) + b"\0" + sid + fstring("None"))
    account = (_tag("OnlineAccountInfo", "StructProperty", len(inner)) + fstring("SBSaveGameData_OnlineAccountInfo")
               + b"\0" * 16 + b"\0" + inner)
    body = (b"GVAS" + struct.pack("<iii", 2, 522, 4) + struct.pack("<HHHI", 4, 26, 2, 0)
            + fstring("++UE4+Release-4.26") + struct.pack("<ii", 3, 0) + fstring("/Script/SB.SBSaveGame")
            + int_map("DataMap_int32", {"Version": 7, "GameDifficulty": 1, "NewGamePlusPlayCount": 2, "PlayTime": 9000,
                                        "LV_Quest_Epic_03_Clear": 1})
            + str_map("DataMap_FString", {"MapName": "G03", "CampAlias": "SD_10_EnvS_010"})
            + name_array("TrackingQuestAliases", ["Quest_Epic_04"])
            + account + fstring("None") + b"\0" * 4)
    return join(MAGIC + struct.pack("<I", 1), body)


def struct_size_ok(data):
    """The OnlineAccountInfo struct's size covers exactly its properties and their None (it is the last property)."""
    _, body, _, _ = split(data)
    s = account_info(body)[0]
    size_at = s + len(fstring("OnlineAccountInfo") + fstring("StructProperty"))
    value_at = size_at + 8 + len(fstring("SBSaveGameData_OnlineAccountInfo")) + 17
    end = body.rfind(fstring("None"), 0, len(body) - 4)  # the save's closing None, after the struct
    return struct.unpack_from("<q", body, size_at)[0] == end - value_at


def selftest(work, real):
    os.makedirs(work, exist_ok=True)
    fails = 0

    def check(name, ok, detail=""):
        nonlocal fails
        print(("ok    " if ok else "FAIL  ") + name + ("" if ok else f"  {detail}"))
        fails += 0 if ok else 1

    def raises(name, fn, text):
        try:
            fn()
            check(name, False, "no error")
        except SaveError as e:
            check(name, text in str(e), str(e))

    src = synthetic()
    path = os.path.join(work, "synthetic.sav")
    write_bytes(path, src)
    r = info(path)
    check("info: CRC, engine, store, owner", r.get("crc_ok") is True and r.get("engine") == "++UE4+Release-4.26"
          and r.get("store") == "ESBOnlineStoreType::Steam" and r.get("steam_id") == "76561190000000001", r)
    check("info: progress fields", (r.get("Version"), r.get("GameDifficulty"), r.get("NewGamePlusPlayCount"),
                                    r.get("PlayTime"), r.get("play_h"), r.get("MapName"), r.get("CampAlias"))
          == (7, 1, 2, 9000, 2.5, "G03", "SD_10_EnvS_010"), r)
    check("info: tracked quest and epic flags", r.get("tracking_quests") == ["Quest_Epic_04"]
          and r.get("epic_flags") == ["LV_Quest_Epic_03_Clear"], r)
    check("synthetic struct size consistent", struct_size_ok(src))

    same_len, old = retarget(src, "76561199750818073")
    check("retarget to another 17 digits: old id returned, same size", old == "76561190000000001"
          and len(same_len) == len(src), (old, len(same_len), len(src)))
    shorter, _ = retarget(src, "123")
    _, body, stored, actual = split(shorter)
    check("retarget to 3 digits: 14 bytes shorter, CRC valid, id reads back",
          len(shorter) == len(src) - 14 and stored == actual and account_info(body)[3] == "123")
    check("retarget to 3 digits: struct size follows", struct_size_ok(shorter))
    back, _ = retarget(shorter, "76561190000000001")
    check("retarget back: byte-identical to the original", back == src)
    blank = synthetic(account_id_raw=b"\0\0\0\0")
    filled, old_blank = retarget(blank, "76561199750818073")
    check("blanked id (empty FString): filled in, sizes follow", old_blank == ""
          and account_info(split(filled)[1])[3] == "76561199750818073" and struct_size_ok(filled), old_blank)
    def rest(row):
        return {k: v for k, v in row.items() if k not in ("path", "size", "steam_id")}
    p2 = os.path.join(work, "shorter.sav")
    write_bytes(p2, shorter)
    check("retarget keeps everything else", rest(info(p2)) == rest(r), (info(p2), r))

    damaged = bytearray(src)
    damaged[100] ^= 0xFF
    raises("CRC mismatch refused", lambda: retarget(bytes(damaged), "1"), "CRC mismatch")
    raises("non-digit id refused", lambda: retarget(src, "7656x"), "must be digits")
    raises("non-ASCII digits refused", lambda: retarget(src, "١٢٣"), "must be digits")
    odd = bytearray(src)
    i = odd.find(fstring("MapName"))
    struct.pack_into("<i", odd, i, -3)  # a UTF-16 string of 6 bytes: "Map" read as three odd code units
    odd[i + 4:i + 10] = b"\x00\xd8\x00\xd8\x00\xd8"  # unpaired surrogates: not decodable
    p_odd = os.path.join(work, "odd-utf16.sav")
    write_bytes(p_odd, join(odd[:8], bytes(odd[8:-4])))
    cp = subprocess.run([sys.executable, "-I", os.path.abspath(__file__), "info", p_odd, "--json"],
                        capture_output=True, text=True)
    rows = json.loads(cp.stdout) if cp.returncode == 0 else []
    check("info: a bad UTF-16 string is that file's error row, no traceback",
          len(rows) == 1 and "bad UTF-16 string" in rows[0].get("error", ""), cp.stdout + cp.stderr)
    raises("not a save refused", lambda: retarget(b"GVAS" + src[8:], "1"), "not an EVAS/GVAS")
    raises("save without OnlineAccountInfo refused",
           lambda: retarget(join(src[:8], src[8:-4].replace(b"OnlineAccountInfo", b"OnlineAccountInfX")), "1"),
           "no OnlineAccountInfo")

    me = os.path.abspath(__file__)
    out = os.path.join(work, "cli-out.sav")
    cp = subprocess.run([sys.executable, "-I", me, "retarget", path, out, "--steamid", "76561199750818073"],
                        capture_output=True, text=True)
    ok = cp.returncode == 0 and json.loads(cp.stdout).get("old_steam_id") == "76561190000000001"
    check("CLI retarget: exit 0, JSON line", ok, cp.stdout + cp.stderr)
    cp = subprocess.run([sys.executable, "-I", me, "check", out], capture_output=True, text=True)
    check("CLI check of the result: exit 0", cp.returncode == 0 and cp.stdout.strip() == "ok", cp.stdout)
    bad = os.path.join(work, "damaged.sav")
    write_bytes(bad, bytes(damaged))
    cp = subprocess.run([sys.executable, "-I", me, "check", bad], capture_output=True, text=True)
    check("CLI check of a damaged save: exit 1", cp.returncode == 1 and "CRC mismatch" in cp.stdout, cp.stdout)
    cp = subprocess.run([sys.executable, "-I", me, "retarget", bad, out + "2", "--steamid", "1"],
                        capture_output=True, text=True)
    check("CLI retarget of a damaged save: exit 1, no output file", cp.returncode == 1
          and "CRC mismatch" in cp.stderr and not os.path.exists(out + "2"), cp.stderr)

    for p in real:
        data = read_bytes(p)
        try:
            r = info(p)
            there, old = retarget(data, "123")
            # a blanked id can't be written back (retarget takes digits only): the CRC check alone then
            identical = retarget(there, old)[0] == data if old else True
            check(f"real save {os.path.basename(p)} ({old or 'blank id'}): CRC ok, round trip identical",
                  r.get("crc_ok") is True and identical, r)
        except SaveError as e:
            check(f"real save {os.path.basename(p)}", False, str(e))
    return 1 if fails else 0


def main(argv):
    if len(argv) < 2:
        print(__doc__)
        return 2
    cmd = argv[1]
    if cmd == "selftest":
        if len(argv) < 4 or argv[2] != "--work":
            print(__doc__)
            return 2
        real = [p for a in argv[4:] for p in sorted(glob.glob(a, recursive=True))]
        return selftest(argv[3], real)
    if cmd == "info":
        paths = [a for a in argv[2:] if a != "--json"]
        rows = []
        for p in paths:
            try:
                rows.append(info(p))
            except (SaveError, struct.error, OSError) as e:
                rows.append({"path": p, "error": str(e)})
        if "--json" in argv:
            print(json.dumps(rows, ensure_ascii=False, indent=1))
        else:
            for r in rows:
                print(json.dumps(r, ensure_ascii=False))
        return 0
    if cmd == "retarget":
        if len(argv) != 6 or argv[4] != "--steamid":
            print(__doc__)
            return 2
        data = read_bytes(argv[2])
        out, old = retarget(data, argv[5])
        write_bytes(argv[3], out)
        print(json.dumps({"in": argv[2], "out": argv[3], "old_steam_id": old, "steam_id": argv[5],
                          "size": len(out)}))
        return 0
    if cmd == "check":
        _, _, stored, actual = split(read_bytes(argv[2]))
        print("ok" if stored == actual else f"CRC mismatch: stored {stored:08x}, body {actual:08x}")
        return 0 if stored == actual else 1
    print(__doc__)
    return 2


if __name__ == "__main__":
    try:
        sys.exit(main(sys.argv))
    except SaveError as e:
        print(f"error: {e}", file=sys.stderr)
        sys.exit(1)

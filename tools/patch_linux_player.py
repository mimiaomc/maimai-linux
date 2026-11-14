#!/usr/bin/env python3
"""
Patch official Unity 2018.4.7f1 LinuxPlayer (linux64_withgfx_nondevelopment_mono)
to allow -force-glcore on Windows-built D3D11 game assets without rejecting
forced GfxDevice because glcore was not in the Windows build target list.
"""
import os
import sys

ORIG_SEQ = bytes.fromhex("48 39 c5 0f 85 48 01 00 00 83 fb 12")
PATCHED_SEQ = bytes.fromhex("48 39 c5 e9 49 01 00 00 90 83 fb 12")
EXPECTED_OFFSET = 0x8e8a7f
MARKER = b"Forced GfxDevice '%s' was not built from editor"


def patch_player(src_path: str, dst_path: str) -> int:
    with open(src_path, "rb") as f:
        data = bytearray(f.read())

    if MARKER not in data:
        print(f"[!] Error: '{MARKER.decode()}' marker not found in {src_path}", file=sys.stderr)
        print("    Make sure the input is Unity 2018.4.7f1 LinuxPlayer (linux64_withgfx_nondevelopment_mono).", file=sys.stderr)
        return 1

    if data[EXPECTED_OFFSET:EXPECTED_OFFSET + len(PATCHED_SEQ)] == PATCHED_SEQ:
        print(f"[=] {src_path} already has the -force-glcore patch applied.")
    elif data[EXPECTED_OFFSET:EXPECTED_OFFSET + len(ORIG_SEQ)] == ORIG_SEQ:
        data[EXPECTED_OFFSET:EXPECTED_OFFSET + len(ORIG_SEQ)] = PATCHED_SEQ
        print(f"[+] Patched -force-glcore check at offset 0x{EXPECTED_OFFSET + 3:x}.")
    else:
        hits = []
        start = 0
        while True:
            idx = data.find(ORIG_SEQ, start)
            if idx == -1:
                break
            hits.append(idx)
            start = idx + 1
        if len(hits) == 1:
            off = hits[0]
            data[off:off + len(ORIG_SEQ)] = PATCHED_SEQ
            print(f"[+] Patched -force-glcore check at offset 0x{off + 3:x}.")
        elif data.find(PATCHED_SEQ) != -1:
            print(f"[=] {src_path} already appears patched.")
        else:
            print(f"[!] Error: Expected instruction sequence not found (hits={len(hits)}).", file=sys.stderr)
            return 1

    with open(dst_path, "wb") as f:
        f.write(data)
    os.chmod(dst_path, 0o755)
    print(f"[+] Wrote executable player to {dst_path}")
    return 0


def main() -> int:
    if len(sys.argv) < 2 or sys.argv[1] in ("-h", "--help"):
        print(f"Usage: {sys.argv[0]} <LinuxPlayer_in> [Sinmai.x86_64_out]")
        return 1
    src = sys.argv[1]
    dst = sys.argv[2] if len(sys.argv) > 2 else src
    if not os.path.isfile(src):
        print(f"[!] File not found: {src}", file=sys.stderr)
        return 1
    return patch_player(src, dst)


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""visionmac: gives the PC's firmware bigger screens.

EDK2's driver for the framebuffer the virtual PC has (QemuRamfbDxe) knows three modes: 640x480, 800x600 and
1024x768. Windows keeps the mode the firmware starts in, so that was all it could have. This replaces the three in
the firmware image by the ones given (default 1280x720, 1600x900, 1920x1080). Which of them the firmware starts in
is said by its PlatformConfig variable (see vars.fd).

The modes sit in the DXE volume, which is one LZMA stream inside the image. It is unpacked, changed, packed again
and put back in the same place, zero-padded to the old length: every size and checksum around it stays as it was.

  patch_firmware.py IN.fd OUT.fd [WxH WxH WxH]
"""
import lzma, struct, sys, uuid

LZMA_SECTION = uuid.UUID("EE4E5898-3914-4259-9D6E-DC7BD79403CF").bytes_le

def main():
    src, dst = sys.argv[1], sys.argv[2]
    want = [tuple(int(n) for n in a.lower().split("x")) for a in sys.argv[3:6]] or [(1280, 720), (1600, 900), (1920, 1080)]
    assert len(want) == 3, "three modes"
    fd = bytearray(open(src, "rb").read())
    # The section's GUID is in the image more than once (the code that unpacks it names it too): the section is the
    # one that unpacks.
    fv, at = None, fd.find(LZMA_SECTION)
    while at >= 0 and fv is None:
        section = at - 4
        size = int.from_bytes(fd[section:section + 3], "little")
        data_off = struct.unpack_from("<H", fd, at + 16)[0]
        start, end = section + data_off, section + size
        try:
            props, dict_size, unpacked = fd[start], struct.unpack_from("<I", fd, start + 1)[0], struct.unpack_from("<Q", fd, start + 5)[0]
            out = lzma.LZMADecompressor(format=lzma.FORMAT_ALONE).decompress(bytes(fd[start:end]))
            if len(out) == unpacked and unpacked > (1 << 20): fv = bytearray(out)
        except (lzma.LZMAError, struct.error, EOFError):
            pass
        if fv is None: at = fd.find(LZMA_SECTION, at + 1)
    assert fv is not None, "no LZMA section that unpacks"
    # EFI_GRAPHICS_OUTPUT_MODE_INFORMATION x 3: version 0, width, height, then 24 bytes of zero.
    old = [(640, 480), (800, 600), (1024, 768)]
    pattern = b"".join(struct.pack("<3I", 0, w, h) + bytes(24) for w, h in old)
    table = fv.find(pattern)
    assert table >= 0 and fv.find(pattern, table + 1) < 0, "the mode table was not found exactly once"
    for i, (w, h) in enumerate(want):
        struct.pack_into("<2I", fv, table + 36 * i + 4, w, h)
    lc, rest = props % 9, props // 9
    filt = {"id": lzma.FILTER_LZMA1, "dict_size": dict_size, "lc": lc, "lp": rest % 5, "pb": rest // 5,
            "preset": 9 | lzma.PRESET_EXTREME}
    packed = bytearray(lzma.compress(bytes(fv), format=lzma.FORMAT_ALONE, filters=[filt]))
    struct.pack_into("<Q", packed, 5, len(fv))  # the firmware sizes its buffer by this; Python leaves it "unknown"
    room = end - start
    assert len(packed) <= room, f"packed again it is {len(packed)} bytes, {len(packed) - room} more than there is room for"
    fd[start:end] = packed + bytes(room - len(packed))
    open(dst, "wb").write(fd)
    print(f"modes {old} -> {want}; LZMA {room} -> {len(packed)} bytes (+{room - len(packed)} zero), DXE volume {len(fv)} bytes")

main()

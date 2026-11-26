"""Deterministic ZIP fixture primitives (standard library only)."""
import io
import struct
import zipfile


class Utf8Info(zipfile.ZipInfo):
    def _encodeFilenameFlags(self):
        # The container profile requires bit 11 even for ASCII entry names.
        return self.filename.encode("utf-8"), self.flag_bits | 0x800


class NonSeekable(io.BytesIO):
    def seekable(self):
        return False

    def seek(self, *args):
        raise io.UnsupportedOperation("descriptor fixture")


def fixture_info(entry, info_type=Utf8Info):
    info = info_type(entry['name'], (2020, 1, 1, 0, 0, 0))
    info.compress_type = {'stored': zipfile.ZIP_STORED, 'deflate': zipfile.ZIP_DEFLATED}[entry.get('method', 'stored')]
    # Level zero avoids zlib/zlib-ng match and Huffman heuristics.
    info._compresslevel = 0
    info.create_system = 3
    info.external_attr = 0o100644 << 16
    info.extra = bytes.fromhex(entry.get('extra_hex', ''))
    info.comment = entry.get('comment', '').encode('utf8')
    return info


def pack_zip(entries, payload_for, *, descriptor=False, zip64=False, info_factory=fixture_info):
    buffer = NonSeekable() if descriptor else io.BytesIO()
    with zipfile.ZipFile(buffer, 'w') as archive:
        for entry in entries:
            with archive.open(info_factory(entry), 'w', force_zip64=zip64) as stream:
                stream.write(payload_for(entry))
    return buffer.getvalue()


def patch_archive(data, patches):
    """Apply declared local/central header patches, never search payload bytes."""
    data = bytearray(data)
    eocd = data.rfind(b"PK\x05\x06")
    central = struct.unpack_from("<I", data, eocd + 16)[0]
    records = []
    while data[central:central + 4] == b"PK\x01\x02":
        local = struct.unpack_from("<I", data, central + 42)[0]
        lengths = struct.unpack_from("<HHH", data, central + 28)
        records.append((local, central))
        central += 46 + sum(lengths)
    for patch in patches:
        if patch["kind"] == "truncate":
            del data[patch["size"]:]
            continue
        local, central = records[patch["entry"]]
        kind = patch["kind"]
        if kind == "name":
            name = patch["value"].encode("utf-8")
            for start, length_offset, fixed in [(local, 26, 30), (central, 28, 46)]:
                length = struct.unpack_from("<H", data, start + length_offset)[0]
                if len(name) != length:
                    raise ValueError("Name patches must preserve encoded length")
                data[start + fixed:start + fixed + length] = name
        elif kind in ("method", "flags", "crc"):
            offsets, fmt = {"method": ((8, 10), "<H"), "flags": ((6, 8), "<H"),
                            "crc": ((14, 16), "<I")}[kind]
            for start, offset in zip((local, central), offsets):
                struct.pack_into(fmt, data, start + offset, patch["value"])
        elif kind == "external_attr":
            struct.pack_into("<I", data, central + 38, patch["value"])
        else:
            raise ValueError(f"Unknown patch kind: {kind}")
    return bytes(data)


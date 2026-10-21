"""Inspect the built PE without installing a binary inspection dependency."""
import json
from pathlib import Path
import struct

root = Path(__file__).resolve().parents[1]
executable = root / "build" / "DesktopBox.exe"
data = executable.read_bytes()
pe = struct.unpack_from("<I", data, 0x3C)[0]
if data[pe:pe + 4] != b"PE\0\0":
    raise RuntimeError("Not a PE executable")
machine, sections = struct.unpack_from("<HH", data, pe + 4)
optional_size = struct.unpack_from("<H", data, pe + 20)[0]
optional = pe + 24
if struct.unpack_from("<H", data, optional)[0] != 0x20B:
    raise RuntimeError("Expected PE32+ / x64")
subsystem = struct.unpack_from("<H", data, optional + 68)[0]
section_table = optional + optional_size


def file_offset(rva):
    for index in range(sections):
        section = section_table + index * 40
        virtual_size, address, raw_size, raw_pointer = struct.unpack_from("<IIII", data, section + 8)
        if address <= rva < address + max(virtual_size, raw_size):
            return raw_pointer + rva - address
    raise RuntimeError(f"Unmapped RVA: {rva:x}")


imports = []
import_rva = struct.unpack_from("<I", data, optional + 112 + 8)[0]
entry = file_offset(import_rva)
while any(data[entry:entry + 20]):
    name_rva = struct.unpack_from("<I", data, entry + 12)[0]
    name = file_offset(name_rva)
    imports.append(data[name:data.index(0, name)].decode("ascii"))
    entry += 20
result = {"file": str(executable), "bytes": len(data), "machine": hex(machine),
          "subsystem": subsystem, "imports": sorted(imports)}
if machine != 0x8664 or subsystem != 2:
    raise RuntimeError("Expected an x64 Windows GUI executable")
(root / "build" / "binary-info.json").write_text(json.dumps(result, indent=2), encoding="utf-8")
print(json.dumps(result, ensure_ascii=True, indent=2))


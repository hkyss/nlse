from __future__ import annotations

import struct
from pathlib import Path

SYSTEM = Path(r"C:\Windows\System32\winmm.dll")
TARGET = r"C:\\Windows\\System32\\winmm"
OUT = Path(__file__).with_name("winmm_exports.cpp")


def exports(path: Path) -> list[tuple[int, str | None]]:
    data = path.read_bytes()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    sections = struct.unpack_from("<H", data, pe + 6)[0]
    optional_size = struct.unpack_from("<H", data, pe + 20)[0]
    optional = pe + 24
    pe32_plus = struct.unpack_from("<H", data, optional)[0] == 0x20B
    export_rva = struct.unpack_from("<I", data, optional + (112 if pe32_plus else 96))[0]
    table = optional + optional_size

    def offset(rva: int) -> int:
        for i in range(sections):
            virtual_size, address, raw_size, raw = struct.unpack_from("<IIII", data, table + 40 * i + 8)
            if address <= rva < address + max(virtual_size, raw_size):
                return rva - address + raw
        raise ValueError(f"rva {rva:#x} is in no section")

    directory = offset(export_rva)
    base, functions, names, functions_rva, names_rva, ordinals_rva = struct.unpack_from(
        "<IIIIII", data, directory + 16
    )
    named: dict[int, str] = {}
    for i in range(names):
        name = offset(struct.unpack_from("<I", data, offset(names_rva) + 4 * i)[0])
        index = struct.unpack_from("<H", data, offset(ordinals_rva) + 2 * i)[0]
        named[index] = data[name : data.index(b"\0", name)].decode("ascii")
    found = []
    for index in range(functions):
        if struct.unpack_from("<I", data, offset(functions_rva) + 4 * index)[0]:
            found.append((base + index, named.get(index)))
    return found


def main() -> None:
    lines = []
    for ordinal, name in exports(SYSTEM):
        if name:
            lines.append(f'#pragma comment(linker, "/EXPORT:{name}={TARGET}.{name},@{ordinal}")')
        else:
            lines.append(
                f'#pragma comment(linker, "/EXPORT:winmm_ordinal_{ordinal}={TARGET}.#{ordinal},@{ordinal},NONAME")'
            )
    OUT.write_text("\n".join(lines) + "\n", encoding="ascii", newline="\n")
    print(f"{len(lines)} exports written to {OUT.name}")


if __name__ == "__main__":
    main()

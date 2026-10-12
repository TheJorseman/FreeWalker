#!/usr/bin/env python3
"""
Survey high-level architecture of TMRealWalk.dll for the WalkerCore roadmap.

Outputs structured information about imports, exports, sections, resources,
version info, and a categorized string catalog. This is metadata about the
binary's architecture - it does NOT reproduce function bodies or
implementation details. The output is meant to inform our own clean
implementation, not substitute for the original source code.

Run:
    python survey_dll.py /path/to/TMRealWalk.dll > survey.json
"""
import json
import re
import sys
from collections import Counter, defaultdict
from pathlib import Path

import pefile


def survey(path: Path) -> dict:
    pe = pefile.PE(str(path))

    # File metadata
    file_info = {
        "path": str(path),
        "size_bytes": path.stat().st_size,
        "sha256_input": None,  # caller fills if needed
        "machine": pe.FILE_HEADER.Machine,
        "characteristics": pe.FILE_HEADER.Characteristics,
        "subsystem": pe.OPTIONAL_HEADER.Subsystem,
        "image_base": hex(pe.OPTIONAL_HEADER.ImageBase),
        "entry_point_rva": hex(pe.OPTIONAL_HEADER.AddressOfEntryPoint),
        "is_dll": bool(pe.FILE_HEADER.Characteristics & 0x2000),
        "linker_version": (
            pe.OPTIONAL_HEADER.MajorLinkerVersion,
            pe.OPTIONAL_HEADER.MinorLinkerVersion,
        ),
        "os_version": (
            pe.OPTIONAL_HEADER.MajorOperatingSystemVersion,
            pe.OPTIONAL_HEADER.MinorOperatingSystemVersion,
        ),
        "image_version": (
            pe.OPTIONAL_HEADER.MajorImageVersion,
            pe.OPTIONAL_HEADER.MinorImageVersion,
        ),
    }

    # Sections
    sections = []
    for s in pe.sections:
        sections.append(
            {
                "name": s.Name.rstrip(b"\x00").decode(errors="replace"),
                "virtual_size": s.Misc_VirtualSize,
                "virtual_address": hex(s.VirtualAddress),
                "raw_size": s.SizeOfRawData,
                "characteristics": hex(s.Characteristics),
                "entropy": s.get_entropy(),
            }
        )

    # Imports - which Windows DLLs and which functions
    imports: dict[str, list[str]] = defaultdict(list)
    for entry in pe.DIRECTORY_ENTRY_IMPORT:
        dll = entry.dll.decode(errors="replace").lower()
        for imp in entry.imports:
            if imp.name:
                imports[dll].append(imp.name.decode(errors="replace"))
            elif imp.ordinal is not None:
                imports[dll].append(f"ord_{imp.ordinal}")

    # Exports - what the DLL exposes
    exports = []
    if hasattr(pe, "DIRECTORY_ENTRY_EXPORT"):
        for exp in pe.DIRECTORY_ENTRY_EXPORT.symbols:
            exports.append(
                {
                    "name": exp.name.decode(errors="replace") if exp.name else None,
                    "ordinal": exp.ordinal,
                    "rva": hex(exp.address) if exp.address else None,
                }
            )

    # Resources - this is the big one (sounds, fonts, icons, etc.)
    resources = []
    if hasattr(pe, "DIRECTORY_ENTRY_RESOURCE"):
        for type_entry in pe.DIRECTORY_ENTRY_RESOURCE.entries:
            for name_entry in type_entry.directory.entries:
                for lang_entry in name_entry.directory.entries:
                    rsrc_type = pefile.RESOURCE_TYPE.get(type_entry.struct.Id, "unknown")
                    data_entry = lang_entry.data
                    resources.append(
                        {
                            "type": rsrc_type,
                            "type_id": type_entry.struct.Id,
                            "name_id": name_entry.struct.Id if hasattr(name_entry.struct, "Id") else None,
                            "language": lang_entry.struct.Id,
                            "size": data_entry.struct.Size,
                            "offset": data_entry.struct.OffsetToData,
                            "rva": hex(data_entry.struct.OffsetToData),
                        }
                    )

    # Version info from VS_FIXEDFILEINFO + StringFileInfo
    version_info = {}
    if hasattr(pe, "VS_FIXEDFILEINFO"):
        ffi = pe.VS_FIXEDFILEINFO[0]
        version_info["fixed"] = {
            "signature": hex(ffi.Signature),
            "struct_version": ffi.StrucVersion,
            "file_version_ms": ffi.FileVersionMS,
            "file_version_ls": ffi.FileVersionLS,
            "product_version_ms": ffi.ProductVersionMS,
            "product_version_ls": ffi.ProductVersionLS,
            "file_flags_mask": hex(ffi.FileFlagsMask),
            "file_flags": hex(ffi.FileFlags),
            "file_os": hex(ffi.FileOS),
            "file_type": hex(ffi.FileType),
        }
    if hasattr(pe, "FileInfo") and len(pe.FileInfo) > 0:
        for entry in pe.FileInfo[0]:
            if hasattr(entry, "string_table"):
                for st in entry.string_table:
                    items = {}
                    for k, v in st.entries.items():
                        try:
                            items[k.decode(errors="replace")] = v.decode(errors="replace")
                        except Exception:
                            items[str(k)] = str(v)
                    version_info.setdefault("string_tables", []).append(items)

    return {
        "file_info": file_info,
        "sections": sections,
        "imports": {k: sorted(v) for k, v in sorted(imports.items())},
        "imports_count": {k: len(v) for k, v in imports.items()},
        "exports": exports,
        "exports_count": len(exports),
        "resources_count": len(resources),
        "resources_summary": summarize_resources(resources),
        "version_info": version_info,
    }


def summarize_resources(resources: list) -> dict:
    by_type: dict[str, dict] = {}
    total_size = 0
    for r in resources:
        t = r["type"]
        if t not in by_type:
            by_type[t] = {"count": 0, "total_bytes": 0}
        by_type[t]["count"] += 1
        by_type[t]["total_bytes"] += r["size"]
        total_size += r["size"]
    return {
        "total": total_size,
        "total_count": len(resources),
        "by_type": by_type,
    }


def categorize_imports(imports: dict[str, list[str]]) -> dict[str, list[str]]:
    """Categorize imported functions into architectural subsystems.
    Used to map the binary to a high-level module layout."""
    # Patterns to identify subsystems. These are well-known Win32 API prefixes -
    # nothing specific to TMRealWalk.
    patterns = {
        "Direct3D 11 (graphics)": re.compile(r"^(D3D|DXGI|ID3D|IDXGI)..."),
        "D3DCompiler (shader compile)": re.compile(r"^D3DCompiler"),
        "DirectInput (legacy input)": re.compile(r"^DirectInput|^DInput"),
        "XInput (gamepad)": re.compile(r"^XInput"),
        "WinHTTP / WinINet": re.compile(r"^WinHttp|^Internet"),
        "WinSock (TCP)": re.compile(r"^(WSA|socket|gethostbyname|inet_addr)..."),
        "CryptoAPI": re.compile(r"^Crypt"),
        "OpenAL / OpenGL": re.compile(r"^(al|wgl|gl)"),
        "Windows Multimedia (timer)": re.compile(r"^time(Begin|End|Get)"),
        "Registry": re.compile(r"^Reg"),
        "File I/O": re.compile(r"^(CreateFile|ReadFile|WriteFile|FindFirst|FindNext|GetTemp)"),
        "Keyboard / mouse hooks": re.compile(r"^(SetWindowsHook|GetAsyncKey|GetKey|RegisterRawInput|GetRawInput)"),
        "Threads / sync": re.compile(r"^(CreateThread|InitializeCritical|Sleep|WaitForSingle|ReleaseSemaphore)"),
        "Memory": re.compile(r"^(Virtual(Alloc|Protect|Free)|Heap(Alloc|Free)|Global(Alloc|Free))"),
        "DLL / module load": re.compile(r"^(LoadLibrary|GetModuleHandle|FreeLibrary|GetProcAddress)"),
        "Window / message": re.compile(r"^(CreateWindow|DefWindow|GetWindow|SetWindow|PeekMessage|GetMessage|DispatchMessage)"),
        "GDI / GDI2": re.compile(r"^(Create|Bitmap|TextOut|GetDC|ReleaseDC)"),
        "Process / thread": re.compile(r"^(GetCurrentProcess|GetCurrentThread|GetExitCode|ExitProcess|TerminateProcess)"),
        "Locale": re.compile(r"^(GetUserDefault|GetSystemDefault|MultiByteToWide|WideCharToMulti|LCMap|IsValidLocale)"),
        "Power management": re.compile(r"^SetThreadExecutionState"),
    }

    categorized: dict[str, list[tuple[str, str]]] = defaultdict(list)
    uncategorized: list[tuple[str, str]] = []

    for dll, fns in imports.items():
        for fn in fns:
            matched = False
            for cat, pat in patterns.items():
                if pat.match(fn):
                    categorized[cat].append((dll, fn))
                    matched = True
                    break
            if not matched:
                uncategorized.append((dll, fn))

    return {
        "categorized": {k: sorted(v) for k, v in sorted(categorized.items())},
        "uncategorized": sorted(uncategorized),
        "uncategorized_count": len(uncategorized),
    }


def main():
    if len(sys.argv) < 2:
        print("usage: survey_dll.py <path-to-dll>", file=sys.stderr)
        sys.exit(1)

    path = Path(sys.argv[1])
    if not path.exists():
        print(f"file not found: {path}", file=sys.stderr)
        sys.exit(1)

    data = survey(path)
    data["imports_categorized"] = categorize_imports(data["imports"])

    print(json.dumps(data, indent=2, ensure_ascii=False))


if __name__ == "__main__":
    main()
#!/usr/bin/env python3
"""Pretty-print summary of survey.json"""
import json
import sys

data = json.load(open(sys.argv[1], encoding="utf-8-sig"))

print("=== FILE INFO ===")
fi = data["file_info"]
for k, v in fi.items():
    print(f"{k}: {v}")

print()
print("=== SECTIONS ===")
for s in data["sections"]:
    print(f"  {s['name']:10s} vsize={s['virtual_size']:>10} rsize={s['raw_size']:>10} entropy={s['entropy']:.2f}")

print()
print("=== IMPORTS SUMMARY (DLLs, count) ===")
for dll, count in sorted(data["imports_count"].items(), key=lambda x: -x[1]):
    print(f"  {dll:35s} {count:>5}")
print(f"  TOTAL: {sum(data['imports_count'].values())}")

print()
print("=== EXPORTS COUNT ===")
print(f"  {data['exports_count']}")
print("  first 5:", [e["name"] for e in data["exports"][:5]])
print("  last 5:", [e["name"] for e in data["exports"][-5:]])

print()
print("=== VERSION INFO ===")
print(json.dumps(data["version_info"], indent=2, ensure_ascii=False))

print()
print("=== RESOURCES SUMMARY ===")
rs = data["resources_summary"]
print(f"Total: {rs['total']} bytes in {rs['total_count']} entries")
for t, info in sorted(rs["by_type"].items(), key=lambda x: -x[1]["total_bytes"]):
    print(f"  {t:30s} count={info['count']:>5}  bytes={info['total_bytes']:>10}")

print()
print("=== CATEGORIZED IMPORTS ===")
cat = data["imports_categorized"]["categorized"]
for c, items in sorted(cat.items(), key=lambda x: -len(x[1])):
    print(f"  {c:40s} {len(items):>4}")
print()
print(f"  UNCATEGORIZED: {data['imports_categorized']['uncategorized_count']}")
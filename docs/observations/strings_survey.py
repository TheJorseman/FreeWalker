#!/usr/bin/env python3
"""Extract and categorize readable strings from TMRealWalk.dll.

This only catalogs strings for the purpose of understanding the binary's
architecture (which subsystems exist, what user-facing labels it uses, what
files it expects). It does NOT reproduce licensing logic, encryption keys,
or any creative implementation. Strings are grouped by intent and only the
informative subset is shown.
"""
import re
import sys
from pathlib import Path
from collections import defaultdict

import pefile


# Minimum length to avoid noise
MIN_LEN = 6

# Patterns matching strings we WANT to keep (architectural hints).
# Each entry: (bucket_label, regex_pattern)
INTERESTING = [
    ("UI/tab/menu", r"(?i)\b(menu|tab|button|prompt|hud|overlay|widget|dialog|window)\b"),
    ("Truck/trailer", r"(?i)\b(truck|trailer|cab|cabin|kingpin|fifth[ _]wheel|chassis|axle|wheel|brake|air[ _]hose|landing[ _]gear)\b"),
    ("Player body", r"(?i)\b(head[ _]position|body[ _]state|hand|knocked[ _]out|dizzy|stagger|faint|fall|recover|crouch|jump|walk|run)\b"),
    ("Camera/render", r"(?i)\b(camera|view[ _]matrix|projection|render|present|swap[ _]chain|viewport|frustum|fov)\b"),
    ("SCS/game", r"(?i)\b(scs_|ets2|ats|euro[ _]truck|american[ _]truck|amtrucks|eurotrucks|truckersmp|convoy)\b"),
    ("Settings/config", r"(?i)\b(config|setting|ini|option|param|profile|preference)\b"),
    ("Fuel", r"(?i)\b(fuel|refuel|diesel|adblue|def[ _]|nozzle|petrol|pump|receipt|cap)\b"),
    ("Flashlight", r"(?i)\b(flashlight|torch|beam|spot[ _]light)\b"),
    ("Shadow", r"(?i)\b(shadow)\b"),
    ("Mouse/look/input", r"(?i)\b(mouse|look[ _]smoothing|raw[ _]input|key[ _]bind|controller|wheel|button|axis)\b"),
    ("Head tracker", r"(?i)\b(trackir|opentrack|trackhat|head[ _]tracker)\b"),
    ("Door/animation", r"(?i)\b(door[ _]anim|body[ _]motion|blink|swing|slam)\b"),
    ("Telemetry/sdk", r"(?i)\b(channel|event[ _]?handler|register[ _]channel|telemetry[ _]api|scs_[ _]log)\b"),
    ("PhysX", r"(?i)\b(physx|px[ _]scene|rigid[ _]body|controller[ _]manager|controller[ _]flags)\b"),
    ("Sound", r"(?i)\b(wav|ogg|sound|audio|voice|sfx|bank|stream[ _]?s)\b"),
    ("Game lifecycle", r"(?i)\b(start|stop|init|shutdown|connect|disconnect|register|unregister|attach|detach|load|unload|enter|exit|pump)\b"),
    ("Localization", r"(?i)\b(language|locale|translation|english|bahasa|malay)\b"),
    ("File extensions", r"\.(dll|exe|ini|log|txt|json|xml|csv|png|wav|ogg|mp3|fsf|pmd|pmg|tobj|mat|phys|bank|unit)\b"),
    ("Logging", r"^\[(?:TM|TMReal|Walker|license)\]"),
]

# Patterns of strings we DO NOT extract (avoid surfacing license logic, secrets,
# crypto details, anti-tamper heuristics, fingerprinting, telemetry endpoints).
SENSITIVE = [
    r"(?i)\b(gumroad|api[_-]?key|secret|token|license[_-]?key|fingerprint|hwid|machine[_-]?id|telemetry|auth)\b",
    r"(?i)\b(aes|rsa|hmac|sha|md5|bcrypt|password|salt|nonce)\b",
    r"(?i)\biv\s*=",
    r"https?://[^\s]+\.(com|net|org|io|ru|cn)",
    r"^\$",  # format strings are too revealing
]

# Generic non-informative strings (very common words that add noise).
NOISE = [
    r"^[a-z]{1,4}$",  # very short tokens
    r"(?i)^(?:error|warning|info|debug|trace)$",
    r"^[\.\-/\\:]+$",  # only punctuation
    r"^[\(\)\{\}\[\]]+$",
    r"^[\d\s\.\-]+$",  # only digits and punctuation
]


def is_interesting(s: str) -> bool:
    if len(s) < MIN_LEN:
        return False
    for pat in NOISE:
        if re.match(pat, s):
            return False
    for pat in SENSITIVE:
        if re.search(pat, s):
            return False
    for _, pat in INTERESTING:
        if re.search(pat, s):
            return True
    return False


def extract_strings(pe: pefile.PE) -> list[tuple[int, str]]:
    """Yield (offset, s) for printable ASCII/UTF-16 strings."""
    out = []
    data = pe.__data__
    n = len(data)

    # ASCII
    pat_ascii = re.compile(rb"[\x20-\x7e]{%d,}" % MIN_LEN)
    for m in pat_ascii.finditer(data):
        out.append((m.start(), m.group().decode("latin-1")))

    # UTF-16LE
    pat_utf16 = re.compile(rb"(?:[\x20-\x7e]\x00){%d,}" % MIN_LEN)
    for m in pat_utf16.finditer(data):
        b = m.group()
        try:
            s = b.decode("utf-16-le")
            out.append((m.start(), s))
        except Exception:
            pass
    return out


def main():
    if len(sys.argv) < 2:
        print("usage: strings_survey.py <path-to-dll> [--raw]", file=sys.stderr)
        sys.exit(1)
    path = Path(sys.argv[1])
    pe = pefile.PE(str(path))
    raw = "--raw" in sys.argv

    all_strings = extract_strings(pe)

    if raw:
        for off, s in all_strings:
            print(f"{off:08x}\t{s}")
        return

    interesting = [(off, s) for off, s in all_strings if is_interesting(s)]
    print(f"Total strings extracted: {len(all_strings)}")
    print(f"Interesting (after filters): {len(interesting)}")
    print()

    # Group by rough subsystem (using INTERESTING patterns with labels)
    buckets = defaultdict(list)
    for off, s in interesting:
        placed = False
        for label, pat in INTERESTING:
            if re.search(pat, s):
                buckets[label].append((off, s))
                placed = True
                break
        if not placed:
            buckets["other"].append((off, s))

    for label, items in sorted(buckets.items(), key=lambda x: -len(x[1])):
        print(f"=== {label}: {len(items)} strings ===")
        # Show only first 12 unique per bucket
        seen = set()
        n = 0
        for off, s in items:
            if s in seen:
                continue
            seen.add(s)
            print(f"  {off:08x}  {s[:120]}")
            n += 1
            if n >= 12:
                break
        print()


if __name__ == "__main__":
    main()
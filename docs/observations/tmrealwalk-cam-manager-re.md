# TMRealWalk camera manager reverse-engineering (M2 long-term)

Work-in-progress RE findings for the SCS dev free camera path in ATS
1.61.3.1s. Goal: enable the F10 "leave truck" transition to drive the
dev free camera directly via the camera_manager singleton + debug_camera
slot, bypassing the per-action controls.sii binding that blocks the
game_free_camera_shim approach.

This doc is intentionally narrow: just the strings / function locations
we've already pinned + the gaps we still need live debug to fill.

## TL;DR

- **Found**: `camera_manager`/`debug_camera`/`photo_camera`/`free_camera_u`
  string locations in `amtrucks.exe`, the per-string DJB2 hash functions
  that compute their registration hashes, and the concommand registration
  wrappers that bind them.
- **Found**: section alignment is `0x1000` while file alignment is
  `0x200`, so RVA = file_offset only inside `.text`; the camera string
  RVAs are all **off by `0x1200`** from the raw offset (they live in
  `.rdata`). Always use the PE-section-aware VA resolver
  (`rea/scripts/tmrealwalk-re/find_rip_refs.py`).
- **Not found without live debug**: the camera_manager singleton
  creator function (the function TMRealWalk's `'[camera] manager
  singleton pointer at exe+0x%llX (creator exe+0x%llX)'` log line
  references). The concommand struct fields are in the `.data` BSS
  extension (zero-initialised at load time), so we can't read them
  statically.

## String locations (PE-section-aware)

| Name             | File offset | RVA         | VA                |
| ---------------- | ----------- | ----------- | ----------------- |
| camera_manager   | `0x22547f8` | `0x22559F8` | `0x1422559F8`     |
| camera_manager_u | `0x22547e0` | `0x22559E0` | `0x1422559E0`     |
| debug_camera     | `0x225ACA8` | `0x225BEA8` | `0x14225BEA8`     |
| debug_camera_u   | `0x225AC98` | `0x225BE98` | `0x14225BE98`     |
| photo_camera     | `0x2311508` | `0x2312708` | `0x142312708`     |
| free_camera      | `0x2335628` | `0x2337828` | `0x142337828`     |
| free_camera_u    | `0x2335618` | `0x2337818` | `0x142337818`     |
| o_im_freecam     | `0x2258562` | `0x2259762` | `0x142259762`     |
| o_im_camera_cycle | (similar    | not pinned) | (not pinned)      |
| im_freecam       | (similar    | not pinned) | (not pinned)      |
| im_camera_cycle  | (similar    | not pinned) | (not pinned)      |

## Concommand registration flow

The DJB2 hash functions live at:

| Hash function VA      | `for` string         | Hash stored at .data VA |
| ------------------- | -------------------- | ----------------------- |
| `0x140044420`        | `camera_manager_u`   | `0x143667488`           |
| `0x140044D10`        | `debug_camera_u`     | similar (.data BSS)     |
| `0x140087080`        | `free_camera_u`      | `0x143667488` nearby    |

The `free_camera_u` block (`0x140087080`):
```
xor       eax, eax
lea       rdx, [rip + 0x2af78f]    ; "free_camera_u"
mov       cl, 0x66                 ; 'f'
.loop:
imul      eax, eax, 0x1f           ; eax *= 31
lea       rdx, [rdx + 1]
movzx     ecx, cl
add       eax, ecx
movzx     ecx, byte ptr [rdx]
test      cl, cl
jne       .loop
mov       dword ptr [rip + 0x35e03df], eax
ret
```

Note the DJB2 variant: `eax` is initialised to 0 but `cl` is set to the
**first character** before the loop, so the first iteration computes
`eax = 0*31 + 'f' = 'f'` and subsequent iterations use the previous char.
The standard DJB2 hash table is NOT used — none of these hash values are
stored as flat DWORDs in the binary.

After the hash function, `0x1400870b0` initialises the ConCommand struct
fields with 6 `mov [rip+disp32], rax` writes:

| Field offset | Struct field VA   | Source VA           |
| ------------ | ----------------- | ------------------- |
| `+0x00`      | `0x14366A600`     | `0x1435BCEB0` (.data) |
| `+0x08`      | `0x14366A5F8`     | `.data`             |
| `+0x10`      | `0x14366A5F0`     | `.data`             |
| `+0x18`      | `0x14366A5E8`     | `.data`             |
| `+0x20`      | `0x14366A5E0`     | `.data`             |
| `+0x28`      | `0x14366A5D8`     | `.data`             |

Then `0x1400871b0` is the wrapper that calls the registration function
`0x140158ed0` (strncpy) + the concommand-register tail-call
`0x141d1fa3c`.

The struct layout (Source-like):
```
struct ConCommand {
    void*          field0;   // +0x00 (typically next-pointer or class vtable)
    const char*     field1;   // +0x08 (command name)
    const char*     field2;   // +0x10 (help text)
    int            field3;   // +0x18 (flags)
    void(*fn)(...) field4;   // +0x20 (callback — what we want to call)
    void*          field5;   // +0x28 (linked list next / arg_size)
};
```

But all six source values are in `.data`'s zero-init trailing region, so
without live debug we cannot tell which field is which.

## TMRealWalk log evidence (camera_manager hierarchy)

TMRealWalk's strings reveal the manager's struct layout (offsets they
read from the manager pointer at runtime):

```
[camera] manager singleton pointer at exe+0x%llX (creator exe+0x%llX)
[camera] census: manager=%p field+0x10=%u field+0x14=%u slots=%llu
[camera]   slot %2llu: object=%p vtable=exe+0x%llX class=%s
[camera] could not find the camera manager creator
[camera]   debug_camera slot uses objects=...
[camera]   photo_camera slot uses objects=...
```

So the manager struct has:
- offset `0x10` (uint32): current slot index
- offset `0x14` (uint32): possibly flags
- offset `0x18+`: slot count or first slot pointer

And each slot has:
- object pointer
- vtable pointer
- class name (the strings we found — `camera_manager`, `debug_camera`, etc.)

## What's blocking M2 observable progress

The blocker is the **`camera_manager` creator function address**. Without
it, we cannot resolve the singleton and thus cannot toggle the dev free
camera from inside WalkerCore without the user binding an action.

The creator function must be findable via signature pattern, but the
specific instruction pattern that uniquely marks "this function returns the
camera_manager singleton" is not documented in TMRealWalk's strings. We
need either:

1. **Live debug session** to find the function via debugger, capture
   its VA, then reverse the signature for future builds; or
3. **Deeper static RE** to recognise the creator by its call-site
   pattern (the registration function `0x141d1fa3c` likely calls the
   creator somewhere).

## Workaround used by WalkerCore (commit `2cf1867`)

Until M2 lander is unblocked, WalkerCore patches `g_developer=1` in
memory (DevCameraCvarPatcher, RVA `0x223F6C0` of ATS 1.61.3.1s) so that
the dev free camera is **available** to the game. The user can:

- Press `~` and type `free_camera_u` to toggle manually; or
- Add a binding in `<profile>\controls.sii` that maps Numpad-0 to
  `free_camera_u` action (the action name still needs verification
  against the live action registry); or
- Wait for the M2 long-term lander (camera_manager resolver).

## References

- `rea/scripts/tmrealwalk-re/find_rip_refs.py` — PE-aware LEA ref finder
- `rea/scripts/tmrealwalk-re/find_freecam_button.py` — finds
  `o_im_freecam` button handler refs (none in static binary; the button
  is wired through the hash lookup path)
- `rea/scripts/tmrealwalk-re/find_scslike_hashes.py` — confirms the
  DJB2 variant and shows hash values are NOT stored as flat DWORDs
- `docs/observations/tmrealwalk-findings.md` — broader TMRealWalk
  architecture study
- PR: `2cf1867 feat(m2+m10): DevCameraCvarPatcher + comprehensive traceback + KO threshold fix`
- PRs for the 4 MinHook gates (`26f858a`, `7df03a1`, `f769506`,
  `0c109d1`) that switched off the hook path that crashed with
  #AM at RVA `0x1d5300a`
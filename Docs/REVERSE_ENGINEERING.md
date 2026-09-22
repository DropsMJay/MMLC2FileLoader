# Mega Man Legacy Collection 2 — Reverse Engineering Notes

Findings from Ghidra static analysis + dynamic testing (MinHook hooks)
done while building the mod loader. Serves as a reference for continuing
the investigation or understanding the game's internals.

**Binary analyzed:** `MMLC2.exe` (after removing the Steam DRM with
[Steamless](https://github.com/atom0s/Steamless) — the original `.exe`
ships protected by SteamStub, which blocks direct analysis in Ghidra).

**Image Base:** `0x140000000` (confirmed via `Window → Memory Map` in
Ghidra). All RVAs below are computed as `address - 0x140000000`.

---

## 1. `disc` file format

The `disc` file (no extension, ~1.4 GB) that stores every game asset is
a **ZIP with ZipCrypto** (`PK` signature at the start, visible in
7-Zip). Notable points:

- The ZipCrypto password only protects the *content* of each entry —
  file names in the central directory are plain text.
- This allows adding/replacing entries without needing to decrypt the
  existing ones (not used in the final mod loader, which prefers a hook
  based redirect, but tested successfully as an alternative "direct
  disc edit" mod on `logo.lzs`).
- The game keeps the `disc` open with a single handle from boot onward
  (seen via `CreateFileA("./disc")`, called exactly once).

---

## 2. System 1: `ResolveDiscResource`

**Function:** `FUN_14021de90` — RVA `0x21de90`

**Signature:** `undefined4 FUN_14021de90(longlong param_1)`
`param_1` is used directly as `char*` (resource name), no offset/struct.

**Covers:** system `.bin` files — fonts (`font_jp.bin`), menu/system
screens (`m_sys_en.bin`, `title_usa.bin`, `x360/gm_menu_en.bin`),
dialog boxes/UI (`msgbox.bin`, `wsc_nrm.bin`), stage select/shop
(`st_sel00_en.bin`, `shp.bin`, `m_shop_en.bin`).

**How it works internally:**
- Tries up to 6 candidate "base directories" (table at
  `PTR_s_rm09/game/media/bin/_1404b44d0`), building the path as
  `"DISC::" + base + resource_name`.
- Decides between reading from the packed `disc` or from a loose
  Windows file, based on the global flags `DAT_1407fb39f` /
  `DAT_140e1ab80` (the latter is, in practice, a **pointer to the
  mounted disc struct**, not a simple boolean — set once at boot,
  inside `FUN_14000a600`).
- **Does not return a pointer to the data.** It writes the read bytes
  straight into a **fixed global buffer**
  (`DAT_1409ca7e8 + 0x629000`, base pointer RVA = `0x9ca7e8`) and
  returns only the **size** read. Callers use that size to read the
  bytes from that fixed address.

**Discovery of `DAT_1409ca7e8 + 0x629000`:** seen directly in the
decompiled source (`lpBuffer = (LPVOID)(DAT_1409ca7e8 + 0x629000)`),
not through dynamic reverse engineering.

**Origin of the disc path/password:** `FUN_14000a600` decrypts an
obfuscated blob (`DAT_140449b7c`, XOR with `0x2b`) to get the path and
likely the password for the `disc`, used in `FUN_14000f4d0` to mount it.

---

## 3. System 2: `ResolveGeneric` (via `CTArcLoader`)

**Hooked function:** `FUN_14012df60` — RVA `0x12df60`

**Signature:**
`LPVOID FUN_14012df60(char *param_1, LPVOID param_2, DWORD *param_3)`
- `param_1`: path, usually prefixed with `"DISC::"`
- `param_2`: caller-provided buffer, or `nullptr` (function allocates one)
- `param_3`: output pointer for the size read (can be `nullptr`)

**Covers:** most of the moddable content — MM7 sprites/objects
(`rm07/obj/*.bin` — characters, bosses, items, effects), stage data
(`rm07/stage/st0/*.MAP/.MC/.ATT/.CODE`), menu/logo/title screens
(`menu/JP/logo.lzs`, `title.lzs`), art gallery (`illust/*.lzs`), text
localization (`message/rcc2_message_all_%s.lzs`), and also **MM8 data**
(`rm08/DATA/JP/*.PAC` — `STAGE00.PAC`, `PLAYER.PAC`, `COMNCHAR.PAC`,
etc).

**Why it's easier to hook than System 1:** it allocates fresh memory
and returns a new pointer (when `param_2 == nullptr`), instead of
writing into a pre-computed fixed buffer. The mod loader's redirect
allocates via `FUN_14013a3a0` (RVA `0x13a3a0`, the engine's own
allocator) to stay compatible with whoever later frees that memory.

**Internal logic** (same spirit as System 1):
```c
if (DAT_1407fb39f != 0 && DAT_140e1ab80 != 0 && strncmp(param_1,"DISC::",6)==0)
    return FUN_14000a910();  // read from the disc pack
else
    // CreateFileA + ReadFile directly with param_1 as a literal path
```

### 3.1 The async chain behind it (CTArcLoader)

Before reaching `FUN_14012df60`, requests go through an asynchronous
priority queue with up to 8 in-flight items, processed in the game's
main loop (`FUN_140146fd0`). Full mapped chain:

```
FUN_140022e50 / FUN_140023070 / etc.  (one per game: rc7, rc8, rc9, rc10)
  → builds the path TEMPLATE (e.g. "DISC::illust/muse_rc7_%03d.lzs")
  → part of a per-game "vtable" table at 140507dc0 (rc7=0, rc8=1,
    rc9=2, rc10=3), accessed via FUN_14003f870 (Museum UI controller)

FUN_140022780 (or a similar per-game function, e.g. FUN_140022e50
called again with the index) → substitutes %03d with the actual index,
builds the final path

FUN_14012e260 → copies the path into an object (offset +0xf8),
  allocates via FUN_14013a3a0(0x188), enqueues via FUN_14012ee70

FUN_14012ee70 → inserts the object into a priority queue (ordered by
  priority 1000)

FUN_14012e920 → runs in the main loop, processes up to 8 pending items
  at a time:
    FUN_14012e500 → kicks off loading via the "CTArcLoader::" class
      (literal class name seen in the code), registers the
      FUN_14012e080 callback
    FUN_14012e080 → callback: calls FUN_14012df60(path, ...) <- HOOK
      IS HERE, reads {size, data} and copies it into the object's buffer
```

**Real example of the logo's path:**
`FUN_14004bfb0` (the only xref to the `"logo.lzs"` string) builds the
path via `FUN_140144a10("logo.lzs")` and calls `FUN_14012e260` directly
— the same flow as the art gallery, but without going through the
`%03d` template layer (since the name is already fixed).

### 3.2 The `param_2 != nullptr` case: caller-provided buffers

Not every caller of `FUN_14012df60` passes `nullptr` for `param_2`.
Some pass a real destination buffer they already allocated. Two
different allocation patterns were found behind this:

- **`FUN_1402cfcc0`** (RVA `0x2cfcc0`, covers `rm10/bin/media/bin/*` —
  `obj_dat.bin`, `obj_fnt.bin`, etc): a **bump/arena allocator**. A
  shared pool (`DAT_140afd940`, an array of pointers) hands out the
  next free slot for each of up to 12 resources in sequence; after each
  read, the *next* slot's start address is recomputed as
  `current_slot + round_up_32(actual_size_read)`. Since this recomputes
  from whatever size the read reports, it's safe to report a **larger**
  size than the original (the mod loader whitelists this caller's RVA,
  `0x2cfe37`, to allow mods bigger than the original file).
- **`FUN_1402cf9d0`** (RVA `0x2cfaf6`, covers `rm10/bin/media/sprite/*`):
  a **reused scratch buffer** — confirmed by observing two different
  sprite files (`s_f_x00_en.bin`, `s_f_x01_en.bin`) resolve to the exact
  same destination address in the same session. This buffer has a real,
  unknown fixed capacity; writing past it would corrupt memory.

**Mod loader's policy for this case:** let the original function run
first (it always knows how to read correctly into that exact buffer,
and reports the real size used via `*param_3`). Only overwrite with a
mod if it's the same size or smaller — or, for whitelisted caller RVAs
confirmed to use the safe bump-arena pattern, any size. This is
universally safe regardless of which allocation pattern is behind a
given buffer, without needing to know its true capacity.

---

## 3.5. The shared low-level reader: `FUN_14000a910`

**Hooked function:** `FUN_14000a910` — RVA `0xa910`

**Signature** (derived directly from Ghidra's analysis of the function
body, x64 `__fastcall`):
`longlong FUN_14000a910(undefined8 param_1, undefined8 param_2, longlong param_3, int *param_4)`
- `param_1`: unused within the function body (context/reserved)
- `param_2`: resource path (same `"DISC::..."` string built by callers)
- `param_3`: destination buffer, or `0` to auto-allocate
- `param_4`: output size pointer (`int*`), can be `nullptr`

Both `FUN_14021de90` (System 1) and `FUN_14012df60` (System 2) call
this internally for the actual pack read — confirmed via
`References → Show References to Address`, which listed exactly these
two callers, plus two more inside a per-room "clone" of
`FUN_14021de90`'s pattern (`FUN_1402cfeb0`, discovered while
investigating audio — see §4.1). Every other resource-loading path
found so far (including MM7's `rm07/*` objects and MM10's `bin/*` and
`sprite/*` families) ultimately funnels through here too, one level
below the two named systems.

**Why hook it too:** it's a safety net. Any resource reached through a
caller this loader hasn't identified and hooked directly (like the
per-room clones) still passes through this single common function,
letting the mod loader catch it without needing to find and hook every
clone individually. Uses the same dual-mode redirect logic as System 2
(§3.2): auto-allocate when `param_3 == 0`, buffered-safe-size check
otherwise (no growth whitelist for this hook yet, since its own callers
haven't been individually vetted the way `FUN_1402cfcc0` was).

**Allocator note:** when `param_3 == 0`, the original function
allocates via `thunk_FUN_1403f4158(&DAT_1407f1120)`, a **different**
allocator than `FUN_14013a3a0` used elsewhere. Freed later via
`FUN_14013a5e0` regardless of origin, which is the same deallocator
paired with `FUN_14013a3a0` — suggesting the two allocators are
compatible (likely the same underlying implementation reached through
different call sites), though this was not independently confirmed.
The mod loader's hook reuses `FUN_14013a3a0`/`g_engineAlloc` for
consistency with the rest of the codebase; no issues were observed.

---

## 4. System 3: Audio (`.xwb` wave banks)

**Function:** `FUN_140140030` — RVA `0x140030`

**Signature:**
`ulonglong FUN_140140030(undefined8 param_1, char *param_2, undefined4 param_3, undefined8 param_4)`
- `param_2`: `.xwb` file name (e.g. `"rm08/DATA/sounds/BGM02.xwb"`)
- `param_3`: track ID inside the wave bank (e.g. `0x2A`)

**Covers:** music and sound effects for **every** game in the
collection (`system/`, `rm07/` through `rm10/`), not just MM8 as
initially suspected.

**Important:** this function **does not read any bytes on the spot** —
it only registers the request in a cache/slot (`FUN_140140100` checks
an existing cache, `FUN_140140280` allocates a new slot, both against a
32-slot array at `DAT_14093acc0`).

### 4.1 Investigation: where the actual read happens (inconclusive)

Extensive dynamic probing failed to find a name-based entry point for
individual `.xwb` files, ruling out every plausible Windows I/O API one
by one:

- **`CreateFileA`/`CreateFileW`** filtered on `.xwb` — never fires.
- **`CreateFileMappingA`/`MapViewOfFile`** — logged unconditionally
  during an audio-heavy session, never fires either.
- **`ReadFile`/`SetFilePointerEx` on the disc's own file handle**
  (captured once at boot via the `"./disc"` `CreateFileA` call) —
  fires constantly, but only from **two** call sites for the entire
  session, neither audio-specific:
  - RVA `0xDDE2`: thousands of 1-byte reads — the ZIP central
    directory parser (matches filenames byte by byte).
  - RVA `0xDD42`: ~12,000 chunked reads (up to 16 KB each, ~120 MB
    total) — the generic buffered zlib/deflate decompression reader,
    shared by every resource type, not audio-specific.

Following the call chain one level up from `FUN_140140030` didn't reach
a file read either:
```
FUN_140140030 → FUN_140140100 (cache check) / FUN_140140280 (slot alloc)
FUN_1402cfeb0 (a per-room "clone" of FUN_14021de90, e.g. for rm10/bin)
  → FUN_1403899a0 (reads a 4-byte header from FUN_1402cfeb0's result,
     delegates to FUN_1402cfb40 - not traced further)
```

**Conclusion:** wave bank data is very likely resolved through an
in-memory index/offset table built once at boot (from the ZIP central
directory already parsed for the `"./disc"` handle), rather than through
any of the name-based resolvers this loader hooks. Redirecting audio
would require locating and hooking that index structure directly (or
patching the zip's central directory in memory), which is a
substantially different — and unexplored — approach. **Not implemented.**

---

## 5. Video/cutscene system

**Files:** `resource/res000` through `res015` (at the game's root,
outside the `disc`), opened via `CreateFileA` directly (the only
confirmed case of a loose file natively used by the game outside the
mod system).

**Index → video mapping:** strings like
`"movie/%s/%s_CAPCOM15_Multi_0215.lzs"` inside the `disc` map a logical
cutscene name to a numeric index. Each `resXXX`'s internal header
contains the original video's name, obfuscated/encrypted on top
(confirmed by opening `res000` in a hex editor: it contained the
readable substring `JP_CAPCOM15_Multi_0215.wmv` amid otherwise
obfuscated bytes).

**Status:** not implemented in the mod loader — modding video would
require understanding the cipher layered on top of the container, not
just the container itself.

---

## 6. MM8 and legacy formats (`.PAC`, `.STR`, `.TIM`)

Initial (wrong) hypothesis: MM8 was thought to run through an embedded
PS1 emulator, given its use of native PS1 formats (`.PAC` container,
`.STR` video/stream, `.TIM` texture). **This was disproven**: MM8's
`.PAC` files go through the same `ResolveGeneric` (`FUN_14012df60`) as
every other game — there's no emulation, it's a native port that keeps
the legacy file names/format as a data container.

---

## 7. Index of mapped functions (RVA, from Image Base 0x140000000)

| RVA | Function | Role |
|---|---|---|
| `0x21de90` | `FUN_14021de90` | ResolveDiscResource (System 1) |
| `0x12df60` | `FUN_14012df60` | ResolveGeneric — actual read (System 2) |
| `0x12e260` | `FUN_14012e260` | ResolveGeneric — enqueues the request |
| `0x12ee70` | `FUN_14012ee70` | Inserts into the priority queue |
| `0x12e920` | `FUN_14012e920` | Processes the queue (up to 8 items), main loop |
| `0x12e500` | `FUN_14012e500` | Kicks off `CTArcLoader::`, registers callback |
| `0x12e080` | `FUN_14012e080` | Callback — calls the actual resolver |
| `0x140030` | `FUN_140140030` | Audio system (.xwb), cache/registration only |
| `0x13a3a0` | `FUN_14013a3a0` | Engine memory allocator |
| `0x9ca7e8` | `DAT_1409ca7e8` | Fixed buffer base pointer (System 1) |
| `0x146fd0` | `FUN_140146fd0` | Game's main loop (WinMain/message pump) |
| `0xa600` | `FUN_14000a600` | Mounts/opens the `disc` at boot |
| `0x22780` | `FUN_140022780` | Builds the final Museum path (rc7, substitutes %d) |
| `0x22e50` | `FUN_140022e50` | Builds the Museum path template (rc7) |
| `0x23070` | `FUN_140023070` | Equivalent to `FUN_140022e50` for rc10 |
| `0x3f870` | `FUN_14003f870` | Museum UI controller (game switching) |
| `0x4bfb0` | `FUN_14004bfb0` | Loads `logo.lzs` at boot |
| `0xa910` | `FUN_14000a910` | Shared low-level pack read, common to Systems 1 and 2 (§3.5) |
| `0x100590` | `FUN_140010590` | ZIP lookup/decompress dispatcher, called by `FUN_14000a910` |
| `0x2cfcc0` | `FUN_1402cfcc0` | `rm10/bin/media/bin/*` bump-arena buffered loader (§3.2) |
| `0x2cfaf6`/`0x2cf9d0` | `FUN_1402cf9d0` | `rm10/bin/media/sprite/*` reused scratch-buffer loader (§3.2) |
| `0x2cfeb0` | `FUN_1402cfeb0` | Per-room clone of `FUN_14021de90`'s pattern for `rm10/bin/media/bin/` (found during audio investigation, §4.1) |
| `0x3899a0` | `FUN_1403899a0` | Reads a 4-byte header from `FUN_1402cfeb0`'s result, delegates onward (audio-adjacent, not fully traced) |

---

## 8. Techniques used

- **String search** (`Search → For Strings` in Ghidra) to find path/
  resource name candidates.
- **References → Show References to Address** to find callers.
- **Dynamic hooks with MinHook**, including a temporary diagnostic hook
  on `FUN_140004f70` (a `sprintf`-like function used by both systems)
  that logged the **return address** (`_ReturnAddress()`) whenever the
  formatted result contained `.lzs` — this made it possible to jump
  straight to the caller function without manually hunting xrefs in
  Ghidra. This was the single most effective technique for unblocking
  stalled parts of the investigation.
- **Build marker in the log** (`BUILD_MARKER=...`) to confirm the
  latest compiled version was actually running in the game, after a
  few cases of stale builds being tested by mistake.
- **Capturing a specific handle at open time and filtering subsequent
  API calls by it** — used during the audio investigation (§4.1): the
  `disc`'s `HANDLE` was captured from its one-time `CreateFileA` call,
  then every `ReadFile`/`SetFilePointerEx` call elsewhere in the
  process was logged only when it matched that exact handle. This
  isolated disc-related I/O from the rest of the process's file
  activity, though in this case it led to a dead end (only two shared,
  non-audio-specific call sites were found).

# MMLC2 Mod Loader

A loose-file mod loader for **Mega Man Legacy Collection 2**. Drop a
modified file into a `mods/` folder using the game's own internal path,
and the loader transparently serves it instead of the content packed
inside the game's `disc` archive — no repacking or editing the original
archive required.

> Built through reverse engineering of the game's resource-loading
> functions with Ghidra and runtime hooks (MinHook). See
> [`REVERSE_ENGINEERING.md`](REVERSE_ENGINEERING.md) for the full
> technical writeup of how the loader works internally.

## Features

- **Drop-in replacement** — no need to touch the game's `disc` archive.
  Just mirror the internal path under `mods/` and the modded file is
  picked up automatically.
- **Covers most loadable content**: sprites and objects, stage data,
  menu/UI screens, fonts, localized text, and the art gallery — across
  every game in the collection (MM7 through MM11), including MM8's
  legacy `.PAC` data.
- **Safe fallback** — if no modded file is found for a given resource,
  the game reads from the original `disc` exactly as it always did.
  Nothing is modified on disk.
- **Zero external dependencies** — MinHook is statically linked, so the
  compiled `.asi` has no extra `.dll` to ship alongside it.
- **Optional debug logging, no rebuild required** — toggle two
  independent logs from an `.ini` file: one listing every resource the
  game requests (useful for discovering what's moddable), and one
  showing exactly which mods were picked up.

## Installation

1. Make sure the game already has an ASI loader installed (a proxy
   `d3d11.dll`, `dinput8.dll`, etc. — most Mega Man Legacy Collection 2
   mods rely on the [Ultimate ASI
   Loader](https://github.com/ThirteenAG/Ultimate-ASI-Loader)).
2. Download the latest release and drop `MMLC2ModLoader.asi` into the
   folder the ASI loader scans (usually the game's root folder, or a
   `scripts/` subfolder — check where your ASI loader looks).
3. Create a `mods/` folder next to the game's `.exe`.
4. Populate `mods/` with your modded files, mirroring the exact
   internal path the game uses for that resource (see below).

## Creating a mod

1. Set `LogAllFiles=1` in `mmlc2_modloader.ini` (see [Configuration](#configuration)),
   relaunch, and play through the area you want to mod. Check
   `mmlc2_all_resources_log.txt` for the exact resource path.
2. Extract the original file from the game's `disc` archive (it's a
   ZIP — 7-Zip opens it directly) using that same path.
3. Edit the content. Format-specific tools are required depending on
   the resource type — for example, `.lzs` art assets can be
   extracted/repacked with `quickbms` and the `sosfomt_dcmp.bms` /
   `sosfomt_lzs.bms` scripts.
4. Copy the edited, repacked file into `mods/<same relative path>`.
5. Launch the game with `LogModRedirects=1` and confirm an `[OK...]`
   line for your file in `mmlc2_mod_redirects_log.txt`.

Example — replacing the Mega Man 7 gallery art:
```
mods/illust/muse_rc7_002.lzs
```

Example — replacing the opening logo:
```
mods/menu/JP/logo.lzs
```

## Configuration

Debug logging is controlled by `mmlc2_modloader.ini`, created
automatically next to the `.exe` on first run:

```ini
[Debug]
LogAllFiles=0
LogModRedirects=1
```

No rebuilding needed — just edit the file and relaunch the game.

| Setting | Output file | Purpose |
|---|---|---|
| `LogAllFiles=1` | `mmlc2_all_resources_log.txt` | Deduplicated list of every resource requested this session — a live catalog of what can be modded. |
| `LogModRedirects=1` | `mmlc2_mod_redirects_log.txt` | Only the files actually replaced by a mod, or mod files found but failed to apply. |

Neither log file is created if its setting is `0`.

## Building from source

Requirements:
- Visual Studio (2022 or newer recommended), with the Desktop
  development with C++ workload.
- [vcpkg](https://github.com/microsoft/vcpkg), with MinHook installed
  statically:
  ```
  vcpkg install minhook:x64-windows-static-md
  vcpkg integrate install
  ```

Steps:
1. Open the project in Visual Studio.
2. Set the configuration to **Release / x64**.
3. In project properties, set `Target Extension` to `.asi` (or rename
   the resulting `.dll` manually after building).
4. Build. The output is a self-contained `.asi` with no runtime
   dependency on `minhook.x64.dll`.

## How it works (short version)

The game has two independent systems for loading resources from its
`disc` archive:

- A synchronous resolver covering fonts, system text and UI.
- An asynchronous, queue-based resolver (internally named
  `CTArcLoader`) covering sprites, maps, menu screens and the art
  gallery — used by every game in the collection, including MM8's
  legacy PS1-era container formats.

Both are intercepted with inline hooks, plus a lower-level hook on the
shared pack-read function they both eventually call, as a safety net
for resources reached through per-room "clone" functions. When a
matching file exists under `mods/`, its contents are served in place of
the original; otherwise the original code path runs unmodified. Full
details, including every mapped function address, are in
[`REVERSE_ENGINEERING.md`](REVERSE_ENGINEERING.md).

## Known limitations

- **Larger buffered mods** — some resources are read into a buffer the
  game already allocated. The loader always allows a same-size-or-
  smaller mod there; growing past the original size is only allowed for
  a short whitelist of call sites known to use a growable memory arena.
- **Audio (`.xwb` wave banks) isn't moddable.** Despite testing several
  Windows file-I/O APIs and tracing every read on the disc's file
  handle, no name-based entry point for individual audio files was
  found — see [`REVERSE_ENGINEERING.md`](REVERSE_ENGINEERING.md) for
  the investigation notes.
- **Video/cutscenes aren't moddable** — also documented in
  [`REVERSE_ENGINEERING.md`](REVERSE_ENGINEERING.md).

## Disclaimer

This is a fan-made modding tool. It does not distribute, modify, or
bypass protections on any copyrighted game assets — it only redirects
file reads to content you provide yourself. Mega Man, Mega Man Legacy
Collection 2, and all related assets are property of Capcom Co., Ltd.
This project is not affiliated with or endorsed by Capcom.

## License

This project is licensed under the [MIT License](LICENSE).

It statically links [MinHook](https://github.com/TsudaKageyu/minhook)
(BSD 2-Clause License). See
[THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md) for its full license
text.

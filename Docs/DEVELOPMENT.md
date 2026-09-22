# MMLC2 Mod Loader

`.asi` plugin (loaded by the Ultimate ASI Loader already installed as
`d3d11.dll` in the game folder) that lets you replace files from the
packed `disc` with loose versions, without editing the `disc` directly.

## How it works

The game has two resource-loading systems, both hooked:

- **ResolveDiscResource** — fonts, system text, UI (`.bin`)
- **ResolveGeneric** — sprites, maps, menu screens, art gallery,
  MM7/MM8 data (`.lzs`, `.bin`, `.PAC`)

When the game requests a resource (e.g. `illust/muse_rc7_002.lzs`), the
mod loader checks whether `mods/illust/muse_rc7_002.lzs` exists next to
the `.exe`. If it does, that file is served instead of the `disc`'s
content. If not, the game proceeds normally, reading from the `disc` as
usual.

A third system (audio, `.xwb` wave banks) is only cataloged, with no
redirect support.

## Build

1. Visual Studio → x64 DLL project, with MinHook installed via vcpkg
   (`vcpkg install minhook:x64-windows` + `vcpkg integrate install`).
2. `Configuration Properties → General → Target Extension` = `.asi`
   (or rename the generated `.dll` manually).
3. Build in **Release x64**.

## Installation

1. Copy the compiled `.asi` to the folder the Ultimate ASI Loader scans
   (either the `.exe`'s own folder, or the `scripts/` subfolder,
   depending on the install).
2. Create a `mods/` folder at the game's root, next to the `.exe`.
3. Inside it, recreate the `disc`'s folder structure for each file you
   want to replace. Confirmed examples:
   - `mods/illust/muse_rc7_002.lzs` — gallery art (Mega Man 7)
   - `mods/menu/JP/logo.lzs` — opening logo
   - `mods/rm07/obj/ROCKB.bin` — Mega Man sprite (MM7)
   - `mods/rm08/DATA/JP/PLAYER.PAC` — player data (MM8)

The path to use is always the same one shown in the resource log (see
below) — copy that exact relative path into `mods/`.

## Debug features (optional)

At the top of `dllmain.cpp`:

```cpp
static const bool DEBUG_LOG_ALL_FILES = false;
static const bool DEBUG_LOG_MOD_REDIRECTS = true;
```

- **`DEBUG_LOG_ALL_FILES = true`** → generates
  `mmlc2_all_resources_log.txt` in the game folder: a deduplicated list
  of every resource the game requested during the session, with a
  sequential number and which system loaded it. Useful for discovering
  which files exist and can be modded, without having to dig through
  the `disc` in 7-Zip.
  ```
  0001 [ResolveDiscResource] font_jp.bin
  0002 [ResolveGeneric] menu/JP/logo.lzs
  0003 [ResolveGeneric] rm07/obj/ROCKB.bin
  ```

- **`DEBUG_LOG_MOD_REDIRECTS = true`** → generates
  `mmlc2_mod_redirects_log.txt`: shows only the files that were
  actually replaced by a modded version (`[OK]`), or where a mod file
  was found but failed to read (`[FAIL]`). Files with no mod aren't
  listed (avoids a huge log during normal play).
  ```
  [OK] "menu/JP/logo.lzs" <- "E:\...\mods\menu\JP\logo.lzs" (388288 bytes)
  ```

Flip `false`/`true` as needed and rebuild. No log file is created if the
corresponding flag is off.

## Workflow for creating a mod

1. Temporarily enable `DEBUG_LOG_ALL_FILES`, play a bit around the area
   you want to mod, and check the log to find the exact resource path.
2. Extract the original file from inside the `disc` (7-Zip) using that
   same path.
3. Edit the content (format-specific tool — for example, `quickbms`
   with the `sosfomt_dcmp.bms`/`sosfomt_lzs.bms` scripts for `.lzs`
   files).
4. Repack it and copy it to `mods/<same relative path>`.
5. Run the game with `DEBUG_LOG_MOD_REDIRECTS` enabled and confirm the
   `[OK]` line in the log.

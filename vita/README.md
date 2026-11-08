# Rainbow Six: Shadow Vanguard for PS Vita

Runs the Android (Xperia Play) build of the game natively on the Vita: the
loader puts the game's ARM library in memory, connects its imports to Vita
code and emulates the Java side. It shares its core with the PC loader in
`../src`.

## What you need on the Vita

- HENkaku / Ensō.
- **kubridge.skprx** in `ur0:tai/` with a line `ur0:tai/kubridge.skprx`
  under `*KERNEL` in `ur0:tai/config.txt`, then reboot.
- **libshacccg.suprx** extracted with ShaRKF00D (it ends up in `ur0:data/`).
  vitaGL compiles its rendering shaders on the console with it.

## Installing

1. Install `r6sv.vpk` with VitaShell.
2. Copy the extracted APK: the contents of `apk/xperia-play/` go to
   `ux0:data/r6sv/apk/`, so that the loader finds
   `ux0:data/r6sv/apk/lib/armeabi-v7a/librainbowsix.so`.
3. Copy the game data. The easy way: start USB mode in VitaShell, then drag
   the data archive (`.rar`/`.zip`/`.7z`, any device's) or the extracted data
   folder onto `Vita-Install-Data.bat` in the `port` folder. It unpacks the
   archive, keeps the data already on the card as a `gamedata-<date>` backup,
   copies the new data in the layout the Vita needs, carries your saves over,
   checks the copy and puts `r6sv.vpk` on the card root to install. Same from a
   terminal: `python tools/vita_install_data.py SOURCE [H:]`.

   By hand: copy the game data **flattened** (see below). With the Vita's memory card
   mounted as, say, `H:` (VitaShell USB mode), from the `port` folder:

   ```
   python vita/flatten_data.py gamedata/GloftR6HP H:/data/r6sv/gamedata
   ```

   If the data was already copied as-is, rearrange it on the card instead:
   `python vita/flatten_data.py --in-place H:/data/r6sv/gamedata`
4. Start the game from the LiveArea. If it still shows the default rainbow
   background after installing over an older version, refresh it: VitaShell,
   △ on the main screen, "Refresh LiveArea", then reboot.

The Vita can't open files more than 9 folders below `ux0:`, and the game's
data goes 9 folders deep by itself, so on the Vita every file's folder path
is stored as a single folder with `~` for `/` (for example
`data~3d~characters~US~Textures/head_fed.tga`). The loader translates the
game's paths to match.

Each run writes `ux0:data/r6sv/log.txt`. If the game crashes, the end of the
log has the registers, the crash address inside the game library
(`lib+xxxxxx`) and the last file operations; send that file along with a
description. An empty file `ux0:data/r6sv/no_crash_handler` turns the crash
report off.

If the game reports a missing file or crashes while loading, compare the card
with the PC copy; a forced power-off can damage files on the memory card, and
so can leaving VitaShell's USB mode without ejecting the card in Windows first
(Windows may write back an out-of-date copy of the card's folders).

## Controls

| Vita            | Game (Xperia Play)     |
|-----------------|------------------------|
| Left stick      | move (left touchpad)   |
| Right stick     | look (right touchpad)  |
| Cross / Circle / Square / Triangle | the same buttons |
| L / R           | L1 / R1                |
| D-pad           | D-pad                  |
| Start           | Start                  |
| Select          | Back (pause menu)      |
| Front touch     | touch screen           |

### Other device builds

The Desire HD, Optimus 2X, Galaxy S and Droid X builds also work. They have
no gamepad mode, so the loader drives their on-screen controls instead: the
left stick works the game's virtual stick (hidden, and touching its spot does
nothing), the right stick drags the view, and these buttons press HUD buttons
that have no key. The HUD controls only act during gameplay, not in menus.

| Vita      | Game (on-screen HUD)                              |
|-----------|---------------------------------------------------|
| Triangle  | grenade                                           |
| L         | aim down sights (also while moving: you stop briefly) |
| Circle    | its usual action, and sprint while held           |

R fires and Square changes stance; the rear touchpad does nothing on these
builds. Any build also works with any build's data, so the data on the card
can come from any device. To switch builds, replace
only `ux0:data/r6sv/apk/lib/armeabi-v7a/librainbowsix.so` with that build's
(from `apk/<build>/lib/armeabi-v7a/`). Keep the Xperia one to switch back.
The log (`ux0:data/r6sv/log.txt`) says which controls are active.

## Building

Needs VitaSDK with the vitaGL, vitaShaRK, kubridge and libmathneon packages
(`vdpm install vitaGL vitaShaRK kubridge libmathneon`). From the `port` folder
on Windows:

```
wsl bash vita/build.sh
```

The result is `vita/r6sv.vpk`.

The LiveArea images in `vita/sce_sys` (bubble icon, background, startup
gate) come from the game's own art: the APK's launcher icon and the title
splash texture (`R6SV_DUMPTEX=e` on the PC loader dumps it; crop the top-left
854x480). `python vita/make_livearea.py ICON SPLASH` regenerates them.
Being game art, they are not in the repository; without them the vpk is
built with the default bubble.

## How it differs from the PC loader

- No JIT: the Vita has the same kind of CPU the game was built for.
- Imports go through a small assembly trampoline (`src/thunks.S`) that saves
  the caller's registers, so the PC loader's HLE code (which reads the game's
  softfp arguments from registers) works unchanged. ABI-compatible functions
  like `memcpy`, `malloc` and the `__aeabi_*` helpers are bound directly
  (`src/native_direct.c`).
- The game renders with its OpenGL ES 2.0 shaders. vitaGL's own GLSL
  translator gets their matrix math wrong, so the loader translates them to Cg
  itself (`../src/glsl_to_cg.cpp`) and hands vitaGL Cg shaders. Empty files in
  `ux0:data/r6sv/` change this: `es1` makes the game use its fixed-function
  ES 1.1 renderer instead (flat lighting, but no shaders), `cg_dump` saves
  every translated shader to `ux0:data/r6sv/cg/`, and `cg_mul_alt` flips the
  matrix multiplication order. On PC, `r6sv --glsl2cg IN OUT` translates
  shaders dumped with `R6SV_GLTRACE=1` the same way.
- The game is told it renders at 854x480, like the Xperia Play; viewport and
  scissor are scaled to the 960x544 screen. Other sizes make the game switch
  to its touch-screen HUD.
- ATC textures are decoded on the CPU, as on PC.
- The fixed-function renderer takes a weapon's reflectivity from the diffuse
  texture's alpha, which most weapon textures don't have, so guns came out as
  pure mirror. Such draws get a fixed 30% reflection instead (`src/ffp_fix.cpp`,
  shared with the PC loader's ES 1.1 mode).

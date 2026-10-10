# DSCS Player Model Swap

A [DSCSModLoader](https://github.com/SydMontague/DSCSModLoader) plugin for **Digimon Story Cyber Sleuth: Complete Edition** (Steam) that lets you play as a Digimon.

- Field: the protagonist is replaced by any Digimon model (`chrNNN`). Its movement collision is sized for the Digimon automatically.
- Missing animations: most Digimon have no field idle/walk/run motions, so their battle idle/run motions are used instead. Animations a model does ship (including modded ones) are always preferred.
- Cutscenes and menus (optional): the protagonist model is replaced wherever it is loaded. Cutscene-only gestures fall back to the battle idle.
- In-game picker (**F7**): a searchable list of every Digimon model in your game data, with localized names. Picking one applies immediately in the field and is saved to the INI.
- **F8** switches temporarily back to the human model.
- Saves are never modified. Remove the plugin and you are the human protagonist again.

Requires the signature-scanning build of the loader: [jfmherokiller/DSCSModLoader `feature/signature-scanning`](https://github.com/jfmherokiller/DSCSModLoader/tree/feature/signature-scanning). All game functions are found by byte signatures, not fixed addresses.

## Install

1. Install the loader above.
2. Copy `DSCSPlayerModelSwap.dll` to `<game>\resources\plugins\`.
3. Copy `PlayerModelSwap.ini` to `<game>\resources\`.

The loader tries to load every file in `resources\plugins\`, so don't keep backups there. SimpleDSCSModManager only packs data files, so the plugin won't appear in its mod list.

## In-game menu

Press **F7** (`MenuKey`) to open it, and F7 or Esc to close it. While it's open the game ignores keyboard, mouse and pad input.

- Click a row to become that Digimon. Choose **Protagonist (no swap)** to go back to the human.
- **Field anims = yes** means the Digimon ships its own field motions. Others walk with battle motions.
- *Enabled*, *Human for now* and *Also in cutscenes* mirror the INI switches.

Cutscene and menu models pick up a change the next time they load.

## Configuration (`resources\PlayerModelSwap.ini`)

| Key | Default | Meaning |
|---|---|---|
| `Model` | `chr090` | Digimon model code (`chr` + 3 digits); empty = vanilla. The menu writes this. |
| `Enabled` | `1` | Master switch. |
| `RemapFieldMotions` | `1` | Substitute battle motions for missing field motions. |
| `ReplaceInCutscenes` | `1` | Also replace the protagonist in cutscenes and menus. |
| `TemporaryHumanKey` | `119` (F8) | Decimal virtual-key code; `0` disables. |
| `MenuKey` | `118` (F7) | Decimal virtual-key code; `0` disables the menu. |
| `LogScriptErrors` | `1` | Write Squirrel script errors to `DSCSPlayerModelSwap.log`. |

Squirrel scripts can also call `PlayerModelSwap.SetModel("chr090")`, `GetModel()` and `SetEnabled(bool)`.

## Build

Requires Visual Studio 2022 (C++ workload), CMake 3.21+ and Ninja. Then run:

```bat
build.bat
```

Dependencies are fetched by [CPM](https://github.com/cpm-cmake/CPM.cmake): DSCSModLoader (pinned commit), MinHook 1.3.3 and Dear ImGui 1.92.9b. To build against a local loader checkout, `set DSCSML_SOURCE=<path>` first. Output: `build\DSCSPlayerModelSwap.dll`.

## Known limitations

- Battle-motion substitutes can look like sliding while walking.
- Interaction and collision ranges are the Digimon's, not the human's.
- The menu needs the game's OpenGL 3+ context. If it fails to start, the log says so and the rest of the plugin keeps working.

## License

GPL-3.0 (see `LICENSE`), matching DSCSModLoader. Not affiliated with Bandai Namco or Media.Vision.

A fork of QuickArmorRebalance for Skyrim Special Edition with improved mod browsing, equipment and armor management.

## Nearby NPC outfits

Enable **NPC Detection** in the main window, choose an NPC in **Target**, then use the existing outfit/item controls. Choose **Player** to return to player equipment. **Save NPC** writes that placed NPC's equipped outfit to a separate shared file; **Outfit Persistence** reapplies saved outfits nearby even with the UI closed.

Both toggles default to off. Temporary spawned NPCs can be dressed but cannot be saved across characters. See [NPC outfit behavior and in-game testing](docs/npc-outfits.md) for scope, limitations, file locations, and the verification checklist.

Use **Detection Range** to adjust the radius from 512 to 16,384 game units (default 4096). The setting is saved automatically; larger ranges still only include loaded NPCs.

## Building

Initialize the pinned CommonLibSSE-NG dependency after cloning or switching to this branch:

```powershell
git submodule update --init --recursive
```

Use a Visual Studio 2022 x64 developer shell with CMake, Ninja, and `VCPKG_ROOT` configured. The presets use the dynamic MSVC runtime (`x64-windows-static-md`), matching upstream's CommonLib configuration. Use a fresh build directory when migrating from the older static-runtime build:

```powershell
cmake --preset release -B build/upstream-release -DOUTPUT_FOLDER:PATH=
cmake --build build/upstream-release --parallel 6
```

The DLL is written to the selected build directory. `OUTPUT_FOLDER` optionally enables copying the DLL into `<OUTPUT_FOLDER>/SKSE/Plugins`; leave it empty to build without deploying. It can also default from `SKYRIM_FOLDER` or `SKYRIM_MODS_FOLDER`.

See [the upstream integration checks](docs/upstream-integration.md) before promoting the merge to `main`.

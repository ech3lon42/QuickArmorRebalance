# Upstream integration

This feature branch merges Xinaths/QuickArmorRebalance through `db59088` into the custom fork at `e8123d7`. It records all 50 upstream commits in the merge history and pins CommonLibSSE-NG to `2fcadbdf5e7fe65580ca5848d6604e5b88fab03b`.

## Integration decisions

- Retain outfit creation, rename, tags, enchantment/stat transfer, tracked-item protection, favorites, table sorting/columns, adjustable font/window sizes, and the existing keyboard shortcuts.
- Preserve the enhanced-armor cosave format, callbacks, and dynamic-form guards.
- Adopt upstream's cosmetic slots, updated renderer/input accessors, and new CommonLib API. Both Alt+double-click and bracket cycling use the same matching-set helper with upstream's limit to one candidate per slot.
- Preserve the custom numpad mappings where they differ from upstream's item navigation shortcuts.
- Build CommonLib from its pinned submodule when a compatible prebuilt binary is unavailable. Include its CMake plugin helper explicitly for source builds and declare its required `rapidcsv` dependency.
- Make deployment optional instead of hardcoding a local mod directory. The integration build does not overwrite the installed plugin.

## Verification performed

- Debug and Release configured and compiled successfully with MSVC 19.44.35222.0, including a source build of the pinned CommonLib version. Release correctly fell back to source because the published prebuilt library targets a newer MSVC toolset.
- Outputs: `build/upstream-debug/QuickArmorRebalance.dll` and `build/upstream-release/QuickArmorRebalance.dll`. Both builds used an empty `OUTPUT_FOLDER` and did not deploy to the game.
- Release exports verified with `dumpbin`: `SKSEPlugin_Load`, `SKSEPlugin_Query`, and `SKSEPlugin_Version`. Its dependencies include the MSVC runtime DLLs, as expected from the updated presets.
- No unresolved conflict markers or added whitespace errors. The cosave implementation/header, outfit data structures, plugin initialization callbacks, and enhanced-enchantment implementation are unchanged from the custom fork.
- Builds report warnings in existing code: one float-to-integer conversion, two shadowed variables, and an unused serialization callback parameter. No in-game tests have been performed.

## In-game regression checklist

Compilation cannot validate Skyrim hooks, inventory operations, or cosave restoration. These checks remain manual; use a separate mod-manager test profile and a copy of an existing save before promoting this branch to `main`.

1. Start Skyrim through the matching SKSE loader. Check the QAR log for successful initialization, then open/close `qar`, test pause and camera passthrough, and reopen the UI.
2. Verify existing favorites, tags, outfits, item blacklist, and table-column settings load. Change a setting, restart, and confirm it persists.
3. Sort/filter the item list and exercise click, Ctrl/Shift selection, numpad item actions, and protected-item removal. Confirm the currently worn list handles enhanced dynamic forms.
4. Cycle mods with `[` and `]`, including both list boundaries and a mod search filter. Verify Body -> Feet -> Hands -> other occupied slot fallback and matching-set equipment. Check that typing brackets into fields and opening a modal suppress these shortcuts.
5. Compare Alt+double-click with bracket cycling. Use a mod with several equivalent pieces for one slot and confirm only one candidate for that slot is given/equipped. Shift+Alt+double-click should select the corresponding limited set.
6. Create an outfit normally and with Shift+Create Outfit / Ctrl+Shift+C. Verify the mod-name suggestion, widened prefix field, rename, tags, and numpad outfit cycling/restore.
7. Apply an enchantment and transferred stats to an outfit piece, equip it, save, exit Skyrim, and reload. Verify the recreated armor, stats, enchantment effects, and equipped state; inspect serialization logs for errors or duplicated effects.
8. Increase the font to 32 and resize the main/secondary windows on a large display. Confirm settings and readable layout persist.
9. Rebalance a normal armor set and verify crafting/tempering, slot remapping, custom keywords, and distribution settings still work. Mark a supplemental slot cosmetic: items using only cosmetic slots should receive zero rating, weight, and value. Ctrl-click the cosmetic checkbox and reopen/restart to verify the default persists.
10. Repeat startup, rendering/input, equip, and save/load checks on each Skyrim runtime this fork intends to support. The new dependency includes upstream's Skyrim 1.7.99 support, but this checklist is not a claim of runtime testing on any version.

# Nearby NPC outfits

## Use

1. Enable **NPC Detection** in the new row at the top of the main UI.
2. Choose an NPC from **Target**. Entries are sorted by distance and include the runtime reference ID, so identical names remain distinguishable. **Player** is an explicit option.
3. Use the existing outfit application, give/equip/toggle, unequip/restore, undo, and removal controls. Worn-item lists, slot/conflict indicators, equipment capture and inventory-based stats sources use the selected actor too.
4. Once the equipment has settled, click **Save NPC**. Capture is delayed briefly behind queued equipment actions. It saves the NPC's actual equipped armor, weapon hands and ammo, not merely the outfit selected in the list. Saving again updates that NPC's file.
5. Enable **Outfit Persistence** to reapply saved NPC outfits nearby, including after a game load and with QAR's UI closed.

The two switches default to off and are stored as `settings.npcDetection` and `settings.npcPersistence`. Persistence requires detection; turning detection off turns persistence off and returns the equipment target to Player. Disabling persistence stops future applications but does not undo equipment already applied or delete any saved files.

The **Detection Range** slider below the targeting controls accepts **512–16,384 Skyrim game units**, with **4096** as the default. **Reset Range** restores the default. The range can be configured with detection off and is saved as `settings.npcDetectionRange` when you finish editing (or close QAR mid-edit). Missing settings use the default; out-of-bounds values are clamped. Range changes affect selection, queued-action validation and automatic outfit application immediately; discovery refreshes within one second. Increasing the range does not load additional NPCs or bypass cell/worldspace restrictions. Shrinking the range can end an NPC's nearby editing session, just like walking away.

## Identity and storage

Files are shared across **all player characters** using this mod installation, as requested. They are stored under:

```text
Data/SKSE/Plugins/QuickArmorRebalance/npc_outfits/NPC_<stable-reference-hash>.json
```

Each versioned JSON file contains the NPC's name, placed reference (`Plugin.ext:0xLocalID`), base reference and equipment recipes. The deterministic filename is derived from the placed reference, not a display name, load-order-dependent full ID, or NPC base template. The embedded identity is checked before overwriting; a hash collision cannot overwrite another NPC's file. Temporary-file writes are checked before replacement.

Light plugins are supported by the existing local-form-ID helpers. If an item/plugin is missing, the complete outfit is skipped before changing equipment. Malformed/oversized files are rejected. External edits to these files are read when Skyrim starts. To forget an outfit, move its JSON file out of this folder while Skyrim is closed; keep it as a backup if desired.

## Safety and behavior

- Discovery runs at most once a second, using only high-process actor handles. It does not enumerate all world references or all game forms. There is at most one scheduled scan, and automatic application considers at most two saved NPC outfits per scan, rotating fairly through nearby actors.
- Candidates must be living, enabled, loaded NPCs with an active process and the `ActorTypeNPC` keyword, outside kill moves, within the configured detection range (default 4096 game units). Interiors must match the player's cell; exteriors must share a non-null worldspace. Animals/creatures without the NPC keyword are excluded.
- Selection stores a handle. The UI holds a strong reference for one frame; game-thread equipment tasks resolve the captured handle again and revalidate it. An unavailable selected NPC does **not** redirect queued actions to the player. Load/revert transitions invalidate old tasks and selections.
- No automatic application while the actor is in combat or an inventory menu is open. The selected NPC is also left alone while the UI is open. User equipment edits suppress automatic repair for that NPC until Save NPC is used or the NPC leaves the nearby session; this prevents persistence immediately undoing a preview.
- NPC removal/undo tracks only quantities given through the UI during the current nearby session. It does not use the player's cross-session item tracking or Keep marks. Original NPC outfit records and shared ActorBase outfit definitions are not changed. Automatically restored equipment is not added to the UI undo ledger.
- A valid placed NPC can be saved; dynamically spawned references (`FFxxxxxx`, including many leveled spawns) can be dressed but have no safe cross-character identity, so Save NPC is disabled for them. Two instances with the same base/name do not inherit each other's files.
- QAR-created enhanced armor is saved as stable base forms plus enchantment/stat recipes and recreated for the NPC. NPC enhancements do not enter the player's cosave equipment list. Unknown dynamic equipment causes Save NPC to fail without partially replacing the existing file. Instance-specific tempering/custom player enchantments, spells and consumable stack quantities are not copied.
- Weapons are best-effort: combat AI, quests, followers and other outfit managers can choose different weapons or reset clothing. This implementation does not replace AI packages or force ActorBase outfit changes. Cross-mod behavior needs in-game testing.
- The new target governs **equipment operations**. Existing rebalance edits to shared armor/weapon forms, recipes, distribution settings and outfit-library files retain their existing global semantics.

## Verification

Debug and Release compilation and the standalone policy checks can be run outside Skyrim. These do not establish runtime safety, FPS cost or compatibility with other plugins.

From a VS 2022 x64 developer PowerShell, the standalone checks require no Skyrim runtime:

```powershell
New-Item -ItemType Directory -Force build/tests | Out-Null
cl /nologo /std:c++20 /EHsc /W4 tests/NPCOutfitRulesTests.cpp /Fo:build/tests/NPCOutfitRulesTests.obj /Fe:build/tests/NPCOutfitRulesTests.exe
if ($LASTEXITCODE -ne 0) { throw 'Compile failed' }
./build/tests/NPCOutfitRulesTests.exe
```

The 29 checks cover default/custom distance boundaries, range clamping, non-finite coordinates, interior/exterior isolation, absent cells/worldspaces, deterministic reference-based filenames, distinct placed NPC identities, and path-safe filenames. They exercise the same policy helpers used by the plugin, not mocked engine calls.

### In-game checklist (not yet executed)

**NPC-selection crash retest (AE 1.6.1170):** the reported Matlara crash was traced to the actor ammo accessor returning `0x1`, which the worn-list validator then dereferenced. All five actor ammo reads now use a shared, null-guarded equipment-process lookup that verifies the object's ammo type. Retest selecting Matlara with the worn list active, then a bow user with arrows, and switching back to Player. Check outfit capture, Save NPC and persistence with and without equipped ammo. This bypasses the observed failing accessor; runtime confirmation is still required.

Use a disposable save and keep a backup of any existing NPC outfit files.

1. With both switches off, verify all existing player outfit actions, hotkeys, undo/removal, Keep marks and enhanced-armor save/load still work.
2. With the game paused by QAR, enable detection in an occupied interior. Verify a list appears, distances/reference IDs are visible, and Player remains the default target. Check an empty room and a crowded exterior; measure FPS/frame time with detection off, on, and persistence enabled, UI both open and closed.
3. Select NPC A, apply regular/enhanced outfits, use single-item double-click, Give, Equip, Unequip, Restore, Undo and Delete Given/Remove Tracked. Confirm the player and nearby NPC B remain unchanged. Confirm worn lists, slot indicators and stats-source inventory follow the target.
4. Switch repeatedly between Player/A/B while clicking outfit actions. Each queued action must stay with the actor selected when requested; no earlier actor's undo or selections may affect the new actor.
5. Move A beyond range, teleport to another cell, disable A, or kill A. Selection must show unavailable and equipment operations must not fall back to Player. Test re-enabling/returning A and manually selecting Player. Do not save over a real playthrough after destructive console tests.
6. Save two placed NPCs sharing a name/template with different outfits. Confirm separate files and correct contents. Restart Skyrim and load the same character; then load another character visiting the same placed NPCs. Confirm distinct outfits apply to each. Also verify after changing a plugin's load-order position.
7. Test a placed NPC from an ESL/light plugin and a temporary spawned NPC. Only the placed NPC should have Save NPC enabled. Spawned NPC equipment actions should still work.
8. Save NPC, immediately close the UI, and wait: the file must still be written with persistence off. Test two-handed weapons, dual-wielded matching weapons, a shield, ammo, an empty armor outfit and slot-conflicting outfit items.
9. Repeatedly load saves in the same Skyrim process with an enhanced NPC outfit. Check armor stats, appearance, enchantment effects and inventory counts for missing gear, duplicate additions and stacked effects. Confirm no NPC enhancement appears on the player.
10. With persistence enabled, edit the selected NPC without saving. Confirm the preview is not immediately reverted. Close the UI, leave range and return; the last saved outfit should return. Save the changed outfit and repeat. Turning either toggle off must stop automatic changes without deleting files.
11. Restart with a referenced equipment plugin missing, an invalid JSON file, a wrong field type, an unsupported version and an unwritable save directory. Confirm no crash, partial outfit application, stripped NPC, or loss of an existing valid file. Check the SKSE plugin log and UI status.
12. Test combat, follower outfit mods, quest NPCs, cell reset, fast travel, main-menu/new-character transitions and loading while commands are pending. Confirm scans/actions resume after loading and previous-character work is discarded. Test supported SE/AE/VR runtimes separately where available.
13. Adjust Detection Range with detection both off and on. Check minimum/maximum values, Ctrl-click numeric entry, Reset Range and restart persistence. Shrink below the selected NPC's distance: actions must become unavailable without falling back to Player. Expand again and verify the list refreshes within one second. Confirm persistence follows the new radius with the UI closed, and that closing QAR during a slider drag still saves the setting.

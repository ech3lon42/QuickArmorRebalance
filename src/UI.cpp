#include "UI.h"

#include "ArmorChanger.h"
#include "ArmorSetBuilder.h"
#include "Config.h"
#include "Data.h"
#include "ImGuiIntegration.h"

using namespace QuickArmorRebalance;

static ImVec2 operator+(const ImVec2& a, const ImVec2& b) { return {a.x + b.x, a.y + b.y}; }
static ImVec2 operator/(const ImVec2& a, int b) { return {a.x / b, a.y / b}; }

bool StringContainsI(const char* s1, const char* s2) {
    std::string str1(s1), str2(s2);
    std::transform(str1.begin(), str1.end(), str1.begin(), ::tolower);
    std::transform(str2.begin(), str2.end(), str2.begin(), ::tolower);
    return str1.contains(str2);
}

void MakeTooltip(const char* str, bool delay = false) {
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | (delay ? ImGuiHoveredFlags_DelayNormal : 0)))
        ImGui::SetTooltip(str);
}

void RightAlign(const char* text) {
    float w = ImGui::CalcTextSize(text).x + ImGui::GetStyle().FramePadding.x * 2.f;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - w);
}

struct GivenItems {
    void UnequipCurrent() {
        if (auto player = RE::PlayerCharacter::GetSingleton()) {
            for (auto& item : player->GetInventory()) {
                if (item.second.second->IsWorn() && item.first->IsArmor()) {
                    if (auto i = item.first->As<RE::TESObjectARMO>()) {
                        if (((unsigned int)i->GetSlotMask() & g_Config.usedSlotsMask) ==
                            0) {  // Not a slot we interact with - probably physics or such
                            continue;
                        }

                        RE::ActorEquipManager::GetSingleton()->UnequipObject(player, i, nullptr, 1, i->GetEquipSlot(),
                                                                             false, false, false);
                    }
                }
            }
        }
    }

    void Give(RE::TESBoundObject* item, bool equip = false) {
        if (!item) return;

        auto player = RE::PlayerCharacter::GetSingleton();
        if (!player) return;

        player->AddObjectToContainer(item, nullptr, 1, nullptr);
        items.push_back(item);

        // After much testing, Skyrim seems to REALLY not like equipping more then one item per
        // frame And the per frame limit counts while the console is open. Doing so results in it
        // letting you equip as many items in one slot as you'd like So instead, have to mimic
        // the expected behavior and hopefully its good enough

        if (equip) {
            if (auto armor = item->As<RE::TESObjectARMO>()) {
                auto slots = (unsigned int)armor->GetSlotMask();
                if ((slots & recentEquipSlots) == 0) {
                    recentEquipSlots |= slots;

                    // Not using AddTask will result in it not un-equipping current items
                    SKSE::GetTaskInterface()->AddTask([=]() {
                        RE::ActorEquipManager::GetSingleton()->EquipObject(player, item, nullptr, 1,
                                                                           armor->GetEquipSlot(), false, false, false);
                    });
                }
            }
        }
    }

    void Remove() {
        auto player = RE::PlayerCharacter::GetSingleton();
        if (!player) return;

        for (auto i : items) player->RemoveItem(i, 1, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);

        items.clear();
    }

    unsigned int recentEquipSlots = 0;
    std::vector<RE::TESBoundObject*> items;
};

bool SliderTable() {
    if (ImGui::BeginTable("Slider Table", 3, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingFixedFit)) {
        ImGui::TableSetupColumn("SliderCol1");
        ImGui::TableSetupColumn("SliderCol2", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("SliderCol3");
        return true;
    }
    return false;
}

void SliderRow(const char* field, ArmorChangeParams::SliderPair& pair, float min = 0.0f, float max = 300.0f) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::PushID(field);
    ImGui::Checkbox(std::format("Modify {}", field).c_str(), &pair.bModify);
    ImGui::BeginDisabled(!pair.bModify);
    ImGui::TableNextColumn();
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    ImGui::SliderFloat("##Scale", &pair.fScale, min, max, "%.0f%%", ImGuiSliderFlags_AlwaysClamp);
    ImGui::TableNextColumn();
    if (ImGui::Button("Reset")) pair.fScale = 100.0f;
    ImGui::EndDisabled();
    ImGui::PopID();
}

void PermissionsChecklist(const char* id, Permissions& p) {
    ImGui::PushID(id);
    ImGui::Checkbox("Distribute loot", &p.bDistributeLoot);
    ImGui::Checkbox("Modify keywords", &p.bModifyKeywords);
    ImGui::Checkbox("Modify armor slots", &p.bModifySlots);
    ImGui::Checkbox("Modify armor rating", &p.bModifyArmorRating);
    ImGui::Checkbox("Modify armor weight", &p.bModifyWeight);
    ImGui::Checkbox("Modify weapon damage", &p.bModifyWeapDamage);
    ImGui::Checkbox("Modify weapon weight", &p.bModifyWeapWeight);
    ImGui::Checkbox("Modify weapon speed", &p.bModifyWeapSpeed);
    ImGui::Checkbox("Modify weapon stagger", &p.bModifyWeapStagger);
    ImGui::Checkbox("Modify value", &p.bModifyValue);
    ImGui::Checkbox("Modify crafting recipes", &p.crafting.bModify);
    ImGui::Checkbox("Create crafting recipes", &p.crafting.bCreate);
    ImGui::Checkbox("Free crafting recipes", &p.crafting.bFree);
    ImGui::Checkbox("Modify temper recipes", &p.temper.bModify);
    ImGui::Checkbox("Create temper recipes", &p.temper.bCreate);
    ImGui::Checkbox("Free temper recipes", &p.temper.bFree);
    ImGui::PopID();
}

struct SlotInfo {
    std::string name;
    bool isActivated;
    uint32_t mask;

    // Default constructor
    SlotInfo() : name(""), isActivated(false), mask(0) {}

    SlotInfo(const std::string& n, uint32_t m) : name(n), isActivated(false), mask(m) {}
};

using SlotMap = std::unordered_map<uint32_t, SlotInfo>;

// Function to equip an item
void SetSlotActive(SlotMap& slots, uint32_t slotMask, bool activated) {
    auto it = slots.find(slotMask);
    if (it != slots.end()) {
        it->second.isActivated = activated;
    }
}

SlotMap initializeSlotMap() {
    SlotMap slots;

    slots[0x00000001] = SlotInfo("HEAD", 0x00000001);            // 1, Slot 30
    slots[0x00000002] = SlotInfo("Hair", 0x00000002);            // 2, Slot 31
    slots[0x00000004] = SlotInfo("BODY", 0x00000004);            // 4, Slot 32
    slots[0x00000008] = SlotInfo("Hands", 0x00000008);           // 8, Slot 33
    slots[0x00000010] = SlotInfo("Forearms", 0x00000010);        // 16, Slot 34
    slots[0x00000020] = SlotInfo("Amulet", 0x00000020);          // 32, Slot 35
    slots[0x00000040] = SlotInfo("Ring", 0x00000040);            // 64, Slot 36
    slots[0x00000080] = SlotInfo("Feet", 0x00000080);            // 128, Slot 37
    slots[0x00000100] = SlotInfo("Calves", 0x00000100);          // 256, Slot 38
    slots[0x00000200] = SlotInfo("SHIELD", 0x00000200);          // 512, Slot 39
    slots[0x00000400] = SlotInfo("TAIL", 0x00000400);            // 1024, Slot 40
    slots[0x00000800] = SlotInfo("LongHair", 0x00000800);        // 2048, Slot 41
    slots[0x00001000] = SlotInfo("Circlet", 0x00001000);         // 4096, Slot 42
    slots[0x00002000] = SlotInfo("Ears", 0x00002000);            // 8192, Slot 43
    slots[0x00004000] = SlotInfo("Unk1", 0x00004000);            // 16384, Slot 44
    slots[0x00008000] = SlotInfo("Unk2", 0x00008000);            // 32768, Slot 45
    slots[0x00010000] = SlotInfo("Unk3", 0x00010000);            // 65536, Slot 46
    slots[0x00020000] = SlotInfo("Unk4", 0x00020000);            // 131072, Slot 47
    slots[0x00040000] = SlotInfo("Unk5", 0x00040000);            // 262144, Slot 48
    slots[0x00080000] = SlotInfo("Unk6", 0x00080000);            // 524288, Slot 49
    slots[0x00100000] = SlotInfo("DecapitateHead", 0x00100000);  // 1048576, Slot 50
    slots[0x00200000] = SlotInfo("Decapitate", 0x00200000);      // 2097152, Slot 51
    slots[0x00400000] = SlotInfo("Unk7", 0x00400000);            // 4194304, Slot 52
    slots[0x00800000] = SlotInfo("Unk8", 0x00800000);            // 8388608, Slot 53
    slots[0x01000000] = SlotInfo("Unk9", 0x01000000);            // 16777216, Slot 54
    slots[0x02000000] = SlotInfo("Unk10", 0x02000000);           // 33554432, Slot 55
    slots[0x04000000] = SlotInfo("Unk11", 0x04000000);           // 67108864, Slot 56
    slots[0x08000000] = SlotInfo("Unk12", 0x08000000);           // 134217728, Slot 57
    slots[0x10000000] = SlotInfo("Unk13", 0x10000000);           // 268435456, Slot 58
    slots[0x20000000] = SlotInfo("Unk14", 0x20000000);           // 536870912, Slot 59
    slots[0x40000000] = SlotInfo("Unk15", 0x40000000);           // 1073741824, Slot 60
    slots[0x80000000] = SlotInfo("FX01", 0x80000000);            // 2147483648, Slot 61

    return slots;
}

void SetSlots(SlotMap& slots, bool bFilterSlotmaskHead, bool bFilterSlotmaskHair, bool bFilterSlotmaskBody,
              bool bFilterSlotmaskHands, bool bFilterSlotmaskForearms, bool bFilterSlotmaskAmulet,
              bool bFilterSlotmaskRing, bool bFilterSlotmaskFeet, bool bFilterSlotmaskCalves,
              bool bFilterSlotmaskShield, bool bFilterSlotmaskTail, bool bFilterSlotmaskLongHair,
              bool bFilterSlotmaskCirclet, bool bFilterSlotmaskEars, bool bFilterSlotmaskUnk1, bool bFilterSlotmaskUnk2,
              bool bFilterSlotmaskUnk3, bool bFilterSlotmaskUnk4, bool bFilterSlotmaskUnk5, bool bFilterSlotmaskUnk6,
              bool bFilterSlotmaskDecapitateHead, bool bFilterSlotmaskDecapitate, bool bFilterSlotmaskUnk7,
              bool bFilterSlotmaskUnk8, bool bFilterSlotmaskUnk9, bool bFilterSlotmaskUnk10, bool bFilterSlotmaskUnk11,
              bool bFilterSlotmaskUnk12, bool bFilterSlotmaskUnk13, bool bFilterSlotmaskUnk14,
              bool bFilterSlotmaskUnk15, bool bFilterSlotmaskFX01) {
    SetSlotActive(slots, 0x00000001, bFilterSlotmaskHead);            // HEAD
    SetSlotActive(slots, 0x00000002, bFilterSlotmaskHair);            // Hair
    SetSlotActive(slots, 0x00000004, bFilterSlotmaskBody);            // BODY
    SetSlotActive(slots, 0x00000008, bFilterSlotmaskHands);           // Hands
    SetSlotActive(slots, 0x00000010, bFilterSlotmaskForearms);        // Forearms
    SetSlotActive(slots, 0x00000020, bFilterSlotmaskAmulet);          // Amulet
    SetSlotActive(slots, 0x00000040, bFilterSlotmaskRing);            // Ring
    SetSlotActive(slots, 0x00000080, bFilterSlotmaskFeet);            // Feet
    SetSlotActive(slots, 0x00000100, bFilterSlotmaskCalves);          // Calves
    SetSlotActive(slots, 0x00000200, bFilterSlotmaskShield);          // SHIELD
    SetSlotActive(slots, 0x00000400, bFilterSlotmaskTail);            // TAIL
    SetSlotActive(slots, 0x00000800, bFilterSlotmaskLongHair);        // LongHair
    SetSlotActive(slots, 0x00001000, bFilterSlotmaskCirclet);         // Circlet
    SetSlotActive(slots, 0x00002000, bFilterSlotmaskEars);            // Ears
    SetSlotActive(slots, 0x00004000, bFilterSlotmaskUnk1);            // Unk1
    SetSlotActive(slots, 0x00008000, bFilterSlotmaskUnk2);            // Unk2
    SetSlotActive(slots, 0x00010000, bFilterSlotmaskUnk3);            // Unk3
    SetSlotActive(slots, 0x00020000, bFilterSlotmaskUnk4);            // Unk4
    SetSlotActive(slots, 0x00040000, bFilterSlotmaskUnk5);            // Unk5
    SetSlotActive(slots, 0x00080000, bFilterSlotmaskUnk6);            // Unk6
    SetSlotActive(slots, 0x00100000, bFilterSlotmaskDecapitateHead);  // DecapitateHead
    SetSlotActive(slots, 0x00200000, bFilterSlotmaskDecapitate);      // Decapitate
    SetSlotActive(slots, 0x00400000, bFilterSlotmaskUnk7);            // Unk7
    SetSlotActive(slots, 0x00800000, bFilterSlotmaskUnk8);            // Unk8
    SetSlotActive(slots, 0x01000000, bFilterSlotmaskUnk9);            // Unk9
    SetSlotActive(slots, 0x02000000, bFilterSlotmaskUnk10);           // Unk10
    SetSlotActive(slots, 0x04000000, bFilterSlotmaskUnk11);           // Unk11
    SetSlotActive(slots, 0x08000000, bFilterSlotmaskUnk12);           // Unk12
    SetSlotActive(slots, 0x10000000, bFilterSlotmaskUnk13);           // Unk13
    SetSlotActive(slots, 0x20000000, bFilterSlotmaskUnk14);           // Unk14
    SetSlotActive(slots, 0x40000000, bFilterSlotmaskUnk15);           // Unk15
    SetSlotActive(slots, 0x80000000, bFilterSlotmaskFX01);            // FX01
}

void GetCurrentListItems(ModData* curMod, bool bFilterModified, const char* itemFilter, bool bFilterSlots,
                         bool bFilterAllArmor, bool bFilterAllWeapons, bool bFilterSlotsReverse, bool bFilterSlotsExact,
                         bool bFilterSlotsBitwise, SlotMap& slots) {
    ArmorChangeParams& params = g_Config.acParams;
    params.items.clear();

    std::set<RE::TESBoundObject*> items;

    if (bFilterAllArmor) {  // If show all armor is selected, get all armors
        auto dataHandler = RE::TESDataHandler::GetSingleton();
        auto itemArray = dataHandler->GetFormArray<RE::TESObjectARMO>();
        for (auto i : itemArray) {
            if (auto armor = i->As<RE::TESObjectARMO>()) {
                items.insert(armor);
            }
        }
    } else if (bFilterAllWeapons) {  // If show all weapons is selected, get all weapons
        auto dataHandler = RE::TESDataHandler::GetSingleton();
        auto itemArray = dataHandler->GetFormArray<RE::TESObjectWEAP>();
        for (auto i : itemArray) {
            if (auto weap = i->As<RE::TESObjectWEAP>()) {
                items.insert(weap);
            }
        }
    } else if (curMod) {  // If a mod is selected, get the items from that mod
        items = curMod->items;
    } else {  // If no mod is selected and we are not showing all items, get the items from the player's inventory
        if (auto player = RE::PlayerCharacter::GetSingleton()) {
            for (auto& item : player->GetInventory()) {
                if (item.second.second->IsWorn() && item.first->IsArmor()) {
                    if (auto i = item.first->As<RE::TESObjectARMO>()) {
                        items.insert(i);
                    }
                }
            }
        }
    }

    for (auto i : items) {
        if (!IsValidItem(i)) continue;
        if (bFilterModified && (g_Data.modifiedItems.contains(i) || g_Data.modifiedItemsShared.contains(i))) continue;
        // if (*itemFilter && !StringContainsI(i->GetFullName(), itemFilter)) continue;
        if (*itemFilter) {
            auto fullName = i->As<RE::TESFullName>();
            if (fullName && !StringContainsI(fullName->GetFullName(), itemFilter)) continue;
        }
        if (bFilterSlots) {
            if (auto armor = i->As<RE::TESObjectARMO>()) {
                auto armorslots = armor->GetSlotMask();
                bool match = false;

                if (!bFilterSlotsBitwise) {
                    // Standard Slot Filtering (Exact, Reverse, or Standard)
                    for (const auto& [slotMask, slotInfo] : slots) {
                        if (slotInfo.isActivated) {
                            if (bFilterSlotsExact) {
                                if ((unsigned int)armorslots == slotInfo.mask) {
                                    match = true;
                                    break;
                                }
                            } else if (bFilterSlotsReverse) {
                                if (((unsigned int)armorslots & slotInfo.mask) == 0) {  // Slot mask not set
                                    match = true;
                                    break;
                                }
                            } else {
                                if ((unsigned int)armorslots & slotInfo.mask) {  // Slot mask matches
                                    match = true;
                                    break;
                                }
                            }
                        }
                    }
                } else {
                    // Bitwise Slot Filtering
                    uint32_t combinedBitmask = 0;
                    for (const auto& [slotMask, slotInfo] : slots) {
                        if (slotInfo.isActivated) {
                            combinedBitmask |= slotInfo.mask;  // Combine activated slots into one bitmask
                        }
                    }

                    // Handling different cases for bitwise filtering
                    if (bFilterSlotsExact) {
                        // Exact: Armor slots must match the combined bitmask exactly
                        if ((unsigned int)armorslots == combinedBitmask) {
                            match = true;
                        }
                    } else if (bFilterSlotsReverse) {
                        // Reverse: None of the combined bitmask slots should be set in the armor
                        if (((unsigned int)armorslots & combinedBitmask) == 0) {
                            match = true;
                        }
                    } else {
                        // Standard bitwise: Armor slots should have **all** of the combined bitmask slots set
                        if (((unsigned int)armorslots & combinedBitmask) == combinedBitmask) {
                            match = true;
                        }
                    }
                }

                // If the slot matches the filter criteria, add the item to the list
                if (match) {
                    params.items.push_back(i);
                }
            } else {
                // If the item is not armor, ignore the slot filtering and add it to the list
                params.items.push_back(i);
            }
        } else {
            // No slot filtering, add the item directly
            params.items.push_back(i);
        }
    }

    if (!params.items.empty()) {
        std::sort(params.items.begin(), params.items.end(),
                  [](RE::TESBoundObject* const a, RE::TESBoundObject* const b) {
                      return _stricmp(a->GetName(), b->GetName()) < 0;
                  });
    }
}

bool WillBeModified(RE::TESBoundObject* i, ArmorSlots remapped) {
    if (auto armor = i->As<RE::TESObjectARMO>()) {
        if (((remapped | g_Config.slotsWillChange) & (ArmorSlots)armor->GetSlotMask()) == 0) return false;
    } else if (auto weap = i->As<RE::TESObjectWEAP>()) {
        if (!g_Config.acParams.armorSet->FindMatching(weap)) return false;
    } else if (auto ammo = i->As<RE::TESAmmo>()) {
        if (!g_Config.acParams.armorSet->FindMatching(ammo)) return false;
    } else
        return false;

    return true;
}

short g_highlightRound = 0;

struct HighlightTrack {
    short round = -1;
    char pop = 0;
    bool enabled = false;

    void Touch() { round = g_highlightRound; }
    operator bool() { return !enabled || g_highlightRound == round; }

    void Push(bool show = true) {
        enabled = show;
        if (g_Config.bHighlights && round != g_highlightRound && show) {
            auto phase = 0.5 + 0.5 * sin(ImGui::GetTime() * (std::_Pi_val / 1.0));
            auto brightness = std::lerp(64, 255, phase);
            const auto colorHighlight = IM_COL32(0, brightness, brightness, 255);

            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_Border, colorHighlight);
            pop = 1;
        } else
            pop = 0;
    }

    void Pop() {
        if (ImGui::IsItemActive()) round = g_highlightRound;

        ImGui::PopStyleVar(pop);
        ImGui::PopStyleColor(pop);
    }
};

void QuickArmorRebalance::RenderUI() {
    SlotMap slots = initializeSlotMap();
    const auto colorChanged = IM_COL32(0, 255, 0, 255);
    const auto colorChangedShared = IM_COL32(255, 255, 0, 255);
    const auto colorDeleted = IM_COL32(255, 0, 0, 255);

    const char* rarity[] = {"Common", "Uncommon", "Rare", nullptr};

    bool isActive = true;
    static std::set<RE::TESBoundObject*> selectedItems;
    static RE::TESBoundObject* lastSelectedItem = nullptr;
    static GivenItems givenItems;
    static ModData* curMod = nullptr;
    static std::set<RE::TESObject*> uncheckedItems;

    if (!RE::UI::GetSingleton()->numPausesGame) givenItems.recentEquipSlots = 0;

    const bool isShiftDown = ImGui::IsKeyDown(ImGuiKey_LeftShift) || ImGui::IsKeyDown(ImGuiKey_RightShift);
    const bool isCtrlDown = ImGui::IsKeyDown(ImGuiKey_LeftCtrl) || ImGui::IsKeyDown(ImGuiKey_RightCtrl);
    const bool isAltDown = ImGui::IsKeyDown(ImGuiKey_LeftAlt) || ImGui::IsKeyDown(ImGuiKey_RightAlt);

    static HighlightTrack hlConvert;
    static HighlightTrack hlDistributeAs;
    static HighlightTrack hlRarity;
    static HighlightTrack hlSlots;

    auto isInventoryOpen = RE::UI::GetSingleton()->IsItemMenuOpen();

    static bool bMenuHovered = false;
    static bool bSlotWarning = false;
    bool popupSettings = false;
    bool popupRemapSlots = false;

    ArmorSlots remappedSrc = 0;
    ArmorSlots remappedTar = 0;

    for (auto i : g_Config.acParams.mapArmorSlots) {
        remappedSrc |= 1 << i.first;
        remappedTar |= i.second < 32 ? (1 << i.second) : 0;
    }

    ImGuiWindowFlags wndFlags = ImGuiWindowFlags_NoScrollbar;
    if (bMenuHovered) wndFlags |= ImGuiWindowFlags_MenuBar;

    ImGui::SetNextWindowSizeConstraints({700, 250}, {1600, 1000});
    if (ImGui::Begin("Quick Armor Rebalance", &isActive, wndFlags)) {
        if (g_Config.strCriticalError.empty()) {
            ArmorChangeParams& params = g_Config.acParams;

            const char* noMod = "<Currently Worn Armor>";
            static bool bFilterChangedMods = false;
            static bool bFilterAllArmor = false;
            static bool bFilterAllWeapons = false;

            if (bMenuHovered) {
                bMenuHovered = false;

                if (ImGui::BeginMenuBar()) {
                    auto menuSize = ImGui::GetItemRectSize();

                    ImGui::SetItemAllowOverlap();

                    RightAlign("Settings");
                    if (ImGui::MenuItem("Settings")) {
                        popupSettings = true;
                    }

                    ImGui::EndMenuBar();

                    // The menu bar doesn't return the right values (but somehow has the right size?), so instead we
                    // work off the last menu item added
                    bMenuHovered |=
                        ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenOverlapped);  // Doesn't actually work
                    bMenuHovered |= ImGui::IsMouseHoveringRect(ImGui::GetWindowPos(), ImGui::GetItemRectMax(), false);

                    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - menuSize.y);
                }
            }
            bMenuHovered |= ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenOverlapped);

            ImGui::PushItemWidth(-FLT_MIN);

            if (ImGui::BeginTable("WindowTable", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
                ImGui::TableSetupColumn("LeftCol", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("RightCol", ImGuiTableColumnFlags_WidthFixed,
                                        0.25f * ImGui::GetContentRegionAvail().x);

                static bool bFilterModified = false;
                static bool bFilterSlots = false;
                static bool bFilterSlotsReverse = false;
                static bool bFilterSlotsExact = false;
                static bool bFilterSlotsBitwise = false;
                static bool bFilterSlotmaskHead = false;
                static bool bFilterSlotmaskHair = false;
                static bool bFilterSlotmaskBody = false;
                static bool bFilterSlotmaskHands = false;
                static bool bFilterSlotmaskForearms = false;
                static bool bFilterSlotmaskAmulet = false;
                static bool bFilterSlotmaskRing = false;
                static bool bFilterSlotmaskFeet = false;
                static bool bFilterSlotmaskCalves = false;
                static bool bFilterSlotmaskShield = false;
                static bool bFilterSlotmaskTail = false;
                static bool bFilterSlotmaskLongHair = false;
                static bool bFilterSlotmaskCirclet = false;
                static bool bFilterSlotmaskEars = false;
                static bool bFilterSlotmaskUnk1 = false;
                static bool bFilterSlotmaskUnk2 = false;
                static bool bFilterSlotmaskUnk3 = false;
                static bool bFilterSlotmaskUnk4 = false;
                static bool bFilterSlotmaskUnk5 = false;
                static bool bFilterSlotmaskUnk6 = false;
                static bool bFilterSlotmaskDecapitateHead = false;
                static bool bFilterSlotmaskDecapitate = false;
                static bool bFilterSlotmaskUnk7 = false;
                static bool bFilterSlotmaskUnk8 = false;
                static bool bFilterSlotmaskUnk9 = false;
                static bool bFilterSlotmaskUnk10 = false;
                static bool bFilterSlotmaskUnk11 = false;
                static bool bFilterSlotmaskUnk12 = false;
                static bool bFilterSlotmaskUnk13 = false;
                static bool bFilterSlotmaskUnk14 = false;
                static bool bFilterSlotmaskUnk15 = false;
                static bool bFilterSlotmaskFX01 = false;

                static char itemFilter[200] = "";
                bool showPopup = false;

                ImGui::TableNextColumn();
                if (ImGui::BeginChild("LeftPane")) {
                    ImGui::PushItemWidth(-FLT_MIN);

                    if (ImGui::BeginTable("Mod Table", 3, ImGuiTableFlags_SizingFixedFit)) {
                        ImGui::TableSetupColumn("ModCol1");
                        ImGui::TableSetupColumn("ModCol2", ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableSetupColumn("ModCol3");
                        ImGui::TableNextColumn();

                        ImGui::Text("Mod");
                        ImGui::TableNextColumn();

                        RE::TESFile* blacklist = nullptr;

                        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
                        if (ImGui::BeginCombo("##Mod", curMod ? curMod->mod->fileName : noMod,
                                              ImGuiComboFlags_HeightLarge)) {
                            {
                                bool selected = !curMod;
                                if (ImGui::Selectable(noMod, selected)) curMod = nullptr;
                                if (selected) ImGui::SetItemDefaultFocus();
                            }

                            for (auto i : g_Data.sortedMods) {
                                bool selected = curMod == i;

                                int pop = 0;
                                if (g_Data.modifiedFiles.contains(i->mod)) {
                                    if (bFilterChangedMods) continue;
                                    if (g_Data.modifiedFilesDeleted.contains(i->mod))
                                        ImGui::PushStyleColor(ImGuiCol_Text, colorDeleted);
                                    else
                                        ImGui::PushStyleColor(ImGuiCol_Text, colorChanged);
                                    pop++;
                                } else if (g_Data.modifiedFilesShared.contains(i->mod)) {
                                    if (bFilterChangedMods) continue;
                                    ImGui::PushStyleColor(ImGuiCol_Text, colorChangedShared);
                                    pop++;
                                }

                                if (ImGui::Selectable(i->mod->fileName, selected)) {
                                    if (isCtrlDown && isAltDown && g_Data.modifiedFiles.contains(i->mod)) {
                                        showPopup = true;
                                    }

                                    curMod = i;
                                    givenItems.items.clear();
                                    selectedItems.clear();
                                    lastSelectedItem = nullptr;

                                    if (g_Config.bResetSliders) {
                                        params.armor.rating.fScale = 100.0f;
                                        params.armor.weight.fScale = 100.0f;
                                        params.weapon.damage.fScale = 100.0f;
                                        params.weapon.speed.fScale = 100.0f;
                                        params.weapon.weight.fScale = 100.0f;
                                        params.weapon.stagger.fScale = 100.0f;
                                        params.value.fScale = 100.0f;
                                    }
                                    if (g_Config.bResetSlotRemap) {
                                        params.mapArmorSlots.clear();
                                    }

                                    g_highlightRound++;
                                }
                                if (isCtrlDown && isAltDown && ImGui::IsItemHovered() &&
                                    ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                                    blacklist = i->mod;
                                }

                                if (selected) ImGui::SetItemDefaultFocus();
                                ImGui::PopStyleColor(pop);
                            }

                            ImGui::EndCombo();

                            params.isWornArmor = !curMod;

                            if (blacklist) g_Config.AddUserBlacklist(blacklist);
                        }

                        {
                            const char* popupTitle = "Delete Changes?";
                            if (showPopup) ImGui::OpenPopup(popupTitle);

                            ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing,
                                                    ImVec2(0.5f, 0.5f));

                            if (ImGui::BeginPopupModal(popupTitle, NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
                                ImGui::Text("Delete all changes to %s?", curMod->mod->fileName);
                                ImGui::Text("Changes will not revert until after restarting Skyrim");

                                if (ImGui::Button("Delete changes", ImVec2(120, 0))) {
                                    DeleteAllChanges(curMod->mod);
                                    ImGui::CloseCurrentPopup();
                                }
                                ImGui::SetItemDefaultFocus();
                                ImGui::SameLine();
                                if (ImGui::Button("Cancel", ImVec2(120, 0))) {
                                    ImGui::CloseCurrentPopup();
                                }
                                ImGui::EndPopup();
                            }
                        }

                        ImGui::TableNextColumn();
                        ImGui::Checkbox("Hide modified", &bFilterChangedMods);
                        
                        ImGui::EndTable();
                    }

                    ImGui::Separator();
                    ImGui::Text("Browse all Items");
                    ImGui::Indent();
                    ImGui::Checkbox("All Armor", &bFilterAllArmor);
                    ImGui::SameLine();
                    ImGui::Checkbox("All Weapons", &bFilterAllWeapons);
                    ImGui::Unindent();
                    ImGui::Separator();
                    ImGui::Text("Filter Items");
                    ImGui::Indent();
                    ImGui::Text("Name contains");
                    ImGui::SameLine();
                    if (ImGui::InputText("##ItemFilter", itemFilter, sizeof(itemFilter) - 1)) g_highlightRound++;
                    if (ImGui::Checkbox("Hide modified items", &bFilterModified)) g_highlightRound++;

                    if (ImGui::BeginTable("Slot Settings", 4, ImGuiTableFlags_SizingFixedFit)) {
                        ImGui::TableSetupColumn("SlotSettingCol1");
                        ImGui::TableSetupColumn("SlotSettingCol2");
                        ImGui::TableSetupColumn("SlotSettingCol3");
                        ImGui::TableSetupColumn("SlotSettingCol4");

                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        if (ImGui::Checkbox("Filter by Slot", &bFilterSlots)) g_highlightRound++;
                        ImGui::BeginDisabled(!bFilterSlots);
                        ImGui::TableNextColumn();
                        if (ImGui::Checkbox("Reverse", &bFilterSlotsReverse)) g_highlightRound++;

                        ImGui::TableNextColumn();
                        if (ImGui::Checkbox("Exact", &bFilterSlotsExact)) g_highlightRound++;

                        ImGui::TableNextColumn();
                        if (ImGui::Checkbox("Bitwise", &bFilterSlotsBitwise)) g_highlightRound++;
                        ImGui::EndDisabled();  // bFilterSlots
                        ImGui::EndTable();
                    }

                    ImGui::Indent();
                    ImGui::BeginDisabled(!bFilterSlots);
                    if (ImGui::BeginTable("Slot Filter", 3, ImGuiTableFlags_SizingFixedFit)) {
                        ImGui::TableSetupColumn("SlotCol1");
                        ImGui::TableSetupColumn("SlotCol2");
                        ImGui::TableSetupColumn("SlotCol3");

                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::Checkbox("30 (HEAD)", &bFilterSlotmaskHead);  // 1

                        ImGui::TableNextColumn();
                        ImGui::Checkbox("31 (Hair)", &bFilterSlotmaskHair);  // 2

                        ImGui::TableNextColumn();
                        ImGui::Checkbox("32 (BODY)", &bFilterSlotmaskBody);  // 4

                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::Checkbox("33 (Hands)", &bFilterSlotmaskHands);  // 8

                        ImGui::TableNextColumn();
                        ImGui::Checkbox("34 (Forearms)", &bFilterSlotmaskForearms);  // 16

                        ImGui::TableNextColumn();
                        ImGui::Checkbox("35 (Amulet)", &bFilterSlotmaskAmulet);  // 32

                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::Checkbox("36 (Ring)", &bFilterSlotmaskRing);  // 64

                        ImGui::TableNextColumn();
                        ImGui::Checkbox("37 (Feet)", &bFilterSlotmaskFeet);  // 128

                        ImGui::TableNextColumn();
                        ImGui::Checkbox("38 (Calves)", &bFilterSlotmaskCalves);  // 256

                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::Checkbox("39 (SHIELD)", &bFilterSlotmaskShield);  // 512

                        ImGui::TableNextColumn();
                        ImGui::Checkbox("40 (TAIL)", &bFilterSlotmaskTail);  // 1024

                        ImGui::TableNextColumn();
                        ImGui::Checkbox("41 (LongHair)", &bFilterSlotmaskLongHair);  // 2048

                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::Checkbox("42 (Circlet)", &bFilterSlotmaskCirclet);  // 4096

                        ImGui::TableNextColumn();
                        ImGui::Checkbox("43 (Ears)", &bFilterSlotmaskEars);  // 8192

                        ImGui::TableNextColumn();
                        ImGui::Checkbox("44 (Unk1)", &bFilterSlotmaskUnk1);  // 16384

                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::Checkbox("45 (Unk2)", &bFilterSlotmaskUnk2);  // 32768

                        ImGui::TableNextColumn();
                        ImGui::Checkbox("46 (Unk3)", &bFilterSlotmaskUnk3);  // 65536

                        ImGui::TableNextColumn();
                        ImGui::Checkbox("47 (Unk4)", &bFilterSlotmaskUnk4);  // 131072

                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::Checkbox("48 (Unk5)", &bFilterSlotmaskUnk5);  // 262144

                        ImGui::TableNextColumn();
                        ImGui::Checkbox("49 (Unk6)", &bFilterSlotmaskUnk6);  // 524288

                        ImGui::TableNextColumn();
                        ImGui::Checkbox("50 (DecapitateHead)", &bFilterSlotmaskDecapitateHead);  // 1048576

                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::Checkbox("51 (Decapitate)", &bFilterSlotmaskDecapitate);  // 2097152

                        ImGui::TableNextColumn();
                        ImGui::Checkbox("52 (Unk7)", &bFilterSlotmaskUnk7);  // 4194304

                        ImGui::TableNextColumn();
                        ImGui::Checkbox("53 (Unk8)", &bFilterSlotmaskUnk8);  // 8388608

                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::Checkbox("54 (Unk9)", &bFilterSlotmaskUnk9);  // 16777216

                        ImGui::TableNextColumn();
                        ImGui::Checkbox("55 (Unk10)", &bFilterSlotmaskUnk10);  // 33554432

                        ImGui::TableNextColumn();
                        ImGui::Checkbox("56 (Unk11)", &bFilterSlotmaskUnk11);  // 67108864

                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::Checkbox("57 (Unk12)", &bFilterSlotmaskUnk12);  // 134217728

                        ImGui::TableNextColumn();
                        ImGui::Checkbox("58 (Unk13)", &bFilterSlotmaskUnk13);  // 268435456

                        ImGui::TableNextColumn();
                        ImGui::Checkbox("59 (Unk14)", &bFilterSlotmaskUnk14);  // 536870912

                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::Checkbox("60 (Unk15)", &bFilterSlotmaskUnk15);  // 1073741824

                        ImGui::TableNextColumn();
                        ImGui::Checkbox("61 (FX01)", &bFilterSlotmaskFX01);  // 2147483648

                        ImGui::EndTable();
                    }
                    ImGui::EndDisabled(); // bFilterSlots
                    ImGui::Unindent(); // Slot Filter

                    ImGui::Unindent();

                    if (ImGui::BeginTable("Convert Table", 3, ImGuiTableFlags_SizingFixedFit)) {
                        ImGui::TableSetupColumn("ConvertCol1");
                        ImGui::TableSetupColumn("ConvertCol2", ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableSetupColumn("ConvertCol3");
                        ImGui::TableNextColumn();

                        ImGui::Text("Convert to");
                        ImGui::TableNextColumn();

                        if (!params.armorSet) params.armorSet = &g_Config.armorSets[0];

                        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);

                        hlConvert.Push();
                        if (ImGui::BeginCombo("##ConvertTo", params.armorSet->name.c_str(),
                                              ImGuiComboFlags_PopupAlignLeft | ImGuiComboFlags_HeightLarge)) {
                            for (auto& i : g_Config.armorSets) {
                                bool selected = params.armorSet == &i;
                                ImGui::PushID(i.name.c_str());
                                if (ImGui::Selectable(i.name.c_str(), selected)) params.armorSet = &i;
                                if (selected) ImGui::SetItemDefaultFocus();
                                MakeTooltip(i.strContents.c_str(), true);
                                ImGui::PopID();
                            }

                            ImGui::EndCombo();
                        }
                        hlConvert.Pop();

                        ImGui::TableNextColumn();

                        static HighlightTrack hlApply;
                        hlApply.Push(hlConvert && hlDistributeAs && hlRarity && hlSlots);

                        if (ImGui::Button("Apply changes")) {
                            g_Config.Save();
                            MakeArmorChanges(params);

                            if (g_Config.bAutoDeleteGiven) givenItems.Remove();

                            hlConvert.Touch();
                            hlDistributeAs.Touch();
                            hlRarity.Touch();
                            hlSlots.Touch();
                        }

                        hlApply.Pop();

                        ImGui::EndTable();
                    }

                    SetSlots(slots, bFilterSlotmaskHead, bFilterSlotmaskHair, bFilterSlotmaskBody, bFilterSlotmaskHands,
                             bFilterSlotmaskForearms, bFilterSlotmaskAmulet, bFilterSlotmaskRing, bFilterSlotmaskFeet,
                             bFilterSlotmaskCalves, bFilterSlotmaskShield, bFilterSlotmaskTail, bFilterSlotmaskLongHair,
                             bFilterSlotmaskCirclet, bFilterSlotmaskEars, bFilterSlotmaskUnk1, bFilterSlotmaskUnk2,
                             bFilterSlotmaskUnk3, bFilterSlotmaskUnk4, bFilterSlotmaskUnk5, bFilterSlotmaskUnk6,
                             bFilterSlotmaskDecapitateHead, bFilterSlotmaskDecapitate, bFilterSlotmaskUnk7,
                             bFilterSlotmaskUnk8, bFilterSlotmaskUnk9, bFilterSlotmaskUnk10, bFilterSlotmaskUnk11,
                             bFilterSlotmaskUnk12, bFilterSlotmaskUnk13, bFilterSlotmaskUnk14, bFilterSlotmaskUnk15,
                             bFilterSlotmaskFX01);

                    GetCurrentListItems(curMod, bFilterModified, itemFilter, bFilterSlots, bFilterAllArmor,
                                        bFilterAllWeapons, bFilterSlotsReverse, bFilterSlotsExact, bFilterSlotsBitwise, slots);

                    g_Config.slotsWillChange = GetConvertableArmorSlots(params);

                    bool hasEnabledArmor = false;
                    bool hasEnabledWeap = false;

                    for (auto i : params.items) {
                        if (i->As<RE::TESObjectARMO>()) {
                            if (!uncheckedItems.contains(i)) hasEnabledArmor = true;
                        } else if (i->As<RE::TESObjectWEAP>()) {
                            if (!uncheckedItems.contains(i)) hasEnabledWeap = true;
                        } else if (i->As<RE::TESAmmo>()) {
                            if (!uncheckedItems.contains(i)) hasEnabledWeap = true;
                        }
                    }

                    // Distribution
                    ImGui::Separator();
                    ImGui::BeginDisabled(!curMod);

                    // Need to create a dummy table to negate stretching the combo boxes
                    ImGui::Checkbox("Distribute as ", &params.bDistribute);
                    MakeTooltip(
                        "Additions or changes to loot distribution will not take effect until you restart Skyrim");
                    ImGui::BeginDisabled(!params.bDistribute);

                    ImGui::SameLine();
                    // ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
                    ImGui::SetNextItemWidth(300);

                    hlDistributeAs.Push(params.bDistribute);

                    if (ImGui::BeginCombo("##DistributeAs", params.distProfile,
                                          ImGuiComboFlags_PopupAlignLeft | ImGuiComboFlags_HeightLarge)) {
                        for (auto& i : g_Config.lootProfiles) {
                            bool selected = params.distProfile == i;
                            if (ImGui::Selectable(i.c_str(), selected)) params.distProfile = i.c_str();
                            if (selected) ImGui::SetItemDefaultFocus();
                        }

                        ImGui::EndCombo();
                    }

                    hlDistributeAs.Pop();

                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(100);

                    hlRarity.Push(params.bDistribute);

                    if (ImGui::BeginCombo("##Rarity", rarity[params.rarity], ImGuiComboFlags_PopupAlignLeft)) {
                        for (int i = 0; rarity[i]; i++) {
                            bool selected = i == params.rarity;
                            if (ImGui::Selectable(rarity[i], selected)) params.rarity = i;
                            if (selected) ImGui::SetItemDefaultFocus();
                        }

                        ImGui::EndCombo();
                    }

                    hlRarity.Pop();

                    ImGui::Indent(60);
                    ImGui::Checkbox("As pieces", &params.bDistAsPieces);
                    ImGui::SameLine();
                    ImGui::Checkbox("As whole set", &params.bDistAsSet);
                    MakeTooltip("You will find entire sets (all slots) together");
                    ImGui::SameLine();
                    ImGui::BeginDisabled(!params.bDistAsSet);
                    ImGui::Checkbox("Matching sets", &params.bMatchSetPieces);
                    MakeTooltip(
                        "Attempts to match sets together - for example, if there are green and blue variants, it will\n"
                        "try to distribute only green or only blue parts as a single set");
                    ImGui::EndDisabled();
                    ImGui::Unindent(60);

                    ImGui::EndDisabled();
                    ImGui::EndDisabled();

                    // Modifications
                    // ImGui::SetNextItemSize();

                    // ImGui::PushStyleColor(ImGuiCol_ChildBg, colorDeleted);
                    if (ImGui::BeginChild("ItemTypeFrame", {ImGui::GetContentRegionAvail().x, 120}, false)) {
                        if (ImGui::BeginTabBar("ItemTypeBar")) {
                            static int iTabOpen = 0;
                            int iTab = 0;
                            int iTabSelected = iTabOpen;

                            bool bTabEnabled[] = {hasEnabledArmor && !params.armorSet->items.empty(),
                                                  hasEnabledWeap && !params.armorSet->weaps.empty()};
                            const int nTabCount = 2;

                            bool bForceSelect = false;
                            if (!bTabEnabled[iTabOpen]) {
                                iTabOpen = 0;
                                while (iTabOpen < nTabCount && !bTabEnabled[iTabOpen]) iTabOpen++;
                                if (iTabOpen >= nTabCount) iTabOpen = 0;

                                bForceSelect = true;
                            }

                            const char* tabLabels[] = {"Armor", "Weapons"};
                            // if (iTabOpen != iTabSelected) ImGui::SetTabItemClosed(tabLabels[iTabSelected]);

                            iTabSelected = iTabOpen;

                            // Amor tab
                            ImGui::BeginDisabled(!bTabEnabled[iTab]);
                            if (ImGui::BeginTabItem(
                                    tabLabels[iTab], nullptr,
                                    bForceSelect && iTabOpen == iTab ? ImGuiTabItemFlags_SetSelected : 0)) {
                                iTabSelected = iTab;
                                if (SliderTable()) {
                                    SliderRow("Armor Rating", params.armor.rating);
                                    SliderRow("Weight", params.armor.weight);

                                    ImGui::EndTable();
                                }

                                ImGui::Separator();
                                ImGui::Text("Stat distribution curve");
                                ImGui::SameLine();

                                static auto* curCurve = &g_Config.curves[0];

                                ImGui::SetNextItemWidth(220);
                                if (ImGui::BeginCombo("##Curve", curCurve->first.c_str(),
                                                      ImGuiComboFlags_PopupAlignLeft | ImGuiComboFlags_HeightLarge)) {
                                    for (auto& i : g_Config.curves) {
                                        bool selected = curCurve == &i;
                                        ImGui::PushID(i.first.c_str());
                                        if (ImGui::Selectable(i.first.c_str(), selected)) curCurve = &i;
                                        if (selected) ImGui::SetItemDefaultFocus();
                                        ImGui::PopID();
                                    }

                                    ImGui::EndCombo();
                                }
                                params.curve = &curCurve->second;

                                ImGui::SameLine();
                                ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 40);

                                hlSlots.Push(bSlotWarning);
                                if (ImGui::Button("Remap Slots")) popupRemapSlots = true;
                                hlSlots.Pop();

                                ImGui::EndTabItem();
                            }
                            iTab++;
                            ImGui::EndDisabled();

                            // Weapon tab
                            ImGui::BeginDisabled(!bTabEnabled[iTab]);
                            if (ImGui::BeginTabItem(
                                    tabLabels[iTab], nullptr,
                                    bForceSelect && iTabOpen == iTab ? ImGuiTabItemFlags_SetSelected : 0)) {
                                iTabSelected = iTab;
                                if (SliderTable()) {
                                    SliderRow("Damage", params.weapon.damage);
                                    SliderRow("Weight", params.weapon.weight);
                                    SliderRow("Speed", params.weapon.speed);
                                    SliderRow("Stagger", params.weapon.stagger);

                                    ImGui::EndTable();
                                }
                                iTab++;

                                ImGui::EndTabItem();
                            }
                            ImGui::EndDisabled();
                            ImGui::EndTabBar();
                            iTabOpen = iTabSelected;
                        }
                    }
                    ImGui::EndChild();
                    // ImGui::PopStyleColor();

                    // ImGui::Separator();

                    if (SliderTable()) {
                        SliderRow("Gold Value", params.value);

                        ImGui::EndTable();
                    }

                    ImGui::Checkbox("Modify Keywords", &params.bModifyKeywords);

                    if (ImGui::BeginTable("Crafting Table", 3, ImGuiTableFlags_SizingFixedFit)) {
                        ImGui::TableSetupColumn("CraftCol1");
                        ImGui::TableSetupColumn("CraftCol2");
                        ImGui::TableSetupColumn("CraftCol3");

                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::Checkbox("Modify Temper Recipe", &params.temper.bModify);
                        ImGui::BeginDisabled(!params.temper.bModify);
                        ImGui::TableNextColumn();
                        ImGui::Checkbox("Create recipe if missing##Temper", &params.temper.bNew);
                        MakeTooltip(
                            "If an item being modified lacks an existing temper recipe to modify, checking\n"
                            "this will create one automatically");
                        ImGui::TableNextColumn();
                        ImGui::Checkbox("Make free if no recipe to copy##Temper", &params.temper.bFree);
                        MakeTooltip(
                            "If the item being copied lacks a tempering recipe, checking will remove all\n"
                            "components and conditions to temper the modified item");
                        ImGui::EndDisabled();

                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::Checkbox("Modify Crafting Recipe", &params.craft.bModify);
                        ImGui::BeginDisabled(!params.craft.bModify);
                        ImGui::TableNextColumn();
                        ImGui::Checkbox("Create recipe if missing##Craft", &params.craft.bNew);
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip(
                                "If an item being modified lacks an existing crafting recipe to modify,\n"
                                "checking this will create one automatically");
                        ImGui::TableNextColumn();
                        ImGui::Checkbox("Make free if no recipe to copy##Craft", &params.craft.bFree);
                        if (ImGui::IsItemHovered())
                            ImGui::SetTooltip(
                                "If the item being copied lacks a crafting recipe, checking will remove all\n"
                                "components and conditions to craft the modified item");
                        ImGui::EndDisabled();

                        ImGui::EndTable();
                    }

                    ImGui::PopItemWidth();
                }
                ImGui::EndChild();

                ImGui::TableNextColumn();
                ImGui::Text("Items");

                // ImGui::SameLine();
                // RightButton("Settings");

                auto avail = ImGui::GetContentRegionAvail();
                avail.y -= ImGui::GetFontSize() * 1 + ImGui::GetStyle().FramePadding.y * 2;

                auto player = RE::PlayerCharacter::GetSingleton();
                decltype(params.items) finalItems;
                finalItems.reserve(params.items.size());

                bool modChangesDeleted = g_Data.modifiedFilesDeleted.contains(curMod->mod);

                if (ImGui::BeginListBox("##Items", avail)) {
                    if (ImGui::BeginPopupContextWindow()) {
                        if (ImGui::Selectable("Enable all")) uncheckedItems.clear();
                        if (ImGui::Selectable("Disable all"))
                            for (auto i : params.items) uncheckedItems.insert(i);
                        ImGui::Separator();

                        ImGui::BeginDisabled(selectedItems.empty());
                        if (ImGui::Selectable("Enable selected")) {
                            for (auto i : selectedItems) uncheckedItems.erase(i);
                        }
                        if (ImGui::Selectable("Disable selected")) {
                            for (auto i : selectedItems) uncheckedItems.insert(i);
                        }
                        ImGui::EndDisabled();

                        ImGui::EndPopup();
                    }

                    if (!selectedItems.contains(lastSelectedItem)) lastSelectedItem = nullptr;

                    // Clear out selections that are no longer visible
                    auto lastSelected(std::move(selectedItems));
                    for (auto i : params.items)
                        if (lastSelected.contains(i)) selectedItems.insert(i);

                    for (auto i : params.items) {
                        int pop = 0;
                        if (g_Data.modifiedItems.contains(i)) {
                            if (modChangesDeleted)
                                ImGui::PushStyleColor(ImGuiCol_Text, colorDeleted);
                            else
                                ImGui::PushStyleColor(ImGuiCol_Text, colorChanged);
                            pop++;
                        } else if (g_Data.modifiedItemsShared.contains(i)) {
                            ImGui::PushStyleColor(ImGuiCol_Text, colorChangedShared);
                            pop++;
                        } else if (!WillBeModified(i, remappedSrc)) {
                            ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                            pop++;
                        }

                        std::string name(i->GetName());
                        // if (!i->fullName.empty()) name = i->GetFullName();

                        if (name.empty()) name = std::format("{}:{:010x}", i->GetFile(0)->fileName, i->formID);

                        ImGui::BeginGroup();

                        bool isChecked = !uncheckedItems.contains(i);
                        ImGui::PushID(name.c_str());
                        if (ImGui::Checkbox("##ItemCheckbox", &isChecked)) {
                            if (!isChecked)
                                uncheckedItems.insert(i);
                            else
                                uncheckedItems.erase(i);
                        }
                        if (isChecked) finalItems.push_back(i);

                        // ImGui::PopID();

                        ImGui::SameLine();

                        bool selected = selectedItems.contains(i);

                        // ImGui::PushID(name.c_str());
                        if (ImGui::Selectable(name.c_str(), &selected, ImGuiSelectableFlags_AllowDoubleClick)) {
                            if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                                if (!isInventoryOpen) {
                                    if (isShiftDown) {
                                        if (isAltDown) {
                                            auto armorSet = BuildSetFrom(i, params.items);

                                            if (!isCtrlDown) selectedItems.clear();
                                            for (auto piece : armorSet) selectedItems.insert(piece);
                                        }
                                    } else {
                                        if (isAltDown) {
                                            auto armorSet = BuildSetFrom(i, params.items);
                                            givenItems.UnequipCurrent();
                                            for (auto piece : armorSet) givenItems.Give(piece, true);
                                        } else {
                                            givenItems.Give(i, true);
                                        }
                                    }
                                }
                            } else {
                                if (!isCtrlDown) selectedItems.clear();

                                if (!isShiftDown) {
                                    if (selected)
                                        selectedItems.insert(i);
                                    else
                                        selectedItems.erase(i);

                                    lastSelectedItem = i;
                                } else {
                                    if (lastSelectedItem) {
                                        bool adding = false;
                                        for (auto j : params.items) {
                                            if (j == i || j == lastSelectedItem) {
                                                if (adding) {
                                                    selectedItems.insert(j);
                                                    break;
                                                }
                                                adding = true;
                                            }

                                            if (adding) selectedItems.insert(j);
                                        }
                                    }
                                }
                            }
                        }

                        ImGui::PopID();
                        ImGui::EndGroup();
                        ImGui::PopStyleColor(pop);
                    }
                    ImGui::EndListBox();
                }

                ImGui::BeginDisabled(!player || (!curMod && (!bFilterAllArmor || !bFilterAllWeapons)) || isInventoryOpen);

                if (ImGui::Button(selectedItems.empty() ? "Give All" : "Give Selected")) {
                    if (selectedItems.empty())
                        for (auto i : params.items) {
                            givenItems.Give(i);
                        }
                    else
                        for (auto i : selectedItems) {
                            givenItems.Give(i);
                        }
                }
                if (isInventoryOpen) MakeTooltip("Can't use while inventory is open");

                ImGui::SameLine();
                if (ImGui::Button(selectedItems.empty() ? "Equip All" : "Equip Selected")) {
                    givenItems.UnequipCurrent();
                    if (selectedItems.empty())
                        for (auto i : params.items) {
                            givenItems.Give(i, true);
                        }
                    else
                        for (auto i : selectedItems) {
                            givenItems.Give(i, true);
                        }
                }
                if (isInventoryOpen) MakeTooltip("Can't use while inventory is open");

                ImGui::BeginDisabled(givenItems.items.empty());
                ImGui::SameLine();
                if (ImGui::Button("Delete Given")) {
                    givenItems.Remove();
                }
                if (isInventoryOpen) MakeTooltip("Can't use while inventory is open");

                ImGui::EndDisabled();  // givenItems.empty()
                ImGui::EndDisabled();  //! player

                params.items = std::move(finalItems);

                bSlotWarning = false;
                for (auto i : params.items) {
                    if (auto armor = i->As<RE::TESObjectARMO>()) {
                        auto itemSlots = MapFindOr(g_Data.modifiedArmorSlots, armor, (ArmorSlots)armor->GetSlotMask());
                        if ((~g_Config.usedSlotsMask) & (~remappedSrc) & itemSlots) {
                            bSlotWarning = true;
                            break;
                        }
                    }
                }

                ImGui::EndTable();
            }

            ImGui::PopItemWidth();
        } else {
            ImGui::Text(g_Config.strCriticalError.c_str());
        }

        if (popupSettings) ImGui::OpenPopup("Settings");
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

        bool bPopupActive = true;
        if (ImGui::BeginPopupModal("Settings", &bPopupActive, ImGuiWindowFlags_AlwaysAutoResize)) {
            const char* logLevels[] = {"Trace", "Debug", "Info", "Warn", "Error", "Critical", "Off"};

            ImGui::Text("Log verbsity");
            ImGui::SameLine();
            if (ImGui::BeginCombo("##Verbosity", logLevels[g_Config.verbosity], ImGuiComboFlags_PopupAlignLeft)) {
                for (int i = 0; i < spdlog::level::n_levels; i++) {
                    bool selected = g_Config.verbosity == i;
                    if (ImGui::Selectable(logLevels[i], selected)) {
                        g_Config.verbosity = i;
                        spdlog::set_level((spdlog::level::level_enum)i);
                    }
                    if (selected) ImGui::SetItemDefaultFocus();
                }

                ImGui::EndCombo();
            }

            ImGui::Checkbox("Close console after qar command", &g_Config.bCloseConsole);
            ImGui::Checkbox("Delete given items after applying changes", &g_Config.bAutoDeleteGiven);
            ImGui::Checkbox("Round weights to 0.1", &g_Config.bRoundWeight);
            ImGui::Checkbox("Reset sliders after changing mods", &g_Config.bResetSliders);
            ImGui::Checkbox("Reset slot remapping after changing mods", &g_Config.bResetSlotRemap);
            ImGui::Checkbox("Highlight things you may want to look at", &g_Config.bHighlights);

            ImGui::Separator();
            ImGui::Checkbox("Enforce loot rarity with empty loot", &g_Config.bEnableRarityNullLoot);
            MakeTooltip(
                "Example: The subset of items elible for loot has 1 rare item, 0 commons and uncommons\n"
                "DISABLED: You will always get that rare item\n"
                "ENABLED: You will have a 5%% chance at the item, and 95%% chance of nothing");

            ImGui::Checkbox("Normalize drop rates between mods", &g_Config.bNormalizeModDrops);
            ImGui::SliderFloat("Adjust drop rates", &g_Config.fDropRates, 0.0f, 300.0f, "%.0f%%",
                               ImGuiSliderFlags_AlwaysClamp);
            ImGui::SliderInt("Drop curve level granularity", &g_Config.levelGranularity, 1, 5, "%d",
                             ImGuiSliderFlags_AlwaysClamp);
            MakeTooltip("A lower number generates more loot lists but more accurate distribution");

            ImGui::Separator();
            ImGui::Text("Only generate crafting recipes for");
            ImGui::SameLine();

            if (ImGui::BeginCombo("##Rarity", rarity[g_Config.craftingRarityMax], ImGuiComboFlags_PopupAlignLeft)) {
                for (int i = 0; rarity[i]; i++) {
                    bool selected = i == g_Config.craftingRarityMax;
                    if (ImGui::Selectable(rarity[i], selected)) g_Config.craftingRarityMax = i;
                    if (selected) ImGui::SetItemDefaultFocus();
                }

                ImGui::EndCombo();
            }

            ImGui::SameLine();
            ImGui::Text("rarity and below");

            ImGui::Indent();
            ImGui::Checkbox("Disable existing crafting recipies above that rarity",
                            &g_Config.bDisableCraftingRecipesOnRarity);
            ImGui::Unindent();

            ImGui::Checkbox("Keep crafting books as requirement", &g_Config.bKeepCraftingBooks);

            if (ImGui::BeginTable(
                    "Permissions", 2,
                    ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingFixedFit | ImGuiTableColumnFlags_NoReorder)) {
                ImGui::TableSetupColumn("Local Changes Permissions");
                ImGui::TableSetupColumn("Shared Changes Permissions");

                ImGui::TableHeadersRow();
                ImGui::TableNextRow();

                ImGui::TableNextColumn();
                PermissionsChecklist("Local", g_Config.permLocal);

                ImGui::TableNextColumn();
                PermissionsChecklist("Shared", g_Config.permShared);

                ImGui::EndTable();
            }

            ImGui::EndPopup();
        }
        if (!bPopupActive) g_Config.Save();

        if (popupRemapSlots) ImGui::OpenPopup("Remap Slots");

        ImGui::SetNextWindowSizeConstraints({300, 500}, {1600, 1000});
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        bPopupActive = true;
        if (ImGui::BeginPopupModal("Remap Slots", &bPopupActive, ImGuiWindowFlags_NoScrollbar)) {
            static int nSlotView = 33;
            uint64_t slotsUsed = 0;

            std::vector<RE::TESObjectARMO*> lsSlotItems;
            for (auto i : g_Config.acParams.items) {
                if (auto armor = i->As<RE::TESObjectARMO>()) {
                    auto itemSlots = MapFindOr(g_Data.modifiedArmorSlots, armor, (ArmorSlots)armor->GetSlotMask());
                    slotsUsed |= itemSlots;
                    if (itemSlots & ((uint64_t)1 << nSlotView)) lsSlotItems.push_back(armor);
                }
            }

            const char* strSlotDesc[] = {"Slot 30 - Head",       "Slot 31 - Hair",       "Slot 32 - Body",
                                         "Slot 33 - Hands",      "Slot 34 - Forearms",   "Slot 35 - Amulet",
                                         "Slot 36 - Ring",       "Slot 37 - Feet",       "Slot 38 - Calves",
                                         "Slot 39 - Shield",     "Slot 40 - Tail",       "Slot 41 - Long Hair",
                                         "Slot 42 - Circlet",    "Slot 43 - Ears",       "Slot 44 - Face",
                                         "Slot 45 - Neck",       "Slot 46 - Chest",      "Slot 47 - Back",
                                         "Slot 48 - ???",        "Slot 49 - Pelvis",     "Slot 50 - Decapitated Head",
                                         "Slot 51 - Decapitate", "Slot 52 - Lower body", "Slot 53 - Leg (right)",
                                         "Slot 54 - Leg (left)", "Slot 55 - Face2",      "Slot 56 - Chest2",
                                         "Slot 57 - Shoulder",   "Slot 58 - Arm (left)", "Slot 59 - Arm (right)",
                                         "Slot 60 - ???",        "Slot 61 - ???",        "<REMOVE SLOT>"};

            nSlotView = 33;
            if (auto payload = ImGui::GetDragDropPayload()) {
                if (payload->IsDataType("ARMOR SLOT")) {
                    nSlotView = *(int*)payload->Data;
                }
            }

            ImGui::Text("Click and drag from the original slot on the left to the new replacement slot on the right");
            ImGui::PushItemWidth(-FLT_MIN);

            if (ImGui::BeginTable("Slot Mapping", 3,
                                  ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit |
                                      ImGuiTableFlags_PadOuterX | ImGuiTableFlags_PreciseWidths |
                                      ImGuiTableFlags_ScrollY)) {
                ImGui::TableSetupColumn("Original");
                ImGui::TableSetupColumn("Items", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Remapped");

                ImGui::TableHeadersRow();
                ImGui::TableNextRow();

                ImVec2 srcCenter[33];
                ImVec2 tarCenter[33];

                for (int i = 0; i < 33; i++) {
                    ImGui::TableNextColumn();

                    if (i < 32) {
                        ImGui::BeginDisabled((((uint64_t)1 << i) & slotsUsed) == 0);
                        ImGui::BeginGroup();

                        const char* strWarn = nullptr;
                        int popCol = 0;
                        if ((((uint64_t)1 << i) & remappedSrc)) {
                            ImGui::PushStyleColor(ImGuiCol_Text, colorChanged);
                            popCol++;
                        } else if ((((uint64_t)1 << i) & slotsUsed & ~(uint64_t)g_Config.usedSlotsMask)) {
                            ImGui::PushStyleColor(ImGuiCol_Text, colorDeleted);
                            strWarn =
                                "Warning: Items in this slot will not be changed unless remapped to another slot.";
                            popCol++;
                        } else if ((1 << i) & slotsUsed & (remappedTar & ~remappedSrc)) {
                            ImGui::PushStyleColor(ImGuiCol_Text, colorDeleted);
                            strWarn =
                                "Warning: Other items are being remapped to this slot.\n"
                                "This will cause conflicts unless this slot is also remapped.";
                            popCol++;
                        }

                        bool bSelected = false;
                        if (ImGui::Selectable(strSlotDesc[i], &bSelected, 0)) {
                        }

                        if (strWarn) MakeTooltip(strWarn);

                        if (ImGui::BeginDragDropSource(0)) {
                            nSlotView = i;
                            g_Config.acParams.mapArmorSlots.erase(i);
                            ImGui::SetDragDropPayload("ARMOR SLOT", &i, sizeof(i));
                            ImGui::Text(strSlotDesc[i]);
                            ImGui::EndDragDropSource();
                        }

                        ImGui::SameLine();

                        float w = ImGui::GetFontSize() * 1 + ImGui::GetStyle().FramePadding.x * 2;
                        ImGui::SetCursorPosX(ImGui::GetCursorPosX() +
                                             std::max(0.0f, ImGui::GetContentRegionAvail().x - w));
                        ImGui::SetCursorPosY(ImGui::GetCursorPosY() -
                                             4.0f);  // Radio circles are weirdly offset down slightly?

                        ImGui::PushID("Source");
                        ImGui::PushID(i);
                        bool bCheckbox = false;
                        if (ImGui::RadioButton("##ItemCheckbox", &bCheckbox)) {
                        }
                        srcCenter[i] = (ImGui::GetItemRectMin() + ImGui::GetItemRectMax()) / 2;
                        ImGui::PopID();
                        ImGui::PopID();
                        ImGui::PopStyleColor(popCol);

                        ImGui::EndGroup();
                        if (ImGui::IsItemHovered()) nSlotView = i;

                        ImGui::EndDisabled();
                    }

                    ImGui::TableNextColumn();
                    if (i < lsSlotItems.size()) ImGui::Text(lsSlotItems[i]->GetName());

                    ImGui::TableNextColumn();
                    auto bDisabled = i != 32 && ((1 << i) & g_Config.usedSlotsMask) == 0;
                    ImGui::BeginDisabled(bDisabled);
                    ImGui::BeginGroup();

                    bool bWarn = false;

                    int popCol = 0;
                    if ((((uint64_t)1 << i) & slotsUsed & (remappedTar & ~remappedSrc))) {
                        ImGui::PushStyleColor(ImGuiCol_Text, colorDeleted);
                        bWarn = true;
                        popCol++;
                    } else if ((((uint64_t)1 << i) & remappedTar)) {
                        ImGui::PushStyleColor(ImGuiCol_Text, colorChanged);
                        popCol++;
                    }

                    ImGui::PushID("Target");
                    ImGui::PushID(i);
                    bool bCheckbox = false;
                    bool bSelected = false;
                    if (ImGui::Selectable("##Select", &bSelected, 0)) {
                    }
                    if (bWarn)
                        MakeTooltip(
                            "Warning: Items are being remapped to this slot, but other items are already using this "
                            "slot.");

                    if (!bDisabled && ImGui::BeginDragDropTarget()) {
                        if (auto payload = ImGui::AcceptDragDropPayload("ARMOR SLOT")) {
                            g_Config.acParams.mapArmorSlots[*(int*)payload->Data] = i;
                        }
                        ImGui::EndDragDropTarget();
                    }
                    ImGui::SameLine();
                    if (ImGui::RadioButton(strSlotDesc[i], &bCheckbox)) {
                    }
                    tarCenter[i] = (ImGui::GetItemRectMin() + ImGui::GetItemRectMax()) / 2;
                    tarCenter[i].x = ImGui::GetItemRectMin().x + ImGui::GetItemRectMax().y -
                                     tarCenter[i].y;  // Want center of the circle
                    ImGui::PopID();
                    ImGui::PopID();

                    ImGui::PopStyleColor(popCol);
                    ImGui::EndGroup();

                    ImGui::EndDisabled();
                    if (ImGui::IsItemHovered()) nSlotView = i;
                }

                ImGui::EndTable();

                constexpr auto lineWidth = 3.0f;
                auto draw = ImGui::GetWindowDrawList();

                if (auto payload = ImGui::GetDragDropPayload()) {
                    if (payload->IsDataType("ARMOR SLOT")) {
                        draw->AddLine(srcCenter[*(int*)payload->Data], ImGui::GetMousePos(), colorChanged, lineWidth);
                    }
                }

                for (auto i : g_Config.acParams.mapArmorSlots) {
                    draw->AddLine(srcCenter[i.first], tarCenter[i.second], colorChanged, lineWidth);
                }
            }

            ImGui::EndPopup();
        }
    }

    ImGuiIntegration::BlockInput(!ImGui::IsWindowCollapsed(), ImGui::IsItemHovered());
    ImGui::End();

    if (!isActive) ImGuiIntegration::Show(false);
}
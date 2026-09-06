#include "UI.h"
#include "NPCTargets.h"

#include "ArmorChanger.h"
#include "ArmorSetBuilder.h"
#include "Config.h"
#include "ConsoleCommands.h"
#include "Data.h"
#include "Enchantments.h"
#include "ImGui/imgui_impl_dx11.h"
#include "ImGuiIntegration.h"
#include "Localization.h"
#include "ModIntegrations.h"
#include "Serialization.h"
// #include "ImGui/imgui_freetype.h"

#include "NameParsing.h"

#include <random>

using namespace QuickArmorRebalance;
constexpr auto LZ = QuickArmorRebalance::Localize;

constexpr int kItemApplyWarningThreshhold = 100;

/*
const char* strSlotDesc[] = {
    "Slot 30 - Head",       "Slot 31 - Hair",        "Slot 32 - Body",        "Slot 33 - Hands",      "Slot 34 - Forearms",  "Slot 35 - Amulet",  "Slot 36 - Ring",
    "Slot 37 - Feet",       "Slot 38 - Calves",      "Slot 39 - Shield",      "Slot 40 - Tail",       "Slot 41 - Long Hair", "Slot 42 - Circlet", "Slot 43 - Ears",
    "Slot 44 - Face",       "Slot 45 - Neck",        "Slot 46 - Chest",       "Slot 47 - Back",       "Slot 48 - ???",       "Slot 49 - Pelvis",  "Slot 50 - Decapitated Head",
    "Slot 51 - Decapitate", "Slot 52 - Lower body",  "Slot 53 - Leg (right)", "Slot 54 - Leg (left)", "Slot 55 - Face2",     "Slot 56 - Chest2",  "Slot 57 - Shoulder",
    "Slot 58 - Arm (left)", "Slot 59 - Arm (right)", "Slot 60 - ???",         "Slot 61 - ???",        "<REMOVE SLOT>"};
    */

static ImVec2 operator+(const ImVec2& a, const ImVec2& b) { return {a.x + b.x, a.y + b.y}; }
static ImVec2 operator/(const ImVec2& a, int b) { return {a.x / b, a.y / b}; }

template <typename... Args>
std::string LZFormat(const char* rt_fmt_str, Args&&... args) {
    return std::vformat(LZ(rt_fmt_str), std::make_format_args(args...));
}

bool StringContainsI(const char* s1, const char* s2) {
    std::string str1(s1), str2(s2);
    ToLower(str1);
    ToLower(str2);
    return str1.contains(str2);
}

void MakeTooltip(const char* str, bool delay = false) {
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled | (delay ? ImGuiHoveredFlags_DelayNormal : 0))) ImGui::SetTooltip(str);
}

struct TriStateCheckbox {
    static const int kTrue = 1;
    static const int kFalse = 0;
    static const int kEither = 2;

    static bool Insert(const char* label, int* state) {
        static const int triState[] = {0, -1, 2};
        int local = triState[*state];
        bool bRet = false;
        if (ImGui::CheckboxFlags(label, &local, 3)) {
            *state = (1 + *state) % 3;
            bRet = true;
        }

        if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
            *state = 2;
            bRet = true;
        }

        return bRet;
    }
};

bool MenuItemConfirmed(const char* str) {
    bool bRet = false;
    if (ImGui::BeginMenu(str)) {
        if (ImGui::Selectable("Yes")) bRet = true;
        ImGui::Selectable("No");
        ImGui::EndMenu();
    }
    return bRet;
}

struct ModFilterSettings {
    int any = TriStateCheckbox::kEither;
    int stats = TriStateCheckbox::kEither;
    int slots = TriStateCheckbox::kEither;
    int keywords = TriStateCheckbox::kEither;
    int loot = TriStateCheckbox::kEither;
    int survival = TriStateCheckbox::kEither;
    int recipes = TriStateCheckbox::kEither;
    int region = TriStateCheckbox::kEither;

    ModFilterSettings() { Reset(); }

    void Reset() {
        any = stats = slots = keywords = loot = survival = recipes = region = TriStateCheckbox::kEither;
        Build();
    }

    void Build() {
        enabledFlags = disabledFlags = 0;

        SetFlag(stats, eChange_Stats);
        SetFlag(slots, eChange_Slots);
        SetFlag(keywords, eChange_Keywords);
        SetFlag(loot, eChange_Loot);
        SetFlag(survival, eChange_Survival);
        SetFlag(recipes, eChange_Recipes);
        SetFlag(region, eChange_Region);
    }

    bool Test(unsigned int flags) const {
        switch (any) {
            case TriStateCheckbox::kTrue:
                if (!flags) return false;
                break;
            case TriStateCheckbox::kFalse:
                if (flags) return false;
                break;
        }

        if ((flags & enabledFlags) != enabledFlags) return false;
        if ((flags & disabledFlags) != 0) return false;

        return true;
    }

    bool Active() { return any != TriStateCheckbox::kEither || enabledFlags || disabledFlags; }

protected:
    void SetFlag(int state, unsigned int flag) {
        if (state == TriStateCheckbox::kTrue)
            enabledFlags |= flag;
        else if (state == TriStateCheckbox::kFalse)
            disabledFlags |= flag;
    }

    unsigned int enabledFlags = 0;
    unsigned int disabledFlags = 0;
};

bool DoClearFilter() { return ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right); }

std::vector<ModData*> GetFilteredMods(int nModFilter, const char* nameFilter, ModFilterSettings& modFilters) {
    std::vector<ModData*> list;
    list.reserve(g_Data.sortedMods.size());
    switch (nModFilter) {
        case 0:
            if (!*nameFilter)
                list = g_Data.sortedMods;
            else
                std::copy_if(g_Data.sortedMods.begin(), g_Data.sortedMods.end(), std::back_inserter(list),
                             [=](ModData* mod) { return StringContainsI(mod->mod->fileName, nameFilter); });
            break;
        case 1: {
            static bool bOnce = false;
            if (!bOnce) {
                bOnce = true;

                for (auto& i : g_Data.modData) {
                    if (i.second->bModified && !i.second->bHasDynamicVariants) {
                        AnalyzeResults results;
                        std::vector<RE::TESBoundObject*> items(i.second->items.begin(), i.second->items.end());
                        AnalyzeArmor(items, results);
                        i.second->bHasPotentialDVs = !results.sets[0].empty() || !results.sets[1].empty();
                    }
                }
            }
        }
            std::copy_if(g_Data.sortedMods.begin(), g_Data.sortedMods.end(), std::back_inserter(list), [=](ModData* mod) {
                return mod->bModified && !mod->bHasDynamicVariants && mod->bHasPotentialDVs && (!*nameFilter || StringContainsI(mod->mod->fileName, nameFilter));
            });
            break;
    }

    if (!modFilters.Active()) return list;

    std::vector<ModData*> ret;
    ret.reserve(list.size());
    std::copy_if(list.begin(), list.end(), std::back_inserter(ret), [&](ModData* mod) { return modFilters.Test(mod->changes); });

    return ret;
}

struct ItemFilter {
    char nameFilter[200]{""};

    enum TypeFilter { ItemType_All, ItemType_Armor, ItemType_Weapon, ItemType_Ammo };

    int nType = 0;

    enum SlotFilterMode { SlotsAny, SlotsAll, SlotsNot };

    int slotMode = SlotsAny;
    ArmorSlots slots = 0;

    bool bArmorClothing = true;
    bool bArmorLight = true;
    bool bArmorHeavy = true;

    int bEnchanted = 2;
    int bFavorite = 2;  // 0=not favorites, 1=favorites only, 2=both/ignore
    bool bGroupVariants = false;  // When true, group items like "X of Y" and show only one per base name
    ModFilterSettings modFilters;

    // Tag filter: map of tag name -> filter state (0=exclude, 1=include, 2=ignore)
    std::map<std::string, int> tagFilters;

    bool HasActiveTagFilter() const {
        for (const auto& [tag, state] : tagFilters) {
            if (state != 2) return true;
        }
        return false;
    }

    bool Pass(RE::TESBoundObject* obj) const {
        // Run these fastest to slowest
        switch (nType) {
            case ItemType_Armor:
                if (!obj->As<RE::TESObjectARMO>()) return false;
            case ItemType_All: {
                if (auto armor = obj->As<RE::TESObjectARMO>()) {
                    switch (armor->bipedModelData.armorType.get()) {
                        case RE::BIPED_MODEL::ArmorType::kClothing:
                            if (!bArmorClothing) return false;
                            break;
                        case RE::BIPED_MODEL::ArmorType::kLightArmor:
                            if (!bArmorLight) return false;
                            break;
                        case RE::BIPED_MODEL::ArmorType::kHeavyArmor:
                            if (!bArmorHeavy) return false;
                            break;
                        default:
                            return false;
                    }
                }
            } break;
            case ItemType_Weapon:
                if (!obj->As<RE::TESObjectWEAP>()) return false;
                break;
            case ItemType_Ammo:
                if (!obj->As<RE::TESAmmo>()) return false;
                break;
        }

        if (*nameFilter && !StringContainsI(obj->GetName(), nameFilter)) return false;

        // Check item blacklist (case-insensitive)
        if (!g_Config.itemBlacklist.empty()) {
            const char* itemName = obj->GetName();
            if (itemName && *itemName) {
                std::string lowerName = itemName;
                for (char& c : lowerName) {
                    if (c >= 'A' && c <= 'Z') c += 32;
                }
                for (const auto& blacklistStr : g_Config.itemBlacklist) {
                    if (lowerName.find(blacklistStr) != std::string::npos) {
                        return false;
                    }
                }
            }
        }

        if (slots) {
            if (auto armor = obj->As<RE::TESObjectARMO>()) {
                auto s = MapFindOr(g_Data.modifiedArmorSlots, armor, (ArmorSlots)armor->GetSlotMask().underlying());
                switch (slotMode) {
                    case SlotsAny:
                        if ((s & slots) == 0) return false;
                        break;
                    case SlotsAll:
                        if ((s & slots) != slots) return false;
                        break;
                    case SlotsNot:
                        if ((s & slots) != 0) return false;
                        break;
                }
            } else
                return false;
        }

        if (bEnchanted != 2 && !!bEnchanted != IsEnchanted(obj)) return false;

        if (!modFilters.Test(MapFindOr(g_Data.modifiedItems, obj, 0u) | MapFindOr(g_Data.modifiedItemsShared, obj, 0u))) return false;

        if (bFavorite != 2) {
            bool isFavorite = g_Data.favoriteItems.contains(obj);
            if (bFavorite == 1 && !isFavorite) return false;  // Favorites only, but not a favorite
            if (bFavorite == 0 && isFavorite) return false;   // Not favorites, but is a favorite
        }

        // Tag filter check
        if (!tagFilters.empty()) {
            const std::set<std::string>* itemTags = GetItemTags(obj);

            for (const auto& [tag, state] : tagFilters) {
                if (state == 2) continue;  // Ignore

                bool hasTag = itemTags && itemTags->contains(tag);
                if (state == 1 && !hasTag) return false;  // Include filter, but item doesn't have tag
                if (state == 0 && hasTag) return false;   // Exclude filter, but item has tag
            }
        }

        return true;
    }

    // Extract base name for grouping variants (e.g., "Iron Sword of Fire" -> "Iron Sword")
    // Returns the original name if no " of " pattern is found
    static std::string GetBaseName(const char* name) {
        if (!name || !*name) return "";
        std::string str(name);

        // Look for " of " pattern (case-insensitive)
        size_t pos = str.find(" of ");
        if (pos == std::string::npos) {
            // Try uppercase
            pos = str.find(" Of ");
        }
        if (pos == std::string::npos) {
            // Try " the " as an alternative pattern for names like "Armor of the Old Gods"
            // Actually, let's keep it simple and just use " of "
            return str;
        }

        return str.substr(0, pos);
    }
};

// Caps a window to the game's resolution rather than a fixed size, so larger displays can use larger windows
void SetWindowSizeLimits(float minW, float minH) {
    const auto vp = ImGui::GetMainViewport()->Size;
    ImGui::SetNextWindowSizeConstraints({minW, minH}, {std::max(minW, vp.x), std::max(minH, vp.y)});
}

const char* RightAlign(const char* text, float extra = 0.0f) {
    float w = extra + ImGui::CalcTextSize(text).x + ImGui::GetStyle().FramePadding.x * 2.f;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - w);
    return text;
}

struct PauseTracker {
    bool bPaused = false;
    bool bSkipFrame = false;

    void SkipFrame() { bSkipFrame = true; }

    void Pause() {
        if (bPaused) return;
        bPaused = true;
        RE::UI::GetSingleton()->numPausesGame++;
    }

    void Unpause() {
        if (!bPaused) return;
        bPaused = false;
        RE::UI::GetSingleton()->numPausesGame--;
    }

    void Update(bool bWant) {
        if (bWant && !bSkipFrame)
            Pause();
        else
            Unpause();
        bSkipFrame = false;
    }
};

PauseTracker g_Pause;

// Global flag to signal equipment changes for cache refresh
static bool g_EquipmentChanged = false;

// Last explicitly equipped outfit name (for Numpad1 restore)
static std::string g_LastEquippedOutfit;

// Cache for dynamically created enhanced armor forms
// Key: "baseFormID:enchFormID:armorRating:weight:value"
static std::unordered_map<std::string, RE::TESObjectARMO*> g_EnhancedArmorCache;
// NPC recipes use a separate cache: background persistence must not mutate the
// player's UI cache or register NPC equipment in the player's cosave.
static std::unordered_map<std::string, RE::TESObjectARMO*> g_NPCEnhancedArmorCache;

// Create a cache key for enhanced armor
std::string MakeEnhancedArmorKey(RE::TESObjectARMO* baseArmor, const EnhancedItemConfig& config) {
    std::string key = std::format("{:x}", baseArmor->GetFormID());
    if (config.HasEnchantment()) {
        key += ":" + config.enchantmentFormID;
        key += std::format(":{:.2f}", config.enchantmentMagnitude);
    }
    if (config.HasStatsTransfer()) {
        key += std::format(":AR{}", config.armorRating.value_or(0));
        key += std::format(":W{:.1f}", config.weight.value_or(0.0f));
        key += std::format(":V{}", config.value.value_or(0));
    }
    return key;
}

// Create a dynamically duplicated armor form with enchantment and/or modified stats
RE::TESObjectARMO* CreateEnhancedArmor(RE::TESObjectARMO* baseArmor, const EnhancedItemConfig& config, bool trackPlayer) {
    if (!baseArmor || !config.IsEnhanced()) return nullptr;

    // Check cache first
    std::string cacheKey = MakeEnhancedArmorKey(baseArmor, config);
    auto& cache = trackPlayer ? g_EnhancedArmorCache : g_NPCEnhancedArmorCache;
    static std::uint64_t npcCacheGeneration = 0;
    if (!trackPlayer && npcCacheGeneration != NPCTargets::Generation()) {
        cache.clear();
        npcCacheGeneration = NPCTargets::Generation();
    }
    auto cacheIt = cache.find(cacheKey);
    if (cacheIt != cache.end()) {
        logger::trace("CreateEnhancedArmor: Using cached form for {}", baseArmor->GetName());
        return cacheIt->second;
    }

    // Create duplicate of the base armor
    auto duplicate = baseArmor->CreateDuplicateForm(false, nullptr);
    if (!duplicate) {
        logger::warn("CreateEnhancedArmor: Failed to duplicate form for {}", baseArmor->GetName());
        return nullptr;
    }

    auto enhancedArmor = duplicate->As<RE::TESObjectARMO>();
    if (!enhancedArmor) {
        logger::warn("CreateEnhancedArmor: Duplicate is not armor for {}", baseArmor->GetName());
        return nullptr;
    }

    // Apply enchantment
    if (config.HasEnchantment()) {
        if (auto enchForm = LookupForm(config.enchantmentFormID)) {
            if (auto enchantment = enchForm->As<RE::EnchantmentItem>()) {
                enhancedArmor->formEnchanting = enchantment;
                // Track this so we can clean up if the dynamic form is lost on reload
                if (trackPlayer) TrackAppliedEnchantment(enhancedArmor, enchantment, baseArmor);
                logger::trace("CreateEnhancedArmor: Applied enchantment {} to {}",
                    enchantment->GetName(), baseArmor->GetName());
            }
        }
    }

    // Apply stats transfer
    if (config.HasStatsTransfer()) {
        logger::info("CreateEnhancedArmor: Applying stats transfer to {}", baseArmor->GetName());
        logger::info("  Base armor - armorRating field: {}, GetArmorRating(): {}, weight: {}, value: {}",
            baseArmor->armorRating, baseArmor->GetArmorRating(), baseArmor->weight, baseArmor->value);

        if (config.armorRating.has_value()) {
            // armorRating field is stored at 100x the displayed value
            enhancedArmor->armorRating = static_cast<float>(config.armorRating.value()) * 100.0f;
            logger::info("  Set armorRating field to {} (from config {} * 100)",
                enhancedArmor->armorRating, config.armorRating.value());
        }
        if (config.weight.has_value()) {
            enhancedArmor->weight = config.weight.value();
            logger::info("  Set weight to {:.1f}", config.weight.value());
        }
        if (config.value.has_value()) {
            enhancedArmor->value = config.value.value();
            logger::info("  Set value to {}", config.value.value());
        }

        logger::info("  After modification - armorRating field: {}, GetArmorRating(): {}, weight: {}, value: {}",
            enhancedArmor->armorRating, enhancedArmor->GetArmorRating(),
            enhancedArmor->weight, enhancedArmor->value);
    }

    // Cache the enhanced form for reuse
    cache[cacheKey] = enhancedArmor;
    NPCTargets::RegisterEnhanced(enhancedArmor, baseArmor, config);
    logger::info("CreateEnhancedArmor: Created enhanced {} (cache key: {})",
        baseArmor->GetName(), cacheKey);

    return enhancedArmor;
}

struct GivenItems {
    bool NPCAction(NPCTargets::Action action, RE::TESBoundObject* item = nullptr, bool equip = false, bool reuse = false,
                   const EnhancedItemConfig* enhancement = nullptr) {
        if (!NPCTargets::IsNPC()) return false;
        NPCTargets::Request(action, item, equip, reuse, enhancement);
        g_EquipmentChanged = true;
        g_Pause.SkipFrame();
        return true;
    }

    void UnequipCurrent() {
        if (NPCAction(NPCTargets::Action::UnequipAll)) return;
        logger::trace("[GUI] UnequipCurrent called");
        stored.clear();
        if (auto player = NPCTargets::GetActor()) {
            logger::trace("[GUI] UnequipCurrent - using GetWornArmor for each slot");
            // Use GetWornArmor() for each biped slot instead of GetInventory()
            // This avoids the TESObjEx plugin hook crash
            std::set<RE::TESObjectARMO*> seenArmor;
            for (int slot = 0; slot < 32; slot++) {
                auto bipedSlot = static_cast<RE::BGSBipedObjectForm::BipedObjectSlot>(1 << slot);
                if (auto armor = player->GetWornArmor(bipedSlot)) {
                    if (seenArmor.insert(armor).second) {  // Only process if not seen before
                        stored.insert(armor);
                        logger::trace("[GUI] UnequipCurrent - unequipping {} from slot {}", armor->GetName(), slot);
                        RE::ActorEquipManager::GetSingleton()->UnequipObject(player, armor, nullptr, 1, armor->GetEquipSlot(), false, false, false);
                        g_Pause.SkipFrame();
                    }
                }
            }
            g_EquipmentChanged = true;  // Signal cache refresh needed
            logger::trace("[GUI] UnequipCurrent - done, {} items stored", stored.size());
        } else {
            logger::warn("[GUI] UnequipCurrent - player is null!");
        }
    }

    // Check if an armor item is currently worn by checking all biped slots
    // Avoids GetInventory() which can crash with other SKSE plugins
    bool IsArmorWorn(RE::TESObjectARMO* armor, RE::Actor* player) {
        if (!armor || !player) return false;
        for (int slot = 0; slot < 32; slot++) {
            auto bipedSlot = static_cast<RE::BGSBipedObjectForm::BipedObjectSlot>(1 << slot);
            if (player->GetWornArmor(bipedSlot) == armor) {
                return true;
            }
        }
        return false;
    }

    // Check if an item exists in the player's inventory
    // Uses GetItemCount instead of GetInventory to avoid plugin hook crashes
    bool FindInInventory(RE::TESBoundObject* item) {
        logger::trace("[GUI] FindInInventory called for item: {}", item ? item->GetName() : "null");
        if (!item) return false;

        auto player = NPCTargets::GetActor();
        if (!player) {
            logger::warn("[GUI] FindInInventory - player is null!");
            return false;
        }

        auto count = NPCTargets::ItemCount(player, item);
        logger::trace("[GUI] FindInInventory - item count: {}", count);
        return count > 0;
    }

    void Unequip(RE::TESBoundObject* item) {
        if (NPCAction(NPCTargets::Action::Unequip, item)) return;
        auto player = NPCTargets::GetActor();
        if (!player) return;

        if (auto armor = item->As<RE::TESObjectARMO>()) recentEquipSlots &= ~(ArmorSlots)armor->GetSlotMask().underlying();

        RE::ActorEquipManager::GetSingleton()->UnequipObject(player, item);
        g_EquipmentChanged = true;  // Signal cache refresh needed
        for (int f = 0; f < static_cast<int>(g_Config.outfitFrameMultiplier); f++) {
            g_Pause.SkipFrame();
        }
    }

    void Equip(RE::TESBoundObject* item) {
        if (NPCAction(NPCTargets::Action::Equip, item)) return;
        auto player = NPCTargets::GetActor();
        if (!player || !item) return;
        const auto handle = player->GetHandle();
        const auto generation = NPCTargets::Generation();

        if (auto armor = item->As<RE::TESObjectARMO>()) {
            auto slots = (unsigned int)armor->GetSlotMask().underlying();
            if ((slots & recentEquipSlots) == 0) {
                recentEquipSlots |= slots;

                // Not using AddTask will result in it not un-equipping current items
                SKSE::GetTaskInterface()->AddTask([handle, generation, item, armor]() {
                    if (generation != NPCTargets::Generation()) return;
                    auto actor = handle.get();
                    auto manager = RE::ActorEquipManager::GetSingleton();
                    if (actor && manager) manager->EquipObject(actor.get(), item, nullptr, 1, armor->GetEquipSlot(), false, false, false);
                });
            }
        } else {
            SKSE::GetTaskInterface()->AddTask([handle, generation, item]() {
                if (generation != NPCTargets::Generation()) return;
                auto actor = handle.get();
                auto manager = RE::ActorEquipManager::GetSingleton();
                if (actor && manager) manager->EquipObject(actor.get(), item);
            });
        }

        g_EquipmentChanged = true;  // Signal cache refresh needed
        for (int f = 0; f < static_cast<int>(g_Config.outfitFrameMultiplier); f++) {
            g_Pause.SkipFrame();
        }

    }

    void Restore() {
        if (NPCAction(NPCTargets::Action::Restore)) return;
        if (stored.empty()) return;

        auto target = NPCTargets::GetActor();
        if (!target) return;
        auto handle = target->GetHandle();
        auto generation = NPCTargets::Generation();
        auto restoreItems = std::move(stored);
        stored.clear();
        SKSE::GetTaskInterface()->AddTask([handle, generation, restoreItems = std::move(restoreItems)]() {
            if (generation != NPCTargets::Generation()) return;
            auto manager = RE::ActorEquipManager::GetSingleton();
            auto actor = handle.get();
            auto player = actor.get();
            if (!player || !manager) return;

            for (auto i : restoreItems) {
                RE::BGSEquipSlot* equipSlot = nullptr;
                if (auto e = i->As<RE::BGSEquipType>()) equipSlot = e->GetEquipSlot();

                manager->EquipObject(player, i, nullptr, 1, equipSlot, false, false, false);
            }
        });

        for (int f = 0; f < static_cast<int>(g_Config.outfitFrameMultiplier); f++) {
            g_Pause.SkipFrame();
        }
    }

    void Give(RE::TESBoundObject* item, bool equip = false, bool reuse = false, bool isEnhanced = false,
              const std::string& baseFormID = "", const std::string& enhancementKey = "") {
        if (NPCAction(NPCTargets::Action::Give, item, equip, reuse)) return;
        if (RE::UI::GetSingleton()->IsItemMenuOpen()) return;  // Can cause crashes
        if (!item) return;

        auto player = NPCTargets::GetActor();
        if (!player) return;

        if (!reuse || !FindInInventory(item)) {
            player->AddObjectToContainer(item, nullptr, 1, nullptr);
            items.push_back({ImGui::GetFrameCount(), item});

            // Track the given item for per-character persistence
            TrackGivenItem(item, isEnhanced, baseFormID, enhancementKey);

            for (int f = 0; f < static_cast<int>(g_Config.outfitFrameMultiplier); f++) {
                g_Pause.SkipFrame();
            }
        }

        // After much testing, Skyrim seems to REALLY not like equipping more then one item per
        // frame And the per frame limit counts while the console is open. Doing so results in it
        // letting you equip as many items in one slot as you'd like So instead, have to mimic
        // the expected behavior and hopefully its good enough

        if (equip) Equip(item);
    }

    // Give item with enchantment and/or modified stats from enhanced config
    // Creates a dynamic duplicate form with the enhancements baked in
    void GiveEnhanced(RE::TESBoundObject* item, const EnhancedItemConfig& config, bool equip = false) {
        if (NPCAction(NPCTargets::Action::Give, item, equip, true, &config)) return;
        if (RE::UI::GetSingleton()->IsItemMenuOpen()) return;
        if (!item) return;

        // Try to create an enhanced armor form if this is armor with enhancements
        RE::TESBoundObject* itemToGive = item;
        std::string baseFormID = QARFormID(item);
        std::string enhancementKey;
        bool isEnhanced = false;

        if (auto armor = item->As<RE::TESObjectARMO>()) {
            if (config.IsEnhanced()) {
                if (auto enhanced = CreateEnhancedArmor(armor, config)) {
                    itemToGive = enhanced;
                    isEnhanced = true;
                    // Build enhancement key for potential recreation
                    enhancementKey = MakeEnhancedArmorKey(armor, config);
                    logger::trace("GiveEnhanced: Using enhanced form for {}", armor->GetName());

                    // Track for SKSE cosave serialization (so it persists across save/load)
                    if (equip) {
                        TrackEquippedEnhancedArmor(armor, enhanced, config, true);
                    }
                }
            }
        }

        // Give the (possibly enhanced) item with tracking info
        Give(itemToGive, equip, false, isEnhanced, baseFormID, enhancementKey);
    }

    void ToggleEquip(RE::TESBoundObject* item) {
        if (NPCAction(NPCTargets::Action::Toggle, item)) return;
        // if (RE::UI::GetSingleton()->IsItemMenuOpen()) return;  //I think this is safe, only if we're not adding /
        // removing from the list
        if (!item) return;

        auto player = NPCTargets::GetActor();
        if (!player) return;

        if (FindInInventory(item)) {
            // Check if item is worn - for armor, use IsArmorWorn to avoid GetInventory
            bool isWorn = false;
            if (auto armor = item->As<RE::TESObjectARMO>()) {
                isWorn = IsArmorWorn(armor, player);
            }
            // For non-armor items (weapons, etc), check equipped objects
            if (!isWorn) {
                for (bool leftHand : {false, true}) {
                    if (player->GetEquippedObject(leftHand) == item) {
                        isWorn = true;
                        break;
                    }
                }
            }

            if (isWorn)
                Unequip(item);
            else
                Equip(item);

            return;
        }

        // Doesn't already have item

        Give(item, true);
    }

    void Remove(RE::TESBoundObject* item) {
        if (NPCAction(NPCTargets::Action::Remove, item)) return;
        if (RE::UI::GetSingleton()->IsItemMenuOpen()) return;  // Can cause crashes

        auto player = NPCTargets::GetActor();
        if (!player) return;

        auto it = std::find_if(items.begin(), items.end(), [=](auto& i) { return i.second == item; });
        if (it != items.end()) {
            player->RemoveItem(item, 1, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
            TrackRemovedItem(item);  // Update tracking
            items.erase(it);
            for (int f = 0; f < static_cast<int>(g_Config.outfitFrameMultiplier); f++) {
                g_Pause.SkipFrame();
            }
        }
    }

    void Remove() {
        if (NPCAction(NPCTargets::Action::RemoveAll)) return;
        if (RE::UI::GetSingleton()->IsItemMenuOpen()) return;  // Can cause crashes

        auto player = NPCTargets::GetActor();
        if (!player) return;

        for (auto i : items) {
            player->RemoveItem(i.second, 1, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
            TrackRemovedItem(i.second);  // Update tracking
        }

        items.clear();
        for (int f = 0; f < static_cast<int>(g_Config.outfitFrameMultiplier); f++) {
            g_Pause.SkipFrame();
        }
    }

    void Pop(bool unequip = false) {
        if (NPCAction(NPCTargets::Action::Undo)) return;
        if (RE::UI::GetSingleton()->IsItemMenuOpen()) return;  // Can cause crashes

        if (items.empty()) return;
        auto player = NPCTargets::GetActor();
        if (!player) return;

        auto t = items.back().first;

        while (!items.empty() && items.back().first == t) {
            auto item = items.back().second;
            if (unequip) {
                if (auto armor = item->As<RE::TESObjectARMO>()) recentEquipSlots &= ~(ArmorSlots)armor->GetSlotMask().underlying();
                RE::ActorEquipManager::GetSingleton()->UnequipObject(player, item);
            }

            TrackRemovedItem(item);  // Update tracking

            // AddTask isn't strictly needed, but other mods were crashing if an unequip was called first and the item was deleted
            SKSE::GetTaskInterface()->AddTask([handle = player->GetHandle(), generation = NPCTargets::Generation(), item = items.back().second]() {
                if (generation != NPCTargets::Generation()) return;
                if (auto actor = handle.get()) actor->RemoveItem(item, 1, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);
            });
            items.pop_back();
            for (int f = 0; f < static_cast<int>(g_Config.outfitFrameMultiplier); f++) {
                g_Pause.SkipFrame();
            }
        }
    }

    // Remove all tracked items from the player's inventory (except those marked to keep)
    // This removes items that were given in previous sessions too
    void RemoveAllTracked() {
        if (NPCAction(NPCTargets::Action::RemoveAll)) return;
        if (RE::UI::GetSingleton()->IsItemMenuOpen()) return;  // Can cause crashes

        auto player = NPCTargets::GetActor();
        if (!player) return;

        // Build list of items to remove (excludes marked to keep)
        std::vector<std::pair<RE::TESBoundObject*, int32_t>> toRemove;

        for (const auto& [formID, entry] : g_ItemTracking.givenItems) {
            // Skip items marked to keep
            if (g_ItemTracking.markedToKeep.contains(formID)) {
                logger::debug("RemoveAllTracked: Skipping {} (marked to keep)", formID);
                continue;
            }

            // Try to resolve the form
            RE::TESBoundObject* item = nullptr;

            if (entry.isEnhanced) {
                // For enhanced items, check the cache
                auto cacheIt = g_EnhancedArmorCache.find(entry.enhancementKey);
                if (cacheIt != g_EnhancedArmorCache.end()) {
                    item = cacheIt->second;
                }
            } else {
                // Regular item - use LookupForm
                item = LookupForm<RE::TESBoundObject>(formID);
            }

            if (item && entry.count > 0) {
                toRemove.push_back({item, entry.count});
            }
        }

        // Remove the items
        for (const auto& [item, count] : toRemove) {
            std::string formID = QARFormID(item);
            logger::debug("RemoveAllTracked: Removing {} x{}", item->GetName(), count);

            // Unequip if worn
            if (auto armor = item->As<RE::TESObjectARMO>()) {
                if (IsArmorWorn(armor, player)) {
                    RE::ActorEquipManager::GetSingleton()->UnequipObject(player, armor);
                }
            }

            // Remove from inventory
            player->RemoveItem(item, count, RE::ITEM_REMOVE_REASON::kRemove, nullptr, nullptr);

            // Clear from tracking
            g_ItemTracking.givenItems.erase(formID);

            // Also remove from session items list
            items.erase(std::remove_if(items.begin(), items.end(),
                [item](const auto& p) { return p.second == item; }), items.end());
        }

        SaveItemTracking();

        for (int f = 0; f < static_cast<int>(g_Config.outfitFrameMultiplier); f++) {
            g_Pause.SkipFrame();
        }
    }

    unsigned int recentEquipSlots = 0;
    std::vector<std::pair<int, RE::TESBoundObject*>> items;
    std::set<RE::TESBoundObject*> stored;
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

void SliderRow(const char* field, ArmorChangeParams::SliderPair& pair, int flatLimit = 0, int flatPrecision = 0, float min = 0.0f, float max = 300.0f, float def = 100.0f) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::PushID(field);
    ImGui::Checkbox(LZFormat("Modify {}", field).c_str(), &pair.bModify);
    ImGui::BeginDisabled(!pair.bModify);
    ImGui::TableNextColumn();
    ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
    if (!flatLimit || !pair.bFlat)
        ImGui::SliderFloat("##Scale", &pair.fScale, min, max, "%.0f%%", ImGuiSliderFlags_AlwaysClamp);
    else {
        const char* strFormat[] = {"%+.0f", "%+.1f", "%+.2f"};
        ImGui::SliderFloat("##Scale", &pair.fScale, (float)-flatLimit, 2.0f * flatLimit, strFormat[flatPrecision], ImGuiSliderFlags_AlwaysClamp);
    }

    if (flatLimit && ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiPopupFlags_MouseButtonRight)) {
        pair.bFlat = !pair.bFlat;
        pair.Reset(def);
    }
    ImGui::TableNextColumn();
    if (ImGui::Button(LZ("Reset"))) pair.Reset(def);
    ImGui::EndDisabled();
    ImGui::PopID();
}

void PermissionsChecklist(const char* id, Permissions& p) {
    ImGui::PushID(id);
    ImGui::Checkbox(LZ("Distribute loot"), &p.bDistributeLoot);
    ImGui::Checkbox(LZ("Modify keywords"), &p.bModifyKeywords);
    ImGui::Checkbox(LZ("Modify custom keywords"), &p.bModifyCustomKeywords);
    ImGui::Checkbox(LZ("Modify armor slots"), &p.bModifySlots);
    ImGui::Checkbox(LZ("Modify armor rating"), &p.bModifyArmorRating);
    ImGui::Checkbox(LZ("Modify armor weight"), &p.bModifyWeight);
    ImGui::Checkbox(LZ("Modify armor warmth"), &p.bModifyWarmth);
    ImGui::Checkbox(LZ("Modify weapon damage"), &p.bModifyWeapDamage);
    ImGui::Checkbox(LZ("Modify weapon weight"), &p.bModifyWeapWeight);
    ImGui::Checkbox(LZ("Modify weapon speed"), &p.bModifyWeapSpeed);
    ImGui::Checkbox(LZ("Modify weapon stagger"), &p.bModifyWeapStagger);
    ImGui::Checkbox(LZ("Modify value"), &p.bModifyValue);
    ImGui::Checkbox(LZ("Modify crafting recipes"), &p.crafting.bModify);
    ImGui::Checkbox(LZ("Create crafting recipes"), &p.crafting.bCreate);
    ImGui::Checkbox(LZ("Free crafting recipes"), &p.crafting.bFree);
    ImGui::Checkbox(LZ("Remove crafting recipes"), &p.crafting.bRemove);
    ImGui::Checkbox(LZ("Modify temper recipes"), &p.temper.bModify);
    ImGui::Checkbox(LZ("Create temper recipes"), &p.temper.bCreate);
    ImGui::Checkbox(LZ("Free temper recipes"), &p.temper.bFree);
    ImGui::Checkbox(LZ("Remove temper recipes"), &p.temper.bRemove);
    ImGui::Checkbox(LZ("Remove armor enchantments"), &p.bStripEnchArmor);
    ImGui::Checkbox(LZ("Remove weapon enchantments"), &p.bStripEnchWeapons);
    ImGui::Checkbox(LZ("Remove staff enchantments"), &p.bStripEnchStaves);
    ImGui::PopID();
}

short g_filterRound = 0;

// Two column "shortcut / what it does" table used by the settings Help tab
bool HelpTable(const char* id) {
    if (ImGui::BeginTable(id, 2, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingFixedFit, {-FLT_MIN, 0.0f})) {
        ImGui::TableSetupColumn("HelpKeyCol", ImGuiTableColumnFlags_WidthFixed, 14.0f * ImGui::GetFontSize());
        ImGui::TableSetupColumn("HelpDescCol", ImGuiTableColumnFlags_WidthStretch);
        return true;
    }
    return false;
}

void HelpRow(const char* keys, const char* desc) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::TextColored(ImVec4(0.6f, 0.9f, 0.6f, 1.0f), "%s", LZ(keys));
    ImGui::TableNextColumn();
    ImGui::TextWrapped("%s", LZ(desc));
}

// Appends the next unused 4 digit suffix for that prefix, ie "outfit0003"
std::string SuggestOutfitName(const std::string& prefix) {
    int maxNum = 0;

    for (const auto& [name, outfit] : g_Data.outfits) {
        if (name.length() != prefix.length() + 4 || name.compare(0, prefix.length(), prefix)) continue;

        auto suffix = name.substr(prefix.length());
        if (!std::all_of(suffix.begin(), suffix.end(), [](unsigned char c) { return std::isdigit(c); })) continue;

        maxNum = std::max(maxNum, std::stoi(suffix));
    }

    return std::format("{}{:04d}", prefix, maxNum + 1);
}

// Turns a file name like "Some Armor Mod.esp" into something IsValidOutfitName accepts
std::string OutfitPrefixFromMod(const RE::TESFile* mod) {
    std::string name(mod->fileName);

    if (auto dot = name.rfind('.'); dot != std::string::npos) name.erase(dot);
    std::erase_if(name, [](unsigned char c) { return !std::isalnum(c) && c != '_' && c != '-' && c != ' '; });

    // Outfit names cap at 64, leave room for the suffix
    if (name.length() > 60) name.resize(60);

    return name;
}

void InsertModificationFilter(ModFilterSettings& filters) {
    ImGui::Text(LZ("QAR changes:"));
    bool bChanged = false;
    bChanged |= TriStateCheckbox::Insert(LZ("Any"), &filters.any);
    ImGui::BeginDisabled(filters.any == TriStateCheckbox::kFalse);
    bChanged |= TriStateCheckbox::Insert(LZ("Distributed as loot"), &filters.loot);
    bChanged |= TriStateCheckbox::Insert(LZ("Region"), &filters.region);
    bChanged |= TriStateCheckbox::Insert(LZ("Alterated stats"), &filters.stats);
    bChanged |= TriStateCheckbox::Insert(LZ("Altered survival stats"), &filters.survival);
    bChanged |= TriStateCheckbox::Insert(LZ("Altered recipes"), &filters.recipes);
    bChanged |= TriStateCheckbox::Insert(LZ("Slot remapping"), &filters.slots);
    bChanged |= TriStateCheckbox::Insert(LZ("Changed keywords"), &filters.keywords);
    ImGui::EndDisabled();

    if (bChanged) {
        filters.Build();
        g_filterRound++;
    }
}

enum { ModSpecial_Worn, ModSpecial_All };

void AddFormsToList(const auto& all, const ItemFilter& filter) {
    auto& data = *g_Config.acParams.data;
    for (auto i : all) {
        if (!IsValidItem(i)) continue;
        if (!filter.Pass(i)) continue;

        if (data.filteredItems.size() < g_Config.itemListLimit) data.filteredItems.push_back(i);
    }
}

// Forward declaration - defined before RenderUI, used here and reset when GUI closes
extern int g_inventoryStartupDelay;

// Helper to cache equipped slot information (compute once per frame)
struct EquippedSlotsCache {
    uint32_t occupiedSlots = 0;  // Bitmask of slots that have items equipped
    std::unordered_map<uint32_t, RE::TESObjectARMO*> slotToArmor;  // Slot bit -> equipped armor

    // Incremental refresh: check only a few slots per frame to spread the cost
    // GetWornArmor internally iterates inventory which is O(inventory_size)
    int nextSlotToCheck = 0;
    static constexpr int SLOTS_PER_FRAME = 4;  // Check 4 slots per frame (full refresh every 8 frames)
    bool needsFullRefresh = true;  // Start with a full refresh
    RE::FormID lastActorID = 0;
    std::uint64_t lastGeneration = 0;

    void Refresh(RE::Actor* player, bool forceFullRefresh = false) {
        const auto actorID = player ? player->GetFormID() : 0;
        if (actorID != lastActorID || NPCTargets::Generation() != lastGeneration) {
            needsFullRefresh = true;
            lastActorID = actorID;
            lastGeneration = NPCTargets::Generation();
        }
        if (!player) {
            occupiedSlots = 0;
            slotToArmor.clear();
            needsFullRefresh = true;
            return;
        }

        // Full refresh on first call or when forced
        if (needsFullRefresh || forceFullRefresh) {
            needsFullRefresh = false;
            occupiedSlots = 0;
            slotToArmor.clear();

            for (int slot = 0; slot < 32; slot++) {
                auto bipedSlot = static_cast<RE::BGSBipedObjectForm::BipedObjectSlot>(1 << slot);
                if (auto armor = player->GetWornArmor(bipedSlot)) {
                    occupiedSlots |= (1 << slot);
                    slotToArmor[1 << slot] = armor;
                }
            }
            nextSlotToCheck = 0;
            return;
        }

        // Incremental refresh: check a few slots per frame
        for (int i = 0; i < SLOTS_PER_FRAME && nextSlotToCheck < 32; i++, nextSlotToCheck++) {
            int slot = nextSlotToCheck;
            uint32_t slotBit = 1 << slot;
            auto bipedSlot = static_cast<RE::BGSBipedObjectForm::BipedObjectSlot>(slotBit);
            auto armor = player->GetWornArmor(bipedSlot);

            // Update the cache for this slot
            if (armor) {
                occupiedSlots |= slotBit;
                slotToArmor[slotBit] = armor;
            } else {
                occupiedSlots &= ~slotBit;
                slotToArmor.erase(slotBit);
            }
        }

        // Wrap around when we've checked all slots
        if (nextSlotToCheck >= 32) {
            nextSlotToCheck = 0;
        }
    }

    void ForceFullRefresh() {
        needsFullRefresh = true;
    }

    bool IsSlotOccupied(int slotBit, RE::TESObjectARMO* excludeItem = nullptr) const {
        if (!(occupiedSlots & slotBit)) return false;
        if (!excludeItem) return true;
        auto it = slotToArmor.find(slotBit);
        return it != slotToArmor.end() && it->second != excludeItem;
    }
};

// Helper struct for item slot display in tables
struct ItemSlotInfo {
    std::string slotName;   // Primary slot name or "Primary +N" for multiple slots
    std::string slotIDs;    // All slot IDs comma-separated
    int primarySlot = -1;   // Primary slot number (for sorting)
    bool hasConflict = false;  // True if any slot conflicts with currently equipped items
    float armorRating = 0.0f;  // Armor rating (0 for non-armor items)

    // Fast version using pre-computed equipped slots cache
    static ItemSlotInfo GetForItem(RE::TESBoundObject* item, const ArmorChangeParams& params, const EquippedSlotsCache* equippedCache = nullptr) {
        ItemSlotInfo info;

        auto armor = item->As<RE::TESObjectARMO>();
        if (!armor) {
            return info;  // Empty for weapons/ammo
        }

        // Get armor rating
        info.armorRating = armor->GetArmorRating();

        if (!params.curve) {
            return info;
        }

        auto slots = (ArmorSlots)armor->GetSlotMask().underlying();
        if (!slots) {
            return info;
        }

        int slotCount = 0;
        char slotIDsBuf[128] = {0};
        char* bufPtr = slotIDsBuf;

        for (int slot = 0; slot < 32; slot++) {
            if (slots & (1 << slot)) {
                // Build slot IDs string efficiently
                if (slotCount > 0) {
                    *bufPtr++ = ',';
                    *bufPtr++ = ' ';
                }
                bufPtr += sprintf(bufPtr, "%d", slot + 30);
                slotCount++;

                if (info.primarySlot < 0) {
                    info.slotName = params.curve->slotName[slot];
                    info.primarySlot = slot + 30;
                }

                // Check for slot conflict using cached data (fast)
                if (equippedCache && !info.hasConflict) {
                    if (equippedCache->IsSlotOccupied(1 << slot, armor)) {
                        info.hasConflict = true;
                    }
                }
            }
        }

        info.slotIDs = slotIDsBuf;

        // Build slot name: "Primary" or "Primary +N"
        if (slotCount > 1 && !info.slotName.empty()) {
            info.slotName = std::format("{} +{}", info.slotName, slotCount - 1);
        }

        return info;
    }
};

// Comprehensive cached data for a single item (avoids per-frame recomputation)
struct CachedItemData {
    RE::TESBoundObject* item = nullptr;
    std::string displayName;           // Pre-computed display name
    std::string lowerName;              // Lowercase name for case-insensitive sorting
    std::string femaleNif;              // Female NIF model path (for armor)
    std::string lowerFemaleNif;         // Lowercase for sorting
    std::string modName;                // Source mod filename
    std::string lowerModName;           // Lowercase for sorting
    ItemSlotInfo slotInfo;              // Cached slot information
    bool isFavorite = false;            // Cached favorite status
    const std::set<std::string>* tags = nullptr;  // Pointer to item's tags (no copy, fast lookup)

    void Compute(RE::TESBoundObject* obj, const ArmorChangeParams& params, const EquippedSlotsCache* equippedCache) {
        item = obj;

        // Compute display name once
        const char* name = obj->GetName();
        if (name && name[0]) {
            displayName = name;
        } else {
            char buf[128];
            if (auto file = obj->GetFile(0)) {
                sprintf(buf, "%s:%010x", file->fileName, obj->formID);
            } else {
                sprintf(buf, "<dynamic>:%010x", obj->formID);
            }
            displayName = buf;
        }

        // Compute lowercase name for sorting (avoid repeated tolower calls)
        lowerName = displayName;
        for (char& c : lowerName) {
            if (c >= 'A' && c <= 'Z') c += 32;
        }

        // Compute female NIF path for armor items
        femaleNif.clear();
        lowerFemaleNif.clear();
        if (auto armor = obj->As<RE::TESObjectARMO>()) {
            for (auto addon : armor->armorAddons) {
                if (addon && !addon->bipedModels[RE::SEXES::kFemale].model.empty()) {
                    femaleNif = addon->bipedModels[RE::SEXES::kFemale].model.c_str();
                    break;  // Use first non-empty female model
                }
            }
            // Compute lowercase for sorting
            if (!femaleNif.empty()) {
                lowerFemaleNif = femaleNif;
                for (char& c : lowerFemaleNif) {
                    if (c >= 'A' && c <= 'Z') c += 32;
                }
            }
        }

        // Compute source mod name
        if (auto file = obj->GetFile(0)) {
            modName = file->fileName;
            lowerModName = modName;
            for (char& c : lowerModName) {
                if (c >= 'A' && c <= 'Z') c += 32;
            }
        } else {
            modName.clear();
            lowerModName.clear();
        }

        // Compute slot info
        slotInfo = ItemSlotInfo::GetForItem(obj, params, equippedCache);

        // Cache favorite status
        isFavorite = g_Data.favoriteItems.contains(obj);

        // Cache tags pointer (fast O(1) lookup)
        tags = GetItemTags(obj);
    }
};

// Cache for the entire items table (rebuilt only when items change)
struct ItemsTableCache {
    std::vector<CachedItemData> cachedItems;
    std::vector<size_t> sortedIndices;          // Indices into cachedItems, sorted
    size_t lastFilteredCount = 0;
    int lastSortColumn = -1;
    bool lastSortAscending = true;
    short lastFilterRound = -1;
    uint32_t lastEquippedSlots = 0;             // To detect equipment changes

    bool NeedsRebuild(const std::vector<RE::TESBoundObject*>& filteredItems, short filterRound, uint32_t /*equippedSlots*/) {
        // Note: We no longer rebuild the entire cache just because equipped slots changed.
        // This was causing severe freezes (rebuilding 20,000+ items) when equipping outfits.
        // Conflict highlighting might be slightly stale, but performance is more important.
        return filterRound != lastFilterRound ||
               filteredItems.size() != lastFilteredCount;
    }

    void Rebuild(const std::vector<RE::TESBoundObject*>& filteredItems, const ArmorChangeParams& params,
                 const EquippedSlotsCache* equippedCache, short filterRound) {
        lastFilterRound = filterRound;
        lastFilteredCount = filteredItems.size();
        lastEquippedSlots = equippedCache ? equippedCache->occupiedSlots : 0;
        lastSortColumn = -1;  // Force re-sort

        cachedItems.resize(filteredItems.size());
        sortedIndices.resize(filteredItems.size());

        for (size_t i = 0; i < filteredItems.size(); i++) {
            cachedItems[i].Compute(filteredItems[i], params, equippedCache);
            sortedIndices[i] = i;
        }
    }

    void Sort(int sortColumn, bool ascending) {
        if (sortColumn == lastSortColumn && ascending == lastSortAscending) {
            return;  // Already sorted
        }

        lastSortColumn = sortColumn;
        lastSortAscending = ascending;

        std::sort(sortedIndices.begin(), sortedIndices.end(),
            [this, sortColumn, ascending](size_t a, size_t b) {
                const auto& itemA = cachedItems[a];
                const auto& itemB = cachedItems[b];

                switch (sortColumn) {
                    case 0: {  // Sort by name (using pre-computed lowercase)
                        int cmp = itemA.lowerName.compare(itemB.lowerName);
                        return ascending ? cmp < 0 : cmp > 0;
                    }
                    case 1: {  // Sort by slot
                        if (itemA.slotInfo.primarySlot < 0 && itemB.slotInfo.primarySlot < 0) {
                            // Both have no slot - sub-sort by name (ascending)
                            return itemA.lowerName.compare(itemB.lowerName) < 0;
                        }
                        if (itemA.slotInfo.primarySlot < 0) return !ascending;
                        if (itemB.slotInfo.primarySlot < 0) return ascending;
                        if (itemA.slotInfo.primarySlot != itemB.slotInfo.primarySlot) {
                            return ascending ? itemA.slotInfo.primarySlot < itemB.slotInfo.primarySlot
                                            : itemA.slotInfo.primarySlot > itemB.slotInfo.primarySlot;
                        }
                        // Same slot - sub-sort by name (ascending)
                        return itemA.lowerName.compare(itemB.lowerName) < 0;
                    }
                    case 2: {  // Sort by armor
                        if (itemA.slotInfo.armorRating == 0 && itemB.slotInfo.armorRating == 0) {
                            // Both have no armor rating - sub-sort by name (ascending)
                            return itemA.lowerName.compare(itemB.lowerName) < 0;
                        }
                        if (itemA.slotInfo.armorRating == 0) return !ascending;
                        if (itemB.slotInfo.armorRating == 0) return ascending;
                        if (itemA.slotInfo.armorRating != itemB.slotInfo.armorRating) {
                            return ascending ? itemA.slotInfo.armorRating < itemB.slotInfo.armorRating
                                            : itemA.slotInfo.armorRating > itemB.slotInfo.armorRating;
                        }
                        // Same armor rating - sub-sort by name (ascending)
                        return itemA.lowerName.compare(itemB.lowerName) < 0;
                    }
                    case 3: {  // Sort by ID
                        if (itemA.slotInfo.slotIDs.empty() && itemB.slotInfo.slotIDs.empty()) {
                            // Both have no ID - sub-sort by name (ascending)
                            return itemA.lowerName.compare(itemB.lowerName) < 0;
                        }
                        if (itemA.slotInfo.slotIDs.empty()) return !ascending;
                        if (itemB.slotInfo.slotIDs.empty()) return ascending;
                        int cmp = itemA.slotInfo.slotIDs.compare(itemB.slotInfo.slotIDs);
                        if (cmp != 0) {
                            return ascending ? cmp < 0 : cmp > 0;
                        }
                        // Same ID - sub-sort by name (ascending)
                        return itemA.lowerName.compare(itemB.lowerName) < 0;
                    }
                    case 4: {  // Sort by NIF (female model path)
                        if (itemA.lowerFemaleNif.empty() && itemB.lowerFemaleNif.empty()) {
                            // Both have no NIF - sub-sort by name (ascending)
                            return itemA.lowerName.compare(itemB.lowerName) < 0;
                        }
                        if (itemA.lowerFemaleNif.empty()) return !ascending;
                        if (itemB.lowerFemaleNif.empty()) return ascending;
                        int cmp = itemA.lowerFemaleNif.compare(itemB.lowerFemaleNif);
                        if (cmp != 0) {
                            return ascending ? cmp < 0 : cmp > 0;
                        }
                        // Same NIF - sub-sort by name (ascending)
                        return itemA.lowerName.compare(itemB.lowerName) < 0;
                    }
                    case 5: {  // Sort by Mod (source plugin filename)
                        if (itemA.lowerModName.empty() && itemB.lowerModName.empty()) {
                            // Both have no mod - sub-sort by name (ascending)
                            return itemA.lowerName.compare(itemB.lowerName) < 0;
                        }
                        if (itemA.lowerModName.empty()) return !ascending;
                        if (itemB.lowerModName.empty()) return ascending;
                        int cmp = itemA.lowerModName.compare(itemB.lowerModName);
                        if (cmp != 0) {
                            return ascending ? cmp < 0 : cmp > 0;
                        }
                        // Same mod - sub-sort by name (ascending)
                        return itemA.lowerName.compare(itemB.lowerName) < 0;
                    }
                    default:
                        return false;
                }
            });
    }

    const CachedItemData& GetSorted(size_t index) const {
        return cachedItems[sortedIndices[index]];
    }

    RE::TESBoundObject* GetSortedItem(size_t index) const {
        return cachedItems[sortedIndices[index]].item;
    }

    size_t Size() const { return sortedIndices.size(); }

    // Find the sorted index of an item, returns SIZE_MAX if not found
    size_t FindSortedIndex(RE::TESBoundObject* item) const {
        for (size_t i = 0; i < sortedIndices.size(); i++) {
            if (cachedItems[sortedIndices[i]].item == item) {
                return i;
            }
        }
        return SIZE_MAX;
    }

    // Get a vector of all sorted items (for BuildSetFrom and similar APIs)
    std::vector<RE::TESBoundObject*> GetAllSortedItems() const {
        std::vector<RE::TESBoundObject*> result;
        result.reserve(sortedIndices.size());
        for (size_t idx : sortedIndices) {
            result.push_back(cachedItems[idx].item);
        }
        return result;
    }

    // Iterate over a range of sorted items [startIdx, endIdx] inclusive
    template<typename Func>
    void ForRange(size_t startIdx, size_t endIdx, Func&& func) const {
        if (startIdx > endIdx) std::swap(startIdx, endIdx);
        for (size_t i = startIdx; i <= endIdx && i < sortedIndices.size(); i++) {
            func(cachedItems[sortedIndices[i]].item);
        }
    }
};

// Cache for checked items computation (avoids O(N) loop every frame)
struct CheckedItemsCache {
    std::vector<RE::TESBoundObject*> checkedItems;
    unsigned int anyItemChanges = 0;
    bool hasModifiedItems = false;

    // Track when we need to rebuild
    short lastFilterRound = -1;
    size_t lastUncheckedCount = 0;
    size_t lastCacheSize = 0;

    bool NeedsRebuild(short filterRound, size_t uncheckedCount, size_t cacheSize) const {
        return filterRound != lastFilterRound ||
               uncheckedCount != lastUncheckedCount ||
               cacheSize != lastCacheSize;
    }

    void Rebuild(const ItemsTableCache& cache, const std::set<RE::TESObject*>& uncheckedItems,
                 unsigned int needChanges, short filterRound) {
        lastFilterRound = filterRound;
        lastUncheckedCount = uncheckedItems.size();
        lastCacheSize = cache.Size();

        checkedItems.clear();
        checkedItems.reserve(lastCacheSize);
        anyItemChanges = 0;
        hasModifiedItems = false;

        for (size_t idx = 0; idx < lastCacheSize; idx++) {
            auto item = cache.GetSortedItem(idx);
            unsigned int itemChanges = MapFindOr(g_Data.modifiedItems, item, 0u);
            const bool canUse = (itemChanges & needChanges) == needChanges;
            // Cast to TESObject* for the uncheckedItems check (TESBoundObject derives from TESObject)
            bool isChecked = !uncheckedItems.contains(static_cast<RE::TESObject*>(item)) && canUse;
            if (isChecked) {
                anyItemChanges |= itemChanges;
                checkedItems.push_back(item);
                auto itemFile = item->GetFile(0);
                if (g_Data.modifiedItems.contains(item) &&
                    !(itemFile && g_Data.modifiedFilesDeleted.contains(itemFile)) &&
                    !g_Data.modifiedItemsDeleted.contains(item)) {
                    hasModifiedItems = true;
                }
            }
        }
    }

    void InvalidateOnCheck() {
        // Called when user checks/unchecks an item to force rebuild
        lastUncheckedCount = SIZE_MAX;
    }
};

static CheckedItemsCache g_CheckedItemsCache;

// Global cache instance for main items table
static ItemsTableCache g_MainItemsCache;

bool GetCurrentListItems(std::set<ModData*>& curMod, int nModSpecial, const ItemFilter& filter, AnalyzeResults& results) {
    logger::trace("[GUI] GetCurrentListItems called - curMod.size={}, nModSpecial={}", curMod.size(), nModSpecial);

    // Delay GetInventory() calls for the first few frames after GUI opens
    // This allows other plugins' hooks to initialize properly
    if (g_inventoryStartupDelay > 0) {
        g_inventoryStartupDelay--;
        logger::trace("[GUI] GetCurrentListItems - startup delay, {} frames remaining", g_inventoryStartupDelay);
        return false;
    }

    static short filterRound = -1;
    if (filterRound == g_filterRound) {
        logger::trace("[GUI] GetCurrentListItems - filter unchanged, returning early");
        return false;
    }
    filterRound = g_filterRound;

    logger::trace("[GUI] GetCurrentListItems - filter changed, rebuilding list");

    auto& data = *g_Config.acParams.data;
    data.filteredItems.clear();
    if (!curMod.empty()) {
        logger::trace("[GUI] GetCurrentListItems - processing {} mod(s)", curMod.size());
        for (auto mod : curMod) {
            logger::trace("[GUI] GetCurrentListItems - processing mod with {} items", mod->items.size());
            for (auto i : mod->items) {
                if (!filter.Pass(i)) continue;

                data.filteredItems.push_back(i);
            }
        }
        logger::trace("[GUI] GetCurrentListItems - mod processing complete, {} filtered items", data.filteredItems.size());
    } else {
        switch (nModSpecial) {
            case ModSpecial_Worn:
                logger::trace("[GUI] GetCurrentListItems - ModSpecial_Worn: getting player");
                if (auto player = NPCTargets::GetActor()) {
                    logger::trace("[GUI] GetCurrentListItems - ModSpecial_Worn: using GetWornArmor for each slot");
                    // Use GetWornArmor() for each biped slot instead of GetInventory()
                    // This avoids the TESObjEx plugin hook crash
                    std::set<RE::TESObjectARMO*> seenArmor;
                    for (int slot = 0; slot < 32; slot++) {
                        auto bipedSlot = static_cast<RE::BGSBipedObjectForm::BipedObjectSlot>(1 << slot);
                        if (auto armor = player->GetWornArmor(bipedSlot)) {
                            if (seenArmor.insert(armor).second) {  // Only add if not seen before
                                logger::trace("[GUI] GetCurrentListItems - ModSpecial_Worn: found armor in slot {}: {}", slot, armor->GetName());
                                if (!IsValidItem(armor)) continue;
                                if (!filter.Pass(armor)) continue;
                                data.filteredItems.push_back(armor);
                            }
                        }
                    }
                    // Also check equipped weapon and ammo via GetEquippedObject
                    logger::trace("[GUI] GetCurrentListItems - ModSpecial_Worn: checking equipped weapons/ammo");
                    for (bool leftHand : {false, true}) {
                        if (auto obj = player->GetEquippedObject(leftHand)) {
                            if (auto weap = obj->As<RE::TESObjectWEAP>()) {
                                if (IsValidItem(weap) && filter.Pass(weap)) {
                                    // Check if not already in list
                                    if (std::find(data.filteredItems.begin(), data.filteredItems.end(), weap) == data.filteredItems.end()) {
                                        data.filteredItems.push_back(weap);
                                    }
                                }
                            }
                        }
                    }
                    // Check equipped ammo
                    if (auto ammo = GetEquippedAmmo(player)) {
                        if (IsValidItem(ammo) && filter.Pass(ammo)) {
                            if (std::find(data.filteredItems.begin(), data.filteredItems.end(), ammo) == data.filteredItems.end()) {
                                data.filteredItems.push_back(ammo);
                            }
                        }
                    }
                    logger::trace("[GUI] GetCurrentListItems - ModSpecial_Worn: done, {} worn items found", data.filteredItems.size());
                } else {
                    logger::warn("[GUI] GetCurrentListItems - ModSpecial_Worn: player is null!");
                }
                break;
            case ModSpecial_All:
                logger::trace("[GUI] GetCurrentListItems - ModSpecial_All: getting data handler");
                auto dh = RE::TESDataHandler::GetSingleton();
                logger::trace("[GUI] GetCurrentListItems - ModSpecial_All: adding armor forms");
                AddFormsToList(dh->GetFormArray<RE::TESObjectARMO>(), filter);
                logger::trace("[GUI] GetCurrentListItems - ModSpecial_All: adding weapon forms");
                AddFormsToList(dh->GetFormArray<RE::TESObjectWEAP>(), filter);
                logger::trace("[GUI] GetCurrentListItems - ModSpecial_All: adding ammo forms");
                AddFormsToList(dh->GetFormArray<RE::TESAmmo>(), filter);
                logger::trace("[GUI] GetCurrentListItems - ModSpecial_All: done, {} items", data.filteredItems.size());
                break;
        }
    }

    logger::trace("[GUI] GetCurrentListItems - clearing results");
    results.Clear();
    data.dvSets.clear();

    if (!data.filteredItems.empty()) {
        logger::trace("[GUI] GetCurrentListItems - sorting {} items", data.filteredItems.size());
        std::sort(data.filteredItems.begin(), data.filteredItems.end(),
                  [](RE::TESBoundObject* const a, RE::TESBoundObject* const b) { return _stricmp(a->GetName(), b->GetName()) < 0; });

        // Apply variant grouping if enabled - keeps only one item per base name
        // e.g., "Iron Sword of Fire", "Iron Sword of Frost" -> shows only "Iron Sword of Fire"
        if (filter.bGroupVariants) {
            logger::trace("[GUI] GetCurrentListItems - grouping variants, {} items before", data.filteredItems.size());
            std::set<std::string> seenBaseNames;
            std::vector<RE::TESBoundObject*> uniqueItems;
            uniqueItems.reserve(data.filteredItems.size());

            for (auto item : data.filteredItems) {
                std::string baseName = ItemFilter::GetBaseName(item->GetName());
                // Convert to lowercase for case-insensitive comparison
                for (char& c : baseName) {
                    if (c >= 'A' && c <= 'Z') c += 32;
                }

                if (seenBaseNames.insert(baseName).second) {
                    // First time seeing this base name, keep the item
                    uniqueItems.push_back(item);
                }
            }

            data.filteredItems = std::move(uniqueItems);
            logger::trace("[GUI] GetCurrentListItems - grouping variants complete, {} items after", data.filteredItems.size());
        }

        logger::trace("[GUI] GetCurrentListItems - analyzing armor");
        //if (!curMod.empty())
        AnalyzeArmor(data.filteredItems, results);
        logger::trace("[GUI] GetCurrentListItems - analysis complete");
    }

    logger::trace("[GUI] GetCurrentListItems - returning true");
    return true;
}

bool WillBeModified(const ArmorChangeParams& params, RE::TESBoundObject* i, ArmorSlots remapped) {
    if (params.armorSet) {
        if (auto armor = i->As<RE::TESObjectARMO>()) {
            if (((remapped | g_Config.slotsWillChange | params.slotsCosmetic) & (ArmorSlots)armor->GetSlotMask().underlying()) == 0) return false;
        } else if (auto weap = i->As<RE::TESObjectWEAP>()) {
            if (!params.armorSet->FindMatching(weap)) return false;
        } else if (auto ammo = i->As<RE::TESAmmo>()) {
            if (!params.armorSet->FindMatching(ammo)) return false;
        } else
            return false;
        return true;
    } else {
        return g_Data.modifiedItems.contains(i);
    }
}

struct HighlightTrack {
    short round = -1;
    char pop = 0;
    bool enabled = false;

    void Touch() { round = g_filterRound; }
    operator bool() { return !enabled || g_filterRound == round; }

    void Push(bool show = true) {
        enabled = show;
        if (g_Config.bHighlights && round != g_filterRound && show) {
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
        if (ImGui::IsItemActive()) round = g_filterRound;

        ImGui::PopStyleVar(pop);
        ImGui::PopStyleColor(pop);
    }
};

struct ErrorTrack {
    static bool hasErrors;

    char pop = 0;
    bool enabled = false;

    operator bool() { return enabled; }

    void Push(bool show = true) {
        enabled = show;
        if (show) {
            hasErrors = true;

            auto phase = 0.5 + 0.5 * sin(ImGui::GetTime() * (std::_Pi_val / 1.0));
            auto brightness = std::lerp(64, 255, phase);
            const auto colorHighlight = IM_COL32(brightness, 0, 0, 255);

            ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
            ImGui::PushStyleColor(ImGuiCol_Border, colorHighlight);
            pop = 1;
        } else
            pop = 0;
    }

    void Pop() {
        ImGui::PopStyleVar(pop);
        ImGui::PopStyleColor(pop);
    }
};

bool ErrorTrack::hasErrors = false;

struct TimedTooltip {
    std::string text;
    double until = 0;

    void Enable(std::string s) {
        text = s;
        until = ImGui::GetTime() + 10.0;
    }

    bool Show() {
        if (ImGui::GetTime() >= until) return false;

        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
            ImGui::SetTooltip(text.c_str());
        }
        return true;
    }
};

void BuildFonts() {
    auto& io = ImGui::GetIO();
    ImFontGlyphRangesBuilder builder;
    builder.AddRanges(io.Fonts->GetGlyphRangesDefault());

    auto AddNameGlyphs = [&](auto& ls) {
        for (auto i : ls) {
            builder.AddText(i->fullName.c_str());

            // Need to get lowercase versions of glyphs too
            std::string name(i->fullName.c_str());
            builder.AddText(toLowerUTF8(name).c_str());
        }
    };

    // Item Glyphs
    auto dh = RE::TESDataHandler::GetSingleton();
    AddNameGlyphs(dh->GetFormArray<RE::TESObjectARMO>());
    AddNameGlyphs(dh->GetFormArray<RE::TESObjectWEAP>());
    AddNameGlyphs(dh->GetFormArray<RE::TESAmmo>());

    // Translated text
    if (auto trans = Localization::Get()->mapTranslated) {
        for (auto& i : *trans) builder.AddText(i.second.c_str());
    }

    for (auto& i : g_Config.mapCustomKWTabs) builder.AddText(i.first.c_str());
    for (auto& i : g_Config.mapCustomKWs) {
        builder.AddText(i.second.name.c_str());
        builder.AddText(i.second.tooltip.c_str());
    }
    for (auto& i : g_Config.curves) {
        for (int s = 0; s < 32; s++) builder.AddText(i.second.slotName[s].c_str());
    }
    for (auto& i : g_Config.lootProfiles) builder.AddText(i.c_str());
    for (auto& i : g_Config.armorSets) builder.AddText(i.name.c_str());

    // Language glyphs - or else the switch language menu might be unusable
    struct LocaleEnum {
        static BOOL CALLBACK Proc(LPWSTR lpLocaleStr, DWORD, LPARAM lParam) {
            // Convert the wide-character locale name to a narrow string
            std::wstring ws(lpLocaleStr);

            size_t underscore_pos = ws.find(L'-');
            if (!underscore_pos) return TRUE;

            if (underscore_pos != std::string::npos) {
                ws = ws.substr(0, underscore_pos);
            }

            wchar_t languageName[256];
            if (GetLocaleInfoEx(ws.c_str(), LOCALE_SLOCALIZEDLANGUAGENAME, languageName, 256) > 0) {
                auto builder = reinterpret_cast<ImFontGlyphRangesBuilder*>(lParam);
                builder->AddText(WStringToString(languageName).c_str());
            }

            // Return true to continue enumeration
            return TRUE;
        }
    };

    EnumSystemLocalesEx(LocaleEnum::Proc, LOCALE_ALL, reinterpret_cast<LPARAM>(&builder), nullptr);

    ImVector<ImWchar> ranges;
    builder.BuildRanges(&ranges);

    io.Fonts->Clear();
    // io.Fonts->FontBuilderIO = ImGuiFreeType::GetBuilderForFreeType();
    //  io.Fonts->AddFontDefault();

    ImFontConfig config;

    const float fontSize = (float)g_Config.nFontSize;

    auto path = std::filesystem::current_path() / PATH_ROOT;
    path /= "fonts/";

    if (std::filesystem::exists(path)) {
        if (std::filesystem::is_directory(path)) {
            for (const auto& entry : std::filesystem::directory_iterator(path)) {
                if (!entry.is_regular_file()) continue;
                if (_stricmp(entry.path().extension().generic_string().c_str(), ".ttf")) continue;

                if (!io.Fonts->Fonts.empty()) config.MergeMode = true;

                io.Fonts->AddFontFromFileTTF(entry.path().generic_string().c_str(), fontSize, &config, ranges.Data);
            }
        }
    }

    if (io.Fonts->Fonts.empty()) {
        config.SizePixels = fontSize;
        io.Fonts->AddFontDefault(&config);
    }

    // Need to force it to rebuild the font textures - suppose to be automatic, but isn't?

    if (io.Fonts->Build()) {
        ImGui_ImplDX11_InvalidateDeviceObjects();
        ImGui_ImplDX11_CreateDeviceObjects();
    }
}

struct RecipeConditionals {
    struct Conditionals {
        std::vector<RE::TESForm*> perks;
        std::vector<RE::TESForm*> items;

        void Clear() {
            perks.clear();
            items.clear();
        }

        void Sort(std::vector<RE::TESForm*>& ls) {
            std::sort(ls.begin(), ls.end(), [](RE::TESForm* const a, RE::TESForm* const b) { return _stricmp(a->GetName(), b->GetName()) < 0; });
        }

        void Sort() {
            Sort(perks);
            Sort(items);
        }

        void Add(std::vector<RE::TESForm*>& forms, std::vector<void*>& list) {
            if (list.size() < (g_Config.bShowAllRecipeConditions ? 1 : 2)) return;

            for (auto i : list) {
                auto form = static_cast<RE::TESForm*>(i);
                if (std::find(forms.begin(), forms.end(), form) == forms.end()) forms.push_back(form);
            }
        }

        void AddFrom(RE::BGSConstructibleObject* obj) {
            std::vector<void*> tempperks;
            std::vector<void*> tempitems;

            for (auto cond = obj->conditions.head; cond; cond = cond->next) {
                switch (cond->data.functionData.function.get()) {
                    case RE::FUNCTION_DATA::FunctionID::kGetItemCount:
                    case RE::FUNCTION_DATA::FunctionID::kGetEquipped:
                        if (obj->requiredItems.GetObjectCount((RE::TESBoundObject*)cond->data.functionData.params[0]) == 0)
                            tempitems.push_back(cond->data.functionData.params[0]);
                        break;
                    case RE::FUNCTION_DATA::FunctionID::kHasPerk:
                        tempperks.push_back(cond->data.functionData.params[0]);
                        break;
                }
            }

            Add(this->perks, tempperks);
            Add(this->items, tempitems);
        }
    };

    struct PurposeConditionals {
        Conditionals orig;
        Conditionals convert;

        void Clear() {
            orig.Clear();
            convert.Clear();
        }

        void Sort() {
            orig.Sort();
            convert.Sort();
        }
    };

    PurposeConditionals craft;
    PurposeConditionals temper;

    bool bListsBuilt = false;
    bool bRefreshed = false;

    void AddFromItem(RE::TESBoundObject* item, Conditionals PurposeConditionals::* which) {
        if (auto cond = MapFindOrNull(g_Data.temperRecipe, item)) {
            (temper.*which).AddFrom(cond);
        }
        if (auto cond = MapFindOrNull(g_Data.craftRecipe, item)) {
            (craft.*which).AddFrom(cond);
        }
    }

    void BuildLists(const ArmorChangeParams& params) {
        bRefreshed = true;
        if (bListsBuilt) return;
        bListsBuilt = true;

        temper.Clear();
        craft.Clear();

        if (params.armorSet) {
            for (auto i : params.armorSet->items) AddFromItem(i, &PurposeConditionals::convert);
        }

        for (auto i : params.data->items) AddFromItem(i, &PurposeConditionals::orig);

        temper.Sort();
        craft.Sort();
    }
};

// Global delay counter for GetInventory() calls - reset when GUI closes
int g_inventoryStartupDelay = 5;

void QuickArmorRebalance::RenderUI() {
    NPCTargets::FrameTarget targetFrame;
    static bool bFirstFrame = true;
    if (bFirstFrame) {
        logger::trace("[GUI] RenderUI called for first time this session");
        bFirstFrame = false;
    }
    logger::trace("[GUI] RenderUI frame start");

    const auto colorTextDefault = ImGui::GetStyleColorVec4(ImGuiCol_Text);

    const auto colorChanged = IM_COL32(100, 255, 100, 255);
    const auto colorChangedPartial = IM_COL32(0, 150, 0, 255);
    const auto colorChangedShared = IM_COL32(255, 255, 0, 255);
    const auto colorDeleted = IM_COL32(255, 0, 0, 255);

    const char* rarity[] = {LZ("Common"), LZ("Uncommon"), LZ("Rare"), nullptr};
    const char* itemTypes[] = {LZ("Any"), LZ("Armor"), LZ("Weapons"), LZ("Ammo"), nullptr};

    bool isActive = true;
    static std::set<RE::TESBoundObject*> selectedItems;
    static RE::TESBoundObject* lastSelectedItem = nullptr;
    static GivenItems givenItems;
    static std::set<ModData*> curMod;
    static std::set<RE::TESObject*> uncheckedItems;
    static int mainItemsSortColumn = 0;  // 0=Name, 1=Slot, 2=Armor, 3=ID
    static bool mainItemsSortAscending = true;
    static RE::TESBoundObject* keyboardNav = nullptr;

    auto syncTarget = [&]() {
        static std::uint64_t lastRevision = 0, lastGeneration = 0;
        static RE::FormID lastTarget = 0;
        auto actor = NPCTargets::GetActor();
        const auto targetID = actor ? actor->GetFormID() : 0;
        const bool changed = lastTarget != targetID || lastGeneration != NPCTargets::Generation();
        if (changed || lastRevision != NPCTargets::Revision()) {
            g_EquipmentChanged = true;
            ++g_filterRound;
            lastRevision = NPCTargets::Revision();
        }
        if (changed) {
            selectedItems.clear();
            lastSelectedItem = nullptr;
            keyboardNav = nullptr;
            givenItems.items.clear();
            givenItems.stored.clear();
            givenItems.recentEquipSlots = 0;
            g_LastEquippedOutfit.clear();
            lastTarget = targetID;
            lastGeneration = NPCTargets::Generation();
        }
    };
    // Refresh before keyboard shortcuts, and again after the target controls below.
    syncTarget();

    if (!RE::UI::GetSingleton()->numPausesGame) givenItems.recentEquipSlots = 0;

    const bool isShiftDown = ImGui::IsKeyDown(ImGuiKey_LeftShift) || ImGui::IsKeyDown(ImGuiKey_RightShift);
    const bool isCtrlDown = ImGui::IsKeyDown(ImGuiKey_LeftCtrl) || ImGui::IsKeyDown(ImGuiKey_RightCtrl);
    const bool isAltDown = ImGui::IsKeyDown(ImGuiKey_LeftAlt) || ImGui::IsKeyDown(ImGuiKey_RightAlt);

    static ArmorChangeParams paramsScratch(g_Config.acData);
    static bool bUseScratchParams = false;

    ArmorChangeParams& params = bUseScratchParams ? paramsScratch : g_Config.acParams;
    auto& data = *params.data;
    static auto& analyzeResults = g_Config.acData.analyzeResults;

    static ItemFilter filter;

    static HighlightTrack hlConvert;
    static HighlightTrack hlDistributeAs;
    static HighlightTrack hlRarity;
    static HighlightTrack hlRegion;
    static HighlightTrack hlDynamicVariants;
    static HighlightTrack hlSlots;

    auto isInventoryOpen = RE::UI::GetSingleton()->IsItemMenuOpen();

    static bool bRebuildFonts = true;

    static bool bMenuHovered = false;
    static bool bSlotWarning = false;
    bool popupSettings = false;
    bool popupRemapSlots = false;
    bool popupDynamicVariants = false;
    bool popupCustomKeywords = false;

    bool hasModifiedItems = false;

    const bool hadErrors = ErrorTrack::hasErrors;
    ErrorTrack::hasErrors = false;

    unsigned int needChanges = 0;
    static unsigned int anyItemChanges = 0;

    ArmorSlots remappedSrc = 0;
    ArmorSlots remappedTar = 0;

    ModData* switchToMod = nullptr;

    static int nShowWords = AnalyzeResults::eWords_EitherVariants;

    struct Local {
        static void SwitchToMod(ModData* mod, bool bModifySelection = false) {
            if (!bModifySelection) {
                curMod.clear();
                curMod.insert(mod);
            } else {
                if (curMod.contains(mod))
                    curMod.erase(mod);
                else
                    curMod.insert(mod);
            }
            givenItems.items.clear();
            selectedItems.clear();
            lastSelectedItem = nullptr;
            keyboardNav = nullptr;

            g_Config.acParams.Reset();
            paramsScratch.Reset();

            g_filterRound++;
        }

        static std::vector<RE::TESBoundObject*> SortedModItems(const ModData* mod) {
            std::vector<RE::TESBoundObject*> items(mod->items.begin(), mod->items.end());
            std::sort(items.begin(), items.end(), [](const RE::TESBoundObject* a, const RE::TESBoundObject* b) {
                return _stricmp(a->GetName(), b->GetName()) < 0;
            });
            return items;
        }

        static RE::TESBoundObject* PreferredOutfitBase(const std::vector<RE::TESBoundObject*>& items) {
            // Prefer the conventional body, feet, and hands slots, in that order. The matching-set builder
            // needs an armor item as its anchor, so fall back to any armor with an occupied biped slot.
            constexpr ArmorSlots bodySlot = 1u << (32 - 30);
            constexpr ArmorSlots feetSlot = 1u << (37 - 30);
            constexpr ArmorSlots handsSlot = 1u << (33 - 30);

            RE::TESBoundObject* best = nullptr;
            int bestPriority = 4;
            for (auto item : items) {
                auto armor = item->As<RE::TESObjectARMO>();
                if (!armor) continue;

                const auto slots = static_cast<ArmorSlots>(armor->GetSlotMask().underlying());
                if (!slots) continue;

                int priority = 3;
                if (slots & bodySlot)
                    priority = 0;
                else if (slots & feetSlot)
                    priority = 1;
                else if (slots & handsSlot)
                    priority = 2;

                if (priority < bestPriority) {
                    best = item;
                    bestPriority = priority;
                }
            }
            return best;
        }

        static void EquipMatchingSet(RE::TESBoundObject* baseItem, const std::vector<RE::TESBoundObject*>& items) {
            auto armorSet = BuildSetFrom(baseItem, items, true);
            if (armorSet.empty()) return;

            givenItems.UnequipCurrent();
            for (auto piece : armorSet) givenItems.Give(piece, true);
        }
    };

    for (auto i : params.mapArmorSlots) {
        remappedSrc |= 1 << i.first;
        remappedTar |= i.second < 32 ? (1 << i.second) : 0;
    }

    if (bRebuildFonts) {
        bRebuildFonts = false;
        ImGuiIntegration::LoadFont(BuildFonts);
    }

    std::string strSlotDesc[33];
    if (params.curve) {
        for (int i = 0; i < 32; i++) strSlotDesc[i] = LZFormat("Slot {} - {}", i + 30, LZ(params.curve->slotName[i].c_str()));
        strSlotDesc[32] = LZ("<REMOVE SLOT>");
    }

    ImGuiWindowFlags wndFlags = ImGuiWindowFlags_NoScrollbar;
    if (bMenuHovered) wndFlags |= ImGuiWindowFlags_MenuBar;

    SetWindowSizeLimits(700, 250);
    if (ImGui::Begin("Quick Armor Rebalance", &isActive, wndFlags)) {
        if (g_Config.bShortcutEscCloseWindow && ImGui::Shortcut(ImGuiKey_Escape)) isActive = false;

        // Numpad9: Toggle window collapse (same as clicking the collapse button)
        if (ImGui::IsKeyPressed(ImGuiKey_Keypad9)) {
            ImGui::SetWindowCollapsed(!ImGui::IsWindowCollapsed());
        }

        if (g_Config.strCriticalError.empty()) {
            static int nModFilter = 0;
            static ModFilterSettings modModFilterSettings;
            static char strModFilter[200] = "";

            const char* strModSpecial[] = {LZ("<Currently Worn Armor>"), LZ("<All Items>"), nullptr};
            bool bModSpecialEnabled[] = {true, g_Config.bEnableAllItems};
            static int nModSpecial = 0;

            static bool bFilterChangedMods = false;

            // [ and ] cycle through the visible mod list and equip the matching armor set for that mod.
            // Do not consume printable brackets while the user is editing a text field.
            const bool previousModPressed = ImGui::IsKeyPressed(ImGuiKey_LeftBracket, false);
            const bool nextModPressed = ImGui::IsKeyPressed(ImGuiKey_RightBracket, false);
            if ((previousModPressed || nextModPressed) &&
                ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows) &&
                !ImGui::GetIO().WantTextInput &&
                !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)) {
                auto visibleMods = GetFilteredMods(nModFilter, strModFilter, modModFilterSettings);
                if (!visibleMods.empty()) {
                    auto current = visibleMods.end();
                    if (curMod.size() == 1) current = std::find(visibleMods.begin(), visibleMods.end(), *curMod.begin());

                    std::size_t newIndex;
                    if (current == visibleMods.end()) {
                        newIndex = previousModPressed ? visibleMods.size() - 1 : 0;
                    } else {
                        const auto currentIndex = static_cast<std::size_t>(std::distance(visibleMods.begin(), current));
                        newIndex = previousModPressed
                                       ? (currentIndex + visibleMods.size() - 1) % visibleMods.size()
                                       : (currentIndex + 1) % visibleMods.size();
                    }

                    auto targetMod = visibleMods[newIndex];
                    Local::SwitchToMod(targetMod);

                    auto modItems = Local::SortedModItems(targetMod);
                    if (auto baseItem = Local::PreferredOutfitBase(modItems)) {
                        selectedItems.insert(baseItem);
                        lastSelectedItem = baseItem;
                        keyboardNav = baseItem;
                        Local::EquipMatchingSet(baseItem, modItems);
                    }
                }
            }

            if (bMenuHovered) {
                bMenuHovered = false;

                if (ImGui::BeginMenuBar()) {
                    auto menuSize = ImGui::GetItemRectSize();

                    ImGui::SetNextItemAllowOverlap();
                    if (ImGui::MenuItem(RightAlign(LZ("Settings")))) {
                        popupSettings = true;
                    }

                    ImGui::EndMenuBar();

                    // The menu bar doesn't return the right values (but somehow has the right size?), so instead we
                    // work off the last menu item added
                    bMenuHovered |= ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenOverlapped);  // Doesn't actually work
                    bMenuHovered |= ImGui::IsMouseHoveringRect(ImGui::GetWindowPos(), ImGui::GetItemRectMax(), false);

                    ImGui::SetCursorPosY(ImGui::GetCursorPosY() - menuSize.y);
                }
            }
            bMenuHovered |= ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenOverlapped);

            // ImGui::PushItemWidth(-FLT_MIN);
            ImGui::SetNextItemWidth(-FLT_MIN);

            NPCTargets::DrawControls();
            syncTarget();

            if (ImGui::BeginTable("WindowTable", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {
                ImGui::TableSetupColumn("LeftCol", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("RightCol", ImGuiTableColumnFlags_WidthFixed, 0.25f * ImGui::GetContentRegionAvail().x);

                bool showPopup = false;

                ImGui::TableNextColumn();
                if (ImGui::BeginChild("LeftPane")) {
                    // ImGui::PushItemWidth(-FLT_MIN);

                    // ImGui::SetNextItemWidth(-FLT_MIN);
                    if (ImGui::BeginTable("Mod Table", 3, ImGuiTableFlags_SizingFixedFit, {-FLT_MIN, 0.0f})) {
                        ImGui::TableSetupColumn("ModCol1");
                        ImGui::TableSetupColumn("ModCol2", ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableSetupColumn("ModCol3");
                        ImGui::TableNextColumn();

                        ImGui::Text(LZ("Mod"));
                        ImGui::TableNextColumn();

                        RE::TESFile* blacklist = nullptr;

                        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
                        if (ImGui::BeginCombo("##Mod", !curMod.empty() ? (curMod.size()==1 ? (*curMod.begin())->mod->fileName : LZ("<Multiple mods>")) : strModSpecial[nModSpecial], ImGuiComboFlags_HeightLarge)) {
                            ImGui::Text(LZ("Search:"));
                            ImGui::SameLine();

                            // ImGui::SetNextItemWidth(200);
                            ImGui::InputText("##ModNameFilter", strModFilter, sizeof(strModFilter) - 1, ImGuiInputTextFlags_AutoSelectAll);
                            if (DoClearFilter()) {
                                strModFilter[0] = '\0';
                            }

                            if (!nModFilter) {
                                for (int i = 0; strModSpecial[i]; i++) {
                                    if (!bModSpecialEnabled[i]) continue;
                                    bool selected = curMod.empty() && i == nModSpecial;
                                    if (ImGui::Selectable(strModSpecial[i], selected)) {
                                        curMod.clear();
                                        nModSpecial = i;
                                        g_filterRound++;
                                    }
                                    if (selected) ImGui::SetItemDefaultFocus();
                                }
                            }

                            for (auto i : GetFilteredMods(nModFilter, strModFilter, modModFilterSettings)) {
                                bool selected = curMod.contains(i);

                                int pop = 0;
                                if (g_Data.modifiedFiles.contains(i->mod)) {
                                    // if (bFilterChangedMods) continue;
                                    if (g_Data.modifiedFilesDeleted.contains(i->mod))
                                        ImGui::PushStyleColor(ImGuiCol_Text, colorDeleted);
                                    else
                                        ImGui::PushStyleColor(ImGuiCol_Text, colorChanged);
                                    pop++;
                                } else if (g_Data.modifiedFilesShared.contains(i->mod)) {
                                    // if (bFilterChangedMods) continue;
                                    ImGui::PushStyleColor(ImGuiCol_Text, colorChangedShared);
                                    pop++;
                                }

                                if (ImGui::Selectable(i->mod->fileName, selected)) {
                                    if (isCtrlDown && isAltDown && g_Data.modifiedFiles.contains(i->mod)) {
                                        showPopup = true;
                                    }

                                    Local::SwitchToMod(i, isCtrlDown);
                                }
                                if (isCtrlDown && isAltDown && ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                                    blacklist = i->mod;
                                }

                                if (selected) ImGui::SetItemDefaultFocus();
                                ImGui::PopStyleColor(pop);
                            }

                            ImGui::EndCombo();

                            // data.isWornArmor = curMod.empty();

                            if (blacklist) g_Config.AddUserBlacklist(blacklist);
                        }

                        {
                            const char* popupTitle = LZ("Delete Changes?");
                            if (showPopup) ImGui::OpenPopup(popupTitle);

                            ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

                            if (ImGui::BeginPopupModal(popupTitle, NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
                                ImGui::Text(LZ("Delete all changes to selected mods?"));
                                ImGui::Text(LZ("Changes will not revert until after restarting Skyrim"));

                                if (ImGui::Button(LZ("Delete changes"), ImVec2(120, 0))) {
                                    for (auto i : curMod) DeleteAllChanges(i->mod);
                                    ImGui::CloseCurrentPopup();
                                }
                                ImGui::SetItemDefaultFocus();
                                ImGui::SameLine();
                                if (ImGui::Button(LZ("Cancel"), ImVec2(120, 0))) {
                                    ImGui::CloseCurrentPopup();
                                }
                                ImGui::EndPopup();
                            }
                        }

                        ImGui::TableNextColumn();

                        // ImGui::Checkbox("Hide modified", &bFilterChangedMods);

                        ImGui::SetNextItemWidth(200.0f);

                        const char* modFilterDesc[] = {LZ("No filter"), LZ("Has possible dynamic variants")};

                        if (ImGui::BeginCombo("##FilterMods", modModFilterSettings.Active() ? LZ("<Filters active>") : modFilterDesc[nModFilter], ImGuiComboFlags_HeightLarge)) {
                            {
                                bool selected = nModFilter == 0;
                                if (ImGui::Selectable(modFilterDesc[0], selected)) {
                                    nModFilter = 0;
                                }
                            }
                            {
                                bool selected = nModFilter == 1;
                                if (ImGui::Selectable(modFilterDesc[1], selected)) {
                                    nModFilter = 1;
                                }
                            }

                            InsertModificationFilter(modModFilterSettings);

                            ImGui::EndCombo();
                        }

                        if (DoClearFilter()) {
                            nModFilter = 0;
                            modModFilterSettings.Reset();
                        }
                        ImGui::EndTable();
                    }

                    ImGui::Separator();

                    ImGui::Text(LZ("Filter"));
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(200);
                    if (ImGui::InputTextWithHint("##ItemFilter", LZ("by Name"), filter.nameFilter, sizeof(filter.nameFilter) - 1, ImGuiInputTextFlags_AutoSelectAll))
                        g_filterRound++;

                    if (DoClearFilter()) {
                        filter.nameFilter[0] = '\0';
                        g_filterRound++;
                    }

                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(80);

                    int popCol = 0;
                    if (!filter.nType) {
                        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                        popCol++;
                    }

                    if (ImGui::BeginCombo("##Typefilter", filter.nType ? itemTypes[filter.nType] : LZ("by Type"), ImGuiComboFlags_HeightLarge)) {
                        ImGui::PopStyleColor(popCol);
                        popCol = 0;

                        for (int i = 0; itemTypes[i]; i++) {
                            if (ImGui::Selectable(itemTypes[i])) {
                                filter.nType = i;
                                g_filterRound++;
                            }

                            if (i == ItemFilter::ItemType_Armor) {
                                ImGui::Indent();
                                if (ImGui::Checkbox(LZ("Clothing"), &filter.bArmorClothing)) g_filterRound++;
                                if (ImGui::Checkbox(LZ("Light"), &filter.bArmorLight)) g_filterRound++;
                                if (ImGui::Checkbox(LZ("Heavy"), &filter.bArmorHeavy)) g_filterRound++;
                                ImGui::Unindent();
                            }
                        }
                        ImGui::EndCombo();
                    }
                    if (DoClearFilter()) {
                        filter.nType = ItemFilter::ItemType_All;
                        filter.bArmorClothing = filter.bArmorLight = filter.bArmorHeavy = true;
                        g_filterRound++;
                    }

                    ImGui::PopStyleColor(popCol);
                    popCol = 0;

                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(100);

                    popCol = 0;
                    if (!filter.slots) {
                        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                        popCol++;
                    }

                    if (ImGui::BeginCombo("##Slotfilter", filter.slots ? LZ("Filtering by Slot") : LZ("by Slot"), ImGuiComboFlags_HeightLargest)) {
                        ImGui::PopStyleColor(popCol);
                        popCol = 0;

                        if (ImGui::Selectable(LZ("Clear filter"))) {
                            filter.slots = 0;
                            g_filterRound++;
                        }
                        if (ImGui::RadioButton(LZ("Any of"), &filter.slotMode, ItemFilter::SlotFilterMode::SlotsAny)) g_filterRound++;
                        if (ImGui::RadioButton(LZ("All of"), &filter.slotMode, ItemFilter::SlotFilterMode::SlotsAll)) g_filterRound++;
                        if (ImGui::RadioButton(LZ("Not"), &filter.slotMode, ItemFilter::SlotFilterMode::SlotsNot)) g_filterRound++;

                        constexpr auto slotCols = 4;
                        if (ImGui::BeginTable("##SlotFilterTable", slotCols)) {
                            for (int i = 0; i < 32 / slotCols; i++) {
                                for (int j = 0; j < slotCols; j++) {
                                    auto s = i + j * 32 / slotCols;
                                    bool bCheck = filter.slots & (1 << s);
                                    ImGui::TableNextColumn();
                                    if (ImGui::Checkbox(strSlotDesc[s].c_str(), &bCheck)) {
                                        g_filterRound++;
                                        if (bCheck)
                                            filter.slots |= (1 << s);
                                        else
                                            filter.slots &= ~(1 << s);
                                    }
                                }
                            }
                            ImGui::EndTable();
                        }

                        // Slot filter presets
                        ImGui::Separator();

                        // Get player's equipped slots for Taken/Avail presets
                        // Cached to avoid calling GetWornArmor 32 times per frame (expensive with large inventory)
                        static ArmorSlots playerEquippedSlots = 0;
                        static int slotCacheFrameCounter = 0;
                        constexpr int SLOT_CACHE_REFRESH_FRAMES = 30;  // Refresh every 30 frames (~0.5s at 60fps)

                        if (++slotCacheFrameCounter >= SLOT_CACHE_REFRESH_FRAMES) {
                            slotCacheFrameCounter = 0;
                            playerEquippedSlots = 0;
                            if (auto player = NPCTargets::GetActor()) {
                                for (int slot = 0; slot < 32; slot++) {
                                    auto bipedSlot = static_cast<RE::BGSBipedObjectForm::BipedObjectSlot>(1 << slot);
                                    if (player->GetWornArmor(bipedSlot)) {
                                        playerEquippedSlots |= (1 << slot);
                                    }
                                }
                            }
                        }

                        // Slot bit indices (slot number - 30)
                        constexpr int kSlotHead = 0;        // Slot 30
                        constexpr int kSlotHair = 1;        // Slot 31
                        constexpr int kSlotBody = 2;        // Slot 32
                        constexpr int kSlotHands = 3;       // Slot 33
                        constexpr int kSlotForearms = 4;    // Slot 34
                        constexpr int kSlotAmulet = 5;      // Slot 35
                        constexpr int kSlotRing = 6;        // Slot 36
                        constexpr int kSlotFeet = 7;        // Slot 37
                        constexpr int kSlotCalves = 8;      // Slot 38
                        constexpr int kSlotShield = 9;      // Slot 39
                        constexpr int kSlotTail = 10;       // Slot 40
                        constexpr int kSlotLongHair = 11;   // Slot 41
                        constexpr int kSlotCirclet = 12;    // Slot 42
                        constexpr int kSlotEars = 13;       // Slot 43
                        constexpr int kSlotFace = 14;       // Slot 44
                        constexpr int kSlotNeck = 15;       // Slot 45
                        constexpr int kSlotChest = 16;      // Slot 46
                        constexpr int kSlotBack = 17;       // Slot 47
                        constexpr int kSlotPelvis = 19;     // Slot 49
                        constexpr int kSlotDecapHead = 20;  // Slot 50
                        constexpr int kSlotDecap = 21;      // Slot 51
                        constexpr int kSlotLowerBody = 22;  // Slot 52
                        constexpr int kSlotLegR = 23;       // Slot 53
                        constexpr int kSlotLegL = 24;       // Slot 54
                        constexpr int kSlotFace2 = 25;      // Slot 55
                        constexpr int kSlotChest2 = 26;     // Slot 56
                        constexpr int kSlotShoulder = 27;   // Slot 57
                        constexpr int kSlotArmL = 28;       // Slot 58
                        constexpr int kSlotArmR = 29;       // Slot 59

                        constexpr ArmorSlots kAllSlots = 0xFFFFFFFF;

                        // Preset button definitions: {name, mode, slots}
                        struct SlotPreset {
                            const char* name;
                            int mode;
                            ArmorSlots slots;
                            bool isDynamic;  // true for Taken/Avail which depend on player state
                        };

                        // Build presets array - ordered by slot number for consistency
                        SlotPreset presets[] = {
                            {"Taken", ItemFilter::SlotsAny, playerEquippedSlots, true},
                            {"Avail", ItemFilter::SlotsNot, playerEquippedSlots, true},
                            {"Head", ItemFilter::SlotsNot, kAllSlots & ~(1 << kSlotHead), false},
                            {"Hair", ItemFilter::SlotsNot, kAllSlots & ~(1 << kSlotHair), false},
                            {"Body", ItemFilter::SlotsNot, kAllSlots & ~(1 << kSlotBody), false},
                            {"Hands", ItemFilter::SlotsNot, kAllSlots & ~(1 << kSlotHands), false},
                            {"Forearms", ItemFilter::SlotsNot, kAllSlots & ~(1 << kSlotForearms), false},
                            {"Amulet", ItemFilter::SlotsNot, kAllSlots & ~(1 << kSlotAmulet), false},
                            {"Ring", ItemFilter::SlotsNot, kAllSlots & ~(1 << kSlotRing), false},
                            {"Feet", ItemFilter::SlotsNot, kAllSlots & ~(1 << kSlotFeet), false},
                            {"Calves", ItemFilter::SlotsNot, kAllSlots & ~(1 << kSlotCalves), false},
                            {"Shield", ItemFilter::SlotsNot, kAllSlots & ~(1 << kSlotShield), false},
                            {"Tail", ItemFilter::SlotsNot, kAllSlots & ~(1 << kSlotTail), false},
                            {"HairL", ItemFilter::SlotsNot, kAllSlots & ~(1 << kSlotLongHair), false},
                            {"Circlet", ItemFilter::SlotsNot, kAllSlots & ~(1 << kSlotCirclet), false},
                            {"Ears", ItemFilter::SlotsNot, kAllSlots & ~(1 << kSlotEars), false},
                            {"Face", ItemFilter::SlotsNot, kAllSlots & ~(1 << kSlotFace), false},
                            {"Neck", ItemFilter::SlotsNot, kAllSlots & ~(1 << kSlotNeck), false},
                            {"Chest", ItemFilter::SlotsNot, kAllSlots & ~(1 << kSlotChest), false},
                            {"Back", ItemFilter::SlotsNot, kAllSlots & ~(1 << kSlotBack), false},
                            {"Pelvis", ItemFilter::SlotsNot, kAllSlots & ~(1 << kSlotPelvis), false},
                            {"HeadD", ItemFilter::SlotsNot, kAllSlots & ~(1 << kSlotDecapHead), false},
                            {"Decap", ItemFilter::SlotsNot, kAllSlots & ~(1 << kSlotDecap), false},
                            {"LowBody", ItemFilter::SlotsNot, kAllSlots & ~(1 << kSlotLowerBody), false},
                            {"LegR", ItemFilter::SlotsNot, kAllSlots & ~(1 << kSlotLegR), false},
                            {"LegL", ItemFilter::SlotsNot, kAllSlots & ~(1 << kSlotLegL), false},
                            {"Face2", ItemFilter::SlotsNot, kAllSlots & ~(1 << kSlotFace2), false},
                            {"Chest2", ItemFilter::SlotsNot, kAllSlots & ~(1 << kSlotChest2), false},
                            {"Shoulder", ItemFilter::SlotsNot, kAllSlots & ~(1 << kSlotShoulder), false},
                            {"ArmL", ItemFilter::SlotsNot, kAllSlots & ~(1 << kSlotArmL), false},
                            {"ArmR", ItemFilter::SlotsNot, kAllSlots & ~(1 << kSlotArmR), false},
                        };

                        // Render preset buttons with small font and horizontal wrapping
                        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(4, 2));
                        float originalFontScale = ImGui::GetFont()->Scale;
                        ImGui::GetFont()->Scale *= 0.85f;
                        ImGui::PushFont(ImGui::GetFont());

                        bool firstButton = true;
                        for (const auto& preset : presets) {
                            // Calculate button width
                            float buttonWidth = ImGui::CalcTextSize(preset.name).x +
                                               ImGui::GetStyle().FramePadding.x * 2 +
                                               ImGui::GetStyle().ItemSpacing.x;

                            // Check if we need to wrap to next line
                            if (!firstButton) {
                                ImGui::SameLine();
                                float cursorX = ImGui::GetCursorPosX();
                                float contentMaxX = ImGui::GetWindowContentRegionMax().x;
                                if (cursorX + buttonWidth > contentMaxX) {
                                    ImGui::NewLine();
                                }
                            }
                            firstButton = false;

                            // Disable Taken/Avail if no equipped slots
                            bool disabled = preset.isDynamic && playerEquippedSlots == 0;
                            ImGui::BeginDisabled(disabled);

                            if (ImGui::SmallButton(preset.name)) {
                                filter.slotMode = preset.mode;
                                filter.slots = preset.slots;
                                g_filterRound++;
                            }

                            ImGui::EndDisabled();
                        }

                        ImGui::GetFont()->Scale = originalFontScale;
                        ImGui::PopFont();
                        ImGui::PopStyleVar();

                        ImGui::EndCombo();
                    }

                    if (DoClearFilter()) {
                        filter.slots = 0;
                        g_filterRound++;
                    }

                    ImGui::PopStyleColor(popCol);
                    popCol = 0;

                    ImGui::SameLine();
                    // if (ImGui::Checkbox(LZ("Unmodified"), &filter.bUnmodified)) g_filterRound++;
                    ImGui::SetNextItemWidth(60);

                    if (!(filter.bEnchanted != 2 || filter.bFavorite != 2 || filter.bGroupVariants || filter.modFilters.Active())) {
                        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                        popCol++;
                    }

                    if (ImGui::BeginCombo("##OtherFilters", LZ("Other"), ImGuiComboFlags_HeightLargest)) {
                        ImGui::PopStyleColor(popCol);
                        popCol = 0;

                        if (TriStateCheckbox::Insert(LZ("Enchanted"), &filter.bEnchanted)) g_filterRound++;
                        if (TriStateCheckbox::Insert(LZ("Favorites"), &filter.bFavorite)) g_filterRound++;
                        ImGui::Separator();
                        if (ImGui::Checkbox(LZ("Group Variants"), &filter.bGroupVariants)) g_filterRound++;
                        if (ImGui::IsItemHovered()) {
                            ImGui::SetTooltip(LZ("Group items like 'Armor of X' and 'Armor of Y' together,\nshowing only one per base name"));
                        }
                        InsertModificationFilter(filter.modFilters);

                        ImGui::EndCombo();
                    }
                    if (DoClearFilter()) {
                        filter.modFilters.Reset();
                        filter.bEnchanted = 2;
                        filter.bFavorite = 2;
                        filter.bGroupVariants = false;
                        g_filterRound++;
                    }
                    ImGui::PopStyleColor(popCol);
                    popCol = 0;

                    // Tag filter dropdown (always visible)
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(80);

                    popCol = 0;
                    if (!filter.HasActiveTagFilter()) {
                        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                        popCol++;
                    }

                    if (ImGui::BeginCombo("##TagFilter", filter.HasActiveTagFilter() ? LZ("Filtering") : LZ("by Tag"), ImGuiComboFlags_HeightLarge)) {
                        ImGui::PopStyleColor(popCol);
                        popCol = 0;

                        if (g_Data.globalItemTags.empty()) {
                            // No tags exist yet - show placeholder
                            ImGui::TextDisabled(LZ("No tags defined."));
                            ImGui::TextDisabled(LZ("Use the Tags column to add tags to items."));
                        } else {
                            // All/None/Clear buttons
                            if (ImGui::Button(LZ("All"))) {
                                for (auto& [tag, state] : filter.tagFilters) {
                                    state = TriStateCheckbox::kTrue;
                                }
                                // Ensure all global tags are included
                                for (const auto& tag : g_Data.globalItemTags) {
                                    filter.tagFilters[tag] = TriStateCheckbox::kTrue;
                                }
                                g_filterRound++;
                            }
                            ImGui::SameLine();
                            if (ImGui::Button(LZ("None"))) {
                                for (auto& [tag, state] : filter.tagFilters) {
                                    state = TriStateCheckbox::kFalse;
                                }
                                // Ensure all global tags are included
                                for (const auto& tag : g_Data.globalItemTags) {
                                    filter.tagFilters[tag] = TriStateCheckbox::kFalse;
                                }
                                g_filterRound++;
                            }
                            ImGui::SameLine();
                            if (ImGui::Button(LZ("Clear"))) {
                                for (auto& [tag, state] : filter.tagFilters) {
                                    state = TriStateCheckbox::kEither;
                                }
                                g_filterRound++;
                            }

                            ImGui::Separator();

                            // Show three-way checkboxes for each tag
                            for (const auto& tag : g_Data.globalItemTags) {
                                // Ensure tag exists in filter map with default neutral state
                                if (!filter.tagFilters.contains(tag)) {
                                    filter.tagFilters[tag] = TriStateCheckbox::kEither;
                                }

                                if (TriStateCheckbox::Insert(tag.c_str(), &filter.tagFilters[tag])) {
                                    g_filterRound++;
                                }
                            }

                            // Clean up filter entries for tags that no longer exist
                            std::erase_if(filter.tagFilters, [&](const auto& entry) {
                                return !g_Data.globalItemTags.contains(entry.first);
                            });
                        }

                        ImGui::EndCombo();
                    }
                    if (DoClearFilter()) {
                        // Reset all tag filters to neutral state
                        for (auto& [tag, state] : filter.tagFilters) {
                            state = TriStateCheckbox::kEither;
                        }
                        g_filterRound++;
                    }
                    ImGui::PopStyleColor(popCol);
                    popCol = 0;

                    // Refresh button to force cache rebuild
                    ImGui::SameLine();
                    if (ImGui::SmallButton(LZ("Refresh"))) {
                        g_filterRound++;
                    }
                    MakeTooltip(LZ("Refresh item list to update slot and equipment info"), true);

                    const auto strSpecialConvert = LZ("<Keep previous>");

                    if (ImGui::BeginTable("Convert Table", 3, ImGuiTableFlags_SizingFixedFit)) {
                        ImGui::TableSetupColumn("ConvertCol1");
                        ImGui::TableSetupColumn("ConvertCol2", ImGuiTableColumnFlags_WidthStretch);
                        ImGui::TableSetupColumn("ConvertCol3");
                        ImGui::TableNextColumn();

                        ImGui::Text(LZ("Convert to"));
                        ImGui::TableNextColumn();

                        ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);

                        int nArmorSet = 0;
                        hlConvert.Push();

                        ErrorTrack errConvert;
                        errConvert.Push(!params.armorSet && (!params.bMerge || !(anyItemChanges & eChange_Conversion)));

                        if (ImGui::BeginCombo("##ConvertTo", params.armorSet ? params.armorSet->name.c_str() : strSpecialConvert,
                                              ImGuiComboFlags_PopupAlignLeft | ImGuiComboFlags_HeightLarge)) {
                            {
                                bool selected = !params.armorSet;
                                if (ImGui::Selectable(strSpecialConvert, selected)) params.armorSet = nullptr;
                                if (selected) ImGui::SetItemDefaultFocus();
                                MakeTooltip(LZ("Keeps the existing conversion set.\nIf there is no existing conversion, no changes will be made to the item."), true);
                            }

                            for (auto& i : g_Config.armorSets) {
                                bool selected = params.armorSet == &i;
                                ImGui::PushID(nArmorSet++);
                                if (ImGui::Selectable(LZ(i.name.c_str()), selected)) params.armorSet = &i;
                                if (selected) ImGui::SetItemDefaultFocus();
                                MakeTooltip(i.strContents.c_str(), true);
                                ImGui::PopID();
                            }

                            ImGui::EndCombo();
                        }

                        if (!params.armorSet) needChanges |= eChange_Conversion;

                        errConvert.Pop();
                        hlConvert.Pop();

                        ImGui::TableNextColumn();

                        ImGui::Checkbox(LZ("Merge"), &params.bMerge);
                        MakeTooltip(
                            LZ("When enabled, merge will keep any previous changes not currently being modified.\n"
                               "Enable only the fields you want to be changing.\n"
                               "Disable this to clear all previous changes."));
                        ImGui::SameLine();

                        static HighlightTrack hlApply;
                        hlApply.Push(hlConvert && hlDistributeAs && hlRarity && hlSlots && hlRegion);

                        ImGui::BeginDisabled(hadErrors || data.filteredItems.size() >= g_Config.itemListLimit);

                        static TimedTooltip respApply;
                        bool bApply = false;
                        if (ImGui::Button(LZ("Apply changes"))) {
                            g_Config.Save();

                            if (data.items.size() < kItemApplyWarningThreshhold) {
                                bApply = true;
                            } else
                                ImGui::OpenPopup("###ApplyWarn");
                        }
                        respApply.Show();
                        ImGui::EndDisabled();
                        hlApply.Pop();

                        if (ImGui::BeginPopupModal("Warning###ApplyWarn", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
                            ImGui::Text(LZ("This will change %d items, are you sure?"), data.items.size());

                            if (ImGui::Button(LZ("Yes, apply changes"))) {
                                bApply = true;
                                ImGui::CloseCurrentPopup();
                            }
                            ImGui::SetItemDefaultFocus();
                            ImGui::SameLine();
                            if (ImGui::Button(LZ("Cancel"))) {
                                ImGui::CloseCurrentPopup();
                            }
                            ImGui::EndPopup();
                        }

                        ImGui::EndTable();

                        if (bApply) {
                            auto r = MakeArmorChanges(params);
                            respApply.Enable(LZFormat("{} changes made", r));

                            if (g_Config.bAutoDeleteGiven) givenItems.Remove();

                            hlConvert.Touch();
                            hlDistributeAs.Touch();
                            hlRarity.Touch();
                            hlSlots.Touch();
                            hlDynamicVariants.Touch();
                        }
                    }

                    logger::trace("[GUI] RenderUI - About to call GetCurrentListItems");
                    if (GetCurrentListItems(curMod, nModSpecial, filter, analyzeResults)) {
                        logger::trace("[GUI] RenderUI - GetCurrentListItems returned true (list changed)");
                        for (auto w : g_Config.wordsAutoDisable) {
                            const auto& it = analyzeResults.mapWordItems.find(w);
                            if (it != analyzeResults.mapWordItems.end()) {
                                for (auto item : it->second.items) uncheckedItems.insert(item);
                            }
                        }
                    }
                    g_Config.slotsWillChange = GetConvertableArmorSlots(params);

                    bool hasEnabledArmor = false;
                    bool hasEnabledWeap = false;

                    if (data.filteredItems.size() < g_Config.itemListLimit) {
                        for (auto i : data.filteredItems) {
                            if (i->As<RE::TESObjectARMO>()) {
                                if (!uncheckedItems.contains(i)) hasEnabledArmor = true;
                            } else if (i->As<RE::TESObjectWEAP>()) {
                                if (!uncheckedItems.contains(i)) hasEnabledWeap = true;
                            } else if (i->As<RE::TESAmmo>()) {
                                if (!uncheckedItems.contains(i)) hasEnabledWeap = true;
                            }
                        }
                    }

                    // Distribution
                    ImGui::Separator();
                    // ImGui::BeginDisabled(curMod.empty());  // || !params.armorSet);

                    // Need to create a dummy table to negate stretching the combo boxes
                    ImGui::Checkbox(LZ("Distribute as "), &params.bDistribute);
                    if (curMod.empty()) MakeTooltip(LZ("Distribution can only be configured for a single mod at a time."));
                    // else if (!params.armorSet)
                    //     MakeTooltip(LZ("Cannot add or change distribution without a conversion set selected."));
                    else
                        MakeTooltip(LZ("Additions or changes to loot distribution will not take effect until you restart Skyrim"));
                    ImGui::BeginDisabled(!params.bDistribute);

                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(180);

                    hlDistributeAs.Push(params.bDistribute);

                    ErrorTrack errDistributeAs;
                    errDistributeAs.Push(params.bDistribute && !params.distProfile && (!params.bMerge || !(anyItemChanges & eChange_Loot)));

                    if (ImGui::BeginCombo("##DistributeAs", params.distProfile ? params.distProfile : strSpecialConvert,
                                          ImGuiComboFlags_PopupAlignLeft | ImGuiComboFlags_HeightLarge)) {
                        {
                            bool selected = !params.distProfile;
                            if (ImGui::Selectable(strSpecialConvert, selected)) params.distProfile = nullptr;
                            if (selected) ImGui::SetItemDefaultFocus();
                            // MakeTooltip(LZ("Keeps the existing conversion set.\nIf there is no existing conversion, no changes will be made to the item."), true);
                        }

                        for (auto& i : g_Config.lootProfiles) {
                            bool selected = params.distProfile == i.c_str();
                            if (ImGui::Selectable(LZ(i.c_str()), selected)) params.distProfile = i.c_str();
                            if (selected) ImGui::SetItemDefaultFocus();
                        }

                        ImGui::EndCombo();
                    }

                    if (params.bDistribute && !params.distProfile) needChanges |= eChange_Loot;

                    errDistributeAs.Pop();
                    hlDistributeAs.Pop();

                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(100);

                    hlRarity.Push(params.bDistribute);

                    ErrorTrack errRarity;
                    errRarity.Push(params.bDistribute && params.rarity < 0 && (!params.bMerge || !(anyItemChanges & eChange_Loot)));

                    if (ImGui::BeginCombo("##Rarity", params.rarity >= 0 ? rarity[params.rarity] : strSpecialConvert, ImGuiComboFlags_PopupAlignLeft)) {
                        {
                            bool selected = params.rarity < 0;
                            if (ImGui::Selectable(strSpecialConvert, selected)) params.rarity = -1;
                            if (selected) ImGui::SetItemDefaultFocus();
                            // MakeTooltip(LZ("Keeps the existing conversion set.\nIf there is no existing conversion, no changes will be made to the item."), true);
                        }

                        for (int i = 0; rarity[i]; i++) {
                            bool selected = i == params.rarity;
                            if (ImGui::Selectable(rarity[i], selected)) params.rarity = i;
                            if (selected) ImGui::SetItemDefaultFocus();
                        }

                        ImGui::EndCombo();
                    }

                    if (params.bDistribute && params.rarity < 0) needChanges |= eChange_Loot;

                    errRarity.Pop();
                    hlRarity.Pop();

                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(140);

                    hlRegion.Push(params.bDistribute && params.region && params.region != kRegion_KeepPrevious);

                    auto& profileRegions = g_Config.lootProfileRegions[params.distProfile];
                    if (params.region != kRegion_KeepPrevious && !profileRegions.empty() && !profileRegions.contains(params.region)) params.region = nullptr;
                    if (ImGui::BeginCombo("##Region",
                                          params.region ? (params.region == kRegion_KeepPrevious ? strSpecialConvert : LZ(params.region->name.c_str())) : LZ("<Anywhere>"),
                                          ImGuiComboFlags_PopupAlignLeft | ImGuiComboFlags_HeightLarge)) {
                        if (ImGui::Selectable(strSpecialConvert, params.region == kRegion_KeepPrevious)) params.region = kRegion_KeepPrevious;
                        if (ImGui::Selectable(LZ("<Anywhere>"), !params.region)) params.region = nullptr;
                        for (auto region : g_Config.lsRegionsSorted) {
                            bool selected = region == params.region;

                            ImGui::BeginDisabled(!profileRegions.empty() && !profileRegions.contains(region));
                            if (ImGui::Selectable(LZ(region->name.c_str()), selected)) params.region = region;
                            if (selected) ImGui::SetItemDefaultFocus();
                            ImGui::EndDisabled();
                        }

                        ImGui::EndCombo();
                    }

                    hlRegion.Pop();

                    ImGui::Indent(60);
                    ImGui::Checkbox(LZ("As pieces"), &params.bDistAsPieces);
                    ImGui::SameLine();
                    ImGui::Checkbox(LZ("As whole set"), &params.bDistAsSet);
                    MakeTooltip(LZ("You will find entire sets (all slots) together"));
                    ImGui::SameLine();
                    ImGui::BeginDisabled(!params.bDistAsSet);
                    ImGui::Checkbox(LZ("Matching sets"), &params.bMatchSetPieces);
                    MakeTooltip(
                        LZ("Attempts to match sets together - for example, if there are green and blue variants, it will\n"
                           "try to distribute only green or only blue parts as a single set"));
                    ImGui::EndDisabled();  //! params.bDistAsSet

                    hlDynamicVariants.Push(!analyzeResults.sets[AnalyzeResults::eWords_DynamicVariants].empty() ||
                                           !analyzeResults.sets[AnalyzeResults::eWords_EitherVariants].empty());

                    ImGui::EndDisabled();  //! params.bDistribute
                    ImGui::SameLine();

                    if (ImGui::Button(LZ("Dynamic Variants"))) {
                        popupDynamicVariants = true;
                    }
                    hlDynamicVariants.Pop();

                    ImGui::Unindent(60);

                    //ImGui::EndDisabled(); //curMod.empty()

                    // Modifications
                    if (ImGui::BeginChild("ItemTypeFrame", {ImGui::GetContentRegionAvail().x, 0}, false)) {
                        {
                            auto cursorPos = ImGui::GetCursorPos();
                            ImGui::SetNextItemAllowOverlap();
                            if (ImGui::Checkbox(RightAlign(LZ("Alt Settings"), ImGui::GetFrameHeight() + ImGui::GetStyle().ItemInnerSpacing.x), &bUseScratchParams)) {
                                paramsScratch.Reset(true);
                                paramsScratch.Clear();
                            }
                            MakeTooltip(
                                LZ("Switches to settings with everything disabled.\n"
                                   "Useful for making individual changes when used with Merge enabled."));

                            ImGui::SetCursorPos(cursorPos);
                        }

                        // Vertical resizable split: Tabs (top) | Bottom section (sliders, keywords, recipes)
                        static float tabsHeight = -1.0f; // Initial height for tabs section (-1 = not initialized)
                        float availableHeight = ImGui::GetContentRegionAvail().y;
                        float bottomSectionMinHeight = 200.0f; // Minimum height for bottom section
                        float tabsMaxHeight = availableHeight - bottomSectionMinHeight;

                        // Initialize tabs height to 60% of available space on first run
                        if (tabsHeight < 0.0f) {
                            tabsHeight = availableHeight * 0.6f;
                        }

                        // Clamp tabs height to reasonable bounds
                        if (tabsHeight > tabsMaxHeight) tabsHeight = tabsMaxHeight;
                        if (tabsHeight < 100.0f) tabsHeight = 100.0f;

                        // Top section: Tabs
                        if (ImGui::BeginChild("TabsSection", ImVec2(0, tabsHeight), false)) {
                            static int iTabOpen = 0;
                            int iTab = 0;
                            int iTabSelected = iTabOpen;

                            bool bTabEnabled[] = {hasEnabledArmor && (!params.armorSet || !params.armorSet->items.empty()),
                                                  hasEnabledWeap && (!params.armorSet || !params.armorSet->weaps.empty()), true, true};
                            bTabEnabled[2] = bTabEnabled[0] || bTabEnabled[1];
                            const int nTabCount = 4;

                            bool bForceSelect = false;
                            if (!bTabEnabled[iTabOpen]) {
                                iTabOpen = 0;
                                while (iTabOpen < nTabCount && !bTabEnabled[iTabOpen]) iTabOpen++;
                                if (iTabOpen >= nTabCount) iTabOpen = 0;

                                bForceSelect = true;
                            }

                            const char* tabLabels[] = {LZ("Armor"), LZ("Weapons"), LZ("Enchantments"), LZ("Outfits")};

                            iTabSelected = iTabOpen;

                            if (ImGui::BeginTabBar("ItemTypeBar")) {
                                // Armor tab
                                ImGui::BeginDisabled(!bTabEnabled[iTab]);
                                if (ImGui::BeginTabItem(tabLabels[iTab], nullptr, bForceSelect && iTabOpen == iTab ? ImGuiTabItemFlags_SetSelected : 0)) {
                                iTabSelected = iTab;
                                if (SliderTable()) {
                                    SliderRow(LZ("Armor Rating"), params.armor.rating, g_Config.flatArmorMod);
                                    SliderRow(LZ("Weight"), params.armor.weight, g_Config.flatWeightMod, 1);

                                    // Warmth
                                    {
                                        const char* warmthDesc[] = {
                                            LZ("None"),       //<0.1
                                            LZ("Cold"),       //<0.2
                                            LZ("Poor"),       //<0.3
                                            LZ("Limited"),    //<0.4
                                            LZ("Fair"),       //<0.5
                                            LZ("Average"),    //<0.6
                                            LZ("Good"),       //<0.7
                                            LZ("Warm"),       //<0.8
                                            LZ("Full"),       //<0.9
                                            LZ("Excellent"),  //<1.0
                                            LZ("Maximum"),    //=1.0
                                        };

                                        const char* coverageDesc[] = {
                                            LZ("None"),         //<0.1
                                            LZ("Minimal"),      //<0.2
                                            LZ("Poor"),         //<0.3
                                            LZ("Limited"),      //<0.4
                                            LZ("Fair"),         //<0.5
                                            LZ("Average"),      //<0.6
                                            LZ("Good"),         //<0.7
                                            LZ("Significant"),  //<0.8
                                            LZ("Full"),         //<0.9
                                            LZ("Excellent"),    //<1.0
                                            LZ("Maximum"),      //=1.0
                                        };

                                        bool showCoverage = g_Config.isFrostfallInstalled || g_Config.bShowFrostfallCoverage;

                                        auto& pair = params.armor.warmth;
                                        ImGui::TableNextRow();
                                        ImGui::TableNextColumn();

                                        ImGui::Checkbox(LZ("Modify Warmth"), &pair.bModify);
                                        MakeTooltip(
                                            LZ("The total warmth of the armor set.\n"
                                               "This number is NOT relative to the base armor set's warmth."));
                                        ImGui::BeginDisabled(!pair.bModify);
                                        ImGui::TableNextColumn();

                                        ImGui::SetNextItemWidth((showCoverage ? 0.5f : 1.0f) * ImGui::GetContentRegionAvail().x);
                                        ImGui::SliderFloat("##WarmthSlider", &pair.fScale, 0.f, 100.f, "%.0f%% Warmth", ImGuiSliderFlags_AlwaysClamp);
                                        MakeTooltip(warmthDesc[(int)(0.1f * pair.fScale)]);

                                        if (showCoverage) {
                                            ImGui::SameLine();
                                            ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x);
                                            ImGui::SliderFloat("##CoverageSlider", &params.armor.coverage, 0.f, 100.f, "%.0f%% Coverage", ImGuiSliderFlags_AlwaysClamp);
                                            MakeTooltip(coverageDesc[(int)(0.1f * params.armor.coverage)]);
                                        }

                                        ImGui::TableNextColumn();
                                        if (ImGui::Button(LZ("Reset"))) {
                                            pair.fScale = 50.0f;
                                            params.armor.coverage = 50.0f;
                                        }
                                        ImGui::EndDisabled();
                                    }

                                    ImGui::EndTable();
                                }

                                ImGui::Separator();
                                ImGui::Text(LZ("Stat distribution curve"));
                                ImGui::SameLine();

                                static auto* curCurve = &g_Config.curves[0];

                                ImGui::SetNextItemWidth(220);
                                if (ImGui::BeginCombo("##Curve", curCurve->first.c_str(), ImGuiComboFlags_PopupAlignLeft | ImGuiComboFlags_HeightLarge)) {
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
                                if (ImGui::Button(LZ("Remap Slots"))) popupRemapSlots = true;
                                hlSlots.Pop();

                                ImGui::EndTabItem();
                            }
                            iTab++;
                            ImGui::EndDisabled();

                            // Weapon tab
                            ImGui::BeginDisabled(!bTabEnabled[iTab]);
                            if (ImGui::BeginTabItem(tabLabels[iTab], nullptr, bForceSelect && iTabOpen == iTab ? ImGuiTabItemFlags_SetSelected : 0)) {
                                iTabSelected = iTab;
                                if (SliderTable()) {
                                    SliderRow(LZ("Damage"), params.weapon.damage, g_Config.flatWeapDamageMod);
                                    SliderRow(LZ("Weight"), params.weapon.weight, g_Config.flatWeightMod, 1);
                                    SliderRow(LZ("Speed"), params.weapon.speed);
                                    SliderRow(LZ("Stagger"), params.weapon.stagger);

                                    ImGui::EndTable();
                                }

                                ImGui::EndTabItem();
                            }
                            iTab++;
                            ImGui::EndDisabled();

                            // Enchantment tab
                            ImGui::BeginDisabled(!bTabEnabled[iTab]);
                            if (ImGui::BeginTabItem(tabLabels[iTab], nullptr, bForceSelect && iTabOpen == iTab ? ImGuiTabItemFlags_SetSelected : 0)) {
                                iTabSelected = iTab;

                                ImGui::Text(LZ("List"));
                                ImGui::SameLine();

                                ImGui::SetNextItemWidth(220);
                                if (ImGui::BeginCombo("##Curve", params.ench.pool ? LZ(params.ench.pool->name.c_str()) : "<None>",
                                                      ImGuiComboFlags_PopupAlignLeft | ImGuiComboFlags_HeightLarge)) {
                                    {
                                        bool selected = !params.ench.pool;
                                        if (ImGui::Selectable(LZ("<None>"), selected)) params.ench.pool = nullptr;
                                        if (selected) ImGui::SetItemDefaultFocus();
                                    }

                                    std::vector<EnchantmentPool*> pools;
                                    pools.reserve(g_Config.mapEnchPools.size());
                                    for (auto& i : g_Config.mapEnchPools) pools.push_back(&i.second);
                                    std::sort(pools.begin(), pools.end(), [](auto a, auto b) { return a->name.compare(b->name) < 0; });

                                    for (auto i : pools) {
                                        bool selected = params.ench.pool == i;
                                        if (ImGui::Selectable(LZ(i->name.c_str()), selected)) params.ench.pool = i;
                                        if (selected) ImGui::SetItemDefaultFocus();
                                        MakeTooltip(i->strContents.c_str(), true);
                                    }

                                    ImGui::EndCombo();
                                }

                                ImGui::BeginDisabled(!params.ench.pool);

                                ImGui::SameLine();
                                ImGui::Checkbox(LZ("Only"), &params.ench.poolRestrict);

                                ImGui::SameLine();
                                ImGui::BeginDisabled(params.ench.poolRestrict);

                                static float fMaxChance = 100.0f;
                                float* pfChance = params.ench.poolRestrict ? &fMaxChance : &params.ench.poolChance;

                                ImGui::SetNextItemWidth(-1.0f);
                                ImGui::SliderFloat("##Bias", pfChance, 0.0f, 100.0f, "%.0f%% chance from this list", ImGuiSliderFlags_AlwaysClamp);
                                ImGui::EndDisabled();
                                ImGui::EndDisabled();

                                if (SliderTable()) {
                                    SliderRow(LZ("Chance"), params.ench.rate, 0, 0, 0.0f, 500.0f);
                                    SliderRow(LZ("Power"), params.ench.power, 0, 0, 10.0f, 300.0f);

                                    ImGui::EndTable();
                                }

                                ImGui::PushID("StripEnch");
                                ImGui::Checkbox(LZ("Remove preset enchantments from"), &params.ench.strip);
                                MakeTooltip(LZ("Items that have a preset enchantment cannot receive random enchantments."));

                                ImGui::BeginDisabled(!params.ench.strip);
                                ImGui::SameLine();
                                ImGui::Checkbox(LZ("Armor"), &params.ench.stripArmor);
                                ImGui::SameLine();
                                ImGui::Checkbox(LZ("Weapons"), &params.ench.stripWeapons);
                                ImGui::SameLine();
                                ImGui::Checkbox(LZ("Staves"), &params.ench.stripStaves);
                                ImGui::EndDisabled();
                                ImGui::PopID();

                                ImGui::EndTabItem();
                            }
                            iTab++;
                            ImGui::EndDisabled();

                            // Outfits tab
                            ImGui::BeginDisabled(!bTabEnabled[iTab]);
                            if (ImGui::BeginTabItem(tabLabels[iTab], nullptr,
                                bForceSelect && iTabOpen == iTab ? ImGuiTabItemFlags_SetSelected : 0)) {

                                iTabSelected = iTab;

                                // State variables
                                static std::string selectedOutfit;
                                static char outfitNameFilter[200] = "";
                                static char newOutfitName[65] = "";
                                static bool showCreateDialog = false;
                                static bool showRenameDialog = false;
                                static std::string renameTarget;
                                static std::set<RE::TESBoundObject*> selectedOutfitItems;
                                static char outfitPrefix[65] = "outfit";
                                static bool ctrlCProcessedInModal = false;
                                static bool focusPrefixTextbox = false;
                                static bool showCreateTagDialog = false;
                                static char newTagName[33] = "name";
                                static std::set<std::string> globalTags;
                                static std::map<std::string, int> outfitTagFilter;
                                static ImGuiTableSortSpecs* outfitItemsSortSpecs = nullptr;
                                static int outfitItemsSortColumn = 0;  // 0=Name, 1=Slot, 2=Armor, 3=ID
                                static bool outfitItemsSortAscending = true;

                                // Enchant dialog state
                                static bool showEnchantDialog = false;
                                static RE::TESBoundObject* enchantTarget = nullptr;
                                static std::string enchantTargetOutfit;
                                static RE::EnchantmentItem* selectedEnchantment = nullptr;
                                static float enchantMagnitude = 1.0f;
                                static char enchantSearchFilter[64] = "";

                                // Stats transfer dialog state
                                static bool showStatsTransferDialog = false;
                                static RE::TESBoundObject* statsTransferTarget = nullptr;
                                static std::string statsTransferTargetOutfit;
                                static RE::TESObjectARMO* selectedStatsSource = nullptr;
                                static char statsSearchFilter[64] = "";
                                static std::vector<RE::TESObjectARMO*> inventoryArmorCache;
                                static int inventoryCacheFrame = -1;
                                static bool transferEnchantment = true;  // Option to also transfer enchantment

                                // Modal window settings (position/size persistence)
                                struct ModalSettings {
                                    ImVec2 pos = ImVec2(0, 0);
                                    ImVec2 size = ImVec2(0, 0);
                                    bool initialized = false;
                                };
                                static ModalSettings enchantModalSettings;
                                static ModalSettings statsTransferModalSettings;

                                // Reset the modal Ctrl+C flag at the start of each frame
                                ctrlCProcessedInModal = false;

                                // Rebuild global tags at the start of each frame
                                globalTags = RebuildGlobalTags();

                                // Two-pane layout: Outfits list (left) | Outfit items (right)
                                ImGui::PushStyleVar(ImGuiStyleVar_CellPadding, ImVec2(0, 0));
                                if (ImGui::BeginTable("OutfitsTable", 2,
                                    ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV)) {

                                    ImGui::TableSetupColumn("OutfitsCol", ImGuiTableColumnFlags_WidthStretch);
                                    ImGui::TableSetupColumn("ItemsCol", ImGuiTableColumnFlags_WidthFixed,
                                                           0.4f * ImGui::GetContentRegionAvail().x);

                                    // LEFT PANE: Outfits List
                                    ImGui::TableNextColumn();
                                    if (ImGui::BeginChild("OutfitsPane")) {
                                        ImGui::Text(LZ("Filter"));

                                        // Filter by name
                                        ImGui::SameLine();
                                        ImGui::SetNextItemWidth(200);
                                        ImGui::InputTextWithHint("##OutfitFilter", LZ("by name"),
                                                                outfitNameFilter, sizeof(outfitNameFilter) - 1,
                                                                ImGuiInputTextFlags_AutoSelectAll);

                                        // Tags filter dropdown - limit width to fit before divider
                                        ImGui::SameLine();
                                        ImGui::SetNextItemWidth(100);
                                        if (ImGui::BeginCombo("##TagsFilter", LZ("by Tag"), ImGuiComboFlags_HeightLarge)) {
                                            // All/None/Clear buttons
                                            if (ImGui::Button(LZ("All"))) {
                                                for (auto& [tag, state] : outfitTagFilter) {
                                                    state = TriStateCheckbox::kTrue;
                                                }
                                            }
                                            ImGui::SameLine();
                                            if (ImGui::Button(LZ("None"))) {
                                                for (auto& [tag, state] : outfitTagFilter) {
                                                    state = TriStateCheckbox::kFalse;
                                                }
                                            }
                                            ImGui::SameLine();
                                            if (ImGui::Button(LZ("Clear"))) {
                                                for (auto& [tag, state] : outfitTagFilter) {
                                                    state = TriStateCheckbox::kEither;
                                                }
                                            }

                                            ImGui::Separator();

                                            // Three-way checkboxes for each global tag
                                            for (const auto& tag : globalTags) {
                                                // Ensure tag exists in filter map
                                                if (!outfitTagFilter.contains(tag)) {
                                                    outfitTagFilter[tag] = TriStateCheckbox::kEither;
                                                }
                                                TriStateCheckbox::Insert(tag.c_str(), &outfitTagFilter[tag]);
                                            }

                                            // Clean up filter entries for tags that no longer exist
                                            std::erase_if(outfitTagFilter, [&](const auto& entry) {
                                                return !globalTags.contains(entry.first);
                                            });

                                            ImGui::EndCombo();
                                        }

                                        // Prefix label and textbox
                                        ImGui::Text(LZ("Prefix"));
                                        ImGui::SameLine();
                                        float textboxWidth = 150.0f;
                                        ImGui::SetNextItemWidth(textboxWidth);

                                        // Set focus if Ctrl+X was pressed
                                        if (focusPrefixTextbox) {
                                            ImGui::SetKeyboardFocusHere();
                                            focusPrefixTextbox = false;
                                        }

                                        // Callback to filter input: prevent typing when Ctrl is pressed (except Shift for uppercase)
                                        auto prefixCallback = [](ImGuiInputTextCallbackData*) -> int {
                                            // Block input if any Ctrl key is pressed
                                            if (ImGui::IsKeyDown(ImGuiKey_LeftCtrl) || ImGui::IsKeyDown(ImGuiKey_RightCtrl)) {
                                                return 1; // Discard character
                                            }
                                            return 0; // Accept character
                                        };

                                        ImGui::InputTextWithHint("##OutfitPrefix", LZ("outfit"), outfitPrefix, sizeof(outfitPrefix) - 1,
                                            ImGuiInputTextFlags_AutoSelectAll | ImGuiInputTextFlags_CallbackCharFilter,
                                            prefixCallback);
                                        if (ImGui::IsItemHovered()) {
                                            ImGui::SetTooltip(LZ("Default prefix for new outfits (e.g., 'outfit' -> 'outfit0001')"));
                                        }

                                        // Create / Overwrite buttons
                                        ImGui::SameLine();
                                        if (ImGui::Button(LZ("Create Outfit"))) {
                                            showCreateDialog = true;

                                            // Shift names it after the selected mod instead of the prefix box
                                            std::string prefix;
                                            if (isShiftDown && curMod.size() == 1) prefix = OutfitPrefixFromMod((*curMod.begin())->mod);
                                            if (prefix.empty()) prefix = outfitPrefix;

                                            auto suggestedName = SuggestOutfitName(prefix);
                                            strncpy(newOutfitName, suggestedName.c_str(), sizeof(newOutfitName) - 1);
                                            newOutfitName[sizeof(newOutfitName) - 1] = '\0';
                                        }
                                        MakeTooltip(
                                            LZ("Creates an outfit from what you are currently wearing.\n"
                                               "Hold Shift to name it after the selected mod instead of the prefix."));

                                        ImGui::Separator();

                                        // Outfits list - use -FLT_MIN for width to fill horizontal space
                                        ImVec2 listSize(-FLT_MIN, ImGui::GetContentRegionAvail().y);
                                        if (ImGui::BeginListBox("##OutfitsList", listSize)) {

                                            // Context menu for outfits
                                            if (ImGui::BeginPopupContextWindow()) {
                                                if (!selectedOutfit.empty()) {
                                                    if (ImGui::Selectable(LZ("Overwrite with current items"))) {
                                                        // Count how many items were recently given that need time to equip
                                                        int currentFrame = ImGui::GetFrameCount();
                                                        int recentItemCount = 0;
                                                        for (const auto& [frameAdded, item] : givenItems.items) {
                                                            if (currentFrame - frameAdded < 30) {
                                                                recentItemCount++;
                                                            }
                                                        }

                                                        // Skip enough frames for all recent items to equip, plus buffer
                                                        int framesToSkip = static_cast<int>(std::max(3, recentItemCount + 5) * g_Config.outfitFrameMultiplier);
                                                        for (int i = 0; i < framesToSkip; i++) {
                                                            g_Pause.SkipFrame();
                                                        }

                                                        // Get currently equipped items
                                                        auto equipped = GetEquippedItems(NPCTargets::GetActor());

                                                        // Create outfit from equipped items
                                                        Outfit newOutfit;
                                                        newOutfit.name = selectedOutfit;
                                                        newOutfit.items.clear();
                                                        newOutfit.itemFormIDs.clear();

                                                        // Preserve existing tags and enhanced items
                                                        if (g_Data.outfits.contains(selectedOutfit)) {
                                                            newOutfit.tags = g_Data.outfits[selectedOutfit].tags;
                                                            // Copy enhanced items - will filter to only keep those still in outfit
                                                            newOutfit.enhancedItems = g_Data.outfits[selectedOutfit].enhancedItems;
                                                        }

                                                        // Build set of new formIDs for filtering enhanced items
                                                        std::unordered_set<std::string> newFormIDs;

                                                        for (auto item : equipped) {
                                                            std::string formID = QARFormID(item);
                                                            newOutfit.itemFormIDs.push_back(formID);
                                                            newOutfit.items.insert(item);
                                                            newFormIDs.insert(formID);
                                                        }

                                                        // Remove enhanced items that are no longer in the outfit
                                                        std::erase_if(newOutfit.enhancedItems, [&newFormIDs](const auto& pair) {
                                                            return !newFormIDs.contains(pair.first);
                                                        });

                                                        // Save outfit
                                                        if (SaveOutfit(selectedOutfit, newOutfit)) {
                                                            g_Data.outfits[selectedOutfit] = newOutfit;
                                                        }
                                                    }

                                                    if (ImGui::Selectable(LZ("Rename"))) {
                                                        showRenameDialog = true;
                                                        renameTarget = selectedOutfit;
                                                        strncpy(newOutfitName, selectedOutfit.c_str(), sizeof(newOutfitName) - 1);
                                                        newOutfitName[sizeof(newOutfitName) - 1] = '\0';
                                                    }

                                                    if (ImGui::Selectable(LZ("Delete"))) {
                                                        if (DeleteOutfit(selectedOutfit)) {
                                                            selectedOutfit.clear();
                                                            selectedOutfitItems.clear();
                                                        }
                                                    }

                                                    ImGui::Separator();

                                                    if (ImGui::Selectable(LZ("New Tag"))) {
                                                        showCreateTagDialog = true;
                                                        strncpy(newTagName, "name", sizeof(newTagName) - 1);
                                                        newTagName[sizeof(newTagName) - 1] = '\0';
                                                    }
                                                }
                                                ImGui::EndPopup();
                                            }

                                            // Render outfit list
                                            for (auto& [name, outfit] : g_Data.outfits) {
                                                // Apply name filter
                                                if (outfitNameFilter[0] != '\0' &&
                                                    !StringContainsI(name.c_str(), outfitNameFilter)) {
                                                    continue;
                                                }

                                                // Apply tag filter
                                                bool passesTagFilter = true;
                                                for (const auto& [tag, filterState] : outfitTagFilter) {
                                                    if (filterState == TriStateCheckbox::kTrue) {
                                                        // Outfit must have this tag
                                                        if (!outfit.tags.contains(tag)) {
                                                            passesTagFilter = false;
                                                            break;
                                                        }
                                                    } else if (filterState == TriStateCheckbox::kFalse) {
                                                        // Outfit must NOT have this tag
                                                        if (outfit.tags.contains(tag)) {
                                                            passesTagFilter = false;
                                                            break;
                                                        }
                                                    }
                                                    // kEither: no effect
                                                }
                                                if (!passesTagFilter) continue;

                                                bool selected = (selectedOutfit == name);

                                                if (ImGui::Selectable(name.c_str(), &selected, ImGuiSelectableFlags_AllowDoubleClick)) {
                                                    // Double-click to equip entire outfit
                                                    if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                                                        // Keep outfit selected
                                                        selectedOutfit = name;
                                                        selectedOutfitItems.clear();

                                                        // Unequip current items first
                                                        givenItems.UnequipCurrent();

                                                        // Equip all items from the outfit
                                                        for (auto item : outfit.items) {
                                                            std::string formID = QARFormID(item);
                                                            if (outfit.enhancedItems.contains(formID) &&
                                                                outfit.enhancedItems[formID].IsEnhanced()) {
                                                                givenItems.GiveEnhanced(item, outfit.enhancedItems[formID], true);
                                                            } else {
                                                                givenItems.Give(item, true);
                                                            }
                                                        }

                                                        // Save as last equipped outfit for Numpad1 restore
                                                        g_LastEquippedOutfit = name;
                                                    } else {
                                                        // Single click: selection handling
                                                        if (selected) {
                                                            selectedOutfit = name;
                                                            selectedOutfitItems.clear();
                                                        } else {
                                                            selectedOutfit.clear();
                                                            selectedOutfitItems.clear();
                                                        }
                                                    }
                                                }

                                                // Tooltip showing tags for this outfit
                                                if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_NoSharedDelay)) {
                                                    if (!outfit.tags.empty()) {
                                                        if (ImGui::BeginTooltip()) {
                                                            ImGui::Text(LZ("Tags:"));
                                                            for (const auto& tag : outfit.tags) {
                                                                ImGui::BulletText("%s", tag.c_str());
                                                            }
                                                            ImGui::EndTooltip();
                                                        }
                                                    }
                                                }
                                            }

                                            // Keyboard shortcuts for outfit list
                                            if (ImGui::IsWindowFocused() && !selectedOutfit.empty()) {
                                                // F2 - Rename outfit
                                                if (ImGui::IsKeyPressed(ImGuiKey_F2)) {
                                                    showRenameDialog = true;
                                                    renameTarget = selectedOutfit;
                                                    strncpy(newOutfitName, selectedOutfit.c_str(), sizeof(newOutfitName) - 1);
                                                    newOutfitName[sizeof(newOutfitName) - 1] = '\0';
                                                }

                                                // Delete - Remove outfit
                                                if (ImGui::IsKeyPressed(ImGuiKey_Delete)) {
                                                    if (DeleteOutfit(selectedOutfit)) {
                                                        selectedOutfit.clear();
                                                        selectedOutfitItems.clear();
                                                    }
                                                }
                                            }

                                            ImGui::EndListBox();
                                        }

                                        ImGui::EndChild();
                                    }

                                    // RIGHT PANE: Outfit Items
                                    ImGui::TableNextColumn();
                                    if (ImGui::BeginChild("OutfitItemsPane")) {
                                        // Add left padding to separate content from the vertical divider
                                        float rightPanePadding = ImGui::GetStyle().ItemSpacing.x;
                                        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + rightPanePadding);
                                        ImGui::BeginGroup();

                                        if (!selectedOutfit.empty() && g_Data.outfits.contains(selectedOutfit)) {
                                            auto& outfit = g_Data.outfits[selectedOutfit];

                                            ImGui::Text(LZ("Items in outfit: %s"), selectedOutfit.c_str());
                                            ImGui::Separator();

                                            // Calculate space for tags pane: base size + one line per tag
                                            float tagsHeaderHeight = ImGui::GetTextLineHeightWithSpacing();
                                            float tagCheckboxHeight = ImGui::GetTextLineHeightWithSpacing() * globalTags.size();
                                            float tagsPaneHeight = tagsHeaderHeight + tagCheckboxHeight + ImGui::GetStyle().ItemSpacing.y * 2;
                                            float minTagsPaneHeight = tagsHeaderHeight + ImGui::GetTextLineHeightWithSpacing(); // Minimum for "Select an outfit" message

                                            // Items list takes remaining space minus tags pane
                                            float itemsAvailableHeight = ImGui::GetContentRegionAvail().y;
                                            float itemsListHeight = itemsAvailableHeight - std::max(tagsPaneHeight, minTagsPaneHeight) - ImGui::GetStyle().ItemSpacing.y;

                                            ImVec2 tableSize(-FLT_MIN, std::max(100.0f, itemsListHeight)); // Minimum 100px for items, full width

                                            // Get player for slot conflict detection
                                            auto outfitPlayer = NPCTargets::GetActor();

                                            // Pre-compute equipped slots cache with incremental refresh (optimization)
                                            static EquippedSlotsCache outfitEquippedCache;
                                            if (g_EquipmentChanged) {
                                                outfitEquippedCache.ForceFullRefresh();
                                            }
                                            outfitEquippedCache.Refresh(outfitPlayer);

                                            // Pre-compute ItemSlotInfo for all outfit items (optimization)
                                            static std::unordered_map<RE::TESBoundObject*, ItemSlotInfo> outfitSlotInfoCache;
                                            outfitSlotInfoCache.clear();
                                            outfitSlotInfoCache.reserve(outfit.items.size());
                                            for (auto item : outfit.items) {
                                                outfitSlotInfoCache[item] = ItemSlotInfo::GetForItem(item, params, &outfitEquippedCache);
                                            }

                                            // Build sorted items vector
                                            std::vector<RE::TESBoundObject*> sortedOutfitItems(outfit.items.begin(), outfit.items.end());

                                            // Sort items based on current sort column (using cached info)
                                            std::sort(sortedOutfitItems.begin(), sortedOutfitItems.end(),
                                                [&](RE::TESBoundObject* a, RE::TESBoundObject* b) {
                                                    switch (outfitItemsSortColumn) {
                                                        case 0: {  // Sort by name
                                                            std::string nameA = a->GetName();
                                                            std::string nameB = b->GetName();
                                                            if (nameA.empty()) {
                                                                auto fileA = a->GetFile(0);
                                                                nameA = std::format("{}:{:010x}", fileA ? fileA->fileName : "<dynamic>", a->formID);
                                                            }
                                                            if (nameB.empty()) {
                                                                auto fileB = b->GetFile(0);
                                                                nameB = std::format("{}:{:010x}", fileB ? fileB->fileName : "<dynamic>", b->formID);
                                                            }
                                                            int cmp = _stricmp(nameA.c_str(), nameB.c_str());
                                                            return outfitItemsSortAscending ? cmp < 0 : cmp > 0;
                                                        }
                                                        case 1: {  // Sort by slot (using cached info)
                                                            const auto& slotA = outfitSlotInfoCache[a];
                                                            const auto& slotB = outfitSlotInfoCache[b];
                                                            if (slotA.primarySlot < 0 && slotB.primarySlot < 0) return false;
                                                            if (slotA.primarySlot < 0) return !outfitItemsSortAscending;
                                                            if (slotB.primarySlot < 0) return outfitItemsSortAscending;
                                                            return outfitItemsSortAscending ? slotA.primarySlot < slotB.primarySlot : slotA.primarySlot > slotB.primarySlot;
                                                        }
                                                        case 2: {  // Sort by armor (using cached info)
                                                            const auto& infoA = outfitSlotInfoCache[a];
                                                            const auto& infoB = outfitSlotInfoCache[b];
                                                            if (infoA.armorRating == 0 && infoB.armorRating == 0) return false;
                                                            if (infoA.armorRating == 0) return !outfitItemsSortAscending;
                                                            if (infoB.armorRating == 0) return outfitItemsSortAscending;
                                                            return outfitItemsSortAscending ? infoA.armorRating < infoB.armorRating : infoA.armorRating > infoB.armorRating;
                                                        }
                                                        case 3: {  // Sort by ID (using cached info)
                                                            const auto& slotA = outfitSlotInfoCache[a];
                                                            const auto& slotB = outfitSlotInfoCache[b];
                                                            if (slotA.slotIDs.empty() && slotB.slotIDs.empty()) return false;
                                                            if (slotA.slotIDs.empty()) return !outfitItemsSortAscending;
                                                            if (slotB.slotIDs.empty()) return outfitItemsSortAscending;
                                                            int cmp = slotA.slotIDs.compare(slotB.slotIDs);
                                                            return outfitItemsSortAscending ? cmp < 0 : cmp > 0;
                                                        }
                                                        case 4: {  // Sort by NIF (female model path)
                                                            std::string nifA, nifB;
                                                            if (auto armorA = a->As<RE::TESObjectARMO>()) {
                                                                for (auto addon : armorA->armorAddons) {
                                                                    if (addon && !addon->bipedModels[RE::SEXES::kFemale].model.empty()) {
                                                                        nifA = addon->bipedModels[RE::SEXES::kFemale].model.c_str();
                                                                        break;
                                                                    }
                                                                }
                                                            }
                                                            if (auto armorB = b->As<RE::TESObjectARMO>()) {
                                                                for (auto addon : armorB->armorAddons) {
                                                                    if (addon && !addon->bipedModels[RE::SEXES::kFemale].model.empty()) {
                                                                        nifB = addon->bipedModels[RE::SEXES::kFemale].model.c_str();
                                                                        break;
                                                                    }
                                                                }
                                                            }
                                                            if (nifA.empty() && nifB.empty()) return false;
                                                            if (nifA.empty()) return !outfitItemsSortAscending;
                                                            if (nifB.empty()) return outfitItemsSortAscending;
                                                            int cmp = _stricmp(nifA.c_str(), nifB.c_str());
                                                            return outfitItemsSortAscending ? cmp < 0 : cmp > 0;
                                                        }
                                                        default:
                                                            return false;
                                                    }
                                                });

                                            // Colors for columns
                                            ImVec4 slotColor = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
                                            ImVec4 outfitSlotConflictColor = ImVec4(0.8f, 0.3f, 0.3f, 1.0f);  // Muted red for slot conflicts

                                            // Get cached column configuration (no allocation)
                                            int outfitVisibleColCount = g_Config.tableColumns.GetVisibleCount();
                                            if (outfitVisibleColCount < 1) outfitVisibleColCount = 1;
                                            const int* outfitDisplayOrder = g_Config.tableColumns.GetDisplayOrder();

                                            if (ImGui::BeginTable("##OutfitItemsTable", outfitVisibleColCount,
                                                ImGuiTableFlags_Sortable | ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg |
                                                ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_Resizable,
                                                tableSize)) {

                                                // Setup columns based on visibility and order
                                                for (int idx = 0; idx < Config::TableColumnConfig::Col_Count; idx++) {
                                                    int col = outfitDisplayOrder[idx];
                                                    if (!g_Config.tableColumns.visible[col]) continue;

                                                    switch (col) {
                                                        case Config::TableColumnConfig::Col_Name:
                                                            ImGui::TableSetupColumn(LZ("Name"), ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_WidthStretch, 0.0f, 0);
                                                            break;
                                                        case Config::TableColumnConfig::Col_Slot:
                                                            ImGui::TableSetupColumn(LZ("Slot"), ImGuiTableColumnFlags_WidthFixed, 80.0f, 1);
                                                            break;
                                                        case Config::TableColumnConfig::Col_Armor:
                                                            ImGui::TableSetupColumn(LZ("Armor"), ImGuiTableColumnFlags_WidthFixed, 50.0f, 2);
                                                            break;
                                                        case Config::TableColumnConfig::Col_ID:
                                                            ImGui::TableSetupColumn(LZ("ID"), ImGuiTableColumnFlags_WidthFixed, 60.0f, 3);
                                                            break;
                                                        case Config::TableColumnConfig::Col_NifF:
                                                            ImGui::TableSetupColumn(LZ("Nif (F)"), ImGuiTableColumnFlags_WidthFixed, 200.0f, 4);
                                                            break;
                                                        case Config::TableColumnConfig::Col_Mod:
                                                            ImGui::TableSetupColumn(LZ("Mod"), ImGuiTableColumnFlags_WidthFixed, 150.0f, 5);
                                                            break;
                                                    }
                                                }
                                                ImGui::TableSetupScrollFreeze(0, 1);
                                                ImGui::TableHeadersRow();

                                                // Handle sorting
                                                if (ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs()) {
                                                    if (sortSpecs->SpecsDirty && sortSpecs->SpecsCount > 0) {
                                                        outfitItemsSortColumn = sortSpecs->Specs[0].ColumnUserID;
                                                        outfitItemsSortAscending = sortSpecs->Specs[0].SortDirection == ImGuiSortDirection_Ascending;
                                                        sortSpecs->SpecsDirty = false;
                                                    }
                                                }

                                                // Context menu for items
                                                if (ImGui::BeginPopupContextWindow()) {
                                                    ImGui::BeginDisabled(selectedOutfitItems.empty());
                                                    if (ImGui::Selectable(LZ("Remove from outfit"))) {
                                                        // Remove selected items from outfit
                                                        for (auto item : selectedOutfitItems) {
                                                            outfit.items.erase(item);

                                                            // Remove from itemFormIDs
                                                            std::string formID = QARFormID(item);
                                                            auto it = std::find(outfit.itemFormIDs.begin(),
                                                                               outfit.itemFormIDs.end(), formID);
                                                            if (it != outfit.itemFormIDs.end()) {
                                                                outfit.itemFormIDs.erase(it);
                                                            }

                                                            // Also remove any enhancements
                                                            outfit.enhancedItems.erase(formID);
                                                        }

                                                        // Save updated outfit
                                                        SaveOutfit(selectedOutfit, outfit);
                                                        selectedOutfitItems.clear();
                                                    }
                                                    ImGui::EndDisabled();

                                                    ImGui::Separator();

                                                    // Enchant option - only for single armor selection
                                                    bool canEnchant = selectedOutfitItems.size() == 1 &&
                                                                      (*selectedOutfitItems.begin())->As<RE::TESObjectARMO>();
                                                    ImGui::BeginDisabled(!canEnchant);
                                                    if (ImGui::Selectable(LZ("Enchant..."))) {
                                                        showEnchantDialog = true;
                                                        enchantTarget = *selectedOutfitItems.begin();
                                                        enchantTargetOutfit = selectedOutfit;
                                                        selectedEnchantment = nullptr;
                                                        enchantMagnitude = 1.0f;
                                                        enchantSearchFilter[0] = '\0';

                                                        // Load existing enchantment magnitude if any
                                                        std::string formID = QARFormID(enchantTarget);
                                                        if (outfit.enhancedItems.contains(formID) &&
                                                            outfit.enhancedItems[formID].HasEnchantment()) {
                                                            enchantMagnitude = outfit.enhancedItems[formID].enchantmentMagnitude;
                                                        }
                                                    }
                                                    ImGui::EndDisabled();

                                                    // Transfer Stats option
                                                    ImGui::BeginDisabled(!canEnchant);
                                                    if (ImGui::Selectable(LZ("Transfer Stats..."))) {
                                                        showStatsTransferDialog = true;
                                                        statsTransferTarget = *selectedOutfitItems.begin();
                                                        statsTransferTargetOutfit = selectedOutfit;
                                                        selectedStatsSource = nullptr;
                                                        statsSearchFilter[0] = '\0';
                                                    }
                                                    ImGui::EndDisabled();

                                                    // Clear Enhancements option
                                                    bool hasEnhancements = false;
                                                    for (auto item : selectedOutfitItems) {
                                                        std::string formID = QARFormID(item);
                                                        if (outfit.enhancedItems.contains(formID) &&
                                                            outfit.enhancedItems[formID].IsEnhanced()) {
                                                            hasEnhancements = true;
                                                            break;
                                                        }
                                                    }
                                                    ImGui::BeginDisabled(!hasEnhancements);
                                                    if (ImGui::Selectable(LZ("Clear Enhancements"))) {
                                                        for (auto item : selectedOutfitItems) {
                                                            std::string formID = QARFormID(item);
                                                            outfit.enhancedItems.erase(formID);
                                                        }
                                                        SaveOutfit(selectedOutfit, outfit);
                                                    }
                                                    ImGui::EndDisabled();

                                                    ImGui::EndPopup();
                                                }

                                                // Render items
                                                static RE::TESBoundObject* lastSelectedOutfitItem = nullptr;

                                                for (auto item : sortedOutfitItems) {
                                                    std::string name(item->GetName());
                                                    if (name.empty()) {
                                                        auto file = item->GetFile(0);
                                                        name = std::format("{}:{:010x}", file ? file->fileName : "<dynamic>", item->formID);
                                                    }

                                                    // Add favorite indicator
                                                    if (g_Data.favoriteItems.contains(item)) {
                                                        name = "* " + name;
                                                    }

                                                    // Check if item is enhanced
                                                    std::string itemFormID = QARFormID(item);
                                                    bool isEnhanced = outfit.enhancedItems.contains(itemFormID) &&
                                                                     outfit.enhancedItems[itemFormID].IsEnhanced();

                                                    // Add enhanced indicator
                                                    if (isEnhanced) {
                                                        name = "[+] " + name;
                                                    }

                                                    // Get cached slot info for this item (no recomputation)
                                                    const auto& slotInfo = outfitSlotInfoCache[item];

                                                    ImGui::TableNextRow();

                                                    // Name column
                                                    ImGui::TableNextColumn();
                                                    bool selected = selectedOutfitItems.contains(item);

                                                    // Apply enhanced item color
                                                    if (isEnhanced) {
                                                        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(150, 200, 255, 255));
                                                    }

                                                    ImGui::PushID(item->GetFormID());
                                                    if (ImGui::Selectable(name.c_str(), &selected,
                                                        ImGuiSelectableFlags_AllowDoubleClick | ImGuiSelectableFlags_SpanAllColumns)) {

                                                        // Double-click to toggle equip (with enhancements if configured)
                                                        if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                                                            std::string itemFormID = QARFormID(item);
                                                            if (outfit.enhancedItems.contains(itemFormID) &&
                                                                outfit.enhancedItems[itemFormID].IsEnhanced()) {
                                                                // Enhanced item - need to get the actual enhanced form to toggle
                                                                auto& config = outfit.enhancedItems[itemFormID];
                                                                if (auto armor = item->As<RE::TESObjectARMO>()) {
                                                                    // Get or create the enhanced armor
                                                                    if (NPCTargets::IsNPC()) {
                                                                        givenItems.NPCAction(NPCTargets::Action::Toggle, armor, false, true, &config);
                                                                    } else {
                                                                        auto enhancedArmor = CreateEnhancedArmor(armor, config);
                                                                        if (enhancedArmor) {
                                                                            // Check if equipping or unequipping
                                                                            bool wasWorn = givenItems.IsArmorWorn(enhancedArmor, NPCTargets::GetActor());
                                                                            // Toggle the enhanced version
                                                                            givenItems.ToggleEquip(enhancedArmor);
                                                                            // Track for cosave if we just equipped
                                                                            if (!wasWorn) {
                                                                                TrackEquippedEnhancedArmor(armor, enhancedArmor, config, true);
                                                                            } else {
                                                                                UntrackEnhancedArmor(enhancedArmor);
                                                                            }
                                                                        } else {
                                                                            // Fallback to base item
                                                                            givenItems.ToggleEquip(item);
                                                                        }
                                                                    }
                                                                } else {
                                                                    givenItems.ToggleEquip(item);
                                                                }
                                                            } else {
                                                                givenItems.ToggleEquip(item);
                                                            }
                                                        } else {
                                                            // Single click: selection handling
                                                            if (!isCtrlDown) {
                                                                selectedOutfitItems.clear();
                                                                selected = true;
                                                            }

                                                            if (!isShiftDown) {
                                                                lastSelectedOutfitItem = item;
                                                                if (selected)
                                                                    selectedOutfitItems.insert(item);
                                                                else
                                                                    selectedOutfitItems.erase(item);
                                                            } else {
                                                                // Shift-click: range selection
                                                                if (lastSelectedOutfitItem == item) {
                                                                    selectedOutfitItems.insert(item);
                                                                } else if (lastSelectedOutfitItem) {
                                                                    bool adding = false;
                                                                    for (auto j : sortedOutfitItems) {
                                                                        if (j == item || j == lastSelectedOutfitItem) {
                                                                            if (adding) {
                                                                                selectedOutfitItems.insert(j);
                                                                                break;
                                                                            }
                                                                            adding = true;
                                                                        }
                                                                        if (adding) selectedOutfitItems.insert(j);
                                                                    }
                                                                }
                                                            }
                                                        }
                                                    }
                                                    ImGui::PopID();

                                                    // Pop enhanced item color
                                                    if (isEnhanced) {
                                                        ImGui::PopStyleColor();
                                                    }

                                                    // Tooltip for outfit items
                                                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_NoSharedDelay)) {
                                                        if (ImGui::BeginTooltip()) {
                                                            ImGui::PushStyleColor(ImGuiCol_Text, colorTextDefault);

                                                            // Show file info
                                                            if (auto file = item->GetFile(0)) {
                                                                ImGui::Text(LZ("File: %s"), file->fileName);
                                                            } else {
                                                                ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 200, 100, 255));
                                                                ImGui::Text(LZ("Dynamic Item"));
                                                                ImGui::TextWrapped(LZ("This item was created at runtime (e.g. by Transmog).\nIt will not persist in outfits/favorites after restarting the game."));
                                                                ImGui::PopStyleColor();
                                                            }

                                                            // Show enhancement info if present
                                                            if (isEnhanced) {
                                                                ImGui::Separator();
                                                                ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(150, 200, 255, 255));
                                                                ImGui::Text(LZ("Enhancements:"));
                                                                ImGui::PopStyleColor();

                                                                const auto& enhConfig = outfit.enhancedItems[itemFormID];

                                                                if (enhConfig.HasEnchantment()) {
                                                                    // Try to resolve enchantment name
                                                                    std::string enchName = enhConfig.enchantmentFormID;
                                                                    if (auto enchForm = LookupForm(enhConfig.enchantmentFormID)) {
                                                                        if (auto enchItem = enchForm->As<RE::EnchantmentItem>()) {
                                                                            enchName = enchItem->GetName();
                                                                        }
                                                                    }
                                                                    ImGui::BulletText(LZ("Enchantment: %s (%.2fx)"),
                                                                        enchName.c_str(), enhConfig.enchantmentMagnitude);
                                                                }

                                                                if (enhConfig.HasStatsTransfer()) {
                                                                    ImGui::BulletText(LZ("Stats from: %s"),
                                                                        enhConfig.statsSourceFormID.c_str());
                                                                    if (enhConfig.armorRating.has_value()) {
                                                                        ImGui::BulletText(LZ("  Armor: %d"), enhConfig.armorRating.value());
                                                                    }
                                                                    if (enhConfig.weight.has_value()) {
                                                                        ImGui::BulletText(LZ("  Weight: %.1f"), enhConfig.weight.value());
                                                                    }
                                                                    if (enhConfig.value.has_value()) {
                                                                        ImGui::BulletText(LZ("  Value: %d"), enhConfig.value.value());
                                                                    }
                                                                }

                                                                ImGui::Separator();
                                                            }

                                                            if (auto armor = item->As<RE::TESObjectARMO>()) {
                                                                static const char* strArmorType[] = {"Light Armor", "Heavy Armor", "Clothing"};
                                                                int nType = (int)armor->bipedModelData.armorType.get();
                                                                if (nType >= 0 && nType <= 2) ImGui::Text(LZ(strArmorType[nType]));

                                                                if (params.curve) {
                                                                    ImGui::Text("Slots:");
                                                                    ImGui::Indent();
                                                                    auto slotsCur = (ArmorSlots)armor->GetSlotMask().underlying();
                                                                    auto slotsOrig = MapFindOr(g_Data.modifiedArmorSlots, armor, slotsCur);
                                                                    auto slotsCombined = slotsCur | slotsOrig;

                                                                    // Get player's equipped armor for each slot
                                                                    auto tooltipPlayer = NPCTargets::GetActor();

                                                                    if (slotsCombined) {
                                                                        for (int slot = 0; slot < 32; slot++) {
                                                                            if (slotsCombined & (1 << slot)) {
                                                                                // Check if player has an item equipped in this slot
                                                                                RE::TESObjectARMO* equippedArmor = nullptr;
                                                                                if (tooltipPlayer) {
                                                                                    auto bipedSlot = static_cast<RE::BGSBipedObjectForm::BipedObjectSlot>(1 << slot);
                                                                                    equippedArmor = tooltipPlayer->GetWornArmor(bipedSlot);
                                                                                    // Don't count as "equipped" if it's the same item we're hovering over
                                                                                    if (equippedArmor == armor) equippedArmor = nullptr;
                                                                                }

                                                                                // Determine text color and prefix
                                                                                bool isSlotTaken = equippedArmor != nullptr;

                                                                                if (!(slotsCur & (1 << slot))) {
                                                                                    // Slot was removed (deleted)
                                                                                    ImGui::PushStyleColor(ImGuiCol_Text, colorDeleted);
                                                                                    ImGui::Text(LZFormat("-Slot {} - {}", slot + 30, LZ(params.curve->slotName[slot].c_str())).c_str());
                                                                                    ImGui::PopStyleColor();
                                                                                } else if (!(slotsOrig & (1 << slot))) {
                                                                                    // Slot was added
                                                                                    if (isSlotTaken) {
                                                                                        ImGui::PushStyleColor(ImGuiCol_Text, colorDeleted);
                                                                                        ImGui::Text(LZFormat("+Slot {} - {}", slot + 30, LZ(params.curve->slotName[slot].c_str())).c_str());
                                                                                        ImGui::Text(LZFormat("  Replaces: {}", equippedArmor->GetName()).c_str());
                                                                                        ImGui::PopStyleColor();
                                                                                    } else {
                                                                                        ImGui::PushStyleColor(ImGuiCol_Text, colorChanged);
                                                                                        ImGui::Text(LZFormat("+Slot {} - {}", slot + 30, LZ(params.curve->slotName[slot].c_str())).c_str());
                                                                                        ImGui::PopStyleColor();
                                                                                    }
                                                                                } else {
                                                                                    // Unchanged slot
                                                                                    if (isSlotTaken) {
                                                                                        ImGui::PushStyleColor(ImGuiCol_Text, colorDeleted);
                                                                                        ImGui::Text(LZFormat("Slot {} - {}", slot + 30, LZ(params.curve->slotName[slot].c_str())).c_str());
                                                                                        ImGui::Text(LZFormat("  Replaces: {}", equippedArmor->GetName()).c_str());
                                                                                        ImGui::PopStyleColor();
                                                                                    } else {
                                                                                        ImGui::Text(LZFormat("Slot {} - {}", slot + 30, LZ(params.curve->slotName[slot].c_str())).c_str());
                                                                                    }
                                                                                }
                                                                            }
                                                                        }
                                                                    } else {
                                                                        ImGui::Text(LZ("None"));
                                                                    }
                                                                    ImGui::Unindent();

                                                                    if (armor->formEnchanting) ImGui::Text(LZFormat("Enchantment: {}", armor->formEnchanting->GetFullName()).c_str());

                                                                    if (armor->numKeywords > 0) {
                                                                        ImGui::Text(LZ("Keywords:"));
                                                                        ImGui::Indent();
                                                                        for (unsigned int n = 0; n < armor->numKeywords; n++)
                                                                            if (armor->keywords[n]) ImGui::Text(armor->keywords[n]->GetFormEditorID());
                                                                        ImGui::Unindent();
                                                                    }
                                                                }

                                                            } else if (auto weapon = item->As<RE::TESObjectWEAP>()) {
                                                                static const char* strWeaponType[] = {"Fist", "1H Sword", "1H Dagger", "1H Axe", "1H Mace",
                                                                                                      "2H Sword", "2H Axe", "Bow", "Staff", "Crossbow"};
                                                                int nType = (int)weapon->GetWeaponType();
                                                                if (nType >= 0 && nType <= 9) ImGui::Text(LZ(strWeaponType[nType]));

                                                                if (weapon->formEnchanting) ImGui::Text(LZFormat("Enchantment: {}", weapon->formEnchanting->GetFullName()).c_str());

                                                                if (weapon->numKeywords > 0) {
                                                                    ImGui::Text(LZ("Keywords:"));
                                                                    ImGui::Indent();
                                                                    for (unsigned int n = 0; n < weapon->numKeywords; n++)
                                                                        if (weapon->keywords[n]) ImGui::Text(weapon->keywords[n]->GetFormEditorID());
                                                                    ImGui::Unindent();
                                                                }
                                                            }

                                                            ImGui::PopStyleColor();
                                                            ImGui::EndTooltip();
                                                        }
                                                    }

                                                    // Render additional columns based on visibility and order
                                                    for (int idx = 0; idx < Config::TableColumnConfig::Col_Count; idx++) {
                                                        int col = outfitDisplayOrder[idx];
                                                        if (!g_Config.tableColumns.visible[col]) continue;
                                                        if (col == Config::TableColumnConfig::Col_Name) continue;  // Name column already rendered

                                                        ImGui::TableNextColumn();

                                                        switch (col) {
                                                            case Config::TableColumnConfig::Col_Slot:
                                                                // Slot column - muted red if conflict, otherwise light grey
                                                                if (slotInfo.hasConflict) {
                                                                    ImGui::PushStyleColor(ImGuiCol_Text, outfitSlotConflictColor);
                                                                } else {
                                                                    ImGui::PushStyleColor(ImGuiCol_Text, slotColor);
                                                                }
                                                                ImGui::Text("%s", slotInfo.slotName.c_str());
                                                                ImGui::PopStyleColor();
                                                                break;

                                                            case Config::TableColumnConfig::Col_Armor:
                                                                // Armor column - show armor rating or empty for non-armor
                                                                ImGui::PushStyleColor(ImGuiCol_Text, slotColor);
                                                                if (slotInfo.armorRating > 0) {
                                                                    ImGui::Text("%.0f", slotInfo.armorRating);
                                                                }
                                                                ImGui::PopStyleColor();
                                                                break;

                                                            case Config::TableColumnConfig::Col_ID:
                                                                // ID column (light grey)
                                                                ImGui::PushStyleColor(ImGuiCol_Text, slotColor);
                                                                ImGui::Text("%s", slotInfo.slotIDs.c_str());
                                                                ImGui::PopStyleColor();
                                                                break;

                                                            case Config::TableColumnConfig::Col_NifF:
                                                                // NIF (Female) column - compute from armor
                                                                ImGui::PushStyleColor(ImGuiCol_Text, slotColor);
                                                                if (auto armor = item->As<RE::TESObjectARMO>()) {
                                                                    for (auto addon : armor->armorAddons) {
                                                                        if (addon && !addon->bipedModels[RE::SEXES::kFemale].model.empty()) {
                                                                            ImGui::Text("%s", addon->bipedModels[RE::SEXES::kFemale].model.c_str());
                                                                            break;
                                                                        }
                                                                    }
                                                                }
                                                                ImGui::PopStyleColor();
                                                                break;

                                                            case Config::TableColumnConfig::Col_Mod:
                                                                // Mod column
                                                                ImGui::PushStyleColor(ImGuiCol_Text, slotColor);
                                                                if (auto file = item->GetFile(0)) {
                                                                    ImGui::Text("%s", file->fileName);
                                                                }
                                                                ImGui::PopStyleColor();
                                                                break;
                                                        }
                                                    }
                                                }

                                                // F key for favorites
                                                if (ImGui::IsWindowFocused() && !selectedOutfitItems.empty() &&
                                                    ImGui::IsKeyPressed(ImGuiKey_F)) {
                                                    for (auto item : selectedOutfitItems) {
                                                        std::string formID = QARFormID(item);

                                                        if (g_Data.favoriteItems.contains(item)) {
                                                            g_Data.favoriteItems.erase(item);
                                                            g_Data.favoriteItemsMap.erase(formID);
                                                        } else {
                                                            g_Data.favoriteItems.insert(item);
                                                            g_Data.favoriteItemsMap.insert(formID);
                                                        }
                                                    }
                                                    SaveFavorites();
                                                    // Note: No g_filterRound++ needed - favorite status is checked live during rendering
                                                }

                                                ImGui::EndTable();
                                            }

                                            // TAGS PANE
                                            ImGui::Separator();
                                            ImGui::Text(LZ("Tags"));
                                            ImGui::SameLine();
                                            if (ImGui::SmallButton(LZ("Add"))) {
                                                showCreateTagDialog = true;
                                                strncpy(newTagName, "name", sizeof(newTagName) - 1);
                                                newTagName[sizeof(newTagName) - 1] = '\0';
                                            }
                                            ImGui::Separator();

                                            // Display checkboxes for all global tags with horizontal wrapping
                                            bool firstTag = true;

                                            for (const auto& tag : globalTags) {
                                                // Make a local copy to avoid iterator invalidation
                                                std::string tagCopy = tag;
                                                bool hasTag = outfit.tags.contains(tagCopy);

                                                // Calculate checkbox width (text + checkbox box + spacing)
                                                float checkboxWidth = ImGui::CalcTextSize(tagCopy.c_str()).x +
                                                                     ImGui::GetFrameHeight() +
                                                                     ImGui::GetStyle().ItemInnerSpacing.x * 2 +
                                                                     ImGui::GetStyle().ItemSpacing.x;

                                                // Check if we need to wrap to next line (recalculate bounds each iteration for resizable columns)
                                                if (!firstTag) {
                                                    // Try to place on same line first
                                                    ImGui::SameLine();

                                                    // Now check if it would actually fit after SameLine positioned the cursor
                                                    float cursorX = ImGui::GetCursorPosX();
                                                    float contentMaxX = ImGui::GetWindowContentRegionMax().x;
                                                    float itemEndX = cursorX + checkboxWidth;

                                                    // If it would overflow, cancel the SameLine and start a new line
                                                    if (itemEndX > contentMaxX) {
                                                        ImGui::NewLine();
                                                    }
                                                }
                                                firstTag = false;

                                                if (ImGui::Checkbox(tagCopy.c_str(), &hasTag)) {
                                                    // Toggle tag
                                                    if (hasTag) {
                                                        outfit.tags.insert(tagCopy);
                                                    } else {
                                                        outfit.tags.erase(tagCopy);
                                                    }

                                                    // Save outfit immediately
                                                    SaveOutfit(selectedOutfit, outfit);

                                                    // Note: globalTags is rebuilt at the start of each frame,
                                                    // so changes will be reflected on next frame
                                                }
                                            }
                                        } else {
                                            ImGui::TextDisabled(LZ("Select an outfit to view items and tags"));
                                        }

                                        ImGui::EndGroup();
                                        ImGui::EndChild();
                                    }

                                    ImGui::EndTable();
                                }
                                ImGui::PopStyleVar();

                                // Create Outfit Dialog
                                if (showCreateDialog) {
                                    ImGui::OpenPopup(LZ("Create Outfit"));
                                }

                                ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),
                                                       ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

                                if (ImGui::BeginPopupModal(LZ("Create Outfit"), &showCreateDialog,
                                    ImGuiWindowFlags_AlwaysAutoResize)) {

                                    ImGui::Text(LZ("Enter outfit name:"));

                                    // Set focus to input text when modal first appears
                                    if (ImGui::IsWindowAppearing()) {
                                        ImGui::SetKeyboardFocusHere();
                                    }

                                    ImGui::InputText("##OutfitName", newOutfitName, sizeof(newOutfitName) - 1, ImGuiInputTextFlags_AutoSelectAll);

                                    // Validation feedback
                                    bool nameValid = IsValidOutfitName(newOutfitName);
                                    bool nameExists = g_Data.outfits.contains(newOutfitName);

                                    if (newOutfitName[0] != '\0' && !nameValid) {
                                        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 0, 0, 255));
                                        ImGui::Text(LZ("Invalid name. Use only alphanumeric, _, -, and space"));
                                        ImGui::PopStyleColor();
                                    }

                                    if (nameExists) {
                                        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 255, 0, 255));
                                        ImGui::Text(LZ("Name already exists"));
                                        ImGui::PopStyleColor();
                                    }

                                    ImGui::Separator();

                                    ImGui::BeginDisabled(!nameValid || nameExists || newOutfitName[0] == '\0');
                                    if (ImGui::Button(LZ("Create"), ImVec2(120, 0))) {
                                        // Count how many items were recently given that need time to equip
                                        // Each item needs at least one frame to equip due to Skyrim limitations
                                        int currentFrame = ImGui::GetFrameCount();
                                        int recentItemCount = 0;
                                        for (const auto& [frameAdded, item] : givenItems.items) {
                                            // Count items added in the last 30 frames as "recent"
                                            if (currentFrame - frameAdded < 30) {
                                                recentItemCount++;
                                            }
                                        }

                                        // Skip enough frames for all recent items to equip, plus buffer
                                        // Apply user-configurable multiplier for slower machines
                                        int framesToSkip = static_cast<int>(std::max(3, recentItemCount + 5) * g_Config.outfitFrameMultiplier);
                                        for (int i = 0; i < framesToSkip; i++) {
                                            g_Pause.SkipFrame();
                                        }

                                        // Get currently equipped items
                                        auto equipped = GetEquippedItems(NPCTargets::GetActor());

                                        // Create outfit
                                        Outfit outfit;
                                        outfit.name = newOutfitName;

                                        for (auto item : equipped) {
                                            std::string formID = QARFormID(item);
                                            outfit.itemFormIDs.push_back(formID);
                                            outfit.items.insert(item);
                                        }

                                        // Save outfit
                                        if (SaveOutfit(newOutfitName, outfit)) {
                                            g_Data.outfits[newOutfitName] = outfit;
                                            selectedOutfit = newOutfitName;
                                            selectedOutfitItems.clear();
                                        }

                                        showCreateDialog = false;
                                        ImGui::CloseCurrentPopup();
                                    }
                                    ImGui::EndDisabled();

                                    ImGui::SetItemDefaultFocus();
                                    ImGui::SameLine();

                                    if (ImGui::Button(LZ("Cancel"), ImVec2(120, 0))) {
                                        showCreateDialog = false;
                                        ImGui::CloseCurrentPopup();
                                    }

                                    // Handle Enter or Ctrl+C to confirm creation
                                    bool isCtrlC = ImGui::IsKeyPressed(ImGuiKey_C) && (ImGui::IsKeyDown(ImGuiKey_LeftCtrl) || ImGui::IsKeyDown(ImGuiKey_RightCtrl));
                                    if ((ImGui::IsKeyPressed(ImGuiKey_Enter) || isCtrlC) &&
                                        nameValid && !nameExists && newOutfitName[0] != '\0') {

                                        // Mark that we processed Ctrl+C in the modal to prevent tab handler from also triggering
                                        if (isCtrlC) {
                                            ctrlCProcessedInModal = true;
                                        }
                                        // Count how many items were recently given that need time to equip
                                        // Each item needs at least one frame to equip due to Skyrim limitations
                                        int currentFrame = ImGui::GetFrameCount();
                                        int recentItemCount = 0;
                                        for (const auto& [frameAdded, item] : givenItems.items) {
                                            // Count items added in the last 30 frames as "recent"
                                            if (currentFrame - frameAdded < 30) {
                                                recentItemCount++;
                                            }
                                        }

                                        // Skip enough frames for all recent items to equip, plus buffer
                                        // Apply user-configurable multiplier for slower machines
                                        int framesToSkip = static_cast<int>(std::max(3, recentItemCount + 5) * g_Config.outfitFrameMultiplier);
                                        for (int i = 0; i < framesToSkip; i++) {
                                            g_Pause.SkipFrame();
                                        }

                                        // Get currently equipped items
                                        auto equipped = GetEquippedItems(NPCTargets::GetActor());

                                        // Create outfit
                                        Outfit outfit;
                                        outfit.name = newOutfitName;

                                        for (auto item : equipped) {
                                            std::string formID = QARFormID(item);
                                            outfit.itemFormIDs.push_back(formID);
                                            outfit.items.insert(item);
                                        }

                                        // Save outfit
                                        if (SaveOutfit(newOutfitName, outfit)) {
                                            g_Data.outfits[newOutfitName] = outfit;
                                            selectedOutfit = newOutfitName;
                                            selectedOutfitItems.clear();
                                        }

                                        showCreateDialog = false;
                                        ImGui::CloseCurrentPopup();
                                    }

                                    ImGui::EndPopup();
                                }

                                // Rename Outfit Dialog
                                if (showRenameDialog) {
                                    ImGui::OpenPopup(LZ("Rename Outfit"));
                                }

                                ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),
                                                       ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

                                if (ImGui::BeginPopupModal(LZ("Rename Outfit"), &showRenameDialog,
                                    ImGuiWindowFlags_AlwaysAutoResize)) {

                                    ImGui::Text(LZ("Enter new name:"));
                                    ImGui::InputText("##NewOutfitName", newOutfitName, sizeof(newOutfitName) - 1);

                                    // Validation feedback
                                    bool nameValid = IsValidOutfitName(newOutfitName);
                                    bool nameExists = g_Data.outfits.contains(newOutfitName) &&
                                                     (newOutfitName != renameTarget);

                                    if (newOutfitName[0] != '\0' && !nameValid) {
                                        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 0, 0, 255));
                                        ImGui::Text(LZ("Invalid name. Use only alphanumeric, _, -, and space"));
                                        ImGui::PopStyleColor();
                                    }

                                    if (nameExists) {
                                        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 255, 0, 255));
                                        ImGui::Text(LZ("Name already exists"));
                                        ImGui::PopStyleColor();
                                    }

                                    ImGui::Separator();

                                    ImGui::BeginDisabled(!nameValid || nameExists || newOutfitName[0] == '\0');
                                    if (ImGui::Button(LZ("Rename"), ImVec2(120, 0))) {
                                        if (RenameOutfit(renameTarget, newOutfitName)) {
                                            if (selectedOutfit == renameTarget) {
                                                selectedOutfit = newOutfitName;
                                            }
                                        }

                                        showRenameDialog = false;
                                        ImGui::CloseCurrentPopup();
                                    }
                                    ImGui::EndDisabled();

                                    ImGui::SetItemDefaultFocus();
                                    ImGui::SameLine();

                                    if (ImGui::Button(LZ("Cancel"), ImVec2(120, 0))) {
                                        showRenameDialog = false;
                                        ImGui::CloseCurrentPopup();
                                    }

                                    // Handle Enter key to confirm rename
                                    if (ImGui::IsKeyPressed(ImGuiKey_Enter) && nameValid && !nameExists && newOutfitName[0] != '\0') {
                                        if (RenameOutfit(renameTarget, newOutfitName)) {
                                            if (selectedOutfit == renameTarget) {
                                                selectedOutfit = newOutfitName;
                                            }
                                        }

                                        showRenameDialog = false;
                                        ImGui::CloseCurrentPopup();
                                    }

                                    ImGui::EndPopup();
                                }

                                // Create Tag Dialog
                                if (showCreateTagDialog) {
                                    ImGui::OpenPopup(LZ("Create Tag"));
                                }

                                ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),
                                                       ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

                                if (ImGui::BeginPopupModal(LZ("Create Tag"), &showCreateTagDialog,
                                    ImGuiWindowFlags_AlwaysAutoResize)) {

                                    ImGui::Text(LZ("Enter tag name:"));

                                    // Set focus to input text when modal first appears
                                    if (ImGui::IsWindowAppearing()) {
                                        ImGui::SetKeyboardFocusHere();
                                    }

                                    ImGui::InputText("##TagName", newTagName, sizeof(newTagName) - 1, ImGuiInputTextFlags_AutoSelectAll);

                                    // Normalize and validate the tag name
                                    std::string normalizedTag = NormalizeTagName(newTagName);
                                    bool tagValid = IsValidTagName(normalizedTag);
                                    bool tagExists = globalTags.contains(normalizedTag);

                                    if (newTagName[0] != '\0' && !tagValid) {
                                        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 0, 0, 255));
                                        ImGui::Text(LZ("Invalid tag. Use only alphanumeric characters (no spaces)"));
                                        ImGui::PopStyleColor();
                                    }

                                    if (tagExists) {
                                        ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 255, 0, 255));
                                        ImGui::Text(LZ("Tag already exists"));
                                        ImGui::PopStyleColor();
                                    }

                                    ImGui::Separator();

                                    ImGui::BeginDisabled(!tagValid || tagExists || newTagName[0] == '\0');
                                    if (ImGui::Button(LZ("Create"), ImVec2(120, 0))) {
                                        // Add tag to selected outfit
                                        if (!selectedOutfit.empty() && g_Data.outfits.contains(selectedOutfit)) {
                                            auto& outfit = g_Data.outfits[selectedOutfit];
                                            outfit.tags.insert(normalizedTag);

                                            // Save outfit with new tag
                                            SaveOutfit(selectedOutfit, outfit);

                                            // Rebuild global tags
                                            globalTags = RebuildGlobalTags();
                                        }

                                        showCreateTagDialog = false;
                                        ImGui::CloseCurrentPopup();
                                    }
                                    ImGui::EndDisabled();

                                    ImGui::SetItemDefaultFocus();
                                    ImGui::SameLine();

                                    if (ImGui::Button(LZ("Cancel"), ImVec2(120, 0))) {
                                        showCreateTagDialog = false;
                                        ImGui::CloseCurrentPopup();
                                    }

                                    // Handle Enter or Ctrl+C to confirm creation
                                    bool isCtrlC = ImGui::IsKeyPressed(ImGuiKey_C) && (ImGui::IsKeyDown(ImGuiKey_LeftCtrl) || ImGui::IsKeyDown(ImGuiKey_RightCtrl));
                                    if ((ImGui::IsKeyPressed(ImGuiKey_Enter) || isCtrlC) &&
                                        tagValid && !tagExists && newTagName[0] != '\0') {

                                        // Mark that we processed Ctrl+C in the modal to prevent tab handler from also triggering
                                        if (isCtrlC) {
                                            ctrlCProcessedInModal = true;
                                        }

                                        // Add tag to selected outfit
                                        if (!selectedOutfit.empty() && g_Data.outfits.contains(selectedOutfit)) {
                                            auto& outfit = g_Data.outfits[selectedOutfit];
                                            outfit.tags.insert(normalizedTag);

                                            // Save outfit with new tag
                                            SaveOutfit(selectedOutfit, outfit);

                                            // Rebuild global tags
                                            globalTags = RebuildGlobalTags();
                                        }

                                        showCreateTagDialog = false;
                                        ImGui::CloseCurrentPopup();
                                    }

                                    ImGui::EndPopup();
                                }

                                // Enchant Dialog
                                if (showEnchantDialog) {
                                    ImGui::OpenPopup(LZ("Enchant Item"));
                                }

                                // Apply stored or default position/size
                                if (enchantModalSettings.initialized) {
                                    ImGui::SetNextWindowPos(enchantModalSettings.pos, ImGuiCond_Appearing);
                                    ImGui::SetNextWindowSize(enchantModalSettings.size, ImGuiCond_Appearing);
                                } else {
                                    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),
                                                           ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
                                    ImGui::SetNextWindowSize(ImVec2(450, 400), ImGuiCond_Appearing);
                                }
                                SetWindowSizeLimits(350, 300);

                                if (ImGui::BeginPopupModal(LZ("Enchant Item"), &showEnchantDialog,
                                    ImGuiWindowFlags_None)) {

                                    // Target item display
                                    if (enchantTarget) {
                                        ImGui::Text(LZ("Target: %s"), enchantTarget->GetName());
                                        ImGui::Separator();
                                    }

                                    // Search filter
                                    ImGui::Text(LZ("Search Enchantments:"));
                                    ImGui::SetNextItemWidth(-1);
                                    if (ImGui::IsWindowAppearing()) {
                                        ImGui::SetKeyboardFocusHere();
                                    }
                                    ImGui::InputTextWithHint("##EnchantSearch", LZ("type to filter..."),
                                                            enchantSearchFilter, sizeof(enchantSearchFilter) - 1);

                                    // Enchantment list (dynamic height, leave room for controls below)
                                    ImGui::Text(LZ("Select Enchantment:"));
                                    float listHeight = ImGui::GetContentRegionAvail().y - 120;  // Reserve space for slider, info, buttons
                                    ImGui::BeginChild("EnchantmentList", ImVec2(-1, std::max(100.0f, listHeight)), true);

                                    for (auto* enchant : g_Data.armorEnchantments) {
                                        if (!enchant) continue;

                                        const char* enchantName = enchant->GetName();
                                        if (!enchantName || enchantName[0] == '\0') continue;

                                        // Apply search filter
                                        if (enchantSearchFilter[0] != '\0' &&
                                            !StringContainsI(enchantName, enchantSearchFilter)) {
                                            continue;
                                        }

                                        bool isSelected = (selectedEnchantment == enchant);
                                        if (ImGui::Selectable(enchantName, isSelected)) {
                                            selectedEnchantment = enchant;
                                        }
                                    }

                                    ImGui::EndChild();

                                    // Magnitude slider
                                    ImGui::Text(LZ("Magnitude Multiplier:"));
                                    ImGui::SetNextItemWidth(-1);
                                    ImGui::SliderFloat("##Magnitude", &enchantMagnitude, 0.1f, 5.0f, "%.2fx");

                                    // Show selected enchantment info
                                    if (selectedEnchantment) {
                                        ImGui::Separator();
                                        ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f),
                                            LZ("Selected: %s"), selectedEnchantment->GetName());
                                    }

                                    ImGui::Separator();

                                    // Buttons
                                    ImGui::BeginDisabled(!selectedEnchantment);
                                    if (ImGui::Button(LZ("Apply"), ImVec2(100, 0))) {
                                        if (enchantTarget && selectedEnchantment &&
                                            !enchantTargetOutfit.empty() &&
                                            g_Data.outfits.contains(enchantTargetOutfit)) {

                                            std::string formID = QARFormID(enchantTarget);
                                            auto& outfit = g_Data.outfits[enchantTargetOutfit];
                                            auto& config = outfit.enhancedItems[formID];
                                            config.enchantmentFormID = QARFormID(selectedEnchantment);
                                            config.enchantmentMagnitude = enchantMagnitude;
                                            SaveOutfit(enchantTargetOutfit, outfit);
                                        }

                                        showEnchantDialog = false;
                                        ImGui::CloseCurrentPopup();
                                    }
                                    ImGui::EndDisabled();

                                    ImGui::SameLine();

                                    // Clear button - only enabled if item has existing enchantment
                                    bool hasExistingEnchant = false;
                                    if (enchantTarget && !enchantTargetOutfit.empty() &&
                                        g_Data.outfits.contains(enchantTargetOutfit)) {
                                        std::string formID = QARFormID(enchantTarget);
                                        auto& outfit = g_Data.outfits[enchantTargetOutfit];
                                        hasExistingEnchant = outfit.enhancedItems.contains(formID) &&
                                                            outfit.enhancedItems[formID].HasEnchantment();
                                    }

                                    ImGui::BeginDisabled(!hasExistingEnchant);
                                    if (ImGui::Button(LZ("Clear"), ImVec2(100, 0))) {
                                        if (enchantTarget && !enchantTargetOutfit.empty() &&
                                            g_Data.outfits.contains(enchantTargetOutfit)) {

                                            std::string formID = QARFormID(enchantTarget);
                                            auto& outfit = g_Data.outfits[enchantTargetOutfit];
                                            if (outfit.enhancedItems.contains(formID)) {
                                                outfit.enhancedItems[formID].enchantmentFormID.clear();
                                                outfit.enhancedItems[formID].enchantmentMagnitude = 1.0f;

                                                // If no enhancements left, remove the entry entirely
                                                if (!outfit.enhancedItems[formID].IsEnhanced()) {
                                                    outfit.enhancedItems.erase(formID);
                                                }
                                                SaveOutfit(enchantTargetOutfit, outfit);
                                            }
                                        }

                                        showEnchantDialog = false;
                                        ImGui::CloseCurrentPopup();
                                    }
                                    ImGui::EndDisabled();

                                    ImGui::SameLine();

                                    if (ImGui::Button(LZ("Cancel"), ImVec2(100, 0))) {
                                        showEnchantDialog = false;
                                        ImGui::CloseCurrentPopup();
                                    }

                                    // Save window position and size
                                    enchantModalSettings.pos = ImGui::GetWindowPos();
                                    enchantModalSettings.size = ImGui::GetWindowSize();
                                    enchantModalSettings.initialized = true;

                                    ImGui::EndPopup();
                                }

                                // Stats Transfer Dialog
                                if (showStatsTransferDialog) {
                                    ImGui::OpenPopup(LZ("Transfer Stats"));

                                    // Build inventory armor cache when dialog opens
                                    // Use GetItemCount instead of GetInventory to avoid plugin hook crashes
                                    int currentFrame = ImGui::GetFrameCount();
                                    if (inventoryCacheFrame != currentFrame) {
                                        inventoryArmorCache.clear();
                                        inventoryCacheFrame = currentFrame;

                                        if (auto player = NPCTargets::GetActor()) {
                                            inventoryArmorCache = NPCTargets::InventoryArmor(player);

                                            // Sort by name
                                            std::sort(inventoryArmorCache.begin(), inventoryArmorCache.end(),
                                                [](RE::TESObjectARMO* a, RE::TESObjectARMO* b) {
                                                    return _stricmp(a->GetName(), b->GetName()) < 0;
                                                });
                                        }
                                    }
                                }

                                // Apply stored or default position/size
                                if (statsTransferModalSettings.initialized) {
                                    ImGui::SetNextWindowPos(statsTransferModalSettings.pos, ImGuiCond_Appearing);
                                    ImGui::SetNextWindowSize(statsTransferModalSettings.size, ImGuiCond_Appearing);
                                } else {
                                    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(),
                                                           ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
                                    ImGui::SetNextWindowSize(ImVec2(500, 450), ImGuiCond_Appearing);
                                }
                                SetWindowSizeLimits(400, 350);

                                if (ImGui::BeginPopupModal(LZ("Transfer Stats"), &showStatsTransferDialog,
                                    ImGuiWindowFlags_None)) {

                                    // Target item display with current stats
                                    if (statsTransferTarget) {
                                        auto targetArmor = statsTransferTarget->As<RE::TESObjectARMO>();
                                        ImGui::Text(LZ("Target: %s"), statsTransferTarget->GetName());
                                        if (targetArmor) {
                                            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f),
                                                LZ("Current - Armor: %d, Weight: %.1f, Value: %d"),
                                                static_cast<int>(targetArmor->GetArmorRating()),
                                                targetArmor->GetWeight(),
                                                targetArmor->GetGoldValue());
                                        }
                                        ImGui::Separator();
                                    }

                                    // Search filter
                                    ImGui::Text(LZ("Search Inventory:"));
                                    ImGui::SetNextItemWidth(-1);
                                    if (ImGui::IsWindowAppearing()) {
                                        ImGui::SetKeyboardFocusHere();
                                    }
                                    ImGui::InputTextWithHint("##StatsSearch", LZ("type to filter..."),
                                                            statsSearchFilter, sizeof(statsSearchFilter) - 1);

                                    // Inventory armor list (dynamic height, leave room for preview and buttons)
                                    ImGui::Text(LZ("Select Source Item:"));
                                    float inventoryListHeight = ImGui::GetContentRegionAvail().y - 140;  // Reserve space for preview, checkbox, buttons
                                    ImGui::BeginChild("InventoryArmorList", ImVec2(-1, std::max(100.0f, inventoryListHeight)), true);

                                    for (auto* armor : inventoryArmorCache) {
                                        if (!armor) continue;

                                        const char* armorName = armor->GetName();
                                        if (!armorName || armorName[0] == '\0') continue;

                                        // Apply search filter
                                        if (statsSearchFilter[0] != '\0' &&
                                            !StringContainsI(armorName, statsSearchFilter)) {
                                            continue;
                                        }

                                        // Format: Name (Armor: X, Weight: Y, Value: Z) [Enchant]
                                        std::string displayText = std::format("{} (AR:{}, W:{:.1f}, V:{})",
                                            armorName,
                                            static_cast<int>(armor->GetArmorRating()),
                                            armor->GetWeight(),
                                            armor->GetGoldValue());

                                        // Add enchantment indicator if present
                                        if (armor->formEnchanting) {
                                            displayText += std::format(" [{}]", armor->formEnchanting->GetName());
                                        }

                                        bool isSelected = (selectedStatsSource == armor);
                                        if (ImGui::Selectable(displayText.c_str(), isSelected)) {
                                            selectedStatsSource = armor;
                                        }
                                    }

                                    ImGui::EndChild();

                                    // Show selected source preview
                                    if (selectedStatsSource) {
                                        ImGui::Separator();
                                        ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f),
                                            LZ("Selected: %s"), selectedStatsSource->GetName());
                                        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
                                            LZ("Will Transfer - Armor: %d, Weight: %.1f, Value: %d"),
                                            static_cast<int>(selectedStatsSource->GetArmorRating()),
                                            selectedStatsSource->GetWeight(),
                                            selectedStatsSource->GetGoldValue());

                                        // Show enchantment info and transfer option
                                        if (selectedStatsSource->formEnchanting) {
                                            ImGui::TextColored(ImVec4(0.8f, 0.6f, 1.0f, 1.0f),
                                                LZ("Enchantment: %s"), selectedStatsSource->formEnchanting->GetName());
                                            ImGui::Checkbox(LZ("Also transfer enchantment"), &transferEnchantment);
                                        } else {
                                            ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
                                                LZ("(No enchantment on source item)"));
                                        }
                                    }

                                    ImGui::Separator();

                                    // Buttons
                                    ImGui::BeginDisabled(!selectedStatsSource);
                                    if (ImGui::Button(LZ("Apply"), ImVec2(100, 0))) {
                                        if (statsTransferTarget && selectedStatsSource &&
                                            !statsTransferTargetOutfit.empty() &&
                                            g_Data.outfits.contains(statsTransferTargetOutfit)) {

                                            std::string formID = QARFormID(statsTransferTarget);
                                            auto& outfit = g_Data.outfits[statsTransferTargetOutfit];
                                            auto& config = outfit.enhancedItems[formID];

                                            // Transfer stats
                                            config.statsSourceFormID = QARFormID(selectedStatsSource);
                                            config.armorRating = static_cast<uint32_t>(selectedStatsSource->GetArmorRating());
                                            config.weight = selectedStatsSource->GetWeight();
                                            config.value = selectedStatsSource->GetGoldValue();

                                            // Also transfer enchantment if option is enabled and source has one
                                            if (transferEnchantment && selectedStatsSource->formEnchanting) {
                                                config.enchantmentFormID = QARFormID(selectedStatsSource->formEnchanting);
                                                config.enchantmentMagnitude = 1.0f;
                                                logger::info("Stats Transfer: Also transferring enchantment {} from {}",
                                                    selectedStatsSource->formEnchanting->GetName(),
                                                    selectedStatsSource->GetName());
                                            }

                                            SaveOutfit(statsTransferTargetOutfit, outfit);
                                        }

                                        showStatsTransferDialog = false;
                                        ImGui::CloseCurrentPopup();
                                    }
                                    ImGui::EndDisabled();

                                    ImGui::SameLine();

                                    // Clear button - only enabled if item has existing stats transfer
                                    bool hasExistingStats = false;
                                    if (statsTransferTarget && !statsTransferTargetOutfit.empty() &&
                                        g_Data.outfits.contains(statsTransferTargetOutfit)) {
                                        std::string formID = QARFormID(statsTransferTarget);
                                        auto& outfit = g_Data.outfits[statsTransferTargetOutfit];
                                        hasExistingStats = outfit.enhancedItems.contains(formID) &&
                                                          outfit.enhancedItems[formID].HasStatsTransfer();
                                    }

                                    ImGui::BeginDisabled(!hasExistingStats);
                                    if (ImGui::Button(LZ("Clear"), ImVec2(100, 0))) {
                                        if (statsTransferTarget && !statsTransferTargetOutfit.empty() &&
                                            g_Data.outfits.contains(statsTransferTargetOutfit)) {

                                            std::string formID = QARFormID(statsTransferTarget);
                                            auto& outfit = g_Data.outfits[statsTransferTargetOutfit];
                                            if (outfit.enhancedItems.contains(formID)) {
                                                outfit.enhancedItems[formID].statsSourceFormID.clear();
                                                outfit.enhancedItems[formID].armorRating.reset();
                                                outfit.enhancedItems[formID].weight.reset();
                                                outfit.enhancedItems[formID].value.reset();

                                                // If no enhancements left, remove the entry entirely
                                                if (!outfit.enhancedItems[formID].IsEnhanced()) {
                                                    outfit.enhancedItems.erase(formID);
                                                }
                                                SaveOutfit(statsTransferTargetOutfit, outfit);
                                            }
                                        }

                                        showStatsTransferDialog = false;
                                        ImGui::CloseCurrentPopup();
                                    }
                                    ImGui::EndDisabled();

                                    ImGui::SameLine();

                                    if (ImGui::Button(LZ("Cancel"), ImVec2(100, 0))) {
                                        showStatsTransferDialog = false;
                                        ImGui::CloseCurrentPopup();
                                    }

                                    // Save window position and size
                                    statsTransferModalSettings.pos = ImGui::GetWindowPos();
                                    statsTransferModalSettings.size = ImGui::GetWindowSize();
                                    statsTransferModalSettings.initialized = true;

                                    ImGui::EndPopup();
                                }

                                // Numpad +/- to cycle through outfits
                                if (ImGui::IsKeyPressed(ImGuiKey_KeypadAdd) || ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract)) {
                                    static double lastNumpadPressTime = 0.0;
                                    double currentTime = ImGui::GetTime();

                                    if (currentTime - lastNumpadPressTime >= (g_Config.outfitCycleTimeout / 1000.0)) {  // Use configurable timeout
                                        // Build list of visible outfits (respecting filter)
                                        std::vector<std::string> visibleOutfits;
                                        for (const auto& [name, outfit] : g_Data.outfits) {
                                            // Apply filter
                                            if (outfitNameFilter[0] != '\0' &&
                                                !StringContainsI(name.c_str(), outfitNameFilter)) {
                                                continue;
                                            }
                                            visibleOutfits.push_back(name);
                                        }

                                        if (!visibleOutfits.empty()) {
                                            int currentIndex = -1;

                                            // Find current selection index
                                            if (!selectedOutfit.empty()) {
                                                for (int i = 0; i < visibleOutfits.size(); i++) {
                                                    if (visibleOutfits[i] == selectedOutfit) {
                                                        currentIndex = i;
                                                        break;
                                                    }
                                                }
                                            }

                                            // If no valid selection, start from -1 (so next becomes 0, prev becomes last)
                                            int visibleCount = static_cast<int>(visibleOutfits.size());
                                            int newIndex;
                                            if (ImGui::IsKeyPressed(ImGuiKey_KeypadAdd)) {
                                                // Next outfit
                                                newIndex = (currentIndex + 1) % visibleCount;
                                            } else {
                                                // Previous outfit
                                                newIndex = (currentIndex - 1 + visibleCount) % visibleCount;
                                            }

                                            // Select and equip the outfit
                                            std::string selectedOutfitName = visibleOutfits[newIndex];
                                            selectedOutfit = selectedOutfitName;
                                            selectedOutfitItems.clear();

                                            if (g_Data.outfits.contains(selectedOutfitName)) {
                                                auto& outfit = g_Data.outfits[selectedOutfitName];

                                                // Unequip current items first
                                                givenItems.UnequipCurrent();

                                                // Skip frames to allow unequip to complete (with multiplier)
                                                int unequipFrames = static_cast<int>(3 * g_Config.outfitFrameMultiplier);
                                                for (int i = 0; i < unequipFrames; i++) {
                                                    g_Pause.SkipFrame();
                                                }

                                                // Equip all items from the outfit
                                                for (auto item : outfit.items) {
                                                    std::string formID = QARFormID(item);
                                                    if (outfit.enhancedItems.contains(formID) &&
                                                        outfit.enhancedItems[formID].IsEnhanced()) {
                                                        givenItems.GiveEnhanced(item, outfit.enhancedItems[formID], true);
                                                    } else {
                                                        givenItems.Give(item, true);
                                                    }
                                                }

                                                // Skip frames to allow equips to complete (with multiplier)
                                                int equipFrames = static_cast<int>(3 * g_Config.outfitFrameMultiplier);
                                                for (int i = 0; i < equipFrames; i++) {
                                                    g_Pause.SkipFrame();
                                                }

                                                // Save as last equipped outfit for Numpad1 restore
                                                g_LastEquippedOutfit = selectedOutfitName;
                                            }
                                        }

                                        lastNumpadPressTime = currentTime;
                                    }
                                }

                                // Ctrl+X to focus the outfit prefix textbox (when no modal is open)
                                if (ImGui::IsKeyPressed(ImGuiKey_X) &&
                                    (ImGui::IsKeyDown(ImGuiKey_LeftCtrl) || ImGui::IsKeyDown(ImGuiKey_RightCtrl)) &&
                                    !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)) {
                                    focusPrefixTextbox = true;
                                }

                                // Ctrl+C to open Create Outfit dialog (when no modal is open and Ctrl+C wasn't already processed in a modal)
                                if (ImGui::IsKeyPressed(ImGuiKey_C) &&
                                    (ImGui::IsKeyDown(ImGuiKey_LeftCtrl) || ImGui::IsKeyDown(ImGuiKey_RightCtrl)) &&
                                    !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId) &&
                                    !ctrlCProcessedInModal) {
                                    showCreateDialog = true;

                                    // Shift names it after the selected mod instead of the prefix box
                                    std::string prefix;
                                    if (isShiftDown && curMod.size() == 1) prefix = OutfitPrefixFromMod((*curMod.begin())->mod);
                                    if (prefix.empty()) prefix = outfitPrefix;

                                    auto suggestedName = SuggestOutfitName(prefix);
                                    strncpy(newOutfitName, suggestedName.c_str(), sizeof(newOutfitName) - 1);
                                    newOutfitName[sizeof(newOutfitName) - 1] = '\0';
                                }

                                ImGui::EndTabItem();
                            }
                            iTab++;
                            ImGui::EndDisabled();

                            ImGui::EndTabBar();
                            iTabOpen = iTabSelected;
                        }
                        ImGui::EndChild(); // End TabsSection
                    }

                    // Resizable separator
                        ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyle().Colors[ImGuiCol_Separator]);
                        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImGui::GetStyle().Colors[ImGuiCol_SeparatorHovered]);
                        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImGui::GetStyle().Colors[ImGuiCol_SeparatorActive]);
                        ImGui::Button("##vsplitter", ImVec2(-1, 4.0f));
                        ImGui::PopStyleColor(3);

                        if (ImGui::IsItemActive()) {
                            tabsHeight += ImGui::GetIO().MouseDelta.y;
                        }
                        if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
                            ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeNS);
                        }

                        // Bottom section: Sliders, Keywords, Recipes
                        if (ImGui::BeginChild("BottomSection", ImVec2(0, 0), false)) {
                            if (SliderTable()) {
                                SliderRow(LZ("Gold Value"), params.value, g_Config.flatValueMod);

                                ImGui::EndTable();
                            }

                            ImGui::Checkbox(LZ("Modify Keywords"), &params.bModifyKeywords);
                    MakeTooltip(LZ("Changes basic keywords to match those on the base item."));

                    ImGui::SameLine();
                    if (ImGui::Button(RightAlign(LZ("Custom Keywords")))) {
                        popupCustomKeywords = true;
                    }
                    MakeTooltip(
                        LZ("Allows you to add or remove any keywords.\n"
                           "Note: Adding and removing basic Skyrim keywords is already included \n"
                           "as part of armor conversion."));

                    static RecipeConditionals recipeConds;

                    struct Recipes {
                        using TypePtr = std::vector<RE::TESForm*> RecipeConditionals::Conditionals::*;

                        static void ReqChecklist(const char* id, ArmorChangeParams::RecipeOptions& opts, const RecipeConditionals::Conditionals& conds, TypePtr type,
                                                 bool bDisabled) {
                            const auto& ls = conds.*type;
                            if (ls.empty()) return;

                            ImGui::BeginDisabled(bDisabled);
                            ImGui::Indent();
                            ImGui::PushID(id);

                            for (auto i : ls) {
                                bool bChecked = !opts.skipForms.contains(i);
                                if (ImGui::Checkbox(i->GetName(), &bChecked)) {
                                    if (bChecked)
                                        opts.skipForms.erase(i);
                                    else
                                        opts.skipForms.insert(i);
                                }
                            }

                            ImGui::PopID();
                            ImGui::Unindent();
                            ImGui::EndDisabled();
                        }

                        static void DrawCombo(const char* name, ArmorChangeParams& params, ArmorChangeParams::RecipeOptions& opts, int& mode,
                                              const RecipeConditionals::PurposeConditionals& conds, TypePtr type) {
                            const char* label[] = {"Replace", "Keep", "Remove"};
                            ImGui::PushID(name);
                            if (ImGui::BeginCombo(LZ(name), LZ(label[mode]))) {
                                recipeConds.BuildLists(params);

                                ImGui::RadioButton(LZ(label[ArmorChangeParams::eRequirementReplace]), &mode, ArmorChangeParams::eRequirementReplace);
                                ReqChecklist("replace", opts, conds.convert, type, mode != ArmorChangeParams::eRequirementReplace);
                                ImGui::RadioButton(LZ(label[ArmorChangeParams::eRequirementKeep]), &mode, ArmorChangeParams::eRequirementKeep);
                                ReqChecklist("keep", opts, conds.orig, type, mode != ArmorChangeParams::eRequirementKeep);
                                ImGui::RadioButton(LZ(label[ArmorChangeParams::eRequirementRemove]), &mode, ArmorChangeParams::eRequirementRemove);

                                ImGui::EndCombo();
                            }

                            ImGui::PopID();
                        }

                        static void Draw(const char* id, ArmorChangeParams& params, ArmorChangeParams::RecipeOptions& options,
                                         const RecipeConditionals::PurposeConditionals& conds) {
                            ImGui::PushID(id);
                            ImGui::TableNextColumn();

                            if (options.bExpanded) {
                                if (ImGui::ArrowButton("##Expand", ImGuiDir_Up)) options.bExpanded = false;

                                ImGui::TableNextRow();
                                ImGui::TableNextColumn();
                                ImGui::Text(LZ("Requirements"));

                                ImGui::TableNextColumn();
                                DrawCombo("Perks", params, options, options.modePerks, conds, &RecipeConditionals::Conditionals::perks);

                                ImGui::TableNextColumn();
                                DrawCombo("Items", params, options, options.modeItems, conds, &RecipeConditionals::Conditionals::items);

                            } else if (ImGui::ArrowButton("##Expand", ImGuiDir_Down))
                                options.bExpanded = true;

                            ImGui::PopID();
                        }
                    };

                    if (ImGui::BeginTable("Crafting Table", 4, ImGuiTableFlags_SizingFixedFit)) {
                        ImGui::TableSetupColumn("CraftCol1");
                        ImGui::TableSetupColumn("CraftCol2");
                        ImGui::TableSetupColumn("CraftCol3");
                        ImGui::TableSetupColumn("CraftCol4");

                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::Checkbox("##Modify Temper Recipe", &params.temper.bModify);
                        ImGui::BeginDisabled(!params.temper.bModify);

                        const char* descTemperAction[] = {"Modify Temper Recipe", "Remove Temper Recipe"};

                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(150.0f);
                        if (ImGui::BeginCombo("##TemperAction", LZ(descTemperAction[params.temper.action]))) {
                            for (int i = 0; i < ArmorChangeParams::RecipeActionCount; i++) {
                                bool selected = params.temper.action == i;
                                if (ImGui::Selectable(LZ(descTemperAction[i]), selected)) params.temper.action = i;
                                if (selected) ImGui::SetItemDefaultFocus();
                            }
                            ImGui::EndCombo();
                        }

                        if (params.temper.action == ArmorChangeParams::eRecipeModify) {
                            ImGui::TableNextColumn();
                            ImGui::Checkbox(LZ("Create recipe if missing##Temper"), &params.temper.bNew);
                            MakeTooltip(
                                LZ("If an item being modified lacks an existing temper recipe to modify, checking\n"
                                   "this will create one automatically"));
                            ImGui::TableNextColumn();
                            ImGui::Checkbox(g_Config.fTemperGoldCostRatio > 0 ? LZ("Cost gold if no recipe to copy##Temper") : LZ("Make free if no recipe to copy##Temper"),
                                            &params.temper.bFree);
                            MakeTooltip(
                                LZ("If the item being copied lacks a tempering recipe, checking will remove all\n"
                                   "components and conditions to temper the modified item"));

                            Recipes::Draw("tempering", params, params.temper, recipeConds.temper);
                        }
                        ImGui::EndDisabled();

                        ImGui::TableNextRow();
                        ImGui::TableNextColumn();
                        ImGui::Checkbox("##Modify Crafting Recipe", &params.craft.bModify);
                        ImGui::BeginDisabled(!params.craft.bModify);

                        const char* descCraftingAction[] = {"Modify Crafting Recipe", "Remove Crafting Recipe"};

                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(150.0f);
                        if (ImGui::BeginCombo("##CraftingAction", LZ(descCraftingAction[params.craft.action]))) {
                            for (int i = 0; i < ArmorChangeParams::RecipeActionCount; i++) {
                                bool selected = params.craft.action == i;
                                if (ImGui::Selectable(LZ(descCraftingAction[i]), selected)) params.craft.action = i;
                                if (selected) ImGui::SetItemDefaultFocus();
                            }
                            ImGui::EndCombo();
                        }

                        if (params.craft.action == ArmorChangeParams::eRecipeModify) {
                            ImGui::TableNextColumn();
                            ImGui::Checkbox(LZ("Create recipe if missing##Craft"), &params.craft.bNew);
                            if (ImGui::IsItemHovered())
                                ImGui::SetTooltip(
                                    LZ("If an item being modified lacks an existing crafting recipe to modify,\n"
                                       "checking this will create one automatically"));
                            ImGui::TableNextColumn();
                            ImGui::Checkbox(g_Config.fCraftGoldCostRatio > 0 ? LZ("Cost gold if no recipe to copy##Craft") : LZ("Make free if no recipe to copy##Craft"),
                                            &params.craft.bFree);
                            MakeTooltip(
                                LZ("If the item being copied lacks a crafting recipe, checking will remove all\n"
                                   "components and conditions to craft the modified item"));

                            Recipes::Draw("cafting", params, params.craft, recipeConds.craft);
                        }

                        ImGui::EndDisabled();

                        ImGui::EndTable();

                    }

                    if (!recipeConds.bRefreshed) recipeConds.bListsBuilt = false;
                    recipeConds.bRefreshed = false;
                }
                ImGui::EndChild(); // End BottomSection
                }
                ImGui::EndChild(); // End ItemTypeFrame
            }
            ImGui::EndChild(); // End LeftPane

            ImGui::TableNextColumn();
                if (data.filteredItems.size() >= g_Config.itemListLimit) {
                    ImGui::Text(
                        LZ("Too many items.\n"
                           "Limit the amount of items by using the filters."));
                } else {
                    ImGui::Text(LZFormat("{} Items", data.filteredItems.size()).c_str());

                    auto avail = ImGui::GetContentRegionAvail();
                    avail.y -= ImGui::GetFontSize() * 1 + ImGui::GetStyle().FramePadding.y * 2;

                    auto player = NPCTargets::GetActor();

                    data.items.clear();
                    data.items.reserve(data.filteredItems.size());

                    // bool modChangesDeleted = curMod ? g_Data.modifiedFilesDeleted.contains(curMod->mod) : false;

                    ImGui::PushStyleColor(ImGuiCol_NavHighlight, IM_COL32(0, 255, 0, 255));

                    // Pre-compute equipped slots cache with incremental refresh (major optimization)
                    // GetWornArmor is O(inventory_size), so we spread the work across frames
                    static EquippedSlotsCache equippedCache;
                    bool forceRefresh = g_EquipmentChanged;
                    if (g_EquipmentChanged) {
                        g_EquipmentChanged = false;
                        equippedCache.ForceFullRefresh();
                    }
                    equippedCache.Refresh(player, forceRefresh);

                    // Rebuild cache only when items change (major optimization)
                    if (g_MainItemsCache.NeedsRebuild(data.filteredItems, g_filterRound, equippedCache.occupiedSlots)) {
                        g_MainItemsCache.Rebuild(data.filteredItems, params, &equippedCache, g_filterRound);
                    }

                    // Sort only when sort parameters change (cached)
                    g_MainItemsCache.Sort(mainItemsSortColumn, mainItemsSortAscending);

                    // Colors for columns
                    ImVec4 mainSlotColor = ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled);
                    ImVec4 slotConflictColor = ImVec4(0.8f, 0.3f, 0.3f, 1.0f);  // Muted red for slot conflicts

                    // Get cached column configuration (no allocation)
                    int visibleColCount = g_Config.tableColumns.GetVisibleCount();
                    if (visibleColCount < 1) visibleColCount = 1;
                    const int* displayOrder = g_Config.tableColumns.GetDisplayOrder();

                    // Add 1 for the Tags column (always visible at end)
                    if (ImGui::BeginTable("##ItemsTable", visibleColCount + 1,
                        ImGuiTableFlags_Sortable | ImGuiTableFlags_ScrollY | ImGuiTableFlags_RowBg |
                        ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_Resizable,
                        avail)) {

                        // Setup columns based on visibility and order
                        for (int i = 0; i < Config::TableColumnConfig::Col_Count; i++) {
                            int col = displayOrder[i];
                            if (!g_Config.tableColumns.visible[col]) continue;

                            switch (col) {
                                case Config::TableColumnConfig::Col_Name:
                                    ImGui::TableSetupColumn(LZ("Name"), ImGuiTableColumnFlags_DefaultSort | ImGuiTableColumnFlags_WidthStretch, 0.0f, 0);
                                    break;
                                case Config::TableColumnConfig::Col_Slot:
                                    ImGui::TableSetupColumn(LZ("Slot"), ImGuiTableColumnFlags_WidthFixed, 80.0f, 1);
                                    break;
                                case Config::TableColumnConfig::Col_Armor:
                                    ImGui::TableSetupColumn(LZ("Armor"), ImGuiTableColumnFlags_WidthFixed, 50.0f, 2);
                                    break;
                                case Config::TableColumnConfig::Col_ID:
                                    ImGui::TableSetupColumn(LZ("ID"), ImGuiTableColumnFlags_WidthFixed, 60.0f, 3);
                                    break;
                                case Config::TableColumnConfig::Col_NifF:
                                    ImGui::TableSetupColumn(LZ("Nif (F)"), ImGuiTableColumnFlags_WidthFixed, 200.0f, 4);
                                    break;
                                case Config::TableColumnConfig::Col_Mod:
                                    ImGui::TableSetupColumn(LZ("Mod"), ImGuiTableColumnFlags_WidthFixed, 150.0f, 5);
                                    break;
                            }
                        }
                        // Tags column - always at end, not sortable
                        ImGui::TableSetupColumn(LZ("Tags"), ImGuiTableColumnFlags_WidthFixed | ImGuiTableColumnFlags_NoSort, 60.0f, 99);
                        ImGui::TableSetupScrollFreeze(0, 1);
                        ImGui::TableHeadersRow();

                        // Handle sorting
                        if (ImGuiTableSortSpecs* sortSpecs = ImGui::TableGetSortSpecs()) {
                            if (sortSpecs->SpecsDirty && sortSpecs->SpecsCount > 0) {
                                mainItemsSortColumn = sortSpecs->Specs[0].ColumnUserID;
                                mainItemsSortAscending = sortSpecs->Specs[0].SortDirection == ImGuiSortDirection_Ascending;
                                sortSpecs->SpecsDirty = false;
                            }
                        }

                        auto xPos = ImGui::GetCursorPosX();

                        if (ImGui::BeginPopupContextWindow()) {
                            if (ImGui::Selectable(LZ("Unequip worn items"))) {
                                givenItems.UnequipCurrent();
                            }

                            // Remove All Tracked Items context menu entry
                            {
                                int totalTracked = NPCTargets::IsNPC() ? NPCTargets::TrackedCount() : g_ItemTracking.GetTotalGivenCount();
                                int removableCount = NPCTargets::IsNPC() ? NPCTargets::TrackedCount() : g_ItemTracking.GetRemovableCount();
                                std::string label;
                                if (totalTracked > 0) {
                                    label = LZFormat("Remove All Tracked Items ({})", removableCount);
                                } else {
                                    label = LZ("Remove All Tracked Items");
                                }
                                if (ImGui::Selectable(label.c_str())) {
                                    givenItems.RemoveAllTracked();
                                }
                            }

                            ImGui::Separator();

                            ImGui::BeginDisabled(selectedItems.empty());
                            if (ImGui::BeginMenu(LZ("Selected ..."))) {
                                if (ImGui::Selectable(LZ("Equip"))) {
                                    for (auto i : selectedItems) givenItems.Give(i, true, true);
                                }
                                if (ImGui::Selectable(LZ("Unequip"))) {
                                    for (auto i : selectedItems) givenItems.Unequip(i);
                                }

                                ImGui::Separator();

                                // Mark to Keep / Unmark - for item tracking system
                                {
                                    bool anyMarked = false;
                                    bool anyUnmarked = false;
                                    for (auto i : selectedItems) {
                                        if (IsItemMarkedToKeep(i)) anyMarked = true;
                                        else anyUnmarked = true;
                                    }

                                    if (!NPCTargets::IsNPC() && anyUnmarked && ImGui::Selectable(LZ("Mark to Keep"))) {
                                        for (auto i : selectedItems) MarkItemToKeep(i, true);
                                    }
                                    if (!NPCTargets::IsNPC() && anyMarked && ImGui::Selectable(LZ("Unmark to Keep"))) {
                                        for (auto i : selectedItems) MarkItemToKeep(i, false);
                                    }
                                }

                                ImGui::Separator();

                                if (ImGui::Selectable(LZ("Enable"))) {
                                    for (auto i : selectedItems) uncheckedItems.erase(i);
                                }
                                if (ImGui::Selectable(LZ("Enable ONLY"))) {
                                    uncheckedItems.clear();
                                    uncheckedItems.insert(data.filteredItems.begin(), data.filteredItems.end());
                                    for (auto i : selectedItems) uncheckedItems.erase(i);
                                }
                                if (ImGui::Selectable(LZ("Disable"))) {
                                    for (auto i : selectedItems) uncheckedItems.insert(i);
                                }

                                ImGui::Separator();
                                auto selectedFile = !selectedItems.empty() ? (*selectedItems.begin())->GetFile(0) : nullptr;
                                ImGui::BeginDisabled(curMod.size()!=1 || !selectedFile);
                                if (ImGui::Selectable(LZ("Select source mod"))) {
                                    if (selectedFile) {
                                        switchToMod = g_Data.modData[selectedFile].get();
                                    }
                                }
                                ImGui::EndDisabled();

                                if (ImGui::BeginMenu(LZ("Delete changes ..."))) {
                                    if (MenuItemConfirmed(LZ("All"))) {
                                        DeleteChanges(selectedItems);
                                    }
                                    ImGui::Separator();
                                    if (MenuItemConfirmed(LZ("Loot Distribution"))) {
                                        const char* fields[] = {"loot", nullptr};
                                        DeleteChanges(selectedItems, fields);
                                    }
                                    if (MenuItemConfirmed(LZ("Slots"))) {
                                        const char* fields[] = {"slots", nullptr};
                                        DeleteChanges(selectedItems, fields);
                                    }
                                    if (MenuItemConfirmed(LZ("Crafting"))) {
                                        const char* fields[] = {"craft", nullptr};
                                        DeleteChanges(selectedItems, fields);
                                    }
                                    if (MenuItemConfirmed(LZ("Tempering"))) {
                                        const char* fields[] = {"temper", nullptr};
                                        DeleteChanges(selectedItems, fields);
                                    }

                                    ImGui::EndMenu();
                                }

                                ImGui::EndMenu();
                            }
                            ImGui::EndDisabled();

                            if (ImGui::BeginMenu(LZ("Select ..."))) {
                                std::function<bool(RE::TESBoundObject*)> cond;

                                if (ImGui::BeginMenu(LZ("Armor"))) {
                                    if (ImGui::Selectable(LZ("Any"))) {
                                        cond = [](RE::TESBoundObject* obj) { return obj->As<RE::TESObjectARMO>(); };
                                    }
                                    if (ImGui::Selectable(LZ("Clothing"))) {
                                        cond = [](RE::TESBoundObject* obj) {
                                            auto armor = obj->As<RE::TESObjectARMO>();
                                            return armor && armor->bipedModelData.armorType == RE::BIPED_MODEL::ArmorType::kClothing;
                                        };
                                    }
                                    if (ImGui::Selectable(LZ("Light"))) {
                                        cond = [](RE::TESBoundObject* obj) {
                                            auto armor = obj->As<RE::TESObjectARMO>();
                                            return armor && armor->bipedModelData.armorType == RE::BIPED_MODEL::ArmorType::kLightArmor;
                                        };
                                    }
                                    if (ImGui::Selectable(LZ("Heavy"))) {
                                        cond = [](RE::TESBoundObject* obj) {
                                            auto armor = obj->As<RE::TESObjectARMO>();
                                            return armor && armor->bipedModelData.armorType == RE::BIPED_MODEL::ArmorType::kHeavyArmor;
                                        };
                                    }

                                    ImGui::EndMenu();
                                }
                                if (ImGui::Selectable(LZ("Weapons"))) {
                                    cond = [](RE::TESBoundObject* obj) { return obj->As<RE::TESObjectWEAP>(); };
                                }
                                if (ImGui::Selectable(LZ("Ammo"))) {
                                    cond = [](RE::TESBoundObject* obj) { return obj->As<RE::TESAmmo>(); };
                                }

                                ImGui::Separator();
                                if (ImGui::Selectable(LZ("Enchanted"))) {
                                    cond = [](RE::TESBoundObject* obj) { return IsEnchanted(obj); };
                                }
                                if (ImGui::Selectable(LZ("Unenchanted"))) {
                                    cond = [](RE::TESBoundObject* obj) { return !IsEnchanted(obj); };
                                }

                                if (cond) {
                                    if (isShiftDown) {
                                        for (auto i : data.filteredItems) {
                                            if (cond(i)) selectedItems.insert(i);
                                        }
                                    } else if (isCtrlDown || selectedItems.size() > 1) {
                                        std::set<RE::TESBoundObject*> copy = std::move(selectedItems);

                                        for (auto i : copy) {
                                            if (cond(i)) selectedItems.insert(i);
                                        }
                                    } else {
                                        selectedItems.clear();
                                        for (auto i : data.filteredItems) {
                                            if (cond(i)) selectedItems.insert(i);
                                        }
                                    }
                                }

                                ImGui::EndMenu();
                            }

                            ImGui::Separator();
                            if (ImGui::Selectable(LZ("Enable all"))) uncheckedItems.clear();
                            if (ImGui::Selectable(LZ("Disable all")))
                                for (auto i : data.filteredItems) uncheckedItems.insert(i);

                            ImGui::EndPopup();
                        }

                        // Clear out selections that are no longer visible (only when filter changes)
                        size_t cacheSize = g_MainItemsCache.Size();
                        static short lastSelectionFilterRound = -1;
                        if (lastSelectionFilterRound != g_filterRound) {
                            lastSelectionFilterRound = g_filterRound;
                            // Build a set of valid items for quick lookup
                            std::set<RE::TESBoundObject*> validItems;
                            for (size_t idx = 0; idx < cacheSize; idx++) {
                                validItems.insert(g_MainItemsCache.GetSortedItem(idx));
                            }
                            // Remove selections that are no longer valid
                            for (auto it = selectedItems.begin(); it != selectedItems.end(); ) {
                                if (!validItems.contains(*it)) {
                                    it = selectedItems.erase(it);
                                } else {
                                    ++it;
                                }
                            }
                            // Check anchor validity
                            if (lastSelectedItem && !validItems.contains(lastSelectedItem)) {
                                lastSelectedItem = nullptr;
                            }
                        }

                        // Use cached checked items computation (avoids O(N) loop every frame)
                        if (g_CheckedItemsCache.NeedsRebuild(g_filterRound, uncheckedItems.size(), cacheSize)) {
                            g_CheckedItemsCache.Rebuild(g_MainItemsCache, uncheckedItems, needChanges, g_filterRound);
                        }
                        anyItemChanges = g_CheckedItemsCache.anyItemChanges;
                        hasModifiedItems = g_CheckedItemsCache.hasModifiedItems;
                        data.items = g_CheckedItemsCache.checkedItems;  // Fast copy of vector

                        // Use ImGuiListClipper for rendering only visible rows (MAJOR optimization)
                        ImGuiListClipper clipper;
                        clipper.Begin(static_cast<int>(cacheSize));

                        while (clipper.Step()) {
                            for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; row++) {
                                const auto& cachedItem = g_MainItemsCache.GetSorted(row);
                                auto i = cachedItem.item;
                                const auto& itemSlotInfo = cachedItem.slotInfo;

                                int popCol = 0;
                                bool isModified = false;
                                if (g_Data.modifiedItems.contains(i)) {
                                    auto itemFile = i->GetFile(0);
                                    if ((itemFile && g_Data.modifiedFilesDeleted.contains(itemFile)) || g_Data.modifiedItemsDeleted.contains(i))
                                        ImGui::PushStyleColor(ImGuiCol_Text, colorDeleted);
                                    else {
                                        ImGui::PushStyleColor(ImGuiCol_Text, colorChanged);
                                        isModified = true;
                                    }
                                    popCol++;
                                } else if (g_Data.modifiedItemsShared.contains(i)) {
                                    ImGui::PushStyleColor(ImGuiCol_Text, colorChangedShared);
                                    popCol++;
                                } else if (!WillBeModified(params, i, remappedSrc)) {
                                    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                                    popCol++;
                                }

                                // Check favorite status live (fast set lookup, only for visible items)
                                // This avoids triggering a full cache rebuild when favorites change
                                bool isFavorite = g_Data.favoriteItems.contains(i);
                                std::string name = isFavorite ? "* " + cachedItem.displayName : cachedItem.displayName;

                                // Check if this is a dynamic/enhanced form (when viewing <Currently Worn Armor>)
                                // Dynamic forms are created at runtime for enhanced items with custom enchants/stats
                                bool isDynamicEnhanced = false;
                                if (curMod.empty() && nModSpecial == ModSpecial_Worn && i->IsDynamicForm()) {
                                    isDynamicEnhanced = true;
                                    name = "[+] " + name;
                                }

                                ImGui::TableNextRow();
                                ImGui::TableNextColumn();
                                ImGui::BeginGroup();

                                unsigned int itemChanges = MapFindOr(g_Data.modifiedItems, i, 0u);
                                const bool canUse = (itemChanges & needChanges) == needChanges;

                                bool isChecked = !uncheckedItems.contains(i) && canUse;
                                ImGui::PushID(i->GetFormID());
                                ImGui::BeginDisabled(!canUse);

                            if (isChecked && isModified) hasModifiedItems = true;

                            if (ImGui::Checkbox("##ItemCheckbox", &isChecked)) {
                                if (!isChecked)
                                    uncheckedItems.insert(i);
                                else
                                    uncheckedItems.erase(i);
                            }
                            if (isChecked) {
                                anyItemChanges |= itemChanges;
                                data.items.push_back(i);
                            }

                            ImGui::SameLine();

                            int popVar = 0;
                            if (i == lastSelectedItem) {
                                ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 1.0f);
                                ImGui::PushStyleColor(ImGuiCol_FrameBg, colorChanged);

                                popVar++;
                                popCol++;
                            }

                            // Apply enhanced item color (light blue) for dynamic forms when viewing worn items
                            if (isDynamicEnhanced) {
                                ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(150, 200, 255, 255));
                                popCol++;
                            }

                            bool selected = selectedItems.contains(i);

                            if (i == keyboardNav) {
                                ImGui::SetKeyboardFocusHere();  // This will cause a frame of jitter if the name is too
                                                                // long, can't find a solution
                                keyboardNav = nullptr;
                            }

                            // Allow overlapping widgets (like Tags button) to receive input
                            ImGui::SetNextItemAllowOverlap();
                            if (ImGui::Selectable(name.c_str(), &selected, ImGuiSelectableFlags_AllowDoubleClick | ImGuiSelectableFlags_SpanAllColumns)) {
                                keyboardNav = nullptr;  // Force clear any pending keyboard navigation

                                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
                                    if (isShiftDown) {
                                        if (isAltDown) {
                                            auto allItems = g_MainItemsCache.GetAllSortedItems();
                                            auto armorSet = BuildSetFrom(i, allItems, true);

                                            if (!isCtrlDown) selectedItems.clear();
                                            for (auto piece : armorSet) selectedItems.insert(piece);
                                        }
                                    } else {
                                        if (isAltDown) {
                                            auto allItems = g_MainItemsCache.GetAllSortedItems();
                                            Local::EquipMatchingSet(i, allItems);
                                        } else {
                                            givenItems.ToggleEquip(i);
                                        }
                                    }
                                } else {
                                    if (!isCtrlDown) {
                                        selectedItems.clear();
                                        selected = true;
                                    }

                                    if (!isShiftDown) {
                                        lastSelectedItem = i;
                                        if (selected)
                                            selectedItems.insert(i);
                                        else
                                            selectedItems.erase(i);
                                    } else {
                                        if (lastSelectedItem == i)
                                            selectedItems.insert(i);
                                        else if (lastSelectedItem) {
                                            // Range selection using cache indices
                                            size_t idxCurrent = g_MainItemsCache.FindSortedIndex(i);
                                            size_t idxLast = g_MainItemsCache.FindSortedIndex(lastSelectedItem);
                                            if (idxCurrent != SIZE_MAX && idxLast != SIZE_MAX) {
                                                if (idxCurrent > idxLast) std::swap(idxCurrent, idxLast);
                                                for (size_t ri = idxCurrent; ri <= idxLast; ri++) {
                                                    selectedItems.insert(g_MainItemsCache.GetSortedItem(ri));
                                                }
                                            }
                                        }
                                    }
                                }
                            }

                            if (ImGui::IsItemFocused()) {
                                ImGui::SetScrollFromPosX(xPos - ImGui::GetCursorPosX(),
                                                         1.0f);  // don't know why this is 1.0 instead of 0, but it works

                                auto draw = ImGui::GetWindowDrawList();
                                draw->AddRect(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(), colorChanged);

                                if (ImGui::IsKeyPressed(ImGuiKey_UpArrow)) {
                                    size_t idx = g_MainItemsCache.FindSortedIndex(i);
                                    if (idx != SIZE_MAX && idx > 0) {
                                        keyboardNav = g_MainItemsCache.GetSortedItem(idx - 1);

                                        if (isShiftDown) {
                                            if (!isCtrlDown) {
                                                selectedItems.clear();
                                                selected = false;
                                            }

                                            if (lastSelectedItem == keyboardNav)
                                                selectedItems.insert(keyboardNav);
                                            else if (lastSelectedItem) {
                                                size_t idxNav = idx - 1;
                                                size_t idxLast = g_MainItemsCache.FindSortedIndex(lastSelectedItem);
                                                if (idxLast != SIZE_MAX) {
                                                    size_t rangeStart = idxNav, rangeEnd = idxLast;
                                                    if (rangeStart > rangeEnd) std::swap(rangeStart, rangeEnd);
                                                    for (size_t ri = rangeStart; ri <= rangeEnd; ri++) {
                                                        selectedItems.insert(g_MainItemsCache.GetSortedItem(ri));
                                                    }
                                                }
                                            }
                                        } else if (!isCtrlDown) {
                                            selectedItems.clear();
                                            lastSelectedItem = keyboardNav;
                                            selectedItems.insert(keyboardNav);
                                        }
                                    }
                                } else if (!keyboardNav &&  // or else it scrolls to bottom
                                           ImGui::IsKeyPressed(ImGuiKey_DownArrow)) {
                                    size_t idxDown = g_MainItemsCache.FindSortedIndex(i);
                                    size_t cacheSizeDown = g_MainItemsCache.Size();
                                    if (idxDown != SIZE_MAX && idxDown + 1 < cacheSizeDown) {
                                        keyboardNav = g_MainItemsCache.GetSortedItem(idxDown + 1);

                                        if (isShiftDown) {
                                            if (!isCtrlDown) {
                                                selectedItems.clear();
                                                selected = false;
                                            }

                                            if (lastSelectedItem == keyboardNav)
                                                selectedItems.insert(keyboardNav);
                                            else if (lastSelectedItem) {
                                                size_t idxNav = idxDown + 1;
                                                size_t idxLast = g_MainItemsCache.FindSortedIndex(lastSelectedItem);
                                                if (idxLast != SIZE_MAX) {
                                                    size_t rangeStart = idxNav, rangeEnd = idxLast;
                                                    if (rangeStart > rangeEnd) std::swap(rangeStart, rangeEnd);
                                                    for (size_t ri = rangeStart; ri <= rangeEnd; ri++) {
                                                        selectedItems.insert(g_MainItemsCache.GetSortedItem(ri));
                                                    }
                                                }
                                            }
                                        } else if (!isCtrlDown) {
                                            selectedItems.clear();
                                            lastSelectedItem = keyboardNav;
                                            selectedItems.insert(keyboardNav);
                                        }
                                    }
                                } else if (ImGui::IsKeyPressed(ImGuiKey_Space)) {
                                    if (isAltDown) {
                                        if (!selectedItems.empty()) {
                                            if (uncheckedItems.contains(*selectedItems.begin())) {
                                                for (auto j : selectedItems) uncheckedItems.erase(j);
                                            } else
                                                for (auto j : selectedItems) uncheckedItems.insert(j);
                                        }
                                    } else {
                                        lastSelectedItem = i;
                                        if (selectedItems.contains(i))
                                            selectedItems.erase(i);
                                        else
                                            selectedItems.insert(i);
                                    }
                                } else if (ImGui::IsKeyPressed(ImGuiKey_Delete)) {
                                    for (auto j : selectedItems) givenItems.Remove(j);
                                } else if (ImGui::IsKeyPressed(ImGuiKey_Backspace)) {
                                    givenItems.Pop();
                                }
                            }

                            ImGui::EndDisabled();
                            ImGui::PopID();
                            ImGui::EndGroup();

                            if (ImGui::IsItemHovered(ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_NoSharedDelay)) {
                                if (ImGui::BeginTooltip()) {
                                    ImGui::PushStyleColor(ImGuiCol_Text, colorTextDefault);

                                    if (curMod.size()!=1) {
                                        if (auto file = i->GetFile(0)) {
                                            ImGui::Text(LZ("File: %s"), file->fileName);
                                        } else {
                                            // Dynamic item - show enhanced info
                                            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(150, 200, 255, 255));
                                            if (curMod.empty() && nModSpecial == ModSpecial_Worn) {
                                                ImGui::Text(LZ("Enhanced Item"));
                                                ImGui::TextWrapped(LZ("This item has custom enchantments or stats applied.\nIt was created from an outfit configuration."));
                                            } else {
                                                ImGui::Text(LZ("Dynamic Item"));
                                                ImGui::TextWrapped(LZ("This item was created at runtime (e.g. by Transmog).\nIt will not persist in outfits/favorites after restarting the game."));
                                            }
                                            ImGui::PopStyleColor();
                                        }
                                    }

                                    if (auto armor = i->As<RE::TESObjectARMO>()) {
                                        static const char* strArmorType[] = {"Light Armor", "Heavy Armor", "Clothing"};
                                        int nType = (int)armor->bipedModelData.armorType.get();
                                        if (nType >= 0 && nType <= 2) ImGui::Text(LZ(strArmorType[nType]));

                                        if (params.curve) {
                                            ImGui::Text("Slots:");
                                            ImGui::Indent();
                                            auto slotsCur = (ArmorSlots)armor->GetSlotMask().underlying();
                                            auto slotsOrig = MapFindOr(g_Data.modifiedArmorSlots, armor, slotsCur);
                                            auto slotsCombined = slotsCur | slotsOrig;

                                            // Get player's equipped armor for each slot
                                            auto player = NPCTargets::GetActor();

                                            if (slotsCombined) {
                                                for (int slot = 0; slot < 32; slot++) {
                                                    if (slotsCombined & (1 << slot)) {
                                                        // Check if player has an item equipped in this slot
                                                        RE::TESObjectARMO* equippedArmor = nullptr;
                                                        if (player) {
                                                            auto bipedSlot = static_cast<RE::BGSBipedObjectForm::BipedObjectSlot>(1 << slot);
                                                            equippedArmor = player->GetWornArmor(bipedSlot);
                                                            // Don't count as "equipped" if it's the same item we're hovering over
                                                            if (equippedArmor == armor) equippedArmor = nullptr;
                                                        }

                                                        // Determine text color and prefix
                                                        bool isSlotTaken = equippedArmor != nullptr;

                                                        if (!(slotsCur & (1 << slot))) {
                                                            // Slot was removed (deleted)
                                                            ImGui::PushStyleColor(ImGuiCol_Text, colorDeleted);
                                                            ImGui::Text(LZFormat("-Slot {} - {}", slot + 30, LZ(params.curve->slotName[slot].c_str())).c_str());
                                                            ImGui::PopStyleColor();
                                                        } else if (!(slotsOrig & (1 << slot))) {
                                                            // Slot was added
                                                            if (isSlotTaken) {
                                                                ImGui::PushStyleColor(ImGuiCol_Text, colorDeleted);
                                                                ImGui::Text(LZFormat("+Slot {} - {}", slot + 30, LZ(params.curve->slotName[slot].c_str())).c_str());
                                                                ImGui::Text(LZFormat("  Replaces: {}", equippedArmor->GetName()).c_str());
                                                                ImGui::PopStyleColor();
                                                            } else {
                                                                ImGui::PushStyleColor(ImGuiCol_Text, colorChanged);
                                                                ImGui::Text(LZFormat("+Slot {} - {}", slot + 30, LZ(params.curve->slotName[slot].c_str())).c_str());
                                                                ImGui::PopStyleColor();
                                                            }
                                                        } else {
                                                            // Unchanged slot
                                                            if (isSlotTaken) {
                                                                ImGui::PushStyleColor(ImGuiCol_Text, colorDeleted);
                                                                ImGui::Text(LZFormat("Slot {} - {}", slot + 30, LZ(params.curve->slotName[slot].c_str())).c_str());
                                                                ImGui::Text(LZFormat("  Replaces: {}", equippedArmor->GetName()).c_str());
                                                                ImGui::PopStyleColor();
                                                            } else {
                                                                ImGui::Text(LZFormat("Slot {} - {}", slot + 30, LZ(params.curve->slotName[slot].c_str())).c_str());
                                                            }
                                                        }
                                                    }
                                                }
                                            } else
                                                ImGui::Text(LZ("None"));
                                            ImGui::Unindent();

                                            if (armor->formEnchanting) ImGui::Text(LZFormat("Enchantment: {}", armor->formEnchanting->GetFullName()).c_str());

                                            if (armor->numKeywords > 0) {
                                                ImGui::Text(LZ("Keywords:"));
                                                ImGui::Indent();
                                                for (unsigned int n = 0; n < armor->numKeywords; n++)
                                                    if (armor->keywords[n]) ImGui::Text(armor->keywords[n]->GetFormEditorID());
                                                ImGui::Unindent();
                                            }
                                        }

                                    } else if (auto weapon = i->As<RE::TESObjectWEAP>()) {
                                        static const char* strWeaponType[] = {"Fist",     "1H Sword", "1H Dagger", "1H Axe", "1H Mace",
                                                                              "2H Sword", "2H Axe",   "Bow",       "Staff",  "Crossbow"};
                                        int nType = (int)weapon->GetWeaponType();
                                        if (nType >= 0 && nType <= 9) ImGui::Text(LZ(strWeaponType[nType]));

                                        if (weapon->formEnchanting) ImGui::Text(LZFormat("Enchantment: {}", weapon->formEnchanting->GetFullName()).c_str());

                                        if (weapon->numKeywords > 0) {
                                            ImGui::Text(LZ("Keywords:"));
                                            ImGui::Indent();
                                            for (unsigned int n = 0; n < weapon->numKeywords; n++)
                                                if (weapon->keywords[n]) ImGui::Text(weapon->keywords[n]->GetFormEditorID());
                                            ImGui::Unindent();
                                        }
                                    }

                                    ImGui::PopStyleColor();
                                    ImGui::EndTooltip();
                                }
                            }

                            // Render additional columns based on visibility and order
                            for (int idx = 0; idx < Config::TableColumnConfig::Col_Count; idx++) {
                                int col = displayOrder[idx];
                                if (!g_Config.tableColumns.visible[col]) continue;
                                if (col == Config::TableColumnConfig::Col_Name) continue;  // Name column already rendered

                                ImGui::TableNextColumn();

                                switch (col) {
                                    case Config::TableColumnConfig::Col_Slot:
                                        // Slot column - muted red if conflict, otherwise light grey
                                        if (itemSlotInfo.hasConflict) {
                                            ImGui::PushStyleColor(ImGuiCol_Text, slotConflictColor);
                                        } else {
                                            ImGui::PushStyleColor(ImGuiCol_Text, mainSlotColor);
                                        }
                                        ImGui::Text("%s", itemSlotInfo.slotName.c_str());
                                        ImGui::PopStyleColor();
                                        break;

                                    case Config::TableColumnConfig::Col_Armor:
                                        // Armor column - show armor rating or empty for non-armor
                                        ImGui::PushStyleColor(ImGuiCol_Text, mainSlotColor);
                                        if (itemSlotInfo.armorRating > 0) {
                                            ImGui::Text("%.0f", itemSlotInfo.armorRating);
                                        }
                                        ImGui::PopStyleColor();
                                        break;

                                    case Config::TableColumnConfig::Col_ID:
                                        // ID column (light grey)
                                        ImGui::PushStyleColor(ImGuiCol_Text, mainSlotColor);
                                        ImGui::Text("%s", itemSlotInfo.slotIDs.c_str());
                                        ImGui::PopStyleColor();
                                        break;

                                    case Config::TableColumnConfig::Col_NifF:
                                        // NIF (Female) column (light grey)
                                        ImGui::PushStyleColor(ImGuiCol_Text, mainSlotColor);
                                        ImGui::Text("%s", cachedItem.femaleNif.c_str());
                                        ImGui::PopStyleColor();
                                        break;

                                    case Config::TableColumnConfig::Col_Mod:
                                        // Mod column (light grey)
                                        ImGui::PushStyleColor(ImGuiCol_Text, mainSlotColor);
                                        ImGui::Text("%s", cachedItem.modName.c_str());
                                        ImGui::PopStyleColor();
                                        break;
                                }
                            }

                            // Tags column (always at end)
                            ImGui::TableNextColumn();
                            {
                                // Count tags for this item
                                size_t tagCount = cachedItem.tags ? cachedItem.tags->size() : 0;

                                // Create a unique popup ID for this item
                                ImGui::PushID(static_cast<int>(reinterpret_cast<intptr_t>(i)));

                                // Show tag count or "..." button
                                char tagLabel[32];
                                if (tagCount > 0) {
                                    snprintf(tagLabel, sizeof(tagLabel), "%zu", tagCount);
                                } else {
                                    strcpy(tagLabel, "...");
                                }

                                ImGui::PushStyleColor(ImGuiCol_Text, mainSlotColor);
                                if (ImGui::SmallButton(tagLabel)) {
                                    ImGui::OpenPopup("##ItemTagsPopup");
                                }
                                ImGui::PopStyleColor();

                                // Tags popup
                                if (ImGui::BeginPopup("##ItemTagsPopup")) {
                                    ImGui::Text(LZ("Tags for: %s"), cachedItem.displayName.c_str());
                                    ImGui::Separator();

                                    // Copy global tags to avoid iterator invalidation when removing tags
                                    auto globalTagsCopy = g_Data.globalItemTags;

                                    // Show all global tags as checkboxes
                                    for (const auto& tag : globalTagsCopy) {
                                        bool hasTag = cachedItem.tags && cachedItem.tags->contains(tag);
                                        if (ImGui::Checkbox(tag.c_str(), &hasTag)) {
                                            if (hasTag) {
                                                AddItemTag(i, tag);
                                            } else {
                                                RemoveItemTag(i, tag);
                                            }
                                            SaveItemTags();
                                            // Update cached tags pointer
                                            const_cast<CachedItemData&>(cachedItem).tags = GetItemTags(i);
                                        }
                                    }

                                    ImGui::Separator();

                                    // Add new tag button
                                    static char newTagBuf[64] = "";
                                    ImGui::SetNextItemWidth(100);
                                    if (ImGui::InputTextWithHint("##NewTag", LZ("New tag"), newTagBuf, sizeof(newTagBuf) - 1,
                                        ImGuiInputTextFlags_EnterReturnsTrue)) {
                                        std::string normalized = NormalizeTagName(newTagBuf);
                                        if (IsValidTagName(normalized)) {
                                            AddItemTag(i, normalized);
                                            SaveItemTags();
                                            const_cast<CachedItemData&>(cachedItem).tags = GetItemTags(i);
                                            newTagBuf[0] = '\0';
                                        }
                                    }
                                    ImGui::SameLine();
                                    if (ImGui::SmallButton(LZ("Add"))) {
                                        std::string normalized = NormalizeTagName(newTagBuf);
                                        if (IsValidTagName(normalized)) {
                                            AddItemTag(i, normalized);
                                            SaveItemTags();
                                            const_cast<CachedItemData&>(cachedItem).tags = GetItemTags(i);
                                            newTagBuf[0] = '\0';
                                        }
                                    }

                                    // Show hint if no tags exist
                                    if (g_Data.globalItemTags.empty()) {
                                        ImGui::TextDisabled(LZ("No tags defined. Add one above."));
                                    }

                                    ImGui::EndPopup();
                                }

                                ImGui::PopID();
                            }

                            ImGui::PopStyleColor(popCol);
                            ImGui::PopStyleVar(popVar);
                            }  // End of for (int row = ...) loop
                        }  // End of while (clipper.Step())

                        // Handle F key for favorites outside item focus - works on selected items
                        if (ImGui::IsWindowFocused() && !selectedItems.empty() && ImGui::IsKeyPressed(ImGuiKey_F)) {
                            // Toggle favorite for all selected items
                            for (auto j : selectedItems) {
                                std::string formID = QARFormID(j);

                                if (g_Data.favoriteItems.contains(j)) {
                                    // Remove from favorites
                                    g_Data.favoriteItems.erase(j);
                                    g_Data.favoriteItemsMap.erase(formID);
                                } else {
                                    // Add to favorites
                                    g_Data.favoriteItems.insert(j);
                                    g_Data.favoriteItemsMap.insert(formID);
                                }
                            }

                            // Save immediately for persistence
                            SaveFavorites();
                            // Note: No g_filterRound++ needed - favorite status is checked live during rendering
                        }

                        // Handle Enter key to equip items - works on selected items regardless of focus
                        // Disabled when any modal dialog is open (e.g. Create Outfit, Rename Outfit)
                        if (!selectedItems.empty() && ImGui::IsKeyPressed(ImGuiKey_Enter) &&
                            !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)) {
                            static double lastEnterPressTime = 0.0;
                            double currentTime = ImGui::GetTime();

                            if (currentTime - lastEnterPressTime >= 0.15) {  // 150ms cooldown
                                for (auto j : selectedItems) givenItems.ToggleEquip(j);

                                // Skip extra frames to allow equip/unequip to complete (with multiplier)
                                int framesToSkip = static_cast<int>(3 * g_Config.outfitFrameMultiplier);
                                for (int i = 0; i < framesToSkip; i++) {
                                    g_Pause.SkipFrame();
                                }

                                lastEnterPressTime = currentTime;
                            }
                        }

                        // Global Numpad shortcuts (work regardless of item list focus)
                        // Only active when no modal is open
                        if (!ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId)) {
                            static double lastNumpadItemTime = 0.0;
                            double currentTime = ImGui::GetTime();
                            const double numpadCooldown = 0.15;  // 150ms cooldown

                            // Note: cacheSize is already defined above at line 4211

                            // Numpad4: Toggle equip selected item (same as Enter)
                            if (ImGui::IsKeyPressed(ImGuiKey_Keypad4) && !selectedItems.empty() &&
                                currentTime - lastNumpadItemTime >= numpadCooldown) {
                                // Clear multi-selection, keep only first item
                                if (selectedItems.size() > 1) {
                                    auto firstItem = *selectedItems.begin();
                                    selectedItems.clear();
                                    selectedItems.insert(firstItem);
                                    lastSelectedItem = firstItem;
                                }

                                for (auto j : selectedItems) givenItems.ToggleEquip(j);

                                int framesToSkip = static_cast<int>(3 * g_Config.outfitFrameMultiplier);
                                for (int i = 0; i < framesToSkip; i++) {
                                    g_Pause.SkipFrame();
                                }

                                lastNumpadItemTime = currentTime;
                            }

                            // Numpad6: Toggle favorite on selected item (same as F)
                            if (ImGui::IsKeyPressed(ImGuiKey_Keypad6) && !selectedItems.empty() &&
                                currentTime - lastNumpadItemTime >= numpadCooldown) {
                                // Clear multi-selection, keep only first item
                                if (selectedItems.size() > 1) {
                                    auto firstItem = *selectedItems.begin();
                                    selectedItems.clear();
                                    selectedItems.insert(firstItem);
                                    lastSelectedItem = firstItem;
                                }

                                for (auto j : selectedItems) {
                                    std::string formID = QARFormID(j);
                                    if (g_Data.favoriteItems.contains(j)) {
                                        g_Data.favoriteItems.erase(j);
                                        g_Data.favoriteItemsMap.erase(formID);
                                    } else {
                                        g_Data.favoriteItems.insert(j);
                                        g_Data.favoriteItemsMap.insert(formID);
                                    }
                                }
                                SaveFavorites();
                                // Note: No g_filterRound++ needed - favorite status is checked live during rendering

                                lastNumpadItemTime = currentTime;
                            }

                            // Numpad3: Toggle "Mark to Keep" on selected items
                            if (!NPCTargets::IsNPC() && ImGui::IsKeyPressed(ImGuiKey_Keypad3) && !selectedItems.empty() &&
                                currentTime - lastNumpadItemTime >= numpadCooldown) {
                                for (auto j : selectedItems) {
                                    // Toggle the keep status
                                    bool currentlyMarked = IsItemMarkedToKeep(j);
                                    MarkItemToKeep(j, !currentlyMarked);
                                }

                                lastNumpadItemTime = currentTime;
                            }

                            // Numpad8: Select previous item and equip (with wrap)
                            if (ImGui::IsKeyPressed(ImGuiKey_Keypad8) && cacheSize > 0 &&
                                currentTime - lastNumpadItemTime >= numpadCooldown) {
                                size_t currentIdx = 0;

                                // Find current selection index, or start from end if none
                                if (!selectedItems.empty()) {
                                    auto lastItem = lastSelectedItem ? lastSelectedItem : *selectedItems.begin();
                                    currentIdx = g_MainItemsCache.FindSortedIndex(lastItem);
                                    if (currentIdx == SIZE_MAX) currentIdx = 0;
                                }

                                // Move to previous with wrap
                                size_t newIdx = (currentIdx == 0) ? (cacheSize - 1) : (currentIdx - 1);
                                auto newItem = g_MainItemsCache.GetSortedItem(newIdx);

                                // Clear selection and select new item
                                selectedItems.clear();
                                selectedItems.insert(newItem);
                                lastSelectedItem = newItem;
                                keyboardNav = newItem;  // Trigger auto-scroll

                                // Equip the item
                                givenItems.ToggleEquip(newItem);

                                int framesToSkip = static_cast<int>(3 * g_Config.outfitFrameMultiplier);
                                for (int i = 0; i < framesToSkip; i++) {
                                    g_Pause.SkipFrame();
                                }

                                lastNumpadItemTime = currentTime;
                            }

                            // Numpad5: Select next item and equip (with wrap)
                            if (ImGui::IsKeyPressed(ImGuiKey_Keypad5) && cacheSize > 0 &&
                                currentTime - lastNumpadItemTime >= numpadCooldown) {
                                size_t currentIdx = SIZE_MAX;

                                // Find current selection index, or start from beginning if none
                                if (!selectedItems.empty()) {
                                    auto lastItem = lastSelectedItem ? lastSelectedItem : *selectedItems.begin();
                                    currentIdx = g_MainItemsCache.FindSortedIndex(lastItem);
                                }
                                if (currentIdx == SIZE_MAX) currentIdx = cacheSize;  // Will wrap to 0

                                // Move to next with wrap
                                size_t newIdx = (currentIdx + 1 >= cacheSize) ? 0 : (currentIdx + 1);
                                auto newItem = g_MainItemsCache.GetSortedItem(newIdx);

                                // Clear selection and select new item
                                selectedItems.clear();
                                selectedItems.insert(newItem);
                                lastSelectedItem = newItem;
                                keyboardNav = newItem;  // Trigger auto-scroll

                                // Equip the item
                                givenItems.ToggleEquip(newItem);

                                int framesToSkip = static_cast<int>(3 * g_Config.outfitFrameMultiplier);
                                for (int i = 0; i < framesToSkip; i++) {
                                    g_Pause.SkipFrame();
                                }

                                lastNumpadItemTime = currentTime;
                            }

                            // Numpad2: Select random unequipped item and equip
                            if (ImGui::IsKeyPressed(ImGuiKey_Keypad2) && cacheSize > 0 &&
                                currentTime - lastNumpadItemTime >= numpadCooldown) {
                                // Pre-compute set of equipped items ONCE (32 GetWornArmor calls max)
                                // This avoids O(N*32) GetWornArmor calls when checking each item
                                std::set<RE::TESBoundObject*> equippedSet;
                                if (player) {
                                    // Get all worn armor (32 slots)
                                    for (int slot = 0; slot < 32; slot++) {
                                        auto bipedSlot = static_cast<RE::BGSBipedObjectForm::BipedObjectSlot>(1 << slot);
                                        if (auto armor = player->GetWornArmor(bipedSlot)) {
                                            equippedSet.insert(armor);
                                        }
                                    }
                                    // Get equipped weapons
                                    for (bool leftHand : {false, true}) {
                                        if (auto obj = player->GetEquippedObject(leftHand)) {
                                            if (auto boundObj = obj->As<RE::TESBoundObject>()) {
                                                equippedSet.insert(boundObj);
                                            }
                                        }
                                    }
                                }

                                // Build list of unequipped items from filtered list
                                std::vector<RE::TESBoundObject*> unequippedItems;
                                unequippedItems.reserve(cacheSize);

                                for (size_t idx = 0; idx < cacheSize; idx++) {
                                    auto item = g_MainItemsCache.GetSortedItem(idx);

                                    // O(1) set lookup instead of O(32) GetWornArmor calls per item
                                    if (!equippedSet.contains(item)) {
                                        unequippedItems.push_back(item);
                                    }
                                }

                                if (!unequippedItems.empty()) {
                                    // Select random item
                                    static std::mt19937 rng(std::random_device{}());
                                    std::uniform_int_distribution<size_t> dist(0, unequippedItems.size() - 1);
                                    auto randomItem = unequippedItems[dist(rng)];

                                    // Select and scroll to item
                                    selectedItems.clear();
                                    selectedItems.insert(randomItem);
                                    lastSelectedItem = randomItem;
                                    keyboardNav = randomItem;  // Trigger auto-scroll

                                    // Equip the item
                                    givenItems.Give(randomItem, true);

                                    int framesToSkip = static_cast<int>(3 * g_Config.outfitFrameMultiplier);
                                    for (int i = 0; i < framesToSkip; i++) {
                                        g_Pause.SkipFrame();
                                    }
                                }

                                lastNumpadItemTime = currentTime;
                            }

                            // Numpad1: Restore last equipped outfit
                            // Uses separate timing to avoid interference with other numpad handlers
                            if (ImGui::IsKeyPressed(ImGuiKey_Keypad1) &&
                                !g_LastEquippedOutfit.empty() &&
                                g_Data.outfits.contains(g_LastEquippedOutfit)) {
                                static double lastNumpad1Time = 0.0;
                                double now = ImGui::GetTime();

                                if (now - lastNumpad1Time >= 0.5) {  // 500ms cooldown for outfit operations
                                    auto& outfit = g_Data.outfits[g_LastEquippedOutfit];

                                    // Unequip current items first
                                    givenItems.UnequipCurrent();

                                    // Skip frames to allow unequip to complete
                                    int unequipFrames = static_cast<int>(3 * g_Config.outfitFrameMultiplier);
                                    for (int i = 0; i < unequipFrames; i++) {
                                        g_Pause.SkipFrame();
                                    }

                                    // Equip all items from the outfit
                                    for (auto item : outfit.items) {
                                        std::string formID = QARFormID(item);
                                        if (outfit.enhancedItems.contains(formID) &&
                                            outfit.enhancedItems[formID].IsEnhanced()) {
                                            givenItems.GiveEnhanced(item, outfit.enhancedItems[formID], true);
                                        } else {
                                            givenItems.Give(item, true);
                                        }
                                    }

                                    // Skip frames to allow equips to complete
                                    int equipFrames = static_cast<int>(3 * g_Config.outfitFrameMultiplier);
                                    for (int i = 0; i < equipFrames; i++) {
                                        g_Pause.SkipFrame();
                                    }

                                    lastNumpad1Time = now;
                                }
                            }
                        }

                        ImGui::EndTable();
                    }

                    ImGui::PopStyleColor();

                    ImGui::BeginDisabled(!player || curMod.empty() || isInventoryOpen);

                    if (ImGui::Button(selectedItems.empty() ? LZ("Give All") : LZ("Give Selected"))) {
                        if (selectedItems.empty())
                            for (auto i : data.filteredItems) {
                                givenItems.Give(i);
                            }
                        else
                            for (auto i : selectedItems) {
                                givenItems.Give(i);
                            }
                    }
                    if (isInventoryOpen) MakeTooltip(LZ("Can't use while inventory is open"));

                    ImGui::SameLine();
                    if (ImGui::Button(selectedItems.empty() ? LZ("Equip All") : LZ("Equip Selected"))) {
                        givenItems.UnequipCurrent();
                        if (selectedItems.empty())
                            for (auto i : data.filteredItems) {
                                givenItems.Give(i, true);
                            }
                        else
                            for (auto i : selectedItems) {
                                givenItems.Give(i, true);
                            }
                    }
                    if (isInventoryOpen) MakeTooltip(LZ("Can't use while inventory is open"));

                    ImGui::BeginDisabled(NPCTargets::IsNPC() ? NPCTargets::TrackedCount() == 0 : givenItems.items.empty());
                    ImGui::SameLine();
                    if (ImGui::Button(LZ("Delete Given"))) {
                        givenItems.Remove();
                    }
                    if (isInventoryOpen) MakeTooltip(LZ("Can't use while inventory is open"));

                    ImGui::EndDisabled();  // givenItems.empty()

                    ImGui::EndDisabled();  //! player || curMod.empty() || isInventoryOpen

                    // Remove All Tracked Items button - removes items given across sessions
                    // This button is OUTSIDE the curMod disabled block so it's always accessible
                    {
                        int totalTracked = NPCTargets::IsNPC() ? NPCTargets::TrackedCount() : g_ItemTracking.GetTotalGivenCount();
                        int removableCount = NPCTargets::IsNPC() ? NPCTargets::TrackedCount() : g_ItemTracking.GetRemovableCount();
                        int keptCount = totalTracked - removableCount;

                        ImGui::BeginDisabled(!player || isInventoryOpen);
                        ImGui::SameLine();

                        // Build button label with count
                        std::string removeLabel;
                        if (keptCount > 0) {
                            removeLabel = LZFormat("Remove Tracked ({}/{})", removableCount, totalTracked);
                        } else if (totalTracked > 0) {
                            removeLabel = LZFormat("Remove Tracked ({})", totalTracked);
                        } else {
                            removeLabel = LZ("Remove Tracked");
                        }

                        if (ImGui::Button(removeLabel.c_str())) {
                            givenItems.RemoveAllTracked();
                        }
                        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
                            if (!player) {
                                ImGui::SetTooltip(LZ("Selected actor not available."));
                            } else if (isInventoryOpen) {
                                ImGui::SetTooltip(LZ("Can't use while inventory is open."));
                            } else if (NPCTargets::IsNPC()) {
                                ImGui::SetTooltip("Remove only quantities given to this NPC during the current nearby session.\nDoes not use the player's tracking or Keep marks; does not delete the NPC outfit file.");
                            } else if (totalTracked > 0) {
                                ImGui::SetTooltip(LZ("Remove all items given via this UI across sessions.\n"
                                                    "Items marked 'Keep' will not be removed.\n"
                                                    "Total tracked: %d, Removable: %d, Kept: %d"),
                                                    totalTracked, removableCount, keptCount);
                            } else {
                                ImGui::SetTooltip(LZ("No tracked items to remove."));
                            }
                        }
                        ImGui::EndDisabled();
                    }

                    bSlotWarning = false;
                    for (auto i : data.items) {
                        if (auto armor = i->As<RE::TESObjectARMO>()) {
                            auto itemSlots = MapFindOr(g_Data.modifiedArmorSlots, armor, (ArmorSlots)armor->GetSlotMask().underlying());
                            if ((~g_Config.usedSlotsMask) & (~remappedSrc) & itemSlots) {
                                bSlotWarning = true;
                                break;
                            }
                        }
                    }
                }

                ImGui::EndTable();
            }

            // ImGui::PopItemWidth();
        } else {
            ImGui::Text(g_Config.strCriticalError.c_str());
        }

        static char bufDisable[4096] = "";
        static char bufItemBlacklist[4096] = "";
        static bool bItemBlacklistChanged = false;

        if (popupSettings) {
            ImGui::OpenPopup(LZ("Settings"));

            std::string strDisable;
            for (auto& w : g_Config.lsDisableWords) {
                if (!strDisable.empty()) strDisable += "\n";
                strDisable += w;
            }
            strDisable.copy(bufDisable, sizeof(bufDisable));
            bufDisable[sizeof(bufDisable) - 1] = '\0';

            std::string strItemBlacklist;
            for (auto& w : g_Config.itemBlacklist) {
                if (!strItemBlacklist.empty()) strItemBlacklist += "\n";
                strItemBlacklist += w;
            }
            strItemBlacklist.copy(bufItemBlacklist, sizeof(bufItemBlacklist));
            bufItemBlacklist[sizeof(bufItemBlacklist) - 1] = '\0';

            bItemBlacklistChanged = false;
        }
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

        bool bPopupActive = true;
        if (ImGui::BeginPopupModal("Settings", &bPopupActive, ImGuiWindowFlags_AlwaysAutoResize)) {
            if (g_Config.bShortcutEscCloseWindow && ImGui::Shortcut(ImGuiKey_Escape)) {
                bPopupActive = false;
                ImGui::CloseCurrentPopup();
            }

            ImGui::Text(LZ("NOTE: Many changes require restarting Skyrim to take full effect."));
            if (ImGui::BeginTabBar(LZ("Settings Tabs"))) {
                if (ImGui::BeginTabItem(LZ("General"))) {
                    ImGui::Text(LZ("Language"));
                    ImGui::SameLine();

                    std::string language;
                    {
                        wchar_t languageName[256];
                        if (GetLocaleInfoEx(Localization::Get()->language.c_str(), LOCALE_SLOCALIZEDLANGUAGENAME, languageName, 256) > 0) {
                            language = WStringToString(std::wstring(languageName));
                        } else {
                            language = "English";
                            Localization::Get()->SetTranslation(L"en");
                        }
                    }

                    if (ImGui::BeginCombo("##Language", language.c_str(), ImGuiComboFlags_PopupAlignLeft)) {
                        static std::vector<std::pair<std::wstring, std::string>> languagesTranslated;
                        static std::vector<std::pair<std::wstring, std::string>> languagesUntranslated;

                        if (languagesTranslated.empty() && languagesUntranslated.empty()) {
                            std::set<std::wstring> codes;
                            struct LocaleEnum {
                                static BOOL CALLBACK Proc(LPWSTR lpLocaleStr, DWORD, LPARAM lParam) {
                                    // Convert the wide-character locale name to a narrow string
                                    std::wstring ws(lpLocaleStr);

                                    size_t underscore_pos = ws.find(L'-');
                                    if (!underscore_pos) return TRUE;

                                    if (underscore_pos != std::string::npos) {
                                        ws = ws.substr(0, underscore_pos);
                                    }
                                    auto locales = reinterpret_cast<std::set<std::wstring>*>(lParam);
                                    locales->insert(ws);

                                    // Return true to continue enumeration
                                    return TRUE;
                                }
                            };

                            EnumSystemLocalesEx(LocaleEnum::Proc, LOCALE_ALL, reinterpret_cast<LPARAM>(&codes), nullptr);

                            languagesTranslated.reserve(Localization::Get()->translations.size());
                            languagesUntranslated.reserve(codes.size());

                            for (auto& i : codes) {
                                wchar_t languageName[256];

                                if (GetLocaleInfoEx(i.c_str(), LOCALE_SLOCALIZEDLANGUAGENAME, languageName, 256) > 0) {
                                    auto str = WStringToString(std::wstring(languageName));
                                    if (Localization::Get()->HasTranslation(i.c_str()))
                                        languagesTranslated.push_back({i, str});
                                    else
                                        languagesUntranslated.push_back({i, str});
                                }
                            }

                            std::sort(languagesTranslated.begin(), languagesTranslated.end(), [](auto& a, auto& b) { return a.second.compare(b.second) < 0; });
                            std::sort(languagesUntranslated.begin(), languagesUntranslated.end(), [](auto& a, auto& b) { return a.second.compare(b.second) < 0; });
                        }

                        auto Entry = [](auto& i) {
                            bool selected = !Localization::Get()->language.compare(i.first);
                            if (ImGui::Selectable(i.second.c_str(), selected)) {
                                Localization::Get()->SetTranslation(i.first);
                                bRebuildFonts = true;
                            }
                            if (selected) ImGui::SetItemDefaultFocus();
                        };
                        std::for_each(languagesTranslated.begin(), languagesTranslated.end(), Entry);

                        ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
                        std::for_each(languagesUntranslated.begin(), languagesUntranslated.end(), Entry);
                        ImGui::PopStyleColor();

                        ImGui::EndCombo();
                    }

                    ImGui::SameLine();
                    ImGui::Checkbox(LZ("Export untranslated strings"), &g_Config.bExportUntranslated);

                    if (ImGui::SliderInt(LZ("Font Size"), &g_Config.nFontSize, kFontSizeMin, kFontSizeMax, "%d", ImGuiSliderFlags_AlwaysClamp)) {
                        bRebuildFonts = true;
                    }
                    MakeTooltip(
                        LZ("Large sizes look best with a .ttf placed in the plugin's fonts folder -\n"
                           "the built in font is a bitmap and gets blocky when scaled up."));

                    const char* logLevels[] = {LZ("Trace"), LZ("Debug"), LZ("Info"), LZ("Warn"), LZ("Error"), LZ("Critical"), LZ("Off")};

                    ImGui::Text(LZ("Log verbsity"));
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

                    ImGui::Checkbox(LZ("Close console after qar command"), &g_Config.bCloseConsole);
                    ImGui::Checkbox(LZ("Pause game while QAR is open"), &g_Config.bPauseWhileOpen);
                    ImGui::Checkbox(LZ("Delete given items after applying changes"), &g_Config.bAutoDeleteGiven);
                    ImGui::Checkbox(LZ("Round weights to 0.1"), &g_Config.bRoundWeight);
                    ImGui::Checkbox(LZ("Reset sliders after changing mods"), &g_Config.bResetSliders);
                    ImGui::Checkbox(LZ("Reset slot remapping after changing mods"), &g_Config.bResetSlotRemap);
                    ImGui::Checkbox(LZ("Allow remapping armor slots to unhandled slots"), &g_Config.bAllowInvalidRemap);
                    ImGui::Checkbox(LZ("Allow remapping of protected armor slots (Not recommended)"), &g_Config.bEnableProtectedSlotRemapping);
                    MakeTooltip(
                        LZ("Body, hands, feet, and head slots tend to break in various ways if you move them to other "
                           "slots."));
                    ImGui::Checkbox(LZ("Highlight things you may want to look at"), &g_Config.bHighlights);
                    ImGui::Checkbox(LZ("Show Frostfall coverage slider even if not installed"), &g_Config.bShowFrostfallCoverage);
                    ImGui::Checkbox(LZ("USE AT YOUR OWN RISK: Enable <All Items> in item list"), &g_Config.bEnableAllItems);
                    MakeTooltip(
                        LZ("This can result in performance issues, and making a mess by changing too many items at once.\n"
                           "Use with caution."));

                    ImGui::Text(LZ("Automatically disable items with the following words (one per line):"));
                    if (ImGui::InputTextMultiline("##DisableWords", bufDisable, sizeof(bufDisable), ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 5))) {
                        std::vector<std::string> lines;
                        std::istringstream stream(bufDisable);
                        std::string line;

                        while (std::getline(stream, line)) {
                            // Trim whitespace
                            size_t start = line.find_first_not_of(" \t\n\r");
                            if (start == std::string::npos) continue;  // String is all whitespace
                            size_t end = line.find_last_not_of(" \t\n\r");
                            line = line.substr(start, end - start + 1);
                            lines.push_back(MakeLower(line));
                        }

                        g_Config.lsDisableWords = std::move(lines);
                        g_Config.RebuildDisabledWords();
                    }

                    ImGui::Spacing();
                    ImGui::Text(LZ("Item blacklist - hide items containing these strings (one per line):"));
                    MakeTooltip(LZ("Items with names containing any of these strings will be hidden from the main items table.\n"
                                   "Case-insensitive matching."));
                    if (ImGui::InputTextMultiline("##ItemBlacklist", bufItemBlacklist, sizeof(bufItemBlacklist), ImVec2(-FLT_MIN, ImGui::GetTextLineHeight() * 5))) {
                        bItemBlacklistChanged = true;
                    }

                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem(LZ("Distribution"))) {
                    ImGui::Checkbox(LZ("Enable regional loot"), &g_Config.bEnableRegionalLoot);
                    MakeTooltip(LZ("If disabled, all items are treated as having no region assignment."));
                    ImGui::BeginDisabled(!g_Config.bEnableRegionalLoot);
                    ImGui::Checkbox(LZ("Enable cross region loot"), &g_Config.bEnableCrossRegionLoot);
                    MakeTooltip(LZ("If disabled, items will ONLY appear in their assigned regions."));
                    ImGui::EndDisabled();
                    ImGui::Checkbox(LZ("Enable out of place loot"), &g_Config.bEnableMigratedLoot);
                    MakeTooltip(
                        LZ("This allows loot to appear in places it wasn't assigned at a reduced rate.\n"
                           "Example: A small chance to find Ancient Nord loot inside a Bandit chest."));

                    ImGui::Checkbox(LZ("Enforce loot rarity with empty loot"), &g_Config.bEnableRarityNullLoot);
                    MakeTooltip(
                        LZ("Example: The subset of items elible for loot has 1 rare item, 0 commons and uncommons\n"
                           "DISABLED: You will always get that rare item\n"
                           "ENABLED: You will have a 5%% chance at the item, and 95%% chance of nothing"));

                    ImGui::Checkbox(LZ("Normalize drop rates between mods"), &g_Config.bNormalizeModDrops);
                    ImGui::SliderFloat(LZ("Adjust drop rates"), &g_Config.fDropRates, 0.0f, 300.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp);
                    ImGui::SliderInt(LZ("Drop curve level granularity"), &g_Config.levelGranularity, 1, 5, "%d", ImGuiSliderFlags_AlwaysClamp);
                    MakeTooltip(LZ("A lower number generates more loot lists but more accurate distribution"));

                    ImGui::Checkbox(LZ("Prevent distribution of dynamic variants"), &g_Config.bPreventDistributionOfDynamicVariants);

                    if (ImGui::Checkbox(LZ("Add random enchantments to distributed gear"), &g_Config.bEnableEnchantmentDistrib)) {
                        if (g_Config.bEnableEnchantmentDistrib) InstallEnchantmentHooks();
                    }
                    ImGui::SliderFloat(LZ("Adjust enchantment rates"), &g_Config.fEnchantRates, 0.0f, 300.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp);
                    MakeTooltip(LZFormat("This is relative to the base enchantment rates, which are currently set between {:.0f}%% and {:.0f}%%.\n"
                                         "Other factors may modify this rate further.",
                                         100.0f * g_Config.enchChanceBase, 100.0f * (g_Config.enchChanceBase + g_Config.enchChanceBonusMax))
                                    .c_str());
                    ImGui::Checkbox(LZ("Randomize remaining charge on weapon enchantments"), &g_Config.bEnchantRandomCharge);
                    ImGui::Checkbox(LZ("Always add enchantments to staves"), &g_Config.bAlwaysEnchantStaves);

                    ImGui::SeparatorText(LZ("Preferred Variants"));
                    MakeTooltip(
                        LZ("When two items exist, one with the word and one without,\n"
                           "these settings will determine which will appear.\n\n"
                           "For example: Mod contains items 'Cape' and 'Cape (SMP)'\n"
                           "Setting SMP as preferred will cause only 'Cape (SMP)' to appear in loot."));

                    const char* prefDesc[] = {LZ("Don't care"), LZ("Prefer items with"), LZ("Prefer items without")};

                    ImGui::PushItemWidth(-FLT_MIN);
                    if (ImGui::BeginTable(LZ("Preference Variants"), 2, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingFixedFit)) {
                        ImGui::TableSetupColumn(LZ("Word"));
                        ImGui::TableSetupColumn(LZ("Options"));

                        for (auto& i : g_Config.mapPrefVariants) {
                            ImGui::TableNextColumn();
                            // ImGui::PushID(i.first.c_str());
                            ImGui::Text(i.first.c_str());

                            ImGui::TableNextColumn();
                            ImGui::SetNextItemWidth(200.0f);
                            ImGui::Combo("##PrefCombo", &i.second.pref, prefDesc, IM_ARRAYSIZE(prefDesc));
                            // ImGui::PopID();
                        }

                        ImGui::EndTable();
                    }

                    static TimedTooltip resp;
                    if (ImGui::Button(LZ("Rescan ALL modified items for preference words"))) {
                        auto r = RescanPreferenceVariants();
                        resp.Enable(LZFormat("{} matching items found", r));
                    }
                    if (!resp.Show())
                        MakeTooltip(
                            LZ("You need only press this when new words are added to the list above.\n"
                               "You do not need to rescan when simply changing preferences."));

                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem(LZ("Crafting"))) {
                    ImGui::Text(LZ("Only generate crafting recipes for"));
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
                    ImGui::Text(LZ("rarity and below"));

                    ImGui::Indent();
                    ImGui::Checkbox(LZ("Disable existing crafting recipies above that rarity"), &g_Config.bDisableCraftingRecipesOnRarity);
                    ImGui::Unindent();

                    ImGui::Checkbox(LZ("Keep crafting books as requirement"), &g_Config.bKeepCraftingBooks);

                    ImGui::Checkbox(LZ("Use recipes from as similar item if primary item is lacking"), &g_Config.bUseSecondaryRecipes);

                    ImGui::Checkbox(LZ("Show all recipe requirements"), &g_Config.bShowAllRecipeConditions);
                    MakeTooltip(
                        LZ("If checked, only requirements with multiples of the same type will be listed.\n"
                           "For example, a recipe that requires two seperate perks to craft."));

                    ImGui::Checkbox(LZ("Enable smelting recipes"), &g_Config.bEnableSmeltingRecipes);

                    ImGui::Text(LZ("Instead of free recipes, make recipes cost gold as a portion of the item's value:"));
                    ImGui::Indent();
                    ImGui::Text(LZ("Temper recipes"));
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    ImGui::SliderFloat("##TemperRatio", &g_Config.fTemperGoldCostRatio, 0.0f, 200.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp);
                    MakeTooltip(LZ("Set to 0%% to keep free"));
                    ImGui::Text(LZ("Craft recipes"));
                    ImGui::SameLine();
                    ImGui::SetNextItemWidth(-FLT_MIN);
                    ImGui::SliderFloat("##CraftRatio", &g_Config.fCraftGoldCostRatio, 0.0f, 200.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp);
                    MakeTooltip(LZ("Set to 0%% to keep free"));
                    ImGui::Unindent();

                    ImGui::Text(LZ("Exclude the following requirements for recipes (does not change materials):"));

                    ImGui::SetNextItemWidth(-FLT_MIN);
                    if (ImGui::BeginListBox("##RecipeConditions")) {
                        for (auto i : g_Data.recipeConditions) {
                            ImGui::PushID(i);
                            bool checked = g_Config.recipeConditionBlacklist.contains(i);
                            const char* name = i->GetName();
                            if (!name || name[0] == '\0') {
                                name = "<Unnamed>";
                            }
                            if (ImGui::Checkbox(name, &checked)) {
                                if (checked)
                                    g_Config.recipeConditionBlacklist.insert(i);
                                else
                                    g_Config.recipeConditionBlacklist.erase(i);
                            }
                            ImGui::PopID();
                        }

                        ImGui::EndListBox();
                    }

                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem(LZ("Permissions"))) {
                    if (ImGui::BeginTable(LZ("Permissions"), 2, ImGuiTableFlags_BordersInnerH | ImGuiTableFlags_SizingFixedFit | ImGuiTableColumnFlags_NoReorder)) {
                        ImGui::TableSetupColumn(LZ("Local Changes Permissions"));
                        ImGui::TableSetupColumn(LZ("Shared Changes Permissions"));

                        ImGui::TableHeadersRow();
                        ImGui::TableNextRow();

                        ImGui::TableNextColumn();
                        PermissionsChecklist(LZ("Local"), g_Config.permLocal);

                        ImGui::TableNextColumn();
                        PermissionsChecklist(LZ("Shared"), g_Config.permShared);

                        ImGui::EndTable();
                    }

                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem(LZ("Shortcuts"))) {
                    ImGui::Checkbox(LZ("Escape closes windows"), &g_Config.bShortcutEscCloseWindow);

                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem(LZ("Hooks"))) {
                    if (ImGui::Checkbox(LZ("Enale console command hook"), &g_Config.bEnableConsoleHook)) {
                        if (g_Config.bEnableConsoleHook) InstallConsoleCommands();
                    }

                    ImGui::Checkbox(LZ("Enable NIF armor slot remapping hook"), &g_Config.bEnableArmorSlotModelFixHook);
                    MakeTooltip(LZ("Fixes armor pieces not rendering when remapping their armor slots"));

                    ImGui::BeginDisabled(REL::Module::IsVR());
                    ImGui::Checkbox(LZ("Enable Skyrim warmth system hook"), &g_Config.bEnableSkyrimWarmthHook);
                    MakeTooltip(
                        LZ("Required to enable changes to item warmth.\n"
                           "WARNING: Likely to cause crashes on VR!"));
                    ImGui::EndDisabled();

                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem(LZ("Integrations"))) {
                    ImGui::SeparatorText(LZ("Base Object Swapper"));
                    ImGui::Checkbox(LZ("Automatically infer new containers from Base Object Swapper"), &g_Config.bEnableBOSDetect);
                    ImGui::BeginDisabled(!g_Config.bEnableBOSDetect);
                    ImGui::Indent();

                    ImGui::Checkbox(LZ("From generic swaps"), &g_Config.bEnableBOSFromGeneric);
                    ImGui::Checkbox(LZ("From conditional swaps"), &g_Config.bEnableBOSFromConditional);
                    ImGui::Checkbox(LZ("From specific reference swaps"), &g_Config.bEnableBOSFromReference);

                    ImGui::Unindent();
                    ImGui::EndDisabled();

                    ImGui::SeparatorText(LZ("Dynamic Armor Variants"));
                    ImGui::Checkbox(LZ("Enable exports to DAV"), &g_Config.bEnableDAVExports);
                    ImGui::SameLine();
                    ImGui::BeginDisabled(!g_Config.bEnableDAVExports);
                    ImGui::Checkbox(LZ("Even if DAV not present"), &g_Config.bEnableDAVExportsAlways);
                    if (ImGui::Button(LZ("Re-export all files to DAV"))) {
                        ExportAllToDAV();
                    }
                    ImGui::EndDisabled();
                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem(LZ("Outfits"))) {
                    ImGui::SeparatorText(LZ("Outfit Cycling"));

                    ImGui::SliderInt(LZ("Cycle timeout (ms)"), &g_Config.outfitCycleTimeout, 100, 3000, "%d", ImGuiSliderFlags_AlwaysClamp);
                    MakeTooltip(
                        LZ("Timeout in milliseconds before allowing another outfit cycle.\n"
                           "Increase this value if outfits are being skipped when using Numpad+/-.\n"
                           "Higher values are needed for outfits with many items or slower machines.\n"
                           "Default: 800ms"));

                    ImGui::SeparatorText(LZ("Performance"));

                    ImGui::SliderFloat(LZ("Frame time multiplier"), &g_Config.outfitFrameMultiplier, 1.0f, 5.0f, "%.1fx", ImGuiSliderFlags_AlwaysClamp);
                    MakeTooltip(
                        LZ("Multiplier for frame skipping during outfit operations.\n"
                           "Increase if items aren't fully equipped before creating/cycling outfits.\n"
                           "Lower values are faster but may cause issues on slower machines.\n"
                           "Higher values ensure all items equip but take longer.\n"
                           "Default: 2.0x"));

                    ImGui::SliderInt(LZ("Item list limit"), &g_Config.itemListLimit, 1000, 30000, "%d", ImGuiSliderFlags_AlwaysClamp);
                    MakeTooltip(
                        LZ("Maximum number of items to display in the general items list.\n"
                           "Higher values show more items but may impact performance.\n"
                           "Lower values improve performance but may hide items.\n"
                           "Default: 2000"));

                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem(LZ("Table Columns"))) {
                    ImGui::Text(LZ("Configure which columns are displayed in item tables and their order."));
                    ImGui::Separator();

                    ImGui::Text(LZ("Visible Columns:"));
                    for (int i = 0; i < Config::TableColumnConfig::Col_Count; i++) {
                        const int* displayOrder = g_Config.tableColumns.GetDisplayOrder();
                        int col = displayOrder[i];

                        ImGui::PushID(col);

                        // Checkbox for visibility
                        bool visible = g_Config.tableColumns.visible[col];
                        if (col == Config::TableColumnConfig::Col_Name) {
                            // Name column is always visible
                            ImGui::BeginDisabled(true);
                            ImGui::Checkbox(LZ(Config::TableColumnConfig::GetColumnName(col)), &visible);
                            ImGui::EndDisabled();
                            MakeTooltip(LZ("Name column is always visible"));
                        } else {
                            if (ImGui::Checkbox(LZ(Config::TableColumnConfig::GetColumnName(col)), &visible)) {
                                g_Config.tableColumns.visible[col] = visible;
                                g_Config.tableColumns.InvalidateCache();
                            }
                        }

                        // Move up/down buttons
                        ImGui::SameLine();
                        ImGui::BeginDisabled(i == 0);
                        if (ImGui::SmallButton("^")) {
                            // Swap order with previous column
                            int prevCol = displayOrder[i - 1];
                            std::swap(g_Config.tableColumns.order[col], g_Config.tableColumns.order[prevCol]);
                            g_Config.tableColumns.InvalidateCache();
                        }
                        ImGui::EndDisabled();

                        ImGui::SameLine();
                        ImGui::BeginDisabled(i == Config::TableColumnConfig::Col_Count - 1);
                        if (ImGui::SmallButton("v")) {
                            // Swap order with next column
                            int nextCol = displayOrder[i + 1];
                            std::swap(g_Config.tableColumns.order[col], g_Config.tableColumns.order[nextCol]);
                            g_Config.tableColumns.InvalidateCache();
                        }
                        ImGui::EndDisabled();

                        ImGui::PopID();
                    }

                    ImGui::Separator();
                    if (ImGui::Button(LZ("Reset to Default"))) {
                        // Reset to default column configuration
                        for (int i = 0; i < Config::TableColumnConfig::Col_Count; i++) {
                            g_Config.tableColumns.visible[i] = true;
                            g_Config.tableColumns.order[i] = i;
                        }
                        g_Config.tableColumns.InvalidateCache();
                    }

                    ImGui::EndTabItem();
                }

                if (ImGui::BeginTabItem(LZ("Help"))) {
                    auto vpSize = ImGui::GetMainViewport()->Size;
                    ImVec2 helpSize(std::min(48.0f * ImGui::GetFontSize(), 0.8f * vpSize.x), std::min(24.0f * ImGui::GetTextLineHeightWithSpacing(), 0.6f * vpSize.y));

                    if (ImGui::BeginChild("HelpScroll", helpSize, true)) {
                        ImGui::TextWrapped(
                            LZ("Shortcuts only apply while the Quick Armor Rebalance window has focus.\n"
                               "Item shortcuts act on the current selection unless noted otherwise."));

                        ImGui::SeparatorText(LZ("Window"));
                        if (HelpTable("HelpWindow")) {
                            HelpRow("qar", "Console command that opens this window. Papyrus can also call QuickArmorRebalance.Open()");
                            HelpRow("Escape", "Closes the window, or the dialog on top of it. Can be turned off under Shortcuts");
                            HelpRow("Numpad 9", "Collapses or expands the window");
                            HelpRow("Hold Middle Mouse", "Passes mouse and keyboard through to the game and unpauses while held, so you can move the camera without closing the window. Collapsing the window does the same");
                            ImGui::EndTable();
                        }

                        ImGui::SeparatorText(LZ("Mod selection"));
                        if (HelpTable("HelpMods")) {
                            HelpRow("[ / ]", "Selects the previous or next mod in the filtered list and equips its matching armor set");
                            HelpRow("Ctrl + Click", "Adds or removes a mod from the selection instead of replacing it");
                            HelpRow("Ctrl + Alt + Click", "On a mod that QAR has changed, prompts to delete all of its changes");
                            HelpRow("Ctrl + Alt + Right click", "Adds the mod to the blacklist");
                            ImGui::EndTable();
                        }

                        ImGui::SeparatorText(LZ("Item list"));
                        if (HelpTable("HelpItems")) {
                            HelpRow("Click", "Selects an item");
                            HelpRow("Ctrl + Click", "Adds or removes an item from the selection");
                            HelpRow("Shift + Click", "Selects everything between the last selected item and this one");
                            HelpRow("Double click", "Equips the item, or unequips it if already worn. Gives you a copy if you don't have one");
                            HelpRow("Alt + Double click", "Unequips everything, then equips the entire matching armor set");
                            HelpRow("Shift + Alt + Double click", "Selects the entire matching armor set. Hold Ctrl as well to add it to the current selection");
                            HelpRow("Up / Down", "Moves through the list. Hold Shift to extend the selection, Ctrl to keep the existing one");
                            HelpRow("Space", "Adds or removes the focused item from the selection");
                            HelpRow("Alt + Space", "Toggles the checkbox on all selected items, which controls whether Apply affects them");
                            HelpRow("Enter", "Equips or unequips every selected item");
                            HelpRow("F", "Toggles favorite on every selected item");
                            HelpRow("Delete", "Removes the selected items that QAR gave you from your inventory");
                            HelpRow("Backspace", "Removes the most recently given items");
                            HelpRow("Right click", "Opens the item list menu (give, equip, mark to keep, select, delete changes)");
                            ImGui::EndTable();
                        }

                        ImGui::SeparatorText(LZ("Item list - numpad"));
                        if (HelpTable("HelpItemsNumpad")) {
                            HelpRow("Numpad 4", "Equips or unequips the selected item. Narrows a multiple selection down to one item first");
                            HelpRow("Numpad 8", "Selects the previous item in the list and equips it, wrapping around at the top");
                            HelpRow("Numpad 5", "Selects the next item in the list and equips it, wrapping around at the bottom");
                            HelpRow("Numpad 2", "Picks a random item you aren't wearing out of the filtered list and equips it");
                            HelpRow("Numpad 6", "Toggles favorite on the selected item");
                            HelpRow("Numpad 3", "Toggles 'Mark to Keep', which protects an item from 'Remove All Tracked Items'");
                            HelpRow("Numpad 1", "Re-equips the outfit you last equipped");
                            ImGui::EndTable();
                        }

                        ImGui::SeparatorText(LZ("Outfits tab"));
                        if (HelpTable("HelpOutfits")) {
                            HelpRow("Double click outfit", "Unequips everything, then equips every item in that outfit");
                            HelpRow("Numpad + / Numpad -", "Moves to the next or previous outfit in the filtered list and equips it. The delay between cycles is set under Outfits");
                            HelpRow("F2", "Renames the selected outfit");
                            HelpRow("Delete", "Deletes the selected outfit");
                            HelpRow("Ctrl + C", "Creates a new outfit from what you are currently wearing, named from the prefix box");
                            HelpRow("Shift + Create Outfit", "Names the new outfit after the selected mod instead of the prefix box. Works with Shift + Ctrl + C too, and needs exactly one mod selected");
                            HelpRow("Ctrl + X", "Moves the cursor to the outfit name prefix box");
                            HelpRow("Right click", "Opens the outfit menu, including overwriting the selected outfit with what you are wearing");
                            HelpRow("Enter", "Confirms the Create Outfit, Rename Outfit, or Create Tag dialog");
                            ImGui::EndTable();
                        }

                        ImGui::SeparatorText(LZ("Outfit item list"));
                        if (HelpTable("HelpOutfitItems")) {
                            HelpRow("Double click", "Equips or unequips that piece, including its enchantment and stat changes if it has any");
                            HelpRow("Click / Ctrl / Shift", "Selects items the same way as the main item list");
                            HelpRow("F", "Toggles favorite on every selected item");
                            ImGui::EndTable();
                        }

                        ImGui::SeparatorText(LZ("Right click shortcuts"));
                        if (HelpTable("HelpRightClick")) {
                            HelpRow("Filter boxes and dropdowns", "Right click any filter to reset it");
                            HelpRow("Checkboxes with three states", "Right click to return them to the 'either' state");
                            HelpRow("Stat sliders", "Right click the Armor Rating, Weight, Damage, or Gold Value slider to switch between a percentage and a flat +/- amount");
                            ImGui::EndTable();
                        }
                    }
                    ImGui::EndChild();

                    ImGui::EndTabItem();
                }

                ImGui::EndTabBar();
            }

            ImGui::EndPopup();
        }
        if (!bPopupActive) {
            // Apply item blacklist changes when settings dialog closes
            if (bItemBlacklistChanged) {
                std::vector<std::string> lines;
                std::istringstream stream(bufItemBlacklist);
                std::string line;

                while (std::getline(stream, line)) {
                    // Trim whitespace
                    size_t start = line.find_first_not_of(" \t\n\r");
                    if (start == std::string::npos) continue;  // String is all whitespace
                    size_t end = line.find_last_not_of(" \t\n\r");
                    line = line.substr(start, end - start + 1);
                    lines.push_back(MakeLower(line));
                }

                g_Config.itemBlacklist = std::move(lines);
                g_filterRound++;  // Trigger filter rebuild
                bItemBlacklistChanged = false;
            }
            g_Config.Save();
        }

        if (popupRemapSlots) ImGui::OpenPopup(LZ("Remap Slots"));

        SetWindowSizeLimits(300, 500);
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        bPopupActive = true;
        if (ImGui::BeginPopupModal(LZ("Remap Slots"), &bPopupActive, ImGuiWindowFlags_NoScrollbar)) {
            if (g_Config.bShortcutEscCloseWindow && ImGui::Shortcut(ImGuiKey_Escape)) {
                bPopupActive = false;
                ImGui::CloseCurrentPopup();
            }

            static int nSlotView = 33;
            uint64_t slotsUsed = 0;
            ArmorSlots slotsProtected = g_Config.bEnableProtectedSlotRemapping ? 0 : kProtectedSlotMask;

            std::vector<RE::TESObjectARMO*> lsSlotItems;
            for (auto i : data.items) {
                if (auto armor = i->As<RE::TESObjectARMO>()) {
                    auto itemSlots = MapFindOr(g_Data.modifiedArmorSlots, armor, (ArmorSlots)armor->GetSlotMask().underlying());
                    slotsUsed |= itemSlots;
                    if (itemSlots & ((uint64_t)1 << nSlotView)) lsSlotItems.push_back(armor);
                }
            }

            nSlotView = 33;
            if (auto payload = ImGui::GetDragDropPayload()) {
                if (payload->IsDataType("ARMOR SLOT")) {
                    nSlotView = *(int*)payload->Data;
                }
            }

            ImGui::Text(LZ("Click and drag from the original slot on the left to the new replacement slot on the right"));
            ImGui::PushItemWidth(-FLT_MIN);

            if (ImGui::BeginTable(
                    "Slot Mapping", 4,
                    ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_PadOuterX | ImGuiTableFlags_PreciseWidths | ImGuiTableFlags_ScrollY)) {
                ImGui::TableSetupColumn(LZ("Original"));
                ImGui::TableSetupColumn(LZ("Items"), ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn(LZ("Remapped"));
                ImGui::TableSetupColumn(LZ("Cosmetic"));

                ImGui::TableHeadersRow();
                ImGui::TableNextRow();

                ImVec2 srcCenter[33];
                ImVec2 tarCenter[33];

                for (int i = 0; i < 33; i++) {
                    bool bProtected = i < 32 ? slotsProtected & (1 << i) : false;

                    ImGui::TableNextColumn();
                    if (i < 32) {
                        ImGui::BeginDisabled(bProtected || (((uint64_t)1 << i) & slotsUsed) == 0);
                        ImGui::BeginGroup();

                        const char* strWarn = nullptr;
                        int popCol = 0;
                        if ((((uint64_t)1 << i) & remappedSrc)) {
                            ImGui::PushStyleColor(ImGuiCol_Text, colorChanged);
                            popCol++;
                        } else if ((1ull << i) & slotsUsed & (remappedTar & ~remappedSrc)) {
                            ImGui::PushStyleColor(ImGuiCol_Text, colorDeleted);
                            strWarn =
                                LZ("Warning: Other items are being remapped to this slot.\n"
                                   "This will cause conflicts unless this slot is also remapped.");
                            popCol++;
                        } else if ((1ull << i) & slotsUsed & params.slotsCosmetic) {
                            ImGui::PushStyleColor(ImGuiCol_Text, colorChangedShared);
                            strWarn = LZ("Items in this slot will be made cosmetic.");
                            popCol++;
                        } else if ((((uint64_t)1 << i) & slotsUsed & ~(uint64_t)g_Config.usedSlotsMask)) {
                            ImGui::PushStyleColor(ImGuiCol_Text, colorDeleted);
                            strWarn = LZ("Warning: Items in this slot will not be changed unless remapped to another slot.");
                            popCol++;
                        }

                        bool bSelected = false;
                        if (ImGui::Selectable(strSlotDesc[i].c_str(), &bSelected, 0)) {
                        }

                        if (strWarn) MakeTooltip(strWarn);

                        if (ImGui::BeginDragDropSource(0)) {
                            nSlotView = i;
                            params.mapArmorSlots.erase(i);
                            ImGui::SetDragDropPayload("ARMOR SLOT", &i, sizeof(i));
                            ImGui::Text(strSlotDesc[i].c_str());
                            ImGui::EndDragDropSource();
                        }

                        ImGui::SameLine();

                        float w = ImGui::GetFontSize() * 1 + ImGui::GetStyle().FramePadding.x * 2;
                        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, ImGui::GetContentRegionAvail().x - w));
                        ImGui::SetCursorPosY(ImGui::GetCursorPosY() - 4.0f);  // Radio circles are weirdly offset down slightly?

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
                        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) nSlotView = i;

                        ImGui::EndDisabled();
                    }

                    ImGui::TableNextColumn();
                    if (i < lsSlotItems.size()) ImGui::Text(lsSlotItems[i]->GetName());

                    ImGui::TableNextColumn();
                    auto bDisabled = i != 32 && ((1 << i) & (g_Config.usedSlotsMask | params.slotsCosmetic)) == 0;
                    ImGui::BeginDisabled(bProtected || bDisabled);
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
                    } else if ((((uint64_t)1 << i) & params.slotsCosmetic)) {
                        ImGui::PushStyleColor(ImGuiCol_Text, colorChangedShared);
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
                            LZ("Warning: Items are being remapped to this slot, but other items are already using this "
                               "slot."));

                    if (!bProtected && (g_Config.bAllowInvalidRemap || !bDisabled) && ImGui::BeginDragDropTarget()) {
                        if (auto payload = ImGui::AcceptDragDropPayload("ARMOR SLOT")) {
                            params.mapArmorSlots[*(int*)payload->Data] = i;
                        }
                        ImGui::EndDragDropTarget();
                    }
                    ImGui::SameLine();
                    if (ImGui::RadioButton(strSlotDesc[i].c_str(), &bCheckbox)) {
                    }
                    tarCenter[i] = (ImGui::GetItemRectMin() + ImGui::GetItemRectMax()) / 2;
                    tarCenter[i].x = ImGui::GetItemRectMin().x + ImGui::GetItemRectMax().y - tarCenter[i].y;  // Want center of the circle
                    ImGui::PopID();
                    ImGui::PopID();

                    ImGui::PopStyleColor(popCol);
                    ImGui::EndGroup();

                    ImGui::EndDisabled();

                    ImGui::TableNextColumn();
                    if (i < 32) {
                        ImGui::PushID("Target");
                        ImGui::PushID(i);
                        bool bCosmetic = !!(params.slotsCosmetic & (1 << i));
                        if (ImGui::Checkbox("##Cosmetic", &bCosmetic)) {
                            if (bCosmetic) {
                                params.slotsCosmetic |= (1 << i);
                                if (isCtrlDown) g_Config.slotsDefaultCosmetic |= (1 << i);
                            } else {
                                params.slotsCosmetic &= ~(1 << i);
                                if (isCtrlDown) g_Config.slotsDefaultCosmetic &= ~(1 << i);
                            }
                        }
                        MakeTooltip(
                            LZ("Items with only cosmetic slots will have armor rating, value, and weight reduced to zero,\n"
                               "but still occupy an armor slot.\n\n"
                               "Holding Ctrl while clicking will also change the default setting for this slot."));

                        ImGui::PopID();
                        ImGui::PopID();
                    }

                    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) nSlotView = i;
                }

                ImGui::EndTable();

                constexpr auto lineWidth = 3.0f;
                auto draw = ImGui::GetWindowDrawList();

                if (auto payload = ImGui::GetDragDropPayload()) {
                    if (payload->IsDataType("ARMOR SLOT")) {
                        draw->AddLine(srcCenter[*(int*)payload->Data], ImGui::GetMousePos(), colorChanged, lineWidth);
                    }
                }

                for (auto i : params.mapArmorSlots) {
                    draw->AddLine(srcCenter[i.first], tarCenter[i.second], colorChanged, lineWidth);
                }
            }

            ImGui::EndPopup();
        }

        static WordSet wordsUsed;
        static WordSet wordsStatic;
        static std::map<const DynamicVariant*, std::vector<std::size_t>> mapDVWords;

        static bool bDVChanged = false;

        if (popupDynamicVariants) {
            ImGui::OpenPopup(LZ("Dynamic Variants"));

            static short nDVFilterRound = -1;
            if (nDVFilterRound != g_filterRound) {
                nDVFilterRound = g_filterRound;

                nShowWords = AnalyzeResults::eWords_StaticVariants;
                wordsStatic.clear();
                wordsUsed.clear();
                mapDVWords.clear();

                for (auto& dv : g_Config.mapDynamicVariants) {
                    if (!dv.second.autos.empty()) {
                        for (auto w : dv.second.autos) {
                            if (analyzeResults.mapWordItems.find(w) != analyzeResults.mapWordItems.end()) {
                                mapDVWords[&dv.second].push_back(w);
                                wordsUsed.insert(w);
                            }
                        }
                    }
                }

                auto& ws = analyzeResults.sets[AnalyzeResults::eWords_StaticVariants];
                wordsStatic.insert(ws.begin(), ws.end());

                wordsUsed.insert(wordsStatic.begin(), wordsStatic.end());
                for (auto& i : mapDVWords) {
                    wordsUsed.insert(i.second.begin(), i.second.end());
                }

                bDVChanged = true;
            }
        }

        SetWindowSizeLimits(450, 300);
        ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        bPopupActive = true;

        static bool dvWndWasOpen = false;

        if (ImGui::BeginPopupModal(LZ("Dynamic Variants"), &bPopupActive, ImGuiWindowFlags_NoScrollbar)) {
            if (g_Config.bShortcutEscCloseWindow && ImGui::Shortcut(ImGuiKey_Escape)) {
                bPopupActive = false;
                ImGui::CloseCurrentPopup();
            }

            dvWndWasOpen = true;
            ImGui::Text(LZ("Drag the appropriate words (if any) to their associated dynamic type on the right side."));

            static TimedTooltip resp;
            ImGui::BeginDisabled(!hasModifiedItems);
            if (ImGui::Button(RightAlign(LZ("Update Dynamic Variants")))) {
                data.dvSets = MapVariants(analyzeResults, mapDVWords);
                auto r = AddDynamicVariants(params);

                resp.Enable(LZFormat("{} dynamic variants added", r));
            }

            if (!resp.Show()) {
                if (!hasModifiedItems)
                    MakeTooltip(
                        LZ("This only updates previously modified items, but none have been detected.\n\n"
                           "Dynamic variants will be included when clicking Apply Changes after closing this window."));
            }

            ImGui::EndDisabled();

            ImGui::SetNextItemWidth(-FLT_MIN);

            struct DragDropWords {
                static void Source(std::size_t w) {
                    if (ImGui::BeginDragDropSource(0)) {
                        std::vector<std::size_t> words;
                        words.push_back(w);

                        ImGui::SetDragDropPayload("WORD HASHES", &words[0], words.size() * sizeof(words[0]));
                        ImGui::Text(analyzeResults.mapWordStrings[w].c_str());
                        ImGui::EndDragDropSource();
                    }
                }

                static void TargetCommon(const std::function<void(std::size_t)> fnInsert) {
                    if (ImGui::BeginDragDropTarget()) {
                        if (auto payload = ImGui::AcceptDragDropPayload("WORD HASHES")) {
                            auto* pWords = (std::size_t*)payload->Data;
                            int nWords = payload->DataSize / sizeof(*pWords);

                            for (int i = 0; i < nWords; i++) {
                                auto w = *pWords++;
                                wordsStatic.erase(w);
                                for (auto& dv : mapDVWords) {
                                    auto it = std::find(dv.second.begin(), dv.second.end(), w);
                                    if (it != dv.second.end()) dv.second.erase(it);
                                }
                                wordsUsed.erase(w);

                                fnInsert(w);
                            }

                            bDVChanged = true;
                        }
                        ImGui::EndDragDropTarget();
                    }
                }

                static void Target(WordSet* pSet) {
                    TargetCommon([&](std::size_t w) {
                        if (pSet) {
                            pSet->insert(w);
                            wordsUsed.insert(w);
                        }
                    });
                }

                static void Target(std::vector<std::size_t>* pSet, std::size_t at) {
                    TargetCommon([&](std::size_t w) {
                        if (pSet) {
                            pSet->insert(at ? std::find(pSet->begin(), pSet->end(), at) : pSet->begin(), w);
                            wordsUsed.insert(w);
                        }
                    });
                }
            };

            if (ImGui::BeginTable("WindowTable", 4, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingFixedSame | ImGuiTableFlags_ScrollY,
                                  ImGui::GetContentRegionAvail())) {
                ImGui::TableSetupColumn(LZ("Static Variants"));
                ImGui::TableSetupColumn(LZ("Unassigned"));
                ImGui::TableSetupColumn(LZ("Dynamic Variants"));
                ImGui::TableSetupColumn(LZ("Output Variant Sets"), ImGuiTableColumnFlags_WidthStretch);

                ImGui::TableNextRow(ImGuiTableRowFlags_Headers);
                ImGui::TableNextColumn();
                ImGui::TableHeader(LZ("Static Variants"));
                MakeTooltip(
                    LZ("Static Variants are similar but distinct items.\n"
                       "Usually colors or different versions of an item piece.\n\n"
                       "The accurracy of words in this table is not important."));

                ImGui::TableNextColumn();
                ImGui::TableHeader(LZ("Unassigned"));

                ImGui::TableNextColumn();
                ImGui::TableHeader(LZ("Dynamic Variants"));
                MakeTooltip(
                    LZ("Dynamic Variants are the same item in different forms.\n\n"
                       "Place associated words under the associated variant type.\n"
                       "If there are multiple, they should be ordered that the most default is at the top and the most "
                       "changed at the bottom."));

                ImGui::TableNextColumn();
                ImGui::TableHeader(LZ("Output Variant Sets"));

                ImGui::TableNextRow();
                ImGui::TableNextColumn();

                if (ImGui::BeginListBox("##StaticWords", ImGui::GetContentRegionAvail())) {
                    for (auto w : wordsStatic) {
                        bool selected = false;
                        ImGui::Selectable(analyzeResults.mapWordStrings[w].c_str(), selected);
                        MakeTooltip(analyzeResults.mapWordItems[w].strItemList.c_str(), true);

                        DragDropWords::Source(w);
                    }

                    ImGui::EndListBox();
                    DragDropWords::Target(&wordsStatic);
                }

                ImGui::TableNextColumn();
                if (ImGui::BeginListBox("##UnsetWords", ImGui::GetContentRegionAvail())) {
                    for (int i = 0; i <= nShowWords; i++) {
                        for (auto w : analyzeResults.sets[i]) {
                            if (wordsUsed.contains(w)) continue;

                            bool selected = false;
                            ImGui::Selectable(analyzeResults.mapWordStrings[w].c_str(), selected);
                            MakeTooltip(analyzeResults.mapWordItems[w].strItemList.c_str(), true);

                            DragDropWords::Source(w);
                        }
                    }

                    ImGui::Separator();

                    ImGui::PushItemWidth(ImGui::GetContentRegionAvail().x);
                    if (ImGui::Button(LZ("Show More Words"))) {
                        nShowWords = std::min(nShowWords + 1, AnalyzeResults::eWords_Count - 1);
                    }

                    if (ImGui::Button(LZ("Show Less Words"))) {
                        nShowWords = std::max(nShowWords - 1, (int)AnalyzeResults::eWords_StaticVariants);
                    }
                    ImGui::PopItemWidth();
                    ImGui::EndListBox();

                    DragDropWords::Target(nullptr);
                }

                ImGui::TableNextColumn();
                for (const auto& dv : g_Config.mapDynamicVariants) {
                    if (ImGui::CollapsingHeader(dv.first.c_str(), ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_Bullet)) {
                        DragDropWords::Target(&mapDVWords[&dv.second], 0);

                        auto copy = mapDVWords[&dv.second];  // Drag drop can modify
                        for (auto it = copy.begin(); it != copy.end(); it++) {
                            // bool selected = false
                            auto w = *it;
                            if (ImGui::TreeNodeEx(analyzeResults.mapWordStrings[w].c_str(), ImGuiTreeNodeFlags_Leaf)) {
                                MakeTooltip(analyzeResults.mapWordItems[w].strItemList.c_str(), true);

                                DragDropWords::Source(w);
                                DragDropWords::Target(&mapDVWords[&dv.second], w);

                                ImGui::TreePop();
                            }
                        }
                    }
                }

                if (bDVChanged) data.dvSets = MapVariants(analyzeResults, mapDVWords);

                ImGui::TableNextColumn();

                ImGui::BeginChild("OutputTree", ImGui::GetContentRegionAvail());

                for (const auto& set : data.dvSets) {
                    if (set.second.empty()) continue;

                    if (ImGui::TreeNodeEx(set.first->name.c_str(), ImGuiTreeNodeFlags_DefaultOpen)) {
                        for (const auto& i : set.second) {
                            if (ImGui::TreeNodeEx(i.second[0]->GetName(), ImGuiTreeNodeFlags_DefaultOpen)) {
                                for (int j = 1; j < i.second.size(); j++)
                                    if (ImGui::TreeNodeEx(i.second[j]->GetName(), ImGuiTreeNodeFlags_Leaf)) {
                                        ImGui::TreePop();
                                    }
                                ImGui::TreePop();
                            }
                        }
                        ImGui::TreePop();
                    }
                }

                ImGui::EndChild();

                ImGui::EndTable();
            }

            ImGui::EndPopup();
        } else {
            if (dvWndWasOpen) {
                dvWndWasOpen = false;
            }
        }

        using ItemGroup = std::map<std::string, std::vector<RE::TESBoundObject*>>;
        static std::vector<std::pair<std::string, ItemGroup>> itemCats;
        static std::unordered_set<RE::TESBoundObject*> selected;

        static RE::TESBoundObject* itemClickedLast = nullptr;
        static GivenItems itemsCustomTemp;

        static char filenameKIDExport[200] = "";
        static char filenameSkypatcherExport[200] = "";

        static std::size_t nMaxCatRows = 0;

        enum { eCatArmor0 = 0, eCatWeapons = eCatArmor0 + 32, eCatAmmo, eCatArmorNoSlots, eCatCount };
        if (popupCustomKeywords) {
            static bool bInit = false;
            if (!bInit) {
                bInit = true;

                std::map<RE::TESFile*, std::map<RE::BGSKeyword*, uint64_t>> mapKeywordDist;

                auto dataHandler = RE::TESDataHandler::GetSingleton();

                for (auto i : dataHandler->GetFormArray<RE::TESObjectARMO>()) {
                    auto file = i->GetFile(0);
                    if (!file) continue;  // Skip dynamic items without source file
                    for (unsigned int kw = 0; kw < i->numKeywords; kw++) {
                        auto slots = (ArmorSlots)i->GetSlotMask().underlying();
                        if (slots)
                            mapKeywordDist[file][i->keywords[kw]] |= slots;
                        else
                            mapKeywordDist[file][i->keywords[kw]] |= (1ull << eCatArmorNoSlots);
                    }
                }

                for (auto i : dataHandler->GetFormArray<RE::TESObjectWEAP>()) {
                    auto file = i->GetFile(0);
                    if (!file) continue;  // Skip dynamic items without source file
                    for (unsigned int kw = 0; kw < i->numKeywords; kw++) {
                        mapKeywordDist[file][i->keywords[kw]] |= (1ull << eCatWeapons);
                    }
                }

                for (auto ammo : dataHandler->GetFormArray<RE::TESAmmo>()) {
                    auto file = ammo->GetFile(0);
                    if (!file) continue;  // Skip dynamic items without source file
                    auto i = ammo->AsKeywordForm();
                    for (unsigned int kw = 0; kw < i->numKeywords; kw++) {
                        mapKeywordDist[file][i->keywords[kw]] |= (1ull << eCatAmmo);
                    }
                }

                struct KWData {
                    int count = 0;
                    int appearance[eCatCount] = {0};
                };

                std::map<RE::BGSKeyword*, KWData> kwTotals;

                for (auto& i : mapKeywordDist) {
                    for (auto& kw : i.second) {
                        auto& kwdata = kwTotals[kw.first];
                        kwdata.count++;

                        auto slots = kw.second;
                        while (slots) {
                            unsigned long slot;
                            _BitScanForward64(&slot, slots);
                            slots &= slots - 1;
                            kwdata.appearance[slot]++;
                        }
                    }
                }

                for (auto& i : kwTotals) {
                    if (!i.second.count) continue;

                    auto it = g_Config.mapCustomKWs.find(i.first);
                    if (it != g_Config.mapCustomKWs.end()) {
                        auto& ckw = it->second;
                        auto min = std::max(5, i.second.count / 10);
                        for (int slot = 0; slot < eCatCount; slot++) {
                            if (i.second.appearance[slot] >= min) ckw.commonSlots |= (1ull << slot);
                        }
                    }
                }
            }

            ImGui::OpenPopup(LZ("Custom Keywords"));

            itemClickedLast = nullptr;

            itemCats.clear();
            itemCats.resize(eCatCount);

            for (int i = 0; i < 32; i++) {
                itemCats[eCatArmor0 + i].first = strSlotDesc[i];
            }
            itemCats[eCatArmorNoSlots].first = LZ("Armor - Unassigned slot");
            itemCats[eCatWeapons].first = LZ("Weapons");
            itemCats[eCatAmmo].first = LZ("Ammo");

            auto itemGroups = GroupItems(data.items, analyzeResults);
            for (auto& i : itemGroups) {
                auto item = i.second[0];
                ItemGroup* pGroup = nullptr;
                if (auto armor = item->As<RE::TESObjectARMO>()) {
                    auto slots = (ArmorSlots)armor->GetSlotMask().underlying();
                    if (slots)
                        pGroup = &itemCats[eCatArmor0 + GetSlotIndex(slots)].second;
                    else
                        pGroup = &itemCats[eCatArmorNoSlots].second;
                } else if (auto weapon = item->As<RE::TESObjectWEAP>()) {
                    pGroup = &itemCats[eCatWeapons].second;
                } else if (auto ammo = item->As<RE::TESAmmo>()) {
                    pGroup = &itemCats[eCatAmmo].second;
                }

                if (pGroup) pGroup->emplace(std::move(i.first), std::move(i.second));
            }

            nMaxCatRows = 0;
            for (auto& i : itemCats) nMaxCatRows = std::max(nMaxCatRows, i.second.size());

            selected.clear();

            if (params.mapKeywordChanges.empty()) params.mapKeywordChanges = LoadKeywordChanges(params);

            filenameKIDExport[0] = '\0';
            if (!data.filteredItems.empty()) {
                auto firstFile = data.filteredItems[0]->GetFile(0);
                const char* firstFileName = firstFile ? firstFile->fileName : "Dynamic";
                strncpy(filenameKIDExport, std::format("{} QAR_Export", firstFileName).c_str(), sizeof(filenameKIDExport) - 1);
                filenameKIDExport[sizeof(filenameKIDExport) - 1] = '\0';

                strncpy(filenameSkypatcherExport, std::format("QAR Keyword Export\\{}", firstFileName).c_str(), sizeof(filenameSkypatcherExport) - 1);
                filenameSkypatcherExport[sizeof(filenameSkypatcherExport) - 1] = '\0';
            }
        }

        SetWindowSizeLimits(450, 300);
        // ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
        bPopupActive = true;

        static bool bPopupKeywordsWasOpen = false;

        if (ImGui::BeginPopupModal(LZ("Custom Keywords"), &bPopupActive, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_MenuBar)) {
            if (g_Config.bShortcutEscCloseWindow && ImGui::Shortcut(ImGuiKey_Escape)) {
                bPopupActive = false;
                ImGui::CloseCurrentPopup();
            }

            bPopupKeywordsWasOpen = true;
            itemsCustomTemp.recentEquipSlots = 0;

            static float fMaxKWSize = 0;

            if (ImGui::BeginMenuBar()) {
                if (ImGui::BeginMenu(LZ("Import Keywords"))) {
                    static RE::TESFile* curKWFile = nullptr;
                    static std::map<RE::TESFile*, std::vector<RE::BGSKeyword*>> mapFileKeywords;
                    static std::vector<RE::TESFile*> lsSortedFiles;
                    static std::unordered_map<RE::BGSKeyword*, bool> mapEnabledKWs;

                    if (mapFileKeywords.empty()) {
                        auto dataHandler = RE::TESDataHandler::GetSingleton();
                        for (auto kw : dataHandler->GetFormArray<RE::BGSKeyword>()) {
                            mapFileKeywords[kw->GetFile(0)].push_back(kw);
                        }

                        mapFileKeywords.erase(nullptr);  // Discard dynamic keywords

                        for (auto& ls : mapFileKeywords) {
                            std::sort(ls.second.begin(), ls.second.end(),
                                      [](RE::BGSKeyword* const a, RE::BGSKeyword* const b) { return _stricmp(a->formEditorID.c_str(), b->formEditorID.c_str()) < 0; });

                            lsSortedFiles.push_back(ls.first);
                        }

                        std::sort(lsSortedFiles.begin(), lsSortedFiles.end(), [](RE::TESFile* const a, RE::TESFile* const b) { return _stricmp(a->fileName, b->fileName) < 0; });

                        for (auto& i : g_Config.mapCustomKWs) {
                            mapEnabledKWs[i.first] = true;
                        }
                    }

                    ImGui::SetNextItemWidth(300.0f);
                    if (ImGui::BeginCombo("##Mod", curKWFile ? curKWFile->fileName : LZ("Select a mod"), ImGuiComboFlags_HeightLarge)) {
                        for (auto file : lsSortedFiles) {
                            if (ImGui::Selectable(file->fileName, file == curKWFile)) {
                                curKWFile = file;
                            }
                        }

                        ImGui::EndCombo();
                    }

                    ImGui::Text(LZ("Tab (optional)"));
                    ImGui::SameLine();
                    static char strTab[64] = "";
                    ImGui::SetNextItemWidth(150.0f);
                    ImGui::InputText("##TabName", strTab, sizeof(strTab) - 1);

                    if (ImGui::BeginListBox("##KeywordList", ImVec2(300.0f, 500.0f))) {
                        if (curKWFile) {
                            if (ImGui::BeginPopupContextWindow()) {
                                if (ImGui::Selectable(LZ("Enable all"))) {
                                    for (auto kw : mapFileKeywords[curKWFile]) mapEnabledKWs[kw] = true;
                                }
                                if (ImGui::Selectable(LZ("Disable all"))) {
                                    for (auto kw : mapFileKeywords[curKWFile]) mapEnabledKWs[kw] = false;
                                }
                                ImGui::EndPopup();
                            }

                            for (auto kw : mapFileKeywords[curKWFile]) {
                                if (ImGui::Checkbox(kw->formEditorID.c_str(), &mapEnabledKWs[kw])) {
                                }
                            }
                        }

                        ImGui::EndListBox();
                    }

                    static TimedTooltip resp;
                    if (ImGui::Button(RightAlign(LZ("Import")))) {
                        if (curKWFile) {
                            std::set<RE::BGSKeyword*> kws;

                            // auto& tab = mapKWTabs[strTab];
                            for (auto kw : mapFileKeywords[curKWFile]) {
                                if (mapEnabledKWs[kw]) {
                                    kws.insert(kw);
                                    fMaxKWSize = std::max(fMaxKWSize, 25.0f + ImGui::CalcTextSize(kw->formEditorID.c_str()).x);
                                }
                            }

                            ImportKeywords(curKWFile, strTab, kws);
                            resp.Enable(LZ("Keywords imported."));
                        }
                    }
                    if (!resp.Show())
                        MakeTooltip(
                            LZ("Creates a basic config file for the selected keywords.\n"
                               "Advanced configuration is available by editing the generated file directly."));

                    ImGui::EndMenu();
                }

                if (ImGui::BeginMenu(LZ("Export Changes"))) {
                    if (ImGui::BeginMenu(LZ("to Keyword Item Distributor (KID)"))) {
                        ImGui::Text(LZ("This will export all keyword additions for currently displayed items to KID."));
                        ImGui::Text(LZ("Note: KID only supports the addition of keywords, not any removals."));

                        ImGui::Text(LZ("File name:"));
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(340.0f);
                        ImGui::InputText("##Filename", filenameKIDExport, sizeof(filenameKIDExport));
                        ImGui::SameLine();
                        ImGui::Text("_KID.ini");

                        std::string filename(std::format("{}_KID.ini", filenameKIDExport));
                        std::filesystem::path path = std::filesystem::current_path() / "Data" / filename;

                        if (std::filesystem::exists(path)) {
                            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 255, 0, 255));
                            ImGui::Text(LZ("Warning: File exists, contents will be overwritten."));
                            ImGui::Spacing();
                            ImGui::PopStyleColor();
                        }

                        ImGui::BeginDisabled(!*filenameKIDExport);
                        static TimedTooltip resp;
                        if (ImGui::Button(RightAlign(LZ("Export Changes##Button")))) {
                            if (ExportToKID(data.filteredItems, params.mapKeywordChanges, path))
                                resp.Enable(LZ("Exported succesfully"));
                            else
                                resp.Enable(LZFormat("Could not open file to write {}:\n\n{}", path.generic_string(), std::strerror(errno)));
                        }
                        resp.Show();
                        ImGui::EndDisabled();

                        ImGui::EndMenu();
                    }
                    if (ImGui::BeginMenu(LZ("to Skypatcher"))) {
                        ImGui::Text(LZ("This will create multiple file(s) for SkyPatcher to use."));
                        ImGui::Text(LZ("It is recommended to export to a subdirectory."));
                        ImGui::Text(LZ("File name(s):"));
                        ImGui::SameLine();
                        ImGui::SetNextItemWidth(420.0f);
                        ImGui::InputText("##Filename", filenameSkypatcherExport, sizeof(filenameSkypatcherExport));
                        ImGui::SameLine();
                        ImGui::Text(".ini");

                        std::filesystem::path filename(std::format("{}.ini", filenameSkypatcherExport));
                        std::filesystem::path pathBase = std::filesystem::current_path() / "Data/SKSE/Plugins/SkyPatcher/";

                        if (std::filesystem::exists(pathBase / "armor" / filename)) {
                            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 255, 0, 255));
                            ImGui::Text(LZ("Warning: Armor file exists, contents may be overwritten."));
                            ImGui::Spacing();
                            ImGui::PopStyleColor();
                        }
                        if (std::filesystem::exists(pathBase / "weapon" / filename)) {
                            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 255, 0, 255));
                            ImGui::Text(LZ("Warning: Weapon file exists, contents may be overwritten."));
                            ImGui::Spacing();
                            ImGui::PopStyleColor();
                        }
                        if (std::filesystem::exists(pathBase / "ammo" / filename)) {
                            ImGui::PushStyleColor(ImGuiCol_Text, IM_COL32(255, 255, 0, 255));
                            ImGui::Text(LZ("Warning: Ammo file exists, contents may be overwritten."));
                            ImGui::Spacing();
                            ImGui::PopStyleColor();
                        }

                        ImGui::BeginDisabled(!*filenameSkypatcherExport);
                        static TimedTooltip resp;
                        if (ImGui::Button(RightAlign(LZ("Export Changes##Button")))) {
                            if (ExportToSkypatcher(data.filteredItems, params.mapKeywordChanges, filename))
                                resp.Enable(LZ("Exported succesfully"));
                            else
                                resp.Enable(LZ("Unable to export to Skypatcher, see logs for details"));
                        }
                        resp.Show();
                        ImGui::EndDisabled();

                        ImGui::EndMenu();
                    }

                    ImGui::EndMenu();
                }

                if (ImGui::BeginMenu(LZ("Options"))) {
                    ImGui::MenuItem(LZ("Show Types and Slots"), nullptr, &g_Config.bShowKeywordSlots);
                    ImGui::MenuItem(LZ("Reorder keywords based on relevance"), nullptr, &g_Config.bReorderKeywordsForRelevance);
                    if (ImGui::MenuItem(LZ("Equip example items"), nullptr, &g_Config.bEquipPreviewForKeywords)) {
                        if (!g_Config.bEquipPreviewForKeywords) {
                            itemsCustomTemp.Pop(true);
                            itemsCustomTemp.Restore();
                        }
                    }

                    ImGui::EndMenu();
                }

                ImGui::EndMenuBar();
            }

            static TimedTooltip resp;
            if (ImGui::Button(RightAlign(LZ("Save changes")))) {
                MakeKeywordChanges(params, true);
                resp.Enable(LZ("Changes saved"));
            }
            if (!resp.Show()) MakeTooltip(LZ("Keyword changes are also saved with the Apply Changes button."));

            if (ImGui::BeginTable("WindowTable", 2, ImGuiTableFlags_Resizable | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchProp,
                                  ImGui::GetContentRegionAvail())) {
                ImGui::TableSetupColumn(LZ("Items"), 0, 100.0f);
                ImGui::TableSetupColumn(LZ("Keywords"), ImGuiTableColumnFlags_WidthStretch, 300.0f);

                ImGui::TableNextRow();
                ImGui::TableNextColumn();

                if (ImGui::BeginChild("ItemTree")) {
                    static bool bSkipHeaders;
                    static ArmorChangeParams* pParams;

                    pParams = &params;
                    bSkipHeaders = !g_Config.bShowKeywordSlots || nMaxCatRows < 2;
                    struct ItemTree {
                        bool IsItemChanged(RE::TESBoundObject* item) {
                            KeywordChangeMap& mapChanges = pParams->mapKeywordChanges;
                            for (auto& kwc : mapChanges) {
                                if (kwc.second.add.contains(item) || kwc.second.remove.contains(item)) return true;
                            }
                            return false;
                        }

                        bool IsAllSelected(const ItemGroup::value_type& group) {
                            for (auto i : group.second) {
                                if (!selected.contains(i)) {
                                    return false;
                                }
                            }

                            return true;
                        }

                        void ItemLeaf(RE::TESBoundObject* item) {
                            int nPopColor = 0;
                            if (IsItemChanged(item)) {
                                ImGui::PushStyleColor(ImGuiCol_Text, colorChanged);
                                nPopColor++;
                            }
                            auto bOpen = ImGui::TreeNodeEx(item->GetName(), (selected.contains(item) ? ImGuiTreeNodeFlags_Selected : 0) | ImGuiTreeNodeFlags_Leaf);

                            if (ImGui::IsItemClicked()) {
                                itemClicked = item;
                            }

                            ImGui::PopStyleColor(nPopColor);

                            if (bOpen) {
                                ImGui::TreePop();
                            }
                        }

                        void ItemGroup(const ItemGroup::value_type& group) {
                            bool bAllChanged = true;
                            bool bAnyChanged = false;

                            for (auto i : group.second) {
                                if (IsItemChanged(i)) {
                                    bAnyChanged = true;
                                    continue;
                                }
                                bAllChanged = false;
                            }

                            int nPopColor = 0;
                            if (bAllChanged) {
                                ImGui::PushStyleColor(ImGuiCol_Text, colorChanged);
                                nPopColor++;
                            } else if (bAnyChanged) {
                                ImGui::PushStyleColor(ImGuiCol_Text, colorChangedPartial);
                                nPopColor++;
                            }

                            auto bOpen = ImGui::TreeNodeEx(group.first.c_str(), (IsAllSelected(group) ? ImGuiTreeNodeFlags_Selected : 0) | ImGuiTreeNodeFlags_OpenOnArrow);

                            if (ImGui::IsItemClicked() && !ImGui::IsItemToggledOpen()) {
                                groupClicked = &group;
                            }

                            ImGui::PopStyleColor(nPopColor);

                            if (bOpen) {
                                for (auto i : group.second) {
                                    ItemLeaf(i);
                                }
                                ImGui::TreePop();
                            }
                        }

                        void Build() {
                            for (auto& cat : itemCats) {
                                if (cat.second.empty()) continue;

                                if (bSkipHeaders || ImGui::CollapsingHeader(cat.first.c_str(), ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_DefaultOpen)) {
                                    for (auto& i : cat.second) {
                                        if (i.second.size() > 1) {
                                            ItemGroup(i);
                                        } else {
                                            ItemLeaf(i.second[0]);
                                        }
                                    }
                                }
                            }
                        }

                        bool HandleSelection() {
                            if (!itemClicked && !groupClicked) return false;

                            const bool isShiftDown = ImGui::IsKeyDown(ImGuiKey_LeftShift) || ImGui::IsKeyDown(ImGuiKey_RightShift);
                            const bool isCtrlDown = ImGui::IsKeyDown(ImGuiKey_LeftCtrl) || ImGui::IsKeyDown(ImGuiKey_RightCtrl);
                            // const bool isAltDown = ImGui::IsKeyDown(ImGuiKey_LeftAlt) || ImGui::IsKeyDown(ImGuiKey_RightAlt);

                            if (!isCtrlDown) selected.clear();

                            if (!isShiftDown) {
                                if (itemClicked) {
                                    if (!selected.contains(itemClicked)) {
                                        selected.insert(itemClicked);
                                        itemClickedLast = itemClicked;
                                    } else
                                        selected.erase(itemClicked);
                                } else if (groupClicked) {
                                    if (!IsAllSelected(*groupClicked)) {
                                        for (auto i : groupClicked->second) selected.insert(i);

                                        itemClickedLast = groupClicked->second[0];
                                    } else {
                                        for (auto i : groupClicked->second) selected.erase(i);
                                    }
                                }
                            } else {
                                if (groupClicked) itemClicked = groupClicked->second[0];

                                bool bSelecting = false;
                                bool bDone = false;
                                for (auto& cat : itemCats) {
                                    for (auto& i : cat.second) {
                                        for (auto item : i.second) {
                                            if (item == itemClicked || item == itemClickedLast) {
                                                if (!bSelecting) {
                                                    bSelecting = true;
                                                } else {
                                                    bDone = true;
                                                    if (!groupClicked) {  // Need to distinguish between clicking on the group and clicking on the first item
                                                        selected.insert(item);
                                                        break;
                                                    }
                                                }
                                            }

                                            if (bSelecting) selected.insert(item);
                                        }
                                        if (bDone) break;
                                    }
                                    if (bDone) break;
                                }
                            }

                            return true;
                        }

                        RE::TESBoundObject* itemClicked = nullptr;
                        const ItemGroup::value_type* groupClicked = nullptr;
                    };

                    ItemTree itemTree;
                    itemTree.Build();
                    if (itemTree.HandleSelection()) {
                        if (g_Config.bEquipPreviewForKeywords) {
                            if (itemsCustomTemp.stored.empty()) itemsCustomTemp.UnequipCurrent();

                            itemsCustomTemp.Pop(true);
                            for (auto i : selected) {
                                if (auto armor = i->As<RE::TESObjectARMO>()) {
                                    if ((itemsCustomTemp.recentEquipSlots & (ArmorSlots)armor->GetSlotMask().underlying()) == 0) itemsCustomTemp.Give(armor, true);
                                }
                            }
                        }
                    }
                }
                ImGui::EndChild();

                ImGui::TableNextColumn();

                if (g_Config.mapCustomKWTabs.empty())
                    ImGui::Text(LZ("No keywords, import some to begin."));
                else {
                    if (fMaxKWSize == 0) {
                        for (auto& tab : g_Config.mapCustomKWTabs) {
                            for (auto i : tab.second) {
                                fMaxKWSize = std::max(fMaxKWSize, 25.0f + ImGui::CalcTextSize(g_Config.mapCustomKWs[i].name.c_str()).x);
                            }
                        }
                    }

                    uint64_t slotsUsed = 0;
                    for (auto i : selected) {
                        if (auto armor = i->As<RE::TESObjectARMO>()) {
                            auto slots = (ArmorSlots)armor->GetSlotMask().underlying();
                            if (slots)
                                slotsUsed |= slots;
                            else
                                slotsUsed |= (1ull << eCatArmorNoSlots);
                        } else if (auto weapon = i->As<RE::TESObjectWEAP>())
                            slotsUsed |= (1ull << eCatWeapons);
                        else if (auto ammo = i->As<RE::TESAmmo>())
                            slotsUsed |= (1ull << eCatAmmo);
                    }

                    if (ImGui::BeginTabBar("##KWTabs")) {
                        for (auto& tab : g_Config.mapCustomKWTabs) {
                            if (ImGui::BeginTabItem(tab.first.empty() ? LZ("General") : tab.first.c_str())) {
                                int nCols = std::clamp((int)(ImGui::GetContentRegionAvail().x / fMaxKWSize), 1, 8);

                                if (ImGui::BeginTable("Keywords Table", nCols, ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_SizingStretchSame | ImGuiTableFlags_ScrollY,
                                                      ImGui::GetContentRegionAvail())) {
                                    ImGui::TableNextRow();

                                    ImGui::BeginDisabled(selected.empty());

                                    for (int priority = g_Config.bReorderKeywordsForRelevance ? 1 : 0; priority >= 0; priority--) {
                                        bool bUsed = false;
                                        KeywordChangeMap& mapChanges = params.mapKeywordChanges;
                                        for (int i = 0; i < tab.second.size(); i++) {
                                            auto& ckw = g_Config.mapCustomKWs[tab.second[i]];
                                            if (g_Config.bReorderKeywordsForRelevance ? (!!priority == !!(ckw.commonSlots & slotsUsed)) : !priority) {
                                                bUsed = true;
                                                ImGui::TableNextColumn();

                                                auto& changes = mapChanges[ckw.kw];

                                                int nChanges = 0;
                                                int nState = (1 << 1);
                                                if (selected.empty())
                                                    nState = 0;
                                                else {
                                                    for (auto item : selected) {
                                                        if (auto form = item->As<RE::BGSKeywordForm>()) {
                                                            if (changes.add.contains(item)) {
                                                                nState |= (1 << 0);
                                                                nChanges |= (1 << 0);
                                                            } else if (changes.remove.contains(item)) {
                                                                nState &= ~(1 << 1);
                                                                nChanges |= (1 << 1);
                                                            } else if (form->HasKeyword(ckw.kw)) {
                                                                nState |= (1 << 0);
                                                            } else
                                                                nState &= ~(1 << 1);
                                                            if (nState == (1 << 0)) break;
                                                        }
                                                    }
                                                }

                                                int nPopColor = 0;

                                                constexpr ImU32 colChange[] = {IM_COL32(255, 255, 255, 255), IM_COL32(0, 255, 0, 255), IM_COL32(255, 0, 0, 255),
                                                                               IM_COL32(255, 255, 0, 255)};
                                                if (nChanges) {
                                                    nPopColor++;
                                                    ImGui::PushStyleColor(ImGuiCol_Text, colChange[nChanges]);
                                                }

                                                if (ImGui::CheckboxFlags(ckw.name.c_str(), &nState, 3)) {
                                                    static void (*RemoveKW)(KeywordChangeMap&, RE::TESBoundObject*, const CustomKeyword&,
                                                                            std::set<RE::BGSKeyword*>&) = [](KeywordChangeMap& changeMap, RE::TESBoundObject* item,
                                                                                                             const CustomKeyword& ckw, std::set<RE::BGSKeyword*>& touched) -> void {
                                                        if (touched.contains(ckw.kw)) return;
                                                        touched.insert(ckw.kw);

                                                        auto& changes = changeMap[ckw.kw];
                                                        if (changes.add.contains(item))
                                                            changes.add.erase(item);
                                                        else {
                                                            if (item->As<RE::BGSKeywordForm>()->HasKeyword(ckw.kw)) changes.remove.insert(item);
                                                        }
                                                    };

                                                    static void (*AddKW)(KeywordChangeMap&, RE::TESBoundObject*, const CustomKeyword&,
                                                                         std::set<RE::BGSKeyword*>&) = [](KeywordChangeMap& changeMap, RE::TESBoundObject* item,
                                                                                                          const CustomKeyword& ckw, std::set<RE::BGSKeyword*>& touched) -> void {
                                                        if (touched.contains(ckw.kw)) return;
                                                        touched.insert(ckw.kw);

                                                        auto& changes = changeMap[ckw.kw];
                                                        if (changes.remove.contains(item))
                                                            changes.remove.erase(item);
                                                        else
                                                            changes.add.insert(item);

                                                        for (auto i : ckw.imply) {
                                                            auto it = g_Config.mapCustomKWs.find(i);
                                                            if (it != g_Config.mapCustomKWs.end()) AddKW(changeMap, item, it->second, touched);
                                                        }

                                                        for (auto i : ckw.exclude) {
                                                            auto it = g_Config.mapCustomKWs.find(i);
                                                            if (it != g_Config.mapCustomKWs.end()) RemoveKW(changeMap, item, it->second, touched);
                                                        }
                                                    };

                                                    for (auto item : selected) {
                                                        std::set<RE::BGSKeyword*> touched;
                                                        if (auto form = item->As<RE::BGSKeywordForm>()) {
                                                            if (nState) {
                                                                AddKW(params.mapKeywordChanges, item, ckw, touched);
                                                            } else {
                                                                RemoveKW(params.mapKeywordChanges, item, ckw, touched);
                                                            }
                                                        }
                                                    }
                                                }

                                                if (!ckw.tooltip.empty()) MakeTooltip(ckw.tooltip.c_str());

                                                ImGui::PopStyleColor(nPopColor);
                                            }
                                        }

                                        if (bUsed && priority) {
                                            ImGui::TableNextRow();
                                            for (int n = 0; n < nCols; n++) {
                                                ImGui::TableNextColumn();
                                                ImGui::Separator();
                                            }
                                            ImGui::TableNextRow();
                                        }
                                    }

                                    ImGui::EndDisabled();

                                    ImGui::EndTable();
                                }

                                ImGui::EndTabItem();
                            }
                        }

                        ImGui::EndTabBar();
                    }
                }

                ImGui::EndTable();
            }

            ImGui::EndPopup();
        } else if (bPopupKeywordsWasOpen) {
            bPopupKeywordsWasOpen = false;
            itemsCustomTemp.Pop(true);
            itemsCustomTemp.Restore();
        }
        if (switchToMod) Local::SwitchToMod(switchToMod);
    }

    if (ImGui::IsMouseDown(ImGuiMouseButton_Middle) || ImGui::IsWindowCollapsed()) g_Pause.SkipFrame();

    ImGuiIntegration::BlockInput(!ImGui::IsMouseDown(ImGuiMouseButton_Middle) && !ImGui::IsWindowCollapsed(), ImGui::IsItemHovered());
    ImGui::End();

    if (g_Config.bExportUntranslated) Localization::Get()->Export();
    if (!isActive) {
        ImGuiIntegration::Show(false);
        // Reset the startup delay for next time GUI opens
        g_inventoryStartupDelay = 5;
        logger::trace("[GUI] RenderUI - GUI closed, startup delay reset for next open");
    }

    g_Pause.Update(isActive && g_Config.bPauseWhileOpen);
}

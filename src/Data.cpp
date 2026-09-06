#include "Data.h"

#include "ArmorChanger.h"
#include "Config.h"
#include "ModIntegrations.h"
#include "rapidjson/document.h"
#include "rapidjson/error/en.h"
#include "rapidjson/error/error.h"
#include "rapidjson/filereadstream.h"
#include "rapidjson/filewritestream.h"
#include "rapidjson/prettywriter.h"
#include "rapidjson/stringbuffer.h"

using namespace rapidjson;

namespace QuickArmorRebalance {
    void LoadChangesFromFolder(const char* sub, const Permissions& perm);
    bool LoadFileChanges(const RE::TESFile* mod, std::filesystem::path path, const Permissions& perm);
}

using namespace QuickArmorRebalance;

ProcessedData QuickArmorRebalance::g_Data;
ItemTracking QuickArmorRebalance::g_ItemTracking;


bool QuickArmorRebalance::ReadJSONFile(std::filesystem::path path, Document& doc, bool bEditing) {
    if (std::filesystem::exists(path)) {
        if (auto fp = std::fopen(path.generic_string().c_str(), "rb")) {
            char readBuffer[1 << 16];
            FileReadStream is(fp, readBuffer, sizeof(readBuffer));
            doc.ParseStream<kParseCommentsFlag | kParseTrailingCommasFlag>(is);
            std::fclose(fp);

            if (doc.HasParseError()) {
                logger::warn("{}: JSON parse error: {} ({})", path.generic_string(), GetParseError_En(doc.GetParseError()), doc.GetErrorOffset());
                if (bEditing) {
                    logger::warn("{}: Overwriting previous file contents due to parsing error", path.generic_string());
                    doc.SetObject();
                }
            }

            if (!doc.IsObject()) {
                if (bEditing) {
                    logger::warn("{}: Unexpected contents, overwriting previous contents", path.generic_string());
                    doc.SetObject();
                }
            }

        } else {
            logger::warn("Could not open file {}", path.filename().generic_string());
            return false;
        }
    } else {
        doc.SetObject();
    }

    return true;
}

bool QuickArmorRebalance::WriteJSONFile(std::filesystem::path path, rapidjson::Document& doc) {
    if (auto fp = std::fopen(path.generic_string().c_str(), "wb")) {
        char buffer[1 << 16];
        FileWriteStream ws(fp, buffer, sizeof(buffer));
        PrettyWriter<FileWriteStream> writer(ws);
        writer.SetIndent('\t', 1);
        doc.Accept(writer);
        std::fclose(fp);
        return true;
    } else {
        logger::error("Could not open file to write {}: {}", path.generic_string(), std::strerror(errno));
        return false;
    }
}

bool QuickArmorRebalance::IsValidItem(RE::TESBoundObject* i) {
    static int callCount = 0;
    callCount++;
    if (callCount % 1000 == 0) {
        logger::trace("[Data] IsValidItem called {} times total", callCount);
    }

    auto mod = i->GetFile(0);
    if (mod && g_Config.blacklist.contains(mod)) return false;

    // Allow dynamic forms if they have a valid name (e.g., our enhanced armor copies)
    if (i->IsDynamicForm()) {
        // Dynamic forms are allowed if they have a name and are playable
        if (!i->GetPlayable() || i->IsDeleted() || i->IsIgnored() || !i->GetName()) return false;
        // Continue to type-specific checks below
    } else {
        if (!i->GetPlayable() || i->IsDeleted() || i->IsIgnored() || !i->GetName()) return false;
    }

    if (auto armor = i->As<RE::TESObjectARMO>()) {
        if (!armor->GetFullName() || armor->GetFullNameLength() <= 0) return false;

        /*
        if (((unsigned int)armor->GetSlotMask().underlying() & g_Config.usedSlotsMask) == 0) {
            // logger::debug("Skipping item for no valid slots {}", i->GetFullName());
            return false;
        }
        */
    } else if (auto weap = i->As<RE::TESObjectWEAP>()) {
        if (!weap->GetFullName() || weap->GetFullNameLength() <= 0) return false;

    } else if (auto ammo = i->As<RE::TESAmmo>()) {
        if (!ammo->GetFullName() || ammo->GetFullNameLength() <= 0) return false;
    } else
        return false;

    return true;
}

void ProcessItem(RE::TESBoundObject* i) {
    if (!IsValidItem(i)) return;

    auto mod = i->GetFile(0);
    auto it = g_Data.modData.find(mod);
    ModData* data = nullptr;

    if (it != g_Data.modData.end())
        data = it->second.get();
    else {
        g_Data.sortedMods.push_back(data = (g_Data.modData[mod] = std::make_unique<ModData>(mod)).get());
        logger::trace("Added {}", mod->fileName);
    }

    data->items.insert(i);
}

void CopyRecipe(std::map<RE::TESBoundObject*, RE::BGSConstructibleObject*>& map, RE::TESBoundObject* src, RE::TESBoundObject* tar) {
    if (map.find(tar) != map.end()) return;

    auto it = map.find(src);
    if (it != map.end()) map.insert({tar, it->second});
}

void QuickArmorRebalance::ProcessData() {
    auto dataHandler = RE::TESDataHandler::GetSingleton();

    logger::trace("Processing armor");
    for (auto i : dataHandler->GetFormArray<RE::TESObjectARMO>()) {
        ProcessItem(i);
    }

    logger::trace("Processing weapons");
    for (auto i : dataHandler->GetFormArray<RE::TESObjectWEAP>()) {
        ProcessItem(i);
    }

    logger::trace("Processing ammo");
    for (auto i : dataHandler->GetFormArray<RE::TESAmmo>()) {
        ProcessItem(i);
    }

    if (!g_Data.sortedMods.empty()) {
        std::sort(g_Data.sortedMods.begin(), g_Data.sortedMods.end(),
                  [](ModData* const a, ModData* const b) { return _stricmp(a->mod->GetFilename().data(), b->mod->GetFilename().data()) < 0; });
    }

    auto temperBench = RE::TESForm::LookupByEditorID<RE::BGSKeyword>("CraftingSmithingArmorTable");
    if (!temperBench) return;

    auto temperWeapBench = RE::TESForm::LookupByEditorID<RE::BGSKeyword>("CraftingSmithingSharpeningWheel");
    if (!temperWeapBench) return;

    auto smelter = RE::TESForm::LookupByEditorID<RE::BGSKeyword>("CraftingSmelter");
    if (!temperBench) return;

    logger::trace("Processing recipes");
    auto& lsRecipies = dataHandler->GetFormArray<RE::BGSConstructibleObject>();
    for (auto i : lsRecipies) {
        if (!i->createdItem) continue;
        auto pObj = i->createdItem->As<RE::TESBoundObject>();
        if (!pObj) continue;

        if (i->benchKeyword == temperBench || i->benchKeyword == temperWeapBench)
            g_Data.temperRecipe.insert({pObj, i});
        else if (i->benchKeyword == smelter) {
            auto& mats = i->requiredItems;
            if (mats.numContainerObjects == 1) g_Data.smeltRecipe.insert({mats.containerObjects[0]->obj, i});
        } else
            g_Data.craftRecipe.insert({pObj, i});
    }

    if (g_Config.bUseSecondaryRecipes) {
        logger::trace("Building secondary recipes");

        for (auto& i : g_Config.armorSets) {
            if (!i.strFallbackRecipeSet.empty()) {
                if (auto as = g_Config.FindArmorSet(i.strFallbackRecipeSet.c_str())) {
                    auto fillRecipes = [as](auto item) {
                        bool hasTemper = g_Data.temperRecipe.find(item) != g_Data.temperRecipe.end();
                        bool hasCraft = g_Data.craftRecipe.find(item) != g_Data.craftRecipe.end();

                        if (!hasTemper || !hasCraft) {
                            if (auto copy = as->FindMatching(item)) {
                                if (!hasTemper) CopyRecipe(g_Data.temperRecipe, copy, item);
                                if (!hasCraft) CopyRecipe(g_Data.craftRecipe, copy, item);
                            }
                        }
                    };

                    std::for_each(i.items.begin(), i.items.end(), fillRecipes);
                    std::for_each(i.weaps.begin(), i.weaps.end(), fillRecipes);
                    std::for_each(i.ammo.begin(), i.ammo.end(), fillRecipes);
                } else
                    logger::warn("Fallback recipe set not found : {}", i.strFallbackRecipeSet);
            }

            /*
            auto reportMissingRecipies = [](auto item) {
                if (g_Data.temperRecipe.find(item) == g_Data.temperRecipe.end())
                    logger::info("{}: Missing temper recipe", item->GetName());
                if (g_Data.craftRecipe.find(item) == g_Data.craftRecipe.end())
                    logger::info("{}: Missing craft recipe", item->GetName());
            };

            std::for_each(i.items.begin(), i.items.end(), reportMissingRecipies);
            std::for_each(i.weaps.begin(), i.weaps.end(), reportMissingRecipies);
            std::for_each(i.ammo.begin(), i.ammo.end(), reportMissingRecipies);
            */
        }
    }

    static std::set<RE::TESForm*> recipeConditionForms;
    for (auto& armorSet : g_Config.armorSets) {

        struct RecipeProcessFuncs {
            static void Pull(RE::TESBoundObject* obj, RE::BGSConstructibleObject* recipe) {
                for (auto cond = recipe->conditions.head; cond; cond = cond->next) {
                    switch (cond->data.functionData.function.get()) {
                        case RE::FUNCTION_DATA::FunctionID::kGetItemCount:
                        case RE::FUNCTION_DATA::FunctionID::kGetEquipped:
                            if (cond->data.functionData.params[0] != obj && recipe->requiredItems.GetObjectCount((RE::TESBoundObject*)cond->data.functionData.params[0]) == 0) {
                                //logger::info("{} requires {}", obj->GetName(), ((RE::TESForm*)cond->data.functionData.params[0])->GetName());
                                recipeConditionForms.insert((RE::TESForm*)cond->data.functionData.params[0]);
                            }
                            break;
                        case RE::FUNCTION_DATA::FunctionID::kHasPerk:
                            recipeConditionForms.insert((RE::TESForm*)cond->data.functionData.params[0]);
                            break;
                    }
                }
                
            }

            static void PullConditions(RE::TESBoundObject* obj) {
                if (auto recipe = MapFindOrNull(g_Data.temperRecipe, obj)) Pull(obj, recipe);
                if (auto recipe = MapFindOrNull(g_Data.craftRecipe, obj)) Pull(obj, recipe);
            }
        };

        for (auto i : armorSet.items) RecipeProcessFuncs::PullConditions(i);
        for (auto i : armorSet.weaps) RecipeProcessFuncs::PullConditions(i);
        for (auto i : armorSet.ammo) RecipeProcessFuncs::PullConditions(i);
    }

    for (auto i : recipeConditionForms) {
        g_Data.recipeConditions.push_back(i);
        std::sort(g_Data.recipeConditions.begin(), g_Data.recipeConditions.end(),
                  [](RE::TESForm* const a, RE::TESForm* const b) { return _stricmp(a->GetName(), b->GetName()) < 0; });        
    }

    logger::trace("Building list of skyrim armor model files");

    auto& lsAddons = dataHandler->GetFormArray<RE::TESObjectARMA>();
    for (auto addon : lsAddons) {
        if ((addon->GetFormID() & 0xff000000) == 0) {  // Skyrim.esm only

            for (int i = 0; i < RE::SEXES::kTotal; i++) {
                if (!addon->bipedModels[i].model.empty()) {
                    std::string modelPath(addon->bipedModels[i].model);
                    ToLower(modelPath);

                    auto hash = std::hash<std::string>{}(modelPath);
                    g_Data.noModifyModels.insert(hash);

                    if (modelPath.length() > 6) {
                        char* pChar = modelPath.data() + modelPath.length() - 6;  //'_X.nif'
                        if (*pChar++ == '_') {
                            if (*pChar == '0') {
                                *pChar = '1';
                                hash = std::hash<std::string>{}(modelPath);
                                g_Data.noModifyModels.insert(hash);
                            } else if (*pChar == '1') {
                                *pChar = '0';
                                hash = std::hash<std::string>{}(modelPath);
                                g_Data.noModifyModels.insert(hash);
                            }
                        }
                    }
                }
            }
        }
    }
}

void QuickArmorRebalance::LoadChangesFromFiles() {
    /*
    //Iterate from existing mods - probably slower so doing it the other way around
    auto count = data->GetLoadedModCount();
    auto mods = data->GetLoadedMods();

    for (auto i = 0; i < count; i++) {
        logger::info("[{}]: {}", i, mods[i]->fileName);
    }

    count = data->GetLoadedLightModCount();
    mods = data->GetLoadedLightMods();
    for (auto i = 0; i < count; i++) {
        mods[i]->logger::info("[{}]: {}", i, mods[i]->fileName);
    }
    */

    ImportFromDAV();

    logger::info("Loading changes from files");
    LoadChangesFromFolder("shared/", QuickArmorRebalance::g_Config.permShared);
    logger::info("{} items affected from shared changes", g_Data.modifiedItemsShared.size());
    LoadChangesFromFolder("local/", QuickArmorRebalance::g_Config.permLocal);
    logger::info("{} items affected from local changes", g_Data.modifiedItems.size());
}

void QuickArmorRebalance::ForChangesInFolder(const char* sub, const std::function<void(const RE::TESFile*, std::filesystem::path)> fn) {
    auto dataHandler = RE::TESDataHandler::GetSingleton();

    auto path = std::filesystem::current_path() / PATH_ROOT PATH_CHANGES;
    path /= sub;

    if (!std::filesystem::exists(path)) return;

    if (!std::filesystem::is_directory(path)) {
        logger::error("Is not a directory ({})", path.generic_string());
        return;
    }

    for (const auto& entry : std::filesystem::directory_iterator(path)) {
        if (!entry.is_regular_file()) continue;
        if (_stricmp(entry.path().extension().generic_string().c_str(), ".json")) continue;

        auto modName = entry.path().filename().generic_string();
        modName.resize(modName.size() - 5);  // strip ".json"

        if (auto mod = dataHandler->LookupModByName(modName)) {
            logger::trace("Loading change file {}", entry.path().filename().generic_string());
            fn(mod, entry.path());
        }
    }
}

void QuickArmorRebalance::LoadChangesFromFolder(const char* sub, const Permissions& perm) {
    ForChangesInFolder(sub, [&](auto mod, auto path) {
        if (!LoadFileChanges(mod, path, perm)) logger::warn("Failed to load change file {}", path.filename().generic_string());
    });

    if (perm.bModifyCustomKeywords) {
        std::filesystem::path dir(sub);
        dir /= PATH_CUSTOMKEYWORDS;

        ForChangesInFolder(dir.generic_string().c_str(), [&](auto mod, auto path) {
            if (!LoadKeywordChanges(mod, path)) logger::warn("Failed to load custom keywords file {}", path.filename().generic_string());
        });
    }
}

bool QuickArmorRebalance::LoadFileChanges(const RE::TESFile* mod, std::filesystem::path path, const Permissions& perm) {
    Document doc;

    if (!ReadJSONFile(path, doc, false)) return false;

    if (doc.HasParseError() || !doc.IsObject()) return false;

    /*
    if (auto fp = std::fopen(path.generic_string().c_str(), "rb")) {
        char readBuffer[1 << 16];
        FileReadStream is(fp, readBuffer, sizeof(readBuffer));
        doc.ParseStream(is);
        std::fclose(fp);

        if (doc.HasParseError()) {
            logger::warn("{}: JSON parse error: {} ({})", path.generic_string(), GetParseError_En(doc.GetParseError()),
                         doc.GetErrorOffset());
            return false;
        }

        if (!doc.IsObject()) {
            logger::warn("{}: Unexpected contents, overwriting previous contents", path.generic_string());
            return false;
        }
    } else {
        logger::warn("{}: Couldn't open file", path.generic_string());
        return false;
    }
    */

    ApplyChanges(mod, doc.GetObj(), perm);

    return true;
}

void QuickArmorRebalance::DeleteAllChanges(RE::TESFile* mod) {
    auto path = std::filesystem::current_path() / PATH_ROOT PATH_CHANGES "local/";
    path /= mod->fileName;
    path += ".json";

    if (!std::filesystem::exists(path)) return;

    std::filesystem::remove(path);
    g_Data.modifiedFilesDeleted.insert(mod);
}

void QuickArmorRebalance::LoadFavorites() {
    auto basePath = std::filesystem::current_path() / PATH_ROOT "favorites/";

    if (!std::filesystem::exists(basePath)) {
        return;  // No favorites directory, nothing to load
    }

    if (!std::filesystem::is_directory(basePath)) {
        logger::error("Favorites path is not a directory ({})", basePath.generic_string());
        return;
    }

    auto dataHandler = RE::TESDataHandler::GetSingleton();

    // Manually iterate favorites directory (same path as SaveFavorites uses)
    for (const auto& entry : std::filesystem::directory_iterator(basePath)) {
        if (!entry.is_regular_file()) continue;
        if (_stricmp(entry.path().extension().generic_string().c_str(), ".json")) continue;

        Document doc;
        if (!ReadJSONFile(entry.path(), doc, false)) continue;

        if (doc.HasMember("favorites") && doc["favorites"].IsArray()) {
            for (auto& favItem : doc["favorites"].GetArray()) {
                if (!favItem.IsString()) continue;

                std::string favFormID = favItem.GetString();
                g_Data.favoriteItemsMap.insert(favFormID);

                // Parse "ModName.esp:0xFormID" format
                auto colonPos = favFormID.find(':');
                if (colonPos == std::string::npos) continue;

                std::string modName = favFormID.substr(0, colonPos);
                std::string formIDStr = favFormID.substr(colonPos + 3);  // Skip ":0x"

                // Look up the actual item pointer
                if (auto modFile = dataHandler->LookupModByName(modName)) {
                    RE::FormID localID = std::strtoul(formIDStr.c_str(), nullptr, 16);
                    RE::FormID fullID = GetFullId(modFile, localID);

                    if (auto item = RE::TESForm::LookupByID(fullID)) {
                        if (auto boundObj = item->As<RE::TESBoundObject>()) {
                            g_Data.favoriteItems.insert(boundObj);
                        }
                    }
                }
            }
        }
    }

    logger::info("Loaded {} favorites", g_Data.favoriteItemsMap.size());
}

void QuickArmorRebalance::SaveFavorites() {
    auto basePath = std::filesystem::current_path() / PATH_ROOT "favorites/";
    std::filesystem::create_directories(basePath);

    // Group favorites by mod file
    std::map<std::string, std::vector<std::string>> favoritesByMod;

    for (const auto& favFormID : g_Data.favoriteItemsMap) {
        auto colonPos = favFormID.find(':');
        if (colonPos == std::string::npos) continue;

        std::string modName = favFormID.substr(0, colonPos);
        favoritesByMod[modName].push_back(favFormID);
    }

    // Write one JSON file per mod
    for (auto& [modName, favList] : favoritesByMod) {
        Document doc;
        auto& al = doc.GetAllocator();
        doc.SetObject();

        rapidjson::Value favsArray(rapidjson::kArrayType);
        for (const auto& formID : favList) {
            favsArray.PushBack(Value(formID.c_str(), al), al);
        }
        doc.AddMember("favorites", favsArray, al);

        std::filesystem::path path = basePath / (modName + ".json");

        if (!WriteJSONFile(path, doc)) {
            logger::error("Failed to save favorites for {}", modName);
        }
    }

    // Clean up JSON files for mods that no longer have any favorites
    if (std::filesystem::exists(basePath) && std::filesystem::is_directory(basePath)) {
        for (const auto& entry : std::filesystem::directory_iterator(basePath)) {
            if (!entry.is_regular_file()) continue;
            if (_stricmp(entry.path().extension().generic_string().c_str(), ".json")) continue;

            auto modName = entry.path().filename().generic_string();
            modName.resize(modName.size() - 5);  // strip ".json"

            // If this mod no longer has favorites, delete its file
            if (!favoritesByMod.contains(modName)) {
                std::filesystem::remove(entry.path());
                logger::info("Removed favorites file for {} (no longer has favorites)", modName);
            }
        }
    }

    logger::info("Saved {} favorites across {} files",
                 g_Data.favoriteItemsMap.size(), favoritesByMod.size());
}

bool QuickArmorRebalance::IsValidOutfitName(const std::string& name) {
    if (name.empty()) return false;
    if (name.length() > 64) return false;

    // Only alphanumeric, underscore, dash, and space allowed
    for (char c : name) {
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '-' && c != ' ') {
            return false;
        }
    }

    return true;
}

RE::TESAmmo* QuickArmorRebalance::GetEquippedAmmo(RE::Actor* actor) {
    if (!actor) return nullptr;
    auto process = actor->GetActorRuntimeData().currentProcess;
    if (!process) return nullptr;

    // Actor::GetCurrentAmmo's virtual call returned 0x1 for an NPC on AE 1.6.1170.
    // Read the equipment process instead. This non-virtual helper checks middleHigh
    // and does not initialize or enumerate inventory. Never treat a non-ammo form as ammo.
    auto entry = process->GetCurrentAmmo();
    auto object = entry ? entry->object : nullptr;
    return object ? object->As<RE::TESAmmo>() : nullptr;
}

std::vector<RE::TESBoundObject*> QuickArmorRebalance::GetEquippedItems(RE::Actor* player) {
    logger::trace("[Data] GetEquippedItems called");
    std::vector<RE::TESBoundObject*> equipped;

    if (!player) {
        logger::warn("[Data] GetEquippedItems - player is null!");
        return equipped;
    }

    // Use GetWornArmor() for each biped slot instead of GetInventory()
    // This avoids crashes with other SKSE plugins that hook GetInventory
    logger::trace("[Data] GetEquippedItems - using GetWornArmor for each slot");
    std::set<RE::TESObjectARMO*> seenArmor;
    for (int slot = 0; slot < 32; slot++) {
        auto bipedSlot = static_cast<RE::BGSBipedObjectForm::BipedObjectSlot>(1 << slot);
        if (auto armor = player->GetWornArmor(bipedSlot)) {
            if (seenArmor.insert(armor).second) {  // Only add if not seen before
                if (IsValidItem(armor)) {
                    equipped.push_back(armor);
                    logger::trace("[Data] GetEquippedItems - found armor in slot {}: {}", slot, armor->GetName());
                }
            }
        }
    }

    // Get equipped weapons using GetEquippedObject
    logger::trace("[Data] GetEquippedItems - checking equipped weapons");
    for (bool leftHand : {false, true}) {
        if (auto obj = player->GetEquippedObject(leftHand)) {
            if (auto weap = obj->As<RE::TESObjectWEAP>()) {
                if (IsValidItem(weap)) {
                    // Check if not already in list
                    if (std::find(equipped.begin(), equipped.end(), weap) == equipped.end()) {
                        equipped.push_back(weap);
                        logger::trace("[Data] GetEquippedItems - found weapon: {}", weap->GetName());
                    }
                }
            }
        }
    }

    // Get equipped ammo
    logger::trace("[Data] GetEquippedItems - checking equipped ammo");
    if (auto ammo = GetEquippedAmmo(player)) {
        if (IsValidItem(ammo)) {
            equipped.push_back(ammo);
            logger::trace("[Data] GetEquippedItems - found ammo: {}", ammo->GetName());
        }
    }

    logger::trace("[Data] GetEquippedItems - done, {} equipped items found", equipped.size());
    return equipped;
}

std::string QuickArmorRebalance::NormalizeTagName(const std::string& tag) {
    if (tag.empty()) return "";

    std::string normalized;
    normalized.reserve(tag.length());

    // First character: uppercase
    normalized += static_cast<char>(std::toupper(static_cast<unsigned char>(tag[0])));

    // Rest: lowercase
    for (size_t i = 1; i < tag.length(); i++) {
        normalized += static_cast<char>(std::tolower(static_cast<unsigned char>(tag[i])));
    }

    return normalized;
}

bool QuickArmorRebalance::IsValidTagName(const std::string& tag) {
    if (tag.empty()) return false;
    if (tag.length() > 32) return false;  // Reasonable max length for a tag

    // Only alphanumeric allowed (no spaces, no special characters)
    for (char c : tag) {
        if (!std::isalnum(static_cast<unsigned char>(c))) {
            return false;
        }
    }

    return true;
}

std::set<std::string> QuickArmorRebalance::RebuildGlobalTags() {
    std::set<std::string> allTags;

    // Scan all outfits and collect their tags
    for (const auto& [name, outfit] : g_Data.outfits) {
        for (const auto& tag : outfit.tags) {
            allTags.insert(tag);
        }
    }

    return allTags;
}

void QuickArmorRebalance::LoadOutfits() {
    auto basePath = std::filesystem::current_path() / PATH_ROOT "outfits/";

    if (!std::filesystem::exists(basePath)) {
        return;  // No outfits directory
    }

    if (!std::filesystem::is_directory(basePath)) {
        logger::error("Outfits path is not a directory ({})", basePath.generic_string());
        return;
    }

    auto dataHandler = RE::TESDataHandler::GetSingleton();

    for (const auto& entry : std::filesystem::directory_iterator(basePath)) {
        if (!entry.is_regular_file()) continue;
        if (_stricmp(entry.path().extension().generic_string().c_str(), ".json")) continue;

        Document doc;
        if (!ReadJSONFile(entry.path(), doc, false)) continue;

        // Parse outfit name from filename (without .json extension)
        std::string outfitName = entry.path().stem().generic_string();

        Outfit outfit;
        outfit.name = outfitName;

        // Parse items array
        if (doc.HasMember("items") && doc["items"].IsArray()) {
            for (auto& itemStr : doc["items"].GetArray()) {
                if (!itemStr.IsString()) continue;

                std::string itemFormID = itemStr.GetString();
                outfit.itemFormIDs.push_back(itemFormID);

                // Parse "ModName.esp:0xFormID" format
                auto colonPos = itemFormID.find(':');
                if (colonPos == std::string::npos) continue;

                std::string modName = itemFormID.substr(0, colonPos);
                std::string formIDStr = itemFormID.substr(colonPos + 3);  // Skip ":0x"

                // Look up the actual item pointer
                if (auto modFile = dataHandler->LookupModByName(modName)) {
                    RE::FormID localID = std::strtoul(formIDStr.c_str(), nullptr, 16);
                    RE::FormID fullID = GetFullId(modFile, localID);

                    if (auto item = RE::TESForm::LookupByID(fullID)) {
                        if (auto boundObj = item->As<RE::TESBoundObject>()) {
                            outfit.items.insert(boundObj);
                        }
                    }
                }
            }
        }

        // Parse tags array
        if (doc.HasMember("tags") && doc["tags"].IsArray()) {
            for (auto& tagStr : doc["tags"].GetArray()) {
                if (!tagStr.IsString()) continue;

                std::string tag = tagStr.GetString();
                // Normalize and validate the tag
                std::string normalized = NormalizeTagName(tag);
                if (IsValidTagName(normalized)) {
                    outfit.tags.insert(normalized);
                }
            }
        }

        // Parse enhanced items object
        if (doc.HasMember("enhanced") && doc["enhanced"].IsObject()) {
            for (auto& item : doc["enhanced"].GetObj()) {
                std::string itemFormID = item.name.GetString();
                EnhancedItemConfig config;
                auto& v = item.value;

                if (v.HasMember("enchantment") && v["enchantment"].IsString()) {
                    config.enchantmentFormID = v["enchantment"].GetString();
                }
                if (v.HasMember("magnitude") && v["magnitude"].IsNumber()) {
                    config.enchantmentMagnitude = v["magnitude"].GetFloat();
                }
                if (v.HasMember("statsSource") && v["statsSource"].IsString()) {
                    config.statsSourceFormID = v["statsSource"].GetString();
                }
                if (v.HasMember("armorRating") && v["armorRating"].IsNumber()) {
                    config.armorRating = v["armorRating"].GetUint();
                }
                if (v.HasMember("weight") && v["weight"].IsNumber()) {
                    config.weight = v["weight"].GetFloat();
                }
                if (v.HasMember("value") && v["value"].IsNumber()) {
                    config.value = v["value"].GetInt();
                }

                if (config.IsEnhanced()) {
                    outfit.enhancedItems[itemFormID] = config;
                }
            }
        }

        g_Data.outfits[outfitName] = std::move(outfit);
    }

    logger::info("Loaded {} outfits", g_Data.outfits.size());
}

bool QuickArmorRebalance::SaveOutfit(const std::string& name, const Outfit& outfit) {
    if (!IsValidOutfitName(name)) {
        logger::error("Invalid outfit name: {}", name);
        return false;
    }

    auto basePath = std::filesystem::current_path() / PATH_ROOT "outfits/";
    std::filesystem::create_directories(basePath);

    Document doc;
    auto& al = doc.GetAllocator();
    doc.SetObject();

    // Create items array
    rapidjson::Value itemsArray(rapidjson::kArrayType);
    for (const auto& formID : outfit.itemFormIDs) {
        itemsArray.PushBack(Value(formID.c_str(), al), al);
    }
    doc.AddMember("items", itemsArray, al);

    // Create tags array
    rapidjson::Value tagsArray(rapidjson::kArrayType);
    for (const auto& tag : outfit.tags) {
        tagsArray.PushBack(Value(tag.c_str(), al), al);
    }
    doc.AddMember("tags", tagsArray, al);

    // Create enhanced items object (only if there are enhancements)
    if (!outfit.enhancedItems.empty()) {
        rapidjson::Value enhancedObj(rapidjson::kObjectType);

        for (const auto& [formID, config] : outfit.enhancedItems) {
            if (!config.IsEnhanced()) continue;

            rapidjson::Value itemConfig(rapidjson::kObjectType);

            if (config.HasEnchantment()) {
                itemConfig.AddMember("enchantment", Value(config.enchantmentFormID.c_str(), al), al);
                itemConfig.AddMember("magnitude", config.enchantmentMagnitude, al);
            }

            if (config.HasStatsTransfer()) {
                itemConfig.AddMember("statsSource", Value(config.statsSourceFormID.c_str(), al), al);
                if (config.armorRating) itemConfig.AddMember("armorRating", *config.armorRating, al);
                if (config.weight) itemConfig.AddMember("weight", *config.weight, al);
                if (config.value) itemConfig.AddMember("value", *config.value, al);
            }

            enhancedObj.AddMember(Value(formID.c_str(), al), itemConfig, al);
        }

        if (enhancedObj.MemberCount() > 0) {
            doc.AddMember("enhanced", enhancedObj, al);
        }
    }

    std::filesystem::path path = basePath / (name + ".json");

    if (!WriteJSONFile(path, doc)) {
        logger::error("Failed to save outfit {}", name);
        return false;
    }

    logger::info("Saved outfit: {}", name);
    return true;
}

bool QuickArmorRebalance::DeleteOutfit(const std::string& name) {
    auto basePath = std::filesystem::current_path() / PATH_ROOT "outfits/";
    std::filesystem::path path = basePath / (name + ".json");

    if (!std::filesystem::exists(path)) {
        logger::warn("Outfit file not found: {}", name);
        return false;
    }

    if (std::filesystem::remove(path)) {
        g_Data.outfits.erase(name);
        logger::info("Deleted outfit: {}", name);
        return true;
    }

    logger::error("Failed to delete outfit file: {}", name);
    return false;
}

bool QuickArmorRebalance::RenameOutfit(const std::string& oldName, const std::string& newName) {
    if (!IsValidOutfitName(newName)) {
        logger::error("Invalid new outfit name: {}", newName);
        return false;
    }

    if (g_Data.outfits.contains(newName)) {
        logger::error("Outfit name already exists: {}", newName);
        return false;
    }

    auto it = g_Data.outfits.find(oldName);
    if (it == g_Data.outfits.end()) {
        logger::error("Outfit not found: {}", oldName);
        return false;
    }

    // Get outfit data
    Outfit outfit = it->second;
    outfit.name = newName;

    // Delete old file
    auto basePath = std::filesystem::current_path() / PATH_ROOT "outfits/";
    std::filesystem::path oldPath = basePath / (oldName + ".json");
    if (std::filesystem::exists(oldPath)) {
        std::filesystem::remove(oldPath);
    }

    // Save with new name
    if (SaveOutfit(newName, outfit)) {
        g_Data.outfits.erase(oldName);
        g_Data.outfits[newName] = std::move(outfit);
        logger::info("Renamed outfit: {} -> {}", oldName, newName);
        return true;
    }

    return false;
}

// ============================================================================
// Item Tags Management
// ============================================================================

void QuickArmorRebalance::LoadItemTags() {
    auto path = std::filesystem::current_path() / PATH_ROOT "itemtags.json";

    if (!std::filesystem::exists(path)) {
        logger::trace("Item tags file not found, starting fresh");
        return;
    }

    Document doc;
    if (!ReadJSONFile(path, doc, false)) {
        logger::warn("Failed to load item tags file");
        return;
    }

    auto dataHandler = RE::TESDataHandler::GetSingleton();

    // Clear existing data
    g_Data.itemTagsMap.clear();
    g_Data.itemTagsRuntime.clear();
    g_Data.globalItemTags.clear();

    // Parse items object
    if (doc.HasMember("items") && doc["items"].IsObject()) {
        for (auto& item : doc["items"].GetObj()) {
            std::string formIDStr = item.name.GetString();

            if (!item.value.IsArray()) continue;

            std::set<std::string> tags;
            for (auto& tagVal : item.value.GetArray()) {
                if (!tagVal.IsString()) continue;

                std::string tag = tagVal.GetString();
                std::string normalized = NormalizeTagName(tag);
                if (IsValidTagName(normalized)) {
                    tags.insert(normalized);
                    g_Data.globalItemTags.insert(normalized);
                }
            }

            if (tags.empty()) continue;

            // Store in map
            g_Data.itemTagsMap[formIDStr] = std::move(tags);

            // Build runtime lookup
            auto colonPos = formIDStr.find(':');
            if (colonPos == std::string::npos) continue;

            std::string modName = formIDStr.substr(0, colonPos);
            std::string localIDStr = formIDStr.substr(colonPos + 3);  // Skip ":0x"

            if (auto modFile = dataHandler->LookupModByName(modName)) {
                RE::FormID localID = std::strtoul(localIDStr.c_str(), nullptr, 16);
                RE::FormID fullID = GetFullId(modFile, localID);

                if (auto form = RE::TESForm::LookupByID(fullID)) {
                    if (auto boundObj = form->As<RE::TESBoundObject>()) {
                        g_Data.itemTagsRuntime[boundObj] = &g_Data.itemTagsMap[formIDStr];
                    }
                }
            }
        }
    }

    logger::info("Loaded item tags: {} tags, {} items tagged", g_Data.globalItemTags.size(), g_Data.itemTagsMap.size());
}

void QuickArmorRebalance::SaveItemTags() {
    auto path = std::filesystem::current_path() / PATH_ROOT "itemtags.json";

    Document doc;
    auto& al = doc.GetAllocator();
    doc.SetObject();

    // Create items object
    rapidjson::Value itemsObj(rapidjson::kObjectType);

    for (const auto& [formID, tags] : g_Data.itemTagsMap) {
        if (tags.empty()) continue;

        rapidjson::Value tagsArray(rapidjson::kArrayType);
        for (const auto& tag : tags) {
            tagsArray.PushBack(Value(tag.c_str(), al), al);
        }

        itemsObj.AddMember(Value(formID.c_str(), al), tagsArray, al);
    }

    doc.AddMember("items", itemsObj, al);

    if (!WriteJSONFile(path, doc)) {
        logger::error("Failed to save item tags");
        return;
    }

    logger::trace("Saved item tags");
}

void QuickArmorRebalance::RebuildGlobalItemTags() {
    g_Data.globalItemTags.clear();

    for (const auto& [formID, tags] : g_Data.itemTagsMap) {
        for (const auto& tag : tags) {
            g_Data.globalItemTags.insert(tag);
        }
    }
}

void QuickArmorRebalance::AddItemTag(RE::TESBoundObject* item, const std::string& tag) {
    if (!item) return;

    std::string normalized = NormalizeTagName(tag);
    if (!IsValidTagName(normalized)) return;

    std::string formID = QARFormID(item);

    // Add to map
    auto& tags = g_Data.itemTagsMap[formID];
    tags.insert(normalized);

    // Update runtime lookup
    g_Data.itemTagsRuntime[item] = &g_Data.itemTagsMap[formID];

    // Update global tags
    g_Data.globalItemTags.insert(normalized);
}

void QuickArmorRebalance::RemoveItemTag(RE::TESBoundObject* item, const std::string& tag) {
    if (!item) return;

    std::string normalized = NormalizeTagName(tag);
    std::string formID = QARFormID(item);

    auto it = g_Data.itemTagsMap.find(formID);
    if (it == g_Data.itemTagsMap.end()) return;

    it->second.erase(normalized);

    // If no tags left, remove from maps
    if (it->second.empty()) {
        g_Data.itemTagsMap.erase(it);
        g_Data.itemTagsRuntime.erase(item);
    }

    // Rebuild global tags to remove unused ones
    RebuildGlobalItemTags();
}

bool QuickArmorRebalance::HasItemTag(RE::TESBoundObject* item, const std::string& tag) {
    if (!item) return false;

    auto it = g_Data.itemTagsRuntime.find(item);
    if (it == g_Data.itemTagsRuntime.end() || !it->second) return false;

    std::string normalized = NormalizeTagName(tag);
    return it->second->contains(normalized);
}

const std::set<std::string>* QuickArmorRebalance::GetItemTags(RE::TESBoundObject* item) {
    if (!item) return nullptr;

    auto it = g_Data.itemTagsRuntime.find(item);
    if (it == g_Data.itemTagsRuntime.end()) return nullptr;

    return it->second;
}

void QuickArmorRebalance::BuildArmorEnchantmentCache() {
    g_Data.armorEnchantments.clear();

    auto dataHandler = RE::TESDataHandler::GetSingleton();
    if (!dataHandler) return;

    auto& enchantments = dataHandler->GetFormArray<RE::EnchantmentItem>();

    for (auto* ench : enchantments) {
        if (!ench) continue;

        // Filter for armor-compatible enchantments:
        // - Constant effect (not fire-and-forget)
        // - Self delivery (not touch or target)
        if (ench->data.castingType == RE::MagicSystem::CastingType::kConstantEffect &&
            ench->data.delivery == RE::MagicSystem::Delivery::kSelf) {

            // Only include enchantments with a name
            const char* name = ench->GetFullName();
            if (name && name[0]) {
                g_Data.armorEnchantments.push_back(ench);
            }
        }
    }

    // Sort by name for UI display
    std::sort(g_Data.armorEnchantments.begin(), g_Data.armorEnchantments.end(),
        [](RE::EnchantmentItem* a, RE::EnchantmentItem* b) {
            return _stricmp(a->GetFullName(), b->GetFullName()) < 0;
        });

    logger::info("Built armor enchantment cache: {} enchantments", g_Data.armorEnchantments.size());
}

// Item Tracking System - Per-character tracking of given items
// ============================================================

void QuickArmorRebalance::LoadItemTracking(const std::string& characterName) {
    g_ItemTracking.Clear();
    g_ItemTracking.characterName = characterName;

    if (characterName.empty()) {
        logger::warn("LoadItemTracking: Empty character name, skipping load");
        return;
    }

    // Create a safe filename from character name
    std::string safeFileName = characterName;
    for (char& c : safeFileName) {
        if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
            c = '_';
        }
    }

    auto path = std::filesystem::current_path() / PATH_ROOT "tracking/" / (safeFileName + ".json");

    if (!std::filesystem::exists(path)) {
        logger::info("LoadItemTracking: No tracking file for character '{}', starting fresh", characterName);
        return;
    }

    Document doc;
    if (!ReadJSONFile(path, doc, false)) {
        logger::error("LoadItemTracking: Failed to read tracking file for '{}'", characterName);
        return;
    }

    if (!doc.IsObject()) {
        logger::error("LoadItemTracking: Invalid tracking file format for '{}'", characterName);
        return;
    }

    // Load given items
    if (doc.HasMember("givenItems") && doc["givenItems"].IsArray()) {
        for (auto& itemVal : doc["givenItems"].GetArray()) {
            if (!itemVal.IsObject()) continue;

            GivenItemEntry entry;
            if (itemVal.HasMember("formID") && itemVal["formID"].IsString()) {
                entry.formID = itemVal["formID"].GetString();
            } else {
                continue;  // formID is required
            }

            entry.count = GetJsonInt(itemVal, "count", 0, INT_MAX, 1);
            entry.isEnhanced = GetJsonBool(itemVal, "isEnhanced", false);

            if (itemVal.HasMember("baseFormID") && itemVal["baseFormID"].IsString()) {
                entry.baseFormID = itemVal["baseFormID"].GetString();
            }
            if (itemVal.HasMember("enhancementKey") && itemVal["enhancementKey"].IsString()) {
                entry.enhancementKey = itemVal["enhancementKey"].GetString();
            }

            if (!entry.formID.empty() && entry.count > 0) {
                g_ItemTracking.givenItems[entry.formID] = entry;
            }
        }
    }

    // Load marked to keep items
    if (doc.HasMember("markedToKeep") && doc["markedToKeep"].IsArray()) {
        for (auto& keepVal : doc["markedToKeep"].GetArray()) {
            if (keepVal.IsString()) {
                g_ItemTracking.markedToKeep.insert(keepVal.GetString());
            }
        }
    }

    // Load applied enchantments (for cleanup)
    if (doc.HasMember("appliedEnchantments") && doc["appliedEnchantments"].IsArray()) {
        for (auto& enchVal : doc["appliedEnchantments"].GetArray()) {
            if (!enchVal.IsObject()) continue;

            AppliedEnchantment entry;
            if (enchVal.HasMember("dynamicFormID") && enchVal["dynamicFormID"].IsString()) {
                entry.dynamicFormID = enchVal["dynamicFormID"].GetString();
            }
            if (enchVal.HasMember("enchantmentFormID") && enchVal["enchantmentFormID"].IsString()) {
                entry.enchantmentFormID = enchVal["enchantmentFormID"].GetString();
            }
            if (enchVal.HasMember("baseArmorFormID") && enchVal["baseArmorFormID"].IsString()) {
                entry.baseArmorFormID = enchVal["baseArmorFormID"].GetString();
            }

            if (!entry.dynamicFormID.empty() && !entry.enchantmentFormID.empty()) {
                g_ItemTracking.appliedEnchantments.push_back(entry);
            }
        }
    }

    logger::info("LoadItemTracking: Loaded {} tracked items, {} marked to keep, {} applied enchantments for '{}'",
        g_ItemTracking.givenItems.size(), g_ItemTracking.markedToKeep.size(),
        g_ItemTracking.appliedEnchantments.size(), characterName);
}

void QuickArmorRebalance::SaveItemTracking() {
    if (g_ItemTracking.characterName.empty()) {
        logger::warn("SaveItemTracking: No character name set, skipping save");
        return;
    }

    // Create a safe filename from character name
    std::string safeFileName = g_ItemTracking.characterName;
    for (char& c : safeFileName) {
        if (c == '\\' || c == '/' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|') {
            c = '_';
        }
    }

    auto basePath = std::filesystem::current_path() / PATH_ROOT "tracking/";
    std::filesystem::create_directories(basePath);

    auto path = basePath / (safeFileName + ".json");

    Document doc;
    doc.SetObject();
    auto& al = doc.GetAllocator();

    // Save character name
    doc.AddMember("characterName", Value(g_ItemTracking.characterName.c_str(), al), al);

    // Save given items array
    Value givenItemsArray(kArrayType);
    for (const auto& [formID, entry] : g_ItemTracking.givenItems) {
        if (entry.count <= 0) continue;

        Value itemObj(kObjectType);
        itemObj.AddMember("formID", Value(entry.formID.c_str(), al), al);
        itemObj.AddMember("count", entry.count, al);

        if (entry.isEnhanced) {
            itemObj.AddMember("isEnhanced", true, al);
            if (!entry.baseFormID.empty()) {
                itemObj.AddMember("baseFormID", Value(entry.baseFormID.c_str(), al), al);
            }
            if (!entry.enhancementKey.empty()) {
                itemObj.AddMember("enhancementKey", Value(entry.enhancementKey.c_str(), al), al);
            }
        }

        givenItemsArray.PushBack(itemObj, al);
    }
    doc.AddMember("givenItems", givenItemsArray, al);

    // Save marked to keep array
    Value markedToKeepArray(kArrayType);
    for (const auto& formID : g_ItemTracking.markedToKeep) {
        markedToKeepArray.PushBack(Value(formID.c_str(), al), al);
    }
    doc.AddMember("markedToKeep", markedToKeepArray, al);

    // Save applied enchantments array
    Value appliedEnchArray(kArrayType);
    for (const auto& ench : g_ItemTracking.appliedEnchantments) {
        Value enchObj(kObjectType);
        enchObj.AddMember("dynamicFormID", Value(ench.dynamicFormID.c_str(), al), al);
        enchObj.AddMember("enchantmentFormID", Value(ench.enchantmentFormID.c_str(), al), al);
        enchObj.AddMember("baseArmorFormID", Value(ench.baseArmorFormID.c_str(), al), al);
        appliedEnchArray.PushBack(enchObj, al);
    }
    doc.AddMember("appliedEnchantments", appliedEnchArray, al);

    if (!WriteJSONFile(path, doc)) {
        logger::error("SaveItemTracking: Failed to write tracking file for '{}'", g_ItemTracking.characterName);
        return;
    }

    logger::debug("SaveItemTracking: Saved {} tracked items, {} applied enchantments for '{}'",
        g_ItemTracking.givenItems.size(), g_ItemTracking.appliedEnchantments.size(),
        g_ItemTracking.characterName);
}

void QuickArmorRebalance::TrackGivenItem(RE::TESBoundObject* item, bool isEnhanced, const std::string& baseFormID, const std::string& enhancementKey) {
    if (!item) return;

    std::string formID = QARFormID(item);
    logger::debug("TrackGivenItem: Tracking item '{}' ({}), enhanced={}", item->GetName(), formID, isEnhanced);

    auto& entry = g_ItemTracking.givenItems[formID];
    entry.formID = formID;
    entry.count++;
    entry.isEnhanced = isEnhanced;
    if (isEnhanced) {
        entry.baseFormID = baseFormID;
        entry.enhancementKey = enhancementKey;
    }

    SaveItemTracking();
}

void QuickArmorRebalance::TrackRemovedItem(RE::TESBoundObject* item) {
    if (!item) return;

    std::string formID = QARFormID(item);

    auto it = g_ItemTracking.givenItems.find(formID);
    if (it == g_ItemTracking.givenItems.end()) {
        logger::debug("TrackRemovedItem: Item '{}' ({}) not in tracking", item->GetName(), formID);
        return;
    }

    it->second.count--;
    logger::debug("TrackRemovedItem: Removed item '{}' ({}), count now {}", item->GetName(), formID, it->second.count);

    if (it->second.count <= 0) {
        g_ItemTracking.givenItems.erase(it);
        g_ItemTracking.markedToKeep.erase(formID);
    }

    SaveItemTracking();
}

void QuickArmorRebalance::MarkItemToKeep(RE::TESBoundObject* item, bool keep) {
    if (!item) return;

    std::string formID = QARFormID(item);
    logger::debug("MarkItemToKeep: Item '{}' ({}) keep={}", item->GetName(), formID, keep);

    if (keep) {
        g_ItemTracking.markedToKeep.insert(formID);
    } else {
        g_ItemTracking.markedToKeep.erase(formID);
    }

    SaveItemTracking();
}

bool QuickArmorRebalance::IsItemMarkedToKeep(RE::TESBoundObject* item) {
    if (!item) return false;

    std::string formID = QARFormID(item);
    return g_ItemTracking.markedToKeep.contains(formID);
}

void QuickArmorRebalance::TrackAppliedEnchantment(RE::TESBoundObject* dynamicArmor, RE::EnchantmentItem* enchantment, RE::TESBoundObject* baseArmor) {
    if (!dynamicArmor || !enchantment) return;

    AppliedEnchantment entry;
    entry.dynamicFormID = QARFormID(dynamicArmor);
    entry.enchantmentFormID = QARFormID(enchantment);
    entry.baseArmorFormID = baseArmor ? QARFormID(baseArmor) : "";

    // Check if already tracked
    for (const auto& existing : g_ItemTracking.appliedEnchantments) {
        if (existing.dynamicFormID == entry.dynamicFormID) {
            return;  // Already tracked
        }
    }

    g_ItemTracking.appliedEnchantments.push_back(entry);
    logger::info("TrackAppliedEnchantment: Tracking enchantment {} on dynamic armor {}",
        enchantment->GetName(), dynamicArmor->GetName());

    SaveItemTracking();
}

void QuickArmorRebalance::CleanupOrphanedEnchantments() {
    auto player = RE::PlayerCharacter::GetSingleton();
    if (!player) {
        logger::warn("CleanupOrphanedEnchantments: Player not available");
        return;
    }

    if (g_ItemTracking.appliedEnchantments.empty()) {
        logger::debug("CleanupOrphanedEnchantments: No enchantments to clean up");
        return;
    }

    logger::info("CleanupOrphanedEnchantments: Checking {} tracked enchantments",
        g_ItemTracking.appliedEnchantments.size());

    // Get player's magic target for dispelling
    auto magicTarget = player->AsMagicTarget();
    if (!magicTarget) {
        logger::warn("CleanupOrphanedEnchantments: Player has no MagicTarget");
        return;
    }

    // Build set of enchantment FormIDs that need to be cleaned up (orphaned dynamic forms)
    std::unordered_set<RE::FormID> enchToClean;
    std::vector<AppliedEnchantment> stillValid;

    for (const auto& entry : g_ItemTracking.appliedEnchantments) {
        // Check if the dynamic form still exists
        auto dynamicForm = LookupForm(entry.dynamicFormID);

        if (dynamicForm) {
            // Form still exists - keep tracking
            stillValid.push_back(entry);
            continue;
        }

        // Form is gone - mark enchantment for cleanup
        logger::info("CleanupOrphanedEnchantments: Dynamic form {} is gone, will clean enchantment {}",
            entry.dynamicFormID, entry.enchantmentFormID);

        auto enchForm = LookupForm(entry.enchantmentFormID);
        if (enchForm) {
            enchToClean.insert(enchForm->GetFormID());
            logger::info("CleanupOrphanedEnchantments: Added enchantment {:08X} to cleanup list", enchForm->GetFormID());
        }
    }

    if (enchToClean.empty()) {
        logger::debug("CleanupOrphanedEnchantments: No orphaned enchantments to clean");
        g_ItemTracking.appliedEnchantments = std::move(stillValid);
        return;
    }

    // Get active effects and dispel matching ones
    // This is more aggressive than DispelEffect - we directly iterate and dispel
    auto activeEffects = magicTarget->GetActiveEffectList();
    int cleanedUp = 0;

    if (activeEffects) {
        logger::info("CleanupOrphanedEnchantments: Checking active effects on player");

        // Collect effects to dispel (don't modify list while iterating)
        std::vector<RE::ActiveEffect*> toDispel;

        for (auto& effect : *activeEffects) {
            if (!effect) continue;

            // Get the spell/enchantment that caused this effect
            auto spell = effect->spell;
            if (!spell) {
                logger::trace("CleanupOrphanedEnchantments: Effect has no spell, skipping");
                continue;
            }

            // Check if this is one of our orphaned enchantments
            if (enchToClean.contains(spell->GetFormID())) {
                logger::info("CleanupOrphanedEnchantments: Found orphaned effect from {} (FormID {:08X})",
                    spell->GetName(), spell->GetFormID());
                toDispel.push_back(effect);
            }
        }

        // Now dispel collected effects
        for (auto* effect : toDispel) {
            logger::info("CleanupOrphanedEnchantments: Dispelling effect...");
            effect->Dispel(true);  // Force dispel
            cleanedUp++;
        }
    } else {
        logger::warn("CleanupOrphanedEnchantments: Could not get active effects list");
    }

    // Update tracking to only include valid entries
    g_ItemTracking.appliedEnchantments = std::move(stillValid);

    logger::info("CleanupOrphanedEnchantments: Cleaned up {} orphaned enchantment effects", cleanedUp);
    SaveItemTracking();
}

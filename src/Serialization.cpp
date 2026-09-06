#include "Serialization.h"
#include "Data.h"
#include "UI.h"

namespace QuickArmorRebalance {

    // Currently tracked enhanced armors (keyed by enhanced form's FormID)
    static std::unordered_map<RE::FormID, SerializedEnhancedArmor> g_TrackedArmors;

    // Recreated armors after load (keyed by base armor FormID)
    static std::unordered_map<RE::FormID, RE::TESObjectARMO*> g_RecreatedArmors;

    // Helper to write a string to the cosave
    static bool WriteString(SKSE::SerializationInterface* a_intfc, const std::string& str) {
        std::uint32_t len = static_cast<std::uint32_t>(str.length());
        if (!a_intfc->WriteRecordData(len)) return false;
        if (len > 0 && !a_intfc->WriteRecordData(str.data(), len)) return false;
        return true;
    }

    // Helper to read a string from the cosave
    static bool ReadString(SKSE::SerializationInterface* a_intfc, std::string& str) {
        std::uint32_t len = 0;
        if (!a_intfc->ReadRecordData(len)) return false;
        if (len > 0) {
            str.resize(len);
            if (!a_intfc->ReadRecordData(str.data(), len)) return false;
        } else {
            str.clear();
        }
        return true;
    }

    static void SaveCallback(SKSE::SerializationInterface* a_intfc) {
        logger::info("Serialization: Saving {} tracked enhanced armors", g_TrackedArmors.size());

        for (const auto& [formID, data] : g_TrackedArmors) {
            if (!a_intfc->OpenRecord(kEnhancedArmor, kSerializationVersion)) {
                logger::error("Serialization: Failed to open record for enhanced armor {:08X}", formID);
                continue;
            }

            // Write base armor info
            if (!a_intfc->WriteRecordData(data.baseArmorFormID)) {
                logger::error("Serialization: Failed to write base armor FormID");
                continue;
            }
            if (!WriteString(a_intfc, data.baseArmorModName)) {
                logger::error("Serialization: Failed to write base armor mod name");
                continue;
            }

            // Write enchantment info
            if (!a_intfc->WriteRecordData(data.enchantmentFormID)) continue;
            if (!WriteString(a_intfc, data.enchantmentModName)) continue;
            if (!a_intfc->WriteRecordData(data.enchantmentMagnitude)) continue;

            // Write stats transfer info
            bool hasAR = data.armorRating.has_value();
            bool hasWeight = data.weight.has_value();
            bool hasValue = data.value.has_value();
            if (!a_intfc->WriteRecordData(hasAR)) continue;
            if (hasAR && !a_intfc->WriteRecordData(data.armorRating.value())) continue;
            if (!a_intfc->WriteRecordData(hasWeight)) continue;
            if (hasWeight && !a_intfc->WriteRecordData(data.weight.value())) continue;
            if (!a_intfc->WriteRecordData(hasValue)) continue;
            if (hasValue && !a_intfc->WriteRecordData(data.value.value())) continue;

            // Write equipped state
            if (!a_intfc->WriteRecordData(data.wasEquipped)) continue;

            logger::trace("Serialization: Saved enhanced armor - base {:08X} ({})",
                data.baseArmorFormID, data.baseArmorModName);
        }
    }

    static void LoadCallback(SKSE::SerializationInterface* a_intfc) {
        logger::info("Serialization: Loading enhanced armors from cosave");
        g_RecreatedArmors.clear();

        std::uint32_t type, version, length;
        std::vector<SerializedEnhancedArmor> toRecreate;

        while (a_intfc->GetNextRecordInfo(type, version, length)) {
            if (type != kEnhancedArmor) {
                logger::warn("Serialization: Unknown record type {:08X}", type);
                continue;
            }

            if (version != kSerializationVersion) {
                logger::warn("Serialization: Version mismatch (got {}, expected {})", version, kSerializationVersion);
                continue;
            }

            SerializedEnhancedArmor data;

            // Read base armor info
            if (!a_intfc->ReadRecordData(data.baseArmorFormID)) continue;
            if (!ReadString(a_intfc, data.baseArmorModName)) continue;

            // Resolve the FormID (may have changed due to load order)
            RE::FormID resolvedBaseID = 0;
            if (!a_intfc->ResolveFormID(data.baseArmorFormID, resolvedBaseID)) {
                logger::warn("Serialization: Failed to resolve base armor FormID {:08X}", data.baseArmorFormID);
                continue;
            }
            data.baseArmorFormID = resolvedBaseID;

            // Read enchantment info
            if (!a_intfc->ReadRecordData(data.enchantmentFormID)) continue;
            if (!ReadString(a_intfc, data.enchantmentModName)) continue;
            if (!a_intfc->ReadRecordData(data.enchantmentMagnitude)) continue;

            // Resolve enchantment FormID if present
            if (data.enchantmentFormID != 0) {
                RE::FormID resolvedEnchID = 0;
                if (!a_intfc->ResolveFormID(data.enchantmentFormID, resolvedEnchID)) {
                    logger::warn("Serialization: Failed to resolve enchantment FormID {:08X}", data.enchantmentFormID);
                    data.enchantmentFormID = 0;  // Clear it, we can still recreate without enchantment
                } else {
                    data.enchantmentFormID = resolvedEnchID;
                }
            }

            // Read stats transfer info
            bool hasAR = false, hasWeight = false, hasValue = false;
            if (!a_intfc->ReadRecordData(hasAR)) continue;
            if (hasAR) {
                uint32_t ar;
                if (!a_intfc->ReadRecordData(ar)) continue;
                data.armorRating = ar;
            }
            if (!a_intfc->ReadRecordData(hasWeight)) continue;
            if (hasWeight) {
                float w;
                if (!a_intfc->ReadRecordData(w)) continue;
                data.weight = w;
            }
            if (!a_intfc->ReadRecordData(hasValue)) continue;
            if (hasValue) {
                int32_t v;
                if (!a_intfc->ReadRecordData(v)) continue;
                data.value = v;
            }

            // Read equipped state
            if (!a_intfc->ReadRecordData(data.wasEquipped)) continue;

            logger::info("Serialization: Loaded enhanced armor data - base {:08X}, ench {:08X}, equipped {}",
                data.baseArmorFormID, data.enchantmentFormID, data.wasEquipped);

            toRecreate.push_back(data);
        }

        // Now recreate the forms (must be done after reading all data)
        // We schedule this on the task interface to ensure it happens at the right time
        if (!toRecreate.empty()) {
            logger::info("Serialization: Scheduling recreation of {} enhanced armors", toRecreate.size());

            // Copy data for the lambda
            auto dataPtr = std::make_shared<std::vector<SerializedEnhancedArmor>>(std::move(toRecreate));

            SKSE::GetTaskInterface()->AddTask([dataPtr]() {
                logger::info("Serialization: Recreating {} enhanced armors", dataPtr->size());
                auto player = RE::PlayerCharacter::GetSingleton();
                if (!player) {
                    logger::error("Serialization: Player not available for recreation");
                    return;
                }

                for (const auto& data : *dataPtr) {
                    // Look up the base armor
                    auto baseForm = RE::TESForm::LookupByID(data.baseArmorFormID);
                    if (!baseForm) {
                        logger::warn("Serialization: Base armor {:08X} not found", data.baseArmorFormID);
                        continue;
                    }

                    auto baseArmor = baseForm->As<RE::TESObjectARMO>();
                    if (!baseArmor) {
                        logger::warn("Serialization: Form {:08X} is not armor", data.baseArmorFormID);
                        continue;
                    }

                    // Build the EnhancedItemConfig from serialized data
                    EnhancedItemConfig config;

                    if (data.enchantmentFormID != 0) {
                        // Build FormID string (ModName.esp:0xFormID format)
                        auto enchForm = RE::TESForm::LookupByID(data.enchantmentFormID);
                        if (enchForm) {
                            config.enchantmentFormID = QARFormID(enchForm);
                            config.enchantmentMagnitude = data.enchantmentMagnitude;
                        }
                    }

                    config.armorRating = data.armorRating;
                    config.weight = data.weight;
                    config.value = data.value;

                    if (!config.IsEnhanced()) {
                        logger::warn("Serialization: No valid enhancements for base armor {:08X}", data.baseArmorFormID);
                        continue;
                    }

                    // Recreate the enhanced armor (global scope function from UI.cpp)
                    auto enhanced = ::CreateEnhancedArmor(baseArmor, config);
                    if (!enhanced) {
                        logger::warn("Serialization: Failed to recreate enhanced armor for {:08X}", data.baseArmorFormID);
                        continue;
                    }

                    logger::info("Serialization: Recreated enhanced {} (FormID {:08X})",
                        baseArmor->GetName(), enhanced->GetFormID());

                    // Store for lookup
                    g_RecreatedArmors[data.baseArmorFormID] = enhanced;

                    // Re-equip if it was equipped before
                    if (data.wasEquipped) {
                        // CRITICAL: Before equipping, dispel any baked enchantment effects from the save
                        // The game saves active effects, so the enchantment from the old dynamic form
                        // is still active even though that form no longer exists
                        if (data.enchantmentFormID != 0) {
                            auto magicTarget = player->AsMagicTarget();
                            if (magicTarget) {
                                auto activeEffects = magicTarget->GetActiveEffectList();
                                if (activeEffects) {
                                    std::vector<RE::ActiveEffect*> toDispel;
                                    for (auto& effect : *activeEffects) {
                                        if (!effect || !effect->spell) continue;
                                        if (effect->spell->GetFormID() == data.enchantmentFormID) {
                                            toDispel.push_back(effect);
                                        }
                                    }
                                    for (auto* effect : toDispel) {
                                        logger::info("Serialization: Dispelling baked enchantment effect before re-equip");
                                        effect->Dispel(true);
                                    }
                                    if (!toDispel.empty()) {
                                        logger::info("Serialization: Dispelled {} baked effects for {}",
                                            toDispel.size(), baseArmor->GetName());
                                    }
                                }
                            }
                        }

                        // First we need to add it to inventory
                        player->AddObjectToContainer(enhanced, nullptr, 1, nullptr);
                        logger::info("Serialization: Added enhanced {} to player inventory", baseArmor->GetName());

                        // Then equip it
                        auto equipManager = RE::ActorEquipManager::GetSingleton();
                        if (equipManager) {
                            equipManager->EquipObject(player, enhanced, nullptr, 1, nullptr, true, false, false);
                            logger::info("Serialization: Re-equipped enhanced {}", baseArmor->GetName());
                        }
                    }

                    // Re-track the armor
                    SerializedEnhancedArmor newData = data;
                    g_TrackedArmors[enhanced->GetFormID()] = newData;
                }
            });
        }

        g_TrackedArmors.clear();  // Will be repopulated by the task
    }

    static void RevertCallback(SKSE::SerializationInterface* a_intfc) {
        logger::info("Serialization: Reverting - clearing tracked armors");
        g_TrackedArmors.clear();
        g_RecreatedArmors.clear();
    }

    void InitializeSerialization() {
        auto serialization = SKSE::GetSerializationInterface();
        if (!serialization) {
            logger::error("Serialization: Failed to get serialization interface");
            return;
        }

        serialization->SetUniqueID(kSerializationID);
        serialization->SetSaveCallback(SaveCallback);
        serialization->SetLoadCallback(LoadCallback);
        serialization->SetRevertCallback(RevertCallback);

        logger::info("Serialization: Initialized with ID '{}'",
            std::string(reinterpret_cast<const char*>(&kSerializationID), 4));
    }

    void TrackEquippedEnhancedArmor(RE::TESObjectARMO* baseArmor, RE::TESObjectARMO* enhancedArmor,
                                     const EnhancedItemConfig& config, bool equipped) {
        if (!baseArmor || !enhancedArmor) return;

        SerializedEnhancedArmor data;
        data.baseArmorFormID = baseArmor->GetFormID();

        // Get mod name for the base armor
        if (auto file = baseArmor->GetFile(0)) {
            data.baseArmorModName = file->fileName;
        }

        // Store enchantment info
        if (config.HasEnchantment()) {
            if (auto enchForm = LookupForm(config.enchantmentFormID)) {
                data.enchantmentFormID = enchForm->GetFormID();
                if (auto file = enchForm->GetFile(0)) {
                    data.enchantmentModName = file->fileName;
                }
            }
            data.enchantmentMagnitude = config.enchantmentMagnitude;
        }

        // Store stats transfer info
        data.armorRating = config.armorRating;
        data.weight = config.weight;
        data.value = config.value;

        data.wasEquipped = equipped;

        g_TrackedArmors[enhancedArmor->GetFormID()] = data;

        logger::info("Serialization: Tracking enhanced armor {} (FormID {:08X}), equipped={}",
            baseArmor->GetName(), enhancedArmor->GetFormID(), equipped);
    }

    void UntrackEnhancedArmor(RE::TESObjectARMO* enhancedArmor) {
        if (!enhancedArmor) return;

        auto it = g_TrackedArmors.find(enhancedArmor->GetFormID());
        if (it != g_TrackedArmors.end()) {
            logger::info("Serialization: Untracking enhanced armor {:08X}", enhancedArmor->GetFormID());
            g_TrackedArmors.erase(it);
        }
    }

    void ClearTrackedArmors() {
        logger::info("Serialization: Clearing {} tracked armors", g_TrackedArmors.size());
        g_TrackedArmors.clear();
        g_RecreatedArmors.clear();
    }

    RE::TESObjectARMO* GetRecreatedEnhancedArmor(RE::TESObjectARMO* baseArmor) {
        if (!baseArmor) return nullptr;

        auto it = g_RecreatedArmors.find(baseArmor->GetFormID());
        if (it != g_RecreatedArmors.end()) {
            return it->second;
        }
        return nullptr;
    }
}

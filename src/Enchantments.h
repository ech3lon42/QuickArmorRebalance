#pragma once

#include "Data.h"

namespace QuickArmorRebalance {
    void InstallEnchantmentHooks();
    void LoadEnchantmentConfigs(std::filesystem::path path, rapidjson::Document& d);

    bool IsEnchanted(RE::TESBoundObject* obj);

    void FinalizeEnchantmentConfig();

    // Apply enchantment from enhanced item config to an item in player's inventory
    // Returns true if enchantment was successfully applied
    bool ApplyEnhancedEnchantment(RE::TESBoundObject* item, const EnhancedItemConfig& config);
}
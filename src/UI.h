#pragma once

#include "Data.h"

namespace QuickArmorRebalance {
    void RenderUI();
}

// These functions are defined at global scope in UI.cpp (after using namespace QuickArmorRebalance)
// Create a dynamically duplicated armor form with enchantment and/or modified stats
RE::TESObjectARMO* CreateEnhancedArmor(RE::TESObjectARMO* baseArmor, const QuickArmorRebalance::EnhancedItemConfig& config);

// Generate a cache key for an enhanced armor configuration
std::string MakeEnhancedArmorKey(RE::TESObjectARMO* baseArmor, const QuickArmorRebalance::EnhancedItemConfig& config);
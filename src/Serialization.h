#pragma once

namespace QuickArmorRebalance {
    // SKSE cosave serialization for persisting enhanced armor across save/load
    // This solves the problem of dynamic forms being lost on reload

    constexpr std::uint32_t kSerializationVersion = 1;
    constexpr std::uint32_t kSerializationID = 'QARN';  // QuickArmorRebalanceNG

    // Record types
    constexpr std::uint32_t kEnhancedArmor = 'EARM';

    // Stored data for an equipped enhanced armor
    struct SerializedEnhancedArmor {
        RE::FormID baseArmorFormID = 0;
        std::string baseArmorModName;  // For cross-save compatibility

        // Enchantment config
        RE::FormID enchantmentFormID = 0;
        std::string enchantmentModName;
        float enchantmentMagnitude = 1.0f;

        // Stats transfer config
        std::optional<uint32_t> armorRating;
        std::optional<float> weight;
        std::optional<int32_t> value;

        bool wasEquipped = false;
    };

    // Initialize serialization callbacks - call during plugin load
    void InitializeSerialization();

    // Track an enhanced armor that should be persisted
    // Call when equipping enhanced armor
    void TrackEquippedEnhancedArmor(RE::TESObjectARMO* baseArmor, RE::TESObjectARMO* enhancedArmor,
                                     const struct EnhancedItemConfig& config, bool equipped);

    // Remove tracking when unequipping
    void UntrackEnhancedArmor(RE::TESObjectARMO* enhancedArmor);

    // Clear all tracked armors (called on revert)
    void ClearTrackedArmors();

    // Get the recreated form for a base armor after load (if any)
    RE::TESObjectARMO* GetRecreatedEnhancedArmor(RE::TESObjectARMO* baseArmor);
}

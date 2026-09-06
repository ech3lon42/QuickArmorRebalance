#pragma once

#include "Data.h"

namespace QuickArmorRebalance::NPCTargets {
    enum class Action { Give, Equip, Unequip, Toggle, UnequipAll, Restore, Remove, Undo, RemoveAll };

    // Tick only queues work; all discovery and NPC mutations execute on the game thread.
    void Initialize();
    void Suspend();
    void Resume();
    void Tick(bool uiOpen);

    // Holds a strong reference for the lifetime of one UI frame, never across frames.
    struct FrameTarget {
        FrameTarget();
        ~FrameTarget();
    };
    RE::Actor* GetActor();
    bool IsNPC();
    std::uint64_t Revision();
    std::uint64_t Generation();
    int TrackedCount();
    int ItemCount(RE::Actor* actor, RE::TESBoundObject* item);
    std::vector<RE::TESObjectARMO*> InventoryArmor(RE::Actor* actor);
    void DrawControls();
    void Request(Action action, RE::TESBoundObject* item = nullptr, bool equip = false,
                 bool reuse = false, const EnhancedItemConfig* enhancement = nullptr);

    // Dynamic armor must be saved as its stable base form plus enhancement recipe.
    void RegisterEnhanced(RE::TESObjectARMO* enhanced, RE::TESObjectARMO* base, const EnhancedItemConfig& config);
}

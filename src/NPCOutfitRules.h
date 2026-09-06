#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <string>
#include <string_view>

// Engine-independent rules, also exercised by tests/NPCOutfitRulesTests.cpp.
namespace QuickArmorRebalance::NPCTargets::Rules {
    inline constexpr int minRadius = 512;
    inline constexpr int defaultRadius = 4096;
    inline constexpr int maxRadius = 16384;

    inline int ClampRadius(int configuredRadius) {
        return std::clamp(configuredRadius, minRadius, maxRadius);
    }

    inline bool InRange(float distanceSquared, int configuredRadius = defaultRadius) {
        const auto radius = static_cast<float>(ClampRadius(configuredRadius));
        return std::isfinite(distanceSquared) && distanceSquared >= 0 && distanceSquared <= radius * radius;
    }

    inline bool SameSpace(bool interior, bool playerInterior, std::uint32_t cell, std::uint32_t playerCell,
                          std::uint32_t world, std::uint32_t playerWorld) {
        if (!cell || !playerCell) return false;
        if (interior || playerInterior) return cell == playerCell;
        return world != 0 && world == playerWorld;
    }

    inline std::string FileName(std::string_view stableReference) {
        // Never use a display name or a raw filename from JSON as a path component.
        std::uint64_t hash = 14695981039346656037ull;
        for (unsigned char ch : stableReference) { hash ^= ch; hash *= 1099511628211ull; }
        return std::format("NPC_{:016X}.json", hash);
    }
}

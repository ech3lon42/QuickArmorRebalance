#include "../src/NPCOutfitRules.h"

#include <cassert>
#include <iostream>
#include <limits>

namespace Rules = QuickArmorRebalance::NPCTargets::Rules;

int main() {
    assert(Rules::InRange(0));
    assert(Rules::InRange(Rules::defaultRadius * Rules::defaultRadius));
    assert(!Rules::InRange((Rules::defaultRadius + 1.0f) * (Rules::defaultRadius + 1.0f)));
    assert(!Rules::InRange(-1));
    assert(!Rules::InRange(std::numeric_limits<float>::infinity()));
    assert(!Rules::InRange(std::numeric_limits<float>::quiet_NaN()));

    assert(Rules::ClampRadius(Rules::defaultRadius) == Rules::defaultRadius);
    assert(Rules::ClampRadius(std::numeric_limits<int>::min()) == Rules::minRadius);
    assert(Rules::ClampRadius(std::numeric_limits<int>::max()) == Rules::maxRadius);
    assert(Rules::ClampRadius(Rules::minRadius) == Rules::minRadius);
    assert(Rules::ClampRadius(Rules::maxRadius) == Rules::maxRadius);
    assert(Rules::InRange(512.0f * 512.0f, Rules::minRadius));
    assert(!Rules::InRange(513.0f * 513.0f, Rules::minRadius));
    assert(Rules::InRange(8192.0f * 8192.0f, 8192));
    assert(!Rules::InRange(8192.0f * 8192.0f, Rules::defaultRadius));
    assert(Rules::InRange(16384.0f * 16384.0f, Rules::maxRadius));
    assert(!Rules::InRange(16385.0f * 16385.0f, Rules::maxRadius));

    assert(Rules::SameSpace(true, true, 1, 1, 0, 0));
    assert(!Rules::SameSpace(true, true, 1, 2, 0, 0));
    assert(!Rules::SameSpace(true, false, 1, 2, 7, 7));
    assert(Rules::SameSpace(false, false, 1, 2, 7, 7));
    assert(!Rules::SameSpace(false, false, 1, 2, 7, 8));
    assert(!Rules::SameSpace(false, false, 1, 2, 0, 0));
    assert(!Rules::SameSpace(true, true, 0, 0, 0, 0));

    const auto key = Rules::FileName("Skyrim.esm:0xa2c94");
    assert(key == Rules::FileName("Skyrim.esm:0xa2c94"));
    assert(key != Rules::FileName("Skyrim.esm:0xa2c95"));  // Same template/name, different placed reference.
    assert(key != Rules::FileName("AnotherMod.esp:0xa2c94"));
    assert(key.size() == 25 && key.ends_with(".json"));
    // Even malicious input cannot escape npc_outfits or create nested paths.
    auto hostile = Rules::FileName("../../bad:path\\with/slashes:0x1");
    assert(hostile.find_first_of("/\\:") == std::string::npos);
    std::cout << "NPC outfit rules: 29 checks passed\n";
}

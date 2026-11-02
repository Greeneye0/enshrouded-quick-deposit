#include "../dev/altar_upgrade.h"
#include <cassert>
#include <cstring>
#include <limits>
#include <cstdio>

int main() {
    altar_upgrade::Command cmd{};
    assert(altar_upgrade::Encode(1025, 3858442422u, cmd));
    // Independent command bytes: station 1025, recipe e5fb28b6, no categories, amount one.
    const uint8_t expected[48] = {0x37,0,0,0, 1,4,0,0, 0xb6,0x28,0xfb,0xe5,
        0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0, 0,0,0,0,
        0,0,0,0, 0,0,0,0, 1,0,0,0};
    assert(std::memcmp(&cmd, expected, sizeof(cmd)) == 0);
    assert(altar_upgrade::Encode(1025, 2390035933u, cmd));
    assert(altar_upgrade::Encode(1025, 1759422421u, cmd));
    assert(!altar_upgrade::Encode(0, 3858442422u, cmd));
    assert(!altar_upgrade::Encode(1025, 587829552u, cmd)); // claim is not an expansion
    assert(!altar_upgrade::Encode(1025, 687239620u, cmd)); // global Flame is not altar coverage
    assert(!altar_upgrade::Encode(1025, 0, cmd));
    assert(altar_upgrade::EncodeFlame(77, 687239620u, cmd));
    assert(cmd.station == 77 && cmd.recipe == 687239620u && cmd.amount == 1 && cmd.code == 0x37);
    for (auto selected : cmd.categorySelections) assert(selected == 0);
    assert(altar_upgrade::EncodeFlame(77, 177141070u, cmd));
    assert(cmd.station == 77 && cmd.recipe == 177141070u && cmd.amount == 1);
    assert(altar_upgrade::FindFlame(177141070u)->requiredHappyNpcCount == 2);
    assert(!altar_upgrade::EncodeFlame(0, 687239620u, cmd));
    assert(!altar_upgrade::EncodeFlame(77, 584380149u, cmd)); // Flame3->4 is out of scope
    assert(!altar_upgrade::EncodeFlame(77, 3858442422u, cmd)); // altar recipe cannot target progress station
    const float player[] = {3744.5f, 844, 1427.5f};
    float altar[] = {3752.5f, 844, 1427.5f};
    assert(altar_upgrade::Nearby(player, altar));
    altar[0] += .01f; assert(!altar_upgrade::Nearby(player, altar));
    altar[0] = 3744.5f; altar[1] = 800; assert(altar_upgrade::InTestArea(altar));
    altar[1] = 799.99f; assert(!altar_upgrade::InTestArea(altar));
    altar[1] = 900.01f; assert(!altar_upgrade::InTestArea(altar));
    altar[1] = 844; altar[0] = 3699.99f; assert(!altar_upgrade::InTestArea(altar));
    altar[0] = 3860.01f; assert(!altar_upgrade::InTestArea(altar));
    altar[0] = 3744.5f; altar[2] = 1369.99f; assert(!altar_upgrade::InTestArea(altar));
    altar[2] = 1520.01f; assert(!altar_upgrade::InTestArea(altar));
    altar[2] = std::numeric_limits<float>::quiet_NaN(); assert(!altar_upgrade::Nearby(player, altar));
    altar[2] = std::numeric_limits<float>::infinity(); assert(!altar_upgrade::Nearby(player, altar));
    std::puts("altar upgrade: native packet, recipe whitelist, bounds and proximity passed");
}

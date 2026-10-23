#pragma once
#include <cstdint>
#include <string>
#include <vector>

#include "inventory.h"

// Kept items: amounts of items you marked in the inventory, which a deposit leaves in your
// backpack.
//
// Hover a stack in an open inventory and press keepKey (default K): Quick Deposit keeps that many
// of the item (split a stack first to keep part of it). Again on a kept item unmarks it. The
// stacks that make up the kept amount get a teal border. The marks are stored in
// quick-deposit.ini (keepItems, "Twigs x20"; a name alone keeps every one), so they survive a
// restart and can be edited by hand.
namespace keep {

constexpr uint32_t kAll = 0xFFFFFFFFu;

// How many of each stack in the backpack's storage rows a deposit leaves there, by page index and
// slot: 0 none, the stack's count all of it, anything between a part of it.
struct Plan {
    uint32_t keep[inv::kMaxPages + 1][inv::kSlotsPerPage];
};

// Hooks the inventory slot drawing. Call before the game's code runs. Without it (signatures
// not found) marks from the ini still keep items, but nothing is drawn and keepKey does nothing.
bool Init();

// The marked names (from keepItems) and the toggle key.
void Load(const char* list, int key);

// True if this item type is marked.
bool IsKept(uint32_t itemId);
bool Any();

// Which stacks of backpack make up the kept amounts: a stack of exactly the amount if there is
// one, else the largest stacks that fit, then the rest from the smallest larger stack.
void PlanFor(const inv::Owner& backpack, Plan& out);

// Your backpack, read twice a second: only its storage rows (not the hotbar) show marks and take
// keepKey.
void SetBackpack(const inv::Owner& backpack);

// True while your backpack's slots are on screen (an inventory or chest is open).
bool BackpackShown();

// Once per UI frame (game thread): handles keepKey over a hovered slot.
void Tick();

// --- for the dev bridge ---

struct SlotSeen {
    float rect[4];
    uint32_t itemId;
    uint8_t data[32]; // slot data: +0 page entity, +4 slot index, +8 item id, +0x10 count
};
std::vector<SlotSeen> LastFrameSlots();
// Marks the item to keep count of it (0: every one), or unmarks a marked item.
bool Toggle(uint32_t itemId, uint32_t count);

} // namespace keep

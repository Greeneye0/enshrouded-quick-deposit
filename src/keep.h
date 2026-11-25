#pragma once
#include "../../common/input_binds.h"
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
// quick-deposit.ini, a list for each of your characters ([Keep.<character id>] keepItems, "Twigs
// x20"; a name alone keeps every one), the same in every world, so they survive a restart and
// can be edited by hand.
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
// The keep mark's key and controller button (keepKey in the ini).
void SetBinding(const greeneye::input::Binding& binding);

// The marks of this character (character.h), from its own list in quick-deposit.ini. The first
// character without a list takes over the shared keepItems list of older versions; any other
// starts with none. Nothing happens for "" or the character already loaded.
void UseCharacter(const std::string& id);

// Once per UI frame: the marks of the character being played (character.h). While it is not known
// yet, none, and Waiting() is true: deposits wait, or they would take that character's kept items.
void FollowCharacter();
bool Waiting();

// True if this item type is marked.
bool IsKept(uint32_t itemId);
bool Any();

// A kept item type by id (amount kAll: every one), as the host gets them from a player.
struct KeptItem {
    uint32_t itemId;
    uint32_t amount;
};

// Your marks as item ids: the marked types in your backpack. version changes whenever the list
// does. Any thread.
std::vector<KeptItem> Snapshot(uint64_t* version = nullptr);

// Which stacks keep a list of item types (another player's, on the host), the same way as
// PlanFor; every page of backpack counts.
void PlanForItems(const inv::Owner& backpack, const std::vector<KeptItem>& items, Plan& out);

// Which stacks of backpack make up the kept amounts: a stack of exactly the amount if there is
// one, else the largest stacks that fit, then the rest from the smallest larger stack.
void PlanFor(const inv::Owner& backpack, Plan& out);

// Your backpack, read twice a second: only its storage rows (not the hotbar) show marks and take
// keepKey.
void SetBackpack(const inv::Owner& backpack);

// True while your backpack's slots are on screen (an inventory or chest is open).
bool BackpackShown();

// True while slots of an inventory that isn't yours are on screen (a chest is open).
bool ChestShown();

// Once per UI frame (game thread): handles keepKey over a hovered slot.
void Tick();

// True while the mouse (pad: the controller's selection) is on an item in your backpack's storage
// rows, not the hotbar, the weapon bars or a chest.
bool OverBackpackSlot(bool pad);

// --- for the dev bridge ---

struct SlotSeen {
    float rect[4];
    uint32_t itemId;
    uint8_t data[32]; // slot data: +0 page entity, +4 slot index, +8 item id, +0x10 count
    bool selected;    // the slot a controller has selected
};
std::vector<SlotSeen> LastFrameSlots();
// The backpack slots kept now, with the copy each holds (inv::CopyOf, -1 unknown). Dev bridge.
struct KeptSlotInfo {
    uint32_t page, slot, itemId, amount;
    int copy;
};
std::vector<KeptSlotInfo> KeptSlotsNow();
// Marks the item to keep count of it (0: every one), or unmarks a marked item.
bool Toggle(uint32_t itemId, uint32_t count);

// Marks of items that do not stack (gear) whose item you put away: none on you (backpack,
// hotbar or worn) and one in these chests (your base's storage). Such a mark would otherwise
// keep the next copy you pick up. Not cleared for an item anywhere else (a grave, a chest
// outside the base), nor for one not seen on you this session (a spare copy in a chest while
// your pages still read wrong after loading), nor before two checks in a row agree. Returns how
// many marks ended. UI thread.
int UnmarkStored(const inv::Owner& player, const inv::Owner* chests, int count);
// True if any mark keeps an amount (gear and old amount marks), so UnmarkStored has work.
bool AnyAmountMark();

} // namespace keep

#pragma once
#include <string>
#include <vector>
#include "../../common/greeneye.h"
#include <cstdint>

// Read-only view of the client's inventories: your backpack and the base's chests.
//
// An inventory owner (player, chest) has an InventorySetup component that links up to 16
// "page" entities, each with an Inventory component of 8 ItemStacks. Items are described by
// ItemInfo records from the client's item database.
namespace inv {

constexpr int kSlotsPerPage = 8;
constexpr int kMaxPages = 16;
// The Position component's type: int64 x, y, z with 32 fraction bits. 133 in the plain game, but
// type numbers shift when other mods add components (EML Tweaks put InventorySetup at 133), so it
// is detected on the local player (DetectPosition); 133 until then.
uint16_t TypePosition();
// True once after Ready() found that the Inventory (and so InventorySetup) type it guessed at first
// was wrong: owners read before then are worth reading again.
bool TypesChanged();
// Finds the Position type: the component of your own entity that holds where the game says you
// are. Call with your position in meters (the world's copy); cheap once found.
void DetectPosition(uint32_t playerId, const float pos[3]);

// InventoryCategory of a page link
enum class PageKind : uint8_t { Invalid = 0, Customization = 1, Equipment = 2, Currency = 3, Generic = 4, Virtual = 5 };

struct Stack {
    uint32_t itemId;
    uint32_t count;
    uint32_t pide;
};

struct Page {
    uint32_t entity;
    PageKind kind;
    Stack slots[kSlotsPerPage];
};

struct Owner {
    uint32_t entity;
    int pageCount;
    Page pages[kMaxPages + 1];
};

struct ItemDesc {
    bool known;
    uint8_t category;       // keen::ItemCategory
    uint8_t weaponType[16]; // ItemInfo.weaponCategoryReference, weapons only
    uint8_t ammoType;       // ItemInfo.ammunitionType: 1 arrow, 2 spell, 3 fishing bait
    uint8_t equipType[16];  // equipment only: what it is worn as (a reference; same for every helmet)
    uint8_t equipSlot;      // equipment only: 0x0d head, 0x0e chest, 0x0f hands, 0x10 legs, 0x11 feet,
                            // 0x12 shield, 0x25 ring, 0x26 backpack (build 1273043)
    uint16_t maxStack;
    char name[64];  // readable: "Wand Epic Fire"
    char raw[96];   // internal: "Weapon_T1_1H_WandEpic_01_Fire"
    char shown[96]; // as the game shows it, in its language ("Raw Wolf Meat"); else name. For
                    // chat and reports only: chest matching and keep marks use name and raw.
};

// Hooks ViewInit (to learn the client's ECS registry) and the item lookup (to learn the item
// database). Call before the game's code runs.
bool Init();

// True once the client's registry and the InventorySetup type are known. Picks the registry
// that knows both the local player and probeChest (any chest of the base).
bool Ready(uint32_t playerId, uint32_t probeChest);

// Reads an inventory owner. Returns false if the client doesn't know the entity.
bool ReadOwner(uint32_t entity, Owner& out);

// True for the player's hotbar pages, which vanilla "Deposit Stacks" never touches either.
// The game keeps their ids at +0xa0/+0xa4 of one player component; that component type is
// detected on first use. When it can't be found, every page counts as hotbar (safe side).
bool IsHotbarPage(const Owner& player, uint32_t pageEntity);

// Every container (entity with an inventory setup, not a player) whose position lies inside
// the box. Walks the client registry's entity table once. Returns how many were written.
int FindContainers(const float lo[3], const float hi[3], uint32_t* out, int max);

// World position of an entity in meters. Returns false if unknown.
bool Position(uint32_t entity, float out[3]);

// The name a player gave the entity in game (CustomPlayerString, resolved through the
// client's custom string store at client + 0xf0). Returns false if it has none.
bool CustomName(void* client, uint32_t entity, char* out, size_t size);

// Storage chests inside the box: every inventory there whose pages are all storage. For a base
// that lists no crafting-storage chests (a small flame altar). Learns the registry from your
// own pages if needed.
int FindPlainChests(uint32_t playerId, const float lo[3], const float hi[3], uint32_t* out, int max);

// A placed storage chest: one of its components names its build item, "Prop Storage ..." (the
// Quick Stack Station's "Prop Storage Drop Station" aside). The base's own chest list also holds
// entities that are none (a grid of 8-slot inventories without a build item, which lost what
// "Deposit Stacks" put in them), so every chest is checked this way.
bool IsStorageProp(uint32_t entity);

// Keeps only storage chests among candidates: entities with the inventory setup whose component
// types are the base's magic (crafting-storage) chests' less a few, with at most a custom name
// besides. Crafting stations and similar inventories drop out. Returns the new count.
int KeepStorageChests(const uint32_t* magic, int magicCount, uint32_t* candidates, int n);

ItemDesc Describe(uint32_t itemId);

#ifdef QD_BRIDGE
// Dev only: the game's text entries containing needle, from its three text tables (the UI's
// text database: *(*root + 0xd0) + 0x33ce8, tables at +0x40, +0x90, +0xe0; each table + 0x20 ->
// {int32 rel, uint32 count} of 24-byte entries sorted by key). text: the entry's first string
// field that reads as text, with the offset it was found at.
struct LocaHit {
    int table;
    uint32_t key;
    int field;
    std::string text;
};
std::vector<LocaHit> LocaFind(const char* needle, int max);
#endif

// The game's own type names for equipment, in its language ("Head Armor", "Foot Armor"), as the
// item tooltip shows them: the names of slot (equipSlot), or with slot 0 the broad one
// ("Equippable"). Empty until the UI has been built once.
std::vector<std::string> TypeNames(uint8_t slot);
// Every slot that has type names, so a chest named for another slot can be told apart.
std::vector<uint8_t> TypeSlots();

// The game state object *(G + 0xd0) as last seen while the UI was built (dev inspection), or null.
void* UiState();

// The item database record itself (dev inspection), or null.
const uint8_t* RawItemInfo(uint32_t itemId);

// Looks up the shown names Describe() could not yet (they need the UI being built): call it
// from code that runs while the game builds its UI.
void ResolveNames();

// You left the world: forget its registries, so the next world's are learned and identified
// afresh (game thread).
void Forget();

// --- inspection, for the dev bridge ---

// The client registry, also outside a base: the one identified by Ready() or, before that,
// the registry that knows playerId and holds the most entities.
void* InspectRegistry(uint32_t playerId);

// Every ECS registry seen so far (the client has several); returns how many were written.
int Registries(void** out, int max);

// Component type indices of an entity (-1: unknown entity).
int Types(void* registry, uint32_t entity, uint16_t* out, int max);

// Raw component data, or null.
void* Data(void* registry, uint16_t type, uint32_t entity);

// Non-player entities with a position within radius meters of center, nearest first.
int EntitiesNear(void* registry, const float center[3], float radius, uint32_t* out, float* dist, int max);

const char* CategoryName(uint8_t category);

// "Weapon_T1_1H_WandEpic_01_Fire" -> "Wand Epic Fire" (ItemDesc.name).
void PrettyName(const char* raw, char* out, size_t size);

// Under the Greeneye Loader: keep what the hooks learned (ECS registries, item databases) in
// memory the loader keeps over a hot reload, so a reloaded Quick Deposit knows them at once.
void UseKept(const GreeneyeHost* host);

} // namespace inv

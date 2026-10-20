#pragma once
#include <cstdint>

// Read-only view of the client's inventories: your backpack and the base's chests.
//
// An inventory owner (player, chest) has an InventorySetup component that links up to 16
// "page" entities, each with an Inventory component of 8 ItemStacks. Items are described by
// ItemInfo records from the client's item database.
namespace inv {

constexpr int kSlotsPerPage = 8;
constexpr int kMaxPages = 16;

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
    uint16_t maxStack;
    char name[64];  // readable: "Wand Epic Fire"
    char raw[96];   // internal: "Weapon_T1_1H_WandEpic_01_Fire"
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

// Keeps only storage chests among candidates: entities with every component type that all
// the base's magic (crafting-storage) chests share, except the magic-only ones. Crafting
// stations and similar inventories drop out. Returns the new count.
int KeepStorageChests(const uint32_t* magic, int magicCount, uint32_t* candidates, int n);

ItemDesc Describe(uint32_t itemId);

const char* CategoryName(uint8_t category);

} // namespace inv

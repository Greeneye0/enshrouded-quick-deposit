#pragma once
#include "../../common/input_binds.h"
#include <cstdint>
#include "chat.h"

enum class Tier2Mode { Off, DryRun, On };

// Tier 2's ways to pick a chest, tried in the order of tier2Order.
enum class Tier2Rule : uint8_t { SameItem, SameKind, ChestName, Weapons, Nearest };
constexpr int kTier2Rules = 5;

struct Config
{
    bool enabled = true;
    greeneye::input::Binding hotkey;     // optional extra key or button to deposit; unbound = none
    greeneye::input::Binding keepBinding; // keep mark: see keepKey
    int doubleSortMs = 500;  // G twice within this time in the inventory deposits; 0 = off
    int delayMs = 100;   // pause between chest commands: the host takes about one per network tick
    int settleMs = 1500; // wait for the host's changes to reach this client before reading again
    // Tier 2: leftovers go to a chest holding the same kind of item (weapons: same weapon type).
    Tier2Mode tier2 = Tier2Mode::On;
    uint32_t tier2Categories = 0; // bit per keen::ItemCategory, set by LoadConfig
    Tier2Rule tier2Order[kTier2Rules] = {Tier2Rule::SameItem, Tier2Rule::SameKind, Tier2Rule::ChestName,
                                         Tier2Rule::Weapons, Tier2Rule::Nearest};
    int tier2RuleCount = kTier2Rules; // a stack no listed rule finds a chest for stays
    bool newItemsStay = false;        // newItems=stay: stacks of items no chest holds stay with you
    char excludeChests[256] = ""; // comma-separated chest names the mod never touches
    int glowMs = 10000;           // chests a deposit filled glow this long after the inventory closes (0: off)
    int keepKey = 'K';            // over an item in the inventory: keep its type in the backpack
    char keepItems[2048] = "";    // comma-separated kept item names (written by keepKey)
    char language[32] = "auto";   // hints' and chat's language: auto (the game's) or a Steam language code
    chat::Mode chat = chat::Mode::Full; // deposit reports in chat: full, short (one line) or off
    bool noFun = false;           // noFun=true: no jokes or surprises
};

// Reads quick-deposit.ini from the data folder (greeneye\quick-deposit\ under the Greeneye
// Loader, the game folder standalone); missing keys keep their defaults.
Config LoadConfig();

// tier2Order's rule names (item, kind, name, weapons, nearest) into c; none known: no change.
void ParseTier2Order(const char* list, Config& c);

// Reads hotkey and keepKey again when the ini changed (checked at most every 2 s); true if it did.
bool RefreshKeys(Config& c);

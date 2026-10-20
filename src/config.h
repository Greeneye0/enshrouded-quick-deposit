#pragma once
#include <cstdint>

enum class Tier2Mode { Off, DryRun, On };

struct Config
{
    bool enabled = true;
    int hotkey = 0;          // optional extra hotkey (virtual-key code); 0 = none
    int doubleSortMs = 500;  // G twice within this time in the inventory deposits; 0 = off
    int delayMs = 100;   // pause between chest commands so each lands in its own network tick
    int settleMs = 1500; // wait for the host's changes to reach this client before reading again
    // Tier 2: leftovers go to a chest holding the same kind of item (weapons: same weapon type).
    Tier2Mode tier2 = Tier2Mode::On;
    uint32_t tier2Categories = 0; // bit per keen::ItemCategory, set by LoadConfig
    char excludeChests[256] = ""; // comma-separated chest names the mod never touches
};

// Reads quick-deposit.ini from the game folder; missing keys keep their defaults.
Config LoadConfig();

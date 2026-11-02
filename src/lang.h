#pragma once

// Quick Deposit's own text in every language Enshrouded supports. The language follows the game
// (Steam's game language), or language= in quick-deposit.ini (auto, english, german, ...).
namespace lang {

enum class Str : int {
    DepositHint, // after the G key glyph: "Twice: Quick Deposit"
    KeepHint,    // after the keepKey glyph: "Keep in Backpack"
    // Chat lines, after "Quick Deposit: ". The %u and %s stay in the English order.
    ChestsUnreadable, // nothing deposited: the chests can't be read and items are marked
    NothingToDeposit,
    KeptMarked,       // followed by " item xN, item xN"
    CouldNotMove,     // %s item, %s chest
    NoLongerKeeping,  // %s item
    KeepingCount,     // %u count, %s item
    KeepingEvery,     // %s item
    FreedStack,       // %u count, %s item: that stack may go, the type stays kept
    DepositSummary,      // chat=short: %u items, %u chests
    DepositSummaryItems, // chat=short, chests unknown: %u items
    // A chest without a name, in chat: by its most common item, or its number.
    ChestOf,    // %s item
    EmptyChest, // %d number
    WaitingForCharacter, // nothing deposited: which character you play is not known yet
    NoRoom,      // stayed because its chests are full; followed by " item xN, item xN"
    NoRoomCount, // chat=short: %u items stayed because their chests are full
    Count
};

// The ini's language= value ("auto" or a Steam language code). Call before T.
void SetOverride(const char* code);

// The text in the current language. The language is decided on the first call that finds it
// (the override, else Steam once it is up; English until then), so call it from the game's
// frames, not at load. The pointer stays valid.
const char* T(Str s);

// True once the language is decided: T's text no longer changes.
bool Decided();

} // namespace lang

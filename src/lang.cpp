// Languages in Enshrouded's order: English, German, French, Italian, Spanish, Portuguese (Brazil),
// Polish, Russian, Ukrainian, Turkish, Japanese, Korean, Simplified Chinese, Traditional Chinese,
// Thai.
#include <windows.h>
#include <cstring>
#include "lang.h"
#include "log.h"

namespace lang {
namespace {

constexpr int kLangs = 15;
using Row = const char* [kLangs];

const Row kTable[static_cast<int>(Str::Count)] = {
    // DepositHint
    {"Twice: Quick Deposit", "Zweimal: Schnell einlagern", "Deux fois : Dépôt rapide", "Due volte: Deposito rapido",
     "Dos veces: Depósito rápido", "Duas vezes: Depósito rápido", "Dwukrotnie: Szybkie odłożenie",
     "Дважды: Быстро сложить", "Двічі: Швидко скласти", "İki kez: Hızlı Bırak", "2回: クイック収納", "두 번: 빠른 보관",
     "两次：快速存放", "兩次：快速存放", "กดสองครั้ง: ฝากด่วน"},
    // KeepHint
    {"Keep in Backpack", "Im Rucksack behalten", "Garder dans le sac", "Tieni nello zaino", "Mantener en la mochila",
     "Manter na mochila", "Zostaw w plecaku", "Оставить в рюкзаке", "Залишити в рюкзаку", "Çantada Tut",
     "バックパックに残す", "배낭에 유지", "保留在背包中", "保留在背包中", "เก็บไว้ในกระเป๋า"},
    // ChestsUnreadable
    {"can't read the chests yet, so nothing was deposited (you keep marked items). Try again in a moment.",
     "Die Truhen sind noch nicht lesbar, daher wurde nichts eingelagert (du behältst markierte Gegenstände). "
     "Versuch es gleich noch einmal.",
     "impossible de lire les coffres pour l'instant, rien n'a été déposé (vous gardez des objets marqués). "
     "Réessayez dans un instant.",
     "non riesco ancora a leggere i bauli, quindi non è stato depositato nulla (tieni oggetti contrassegnati). "
     "Riprova tra un attimo.",
     "aún no se pueden leer los cofres, así que no se ha depositado nada (guardas objetos marcados). "
     "Inténtalo de nuevo en un momento.",
     "ainda não é possível ler os baús, então nada foi depositado (você mantém itens marcados). "
     "Tente novamente em instantes.",
     "nie można jeszcze odczytać skrzyń, więc nic nie zostało odłożone (zatrzymujesz oznaczone przedmioty). "
     "Spróbuj ponownie za chwilę.",
     "сундуки пока не читаются, поэтому ничего не сложено (у вас есть отмеченные предметы). "
     "Попробуйте ещё раз через мгновение.",
     "скрині поки що не читаються, тому нічого не складено (у вас є позначені предмети). Спробуйте ще раз за мить.",
     "sandıklar henüz okunamıyor, bu yüzden hiçbir şey bırakılmadı (işaretli eşyaları tutuyorsun). Birazdan tekrar dene.",
     "まだチェストを読み取れないため、何も収納しませんでした（印を付けたアイテムがあります）。少し待ってから再度お試しください。",
     "아직 상자를 읽을 수 없어 아무것도 보관하지 않았습니다(표시한 아이템이 있습니다). 잠시 후 다시 시도하세요.",
     "暂时无法读取箱子，因此未存放任何物品（你有标记保留的物品）。请稍后再试。",
     "暫時無法讀取箱子，因此未存放任何物品（你有標記保留的物品）。請稍後再試。",
     "ยังอ่านหีบไม่ได้ จึงไม่ได้ฝากอะไรเลย (คุณมีไอเท็มที่ทำเครื่องหมายเก็บไว้) ลองใหม่อีกครั้งในอีกสักครู่"},
    // NothingToDeposit
    {"nothing to deposit", "nichts einzulagern", "rien à déposer", "niente da depositare", "nada que depositar",
     "nada para depositar", "nic do odłożenia", "нечего сложить", "нічого скласти", "bırakılacak bir şey yok",
     "収納するものがありません", "보관할 아이템이 없습니다", "没有可存放的物品", "沒有可存放的物品", "ไม่มีอะไรให้ฝาก"},
    // KeptMarked
    {"Kept (marked):", "Behalten (markiert):", "Gardé (marqué) :", "Tenuti (contrassegnati):", "Guardado (marcado):",
     "Mantido (marcado):", "Zatrzymane (oznaczone):", "Оставлено (отмечено):", "Залишено (позначено):",
     "Tutulan (işaretli):", "残したもの（印付き）:", "유지됨(표시):", "已保留（已标记）：", "已保留（已標記）：",
     "เก็บไว้ (ทำเครื่องหมาย):"},
    // CouldNotMove
    {"%s could not be moved to %s", "%s konnte nicht verschoben werden nach: %s", "impossible de déplacer %s vers : %s",
     "impossibile spostare %s in: %s", "no se pudo mover %s a: %s", "não foi possível mover %s para: %s",
     "nie udało się przenieść: %s → %s", "не удалось переместить: %s → %s", "не вдалося перемістити: %s → %s",
     "taşınamadı: %s → %s", "%sを%sへ移動できませんでした", "%s을(를) %s(으)로 옮기지 못했습니다", "无法将%s移到%s",
     "無法將%s移到%s", "ย้าย %s ไปที่ %s ไม่ได้"},
    // NoLongerKeeping
    {"no longer keeping %s", "behalte nicht mehr: %s", "ne garde plus : %s", "non tengo più: %s",
     "ya no se guarda: %s", "não mantém mais: %s", "już nie zatrzymuję: %s", "больше не оставляю: %s",
     "більше не залишаю: %s", "artık tutulmuyor: %s", "%sの保持を解除しました", "%s 유지 해제", "不再保留%s", "不再保留%s",
     "เลิกเก็บ %s แล้ว"},
    // KeepingCount
    {"keeping %u %s", "behalte %u × %s", "garde %u × %s", "tengo %u × %s", "guardando %u × %s", "mantendo %u × %s",
     "zatrzymuję %u × %s", "оставляю %u × %s", "залишаю %u × %s", "%u adet tutuluyor: %s", "%u個の%sを残します",
     "%u개의 %s 유지", "保留 %u 个%s", "保留 %u 個%s", "เก็บ %u ชิ้น: %s"},
    // KeepingEvery
    {"keeping every %s", "behalte alle: %s", "garde tous : %s", "tengo tutti: %s", "guardando todos: %s",
     "mantendo todos: %s", "zatrzymuję wszystkie: %s", "оставляю все: %s", "залишаю всі: %s", "tümü tutuluyor: %s",
     "%sをすべて残します", "%s 전부 유지", "保留全部%s", "保留全部%s", "เก็บ %s ทั้งหมด"},
    // FreedStack
    {"%u %s can be deposited, new ones are still kept", "%u × %s können eingelagert werden, neue bleiben behalten",
     "%u × %s peuvent être déposés, les nouveaux restent gardés", "%u × %s possono essere depositati, i nuovi restano tenuti",
     "%u × %s se pueden depositar, los nuevos se siguen guardando", "%u × %s podem ser depositados, os novos continuam mantidos",
     "%u × %s można odłożyć, nowe nadal są zatrzymywane", "%u × %s можно сложить, новые по-прежнему остаются",
     "%u × %s можна скласти, нові й надалі залишаються", "%u adet %s bırakılabilir, yenileri tutulmaya devam eder",
     "%u個の%sは収納できます。新しく入手した分は引き続き残します", "%u개의 %s은(는) 보관할 수 있습니다. 새로 얻는 것은 계속 유지합니다",
     "%u 个%s可以存放，新获得的仍会保留", "%u 個%s可以存放，新獲得的仍會保留", "ฝากได้ %u ชิ้น: %s ของที่ได้ใหม่ยังคงเก็บไว้"},
    // DepositSummary
    {"stored %u items in %u chests", "%u Gegenstände in %u Truhen eingelagert", "%u objets déposés dans %u coffres",
     "%u oggetti depositati in %u bauli", "%u objetos depositados en %u cofres", "%u itens depositados em %u baús",
     "odłożono przedmiotów: %u, do skrzyń: %u", "сложено предметов: %u, в сундуки: %u",
     "складено предметів: %u, у скрині: %u", "%u eşya %u sandığa bırakıldı", "%u個のアイテムを%u個のチェストに収納しました",
     "아이템 %u개를 상자 %u개에 보관했습니다", "已将 %u 件物品存入 %u 个箱子", "已將 %u 件物品存入 %u 個箱子",
     "ฝากไอเท็ม %u ชิ้นลงในหีบ %u ใบ"},
    // DepositSummaryItems
    {"stored %u items", "%u Gegenstände eingelagert", "%u objets déposés", "%u oggetti depositati",
     "%u objetos depositados", "%u itens depositados", "odłożono przedmiotów: %u", "сложено предметов: %u",
     "складено предметів: %u", "%u eşya bırakıldı", "%u個のアイテムを収納しました", "아이템 %u개를 보관했습니다",
     "已存放 %u 件物品", "已存放 %u 件物品", "ฝากไอเท็ม %u ชิ้น"},
    // ChestOf
    {"%s chest", "%s-Truhe", "coffre de %s", "baule di %s", "cofre de %s", "baú de %s", "skrzynia: %s",
     "сундук: %s", "скриня: %s", "%s sandığı", "%sのチェスト", "%s 상자", "%s的箱子", "%s的箱子", "หีบ %s"},
    // EmptyChest
    {"empty chest %d", "leere Truhe %d", "coffre vide %d", "baule vuoto %d", "cofre vacío %d", "baú vazio %d",
     "pusta skrzynia %d", "пустой сундук %d", "порожня скриня %d", "boş sandık %d", "空のチェスト%d", "빈 상자 %d",
     "空箱子 %d", "空箱子 %d", "หีบว่าง %d"},
    // WaitingForCharacter
    {"not yet sure which character you play - pick up or move any item, then try again in half a minute",
     "noch unklar, welchen Charakter du spielst - nimm einen Gegenstand auf oder verschiebe einen und versuch es in einer halben Minute erneut",
     "personnage joué pas encore reconnu - ramassez ou déplacez un objet, puis réessayez dans une demi-minute",
     "non è ancora chiaro quale personaggio usi - raccogli o sposta un oggetto e riprova tra mezzo minuto",
     "aún no se sabe qué personaje usas - recoge o mueve un objeto y vuelve a intentarlo en medio minuto",
     "ainda não se sabe qual personagem você joga - pegue ou mova um item e tente de novo em meio minuto",
     "jeszcze nie wiadomo, którą postacią grasz - podnieś lub przesuń dowolny przedmiot i spróbuj ponownie za pół minuty",
     "пока неясно, каким персонажем вы играете - подберите или переложите любой предмет и повторите через полминуты",
     "поки невідомо, яким персонажем ви граєте - підберіть або перекладіть будь-який предмет і повторіть за пів хвилини",
     "hangi karakterle oynadığın henüz belli değil - bir eşya al ya da taşı, yarım dakika sonra tekrar dene",
     "どのキャラクターでプレイ中かまだ分かりません。アイテムを拾うか動かして、30秒後にもう一度お試しください",
     "어떤 캐릭터로 플레이 중인지 아직 알 수 없습니다. 아이템을 줍거나 옮긴 뒤 30초 후에 다시 시도하세요",
     "尚未确定你正在使用哪个角色 - 拾取或移动任意物品，半分钟后再试",
     "尚未確定你正在使用哪個角色 - 拾取或移動任意物品，半分鐘後再試",
     "ยังไม่ทราบว่าคุณเล่นตัวละครใด - เก็บหรือย้ายไอเท็มใดก็ได้ แล้วลองอีกครั้งในอีกครึ่งนาที"},
    // NoRoom
    {"No room in chests for:", "Kein Platz in den Truhen für:", "Plus de place dans les coffres pour :",
     "Nessuno spazio nei bauli per:", "No hay espacio en los cofres para:", "Sem espaço nos baús para:",
     "Brak miejsca w skrzyniach na:", "Нет места в сундуках для:", "Немає місця в скринях для:",
     "Sandıklarda yer yok:", "チェストに空きがありません:", "상자에 공간이 없습니다:", "箱子已满，无法存放：",
     "箱子已滿，無法存放：", "หีบเต็ม ไม่มีที่สำหรับ:"},
    // NoRoomCount
    {"no room in chests for %u items", "kein Platz in den Truhen für %u Gegenstände",
     "plus de place dans les coffres pour %u objets", "nessuno spazio nei bauli per %u oggetti",
     "no hay espacio en los cofres para %u objetos", "sem espaço nos baús para %u itens",
     "brak miejsca w skrzyniach na przedmioty: %u", "нет места в сундуках для предметов: %u",
     "немає місця в скринях для предметів: %u", "sandıklarda %u eşya için yer yok",
     "チェストに空きがなく、%u個のアイテムを収納できません", "상자에 공간이 없어 아이템 %u개를 보관하지 못했습니다",
     "箱子已满，%u 件物品无法存放", "箱子已滿，%u 件物品無法存放", "หีบเต็ม ฝากไอเท็มไม่ได้ %u ชิ้น"},
};

// Steam's language codes, by column.
const char* const kSteamCodes[kLangs] = {
    "english", "german", "french", "italian", "spanish", "brazilian", "polish", "russian", "ukrainian", "turkish",
    "japanese", "koreana", "schinese", "tchinese", "thai",
};

char g_override[32] = "";
int g_lang = 0;
bool g_decided = false;

int FromCode(const char* code)
{
    if (!code || !*code)
        return -1;
    for (int i = 0; i < kLangs; ++i)
        if (_stricmp(code, kSteamCodes[i]) == 0)
            return i;
    // Steam languages the game has no text for, mapped to the nearest one it does.
    if (_stricmp(code, "latam") == 0)
        return 4;
    if (_stricmp(code, "portuguese") == 0)
        return 5;
    return -1;
}

using SteamAppsFn = void* (*)();
using GameLanguageFn = const char* (*)(void* apps);

// The game's Steam language; null while Steam is not up yet.
const char* SteamGameLanguage()
{
    HMODULE steam = GetModuleHandleW(L"steam_api64.dll");
    if (!steam)
        return nullptr;
    auto apps = reinterpret_cast<SteamAppsFn>(GetProcAddress(steam, "SteamAPI_SteamApps_v009"));
    if (!apps)
        apps = reinterpret_cast<SteamAppsFn>(GetProcAddress(steam, "SteamAPI_SteamApps_v008"));
    auto language = reinterpret_cast<GameLanguageFn>(GetProcAddress(steam, "SteamAPI_ISteamApps_GetCurrentGameLanguage"));
    if (!apps || !language)
        return nullptr;
    void* a = apps();
    return a ? language(a) : nullptr;
}

void Decide()
{
    if (g_decided)
        return;
    int forced = FromCode(g_override);
    if (forced >= 0) {
        g_lang = forced;
        g_decided = true;
        Log("language: %s (quick-deposit.ini)", kSteamCodes[g_lang]);
        return;
    }
    const char* steam = SteamGameLanguage();
    if (!steam)
        return; // ask again next time
    int fromSteam = FromCode(steam);
    g_lang = fromSteam >= 0 ? fromSteam : 0;
    g_decided = true;
    Log("language: %s (game: %s)", kSteamCodes[g_lang], steam);
}

} // namespace

void SetOverride(const char* code)
{
    strncpy_s(g_override, code ? code : "", _TRUNCATE);
    if (*g_override && _stricmp(g_override, "auto") != 0 && FromCode(g_override) < 0)
        Log("language: '%s' in quick-deposit.ini is not a language; following the game", g_override);
}

const char* T(Str s)
{
    Decide();
    const Row& row = kTable[static_cast<int>(s)];
    const char* text = row[g_lang];
    return text && *text ? text : row[0];
}

bool Decided()
{
    return g_decided;
}

} // namespace lang

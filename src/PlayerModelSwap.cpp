// DSCS Player Model Swap - play Digimon Story Cyber Sleuth as a Digimon.
//
// Hooks the game's control-player model resolver (Player_GetControlModelName) and returns
// a Digimon model code (chrNNN) while the protagonist is the controlled character. The
// resolver's callers only read the string, and nothing is written to the player struct,
// so saves are never modified: remove the plugin and the protagonist is back.
//
// The engine already supports a Digimon as the controlled field character (control type 5
// uses chr320): the character controller is sized from the Digimon id parsed out of the
// model name, and every Digimon skeleton carries the TP01/CP01/GP01/FP01 locators.
#include "PlayerModelSwap.h"

#include <dscs/GameInterface.h>
#include <dscs/Symbols.h>
#include <modloader/plugin.h>
#include <modloader/sigscan.h>
#include <modloader/utils.h>
#include <squirrel/squirrel.h>

#include <MinHook.h>
#include <Windows.h>

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <format>
#include <mutex>
#include <set>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace
{
    // Player_GetControlModelName(Player* player) -> const char*
    //   sub rsp,28h; mov eax,[rcx+34h]; lea r8,"pc001"; dec eax; cmp eax,8; ja default
    constexpr const char* SIG_GET_CONTROL_MODEL_NAME =
        "48 83 EC 28 8B 41 34 4C 8D 05 ?? ?? ?? ?? FF C8 83 F8 08 0F 87";

    // Player struct: controlPlayerType at +0x34. 0 = default (pc001), 1/2 = protagonist
    // (cloth/default model), 3..9 = characters the story puts you in control of.
    constexpr int OFF_CONTROL_PLAYER_TYPE = 0x34;

    // Model_BuildMotionName(Model* model, std::string* out, const char* motion) -> out
    //   formats "%s_%s" from the model name (+0xD8) and the short motion name; signature hits
    //   0x1E bytes into the function (after the generic /GS prologue).
    constexpr const char* SIG_BUILD_MOTION_NAME =
        "4D 8B C8 48 8B DA 4C 8B D9 48 89 54 24 30 45 33 C0 44 89 44 24 20 48 C7 42 18 0F 00 00 00 "
        "4C 89 42 10 48 83 7A 18 10 72 05 48 8B 02 EB 03 48 8B C3 44 88 00 C7 44 24 20 01 00 00 00 "
        "48 8D 81 D8 00 00 00";
    constexpr int SIG_BUILD_MOTION_NAME_OFFSET = -0x1E;
    constexpr int OFF_MODEL_NAME               = 0xD8;

    using BuildMotionNameFn = void* (*)(void* model, void* out, const char* motion);
    BuildMotionNameFn g_originalBuildMotionName = nullptr;

    // FieldChar_IsPlayingMotion(FieldChar* self, const char* motion) -> bool
    //   compares "<model>_<motion>" with the playing anim; gates locomotion restarts, footsteps
    //   and interactions (idle check), so it must see the same remap as Model_BuildMotionName.
    constexpr const char* SIG_IS_PLAYING_MOTION =
        "40 53 48 83 EC 50 48 8B 05 ?? ?? ?? ?? 48 33 C4 48 89 44 24 40 4C 8B 41 08 4D 85 C0 75 04 33 DB EB 07 "
        "49 8D 98 F8 00 00 00 49 83 C0 18";
    using IsPlayingMotionFn                   = bool (*)(void* self, const char* motion);
    IsPlayingMotionFn g_originalIsPlayingMotion = nullptr;

    // CGameModel::Load (secondary vtable slot 7): (ModelBase* self, const char* name, bool flag) -> bool.
    //   Every model load (field, cutscene actors, menus) passes through here; the name becomes the
    //   model's name (+0xD8), which motion lookups are built from.
    constexpr const char* SIG_MODEL_LOAD =
        "40 53 48 83 EC 40 48 8B D9 E8 ?? ?? ?? ?? 84 C0 75 06 48 83 C4 40 5B C3 48 8B 8B 20 01 00 00 48 85 C9 "
        "74 07 B2 01 E8";
    using ModelLoadFn             = bool (*)(void* self, const char* name, uint8_t flag);
    ModelLoadFn g_originalModelLoad = nullptr;

    // Player_GetDefaultModelName(Player*) -> "pc001"/"pc002" (or "pc020" in Hacker's Memory).
    constexpr const char* SIG_GET_DEFAULT_MODEL_NAME =
        "40 53 48 83 EC 20 48 8B D9 E8 ?? ?? ?? ?? 48 8B 90 90 00 00 00 83 7A 08 00 74 0D 48 8D 05 ?? ?? ?? ?? "
        "48 83 C4 20 5B C3 8B 4B 08";
    using GetDefaultModelNameFn                   = const char* (*)(void* player);
    GetDefaultModelNameFn g_getDefaultModelName   = nullptr;
    constexpr int OFF_PLAYER_CLOTH_MODEL          = 0x1D0; // char[8], costume model (pc1xx..pc3xx)
    std::atomic<bool> g_replaceInCutscenes{ true };

    // Archive_FileExists(const char* name, const char* ext, const char* archive) -> bool
    //   archive == nullptr checks every registered archive, mod folders included.
    constexpr const char* SIG_FILE_EXISTS =
        "49 8B D8 4C 8B FA 4C 8B F1 80 3D ?? ?? ?? ?? 00 75 05 E8 ?? ?? ?? ?? 45 33 E4 44 89 65 C7";
    constexpr int SIG_FILE_EXISTS_OFFSET = -0x2D;
    using FileExistsFn                   = bool (*)(const char* name, const char* ext, const char* archive);
    FileExistsFn g_fileExists            = nullptr;

    // Field motions the CFieldPlayer requests -> battle motions most Digimon have
    // (fn = idle, fw = walk, fr = run, ff01/ff02 = alternate idle/move mode). Any other
    // missing motion (cutscene pc motions) falls back to the battle idle.
    struct MotionFallback
    {
        const char* field;
        const char* battle;
    };
    constexpr MotionFallback MOTION_FALLBACKS[] = {
        { "fn01", "bn01" }, { "fw01", "br01" }, { "fr01", "br01" }, { "ff01", "bn01" }, { "ff02", "br01" },
    };
    constexpr const char* IDLE_FALLBACK = "bn01";
    std::atomic<bool> g_remapMotions{ true };

    // "<model>_<motion>" -> exists; filled lazily (key includes the model, so no reset needed).
    std::mutex g_existsMutex;
    std::unordered_map<std::string, bool> g_existsCache;

    using GetControlModelNameFn = const char* (*)(void* player, void* unused);
    GetControlModelNameFn g_original = nullptr;

    // SqNative_SetControlPlayerType(int type): sets the type, then rebuilds the player through
    //   sceneManager->scene->vtbl[54]; we reuse that path to apply a new model immediately.
    constexpr const char* SIG_SET_CONTROL_PLAYER_TYPE =
        "40 53 48 83 EC 30 48 C7 44 24 20 FE FF FF FF 8B D9 E8 ?? ?? ?? ?? 48 8B 90 90 00 00 00 83 7A 08 00 "
        "74 06 48 8B 50 78 EB 04 48 8B 50 30 89 5A 34 48 8B 05";
    constexpr int SET_CONTROL_SCENE_MANAGER_DISP = 0x33; // mov rax, [rip+disp32] (scene manager singleton)
    constexpr int OFF_MANAGER_SCENE              = 0x1A0;
    constexpr int OFF_SCENE_VTBL_REBUILD_PLAYER  = 0x1B0;
    // Field scene's rebuild slot: mov rcx,[rcx+1F8h] (field player); jmp FieldPlayer_RebuildModel.
    constexpr uint8_t FIELD_REBUILD_PLAYER_PREFIX[] = { 0x48, 0x8B, 0x89, 0xF8, 0x01, 0x00, 0x00, 0xE9 };
    constexpr int OFF_FIELD_SCENE_PLAYER             = 0x1F8;
    void** g_sceneManagerSlot                        = nullptr;
    std::atomic<bool> g_applyPending{ false };

    // SqNative_GetCharName(uint id) -> const char* (current language). GetItemName & co. share the
    //   body byte for byte; only the table-name string loaded at +0x4A differs.
    constexpr const char* SIG_GET_TEXT_NATIVE =
        "48 89 5C 24 08 57 48 83 EC 30 8B D9 E8 ?? ?? ?? ?? 48 8B F8 48 8B 05 ?? ?? ?? ?? 48 85 C0 75 23 "
        "B9 50 01 00 00 E8 ?? ?? ?? ?? 48 89 44 24 48 48 85 C0 74 08 48 8B C8 E8 ?? ?? ?? ?? 48 89 05 ?? ?? ?? ?? "
        "44 8B 88 B4 00 00 00 48 8D 15";
    constexpr int GET_TEXT_TABLE_DISP = 0x4D;
    using GetCharNameFn               = const char* (*)(uint32_t id);
    GetCharNameFn g_getCharName       = nullptr;
    constexpr uint32_t CHARNAME_DIGIMON_BASE = 1000; // charname id = 1000 + Digimon id

    std::string g_iniPath;
    int g_menuKey = VK_F7;

    std::atomic<bool> g_enabled{ true };
    std::atomic<bool> g_temporaryHuman{ false };
    char g_model[16] = "";     // e.g. "chr320"; empty = no swap
    int g_toggleKey   = VK_F8; // temporary-human toggle, 0 = off

    void log(const std::string& message)
    {
        // DSCSModLoader's logger is not exported; keep our own small log next to the game.
        static FILE* file = nullptr;
        if (!file) fopen_s(&file, "DSCSPlayerModelSwap.log", "w");
        if (file)
        {
            std::fprintf(file, "%s\n", message.c_str());
            std::fflush(file);
        }
    }

    void logLine(const std::string& message) { log(message); }

    bool isValidModel(const char* model)
    {
        // chr### only: the controller setup parses atoi(name + 3) as the Digimon id.
        return std::strlen(model) == 6 && std::strncmp(model, "chr", 3) == 0 && std::isdigit(model[3])
               && std::isdigit(model[4]) && std::isdigit(model[5]);
    }

    bool isProtagonist(void* player)
    {
        if (player == nullptr) return false;
        const int type = *reinterpret_cast<int*>(static_cast<char*>(player) + OFF_CONTROL_PLAYER_TYPE);
        return type >= 0 && type <= 2;
    }

    const char* hookGetControlModelName(void* player, void* unused)
    {
        if (g_enabled && !g_temporaryHuman && g_model[0] != '\0' && isProtagonist(player)) return g_model;
        return g_original(player, unused);
    }

    bool motionExists(const char* model, const char* motion)
    {
        const std::string file = std::format("{}_{}", model, motion);
        std::lock_guard lock(g_existsMutex);
        auto it = g_existsCache.find(file);
        if (it != g_existsCache.end()) return it->second;
        const bool exists = g_fileExists(file.c_str(), "anim", nullptr);
        g_existsCache.emplace(file, exists);
        if (!exists) log(std::format("motion {} missing", file));
        return exists;
    }

    // Short motion name to use for the swapped model, or the original if it exists / no fallback works.
    const char* remapMotion(const char* model, const char* motion)
    {
        if (motion[0] == '\0' || std::strcmp(motion, model) == 0) return motion; // bind pose / full name
        if (motionExists(model, motion)) return motion;

        const char* fallback = IDLE_FALLBACK;
        for (const auto& entry : MOTION_FALLBACKS)
            if (std::strcmp(motion, entry.field) == 0) fallback = entry.battle;

        if (motionExists(model, fallback)) return fallback;
        if (fallback != IDLE_FALLBACK && motionExists(model, IDLE_FALLBACK)) return IDLE_FALLBACK;
        return motion;
    }

    const char* remapForModel(const void* model, const char* motion)
    {
        if (g_fileExists == nullptr || !g_remapMotions || !g_enabled || g_temporaryHuman || g_model[0] == '\0'
            || model == nullptr || motion == nullptr)
            return motion;
        const char* name = static_cast<const char*>(model) + OFF_MODEL_NAME;
        return std::strcmp(name, g_model) == 0 ? remapMotion(name, motion) : motion;
    }

    void* hookBuildMotionName(void* model, void* out, const char* motion)
    {
        return g_originalBuildMotionName(model, out, remapForModel(model, motion));
    }

    bool hookIsPlayingMotion(void* self, const char* motion)
    {
        const void* model = self ? *reinterpret_cast<void**>(static_cast<char*>(self) + 8) : nullptr;
        return g_originalIsPlayingMotion(self, remapForModel(model, motion));
    }

    // Same player selection as the game's Squirrel natives: ctx[18]->mode ? ctx[15] : ctx[6].
    void* currentPlayer()
    {
        auto* ctx = reinterpret_cast<uint64_t*>(dscs::getGameContext());
        if (ctx == nullptr || ctx[18] == 0) return nullptr;
        const int mode = *reinterpret_cast<int*>(ctx[18] + 8);
        return reinterpret_cast<void*>(mode ? ctx[15] : ctx[6]);
    }

    bool isProtagonistModelName(const char* name)
    {
        void* player = currentPlayer();
        if (player == nullptr) return false;
        const char* cloth = static_cast<const char*>(player) + OFF_PLAYER_CLOTH_MODEL;
        if (cloth[0] != '\0' && strncmp(name, cloth, 8) == 0 && std::strlen(name) == strnlen(cloth, 8)) return true;
        const char* def = g_getDefaultModelName(player);
        return def != nullptr && std::strcmp(name, def) == 0;
    }

    bool hookModelLoad(void* self, const char* name, uint8_t flag)
    {
        if (g_replaceInCutscenes && g_enabled && !g_temporaryHuman && g_model[0] != '\0' && name != nullptr
            && name[0] == 'p' && name[1] == 'c' && isProtagonistModelName(name))
        {
            static std::string lastLogged;
            std::lock_guard lock(g_existsMutex);
            if (lastLogged != name)
            {
                lastLogged = name;
                log(std::format("model {} -> {}", name, g_model));
            }
            name = g_model;
        }
        return g_originalModelLoad(self, name, flag);
    }

    // --- Script error logging (diagnostic) ---
    // The game installs no Squirrel error handler, so failing event-script helpers abort silently.
    // Install one on every VM/thread we see and force raiseerror so it runs; results are unchanged.
    using SqCallFn   = SQRESULT (*)(HSQUIRRELVM v, SQInteger params, SQBool retval, SQBool raiseerror);
    using SqWakeupFn = SQRESULT (*)(HSQUIRRELVM v, SQBool resumedret, SQBool retval, SQBool raiseerror, SQBool throwerror);
    SqCallFn g_originalSqCall     = nullptr;
    SqWakeupFn g_originalSqWakeup = nullptr;
    std::atomic<bool> g_logScriptErrors{ true };
    std::mutex g_sqMutex;
    std::unordered_set<HSQUIRRELVM> g_sqHandled;
    std::set<std::string> g_sqLogged;

    SQInteger scriptErrorHandler(HSQUIRRELVM v)
    {
        const SQChar* message = "?";
        if (sq_gettop(v) >= 2)
        {
            sq_tostring(v, 2);
            sq_getstring(v, -1, &message);
        }
        std::string text = std::format("script error: {}", message ? message : "?");
        SQStackInfos si{};
        for (SQInteger level = 1; level < 8 && SQ_SUCCEEDED(sq_stackinfos(v, level, &si)); level++)
            text += std::format("\n    at {} ({}:{})", si.funcname ? si.funcname : "?",
                                si.source ? si.source : "?", static_cast<int64_t>(si.line));
        if (sq_gettop(v) >= 2) sq_pop(v, 1);

        std::lock_guard lock(g_sqMutex);
        if (g_sqLogged.size() < 500 && g_sqLogged.insert(text).second) log(text);
        return 0;
    }

    void ensureErrorHandler(HSQUIRRELVM v)
    {
        if (!g_logScriptErrors || v == nullptr) return;
        {
            std::lock_guard lock(g_sqMutex);
            if (!g_sqHandled.insert(v).second) return;
        }
        sq_newclosure(v, scriptErrorHandler, 0);
        sq_seterrorhandler(v);
    }

    SQRESULT hookSqCall(HSQUIRRELVM v, SQInteger params, SQBool retval, SQBool raiseerror)
    {
        ensureErrorHandler(v);
        return g_originalSqCall(v, params, retval, g_logScriptErrors ? SQTrue : raiseerror);
    }

    SQRESULT hookSqWakeup(HSQUIRRELVM v, SQBool resumedret, SQBool retval, SQBool raiseerror, SQBool throwerror)
    {
        ensureErrorHandler(v);
        return g_originalSqWakeup(v, resumedret, retval, g_logScriptErrors ? SQTrue : raiseerror, throwerror);
    }

    // --- Immediate apply / Digimon names ---
    // All .text matches of a pattern (the loader only exports a unique-match scan).
    std::vector<char*> findAllInText(const char* pattern)
    {
        std::vector<int> bytes; // -1 = wildcard
        for (const char* p = pattern; *p;)
        {
            if (*p == ' ') { p++; continue; }
            if (*p == '?') { bytes.push_back(-1); while (*p == '?') p++; continue; }
            bytes.push_back(static_cast<int>(std::strtoul(std::string(p, 2).c_str(), nullptr, 16)));
            p += 2;
        }
        std::vector<char*> hits;
        auto* base = reinterpret_cast<uint8_t*>(getBaseOffset());
        auto* nt   = reinterpret_cast<IMAGE_NT_HEADERS*>(base + reinterpret_cast<IMAGE_DOS_HEADER*>(base)->e_lfanew);
        auto* sec  = IMAGE_FIRST_SECTION(nt);
        for (int s = 0; s < nt->FileHeader.NumberOfSections; s++, sec++)
        {
            if (std::memcmp(sec->Name, ".text", 6) != 0) continue;
            uint8_t* begin = base + sec->VirtualAddress;
            const size_t size = sec->Misc.VirtualSize;
            for (size_t i = 0; i + bytes.size() <= size; i++)
            {
                size_t k = 0;
                while (k < bytes.size() && (bytes[k] < 0 || begin[i + k] == bytes[k])) k++;
                if (k == bytes.size()) hits.push_back(reinterpret_cast<char*>(begin + i));
            }
        }
        return hits;
    }

    char* ripTarget(char* insnDisp) // rel32 at insnDisp, instruction ends right after it
    {
        return insnDisp + 4 + *reinterpret_cast<int32_t*>(insnDisp);
    }

    void resolveExtras()
    {
        if (char* fn = findSignature(SIG_SET_CONTROL_PLAYER_TYPE))
            g_sceneManagerSlot = reinterpret_cast<void**>(ripTarget(fn + SET_CONTROL_SCENE_MANAGER_DISP));
        else
            log("SetControlPlayerType signature not found; model changes apply on the next map load.");

        for (char* fn : findAllInText(SIG_GET_TEXT_NATIVE))
            if (std::strcmp(ripTarget(fn + GET_TEXT_TABLE_DISP), "CharName") == 0)
                g_getCharName = reinterpret_cast<GetCharNameFn>(fn);
        if (g_getCharName == nullptr) log("GetCharName not found; the menu lists model codes only.");
    }

    // Rebuild the field player now (same call SetControlPlayerType makes). Field scene only.
    void rebuildFieldPlayer()
    {
        if (g_sceneManagerSlot == nullptr || *g_sceneManagerSlot == nullptr) return;
        char* scene = *reinterpret_cast<char**>(static_cast<char*>(*g_sceneManagerSlot) + OFF_MANAGER_SCENE);
        if (scene == nullptr) return;
        auto* vtbl    = *reinterpret_cast<char***>(scene);
        char* rebuild = vtbl[OFF_SCENE_VTBL_REBUILD_PLAYER / sizeof(void*)];
        if (std::memcmp(rebuild, FIELD_REBUILD_PLAYER_PREFIX, sizeof(FIELD_REBUILD_PLAYER_PREFIX)) != 0) return;
        if (*reinterpret_cast<void**>(scene + OFF_FIELD_SCENE_PLAYER) == nullptr) return;
        if (!isProtagonist(currentPlayer())) return;
        reinterpret_cast<void (*)(char*)>(rebuild)(scene);
        log("field player rebuilt");
    }

    void writeIni(const char* key, const char* value)
    {
        if (!g_iniPath.empty()) WritePrivateProfileStringA("PlayerModelSwap", key, value, g_iniPath.c_str());
    }
} // namespace

// resources/PlayerModelSwap.ini (see PlayerModelSwap.ini for every key)
//   [PlayerModelSwap]
//   Model=chr320        ; Digimon model code (chr + 3 digits). Empty = protagonist.
//   Enabled=1
//   TemporaryHumanKey=119   ; decimal virtual-key code (F8), 0 disables the hotkey
//   MenuKey=118             ; in-game picker (F7), 0 disables the menu
static void readIni(const std::string& path)
{
    char buffer[32]{};
    GetPrivateProfileStringA("PlayerModelSwap", "Model", "", buffer, sizeof(buffer), path.c_str());
    for (char* c = buffer; *c; c++)
        *c = static_cast<char>(std::tolower(static_cast<unsigned char>(*c)));
    if (buffer[0] != '\0' && !isValidModel(buffer))
    {
        log(std::format("Model '{}' is not a chr### model code; swap disabled.", buffer));
        buffer[0] = '\0';
    }
    strcpy_s(g_model, buffer);

    g_enabled   = GetPrivateProfileIntA("PlayerModelSwap", "Enabled", 1, path.c_str()) != 0;
    g_toggleKey = GetPrivateProfileIntA("PlayerModelSwap", "TemporaryHumanKey", VK_F8, path.c_str());
    g_remapMotions = GetPrivateProfileIntA("PlayerModelSwap", "RemapFieldMotions", 1, path.c_str()) != 0;
    g_replaceInCutscenes = GetPrivateProfileIntA("PlayerModelSwap", "ReplaceInCutscenes", 1, path.c_str()) != 0;
    g_logScriptErrors = GetPrivateProfileIntA("PlayerModelSwap", "LogScriptErrors", 1, path.c_str()) != 0;
    g_menuKey = GetPrivateProfileIntA("PlayerModelSwap", "MenuKey", VK_F7, path.c_str());
    g_iniPath = path;
}

// --- Interface for the in-game menu (PlayerModelSwap.h) ---
void pms::log(const std::string& message) { logLine(message); }

std::vector<pms::ModelEntry> pms::listModels()
{
    std::vector<ModelEntry> models;
    for (int id = 1; id <= 999; id++)
    {
        char code[8];
        std::snprintf(code, sizeof(code), "chr%03d", id);
        if (g_fileExists != nullptr && !g_fileExists(code, "geom", nullptr)) continue;
        ModelEntry entry{ code, "", false };
        if (g_getCharName != nullptr)
            if (const char* name = g_getCharName(CHARNAME_DIGIMON_BASE + id)) entry.name = name;
        entry.fieldMotions = g_fileExists != nullptr && g_fileExists((entry.code + "_fn01").c_str(), "anim", nullptr);
        models.push_back(std::move(entry));
    }
    return models;
}

void pms::onFrame()
{
    if (g_applyPending.exchange(false)) rebuildFieldPlayer();
}

std::string pms::getModel() { return g_model; }

void pms::setModel(const std::string& code)
{
    if (!code.empty() && !isValidModel(code.c_str())) return;
    strcpy_s(g_model, code.c_str());
    writeIni("Model", g_model);
    g_applyPending = true;
    log(std::format("model set to '{}' from the menu", g_model));
}

bool pms::getEnabled() { return g_enabled; }
void pms::setEnabled(bool enabled)
{
    g_enabled = enabled;
    writeIni("Enabled", enabled ? "1" : "0");
    g_applyPending = true;
}

bool pms::getTemporaryHuman() { return g_temporaryHuman; }
void pms::setTemporaryHuman(bool human)
{
    g_temporaryHuman = human;
    g_applyPending   = true;
}

bool pms::getReplaceInCutscenes() { return g_replaceInCutscenes; }
void pms::setReplaceInCutscenes(bool replace)
{
    g_replaceInCutscenes = replace;
    writeIni("ReplaceInCutscenes", replace ? "1" : "0");
}

// --- Squirrel API: PlayerModelSwap.SetModel("chr320"), GetModel(), SetEnabled(bool) ---
// Raw natives: DSCSModLoader doesn't export the SQUIRREL_AWAY marshalling specializations.
// Stack: 1 = this, 2.. = arguments, top = the loader's userdata free variable.
static SQInteger SqSetModel(HSQUIRRELVM vm)
{
    const SQChar* model = nullptr;
    if (SQ_FAILED(sq_getstring(vm, 2, &model))) return 0;
    if (model[0] == '\0')
        g_model[0] = '\0';
    else if (isValidModel(model))
        strcpy_s(g_model, model);
    return 0;
}
static SQInteger SqGetModel(HSQUIRRELVM vm)
{
    sq_pushstring(vm, g_model, -1);
    return 1;
}
static SQInteger SqSetEnabled(HSQUIRRELVM vm)
{
    SQBool enabled = SQTrue;
    if (SQ_SUCCEEDED(sq_getbool(vm, 2, &enabled))) g_enabled = enabled != SQFalse;
    return 0;
}

static DWORD WINAPI hotkeyThread(LPVOID)
{
    bool wasDown = false;
    for (;;)
    {
        const int key   = g_toggleKey;
        const bool down = key != 0 && (GetAsyncKeyState(key) & 0x8000) != 0;
        // only react while the game window is focused
        DWORD pid = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &pid);
        if (down && !wasDown && pid == GetCurrentProcessId() && g_model[0] != '\0')
        {
            g_temporaryHuman = !g_temporaryHuman;
            g_applyPending   = true;
            log(std::format("temporary human: {}", g_temporaryHuman ? "on" : "off"));
        }
        wasDown = down;
        Sleep(25);
    }
}

class PlayerModelSwapPlugin : public BasePlugin
{
public:
    using BasePlugin::BasePlugin;

    void onEnable() override
    {
        readIni(std::string(dscs::getResourceDir()) + "/PlayerModelSwap.ini");

        char* target = findSignature(SIG_GET_CONTROL_MODEL_NAME);
        if (target == nullptr)
        {
            log("Player_GetControlModelName signature not found - unsupported game version, plugin inactive.");
            return;
        }

        if (MH_Initialize() != MH_OK || MH_CreateHook(target, reinterpret_cast<void*>(&hookGetControlModelName),
                                                      reinterpret_cast<void**>(&g_original)) != MH_OK
            || MH_EnableHook(target) != MH_OK)
        {
            log("failed to install the model resolver hook.");
            return;
        }

        log(std::format("hooked Player_GetControlModelName @ RVA 0x{:X}; model='{}', enabled={}",
                        static_cast<uint64_t>(target - getBaseOffset()),
                        g_model,
                        g_enabled.load()));

        // Motion fallback is optional: without it the swap still works, the Digimon just
        // keeps its previous motion when a requested one is missing.
        if (char* exists = findSignature(SIG_FILE_EXISTS))
            g_fileExists = reinterpret_cast<FileExistsFn>(exists + SIG_FILE_EXISTS_OFFSET);
        else
            log("Archive_FileExists signature not found; motions will not be remapped.");

        if (char* hit = findSignature(SIG_BUILD_MOTION_NAME); hit != nullptr && g_fileExists != nullptr)
        {
            char* motionTarget = hit + SIG_BUILD_MOTION_NAME_OFFSET;
            if (MH_CreateHook(motionTarget, reinterpret_cast<void*>(&hookBuildMotionName),
                              reinterpret_cast<void**>(&g_originalBuildMotionName)) == MH_OK
                && MH_EnableHook(motionTarget) == MH_OK)
                log(std::format("hooked Model_BuildMotionName @ RVA 0x{:X}; field motion remap={}",
                                static_cast<uint64_t>(motionTarget - getBaseOffset()),
                                g_remapMotions.load()));
            else
                log("failed to hook Model_BuildMotionName; field motions will not be remapped.");

            // Without this the remap breaks the engine's "already playing" checks (restarts every
            // frame, no footsteps, no interactions), so disable the remap if it can't be hooked.
            char* isPlaying = findSignature(SIG_IS_PLAYING_MOTION);
            if (isPlaying != nullptr
                && MH_CreateHook(isPlaying, reinterpret_cast<void*>(&hookIsPlayingMotion),
                                 reinterpret_cast<void**>(&g_originalIsPlayingMotion)) == MH_OK
                && MH_EnableHook(isPlaying) == MH_OK)
                log(std::format("hooked FieldChar_IsPlayingMotion @ RVA 0x{:X}",
                                static_cast<uint64_t>(isPlaying - getBaseOffset())));
            else
            {
                log("FieldChar_IsPlayingMotion not hooked; disabling motion remap.");
                g_remapMotions = false;
            }
        }
        else
            log("Model_BuildMotionName signature not found; field motions will not be remapped.");

        // Cutscene/menu actors: swap the protagonist's model wherever it is loaded by name.
        char* modelLoad = findSignature(SIG_MODEL_LOAD);
        char* getDefault = findSignature(SIG_GET_DEFAULT_MODEL_NAME);
        if (modelLoad != nullptr && getDefault != nullptr)
        {
            g_getDefaultModelName = reinterpret_cast<GetDefaultModelNameFn>(getDefault);
            if (MH_CreateHook(modelLoad, reinterpret_cast<void*>(&hookModelLoad),
                              reinterpret_cast<void**>(&g_originalModelLoad)) == MH_OK
                && MH_EnableHook(modelLoad) == MH_OK)
                log(std::format("hooked CGameModel::Load @ RVA 0x{:X}; cutscene replacement={}",
                                static_cast<uint64_t>(modelLoad - getBaseOffset()),
                                g_replaceInCutscenes.load()));
            else
                log("failed to hook CGameModel::Load; cutscenes keep the human model.");
        }
        else
            log("CGameModel::Load / Player_GetDefaultModelName signature not found; cutscenes keep the human model.");

        if (g_logScriptErrors)
        {
            char* sqCall   = getGameSymbol("sq_call");
            char* sqWakeup = getGameSymbol("sq_wakeupvm");
            const bool ok  = sqCall && sqWakeup
                            && MH_CreateHook(sqCall, reinterpret_cast<void*>(&hookSqCall),
                                             reinterpret_cast<void**>(&g_originalSqCall)) == MH_OK
                            && MH_CreateHook(sqWakeup, reinterpret_cast<void*>(&hookSqWakeup),
                                             reinterpret_cast<void**>(&g_originalSqWakeup)) == MH_OK
                            && MH_EnableHook(sqCall) == MH_OK && MH_EnableHook(sqWakeup) == MH_OK;
            log(ok ? "script error logging enabled" : "script error logging unavailable");
        }

        modLoader.addSquirrelFunction("PlayerModelSwap", "SetModel", nullptr, SqSetModel);
        modLoader.addSquirrelFunction("PlayerModelSwap", "GetModel", nullptr, SqGetModel);
        modLoader.addSquirrelFunction("PlayerModelSwap", "SetEnabled", nullptr, SqSetEnabled);

        resolveExtras();
        overlay::install(g_menuKey);

        CreateThread(nullptr, 0, hotkeyThread, nullptr, 0, nullptr);
    }

    const PluginInfo getPluginInfo() override
    {
        PluginInfo info;
        info.apiVersion = { 0, 0, 0 };
        info.version    = { 0, 1, 0 };
        info.name       = "PlayerModelSwap";
        return info;
    }
};

extern "C" __declspec(dllexport) PlayerModelSwapPlugin* getPlugin(DSCSModLoader& modLoader)
{
    return new PlayerModelSwapPlugin(modLoader);
}

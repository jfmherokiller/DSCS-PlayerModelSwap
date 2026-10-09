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
#include <dscs/GameInterface.h>
#include <modloader/plugin.h>
#include <modloader/sigscan.h>
#include <modloader/utils.h>

#include <MinHook.h>
#include <Windows.h>

#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <format>
#include <mutex>
#include <string>
#include <unordered_map>

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
} // namespace

// resources/PlayerModelSwap.ini
//   [PlayerModelSwap]
//   Model=chr320        ; Digimon model code (chr + 3 digits). Empty = protagonist.
//   Enabled=1
//   TemporaryHumanKey=119   ; decimal virtual-key code (F8), 0 disables the hotkey
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
            log(std::format("temporary human: {} (applies on the next map load)", g_temporaryHuman ? "on" : "off"));
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

        modLoader.addSquirrelFunction("PlayerModelSwap", "SetModel", nullptr, SqSetModel);
        modLoader.addSquirrelFunction("PlayerModelSwap", "GetModel", nullptr, SqGetModel);
        modLoader.addSquirrelFunction("PlayerModelSwap", "SetEnabled", nullptr, SqSetEnabled);

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

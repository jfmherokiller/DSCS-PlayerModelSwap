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
#include <string>

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

    // Field motions the CFieldPlayer requests -> battle motions every Digimon has
    // (fn = idle, fw = walk, fr = run, ff01/ff02 = alternate idle/move mode).
    struct MotionFallback
    {
        const char* field;
        const char* battle;
        const char* nativeModels; // models that ship the field motion (from DSDB/DSDBP)
    };
    constexpr MotionFallback MOTION_FALLBACKS[] = {
        { "fn01", "bn01", "chr320 chr781 chr783 chr805 chr811 chr816 chr999" },
        { "fw01", "br01", "chr320 chr805" },
        { "fr01", "br01", "chr320 chr805" },
        { "ff01", "bn01", "" },
        { "ff02", "br01", "" },
    };
    std::atomic<bool> g_remapMotions{ true };

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

    void* hookBuildMotionName(void* model, void* out, const char* motion)
    {
        if (g_remapMotions && g_enabled && !g_temporaryHuman && g_model[0] != '\0' && model != nullptr
            && motion != nullptr)
        {
            const char* name = static_cast<const char*>(model) + OFF_MODEL_NAME;
            if (std::strcmp(name, g_model) == 0)
            {
                for (const auto& fallback : MOTION_FALLBACKS)
                {
                    if (std::strcmp(motion, fallback.field) == 0)
                    {
                        if (std::strstr(fallback.nativeModels, name) == nullptr) motion = fallback.battle;
                        break;
                    }
                }
            }
        }
        return g_originalBuildMotionName(model, out, motion);
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
        // holds its bind pose when the field motions are missing.
        if (char* hit = findSignature(SIG_BUILD_MOTION_NAME))
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

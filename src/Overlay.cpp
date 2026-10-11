// In-game model picker: Dear ImGui drawn from a SwapBuffers hook (the game renders with OpenGL).
// While the menu is open the game's own input (DirectInput, XInput, cursor warps) is blanked.
#include "PlayerModelSwap.h"

#define DIRECTINPUT_VERSION 0x0800
#include <Windows.h>
#include <Xinput.h>
#include <dinput.h>
#include <gl/GL.h>

#include <MinHook.h>
#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_win32.h>

#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstring>
#include <format>
#include <string>
#include <vector>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam);

namespace
{
    constexpr GLenum GL_FRAMEBUFFER_              = 0x8D40;
    constexpr GLenum GL_DRAW_FRAMEBUFFER_BINDING_ = 0x8CA6;
    using BindFramebufferFn                       = void(APIENTRY*)(GLenum target, GLuint framebuffer);
    BindFramebufferFn g_glBindFramebuffer         = nullptr;

    int g_menuKey = 0;
    std::atomic<bool> g_open{ false };
    bool g_initialized        = false;
    bool g_failed             = false;
    HWND g_hwnd               = nullptr;
    WNDPROC g_originalWndProc = nullptr;

    std::vector<pms::ModelEntry> g_models;
    bool g_modelsLoaded = false;
    char g_filter[64]{};

    using SwapBuffersFn = BOOL(WINAPI*)(HDC);
    SwapBuffersFn g_originalSwapBuffers = nullptr;

    // --- input blocking while the menu is open ---
    using GetDeviceStateFn = HRESULT(STDMETHODCALLTYPE*)(void* self, DWORD size, void* data);
    using GetDeviceDataFn  = HRESULT(STDMETHODCALLTYPE*)(void* self, DWORD size, DIDEVICEOBJECTDATA* data,
                                                         DWORD* inOut, DWORD flags);
    using XInputGetStateFn = DWORD(WINAPI*)(DWORD user, XINPUT_STATE* state);
    using SetCursorPosFn   = BOOL(WINAPI*)(int x, int y);
    GetDeviceStateFn g_originalGetDeviceState[2]{};
    GetDeviceDataFn g_originalGetDeviceData[2]{};
    XInputGetStateFn g_originalXInputGetState = nullptr;
    SetCursorPosFn g_originalSetCursorPos     = nullptr;

    template <int I>
    HRESULT STDMETHODCALLTYPE hookGetDeviceState(void* self, DWORD size, void* data)
    {
        if (g_open)
        {
            // Give up exclusive access so Windows delivers mouse/keyboard messages and the cursor again.
            reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*)>((*reinterpret_cast<void***>(self))[8])(self);
            if (data) std::memset(data, 0, size);
            return DI_OK;
        }
        return g_originalGetDeviceState[I](self, size, data);
    }

    template <int I>
    HRESULT STDMETHODCALLTYPE hookGetDeviceData(void* self, DWORD size, DIDEVICEOBJECTDATA* data, DWORD* inOut,
                                                DWORD flags)
    {
        if (g_open)
        {
            reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*)>((*reinterpret_cast<void***>(self))[8])(self);
            if (inOut) *inOut = 0;
            return DI_OK;
        }
        return g_originalGetDeviceData[I](self, size, data, inOut, flags);
    }

    using AcquireFn             = HRESULT(STDMETHODCALLTYPE*)(void* self);
    using SetCooperativeLevelFn = HRESULT(STDMETHODCALLTYPE*)(void* self, HWND hwnd, DWORD flags);
    AcquireFn g_originalAcquire[2]{};
    SetCooperativeLevelFn g_originalSetCooperativeLevel[2]{};

    template <int I>
    HRESULT STDMETHODCALLTYPE hookAcquire(void* self)
    {
        return g_open ? DI_OK : g_originalAcquire[I](self); // stay unacquired while the menu is open
    }

    template <int I>
    HRESULT STDMETHODCALLTYPE hookSetCooperativeLevel(void* self, HWND hwnd, DWORD flags)
    {
        pms::log(std::format("DirectInput cooperative level 0x{:X} (exclusive={})", flags, (flags & DISCL_EXCLUSIVE) != 0));
        return g_originalSetCooperativeLevel[I](self, hwnd, flags);
    }

    DWORD WINAPI hookXInputGetState(DWORD user, XINPUT_STATE* state)
    {
        const DWORD result = g_originalXInputGetState(user, state);
        if (g_open && result == ERROR_SUCCESS && state) state->Gamepad = {};
        return result;
    }

    BOOL WINAPI hookSetCursorPos(int x, int y)
    {
        return g_open ? TRUE : g_originalSetCursorPos(x, y);
    }

    bool hook(void* target, void* detour, void** original)
    {
        return target && MH_CreateHook(target, detour, original) == MH_OK && MH_EnableHook(target) == MH_OK;
    }

    // Device methods live in dinput8.dll; take them from throwaway keyboard devices (W and A).
    void hookDirectInput()
    {
        void* seen = nullptr;
        for (int i = 0; i < 2; i++)
        {
            void* di = nullptr;
            if (FAILED(DirectInput8Create(GetModuleHandleW(nullptr), DIRECTINPUT_VERSION,
                                          i == 0 ? IID_IDirectInput8W : IID_IDirectInput8A, &di, nullptr)))
                continue;
            IDirectInputDevice8W* device = nullptr;
            if (SUCCEEDED(static_cast<IDirectInput8W*>(di)->CreateDevice(GUID_SysKeyboard, &device, nullptr)))
            {
                auto** vtbl = *reinterpret_cast<void***>(device);
                if (vtbl[9] != seen) // A and W may share implementations
                {
                    seen = vtbl[9];
                    hook(vtbl[9], i == 0 ? (void*)&hookGetDeviceState<0> : (void*)&hookGetDeviceState<1>,
                         reinterpret_cast<void**>(&g_originalGetDeviceState[i]));
                    hook(vtbl[10], i == 0 ? (void*)&hookGetDeviceData<0> : (void*)&hookGetDeviceData<1>,
                         reinterpret_cast<void**>(&g_originalGetDeviceData[i]));
                    hook(vtbl[7], i == 0 ? (void*)&hookAcquire<0> : (void*)&hookAcquire<1>,
                         reinterpret_cast<void**>(&g_originalAcquire[i]));
                    hook(vtbl[13], i == 0 ? (void*)&hookSetCooperativeLevel<0> : (void*)&hookSetCooperativeLevel<1>,
                         reinterpret_cast<void**>(&g_originalSetCooperativeLevel[i]));
                }
                device->Release();
            }
            static_cast<IDirectInput8W*>(di)->Release();
        }
    }

    std::atomic<int> g_messagesSeen{ 0 };

    LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
    {
        g_messagesSeen++;
        if (g_open)
        {
            ImGui_ImplWin32_WndProcHandler(hwnd, msg, wParam, lParam);
            if ((msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST) || (msg >= WM_KEYFIRST && msg <= WM_KEYLAST))
                return 0;
            if (msg == WM_SETCURSOR && LOWORD(lParam) == HTCLIENT) return TRUE; // ImGui draws the cursor
        }
        return CallWindowProcW(g_originalWndProc, hwnd, msg, wParam, lParam);
    }

    // Keys are polled (like the F8 hotkey): the game's DirectInput can keep WM_KEYDOWN from arriving.
    bool pressed(int key)
    {
        static bool down[256]{};
        const bool now  = key > 0 && key < 256 && (GetAsyncKeyState(key) & 0x8000) != 0;
        const bool edge = now && !down[key & 0xFF];
        if (key > 0 && key < 256) down[key] = now;
        return edge;
    }

    bool gameFocused()
    {
        DWORD pid = 0;
        GetWindowThreadProcessId(GetForegroundWindow(), &pid);
        return pid == GetCurrentProcessId();
    }

    void pollToggle()
    {
        const bool toggle = pressed(g_menuKey);
        const bool escape = pressed(VK_ESCAPE);
        if (!gameFocused()) return;
        if (toggle || (escape && g_open))
        {
            g_open = !g_open;
            if (g_open) ImGui::GetIO().ClearInputKeys();
            pms::log(std::format("menu {} (window messages seen so far: {})", g_open ? "opened" : "closed",
                                 g_messagesSeen.load()));
        }
    }

    // Mouse fallback for when the game's exclusive input hides window messages.
    void pollMouse()
    {
        ImGuiIO& io = ImGui::GetIO();
        POINT point{};
        if (GetCursorPos(&point) && ScreenToClient(g_hwnd, &point))
            io.AddMousePosEvent(static_cast<float>(point.x), static_cast<float>(point.y));
        const bool swapped = GetSystemMetrics(SM_SWAPBUTTON) != 0;
        io.AddMouseButtonEvent(0, (GetAsyncKeyState(swapped ? VK_RBUTTON : VK_LBUTTON) & 0x8000) != 0);
        io.AddMouseButtonEvent(1, (GetAsyncKeyState(swapped ? VK_LBUTTON : VK_RBUTTON) & 0x8000) != 0);
    }

    void addFonts()
    {
        ImGuiIO& io = ImGui::GetIO();
        char windir[MAX_PATH]{};
        GetWindowsDirectoryA(windir, MAX_PATH);
        const std::string fonts = std::string(windir) + "\\Fonts\\";
        if (GetFileAttributesA((fonts + "segoeui.ttf").c_str()) == INVALID_FILE_ATTRIBUTES)
        {
            io.Fonts->AddFontDefault();
            return;
        }
        io.Fonts->AddFontFromFileTTF((fonts + "segoeui.ttf").c_str(), 18.0f);
        // Japanese names (game language) fall back to a CJK font.
        for (const char* cjk : { "YuGothM.ttc", "meiryo.ttc", "msgothic.ttc" })
            if (GetFileAttributesA((fonts + cjk).c_str()) != INVALID_FILE_ATTRIBUTES)
            {
                ImFontConfig config;
                config.MergeMode = true;
                io.Fonts->AddFontFromFileTTF((fonts + cjk).c_str(), 18.0f, &config);
                break;
            }
    }

    bool initialize(HDC hdc)
    {
        g_hwnd              = WindowFromDC(hdc);
        g_glBindFramebuffer = reinterpret_cast<BindFramebufferFn>(wglGetProcAddress("glBindFramebuffer"));
        if (g_hwnd == nullptr) return false;

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGuiIO& io    = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        ImGui::StyleColorsDark();
        addFonts();

        if (!ImGui_ImplWin32_Init(g_hwnd)) return false;
        if (!ImGui_ImplOpenGL3_Init(nullptr))
        {
            ImGui_ImplWin32_Shutdown();
            return false;
        }
        g_originalWndProc =
            reinterpret_cast<WNDPROC>(SetWindowLongPtrW(g_hwnd, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&wndProc)));
        return g_originalWndProc != nullptr;
    }

    std::string lower(std::string s)
    {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    }

    bool matchesFilter(const pms::ModelEntry& entry)
    {
        if (g_filter[0] == '\0') return true;
        const std::string needle = lower(g_filter);
        return lower(entry.code).find(needle) != std::string::npos || lower(entry.name).find(needle) != std::string::npos;
    }

    void drawMenu()
    {
        if (!g_modelsLoaded)
        {
            g_models       = pms::listModels();
            g_modelsLoaded = true;
            pms::log(std::format("menu: {} Digimon models found", g_models.size()));
        }

        const ImGuiViewport* viewport = ImGui::GetMainViewport();
        ImGui::SetNextWindowPos(viewport->GetCenter(), ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
        ImGui::SetNextWindowSize(ImVec2(viewport->Size.x * 0.35f, viewport->Size.y * 0.7f), ImGuiCond_FirstUseEver);
        bool open = true;
        if (ImGui::Begin("Player Model Swap", &open, ImGuiWindowFlags_NoCollapse))
        {
            const std::string current = pms::getModel();
            std::string currentName   = current.empty() ? "Protagonist" : current;
            for (const auto& entry : g_models)
                if (entry.code == current && !entry.name.empty()) currentName = entry.name + " (" + current + ")";
            ImGui::Text("Playing as: %s", currentName.c_str());

            bool enabled = pms::getEnabled();
            if (ImGui::Checkbox("Enabled", &enabled)) pms::setEnabled(enabled);
            ImGui::SameLine();
            bool human = pms::getTemporaryHuman();
            if (ImGui::Checkbox("Human for now", &human)) pms::setTemporaryHuman(human);
            ImGui::SameLine();
            bool cutscenes = pms::getReplaceInCutscenes();
            if (ImGui::Checkbox("Also in cutscenes", &cutscenes)) pms::setReplaceInCutscenes(cutscenes);

            ImGui::SetNextItemWidth(-FLT_MIN);
            ImGui::InputTextWithHint("##filter", "Search by name or code", g_filter, sizeof(g_filter));

            const float footer              = ImGui::GetTextLineHeightWithSpacing() * 2.2f;
            constexpr ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY
                                              | ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_Resizable;
            if (ImGui::BeginTable("models", 3, flags, ImVec2(0, -footer)))
            {
                ImGui::TableSetupScrollFreeze(0, 1);
                ImGui::TableSetupColumn("Code", ImGuiTableColumnFlags_WidthFixed);
                ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Field anims", ImGuiTableColumnFlags_WidthFixed);
                ImGui::TableHeadersRow();

                constexpr ImGuiSelectableFlags rowFlags = ImGuiSelectableFlags_SpanAllColumns;
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                if (ImGui::Selectable("-", current.empty(), rowFlags)) pms::setModel("");
                ImGui::TableNextColumn();
                ImGui::TextUnformatted("Protagonist (no swap)");
                ImGui::TableNextColumn();

                for (const auto& entry : g_models)
                {
                    if (!matchesFilter(entry)) continue;
                    ImGui::TableNextRow();
                    ImGui::TableNextColumn();
                    if (ImGui::Selectable(entry.code.c_str(), entry.code == current, rowFlags))
                        pms::setModel(entry.code);
                    ImGui::TableNextColumn();
                    ImGui::TextUnformatted(entry.name.empty() ? "?" : entry.name.c_str());
                    ImGui::TableNextColumn();
                    if (entry.fieldMotions) ImGui::TextUnformatted("yes");
                }
                ImGui::EndTable();
            }
            ImGui::TextDisabled("Applies at once in the field; cutscenes and menus on their next load.");
            ImGui::TextDisabled("Without field anims the Digimon walks with its battle motions.");
        }
        ImGui::End();
        if (!open) g_open = false;
    }

    void renderMenu()
    {
        ImGui::GetIO().MouseDrawCursor = true;
        RECT rect{};
        GetClientRect(g_hwnd, &rect);
        ImGui::GetStyle().FontScaleMain = std::clamp((rect.bottom - rect.top) / 1080.0f, 1.0f, 2.5f);

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplWin32_NewFrame();
        pollMouse();
        ImGui::NewFrame();
        drawMenu();
        ImGui::Render();

        GLint framebuffer = 0;
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING_, &framebuffer);
        if (framebuffer != 0 && g_glBindFramebuffer) g_glBindFramebuffer(GL_FRAMEBUFFER_, 0);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        if (framebuffer != 0 && g_glBindFramebuffer) g_glBindFramebuffer(GL_FRAMEBUFFER_, framebuffer);
    }

    BOOL WINAPI hookSwapBuffers(HDC hdc)
    {
        pms::onFrame();
        if (g_menuKey != 0 && !g_failed)
        {
            if (!g_initialized)
            {
                g_initialized = initialize(hdc);
                g_failed      = !g_initialized;
                char cls[64]{};
                GetClassNameA(g_hwnd, cls, sizeof(cls));
                pms::log(g_initialized ? std::format("menu ready (virtual key {}, window class '{}', top-level={})",
                                                     g_menuKey, cls, GetAncestor(g_hwnd, GA_ROOT) == g_hwnd)
                                       : "menu: ImGui/OpenGL initialization failed; menu disabled");
            }
            if (g_initialized)
            {
                pollToggle();
                if (g_open) renderMenu();
            }
        }
        return g_originalSwapBuffers(hdc);
    }
} // namespace

void overlay::install(int menuKey)
{
    g_menuKey = menuKey;
    // MH_Initialize already ran in the plugin's onEnable.
    if (MH_CreateHookApi(L"gdi32.dll", "SwapBuffers", reinterpret_cast<void*>(&hookSwapBuffers),
                         reinterpret_cast<void**>(&g_originalSwapBuffers))
            != MH_OK
        || MH_EnableHook(MH_ALL_HOOKS) != MH_OK)
    {
        pms::log("SwapBuffers hook failed; no menu, and model changes apply on the next map load.");
        return;
    }
    if (menuKey == 0) return;

    hookDirectInput();
    if (GetModuleHandleW(L"xinput1_3.dll"))
        MH_CreateHookApi(L"xinput1_3.dll", "XInputGetState", reinterpret_cast<void*>(&hookXInputGetState),
                         reinterpret_cast<void**>(&g_originalXInputGetState));
    MH_CreateHookApi(L"user32.dll", "SetCursorPos", reinterpret_cast<void*>(&hookSetCursorPos),
                     reinterpret_cast<void**>(&g_originalSetCursorPos));
    MH_EnableHook(MH_ALL_HOOKS);
}

// Minecraft 1.21.11 (OpenGL) — HUD DLL.
// Хукает gdi32!SwapBuffers (его вызывает GLFW/LWJGL) и рисует HUD поверх кадра через ImGui.
// Не зависит от версии игры и маппингов.
//
// Клавиши (окно игры должно быть активно):
//   RIGHT SHIFT - показать/скрыть HUD
//   NUMPAD1..3 - переключить демо-модули (пока просто флаги для списка)
//   END     - выгрузить DLL

#include <windows.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "MinHook.h"
#include "imgui.h"
#include "imgui_impl_opengl3.h"

using SwapBuffers_t = BOOL(WINAPI*)(HDC);

static SwapBuffers_t     g_origSwap = nullptr;
static LPVOID            g_target   = nullptr;
static HMODULE           g_module   = nullptr;
static std::atomic<bool> g_unload{false};
static std::atomic<bool> g_cleaned{false};
static bool              g_init     = false;
static bool              g_showHud  = true;
static DWORD             g_startTick = 0;

// Лог в %TEMP%\mchud.log — чтобы можно было проверить, что DLL загрузилась
static void Log(const char* msg)
{
    char path[MAX_PATH];
    if (!GetTempPathA(MAX_PATH, path)) return;
    strcat_s(path, "mchud.log");
    FILE* f = nullptr;
    if (fopen_s(&f, path, "a") == 0 && f) { fprintf(f, "%s\n", msg); fclose(f); }
}

struct Module { const char* name; int key; bool enabled; bool prev; };
static std::vector<Module> g_modules = {
    { "Sprint",    VK_NUMPAD1, false, false },
    { "FullBright", VK_NUMPAD2, false, false },
    { "Zoom",      VK_NUMPAD3, false, false },
};

static bool KeyPressed(int vk, bool& prev)
{
    bool down = (GetAsyncKeyState(vk) & 0x8000) != 0;
    bool fired = down && !prev;
    prev = down;
    return fired;
}

static void HandleInput(HWND wnd)
{
    static bool pShift = false, pEnd = false;
    if (GetForegroundWindow() != wnd) return;

    if (KeyPressed(VK_RSHIFT, pShift)) g_showHud = !g_showHud;
    if (KeyPressed(VK_END, pEnd))    g_unload = true;
    for (auto& m : g_modules)
        if (KeyPressed(m.key, m.prev)) m.enabled = !m.enabled;
}

static void DrawTextShadow(ImDrawList* dl, ImVec2 pos, ImU32 col, const char* text)
{
    dl->AddText(ImVec2(pos.x + 1, pos.y + 1), IM_COL32(0, 0, 0, 200), text);
    dl->AddText(pos, col, text);
}

static void DrawHud(float w)
{
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    const float pad = 6.f;

    // Левый верхний угол: название + FPS
    static int   frames = 0;
    static float acc = 0.f;
    static int   fps = 0;
    frames++;
    acc += ImGui::GetIO().DeltaTime;
    if (acc >= 0.5f) { fps = (int)(frames / acc); frames = 0; acc = 0.f; }

    char buf[64];
    DrawTextShadow(dl, ImVec2(pad, pad), IM_COL32(120, 200, 255, 255), "MyClient 1.21.11");
    snprintf(buf, sizeof buf, "FPS: %d", fps);
    DrawTextShadow(dl, ImVec2(pad, pad + 16.f), IM_COL32(255, 255, 255, 255), buf);

    // Первые 8 секунд после инжекта: подтверждение, что DLL работает
    if (GetTickCount() - g_startTick < 8000)
        DrawTextShadow(dl, ImVec2(pad, pad + 32.f), IM_COL32(255, 220, 80, 255),
                       "Injected OK | RShift - hide/show HUD | END - unload");

    // Правый верхний угол: список включённых модулей (по длине, как ArrayList)
    std::vector<const char*> on;
    for (auto& m : g_modules) if (m.enabled) on.push_back(m.name);
    std::sort(on.begin(), on.end(), [](const char* a, const char* b) {
        return ImGui::CalcTextSize(a).x > ImGui::CalcTextSize(b).x;
    });
    float y = pad;
    for (auto* n : on) {
        ImVec2 sz = ImGui::CalcTextSize(n);
        DrawTextShadow(dl, ImVec2(w - sz.x - pad, y), IM_COL32(120, 255, 140, 255), n);
        y += sz.y + 2.f;
    }
}

static void Cleanup()
{
    if (g_init) {
        ImGui_ImplOpenGL3_Shutdown();
        ImGui::DestroyContext();
        g_init = false;
    }
    g_cleaned = true;
}

static void RenderHud(HDC hdc)
{
    HWND wnd = WindowFromDC(hdc);
    if (!wnd) return;

    if (!g_init) {
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.FontGlobalScale = 1.0f;
        ImGui::StyleColorsDark();
        // Minecraft использует core profile >= 3.2
        if (!ImGui_ImplOpenGL3_Init("#version 150")) { ImGui::DestroyContext(); return; }
        g_init = true;
    }

    HandleInput(wnd);

    RECT rc;
    GetClientRect(wnd, &rc);
    float w = (float)(rc.right - rc.left), h = (float)(rc.bottom - rc.top);
    if (w < 1.f || h < 1.f) return;

    static auto last = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    float dt = std::chrono::duration<float>(now - last).count();
    last = now;

    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(w, h);
    io.DeltaTime   = dt > 0.f ? dt : 1.f / 60.f;

    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();
    if (g_showHud) DrawHud(w);
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
}

static BOOL WINAPI hkSwapBuffers(HDC hdc)
{
    static thread_local bool busy = false;
    if (g_unload) {
        if (!g_cleaned) Cleanup();   // выполняется в потоке с активным GL-контекстом
    } else if (!busy) {
        busy = true;
        RenderHud(hdc);
        busy = false;
    }
    return g_origSwap(hdc);
}

static DWORD WINAPI MainThread(LPVOID)
{
    g_startTick = GetTickCount();
    Log("mchud: DLL loaded, installing hook");
    if (MH_Initialize() != MH_OK) { Log("mchud: MH_Initialize failed"); FreeLibraryAndExitThread(g_module, 1); }

    HMODULE gdi = GetModuleHandleW(L"gdi32.dll");
    if (!gdi) gdi = LoadLibraryW(L"gdi32.dll");
    g_target = gdi ? (LPVOID)GetProcAddress(gdi, "SwapBuffers") : nullptr;

    if (!g_target ||
        MH_CreateHook(g_target, (LPVOID)&hkSwapBuffers, (LPVOID*)&g_origSwap) != MH_OK ||
        MH_EnableHook(g_target) != MH_OK) {
        Log("mchud: hook FAILED");
        MH_Uninitialize();
        FreeLibraryAndExitThread(g_module, 1);
    }
    Log("mchud: hook installed OK");

    while (!g_unload) Sleep(50);

    // ждём, пока рендер-поток сам освободит ImGui (до 2 сек)
    for (int i = 0; i < 40 && !g_cleaned; ++i) Sleep(50);

    MH_DisableHook(g_target);
    Sleep(200);                 // дать выйти потокам, которые ещё внутри хука
    MH_RemoveHook(g_target);
    MH_Uninitialize();
    FreeLibraryAndExitThread(g_module, 0);
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH) {
        g_module = hModule;
        DisableThreadLibraryCalls(hModule);
        HANDLE t = CreateThread(nullptr, 0, MainThread, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    }
    return TRUE;
}

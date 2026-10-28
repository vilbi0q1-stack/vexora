// Minecraft 1.21.11 (OpenGL) — HUD DLL.
// Хукает gdi32!SwapBuffers и opengl32!wglSwapBuffers (их вызывает GLFW/LWJGL)
// и рисует HUD поверх кадра через ImGui. Не зависит от версии игры и маппингов.
//
// Клавиши (окно игры должно быть активно):
//   RIGHT SHIFT - показать/скрыть HUD
//   NUMPAD1..3  - переключить демо-модули (пока просто флаги для списка)
//   END         - выгрузить DLL
//
// Файлов и логов DLL не создаёт.

#include <windows.h>
#include <GL/gl.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "MinHook.h"
#include "imgui.h"
#include "imgui_impl_opengl3.h"

// Временная диагностика: окно "DLL загружена" при инжекте.
// Когда убедишься, что всё работает, поставь 0 и пересобери.
#define MCHUD_LOAD_POPUP 1

#ifndef GL_FRAMEBUFFER
#define GL_FRAMEBUFFER          0x8D40
#endif
#ifndef GL_FRAMEBUFFER_BINDING
#define GL_FRAMEBUFFER_BINDING  0x8CA6
#endif

using SwapBuffers_t = BOOL(WINAPI*)(HDC);
using glBindFramebuffer_t = void(WINAPI*)(GLenum, GLuint);

static SwapBuffers_t     g_origGdi = nullptr;
static SwapBuffers_t     g_origWgl = nullptr;
static LPVOID            g_targetGdi = nullptr;
static LPVOID            g_targetWgl = nullptr;
static HMODULE           g_module   = nullptr;
static std::atomic<bool> g_unload{false};
static std::atomic<bool> g_cleaned{false};
static bool              g_init     = false;
static bool              g_showHud  = true;
static DWORD             g_startTick = 0;
static glBindFramebuffer_t g_glBindFramebuffer = nullptr;

// Сообщение в отдельном потоке, чтобы не блокировать рендер/инициализацию
static DWORD WINAPI PopupThread(LPVOID p)
{
    char* msg = (char*)p;
    MessageBoxA(nullptr, msg, "mchud", MB_OK | MB_TOPMOST | MB_SETFOREGROUND);
    free(msg);
    return 0;
}
static void Popup(const char* msg)
{
    char* copy = _strdup(msg);
    if (!copy) return;
    HANDLE t = CreateThread(nullptr, 0, PopupThread, copy, 0, nullptr);
    if (t) CloseHandle(t); else free(copy);
}

struct Module { const char* name; int key; bool enabled; bool prev; };
static std::vector<Module> g_modules = {
    { "Sprint",     VK_NUMPAD1, false, false },
    { "FullBright", VK_NUMPAD2, false, false },
    { "Zoom",       VK_NUMPAD3, false, false },
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
    if (KeyPressed(VK_END, pEnd))      g_unload = true;
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
    if (!wglGetCurrentContext()) return;

    HWND wnd = WindowFromDC(hdc);
    if (!wnd) wnd = GetForegroundWindow();
    if (!wnd) return;

    static bool initFailedShown = false;
    if (!g_init) {
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        ImGui::StyleColorsDark();
        // Minecraft использует core profile >= 3.2
        if (!ImGui_ImplOpenGL3_Init("#version 150")) {
            ImGui::DestroyContext();
            if (!initFailedShown) { initFailedShown = true; Popup("mchud: ImGui OpenGL init failed"); }
            return;
        }
        g_glBindFramebuffer = (glBindFramebuffer_t)wglGetProcAddress("glBindFramebuffer");
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

    // Игра может оставить привязанным не основной буфер экрана и выключенную запись цвета —
    // тогда HUD рисуется "в никуда". Принудительно рисуем в буфер 0 и возвращаем всё назад.
    GLint prevFbo = 0;
    GLboolean prevMask[4] = { GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE };
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prevFbo);
    glGetBooleanv(GL_COLOR_WRITEMASK, prevMask);
    if (g_glBindFramebuffer) g_glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);

    ImGui_ImplOpenGL3_NewFrame();
    ImGui::NewFrame();
    if (g_showHud) DrawHud(w);
    ImGui::Render();
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

    glColorMask(prevMask[0], prevMask[1], prevMask[2], prevMask[3]);
    if (g_glBindFramebuffer) g_glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prevFbo);
}

static BOOL CommonSwap(HDC hdc, SwapBuffers_t orig)
{
    // Флаг держим на время вызова оригинала: wglSwapBuffers может внутри вызвать
    // gdi32!SwapBuffers, и тогда HUD нарисовался бы дважды за кадр.
    static thread_local bool inSwap = false;
    if (inSwap) return orig(hdc);

    inSwap = true;
    if (g_unload) {
        if (!g_cleaned) Cleanup();   // выполняется в потоке с активным GL-контекстом
    } else {
        RenderHud(hdc);
    }
    BOOL r = orig(hdc);
    inSwap = false;
    return r;
}

static BOOL WINAPI hkGdiSwap(HDC hdc) { return CommonSwap(hdc, g_origGdi); }
static BOOL WINAPI hkWglSwap(HDC hdc) { return CommonSwap(hdc, g_origWgl); }

static bool HookFunction(const wchar_t* dll, const char* name, LPVOID detour,
                         LPVOID* outTarget, LPVOID* outOrig)
{
    HMODULE m = GetModuleHandleW(dll);
    if (!m) m = LoadLibraryW(dll);
    if (!m) return false;
    LPVOID target = (LPVOID)GetProcAddress(m, name);
    if (!target) return false;
    if (MH_CreateHook(target, detour, outOrig) != MH_OK) return false;
    if (MH_EnableHook(target) != MH_OK) { MH_RemoveHook(target); return false; }
    *outTarget = target;
    return true;
}

static DWORD WINAPI MainThread(LPVOID)
{
    g_startTick = GetTickCount();

    if (MH_Initialize() != MH_OK) {
        Popup("mchud: MH_Initialize failed");
        FreeLibraryAndExitThread(g_module, 1);
    }

    bool okGdi = HookFunction(L"gdi32.dll", "SwapBuffers", (LPVOID)&hkGdiSwap,
                              &g_targetGdi, (LPVOID*)&g_origGdi);
    bool okWgl = HookFunction(L"opengl32.dll", "wglSwapBuffers", (LPVOID)&hkWglSwap,
                              &g_targetWgl, (LPVOID*)&g_origWgl);

    if (!okGdi && !okWgl) {
        Popup("mchud: hook FAILED (SwapBuffers not hooked)");
        MH_Uninitialize();
        FreeLibraryAndExitThread(g_module, 1);
    }

#if MCHUD_LOAD_POPUP
    Popup(okGdi && okWgl ? "mchud: DLL loaded, hooks: gdi32 OK, opengl32 OK"
        : okGdi          ? "mchud: DLL loaded, hooks: gdi32 OK, opengl32 FAILED"
                         : "mchud: DLL loaded, hooks: gdi32 FAILED, opengl32 OK");
#endif

    while (!g_unload) Sleep(50);

    // ждём, пока рендер-поток сам освободит ImGui (до 2 сек)
    for (int i = 0; i < 40 && !g_cleaned; ++i) Sleep(50);

    MH_DisableHook(MH_ALL_HOOKS);
    Sleep(200);                 // дать выйти потокам, которые ещё внутри хука
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

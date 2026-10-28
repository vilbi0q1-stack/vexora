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

// Отладочная консоль: окно cmd, которое открывает сама DLL (никаких файлов на диске).
// Когда убедишься, что всё работает, поставь 0 и пересобери — тогда консоли не будет вообще.
#define MCHUD_CONSOLE 1

#if MCHUD_CONSOLE
#include <cstdarg>
static void Log(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
    printf("\n");
    fflush(stdout);
}
static void OpenConsole()
{
    if (!AllocConsole()) return;
    FILE* f = nullptr;
    freopen_s(&f, "CONOUT$", "w", stdout);
    freopen_s(&f, "CONOUT$", "w", stderr);
    SetConsoleTitleA("mchud debug (do not close this window - use END in game)");
    // Крестик убираем: закрытие консоли убило бы весь процесс игры
    HWND c = GetConsoleWindow();
    if (c) {
        HMENU m = GetSystemMenu(c, FALSE);
        if (m) DeleteMenu(m, SC_CLOSE, MF_BYCOMMAND);
    }
}
static void CloseConsole() { FreeConsole(); }
#else
#define Log(...) ((void)0)
static void OpenConsole() {}
static void CloseConsole() {}
#endif

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
    if (!wglGetCurrentContext()) {
        static bool noCtx = false;
        if (!noCtx) { noCtx = true; Log("[render] no current GL context in this thread, skipping"); }
        return;
    }

    HWND wnd = WindowFromDC(hdc);
    if (!wnd) wnd = GetForegroundWindow();
    if (!wnd) {
        static bool noWnd = false;
        if (!noWnd) { noWnd = true; Log("[render] no window for DC, skipping"); }
        return;
    }

    if (!g_init) {
        static bool initFailed = false;
        Log("[render] GL_VERSION  = %s", (const char*)glGetString(GL_VERSION));
        Log("[render] GL_RENDERER = %s", (const char*)glGetString(GL_RENDERER));
        ImGui::CreateContext();
        ImGuiIO& io = ImGui::GetIO();
        io.IniFilename = nullptr;
        ImGui::StyleColorsDark();
        // Minecraft использует core profile >= 3.2
        if (!ImGui_ImplOpenGL3_Init("#version 150")) {
            ImGui::DestroyContext();
            if (!initFailed) { initFailed = true; Log("[render] ImGui_ImplOpenGL3_Init FAILED"); }
            return;
        }
        g_glBindFramebuffer = (glBindFramebuffer_t)wglGetProcAddress("glBindFramebuffer");
        Log("[render] ImGui init OK, glBindFramebuffer = %p", (void*)g_glBindFramebuffer);
        g_init = true;
    }

    HandleInput(wnd);

    RECT rc;
    GetClientRect(wnd, &rc);
    float w = (float)(rc.right - rc.left), h = (float)(rc.bottom - rc.top);
    if (w < 1.f || h < 1.f) return;

    static int frameCount = 0;
    static DWORD lastReport = 0;
    frameCount++;
    if (frameCount == 1) Log("[render] first frame drawn, window %dx%d", (int)w, (int)h);
    if (GetTickCount() - lastReport > 5000) {
        lastReport = GetTickCount();
        Log("[render] alive, frames drawn: %d", frameCount);
    }

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

static BOOL CommonSwap(HDC hdc, SwapBuffers_t orig, const char* tag)
{
    // Флаг держим на время вызова оригинала: wglSwapBuffers может внутри вызвать
    // gdi32!SwapBuffers, и тогда HUD нарисовался бы дважды за кадр.
    static thread_local bool inSwap = false;
    if (inSwap) return orig(hdc);

    static bool seenGdi = false, seenWgl = false;
    bool& seen = (tag[0] == 'g') ? seenGdi : seenWgl;
    if (!seen) { seen = true; Log("[hook] %s called by the game (thread %lu)", tag, GetCurrentThreadId()); }

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

static BOOL WINAPI hkGdiSwap(HDC hdc) { return CommonSwap(hdc, g_origGdi, "gdi32!SwapBuffers"); }
static BOOL WINAPI hkWglSwap(HDC hdc) { return CommonSwap(hdc, g_origWgl, "wglSwapBuffers"); }

static bool HookFunction(const wchar_t* dll, const char* name, LPVOID detour,
                         LPVOID* outTarget, LPVOID* outOrig)
{
    HMODULE m = GetModuleHandleW(dll);
    if (!m) m = LoadLibraryW(dll);
    if (!m) { Log("[init] %s: module not found", name); return false; }
    LPVOID target = (LPVOID)GetProcAddress(m, name);
    if (!target) { Log("[init] %s: GetProcAddress failed", name); return false; }
    MH_STATUS s = MH_CreateHook(target, detour, outOrig);
    if (s != MH_OK) { Log("[init] %s: MH_CreateHook failed: %s", name, MH_StatusToString(s)); return false; }
    s = MH_EnableHook(target);
    if (s != MH_OK) { Log("[init] %s: MH_EnableHook failed: %s", name, MH_StatusToString(s)); MH_RemoveHook(target); return false; }
    Log("[init] %s hooked at %p", name, target);
    *outTarget = target;
    return true;
}

static DWORD WINAPI MainThread(LPVOID)
{
    g_startTick = GetTickCount();
    OpenConsole();
    Log("[init] mchud loaded into pid %lu", GetCurrentProcessId());

    MH_STATUS ms = MH_Initialize();
    if (ms != MH_OK) {
        Log("[init] MH_Initialize failed: %s", MH_StatusToString(ms));
        Sleep(15000);               // дать время прочитать сообщение
        CloseConsole();
        FreeLibraryAndExitThread(g_module, 1);
    }

    bool okGdi = HookFunction(L"gdi32.dll", "SwapBuffers", (LPVOID)&hkGdiSwap,
                              &g_targetGdi, (LPVOID*)&g_origGdi);
    bool okWgl = HookFunction(L"opengl32.dll", "wglSwapBuffers", (LPVOID)&hkWglSwap,
                              &g_targetWgl, (LPVOID*)&g_origWgl);

    if (!okGdi && !okWgl) {
        Log("[init] no hook installed, unloading in 15 s");
        Sleep(15000);               // дать время прочитать сообщение
        MH_Uninitialize();
        CloseConsole();
        FreeLibraryAndExitThread(g_module, 1);
    }

    Log("[init] ready. Waiting for the game to swap buffers... (END in game = unload)");

    while (!g_unload) Sleep(50);

    Log("[unload] END pressed, cleaning up");

    // ждём, пока рендер-поток сам освободит ImGui (до 2 сек)
    for (int i = 0; i < 40 && !g_cleaned; ++i) Sleep(50);

    MH_DisableHook(MH_ALL_HOOKS);
    Sleep(200);                 // дать выйти потокам, которые ещё внутри хука
    MH_Uninitialize();
    Log("[unload] done");
    Sleep(500);
    CloseConsole();
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

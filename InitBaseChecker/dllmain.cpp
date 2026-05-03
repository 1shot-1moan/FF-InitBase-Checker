// InitBaseChecker/dllmain.cpp
// Console-based IL2CPP pointer chain validator — FF / FF MAX
// Injected into HD-Player.exe — entry: Load(pVM) called by ScriptKittens.dll

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>
#include <cstdint>
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <cstdio>
#include <cstring>
#include <cstdarg>

#include "PEBWalk.h"
#include "XorStr.h"
#include "Internalmemory.h"
#include "Adb.h"

// ─── Game profiles ────────────────────────────────────────────────────────────

struct Profile {
    const char* name;
    const char* pkg;
    uint32_t    initBase;      // known-good default
    bool        directFacade;
    bool        useRwSegment;
};

static constexpr Profile Profiles[2] = {
    { "Free Fire",     "com.dts.freefireth",  0x9EC1C48, false, false },
    { "Free Fire MAX", "com.dts.freefiremax", 0xB156C90, true,  true  },
};

static constexpr uint32_t OFF_STATICS = 0x5C;
static constexpr uint32_t OFF_MATCH   = 0x50;
static constexpr uint32_t OFF_PLAYER  = 0x94;

// ─── Shared state ─────────────────────────────────────────────────────────────

static std::atomic<int>      g_GameIdx  { -1 };
static std::atomic<uint32_t> g_Il2Cpp   { 0  };
static std::atomic<uint32_t> g_InitBase { 0  };  // user-provided value; 0 = use profile default
static std::atomic<bool>     g_Running  { true };
static std::atomic<bool>     g_Prompting{ false }; // render pauses while user types

static std::string g_AdbStatus = "Idle";
static std::mutex  g_AdbMtx;

struct ChainResult {
    uint32_t base      = 0;
    uint32_t facade    = 0;
    uint32_t statics   = 0;
    uint32_t game      = 0;
    uint32_t matchInst = 0;
    uint32_t player    = 0;
    int      failAt    = -2;  // -2=not started, -1=all OK, 0-5=step that failed
};

static ChainResult g_Chain;
static std::mutex  g_ChainMtx;

// ─── Console helpers ──────────────────────────────────────────────────────────

static HANDLE g_Con = INVALID_HANDLE_VALUE;

#define C_GRAY   (FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE)
#define C_WHITE  (FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY)
#define C_GREEN  (FOREGROUND_GREEN | FOREGROUND_INTENSITY)
#define C_RED    (FOREGROUND_RED   | FOREGROUND_INTENSITY)
#define C_YELLOW (FOREGROUND_RED   | FOREGROUND_GREEN | FOREGROUND_INTENSITY)
#define C_CYAN   (FOREGROUND_GREEN | FOREGROUND_BLUE  | FOREGROUND_INTENSITY)

static void SetCol(WORD w)            { SetConsoleTextAttribute(g_Con, w); }
static void GotoXY(short x, short y) { SetConsoleCursorPosition(g_Con, { x, y }); }
static void ShowCursor(bool show)     { CONSOLE_CURSOR_INFO ci = { 10, show ? TRUE : FALSE }; SetConsoleCursorInfo(g_Con, &ci); }

static void PL(const char* fmt, ...) {
    char buf[128] = {};
    va_list a; va_start(a, fmt); vsnprintf(buf, 127, fmt, a); va_end(a);
    printf("%-58s\n", buf);
}

// ─── InitBase input prompt ────────────────────────────────────────────────────
// Reads a hex InitBase from the user. Returns defaultVal if user presses Enter empty.

static uint32_t ReadInitBase(uint32_t defaultVal) {
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);

    ShowCursor(true);
    SetConsoleMode(hIn, ENABLE_PROCESSED_INPUT | ENABLE_ECHO_INPUT | ENABLE_LINE_INPUT);

    system("cls");
    SetCol(C_WHITE);
    printf("========================================================\n");
    printf("   SCRIPT KITTENS  -  INIT BASE CHECKER\n");
    printf("========================================================\n\n");
    SetCol(C_GRAY);
    printf("  Enter InitBase offset (hex, e.g. 9EC1C48)\n");
    printf("  Known default: ");
    SetCol(C_YELLOW); printf("0x%X\n", defaultVal);
    SetCol(C_GRAY);
    printf("  Press Enter alone to use the default.\n\n");
    SetCol(C_CYAN);   printf("  > 0x");
    SetCol(C_WHITE);  fflush(stdout);

    char buf[32] = {};
    fgets(buf, sizeof(buf), stdin);
    for (char& c : buf) { if (c == '\r' || c == '\n') { c = 0; break; } }

    SetConsoleMode(hIn, ENABLE_PROCESSED_INPUT);
    ShowCursor(false);

    if (buf[0] == 0) return defaultVal;

    // Strip 0x prefix if user typed it anyway
    const char* p = buf;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;

    uint32_t val = (uint32_t)strtoul(p, nullptr, 16);
    return val > 0 ? val : defaultVal;
}

// ─── ADB worker ───────────────────────────────────────────────────────────────

static void SetAdbStatus(const char* s) {
    std::lock_guard<std::mutex> lk(g_AdbMtx);
    g_AdbStatus = s;
}

static void AdbWorker() {
    int idx = g_GameIdx.load();
    if (idx < 0 || idx > 1) return;
    const Profile& P = Profiles[idx];

    uint32_t ib = g_InitBase.load();
    if (!ib) ib = P.initBase;

    SetAdbStatus("Finding HD-Player...");

    struct FD { HWND h = nullptr; };
    FD fd;
    EnumWindows([](HWND h, LPARAM lp) -> BOOL {
        char t[128] = {};
        GetWindowTextA(h, t, 128);
        if (strstr(t, "BlueStacks") || strstr(t, "HD-Player"))
            { reinterpret_cast<FD*>(lp)->h = h; return FALSE; }
        return TRUE;
    }, reinterpret_cast<LPARAM>(&fd));

    if (!fd.h) { SetAdbStatus("HD-Player not found!"); return; }

    DWORD pid = 0;
    GetWindowThreadProcessId(fd.h, &pid);
    char ep[MAX_PATH] = {};
    HANDLE hp = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (hp) { DWORD sz = MAX_PATH; QueryFullProcessImageNameA(hp, 0, ep, &sz); CloseHandle(hp); }
    std::string dir(ep);
    auto lsep = dir.find_last_of('\\');
    if (lsep != std::string::npos) dir = dir.substr(0, lsep + 1);

    Adb adb(dir + "HD-Adb.exe");

    SetAdbStatus("Restarting ADB...");
    adb.Kill(); Sleep(700); adb.Start();

    SetAdbStatus("Waiting for game...");
    bool up = false;
    for (int i = 0; i < 60 && g_Running.load(); i++) {
        std::string r = adb.RunAdbCommandWithOutput(std::string("shell pidof ") + P.pkg);
        if (!r.empty() && r.find("not found") == std::string::npos) { up = true; break; }
        Sleep(500);
    }
    if (!up) { SetAdbStatus("Game not running!"); return; }

    SetAdbStatus("Scanning libil2cpp.so...");
    uint32_t addr = 0;
    for (int i = 0; i < 80 && g_Running.load(); i++) {
        addr = adb.FindModule(P.pkg, "libil2cpp.so");
        if (addr > 0x10000) break;
        Sleep(500);
    }
    if (addr <= 0x10000) { SetAdbStatus("Module not found!"); return; }

    // FF MAX: probe all rw-p segments using the active InitBase
    if (P.useRwSegment) {
        SetAdbStatus("Probing rw-p segments...");
        for (uint32_t seg : adb.GetCachedSegments()) {
            uint32_t probe = 0;
            if (Mem::Read<uint32_t>((uint64_t)seg + ib, probe) && probe > 0x10000)
                { addr = seg; break; }
        }
    }

    g_Il2Cpp.store(addr);
    SetAdbStatus("Ready");
}

// ─── Chain walker ─────────────────────────────────────────────────────────────

static void ChainWalker() {
    while (g_Running.load()) {
        Sleep(400);
        int idx      = g_GameIdx.load();
        uint32_t il2 = g_Il2Cpp.load();
        if (idx < 0 || !il2) continue;

        const Profile& P = Profiles[idx];
        uint32_t ib = g_InitBase.load();
        if (!ib) ib = P.initBase;

        ChainResult r;
        r.failAt = -1;

#define TRY(addr_expr, field, step) \
        if (!Mem::Read<uint32_t>((uint64_t)(addr_expr), r.field) || r.field <= 0x10000) \
            { r.failAt = (step); goto chainDone; }

        TRY(il2 + ib,                base,      0)

        if (P.directFacade) {
            r.facade = r.base;
        } else {
            TRY(r.base,              facade,    1)
        }

        TRY(r.facade + OFF_STATICS,  statics,   2)
        TRY(r.statics,               game,      3)
        TRY(r.game    + OFF_MATCH,   matchInst, 4)
        TRY(r.matchInst + OFF_PLAYER, player,   5)

#undef TRY

    chainDone:
        { std::lock_guard<std::mutex> lk(g_ChainMtx); g_Chain = r; }
    }
}

// ─── Render ───────────────────────────────────────────────────────────────────

static void PrintStep(const char* label, uint32_t val, int step, int failAt, bool isDirectFacade = false) {
    printf("  %-8s : ", label);
    bool notReached = (failAt != -1 && failAt < step);
    bool failed     = (failAt == step);
    if (notReached)         { SetCol(C_GRAY);  printf("  ---                       "); }
    else if (isDirectFacade){ SetCol(C_CYAN);  printf("  0x%08X  [Direct]     ", val); }
    else if (failed)        { SetCol(C_RED);   printf("  0x%08X  [FAIL]       ", val); }
    else                    { SetCol(C_GREEN); printf("  0x%08X  [OK]         ", val); }
    SetCol(C_GRAY);
    printf("\n");
}

static void RenderLoop() {
    while (g_Running.load()) {
        Sleep(350);
        if (g_Prompting.load()) continue;   // user is typing — don't touch the console

        GotoXY(0, 0);

        int      idx  = g_GameIdx.load();
        uint32_t il2  = g_Il2Cpp.load();

        // Header
        SetCol(C_WHITE);
        PL("========================================================");
        PL("   SCRIPT KITTENS  -  INIT BASE CHECKER");
        PL("========================================================");
        SetCol(C_GRAY);

        if (idx < 0) {
            PL(""); PL("  Select game:"); PL("");
            SetCol(C_CYAN);   PL("    [1]  Free Fire");
            SetCol(C_YELLOW); PL("    [2]  Free Fire MAX");
            SetCol(C_GRAY);
            for (int i = 0; i < 13; i++) PL("");
            continue;
        }

        const Profile& P = Profiles[idx];

        // Resolve active InitBase — user value or profile default
        uint32_t ib       = g_InitBase.load();
        uint32_t activeIB = ib ? ib : P.initBase;
        bool     custom   = (ib != 0 && ib != P.initBase);

        // Info block (5 lines)
        printf("  Game    : "); SetCol(C_CYAN); PL("%s", P.name); SetCol(C_GRAY);
        PL("  Package : %s", P.pkg);
        printf("  InitBase: "); SetCol(custom ? C_YELLOW : C_CYAN);
        printf("0x%X", activeIB);
        SetCol(C_GRAY);
        printf("  %s", custom ? "[custom]" : "[default]");
        printf("  Facade: %s\n", P.directFacade ? "Direct" : "Indirect");

        // Il2Cpp line
        printf("  Il2Cpp  : ");
        if (il2) {
            SetCol(C_GREEN); printf("0x%08X", il2);
            SetCol(C_GRAY);  printf("  [ADB OK]\n");
        } else {
            std::string st; { std::lock_guard<std::mutex> lk(g_AdbMtx); st = g_AdbStatus; }
            SetCol(C_YELLOW); printf("%-44s\n", st.c_str()); SetCol(C_GRAY);
        }
        PL("");

        // Chain steps
        PL("--------------------------------------------------------");

        ChainResult r;
        { std::lock_guard<std::mutex> lk(g_ChainMtx); r = g_Chain; }

        if (!il2 || r.failAt == -2) {
            SetCol(C_YELLOW); PL("  Waiting for Il2Cpp base...");
            SetCol(C_GRAY);   for (int i = 0; i < 5; i++) PL("");
        } else {
            PrintStep("base",    r.base,      0, r.failAt);
            PrintStep("facade",  r.facade,    1, r.failAt, P.directFacade && r.failAt != 0);
            PrintStep("statics", r.statics,   2, r.failAt);
            PrintStep("game",    r.game,      3, r.failAt);
            PrintStep("match",   r.matchInst, 4, r.failAt);
            PrintStep("player",  r.player,    5, r.failAt);
        }

        PL("--------------------------------------------------------");

        // Result
        if (il2 && r.failAt != -2) {
            if (r.failAt == -1) {
                SetCol(C_GREEN);  PL("  RESULT : Chain VALID  -  player loaded!");
            } else if (r.failAt >= 3) {
                SetCol(C_YELLOW); PL("  RESULT : In menu / no match active");
            } else {
                SetCol(C_RED);    PL("  RESULT : FAIL at step %d  (wrong InitBase?)", r.failAt);
            }
        } else {
            SetCol(C_GRAY);       PL("  RESULT : ---");
        }
        SetCol(C_GRAY);

        // Footer
        PL("");
        SetCol(C_WHITE); PL("========================================================"); SetCol(C_GRAY);
        PL("  [I] InitBase   [R] Retry   [G] Game   [X] Exit");
    }
}

// ─── Input loop ───────────────────────────────────────────────────────────────

static void InputLoop() {
    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    SetConsoleMode(hIn, ENABLE_PROCESSED_INPUT);
    INPUT_RECORD ir; DWORD cnt;

    while (g_Running.load()) {
        if (WaitForSingleObject(hIn, 200) != WAIT_OBJECT_0) continue;
        if (!ReadConsoleInput(hIn, &ir, 1, &cnt)) continue;
        if (ir.EventType != KEY_EVENT || !ir.Event.KeyEvent.bKeyDown) continue;
        char k = ir.Event.KeyEvent.uChar.AsciiChar;

        if (k == 'x' || k == 'X') {
            g_Running.store(false);

        } else if (k == 'i' || k == 'I') {
            // Prompt for new InitBase — pause render while typing
            int idx2 = g_GameIdx.load();
            if (idx2 < 0) continue;
            g_Prompting.store(true);
            Sleep(100);  // let any in-progress render finish

            uint32_t newIB = ReadInitBase(Profiles[idx2].initBase);
            g_InitBase.store(newIB);

            // Reset chain and re-run ADB with new InitBase
            g_Il2Cpp.store(0);
            { std::lock_guard<std::mutex> lk(g_ChainMtx); g_Chain = {}; g_Chain.failAt = -2; }
            Mem::Flush();
            SetAdbStatus("Re-probing with new InitBase...");
            std::thread(AdbWorker).detach();

            system("cls");
            g_Prompting.store(false);

        } else if (k == 'r' || k == 'R') {
            g_Il2Cpp.store(0);
            { std::lock_guard<std::mutex> lk(g_ChainMtx); g_Chain = {}; g_Chain.failAt = -2; }
            Mem::Flush();
            SetAdbStatus("Retrying...");
            std::thread(AdbWorker).detach();

        } else if (k == 'g' || k == 'G') {
            g_GameIdx.store(-1);
            g_InitBase.store(0);
            g_Il2Cpp.store(0);
            { std::lock_guard<std::mutex> lk(g_ChainMtx); g_Chain = {}; g_Chain.failAt = -2; }
            Mem::Flush();
            SetAdbStatus("Idle");
        }
    }
}

// ─── Entry ────────────────────────────────────────────────────────────────────

static void Run() {
    AllocConsole();
    g_Con = GetStdHandle(STD_OUTPUT_HANDLE);
    FILE *fo = nullptr, *fi = nullptr;
    freopen_s(&fo, "CONOUT$", "w", stdout);
    freopen_s(&fi, "CONIN$",  "r", stdin);

    ShowCursor(false);
    COORD bufSz = { 60, 22 };
    SetConsoleScreenBufferSize(g_Con, bufSz);
    SMALL_RECT winSz = { 0, 0, 59, 21 };
    SetConsoleWindowInfo(g_Con, TRUE, &winSz);
    SetConsoleTitleA("Script Kittens - Init Base Checker");
    system("cls");

    // ── Step 1: Select game ──
    SetCol(C_WHITE);
    printf("========================================================\n");
    printf("   SCRIPT KITTENS  -  INIT BASE CHECKER\n");
    printf("========================================================\n\n");
    SetCol(C_GRAY);  printf("  Select game:\n\n");
    SetCol(C_CYAN);   printf("    [1]  Free Fire\n");
    SetCol(C_YELLOW); printf("    [2]  Free Fire MAX\n");
    SetCol(C_GRAY);   printf("\n  Press 1 or 2 ...\n");

    HANDLE hIn = GetStdHandle(STD_INPUT_HANDLE);
    SetConsoleMode(hIn, ENABLE_PROCESSED_INPUT);
    INPUT_RECORD ir; DWORD cnt;
    while (g_GameIdx.load() < 0) {
        if (WaitForSingleObject(hIn, 200) != WAIT_OBJECT_0) continue;
        if (!ReadConsoleInput(hIn, &ir, 1, &cnt)) continue;
        if (ir.EventType == KEY_EVENT && ir.Event.KeyEvent.bKeyDown) {
            char k = ir.Event.KeyEvent.uChar.AsciiChar;
            if (k == '1') g_GameIdx.store(0);
            if (k == '2') g_GameIdx.store(1);
        }
    }

    // ── Step 2: Enter InitBase ──
    int idx = g_GameIdx.load();
    g_InitBase.store(ReadInitBase(Profiles[idx].initBase));

    system("cls");

    std::thread(AdbWorker).detach();
    std::thread(ChainWalker).detach();
    std::thread(InputLoop).detach();

    RenderLoop();
    FreeConsole();
}

// ─── DLL exports ──────────────────────────────────────────────────────────────

extern "C" __declspec(dllexport) void __stdcall Load(void* pVM) {
    if (!pVM) return;
    if (!Mem::Init(pVM)) return;
    HANDLE hDone = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    std::thread([hDone]() { Run(); SetEvent(hDone); }).detach();
    WaitForSingleObject(hDone, INFINITE);
    CloseHandle(hDone);
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) DisableThreadLibraryCalls(hModule);
    return TRUE;
}

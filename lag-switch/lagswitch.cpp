// lagswitch.cpp
// A lightweight "lag switch" for Windows - the optimized successor to ipflip.
//
// ipflip cut the net with `ipconfig /release` and restored it with
// `ipconfig /renew`. That tears down the whole DHCP lease: slow (2-5s), hits the
// router's DHCP server, drops every connection, and can change your IP.
//
// This instead toggles ONE Windows Firewall "block outbound" rule. Flipping it
// on/off is instant and purely local - the router never sees a thing.
//
// Same feel as ipflip:
//   * pick a key (single character)
//   * pick an auto-restore timeout in seconds (recommended 9)
//   * HOLD the key          -> outbound traffic is cut
//   * restore fires on the FIRST of { you release the key, timer expires }
//   * after an auto-restore while you keep holding, it won't cut again until
//     you release and press once more
//
// A low-level keyboard hook is used so key-down and key-up are seen separately
// (RegisterHotKey can't see key-up). The bound key is swallowed globally so it
// won't type into other apps while this runs. Quit with Ctrl+C / close window.
//
// Requires administrator (creating firewall rules needs elevation).
//
// Build (MinGW):  g++ -O2 -static lagswitch.cpp manifest.res -o lagswitch.exe \
//                     -lole32 -loleaut32 -luuid
// Build (MSVC) :  cl /EHsc /O2 /utf-8 lagswitch.cpp /link ole32.lib oleaut32.lib

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <netfw.h>
#include <oleauto.h>
#include <stdio.h>
#include <string>
#include <atomic>
#include <thread>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")

static const wchar_t *RULE_NAME = L"LagSwitch_BlockOutbound";
#define WM_APP_RESTORE (WM_APP + 1)

// ---- firewall state --------------------------------------------------------

static INetFwPolicy2 *gPolicy = nullptr;
static INetFwRules   *gRules  = nullptr;
static INetFwRule    *gRule   = nullptr;

static DWORD              gMainTid   = 0;
static int                gVk        = 0;      // bound virtual-key
static int                gSeconds   = 9;      // auto-restore timeout
static std::atomic<bool>  gDown{false};        // physical key currently held
static std::atomic<bool>  gCut{false};         // outbound currently blocked
static std::atomic<int>   gActivation{0};      // invalidates stale timers

static void RemoveStaleRule() {
    if (!gRules) return;
    BSTR name = SysAllocString(RULE_NAME);
    gRules->Remove(name);
    SysFreeString(name);
}

static bool CreateRule() {
    if (FAILED(CoCreateInstance(__uuidof(NetFwPolicy2), nullptr,
            CLSCTX_INPROC_SERVER, __uuidof(INetFwPolicy2), (void **)&gPolicy)))
        return false;
    if (FAILED(gPolicy->get_Rules(&gRules)))
        return false;

    RemoveStaleRule(); // clear any leftover from a previous crash

    if (FAILED(CoCreateInstance(__uuidof(NetFwRule), nullptr,
            CLSCTX_INPROC_SERVER, __uuidof(INetFwRule), (void **)&gRule)))
        return false;

    BSTR name = SysAllocString(RULE_NAME);
    BSTR desc = SysAllocString(L"Temporary outbound block toggled by lagswitch.");
    gRule->put_Name(name);
    gRule->put_Description(desc);
    gRule->put_Direction(NET_FW_RULE_DIR_OUT);
    gRule->put_Action(NET_FW_ACTION_BLOCK);
    gRule->put_Protocol(256);            // NET_FW_IP_PROTOCOL_ANY
    gRule->put_Profiles(0x7FFFFFFF);     // NET_FW_PROFILE2_ALL
    gRule->put_Enabled(VARIANT_FALSE);   // start OFF
    SysFreeString(name);
    SysFreeString(desc);

    return SUCCEEDED(gRules->Add(gRule));
}

static void SetRule(bool on) {
    if (gRule) gRule->put_Enabled(on ? VARIANT_TRUE : VARIANT_FALSE);
}

static void Cleanup() {
    SetRule(false);
    RemoveStaleRule();
    if (gRule)   { gRule->Release();   gRule = nullptr; }
    if (gRules)  { gRules->Release();  gRules = nullptr; }
    if (gPolicy) { gPolicy->Release(); gPolicy = nullptr; }
    CoUninitialize();
}

static BOOL WINAPI CtrlHandler(DWORD) { Cleanup(); return FALSE; }

// All firewall toggling happens on the main thread (COM STA). Status prints here.
static void Cut() {
    gCut = true;
    SetRule(true);
    printf("\r  * CUT       (auto-restore in %ds)        \n", gSeconds);
    fflush(stdout);
}

static void Restore(const char *why) {
    gCut = false;
    SetRule(false);
    printf("\r  * restored  (%s)                         \n", why);
    fflush(stdout);
}

// ---- keyboard hook ---------------------------------------------------------

static HHOOK gHook = nullptr;

static LRESULT CALLBACK HookProc(int code, WPARAM wp, LPARAM lp) {
    if (code == HC_ACTION) {
        KBDLLHOOKSTRUCT *k = (KBDLLHOOKSTRUCT *)lp;
        if ((int)k->vkCode == gVk) {
            bool isDown = (wp == WM_KEYDOWN || wp == WM_SYSKEYDOWN);
            bool isUp   = (wp == WM_KEYUP   || wp == WM_SYSKEYUP);

            if (isDown && !gDown.exchange(true)) {       // first press (no repeat)
                int id = ++gActivation;
                Cut();
                int secs = gSeconds;
                std::thread([id, secs]() {               // auto-restore timer
                    std::this_thread::sleep_for(std::chrono::seconds(secs));
                    if (gActivation.load() == id && gCut.load())
                        PostThreadMessageW(gMainTid, WM_APP_RESTORE, id, 0);
                }).detach();
            } else if (isUp) {
                gDown = false;
                ++gActivation;                           // cancel pending timer
                if (gCut.load()) Restore("released");
            }
            return 1; // swallow the key so it doesn't type elsewhere
        }
    }
    return CallNextHookEx(gHook, code, wp, lp);
}

// ---- menu ------------------------------------------------------------------

static const char *kArt =
"\n"
"⠀⠀⠀⠀⠀⠀⠀⣀⣤⣶⣿⠷⠾⠛⠛⠛⠛⠷⠶⢶⣶⣤⣄⡀⠀⠀⠀⠀⠀⠀\n"
"⠀⠀⠀⠀⣀⣴⡾⠛⠉⠁⠀⣰⡶⠶⠶⠶⠶⠶⣶⡄⠀⠉⠛⠿⣷⣄⡀⠀⠀⠀\n"
"⠀⠀⣠⣾⠟⠁⠀⠀⠀⠀⠀⢸⡇⠀⠀⠀⠀⠀⣼⠃⠀⠀⠀⠀⠈⠛⢿⣦⡀⠀\n"
"⢠⣼⠟⠁⠀⠀⠀⠀⣠⣴⣶⣿⡇⠀⠀⠀⠀⠀⣿⣷⣦⣄⠀⠀⠀⠀⠀⠙⣧⡀\n"
"⣿⡇⠀⠀⠀⢀⣴⣾⣿⣿⣿⣿⣇⠀⠀⠀⠀⠸⣿⣿⣿⣿⣿⣦⡀⠀⠀⠀⢈⣷\n"
"⣿⣿⣦⡀⣠⣾⣿⣿⣿⡿⠟⢻⣿⠀⠀⠀⠀⢠⣿⠻⢿⣿⣿⣿⣿⣆⣀⣠⣾⣿\n"
"⠉⠻⣿⣿⣿⣿⣽⡿⠋⠀⠀⠸⣿⠀⠀⠀⠀⢸⡿⠀⠀⠉⠻⣿⣿⣿⣿⣿⠟⠁\n"
"⠀⠀⠈⠙⠛⣿⣿⠀⠀⠀⠀⢀⣿⠀⠀⠀⠀⢸⣇⠀⠀⠀⠀⣹⣿⡟⠋⠁⠀⠀\n"
"⠀⠀⠀⠀⠀⢿⣿⣷⣄⣀⣴⣿⣿⣤⣤⣤⣤⣼⣿⣷⣀⣀⣾⣿⣿⠇⠀⠀⠀⠀\n"
"⠀⠀⠀⠀⠀⠈⠻⢿⣿⣿⣿⣿⣿⠟⠛⠛⠻⣿⣿⣿⣿⣿⡿⠛⠉⠀⠀⠀⠀⠀\n"
"⠀⠀⠀⠀⠀⠀⠀⠀⠉⠉⠁⣿⡇⠀⠀⠀⠀⢸⣿⡏⠙⠋⠁⠀⠀⠀⠀⠀⠀⠀\n"
"⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⣿⣷⣄⠀⠀⣀⣾⣿⡇⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀\n"
"⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠙⢿⣿⣿⣿⣿⣿⣏⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀⠀\n"
"\n"
"        lagswitch - firewall hold-to-cut switch\n\n";

static int AskKey() {
    for (;;) {
        printf("  Bind which key? (single character): ");
        fflush(stdout);
        char line[64];
        if (!fgets(line, sizeof(line), stdin)) return 'P';
        for (char *p = line; *p; ++p) {
            if (*p > ' ') {                         // first printable char
                SHORT s = VkKeyScanA(*p);
                if (s != -1) return s & 0xFF;       // low byte = virtual-key
            }
        }
        printf("  Please type one character.\n");
    }
}

static int AskSeconds() {
    printf("  Auto-restore after how many seconds? [recommended: 9]: ");
    fflush(stdout);
    char line[64];
    if (!fgets(line, sizeof(line), stdin)) return 9;
    int n = atoi(line);
    return (n > 0) ? n : 9;                          // blank/invalid -> 9
}

// ---------------------------------------------------------------------------

int main() {
    SetConsoleOutputCP(CP_UTF8);     // so the Braille logo renders
    SetConsoleTitleW(L"Lag Switch");

    if (FAILED(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED))) {
        printf("[!] COM init failed.\n");
        return 1;
    }
    SetConsoleCtrlHandler(CtrlHandler, TRUE);

    if (!CreateRule()) {
        printf("[!] Could not create the firewall rule.\n");
        printf("    Make sure you ran this as administrator.\n\n");
        Cleanup();
        printf("(Press Enter to exit.)\n");
        getchar();
        return 1;
    }

    fputs(kArt, stdout);
    gVk      = AskKey();
    gSeconds = AskSeconds();

    printf("\n  Ready. Hold your key -> CUT   |   restores on release or after %ds.\n",
           gSeconds);
    printf("  Ctrl+C or close window to quit (auto-restores net).\n\n");

    gMainTid = GetCurrentThreadId();
    gHook = SetWindowsHookExW(WH_KEYBOARD_LL, HookProc, GetModuleHandleW(NULL), 0);
    if (!gHook) {
        printf("[!] Failed to install keyboard hook.\n");
        Cleanup();
        return 1;
    }

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0)) {
        if (msg.message == WM_APP_RESTORE) {
            if (gActivation.load() == (int)msg.wParam && gCut.load())
                Restore("auto");                    // timer fired, still held
            continue;
        }
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    UnhookWindowsHookEx(gHook);
    Cleanup();
    return 0;
}

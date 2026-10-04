// roblox_multi.cpp
// Universal Roblox multi-instance unlocker.
//
// Roblox enforces single-instance by holding a named Event object:
//     \Sessions\<N>\BaseNamedObjects\ROBLOX_singletonEvent
// (Some builds also create ROBLOX_singletonMutex.)
//
// While that object exists, a second RobloxPlayerBeta.exe refuses to launch.
// Closing every open handle to it destroys the object; the next launch creates
// a fresh one, so multiple clients can coexist.
//
// This tool enumerates all system handles, finds objects named
// "...ROBLOX_singletonEvent" (session-agnostic => universal) and closes them in
// their owning process. It does not read/modify Roblox memory or inject code.
//
// Run as Administrator.

#include <windows.h>
#include <winternl.h>
#include <stdio.h>
#include <conio.h>
#include <string>
#include <unordered_map>

#pragma comment(lib, "ntdll.lib")

#ifndef NT_SUCCESS
#define NT_SUCCESS(Status) (((NTSTATUS)(Status)) >= 0)
#endif
#ifndef STATUS_INFO_LENGTH_MISMATCH
#define STATUS_INFO_LENGTH_MISMATCH ((NTSTATUS)0xC0000004L)
#endif

// --- NT structures / enums not exposed in winternl.h -----------------------

typedef struct _SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX {
    PVOID       Object;
    ULONG_PTR   UniqueProcessId;
    HANDLE      HandleValue;
    ULONG       GrantedAccess;
    USHORT      CreatorBackTraceIndex;
    USHORT      ObjectTypeIndex;
    ULONG       HandleAttributes;
    ULONG       Reserved;
} SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX;

typedef struct _SYSTEM_HANDLE_INFORMATION_EX {
    ULONG_PTR NumberOfHandles;
    ULONG_PTR Reserved;
    SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX Handles[1];
} SYSTEM_HANDLE_INFORMATION_EX;

typedef struct _OBJECT_TYPE_INFORMATION_T {
    UNICODE_STRING TypeName;
    ULONG Reserved[22];
} OBJECT_TYPE_INFORMATION_T;

typedef struct _OBJECT_NAME_INFORMATION_T {
    UNICODE_STRING Name;
    WCHAR NameBuffer[1];
} OBJECT_NAME_INFORMATION_T;

static const SYSTEM_INFORMATION_CLASS SystemExtendedHandleInformation =
    (SYSTEM_INFORMATION_CLASS)0x40;
static const OBJECT_INFORMATION_CLASS ObjectNameInformationC =
    (OBJECT_INFORMATION_CLASS)1;
static const OBJECT_INFORMATION_CLASS ObjectTypeInformationC =
    (OBJECT_INFORMATION_CLASS)2;

typedef NTSTATUS (NTAPI *pNtQueryObject)(
    HANDLE, OBJECT_INFORMATION_CLASS, PVOID, ULONG, PULONG);
typedef NTSTATUS (NTAPI *pNtQuerySystemInformation)(
    SYSTEM_INFORMATION_CLASS, PVOID, ULONG, PULONG);

// ---------------------------------------------------------------------------

static const wchar_t *TARGET_SUFFIX = L"ROBLOX_singletonEvent";

static bool EnableDebugPrivilege() {
    HANDLE tok;
    if (!OpenProcessToken(GetCurrentProcess(),
                          TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &tok))
        return false;
    TOKEN_PRIVILEGES tp = {};
    tp.PrivilegeCount = 1;
    if (!LookupPrivilegeValueW(NULL, L"SeDebugPrivilege", &tp.Privileges[0].Luid)) {
        CloseHandle(tok);
        return false;
    }
    tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
    BOOL ok = AdjustTokenPrivileges(tok, FALSE, &tp, sizeof(tp), NULL, NULL);
    DWORD err = GetLastError();
    CloseHandle(tok);
    return ok && err == ERROR_SUCCESS;
}

// Case-insensitive "does `s` end with `suffix`".
static bool EndsWithI(const std::wstring &s, const wchar_t *suffix) {
    size_t sl = wcslen(suffix);
    if (s.size() < sl) return false;
    return _wcsnicmp(s.c_str() + (s.size() - sl), suffix, sl) == 0;
}

// Scan all handles, close any ...ROBLOX_singletonEvent.
// Returns the number closed; sets `found` to the number of matches seen.
static int KillSingleton(int &found) {
    found = 0;
    int closed = 0;

    pNtQuerySystemInformation NtQuerySystemInformation =
        (pNtQuerySystemInformation)GetProcAddress(
            GetModuleHandleW(L"ntdll.dll"), "NtQuerySystemInformation");
    pNtQueryObject NtQueryObject =
        (pNtQueryObject)GetProcAddress(
            GetModuleHandleW(L"ntdll.dll"), "NtQueryObject");
    if (!NtQuerySystemInformation || !NtQueryObject)
        return 0;

    // 1) Snapshot all system handles (grow buffer until it fits).
    ULONG len = 1 << 20;
    std::string buf;
    NTSTATUS st;
    for (;;) {
        buf.resize(len);
        ULONG need = 0;
        st = NtQuerySystemInformation(SystemExtendedHandleInformation,
                                      buf.data(), len, &need);
        if (st == STATUS_INFO_LENGTH_MISMATCH) {
            len = (need ? need : len * 2) + (1 << 16);
            continue;
        }
        break;
    }
    if (!NT_SUCCESS(st))
        return 0;

    auto *info = reinterpret_cast<SYSTEM_HANDLE_INFORMATION_EX *>(buf.data());
    ULONG_PTR count = info->NumberOfHandles;

    HANDLE self = GetCurrentProcess();
    std::unordered_map<ULONG_PTR, HANDLE> procCache; // pid -> PROCESS_DUP_HANDLE

    std::string tbuf(4096, '\0');
    std::string nbuf(8192, '\0');

    for (ULONG_PTR i = 0; i < count; i++) {
        SYSTEM_HANDLE_TABLE_ENTRY_INFO_EX &h = info->Handles[i];

        auto it = procCache.find(h.UniqueProcessId);
        HANDLE proc;
        if (it == procCache.end()) {
            proc = OpenProcess(PROCESS_DUP_HANDLE, FALSE,
                               (DWORD)h.UniqueProcessId);
            procCache[h.UniqueProcessId] = proc; // may be NULL; cache either way
        } else {
            proc = it->second;
        }
        if (!proc) continue;

        HANDLE dup = NULL;
        if (!DuplicateHandle(proc, h.HandleValue, self, &dup,
                             0, FALSE, DUPLICATE_SAME_ACCESS))
            continue;

        // Check type first -> only query names of Event objects.
        // (Avoids the well-known NtQueryObject hang on sync named pipes.)
        ULONG rl = 0;
        st = NtQueryObject(dup, ObjectTypeInformationC,
                           tbuf.data(), (ULONG)tbuf.size(), &rl);
        bool isEvent = false;
        if (NT_SUCCESS(st)) {
            auto *ti = reinterpret_cast<OBJECT_TYPE_INFORMATION_T *>(tbuf.data());
            if (ti->TypeName.Buffer &&
                _wcsnicmp(ti->TypeName.Buffer, L"Event", 5) == 0 &&
                ti->TypeName.Length == 5 * sizeof(WCHAR))
                isEvent = true;
        }
        if (!isEvent) { CloseHandle(dup); continue; }

        rl = 0;
        st = NtQueryObject(dup, ObjectNameInformationC,
                           nbuf.data(), (ULONG)nbuf.size(), &rl);
        if (NT_SUCCESS(st)) {
            auto *ni = reinterpret_cast<OBJECT_NAME_INFORMATION_T *>(nbuf.data());
            if (ni->Name.Buffer && ni->Name.Length) {
                std::wstring name(ni->Name.Buffer,
                                  ni->Name.Length / sizeof(WCHAR));
                if (EndsWithI(name, TARGET_SUFFIX)) {
                    found++;
                    // Close the handle in the *owning* process.
                    HANDLE sink = NULL;
                    if (DuplicateHandle(proc, h.HandleValue, self, &sink,
                                        0, FALSE, DUPLICATE_CLOSE_SOURCE)) {
                        if (sink) CloseHandle(sink);
                        closed++;
                    }
                }
            }
        }
        CloseHandle(dup);
    }

    for (auto &kv : procCache)
        if (kv.second) CloseHandle(kv.second);

    return closed;
}

static void HangForever() {
    // Stay open until the user closes the window.
    for (;;) Sleep(1000);
}

static const char *ASCII_ART =
"\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA2\x80\xE2\xA3\xA4\xE2\xA3\xB4\xE2\xA3\xB6\xE2\xA3\xB6\xE2\xA3\xA4\xE2\xA3\xA4\xE2\xA3\xA4\xE2\xA3\xB6\xE2\xA3\xA4\xE2\xA1\x84\n"
"\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA3\xB4\xE2\xA3\xBF\xE2\xA3\xBF\xE2\xA1\xBF\xE2\xA0\x9F\xE2\xA0\x9B\xE2\xA0\x9B\xE2\xA0\x9B\xE2\xA0\xBF\xE2\xA3\xBF\xE2\xA3\xA7\n"
"\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA2\xB8\xE2\xA3\xBF\xE2\xA3\xBF\xE2\xA0\x83\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x98\xE2\xA3\xBF\xE2\xA3\xBF\xE2\xA3\xB7\xE2\xA3\xB6\xE2\xA1\x84\n"
"\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA2\xBF\xE2\xA3\xBF\xE2\xA3\xAF\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA2\xB8\xE2\xA3\xBF\xE2\xA3\xBF\xE2\xA0\x9B\xE2\xA0\x81\n"
"\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x98\xE2\xA2\xBF\xE2\xA3\xBF\xE2\xA3\xA6\xE2\xA1\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA2\xB8\xE2\xA3\xBF\xE2\xA3\xBF\xE2\xA3\xA4\xE2\xA3\x84\n"
"\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x99\xE2\xA0\xBF\xE2\xA0\xBF\xE2\xA0\xBF\xE2\xA0\xBF\xE2\xA0\xBF\xE2\xA0\x8B\xE2\xA0\x80\xE2\xA0\x80\xE2\xA2\xB8\xE2\xA3\xBF\xE2\xA3\xBF\xE2\xA3\xBF\xE2\xA3\xBF\n"
"\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA3\xA0\xE2\xA3\xBF\xE2\xA3\xBF\xE2\xA0\x8B\xE2\xA0\x89\n"
"\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA0\x80\xE2\xA3\xA0\xE2\xA3\xB6\xE2\xA3\xB6\xE2\xA3\xB6\xE2\xA3\xB6\xE2\xA3\xB6\xE2\xA3\xB6\xE2\xA3\xA4\xE2\xA3\x84\xE2\xA3\x80\xE2\xA3\x80\xE2\xA3\x80\xE2\xA3\x80\xE2\xA3\xA4\xE2\xA3\xB6\xE2\xA3\xBF\xE2\xA3\xBF\xE2\xA3\xBF\xE2\xA1\x80\n"
"\xE2\xA0\x80\xE2\xA0\x80\xE2\xA3\xA0\xE2\xA3\xBF\xE2\xA3\xBF\xE2\xA3\xBF\xE2\xA3\xBF\xE2\xA3\xBF\xE2\xA3\xBF\xE2\xA3\xBF\xE2\xA3\xBF\xE2\xA3\xBF\xE2\xA3\xBF\xE2\xA3\xBF\xE2\xA3\xBF\xE2\xA3\xBF\xE2\xA3\xBF\xE2\xA3\xBF\xE2\xA3\xBF\xE2\xA3\xBF\xE2\xA3\xBF\xE2\xA1\xBF\n"
"\xE2\xA0\x80\xE2\xA2\xB0\xE2\xA3\xBF\xE2\xA3\xBF\xE2\xA3\xBF\xE2\xA3\xBF\xE2\xA3\xBF\xE2\xA3\xBF\xE2\xA3\xBF\xE2\xA3\xBF\xE2\xA1\xBF\xE2\xA0\xBF\xE2\xA0\xBF\xE2\xA2\xBF\xE2\xA3\xBF\xE2\xA3\xBF\xE2\xA3\xBF\xE2\xA0\x89\n"
"\xE2\xA0\x80\xE2\xA3\xBF\xE2\xA3\xBF\xE2\xA3\xBF\xE2\xA3\xBF\xE2\xA3\xBF\xE2\xA3\xBF\xE2\xA3\xBF\xE2\xA3\xBF\xE2\xA3\xBF\xE2\xA3\xB7\xE2\xA1\x84\xE2\xA0\xB8\xE2\xA0\xBF\xE2\xA0\xBF\xE2\xA0\xBF\n"
"\xE2\xA2\xA0\xE2\xA3\xBF\xE2\xA3\xBF\xE2\xA3\xBF\xE2\xA3\xBF\xE2\xA3\xBF\xE2\xA1\xBF\xE2\xA0\x9B\xE2\xA2\xBF\xE2\xA3\xBF\xE2\xA1\xBF\xE2\xA0\x83\n"
"\xE2\xA0\xB8\xE2\xA0\xBF\xE2\xA0\xBF\xE2\xA0\xBF\xE2\xA0\xBF\xE2\xA0\xBF\xE2\xA0\x87\n";

int main() {
    SetConsoleOutputCP(CP_UTF8);
    SetConsoleTitleW(L"Roblox Multi-Instance Unlocker");

    fputs(ASCII_ART, stdout);
    printf("\n===============================================\n");
    printf("   Roblox Multi-Instance Unlocker\n");
    printf("===============================================\n\n");

    if (!EnableDebugPrivilege())
        printf("[!] Warning: SeDebugPrivilege not enabled. "
               "Right-click -> Run as administrator.\n\n");

    printf("  1. Launch Roblox.\n");
    printf("  2. Once it's open, press any key to continue...\n\n");
    (void)_getch();

    printf("[*] Scanning for the Roblox singleton handle...\n\n");

    int found = 0;
    int closed = KillSingleton(found);

    if (closed > 0) {
        printf("===============================================\n");
        printf("   SUCCESS - singleton handle killed.\n");
        printf("   You can now launch multiple Roblox instances.\n");
        printf("===============================================\n\n");
        printf("(You can close this window.)\n");
    } else {
        printf("===============================================\n");
        printf("   Failed: unable to find the Roblox handle.\n");
        printf("   Make sure Roblox is actually running and that\n");
        printf("   you launched this as administrator, then retry.\n");
        printf("===============================================\n\n");
        printf("(You can close this window.)\n");
    }

    HangForever();
    return 0;
}

/*
Copyright 2017-2020 Intel Corporation

Permission is hereby granted, free of charge, to any person obtaining a copy of
this software and associated documentation files (the "Software"), to deal in
the Software without restriction, including without limitation the rights to
use, copy, modify, merge, publish, distribute, sublicense, and/or sell copies
of the Software, and to permit persons to whom the Software is furnished to do
so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
*/

#include "PresentMon.hpp"

namespace {

typedef BOOL(WINAPI *OpenProcessTokenProc)(HANDLE ProcessHandle, DWORD DesiredAccess, PHANDLE TokenHandle);
typedef BOOL(WINAPI *GetTokenInformationProc)(HANDLE TokenHandle, TOKEN_INFORMATION_CLASS TokenInformationClass, LPVOID TokenInformation, DWORD TokenInformationLength, DWORD *ReturnLength);
typedef BOOL(WINAPI *LookupPrivilegeValueAProc)(LPCSTR lpSystemName, LPCSTR lpName, PLUID lpLuid);
typedef BOOL(WINAPI *AdjustTokenPrivilegesProc)(HANDLE TokenHandle, BOOL DisableAllPrivileges, PTOKEN_PRIVILEGES NewState, DWORD BufferLength, PTOKEN_PRIVILEGES PreviousState, PDWORD ReturnLength);

struct Advapi {
    HMODULE HModule;
    OpenProcessTokenProc OpenProcessToken;
    GetTokenInformationProc GetTokenInformation;
    LookupPrivilegeValueAProc LookupPrivilegeValueA;
    AdjustTokenPrivilegesProc AdjustTokenPrivileges;

    Advapi()
        : HModule(NULL)
    {
    }

    ~Advapi()
    {
        if (HModule != NULL) {
            FreeLibrary(HModule);
        }
    }

    bool Load()
    {
        HModule = LoadLibraryA("advapi32.dll");
        if (HModule == NULL) {
            return false;
        }

        OpenProcessToken = (OpenProcessTokenProc) GetProcAddress(HModule, "OpenProcessToken");
        GetTokenInformation = (GetTokenInformationProc) GetProcAddress(HModule, "GetTokenInformation");
        LookupPrivilegeValueA = (LookupPrivilegeValueAProc) GetProcAddress(HModule, "LookupPrivilegeValueA");
        AdjustTokenPrivileges = (AdjustTokenPrivilegesProc) GetProcAddress(HModule, "AdjustTokenPrivileges");

        if (OpenProcessToken == nullptr ||
            GetTokenInformation == nullptr ||
            LookupPrivilegeValueA == nullptr ||
            AdjustTokenPrivileges == nullptr) {
            FreeLibrary(HModule);
            HModule = NULL;
            return false;
        }

        return true;
    }

    bool HasElevatedPrivilege() const
    {
        HANDLE hToken = NULL;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &hToken)) {
            return false;
        }

        /** BEGIN WORKAROUND: struct TOKEN_ELEVATION and enum value TokenElevation
         * are not defined in the vs2003 headers, so we reproduce them here. **/
        enum { WA_TokenElevation = 20 };
        DWORD TokenIsElevated = 0;
        /** END WA **/

        DWORD dwSize = 0;
        if (!GetTokenInformation(hToken, (TOKEN_INFORMATION_CLASS) WA_TokenElevation, &TokenIsElevated, sizeof(TokenIsElevated), &dwSize)) {
            TokenIsElevated = 0;
        }

        CloseHandle(hToken);

        return TokenIsElevated != 0;
    }

    bool EnableDebugPrivilege() const
    {
        HANDLE hToken = NULL;
        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES, &hToken)) {
            return false;
        }

        TOKEN_PRIVILEGES tp = {};
        tp.PrivilegeCount = 1;
        tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;

        bool enabled =
            LookupPrivilegeValueA(NULL, "SeDebugPrivilege", &tp.Privileges[0].Luid) &&
            AdjustTokenPrivileges(hToken, FALSE, &tp, sizeof(TOKEN_PRIVILEGES), nullptr, nullptr) &&
            GetLastError() != ERROR_NOT_ALL_ASSIGNED;

        CloseHandle(hToken);

        return enabled;
    }
};

int RestartAsAdministrator(
    int argc,
    char** argv)
{
    // Get the exe path
    char exe_path[MAX_PATH] = {};
    GetModuleFileNameA(NULL, exe_path, sizeof(exe_path));

    // Combine arguments into single char* and add -dont_restart_as_admin to
    // prevent an endless loop if the escalation fails.
    char* args = nullptr;
    {
        static char const* const extra_args = "-dont_restart_as_admin";
        size_t idx = strlen(extra_args);
        size_t len = idx + 1;
        for (int i = 1; i < argc; ++i) {
            len += strlen(argv[i]) + 2 + 1; // +2 for possible quotes, +1 for space or null
        }

        args = new char [len];
        memcpy(args, extra_args, idx + 1);
        for (int i = 1; i < argc; ++i) {
            auto addQuotes = argv[i][0] != '\"' && strchr(argv[i], ' ') != nullptr;
            auto n = strlen(argv[i]);

            args[idx] = ' ';

            if (addQuotes) {
                args[idx + 1] = '\"';
                memcpy(args + idx + 2, argv[i], n);
                args[idx + n + 2] = '\"';
                args[idx + n + 3] = '\0';
                idx += 2;
            } else {
                memcpy(args + idx + 1, argv[i], n + 1);
            }

            idx += n + 1;
        }
    }

    // Re-run the process with the runas verb
    DWORD code = 2;

    SHELLEXECUTEINFOA info = {};
    info.cbSize       = sizeof(info);
    info.fMask        = SEE_MASK_NOCLOSEPROCESS; // return info.hProcess for explicit wait
    info.lpVerb       = "runas";
    info.lpFile       = exe_path;
    info.lpParameters = args;
    info.nShow        = SW_SHOWDEFAULT;
    auto ok = ShellExecuteExA(&info);
    delete[] args;
    if (ok) {
        WaitForSingleObject(info.hProcess, INFINITE);
        GetExitCodeProcess(info.hProcess, &code);
        CloseHandle(info.hProcess);
    } else {
        fprintf(stderr, "error: failed to elevate privilege ");
        int e = GetLastError();
        switch (e) {
        case ERROR_FILE_NOT_FOUND:    fprintf(stderr, "(file not found).\n"); break;
        case ERROR_PATH_NOT_FOUND:    fprintf(stderr, "(path not found).\n"); break;
        case ERROR_DLL_NOT_FOUND:     fprintf(stderr, "(dll not found).\n"); break;
        case ERROR_ACCESS_DENIED:     fprintf(stderr, "(access denied).\n"); break;
        case ERROR_CANCELLED:         fprintf(stderr, "(cancelled).\n"); break;
        case ERROR_NOT_ENOUGH_MEMORY: fprintf(stderr, "(out of memory).\n"); break;
        case ERROR_SHARING_VIOLATION: fprintf(stderr, "(sharing violation).\n"); break;
        default:                      fprintf(stderr, "(%u).\n", e); break;
        }
    }

    return code;
}

}

// Returning from this function means keep running in this process.
void ElevatePrivilege(int argc, char** argv)
{
    auto const& args = GetCommandLineArgs();

    // If we are processing an ETL file, then we don't need elevated privilege
    if (args.mEtlFileName != nullptr) {
        return;
    }

    // Try to load advapi to check and set required privilege.
    Advapi advapi;
    if (advapi.Load() && advapi.EnableDebugPrivilege()) {
        return;
    }

    // If user requested to run anyway, warn about potential issues.
    if (!args.mTryToElevate) {
        fprintf(stderr,
            "warning: PresentMon requires elevated privilege in order to query processes\n"
            "    started on another account.  Without elevation, these processes can't be\n"
            "    targetted by name and will be listed as '<error>'.\n");
        if (args.mTerminateOnProcExit && args.mTargetPid == 0) {
            fprintf(stderr, "    -terminate_on_proc_exit will also not work.\n");
        }
        return;
    }

    // Try to restart PresentMon with admin privileve
    exit(RestartAsAdministrator(argc, argv));
}


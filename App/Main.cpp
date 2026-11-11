#include "UI.h"
#include <shellapi.h>
#include <fstream>
#include <filesystem>

int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    int argc = 0;
    auto argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv)
        return 1;
    const std::wstring mode = argc > 1 ? argv[1] : L"", path = argc > 2 ? argv[2] : L"";
    LocalFree(argv);
    const bool offline = mode == L"--preview" || mode == L"--render" || mode == L"--self-test";
    if (!mode.empty() && !offline)
        return ERROR_INVALID_PARAMETER;
    try {
        if (!offline) {
            SID_IDENTIFIER_AUTHORITY authority = SECURITY_NT_AUTHORITY;
            PSID admins = nullptr;
            BOOL elevated = FALSE;
            if (AllocateAndInitializeSid(&authority, 2, SECURITY_BUILTIN_DOMAIN_RID,
                                         DOMAIN_ALIAS_RID_ADMINS, 0, 0, 0, 0, 0, 0, &admins)) {
                CheckTokenMembership(nullptr, admins, &elevated);
                FreeSid(admins);
            }
            if (!elevated) {
                wchar_t file[32768]{};
                GetModuleFileNameW(nullptr, file, 32768);
                SHELLEXECUTEINFOW launch{sizeof(launch)};
                launch.lpVerb = L"runas";
                launch.lpFile = file;
                launch.nShow = SW_SHOWNORMAL;
                if (!ShellExecuteExW(&launch))
                    return static_cast<int>(GetLastError());
                return 0;
            }
        }
        HANDLE single =
            offline ? nullptr : CreateMutexW(nullptr, TRUE, L"Local\\TitanCSA.MouseControl.1");
        if (!offline && (!single || GetLastError() == ERROR_ALREADY_EXISTS)) {
            if (single)
                CloseHandle(single);
            MessageBoxW(nullptr, L"MouseControl is already open. Use its existing window.",
                        L"MouseControl", MB_OK | MB_ICONINFORMATION);
            return ERROR_ALREADY_EXISTS;
        }
        struct Mutex {
            HANDLE value;
            ~Mutex() {
                if (value)
                    CloseHandle(value);
            }
        } mutex{single};
        SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
        const auto com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        if (FAILED(com))
            return 1;
        int result = 0;
        {
            MouseControl::Window window(offline);
            if (mode == L"--render") {
                window.Render(path);
            } else if (mode == L"--self-test") {
                window.Create(false);
                const auto checks = window.SelfTest();
                std::ofstream report(std::filesystem::path(path), std::ios::binary);
                report << "{\"passed\":true,\"checks\":" << checks
                       << ",\"driverOpened\":false,\"preferencesWritten\":false}";
                if (!report)
                    result = 1;
            } else {
                window.Create();
                MSG message{};
                while (GetMessageW(&message, nullptr, 0, 0) > 0) {
                    if (!IsDialogMessageW(message.hwnd, &message)) {
                        TranslateMessage(&message);
                        DispatchMessageW(&message);
                    }
                }
            }
        }
        CoUninitialize();
        return result;
    } catch (const std::exception &error) {
        if (mode == L"--self-test") {
            std::ofstream out(std::filesystem::path(path), std::ios::binary);
            out << "{\"passed\":false}";
        }
        OutputDebugStringA(error.what());
        if (!offline)
            MessageBoxA(nullptr, error.what(), "MouseControl", MB_OK | MB_ICONERROR);
        return 1;
    }
}

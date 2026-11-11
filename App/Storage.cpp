#include "Storage.h"
#include <shlobj.h>
#include <shellapi.h>
#include <bcrypt.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <system_error>

namespace MouseControl {
namespace {
struct Handle {
    HANDLE value = INVALID_HANDLE_VALUE;
    ~Handle() {
        if (value != INVALID_HANDLE_VALUE)
            CloseHandle(value);
    }
};
[[noreturn]] void Fail(const char *what) {
    throw std::system_error(GetLastError(), std::system_category(), what);
}
std::wstring Folder() {
    PWSTR raw = nullptr;
    const auto hr = SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &raw);
    if (FAILED(hr))
        throw std::system_error(static_cast<int>(hr), std::system_category(),
                                "Locate app preferences");
    std::wstring folder = raw;
    CoTaskMemFree(raw);
    folder += L"\\MouseControl";
    return folder;
}
void Regular(const std::wstring &path) {
    const DWORD attr = GetFileAttributesW(path.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_REPARSE_POINT))
        throw std::runtime_error("A settings or installer path is redirected; access was refused.");
}
} // namespace
std::wstring Wide(const std::string &s) {
    if (s.empty())
        return {};
    const auto n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(),
                                       static_cast<int>(s.size()), nullptr, 0);
    if (!n)
        Fail("Decode UTF-8");
    std::wstring out(n, 0);
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()),
                        out.data(), n);
    return out;
}
std::string Utf8(const std::wstring &s) {
    if (s.empty())
        return {};
    const auto n = WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(),
                                       static_cast<int>(s.size()), nullptr, 0, nullptr, nullptr);
    if (!n)
        Fail("Encode UTF-8");
    std::string out(n, 0);
    WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, s.data(), static_cast<int>(s.size()),
                        out.data(), n, nullptr, nullptr);
    return out;
}
std::wstring KeyName(Key key) {
    wchar_t name[128]{};
    const auto bits = static_cast<LONG>((key.code << 16) | ((key.extended & 2) ? 1u << 24 : 0));
    if (GetKeyNameTextW(bits, name, 128) > 0)
        return name;
    return L"Scan " + std::to_wstring(key.code);
}
Settings Load(std::wstring &notice) {
    const auto folder = Folder(), path = folder + L"\\preferences.mcontrol";
    Regular(folder);
    Regular(path);
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES)
        return {};
    try {
        std::ifstream in(std::filesystem::path(path), std::ios::binary);
        if (!in)
            throw std::runtime_error("Could not read saved preferences.");
        std::string text(20001, '\0');
        in.read(text.data(), static_cast<std::streamsize>(text.size()));
        text.resize(static_cast<std::size_t>(in.gcount()));
        return Decode(text);
    } catch (const std::exception &e) {
        notice = L"Saved settings were not loaded: " + Wide(e.what());
        return {};
    }
}
void Save(const Settings &settings) {
    const auto folder = Folder();
    Regular(folder);
    if (!CreateDirectoryW(folder.c_str(), nullptr) && GetLastError() != ERROR_ALREADY_EXISTS)
        Fail("Create settings folder");
    const auto path = folder + L"\\preferences.mcontrol";
    Regular(path);
    GUID id{};
    if (FAILED(CoCreateGuid(&id)))
        throw std::runtime_error("Cannot create a unique preferences filename.");
    wchar_t identifier[40]{};
    StringFromGUID2(id, identifier, 40);
    const auto temporary = folder + L"\\preferences-" + identifier + L".tmp";
    const auto text = Encode(settings);
    Handle file;
    file.value = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file.value == INVALID_HANDLE_VALUE)
        Fail("Create temporary preferences");
    DWORD bytes = 0;
    const auto ok =
        WriteFile(file.value, text.data(), static_cast<DWORD>(text.size()), &bytes, nullptr) &&
        bytes == text.size() && FlushFileBuffers(file.value);
    const auto error = GetLastError();
    CloseHandle(file.value);
    file.value = INVALID_HANDLE_VALUE;
    if (!ok) {
        DeleteFileW(temporary.c_str());
        throw std::system_error(error ? error : ERROR_WRITE_FAULT, std::system_category(),
                                "Write preferences");
    }
    if (!MoveFileExW(temporary.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const auto code = GetLastError();
        DeleteFileW(temporary.c_str());
        throw std::system_error(code, std::system_category(), "Commit preferences");
    }
}
void OpenSetup(HWND owner) {
    wchar_t executable[32768]{};
    const auto size = GetModuleFileNameW(nullptr, executable, 32768);
    if (!size || size >= 32768)
        Fail("Find application folder");
    std::wstring path(executable, size);
    path.resize(path.find_last_of(L"\\/") + 1);
    path += L"InterceptorSetup.exe";
    Regular(path);
    Handle file;
    file.value = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file.value == INVALID_HANDLE_VALUE) {
        if (GetLastError() != ERROR_FILE_NOT_FOUND)
            Fail("Open driver setup");
        if (reinterpret_cast<INT_PTR>(ShellExecuteW(
                owner, L"open",
                L"https://github.com/TitanCSA/Interceptor-Keyboard-Mouse/releases/latest", nullptr,
                nullptr, SW_SHOWNORMAL)) <= 32)
            throw std::runtime_error(
                "Open the Interceptor Keyboard & Mouse release page to download setup.");
        return;
    }
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    BYTE digest[32]{};
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
        throw std::runtime_error("Cannot verify driver setup integrity.");
    struct Crypto {
        BCRYPT_ALG_HANDLE a;
        BCRYPT_HASH_HANDLE *h;
        ~Crypto() {
            if (*h)
                BCryptDestroyHash(*h);
            BCryptCloseAlgorithmProvider(a, 0);
        }
    } crypto{algorithm, &hash};
    if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) < 0)
        throw std::runtime_error("Cannot create setup digest.");
    BYTE buffer[32768];
    DWORD bytes = 0;
    std::uint64_t total = 0;
    for (;;) {
        if (!ReadFile(file.value, buffer, sizeof(buffer), &bytes, nullptr))
            Fail("Read driver setup");
        if (!bytes)
            break;
        total += bytes;
        if (total > 67108864 || BCryptHashData(hash, buffer, bytes, 0) < 0)
            throw std::runtime_error("Invalid driver setup payload.");
    }
    if (BCryptFinishHash(hash, digest, sizeof(digest), 0) < 0)
        throw std::runtime_error("Cannot finish setup digest.");
    std::ostringstream hex;
    for (auto value : digest)
        hex << std::hex << std::setw(2) << std::setfill('0') << static_cast<unsigned>(value);
    if (hex.str() != "54bdd2cb3e5319eea2588fec7579721b0ce7b38ab1acfc888c90b48798f2ac83")
        throw std::runtime_error("Adjacent setup does not match Keyboard & Mouse 1.0.5.0. Download "
                                 "the complete MouseControl ZIP again.");
    SHELLEXECUTEINFOW launch{sizeof(launch)};
    launch.fMask = SEE_MASK_NOASYNC;
    launch.hwnd = owner;
    launch.lpVerb = L"open";
    launch.lpFile = path.c_str();
    launch.nShow = SW_SHOWNORMAL;
    if (!ShellExecuteExW(&launch))
        Fail("Launch driver setup");
}
} // namespace MouseControl

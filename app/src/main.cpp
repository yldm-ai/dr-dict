#include "application.h"
#include "dr/text.h"
#include <shellapi.h>
#include <shlobj.h>
namespace
{
// Lets other programs (DR Reader's “查词典”) start DR Dict by name: ShellExecute resolves
// "dr-dict.exe" through App Paths. Per user, so no elevation; follows the copy that ran last.
void register_app_path(const std::wstring &executable)
{
    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\App Paths\\dr-dict.exe", 0,
                        nullptr, 0, KEY_SET_VALUE, nullptr, &key, nullptr) != ERROR_SUCCESS)
        return;
    RegSetValueExW(key, nullptr, 0, REG_SZ, reinterpret_cast<const BYTE *>(executable.c_str()),
                   static_cast<DWORD>((executable.size() + 1) * sizeof(wchar_t)));
    RegCloseKey(key);
}
// The popup host of the instance using this data folder (see popup.h's instance_property).
HWND find_popup(size_t tag)
{
    for (HWND popup = nullptr; (popup = FindWindowExW(nullptr, popup, dr::popup_class, nullptr));)
        if (reinterpret_cast<size_t>(GetPropW(popup, dr::instance_property)) == tag)
            return popup;
    return nullptr;
}
// Hands a request to the running instance's popup host, which may then take the foreground.
bool send_request(HWND popup, ULONG_PTR kind, const std::string &payload)
{
    if (!popup)
        return false;
    DWORD process = 0;
    GetWindowThreadProcessId(popup, &process);
    AllowSetForegroundWindow(process);
    COPYDATASTRUCT data{kind, static_cast<DWORD>(payload.size()), const_cast<char *>(payload.data())};
    DWORD_PTR handled = 0;
    return SendMessageTimeoutW(popup, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&data), SMTO_ABORTIFHUNG, 3000,
                               &handled) &&
           handled;
}
} // namespace
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int)
{
    auto initialized = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(initialized))
        return 1;
    int result = 1;
    try
    {
        PWSTR local = nullptr;
        if (FAILED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &local)))
            throw std::runtime_error("无法找到本地应用数据目录");
        dr::fs::path root = dr::fs::path(local) / "DRDict";
        CoTaskMemFree(local);
        std::wstring executable(32768, L'\0');
        auto size = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
        executable.resize(size);
        int argc = 0;
        LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
        std::vector<dr::fs::path> files;
        std::string shell;
        std::optional<dr::LookupRequest> lookup;
        for (int i = 1; i < argc; ++i)
        {
            std::wstring arg = argv[i];
            if (arg == L"--data-dir" && i + 1 < argc)
                root = argv[++i];
            else if (arg == L"--lookup" && i + 1 < argc)
            {
                // The popup opens beside the pointer, as for Ctrl+C+C.
                lookup = dr::LookupRequest{dr::narrow(argv[++i])};
                GetCursorPos(&lookup->at);
            }
            else if (arg == L"--shell" && i + 1 < argc)
            {
                shell = dr::narrow(argv[++i]);
                if (shell != "mica" && shell != "direct2d")
                    throw std::runtime_error("--shell 仅支持 mica 或 direct2d");
            }
            else if (arg.starts_with(L"--"))
                throw std::runtime_error("未知命令行参数");
            else
                files.emplace_back(arg);
        }
        LocalFree(argv);
        root = dr::fs::absolute(root);
        // One instance per data folder: a second one would keep its own copy of the history and
        // favourites and add a second Ctrl+C+C hook, so popup lookups could land in a main window
        // the user is not looking at. A second start hands its work to the running instance.
        const size_t tag = dr::instance_tag(root);
        wchar_t mutex_name[64];
        swprintf_s(mutex_name, L"Local\\DRDict.Instance.%zx", tag);
        HANDLE instance_mutex = CreateMutexW(nullptr, FALSE, mutex_name);
        const bool running = instance_mutex && GetLastError() == ERROR_ALREADY_EXISTS;
        if (lookup && send_request(find_popup(tag), dr::lookup_request,
                                   dr::Json{{"word", lookup->word}, {"x", lookup->at.x}, {"y", lookup->at.y}}.dump()))
        {
            CloseHandle(instance_mutex);
            CoUninitialize();
            return 0;
        }
        if (running)
        {
            dr::Json paths = dr::Json::array();
            for (const auto &file : files)
                paths.push_back(dr::narrow(dr::fs::absolute(file).wstring()));
            const auto payload = dr::Json{{"files", paths}}.dump();
            // The running instance creates its popup right after its main window; give one that is
            // still starting a moment.
            for (int attempt = 0; attempt < 50 && !send_request(find_popup(tag), dr::open_request, payload); ++attempt)
                Sleep(100);
            CloseHandle(instance_mutex);
            CoUninitialize();
            return 0;
        }
        auto assets = dr::fs::path(executable).parent_path() / "web";
        if (!dr::fs::exists(assets / "index.html"))
            throw std::runtime_error("未找到 web/index.html，请保持 web 目录与程序位于一起");
        register_app_path(executable);
        {
            dr::Application app(dr::fs::absolute(root), assets, std::move(files), shell, std::move(lookup));
            result = app.run(instance);
        }
    }
    catch (const std::exception &e)
    {
        MessageBoxW(nullptr, dr::widen(e.what()).c_str(), L"DR Dict", MB_OK | MB_ICONERROR);
    }
    CoUninitialize();
    return result;
}

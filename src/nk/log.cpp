#include "log.h"

#include <windows.h>
#include <share.h>
#include <stdio.h>
#include <string>
#include <mutex>

namespace nk {
namespace {

std::mutex g_mutex;
FILE* g_file = nullptr;
bool g_enabled = false;
bool g_initialised = false;
std::string g_tag = "nk";
std::wstring g_path;

std::wstring module_dir() {
    // The address of a function in this module, so the same code finds the
    // DLL's directory when linked into the SAPI DLL and the executable's when
    // linked into the host.
    HMODULE mod = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&module_dir), &mod);
    wchar_t buf[MAX_PATH] = {0};
    GetModuleFileNameW(mod, buf, MAX_PATH);
    std::wstring path(buf);
    size_t cut = path.find_last_of(L'\\');
    return cut == std::wstring::npos ? std::wstring() : path.substr(0, cut);
}

// %LOCALAPPDATA%\NokiaKlatt is writable for a per-user install and for a
// machine-wide one under Program Files, which the module directory is not.
std::wstring log_directory() {
    wchar_t* base = nullptr;
    size_t len = 0;
    if (_wdupenv_s(&base, &len, L"LOCALAPPDATA") == 0 && base) {
        std::wstring dir(base);
        free(base);
        dir += L"\\NokiaKlatt";
        CreateDirectoryW(dir.c_str(), nullptr);
        return dir;
    }
    return module_dir();
}

bool env_flag(const wchar_t* name) {
    wchar_t* value = nullptr;
    size_t len = 0;
    if (_wdupenv_s(&value, &len, name) != 0 || !value) return false;
    bool on = value[0] && !(value[0] == L'0' && value[1] == 0);
    free(value);
    return on;
}

void open_locked() {
    if (g_file) return;
    g_path = log_directory() + L"\\nokiaklatt.log";
    // Plain "a", never ccs=UTF-8: the Unicode CRT modes fail-fast inside
    // fprintf and take the whole process with them.
    g_file = _wfsopen(g_path.c_str(), L"a", _SH_DENYNO);
    if (!g_file) {
        g_enabled = false;
        return;
    }
    setvbuf(g_file, nullptr, _IOLBF, 4096);
}

}  // namespace

void log_init(const char* tag) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (tag) g_tag = tag;
    if (g_initialised) return;
    g_initialised = true;

    g_enabled = env_flag(L"NOKIAKLATT_LOG");
    if (!g_enabled) {
        // A log file the user (or the installer) left in place is itself the
        // request: logging can be turned on without setting an environment
        // variable for a process the screen reader starts.
        std::wstring marker = log_directory() + L"\\logging.on";
        g_enabled = GetFileAttributesW(marker.c_str()) != INVALID_FILE_ATTRIBUTES;
    }
    if (!g_enabled) return;

    open_locked();
    if (!g_file) return;

    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(g_file,
            "\n===== %s starting %04d-%02d-%02d %02d:%02d:%02d  pid %lu  "
            "%d-bit =====\n",
            g_tag.c_str(), st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute,
            st.wSecond, GetCurrentProcessId(),
            static_cast<int>(sizeof(void*) * 8));
    fflush(g_file);
}

bool log_enabled() { return g_enabled; }

const wchar_t* log_path() { return g_path.c_str(); }

void log_set_enabled(bool on) {
    std::lock_guard<std::mutex> lock(g_mutex);
    std::wstring marker = log_directory() + L"\\logging.on";
    if (on) {
        HANDLE h = CreateFileW(marker.c_str(), GENERIC_WRITE, FILE_SHARE_READ,
                               nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                               nullptr);
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
        g_enabled = true;
        g_initialised = true;
        open_locked();
    } else {
        DeleteFileW(marker.c_str());
        g_enabled = false;
    }
}

void log_write(const char* file, int line, const char* fmt, ...) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_enabled) return;
    open_locked();
    if (!g_file) return;

    const char* leaf = strrchr(file, '\\');
    leaf = leaf ? leaf + 1 : file;

    SYSTEMTIME st;
    GetLocalTime(&st);
    fprintf(g_file, "%02d:%02d:%02d.%03d %-9s t%-5lu %s:%d  ", st.wHour,
            st.wMinute, st.wSecond, st.wMilliseconds, g_tag.c_str(),
            GetCurrentThreadId(), leaf, line);

    va_list args;
    va_start(args, fmt);
    vfprintf(g_file, fmt, args);
    va_end(args);

    fputc('\n', g_file);
    fflush(g_file);
}

}  // namespace nk

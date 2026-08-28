#include <string>

#include "uc_api.h"
#include "log.h"

namespace nk {
namespace {

UcApi g_api;
bool g_loaded = false;
std::string g_error;

template <typename T>
bool bind(HMODULE mod, T& slot, const char* name) {
    slot = reinterpret_cast<T>(GetProcAddress(mod, name));
    if (!slot) {
        g_error = std::string("unicorn.dll is missing export ") + name;
        return false;
    }
    return true;
}

HMODULE open_library(const wchar_t* hint_dir) {
    if (hint_dir && *hint_dir) {
        std::wstring path(hint_dir);
        if (!path.empty() && path.back() != L'\\') path += L'\\';
        path += L"unicorn.dll";
        // Let the loader resolve unicorn's own dependencies out of the same
        // directory rather than only out of the application directory.
        HMODULE mod = LoadLibraryExW(path.c_str(), nullptr,
                                     LOAD_WITH_ALTERED_SEARCH_PATH);
        if (mod) return mod;
    }
    return LoadLibraryW(L"unicorn.dll");
}

}  // namespace

const UcApi* uc_load(const wchar_t* hint_dir, std::string* error) {
    if (g_loaded) return &g_api;
    if (!g_error.empty()) {
        if (error) *error = g_error;
        return nullptr;
    }

    HMODULE mod = open_library(hint_dir);
    if (!mod) {
        g_error = "could not load unicorn.dll (error " +
                  std::to_string(GetLastError()) + ")";
        if (error) *error = g_error;
        return nullptr;
    }

    bool ok = bind(mod, g_api.open, "uc_open") &&
              bind(mod, g_api.close, "uc_close") &&
              bind(mod, g_api.mem_map, "uc_mem_map") &&
              bind(mod, g_api.mem_map_ptr, "uc_mem_map_ptr") &&
              bind(mod, g_api.mem_unmap, "uc_mem_unmap") &&
              bind(mod, g_api.mem_read, "uc_mem_read") &&
              bind(mod, g_api.mem_write, "uc_mem_write") &&
              bind(mod, g_api.reg_read, "uc_reg_read") &&
              bind(mod, g_api.reg_write, "uc_reg_write") &&
              bind(mod, g_api.emu_start, "uc_emu_start") &&
              bind(mod, g_api.emu_stop, "uc_emu_stop") &&
              bind(mod, g_api.hook_add, "uc_hook_add") &&
              bind(mod, g_api.hook_del, "uc_hook_del") &&
              bind(mod, g_api.strerror, "uc_strerror") &&
              bind(mod, g_api.version, "uc_version");
    if (!ok) {
        if (error) *error = g_error;
        return nullptr;
    }

    unsigned major = 0, minor = 0;
    g_api.version(&major, &minor);
    NK_LOG("unicorn %u.%u loaded", major, minor);

    g_loaded = true;
    return &g_api;
}

}  // namespace nk

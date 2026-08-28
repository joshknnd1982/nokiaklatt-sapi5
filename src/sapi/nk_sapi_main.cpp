// DLL entry points and COM registration for the Nokia Klatt SAPI5 engine.
//
// Built twice, for x86 and x64. Both talk to the same x64 host, so the two
// registrations are of the same voices and an application of either
// architecture sees the same list.

#include <new>

#include <sapi.h>

#include "com.hpp"
#include "nk/log.h"
#include "nk_engine.hpp"
#include "nk_enum.hpp"
#include "nk_voices.hpp"
#include "registry.hpp"

namespace {

HINSTANCE g_dll = nullptr;
NokiaKlatt::com::class_object_factory g_factory;

// SAPI consults token enumerators only under HKLM. One registered under HKCU
// is accepted without complaint and then ignored, which looks exactly like a
// working install with no voices.
const std::wstring kTokenEnumsPath =
    L"Software\\Microsoft\\Speech\\Voices\\TokenEnums";
const std::wstring kEnumName = L"NokiaKlatt";

[[nodiscard]] std::wstring clsid_to_string(const GUID& clsid) {
    wchar_t buf[64];
    StringFromGUID2(clsid, buf, 64);
    return std::wstring(buf);
}

void register_token_enumerator() {
    using namespace NokiaKlatt::sapi;
    using namespace NokiaKlatt::registry;

    const std::wstring clsid =
        clsid_to_string(__uuidof(IEnumSpObjectTokensImpl));

    key enums(HKEY_LOCAL_MACHINE, kTokenEnumsPath,
              KEY_CREATE_SUB_KEY | KEY_SET_VALUE, true);
    key mine(enums, kEnumName, KEY_SET_VALUE, true);
    mine.set(L"Nokia Klatt Voices");
    mine.set(L"CLSID", clsid);
}

void unregister_token_enumerator() noexcept {
    using namespace NokiaKlatt::registry;
    try {
        key enums(HKEY_LOCAL_MACHINE, kTokenEnumsPath, KEY_ALL_ACCESS);
        enums.delete_subkey(kEnumName);
    } catch (...) {
    }
}

}  // namespace

BOOL APIENTRY DllMain(HINSTANCE instance, DWORD reason, LPVOID /*reserved*/) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_dll = instance;
        DisableThreadLibraryCalls(instance);
        nk::log_init(sizeof(void*) == 8 ? "sapi64" : "sapi32");
        try {
            g_factory.register_class<
                NokiaKlatt::sapi::IEnumSpObjectTokensImpl>();
            g_factory.register_class<NokiaKlatt::sapi::ISpTTSEngineImpl>();
        } catch (...) {
            return FALSE;
        }
    } else if (reason == DLL_PROCESS_DETACH) {
        nksapi::shutdown_client();
    }
    return TRUE;
}

STDAPI DllGetClassObject(REFCLSID rclsid, REFIID riid, void** ppv) {
    return g_factory.create(rclsid, riid, ppv);
}

STDAPI DllCanUnloadNow() {
    return NokiaKlatt::com::object_counter::is_zero() ? S_OK : S_FALSE;
}

STDAPI DllRegisterServer() {
    try {
        NokiaKlatt::com::class_registrar r(g_dll);
        r.register_class<NokiaKlatt::sapi::IEnumSpObjectTokensImpl>();
        r.register_class<NokiaKlatt::sapi::ISpTTSEngineImpl>();
        register_token_enumerator();
        NK_LOG("registered");
        return S_OK;
    } catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    } catch (...) {
        return E_UNEXPECTED;
    }
}

STDAPI DllUnregisterServer() {
    try {
        unregister_token_enumerator();
        NokiaKlatt::com::class_registrar r(g_dll);
        r.unregister_class<NokiaKlatt::sapi::IEnumSpObjectTokensImpl>();
        r.unregister_class<NokiaKlatt::sapi::ISpTTSEngineImpl>();
        NK_LOG("unregistered");
        return S_OK;
    } catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    } catch (...) {
        return E_UNEXPECTED;
    }
}

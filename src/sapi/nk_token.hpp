// One SAPI voice token, built from what the host reported.
#pragma once

#include <map>

#include <comdef.h>
#include <comip.h>

#include "ISpDataKeyImpl.hpp"
#include "nk_voices.hpp"
#include "utils.hpp"

namespace NokiaKlatt {
namespace sapi {

class voice_token : public ISpDataKeyImpl {
  public:
    explicit voice_token(const nksapi::VoiceAttributes& attr);

    STDMETHOD(OpenKey)(LPCWSTR pszSubKeyName, ISpDataKey** ppSubKey) override;
    STDMETHOD(EnumKeys)(ULONG Index, LPWSTR* ppszSubKeyName) override;

  private:
    [[nodiscard]] bool str_equal(const std::wstring& a,
                                 const std::wstring& b) const noexcept {
        return _wcsicmp(a.c_str(), b.c_str()) == 0;
    }

    using attribute_map = std::map<std::wstring, std::wstring, str_less>;
    attribute_map attributes_;
};

}  // namespace sapi
}  // namespace NokiaKlatt

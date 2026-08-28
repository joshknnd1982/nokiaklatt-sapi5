// The token enumerator SAPI asks for the Nokia Klatt voices.
#pragma once

#include <vector>

#include <windows.h>
#include <sapi.h>
#include <sapiddk.h>
#include <sperror.h>
#include <comdef.h>
#include <comip.h>

#include "com.hpp"
#include "nk_token.hpp"
#include "nk_voices.hpp"

namespace NokiaKlatt {
namespace sapi {

class __declspec(uuid("6b6d0f2a-4c3e-4f18-9b7d-2a41d5e08c31"))
    IEnumSpObjectTokensImpl : public IEnumSpObjectTokens {
  public:
    explicit IEnumSpObjectTokensImpl(bool initialize = true);

    IEnumSpObjectTokensImpl(const IEnumSpObjectTokensImpl&) = delete;
    IEnumSpObjectTokensImpl& operator=(const IEnumSpObjectTokensImpl&) = delete;

    STDMETHOD(Next)(ULONG celt, ISpObjectToken** pelt,
                    ULONG* pceltFetched) override;
    STDMETHOD(Skip)(ULONG celt) override;
    STDMETHOD(Reset)() override;
    STDMETHOD(Clone)(IEnumSpObjectTokens** ppEnum) override;
    STDMETHOD(Item)(ULONG Index, ISpObjectToken** ppToken) override;
    STDMETHOD(GetCount)(ULONG* pulCount) override;

  protected:
    [[nodiscard]] void* get_interface(REFIID riid) noexcept {
        return com::try_primary_interface<IEnumSpObjectTokens>(this, riid);
    }

  private:
    _COM_SMARTPTR_TYPEDEF(ISpObjectToken, __uuidof(ISpObjectToken));
    _COM_SMARTPTR_TYPEDEF(ISpObjectTokenInit, __uuidof(ISpObjectTokenInit));

    [[nodiscard]] ISpObjectTokenPtr create_token(
        const nksapi::VoiceAttributes& attr) const;

    std::size_t index_ = 0;
    std::vector<nksapi::VoiceAttributes> voices_;
};

}  // namespace sapi
}  // namespace NokiaKlatt

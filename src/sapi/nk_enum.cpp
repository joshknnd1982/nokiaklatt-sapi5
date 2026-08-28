#include "nk_enum.hpp"

#include <algorithm>
#include <new>
#include <stdexcept>

#include "nk/log.h"

namespace NokiaKlatt {
namespace sapi {

IEnumSpObjectTokensImpl::IEnumSpObjectTokensImpl(bool initialize) {
    if (!initialize) return;
    // Re-asking the host each time the enumerator is created is what makes a
    // change in the configuration utility show up without a reboot: an
    // application that reopens its voice list sees the Custom Voice's new
    // language straight away.
    voices_ = nksapi::voices(true);
    NK_LOG("enumerator built with %zu voice(s)", voices_.size());
}

IEnumSpObjectTokensImpl::ISpObjectTokenPtr
IEnumSpObjectTokensImpl::create_token(
    const nksapi::VoiceAttributes& attr) const {
    // A token id has to be a registry-style path, and a voice id contains a
    // colon, so the id is spelled with dashes here. It still has to be stable
    // between releases: this is what an application stores when the user
    // picks a voice.
    std::wstring safe = utils::string_to_wstring(attr.id);
    std::replace(safe.begin(), safe.end(), L':', L'-');

    std::wstring token_id =
        std::wstring(SPCAT_VOICES) + L"\\TokenEnums\\NokiaKlatt\\" + safe;

    com::object<voice_token> obj_data_key(attr);
    com::interface_ptr<ISpDataKey> int_data_key(obj_data_key);

    ISpObjectTokenInitPtr token_init(CLSID_SpObjectToken);
    if (!token_init) throw std::runtime_error("could not create a voice token");

    if (FAILED(token_init->InitFromDataKey(SPCAT_VOICES, token_id.c_str(),
                                           int_data_key.get(false))))
        throw std::runtime_error("could not initialise a voice token");

    ISpObjectTokenPtr token = token_init;
    return token;
}

STDMETHODIMP IEnumSpObjectTokensImpl::Next(ULONG celt, ISpObjectToken** pelt,
                                           ULONG* pceltFetched) {
    if (celt == 0) return E_INVALIDARG;
    if (!pelt) return E_POINTER;
    if (!pceltFetched && celt > 1) return E_POINTER;
    if (pceltFetched) *pceltFetched = 0;

    try {
        std::vector<ISpObjectTokenPtr> tokens;
        tokens.reserve(celt);

        const std::size_t end =
            (std::min)(index_ + static_cast<std::size_t>(celt), voices_.size());
        for (std::size_t i = index_; i < end; ++i)
            tokens.push_back(create_token(voices_[i]));

        for (std::size_t i = 0; i < tokens.size(); ++i) {
            tokens[i].AddRef();
            pelt[i] = tokens[i].GetInterfacePtr();
        }
        if (pceltFetched) *pceltFetched = static_cast<ULONG>(tokens.size());
        index_ += tokens.size();
        return tokens.size() == celt ? S_OK : S_FALSE;
    } catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    } catch (...) {
        return E_UNEXPECTED;
    }
}

STDMETHODIMP IEnumSpObjectTokensImpl::Skip(ULONG celt) {
    const std::size_t remaining = voices_.size() - index_;
    const std::size_t skipped =
        (std::min)(remaining, static_cast<std::size_t>(celt));
    index_ += skipped;
    return skipped == celt ? S_OK : S_FALSE;
}

STDMETHODIMP IEnumSpObjectTokensImpl::Reset() {
    index_ = 0;
    return S_OK;
}

STDMETHODIMP IEnumSpObjectTokensImpl::GetCount(ULONG* pulCount) {
    if (!pulCount) return E_POINTER;
    *pulCount = static_cast<ULONG>(voices_.size());
    return S_OK;
}

STDMETHODIMP IEnumSpObjectTokensImpl::Item(ULONG Index,
                                           ISpObjectToken** ppToken) {
    if (!ppToken) return E_POINTER;
    *ppToken = nullptr;
    if (Index >= voices_.size()) return SPERR_NO_MORE_ITEMS;

    try {
        ISpObjectTokenPtr token = create_token(voices_[Index]);
        token.AddRef();
        *ppToken = token.GetInterfacePtr();
        return S_OK;
    } catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    } catch (...) {
        return E_UNEXPECTED;
    }
}

STDMETHODIMP IEnumSpObjectTokensImpl::Clone(IEnumSpObjectTokens** ppEnum) {
    if (!ppEnum) return E_POINTER;
    *ppEnum = nullptr;

    try {
        com::object<IEnumSpObjectTokensImpl> obj(false);
        obj->voices_ = voices_;
        obj->index_ = index_;
        com::interface_ptr<IEnumSpObjectTokens> ptr(obj);
        *ppEnum = ptr.get();
        return S_OK;
    } catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    } catch (...) {
        return E_UNEXPECTED;
    }
}

}  // namespace sapi
}  // namespace NokiaKlatt

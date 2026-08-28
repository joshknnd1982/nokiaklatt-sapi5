// The SAPI5 text-to-speech engine for Nokia Klatt.
#pragma once

#include <string>

#include <windows.h>
#include <sapi.h>
#include <sapiddk.h>
#include <comdef.h>
#include <comip.h>

#include "com.hpp"
#include "nk_voices.hpp"

namespace NokiaKlatt {
namespace sapi {

class __declspec(uuid("2f5b8c14-7d69-4a3f-8e02-9c7b1f4a6d58")) ISpTTSEngineImpl
    : public ISpTTSEngine,
      public ISpObjectWithToken {
  public:
    ISpTTSEngineImpl();
    ~ISpTTSEngineImpl();

    ISpTTSEngineImpl(const ISpTTSEngineImpl&) = delete;
    ISpTTSEngineImpl& operator=(const ISpTTSEngineImpl&) = delete;

    STDMETHOD(Speak)
    (DWORD dwSpeakFlags, REFGUID rguidFormatId,
     const WAVEFORMATEX* pWaveFormatEx, const SPVTEXTFRAG* pTextFragList,
     ISpTTSEngineSite* pOutputSite) override;

    STDMETHOD(GetOutputFormat)
    (const GUID* pTargetFmtId, const WAVEFORMATEX* pTargetWaveFormatEx,
     GUID* pOutputFormatId,
     WAVEFORMATEX** ppCoMemOutputWaveFormatEx) override;

    STDMETHOD(SetObjectToken)(ISpObjectToken* pToken) override;
    STDMETHOD(GetObjectToken)(ISpObjectToken** ppToken) override;

  protected:
    [[nodiscard]] void* get_interface(REFIID riid) noexcept {
        void* ptr = com::try_primary_interface<ISpTTSEngine>(this, riid);
        return ptr ? ptr : com::try_interface<ISpObjectWithToken>(this, riid);
    }

  private:
    _COM_SMARTPTR_TYPEDEF(ISpObjectToken, __uuidof(ISpObjectToken));
    _COM_SMARTPTR_TYPEDEF(ISpDataKey, __uuidof(ISpDataKey));

    // One text fragment, spoken with the events its characters map to.
    // `stream_offset` is the position in this Speak call's output where the
    // fragment starts, and is advanced past the audio it produced; every event
    // offset is measured against it. `spell` reads the text a character at a
    // time.
    HRESULT speak_fragment(const SPVTEXTFRAG* frag, ISpTTSEngineSite* site,
                           ULONGLONG* stream_offset, bool spell);

    ISpObjectTokenPtr token_;
    std::string voice_id_;
};

}  // namespace sapi
}  // namespace NokiaKlatt

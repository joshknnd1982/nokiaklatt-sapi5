// The voice list as SAPI sees it.
//
// The voices are not registered one key at a time: there are 139 of them and
// the Custom Voice changes what it points at whenever the user saves the
// configuration utility. Instead one token enumerator is registered under
// HKLM\Software\Microsoft\Speech\Voices\TokenEnums and hands SAPI a token per
// voice, built on demand from what the host reports.
//
// (HKLM specifically: an enumerator registered under HKCU is accepted without
// complaint and then never consulted.)
#pragma once

#include <windows.h>

#include <mutex>
#include <string>
#include <vector>

#include "nk/client.h"

namespace nksapi {

// The process-wide connection to the host, shared by every voice token and
// engine instance in this application.
nk::Client& client();
void shutdown_client();

struct VoiceAttributes {
    std::string id;
    std::wstring name;      // what the user sees and what SAPI keys on
    std::wstring language;  // hex LCID, e.g. "809"
    std::wstring gender;    // "Male" or "Female"
    std::wstring age;       // "Adult"
    bool custom = false;

    VoiceAttributes() = default;
    explicit VoiceAttributes(const nk::ClientVoice& voice);
};

// The current voice list, cached per process. `force` re-asks the host, which
// the configuration utility's changes make worth doing.
const std::vector<VoiceAttributes>& voices(bool force = false);

// The voice a SAPI token names, by its stable id, then by display name.
const VoiceAttributes* find_voice(const std::string& id,
                                  const std::wstring& name);

}  // namespace nksapi

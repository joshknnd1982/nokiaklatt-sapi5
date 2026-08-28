#include "nk_voices.hpp"

#include <stdio.h>

#include "nk/log.h"

namespace nksapi {
namespace {

std::mutex g_mutex;
nk::Client* g_client = nullptr;
std::vector<VoiceAttributes> g_voices;
bool g_have_voices = false;

}  // namespace

nk::Client& client() {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_client) g_client = new nk::Client();
    return *g_client;
}

void shutdown_client() {
    std::lock_guard<std::mutex> lock(g_mutex);
    delete g_client;
    g_client = nullptr;
    g_voices.clear();
    g_have_voices = false;
}

VoiceAttributes::VoiceAttributes(const nk::ClientVoice& voice)
    : id(voice.id),
      name(voice.label),
      gender(voice.female ? L"Female" : L"Male"),
      age(L"Adult"),
      custom(voice.custom) {
    wchar_t buf[16];
    // SAPI wants the LCID in hex, without a leading zero.
    swprintf_s(buf, L"%x", voice.lcid);
    language = buf;
}

const std::vector<VoiceAttributes>& voices(bool force) {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_have_voices && !force) return g_voices;

    if (!g_client) g_client = new nk::Client();
    std::vector<nk::ClientVoice> raw;
    if (g_client->get_voices(&raw)) {
        g_voices.clear();
        g_voices.reserve(raw.size());
        for (const auto& v : raw) g_voices.emplace_back(v);
        g_have_voices = true;
        NK_LOG("voice list: %zu voice(s)", g_voices.size());
    } else {
        NK_LOG("could not get the voice list: %s",
               g_client->last_error().c_str());
        // Leave whatever was cached: an application that has already
        // enumerated should not watch its voices disappear because the host
        // was restarting.
    }
    return g_voices;
}

const VoiceAttributes* find_voice(const std::string& id,
                                  const std::wstring& name) {
    const auto& list = voices();
    if (!id.empty()) {
        for (const auto& v : list)
            if (v.id == id) return &v;
    }
    if (!name.empty()) {
        for (const auto& v : list)
            if (_wcsicmp(v.name.c_str(), name.c_str()) == 0) return &v;
    }
    return nullptr;
}

}  // namespace nksapi

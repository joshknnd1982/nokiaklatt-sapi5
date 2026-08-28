#include "nk_engine.hpp"

#include <algorithm>
#include <new>
#include <vector>

#include "nk/log.h"
#include "nk/protocol.h"
#include "utils.hpp"

namespace NokiaKlatt {
namespace sapi {
namespace {

// The engine has one sample rate and ignores every request to change it, so
// this is what SAPI is offered whatever it asks for.
constexpr WORD kChannels = nk::kHostChannels;
constexpr DWORD kSampleRate = nk::kHostSampleRate;
constexpr WORD kBitsPerSample = nk::kHostBitsPerSample;
constexpr DWORD kBytesPerSecond = kSampleRate * kChannels * kBitsPerSample / 8;

// What SAPI is told the engine is doing, so a screen reader can follow along.
struct SpeakState {
    ISpTTSEngineSite* site = nullptr;
    const SPVTEXTFRAG* frag = nullptr;
    ULONGLONG written = 0;      // bytes handed to SAPI for this fragment
    ULONG events = 0;           // which boundary events the caller wants
    bool aborted = false;

    // Word boundaries, as (character offset, length) into the fragment, and
    // where in the output each one is expected to fall. The engine reports
    // nothing about its own progress, so the positions come from the text.
    struct Mark {
        ULONG offset;
        ULONG length;
        ULONGLONG at_byte;
    };
    std::vector<Mark> marks;
    size_t next_mark = 0;
};

bool wants(ULONGLONG interest, int event) {
    return (interest & (1ULL << event)) != 0;
}

// A bookmark is not speech.
//
// A caller marks its place in an utterance with <bookmark mark="127"/>, and
// SAPI hands that to the engine as an ordinary text fragment whose text is
// "127" - distinguished from real words only by State.eAction. An engine that
// speaks every fragment reads the number out loud, which is why NVDA appeared
// to be counting: "Hello, 127, This is Josh, 128". The number has to become an
// event instead.
void fire_bookmark(const SPVTEXTFRAG* frag, ISpTTSEngineSite* site,
                   ULONGLONG stream_offset) {
    ULONGLONG interest = 0;
    site->GetEventInterest(&interest);
    if (!wants(interest, SPEI_TTS_BOOKMARK)) return;

    std::wstring mark;
    if (frag->pTextStart && frag->ulTextLen)
        mark.assign(frag->pTextStart, frag->ulTextLen);

    // SAPI takes ownership of a string lParam and releases it with
    // CoTaskMemFree (that is what SpClearEvent does), so it has to be
    // allocated that way and handed over rather than pointed at the
    // fragment, whose text is not NUL-terminated at the fragment boundary.
    size_t bytes = (mark.size() + 1) * sizeof(wchar_t);
    auto* copy = static_cast<wchar_t*>(CoTaskMemAlloc(bytes));
    if (!copy) return;
    memcpy(copy, mark.c_str(), bytes);

    SPEVENT event = {};
    event.eEventId = SPEI_TTS_BOOKMARK;
    event.elParamType = SPET_LPARAM_IS_STRING;
    event.ullAudioStreamOffset = stream_offset;
    event.lParam = reinterpret_cast<LPARAM>(copy);
    event.wParam = static_cast<WPARAM>(_wtol(copy));

    NK_LOG("bookmark \"%ls\" at stream offset %llu", copy, stream_offset);
    if (FAILED(site->AddEvents(&event, 1))) CoTaskMemFree(copy);
}

// <silence msec="N"/>, which is how a caller asks for a pause between
// phrases. The engine has no way to be told to pause, so the silence is
// written straight to the output.
void emit_silence(ULONG msecs, ISpTTSEngineSite* site,
                  ULONGLONG* stream_offset) {
    if (!msecs) return;
    size_t samples = static_cast<size_t>(kSampleRate) * msecs / 1000;
    std::vector<uint8_t> quiet(samples * 2, 0);
    ULONG written = 0;
    if (SUCCEEDED(site->Write(quiet.data(),
                              static_cast<ULONG>(quiet.size()), &written)))
        *stream_offset += quiet.size();
}

// <spell>, which asks for the text a character at a time. The Klatt engine has
// no spelling mode, so the characters are separated instead and its own text
// processor reads them individually.
std::wstring spell_out(const std::wstring& text) {
    std::wstring out;
    out.reserve(text.size() * 2);
    for (wchar_t c : text) {
        if (!out.empty()) out.push_back(L' ');
        out.push_back(c);
    }
    return out;
}

// Where each word starts, and roughly when it will be heard.
//
// A Klatt formant synthesiser says a character in about the same time
// wherever it is, so a linear map from character offset to byte offset places
// word boundaries closely enough for a screen reader to track a line - and
// it is the only estimate available, because the engine reports no timing at
// all.
std::vector<SpeakState::Mark> plan_word_marks(const wchar_t* text, ULONG length,
                                              ULONGLONG expected_bytes) {
    std::vector<SpeakState::Mark> marks;
    if (!text || !length || !expected_bytes) return marks;

    ULONG i = 0;
    while (i < length) {
        while (i < length && iswspace(text[i])) ++i;
        if (i >= length) break;
        ULONG start = i;
        while (i < length && !iswspace(text[i])) ++i;
        SpeakState::Mark mark;
        mark.offset = start;
        mark.length = i - start;
        mark.at_byte = static_cast<ULONGLONG>(
            expected_bytes * (static_cast<double>(start) / length));
        mark.at_byte &= ~1ull;  // sample-aligned
        marks.push_back(mark);
    }
    return marks;
}

void fire_marks(SpeakState& state) {
    while (state.next_mark < state.marks.size() &&
           state.marks[state.next_mark].at_byte <= state.written) {
        const auto& mark = state.marks[state.next_mark++];
        SPEVENT event = {};
        event.eEventId = SPEI_WORD_BOUNDARY;
        event.elParamType = SPET_LPARAM_IS_UNDEFINED;
        event.ullAudioStreamOffset = state.written;
        event.lParam = static_cast<LPARAM>(state.frag->ulTextSrcOffset +
                                           mark.offset);
        event.wParam = static_cast<WPARAM>(mark.length);
        state.site->AddEvents(&event, 1);
    }
}

}  // namespace

ISpTTSEngineImpl::ISpTTSEngineImpl() = default;
ISpTTSEngineImpl::~ISpTTSEngineImpl() = default;

STDMETHODIMP ISpTTSEngineImpl::SetObjectToken(ISpObjectToken* pToken) {
    if (!pToken) return E_INVALIDARG;

    try {
        ISpDataKeyPtr attributes;
        if (FAILED(pToken->OpenKey(L"Attributes", &attributes))) {
            NK_LOG("SetObjectToken: the token has no Attributes key");
            return E_INVALIDARG;
        }

        // Prefer the engine's own id: it survives a relabelled voice, and it
        // is how the Custom Voice - whose display name never changes - is
        // told apart from the rest.
        std::string id;
        utils::out_ptr<wchar_t> value(CoTaskMemFree);
        if (SUCCEEDED(attributes->GetStringValue(L"NokiaVoiceId",
                                                 value.address()))) {
            id = utils::wstring_to_string(value.get());
        }

        std::wstring name;
        utils::out_ptr<wchar_t> name_value(CoTaskMemFree);
        if (SUCCEEDED(attributes->GetStringValue(L"Name",
                                                 name_value.address()))) {
            name = name_value.get();
        }

        const nksapi::VoiceAttributes* voice = nksapi::find_voice(id, name);
        if (!voice) {
            // A token stored by an earlier release, or a host that is not
            // answering. Falling back to the id as written keeps the engine
            // usable: the host resolves it, and reports a clean error if it
            // cannot.
            NK_LOG("SetObjectToken: voice \"%ls\" is not in the list; using "
                   "id \"%s\" as given", name.c_str(), id.c_str());
            voice_id_ = id.empty() ? nk::kCustomVoiceId : id;
        } else {
            voice_id_ = voice->id;
        }

        token_ = pToken;
        NK_LOG("SetObjectToken: voice %s (%ls)", voice_id_.c_str(),
               name.c_str());
        return S_OK;
    } catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    } catch (...) {
        return E_UNEXPECTED;
    }
}

STDMETHODIMP ISpTTSEngineImpl::GetObjectToken(ISpObjectToken** ppToken) {
    if (!ppToken) return E_POINTER;
    *ppToken = nullptr;
    if (!token_) return E_UNEXPECTED;
    token_.AddRef();
    *ppToken = token_.GetInterfacePtr();
    return S_OK;
}

STDMETHODIMP ISpTTSEngineImpl::GetOutputFormat(
    const GUID* /*pTargetFmtId*/, const WAVEFORMATEX* /*pTargetWaveFormatEx*/,
    GUID* pOutputFormatId, WAVEFORMATEX** ppCoMemOutputWaveFormatEx) {
    if (!pOutputFormatId || !ppCoMemOutputWaveFormatEx) return E_POINTER;

    *pOutputFormatId = SPDFID_WaveFormatEx;
    *ppCoMemOutputWaveFormatEx = nullptr;

    auto* format =
        static_cast<WAVEFORMATEX*>(CoTaskMemAlloc(sizeof(WAVEFORMATEX)));
    if (!format) return E_OUTOFMEMORY;

    format->wFormatTag = WAVE_FORMAT_PCM;
    format->nChannels = kChannels;
    format->nSamplesPerSec = kSampleRate;
    format->wBitsPerSample = kBitsPerSample;
    format->nBlockAlign = format->nChannels * format->wBitsPerSample / 8;
    format->nAvgBytesPerSec = format->nSamplesPerSec * format->nBlockAlign;
    format->cbSize = 0;

    *ppCoMemOutputWaveFormatEx = format;
    return S_OK;
}

HRESULT ISpTTSEngineImpl::speak_fragment(const SPVTEXTFRAG* frag,
                                         ISpTTSEngineSite* site,
                                         ULONGLONG* stream_offset,
                                         bool spell) {
    if (!frag->pTextStart || frag->ulTextLen == 0) return S_OK;

    std::wstring text(frag->pTextStart, frag->ulTextLen);
    // Whitespace-only fragments are not an utterance; the engine treats them
    // as an error and there is nothing to say.
    if (text.find_first_not_of(L" \t\r\n") == std::wstring::npos) return S_OK;
    if (spell) text = spell_out(text);

    long rate = 0;
    site->GetRate(&rate);
    USHORT volume = 100;
    site->GetVolume(&volume);

    // SAPI carries pitch as an XML state on the fragment, in half-semitones
    // relative to the voice's default.
    long pitch = frag->State.PitchAdj.MiddleAdj;

    ULONGLONG interest = 0;
    site->GetEventInterest(&interest);

    SpeakState state;
    state.site = site;
    state.frag = frag;
    // Event offsets are positions in the whole output stream for this Speak
    // call, not in this fragment. Starting each fragment back at zero would
    // report every boundary as though it were in the first phrase.
    state.written = *stream_offset;

    // Roughly how long this fragment will be, for placing word boundaries.
    // A Klatt voice runs at about twenty characters a second at normal speed.
    const double chars_per_second = 20.0;
    double seconds = frag->ulTextLen / chars_per_second;
    ULONGLONG expected =
        static_cast<ULONGLONG>(seconds * kBytesPerSecond) & ~1ull;
    if (wants(interest, SPEI_WORD_BOUNDARY)) {
        state.marks = plan_word_marks(frag->pTextStart, frag->ulTextLen,
                                      expected);
        for (auto& mark : state.marks) mark.at_byte += *stream_offset;
    }

    if (wants(interest, SPEI_SENTENCE_BOUNDARY)) {
        SPEVENT event = {};
        event.eEventId = SPEI_SENTENCE_BOUNDARY;
        event.elParamType = SPET_LPARAM_IS_UNDEFINED;
        event.ullAudioStreamOffset = *stream_offset;
        event.lParam = static_cast<LPARAM>(frag->ulTextSrcOffset);
        event.wParam = static_cast<WPARAM>(frag->ulTextLen);
        site->AddEvents(&event, 1);
    }

    std::string error;
    bool ok = nksapi::client().speak(
        voice_id_, text, rate, pitch, volume,
        [&state](const uint8_t* data, size_t size) {
            const DWORD actions = state.site->GetActions();
            if (actions & SPVES_ABORT) {
                state.aborted = true;
                return false;
            }
            if (actions & SPVES_SKIP) {
                state.site->CompleteSkip(0);
                state.aborted = true;
                return false;
            }

            // ISpTTSEngineSite::Write consumes the whole buffer when it
            // succeeds, and several SAPI implementations never touch
            // pcbWritten. Advancing by what it reports therefore re-sends or
            // drops audio depending on the garbage left in that variable,
            // which comes out as speech that stops mid-word. The HRESULT is
            // the only trustworthy part of the answer.
            ULONG written = 0;
            if (FAILED(state.site->Write(data, static_cast<ULONG>(size),
                                         &written))) {
                state.aborted = true;
                return false;
            }
            state.written += size;
            fire_marks(state);
            return true;
        },
        &error);

    if (!ok && !state.aborted && !error.empty()) {
        NK_LOG("fragment failed: %s", error.c_str());
        // A voice that cannot speak this text is not a reason to fail the
        // whole utterance: the next fragment may be fine, and going silent is
        // the worst thing a speech engine can do to someone who depends on it.
    }

    // Anything the estimate overshot still has to be reported, or a caller
    // waiting on a word boundary waits for a position that never arrives.
    const ULONGLONG produced = state.written;
    if (!state.aborted) {
        state.written = ULLONG_MAX;
        fire_marks(state);
    }
    // Where the next fragment - and the bookmark that follows it - begins.
    *stream_offset = produced;
    return state.aborted ? S_FALSE : S_OK;
}

STDMETHODIMP ISpTTSEngineImpl::Speak(DWORD /*dwSpeakFlags*/,
                                     REFGUID /*rguidFormatId*/,
                                     const WAVEFORMATEX* /*pWaveFormatEx*/,
                                     const SPVTEXTFRAG* pTextFragList,
                                     ISpTTSEngineSite* pOutputSite) {
    if (!pTextFragList || !pOutputSite) return E_INVALIDARG;

    try {
        // How much audio this call has produced so far, which is what every
        // event's offset is measured against.
        ULONGLONG stream_offset = 0;

        for (const SPVTEXTFRAG* frag = pTextFragList; frag;
             frag = frag->pNext) {
            if (pOutputSite->GetActions() & SPVES_ABORT) break;

            // Not every fragment is speech. SAPI puts bookmarks, silences and
            // spelling requests through the same list, distinguished only by
            // this field; speaking all of them reads the caller's own markup
            // out loud.
            switch (frag->State.eAction) {
                case SPVA_Speak:
                case SPVA_Pronounce:
                    if (speak_fragment(frag, pOutputSite, &stream_offset,
                                       false) == S_FALSE)
                        return S_OK;
                    break;

                case SPVA_SpellOut:
                    if (speak_fragment(frag, pOutputSite, &stream_offset,
                                       true) == S_FALSE)
                        return S_OK;
                    break;

                case SPVA_Silence:
                    emit_silence(frag->State.SilenceMSecs, pOutputSite,
                                 &stream_offset);
                    break;

                case SPVA_Bookmark:
                    fire_bookmark(frag, pOutputSite, stream_offset);
                    break;

                case SPVA_Section:
                case SPVA_ParseUnknownTag:
                default:
                    // Structure and markup the engine has nothing to say
                    // about.
                    break;
            }
        }
        return S_OK;
    } catch (const std::bad_alloc&) {
        return E_OUTOFMEMORY;
    } catch (...) {
        NK_LOG("Speak: unexpected exception");
        return E_UNEXPECTED;
    }
}

}  // namespace sapi
}  // namespace NokiaKlatt

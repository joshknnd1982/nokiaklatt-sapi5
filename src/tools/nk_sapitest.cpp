// Drive NokiaKlattSAPI.dll the way SAPI does, without registering it.
//
// Registration needs administrator rights, and the interesting failures are
// not in the registry: they are in the enumerator, the token, and what the
// engine does with an ISpTTSEngineSite. This loads the DLL, asks it for its
// class objects directly, and speaks through a site of its own - so the same
// test runs against both the 32-bit and 64-bit builds.
//
//   nk_sapitest <path-to-NokiaKlattSAPI.dll> [out.wav] [text] [rate] [pitch]

#include <windows.h>
#include <sapi.h>
#include <sapiddk.h>
#include <sperror.h>
#include <stdio.h>

#include <string>
#include <vector>

namespace {

// The smallest ISpTTSEngineSite that will do: it collects the PCM, counts the
// events, and never asks the engine to stop.
class TestSite : public ISpTTSEngineSite {
  public:
    std::vector<uint8_t> pcm;
    ULONG word_events = 0;
    ULONG sentence_events = 0;
    long rate = 0;
    USHORT volume = 100;

    struct Bookmark {
        long id;
        std::wstring text;
        ULONGLONG offset;
    };
    std::vector<Bookmark> bookmarks;

    void reset() {
        pcm.clear();
        word_events = 0;
        sentence_events = 0;
        bookmarks.clear();
    }

    // IUnknown
    STDMETHOD(QueryInterface)(REFIID riid, void** ppv) override {
        if (!ppv) return E_POINTER;
        if (riid == IID_IUnknown || riid == __uuidof(ISpTTSEngineSite) ||
            riid == __uuidof(ISpEventSink)) {
            *ppv = static_cast<ISpTTSEngineSite*>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    STDMETHOD_(ULONG, AddRef)() override { return 2; }
    STDMETHOD_(ULONG, Release)() override { return 1; }

    // ISpEventSink
    STDMETHOD(AddEvents)(const SPEVENT* events, ULONG count) override {
        for (ULONG i = 0; i < count; ++i) {
            const SPEVENT& e = events[i];
            if (e.eEventId == SPEI_WORD_BOUNDARY) ++word_events;
            if (e.eEventId == SPEI_SENTENCE_BOUNDARY) ++sentence_events;
            if (e.eEventId == SPEI_TTS_BOOKMARK) {
                Bookmark mark;
                mark.id = static_cast<long>(e.wParam);
                mark.offset = e.ullAudioStreamOffset;
                if (e.elParamType == SPET_LPARAM_IS_STRING && e.lParam)
                    mark.text = reinterpret_cast<const wchar_t*>(e.lParam);
                bookmarks.push_back(mark);
                // SAPI owns a string lParam once it has been added, and frees
                // it with CoTaskMemFree. Standing in for SAPI means doing the
                // same, or every bookmark leaks.
                if (e.elParamType == SPET_LPARAM_IS_STRING && e.lParam)
                    CoTaskMemFree(reinterpret_cast<void*>(e.lParam));
            }
        }
        return S_OK;
    }
    STDMETHOD(GetEventInterest)(ULONGLONG* interest) override {
        *interest = (1ULL << SPEI_WORD_BOUNDARY) |
                    (1ULL << SPEI_SENTENCE_BOUNDARY) |
                    (1ULL << SPEI_TTS_BOOKMARK);
        return S_OK;
    }

    // ISpTTSEngineSite
    STDMETHOD_(DWORD, GetActions)() override { return SPVES_CONTINUE; }
    STDMETHOD(Write)(const void* data, ULONG count, ULONG* written) override {
        const auto* p = static_cast<const uint8_t*>(data);
        pcm.insert(pcm.end(), p, p + count);
        // Deliberately left alone: a real SAPI site often does the same, and
        // an engine that trusts this value truncates its own speech.
        if (written) *written = 0;
        return S_OK;
    }
    STDMETHOD(GetRate)(long* adjust) override {
        *adjust = rate;
        return S_OK;
    }
    STDMETHOD(GetVolume)(USHORT* level) override {
        *level = volume;
        return S_OK;
    }
    STDMETHOD(GetSkipInfo)(SPVSKIPTYPE* type, long* count) override {
        *type = SPVST_SENTENCE;
        *count = 0;
        return S_OK;
    }
    STDMETHOD(CompleteSkip)(long) override { return S_OK; }
};

void write_wav(const std::wstring& path, const std::vector<uint8_t>& pcm) {
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return;
    uint32_t data_size = static_cast<uint32_t>(pcm.size());
    uint32_t riff = 36 + data_size, rate = 16000, byte_rate = 32000,
             fmt_size = 16;
    uint16_t fmt = 1, channels = 1, align = 2, bits = 16;
    fwrite("RIFF", 1, 4, f);
    fwrite(&riff, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f);
    fwrite(&fmt_size, 4, 1, f);
    fwrite(&fmt, 2, 1, f);
    fwrite(&channels, 2, 1, f);
    fwrite(&rate, 4, 1, f);
    fwrite(&byte_rate, 4, 1, f);
    fwrite(&align, 2, 1, f);
    fwrite(&bits, 2, 1, f);
    fwrite("data", 1, 4, f);
    fwrite(&data_size, 4, 1, f);
    if (data_size) fwrite(pcm.data(), 1, data_size, f);
    fclose(f);
}

typedef HRESULT(STDAPICALLTYPE* GetClassObjectFn)(REFCLSID, REFIID, void**);

// The engine's own CLSIDs, which have to match the ones in the DLL.
const CLSID kEnumClsid = {0x6b6d0f2a, 0x4c3e, 0x4f18,
                          {0x9b, 0x7d, 0x2a, 0x41, 0xd5, 0xe0, 0x8c, 0x31}};
const CLSID kEngineClsid = {0x2f5b8c14, 0x7d69, 0x4a3f,
                            {0x8e, 0x02, 0x9c, 0x7b, 0x1f, 0x4a, 0x6d, 0x58}};

template <typename T>
HRESULT create(GetClassObjectFn get_class, REFCLSID clsid, T** out) {
    IClassFactory* factory = nullptr;
    HRESULT hr = get_class(clsid, IID_IClassFactory,
                           reinterpret_cast<void**>(&factory));
    if (FAILED(hr)) return hr;
    hr = factory->CreateInstance(nullptr, __uuidof(T),
                                 reinterpret_cast<void**>(out));
    factory->Release();
    return hr;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) {
        fwprintf(stderr,
                 L"usage: nk_sapitest <NokiaKlattSAPI.dll> [out.wav] [text] "
                 L"[rate] [pitch]\n");
        return 2;
    }
    std::wstring dll_path = argv[1];
    std::wstring out = argc > 2 ? argv[2] : L"sapitest.wav";
    std::wstring text =
        argc > 3 ? argv[3] : L"The quick brown fox jumps over the lazy dog.";
    long rate = argc > 4 ? _wtol(argv[4]) : 0;
    long pitch = argc > 5 ? _wtol(argv[5]) : 0;

    printf("nk_sapitest, %d-bit\n", static_cast<int>(sizeof(void*) * 8));

    HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    if (FAILED(hr)) {
        printf("CoInitializeEx failed: 0x%08lx\n", hr);
        return 1;
    }

    HMODULE dll = LoadLibraryW(dll_path.c_str());
    if (!dll) {
        printf("could not load %ls (error %lu)\n", dll_path.c_str(),
               GetLastError());
        return 1;
    }
    auto get_class = reinterpret_cast<GetClassObjectFn>(
        GetProcAddress(dll, "DllGetClassObject"));
    if (!get_class) {
        printf("the DLL has no DllGetClassObject\n");
        return 1;
    }

    // ---- the voice list ------------------------------------------------
    IEnumSpObjectTokens* tokens = nullptr;
    hr = create(get_class, kEnumClsid, &tokens);
    if (FAILED(hr)) {
        printf("could not create the token enumerator: 0x%08lx\n", hr);
        return 1;
    }
    ULONG count = 0;
    tokens->GetCount(&count);
    printf("voice list: %lu voice(s)\n", count);
    if (count == 0) {
        printf("FAIL: no voices\n");
        return 1;
    }

    // Report a few, and pick British English male if it is there. The name
    // comes from the token's own Attributes key rather than from
    // SpGetDescription, which lives in sphelper.h and drags in more of the
    // SDK than this needs.
    ULONG chosen = 0;
    for (ULONG i = 0; i < count; ++i) {
        ISpObjectToken* item = nullptr;
        if (FAILED(tokens->Item(i, &item))) continue;

        ISpDataKey* attributes = nullptr;
        if (SUCCEEDED(item->OpenKey(L"Attributes", &attributes))) {
            LPWSTR name = nullptr;
            if (SUCCEEDED(attributes->GetStringValue(L"Name", &name)) && name) {
                if (i < 3 || i + 1 == count) printf("  [%lu] %ls\n", i, name);
                if (wcsstr(name, L"English (UK) male") && chosen == 0)
                    chosen = i;
                CoTaskMemFree(name);
            }
            attributes->Release();
        }
        item->Release();
    }
    printf("speaking with voice %lu\n", chosen);

    ISpObjectToken* token = nullptr;
    hr = tokens->Item(chosen, &token);
    tokens->Release();
    if (FAILED(hr)) {
        printf("could not fetch the token: 0x%08lx\n", hr);
        return 1;
    }

    // ---- the engine ----------------------------------------------------
    ISpTTSEngine* engine = nullptr;
    hr = create(get_class, kEngineClsid, &engine);
    if (FAILED(hr)) {
        printf("could not create the engine: 0x%08lx\n", hr);
        return 1;
    }

    ISpObjectWithToken* with_token = nullptr;
    hr = engine->QueryInterface(__uuidof(ISpObjectWithToken),
                                reinterpret_cast<void**>(&with_token));
    if (FAILED(hr)) {
        printf("the engine does not support ISpObjectWithToken: 0x%08lx\n", hr);
        return 1;
    }
    hr = with_token->SetObjectToken(token);
    with_token->Release();
    if (FAILED(hr)) {
        printf("SetObjectToken failed: 0x%08lx\n", hr);
        return 1;
    }

    GUID format_id = {};
    WAVEFORMATEX* format = nullptr;
    hr = engine->GetOutputFormat(nullptr, nullptr, &format_id, &format);
    if (FAILED(hr) || !format) {
        printf("GetOutputFormat failed: 0x%08lx\n", hr);
        return 1;
    }
    printf("output format: %u Hz, %u-bit, %u channel(s)\n",
           format->nSamplesPerSec, format->wBitsPerSample, format->nChannels);

    // A fragment as SAPI builds it: everything but the action is the same
    // whether the fragment is speech or a bookmark.
    auto make_frag = [&](SPVACTIONS action, const wchar_t* body, ULONG len,
                         ULONG src_offset) {
        SPVTEXTFRAG f = {};
        f.pNext = nullptr;
        f.pTextStart = body;
        f.ulTextLen = len;
        f.ulTextSrcOffset = src_offset;
        f.State.eAction = action;
        f.State.LangID = 0x0809;
        f.State.EmphAdj = 0;
        f.State.RateAdj = 0;
        f.State.Volume = 100;
        f.State.PitchAdj.MiddleAdj = pitch;
        f.State.PitchAdj.RangeAdj = 0;
        return f;
    };

    SPVTEXTFRAG frag = make_frag(SPVA_Speak, text.c_str(),
                                 static_cast<ULONG>(text.size()), 0);

    TestSite site;
    site.rate = rate;

    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    hr = engine->Speak(0, format_id, format, &frag, &site);
    QueryPerformanceCounter(&t1);

    // The engine, its token and the format are released at the end: the
    // bookmark and silence checks below speak through the same instance.
    if (FAILED(hr)) {
        printf("Speak failed: 0x%08lx\n", hr);
        return 1;
    }

    double elapsed = double(t1.QuadPart - t0.QuadPart) / freq.QuadPart;
    double seconds = site.pcm.size() / 2.0 / 16000.0;
    printf("spoke %zu bytes (%.2fs audio) in %.2fs; %lu word event(s), "
           "%lu sentence event(s)\n",
           site.pcm.size(), seconds, elapsed, site.word_events,
           site.sentence_events);

    if (site.pcm.empty()) {
        printf("FAIL: no audio\n");
        return 1;
    }
    write_wav(out, site.pcm);
    printf("wrote %ls\n", out.c_str());

    // ---- the bookmark regression ---------------------------------------
    //
    // A caller marks its place in an utterance with <bookmark mark="127"/>,
    // and SAPI passes that down as a fragment whose text is "127". An engine
    // that speaks every fragment reads the number aloud - "Hello, 127, This
    // is Josh, 128". So: speak the same words with and without bookmarks
    // between them, and require the audio to be identical.
    printf("\nbookmark check:\n");

    const wchar_t* hello = L"Hello,";
    const wchar_t* josh = L"This is Josh";
    const wchar_t* mark127 = L"127";
    const wchar_t* mark128 = L"128";

    SPVTEXTFRAG f_hello = make_frag(SPVA_Speak, hello, 6, 0);
    SPVTEXTFRAG f_b127 = make_frag(SPVA_Bookmark, mark127, 3, 6);
    SPVTEXTFRAG f_josh = make_frag(SPVA_Speak, josh, 12, 9);
    SPVTEXTFRAG f_b128 = make_frag(SPVA_Bookmark, mark128, 3, 21);
    f_hello.pNext = &f_b127;
    f_b127.pNext = &f_josh;
    f_josh.pNext = &f_b128;

    site.reset();
    hr = engine->Speak(0, format_id, format, &f_hello, &site);
    std::vector<uint8_t> with_marks = site.pcm;
    std::vector<TestSite::Bookmark> marks = site.bookmarks;
    if (FAILED(hr)) {
        printf("FAIL: Speak with bookmarks failed: 0x%08lx\n", hr);
        return 1;
    }

    // The same words, with nothing between them.
    SPVTEXTFRAG p_hello = make_frag(SPVA_Speak, hello, 6, 0);
    SPVTEXTFRAG p_josh = make_frag(SPVA_Speak, josh, 12, 9);
    p_hello.pNext = &p_josh;

    site.reset();
    hr = engine->Speak(0, format_id, format, &p_hello, &site);
    std::vector<uint8_t> plain = site.pcm;
    if (FAILED(hr)) {
        printf("FAIL: Speak without bookmarks failed: 0x%08lx\n", hr);
        return 1;
    }

    // Written out so the result can be listened to as well as measured.
    write_wav(out + L".bookmarks.wav", with_marks);

    printf("  with bookmarks:    %zu bytes, %zu bookmark event(s)\n",
           with_marks.size(), marks.size());
    printf("  without bookmarks: %zu bytes\n", plain.size());
    for (const auto& m : marks)
        printf("    bookmark id %ld (\"%ls\") at stream offset %llu\n", m.id,
               m.text.c_str(), m.offset);

    bool failed = false;
    if (with_marks != plain) {
        printf("FAIL: the bookmarks changed the audio - the numbers are "
               "being spoken\n");
        failed = true;
    }
    if (marks.size() != 2) {
        printf("FAIL: expected 2 bookmark events, got %zu\n", marks.size());
        failed = true;
    } else if (marks[0].id != 127 || marks[1].id != 128) {
        printf("FAIL: bookmark ids were %ld and %ld, expected 127 and 128\n",
               marks[0].id, marks[1].id);
        failed = true;
    } else if (marks[0].offset == 0 && marks[1].offset == 0) {
        printf("FAIL: both bookmarks landed at stream offset 0, so a caller "
               "cannot tell them apart in time\n");
        failed = true;
    }

    // ---- silence --------------------------------------------------------
    // <silence msec="500"/> has to become half a second of actual quiet, so
    // a caller's requested pause between phrases is really there.
    SPVTEXTFRAG q_word = make_frag(SPVA_Speak, hello, 6, 0);
    site.reset();
    engine->Speak(0, format_id, format, &q_word, &site);
    const size_t word_only = site.pcm.size();

    SPVTEXTFRAG s_word = make_frag(SPVA_Speak, hello, 6, 0);
    SPVTEXTFRAG s_gap = make_frag(SPVA_Silence, L"", 0, 6);
    s_gap.State.SilenceMSecs = 500;
    s_word.pNext = &s_gap;

    site.reset();
    engine->Speak(0, format_id, format, &s_word, &site);
    const size_t expected_gap = 16000 * 500 / 1000 * 2;
    const size_t added =
        site.pcm.size() > word_only ? site.pcm.size() - word_only : 0;
    printf("\nsilence check: the word alone is %zu bytes, with a 500 ms pause "
           "%zu; added %zu (expected %zu)\n",
           word_only, site.pcm.size(), added, expected_gap);
    if (added != expected_gap) {
        printf("FAIL: a requested pause did not produce the silence asked "
               "for\n");
        failed = true;
    }

    CoTaskMemFree(format);
    engine->Release();
    token->Release();

    if (failed) {
        printf("\nFAIL\n");
        return 1;
    }
    printf("\nPASS\n");
    CoUninitialize();
    return 0;
}

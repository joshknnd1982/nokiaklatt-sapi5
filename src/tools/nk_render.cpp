// Render one utterance with the C++ engine and write it to a WAV.
//
// This exists to be diffed against the Python add-on's output: the port is
// only correct if it produces the same samples, and "sounds about right" is
// not a test.
//
//   nk_render <rom> <data-tree> <language> <voice|-> <out.wav> <text>

#include <windows.h>
#include <stdio.h>

#include <string>
#include <vector>

#include "../nk/engine.h"
#include "../nk/log.h"

namespace {

void write_wav(const std::wstring& path, const std::vector<uint8_t>& pcm) {
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) {
        fwprintf(stderr, L"could not write %s\n", path.c_str());
        return;
    }
    uint32_t data_size = static_cast<uint32_t>(pcm.size());
    uint32_t rate = nk::kSampleRate;
    uint32_t byte_rate = rate * 2;

    fwrite("RIFF", 1, 4, f);
    uint32_t riff = 36 + data_size;
    fwrite(&riff, 4, 1, f);
    fwrite("WAVEfmt ", 1, 8, f);
    uint32_t fmt_size = 16;
    uint16_t fmt = 1, channels = 1, align = 2, bits = 16;
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

std::wstring module_dir() {
    wchar_t buf[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring p(buf);
    size_t cut = p.find_last_of(L'\\');
    return cut == std::wstring::npos ? L"." : p.substr(0, cut);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    nk::log_init("render");
    if (argc < 7) {
        fwprintf(stderr,
                 L"usage: nk_render <rom> <tree> <lang> <voice|-> <out.wav> "
                 L"<text> [bank]\n");
        return 2;
    }

    std::wstring rom = argv[1];
    std::wstring tree = argv[2];
    uint32_t language = static_cast<uint32_t>(_wtoi(argv[3]));
    std::wstring wvoice = argv[4];
    std::wstring out = argv[5];
    std::wstring text = argv[6];
    uint32_t bank = argc > 7 ? static_cast<uint32_t>(_wtoi(argv[7])) : 0;

    std::string voice;
    if (wvoice != L"-")
        for (wchar_t c : wvoice) voice.push_back(static_cast<char>(c));

    std::string error;
    // The vendored unicorn.dll lives under bin/ in the source tree and beside
    // the executable in an installed copy; try both.
    const nk::UcApi* api = nk::uc_load(module_dir().c_str(), &error);
    if (!api) {
        std::wstring alt = module_dir() +
                           L"\\..\\..\\..\\bin\\_nokia\\lib\\unicorn\\lib";
        api = nk::uc_load(alt.c_str(), &error);
    }
    if (!api) {
        fprintf(stderr, "unicorn: %s\n", error.c_str());
        return 1;
    }

    LARGE_INTEGER freq, t0, t1, t2;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);

    try {
        nk::Engine::Options options;
        options.language = language;
        options.voice = voice;
        options.voice_bank_override = bank;

        nk::Engine engine(rom, tree, options, api);
        QueryPerformanceCounter(&t1);

        std::vector<uint8_t> pcm;
        engine.speak(text, [&pcm](const uint8_t* data, size_t n) {
            pcm.insert(pcm.end(), data, data + n);
            return true;
        });
        QueryPerformanceCounter(&t2);

        write_wav(out, pcm);

        double build = double(t1.QuadPart - t0.QuadPart) / freq.QuadPart;
        double speak = double(t2.QuadPart - t1.QuadPart) / freq.QuadPart;
        double seconds = pcm.size() / 2.0 / nk::kSampleRate;
        printf("%zu bytes  %.2fs audio  build %.3fs  speak %.3fs  %.2fx\n",
               pcm.size(), seconds, build, speak,
               speak > 0 ? seconds / speak : 0.0);
        printf("voice_applied=%s\n",
               engine.voice_applied_known()
                   ? (engine.voice_applied() ? "yes" : "no")
                   : "unknown");
        return pcm.empty() ? 1 : 0;
    } catch (const nk::EngineError& e) {
        fprintf(stderr, "engine error (%s): %s\n",
                e.kind == nk::EngineFailure::kInit    ? "init"
                : e.kind == nk::EngineFailure::kText  ? "text"
                                                      : "fault",
                e.what());
        return 1;
    } catch (const std::exception& e) {
        fprintf(stderr, "error: %s\n", e.what());
        return 1;
    }
}

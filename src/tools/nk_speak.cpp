// Exercise the host over its pipe, the way the SAPI engines do.
//
//   nk_speak voices
//   nk_speak say <voice-id> <out.wav> <text> [rate] [pitch] [volume]
//   nk_speak ping | reload | shutdown

#include <windows.h>
#include <stdio.h>

#include <string>
#include <vector>

#include "../nk/client.h"
#include "../nk/log.h"

namespace {

void write_wav(const std::wstring& path, const std::vector<uint8_t>& pcm) {
    FILE* f = _wfopen(path.c_str(), L"wb");
    if (!f) return;
    uint32_t data_size = static_cast<uint32_t>(pcm.size());
    uint32_t rate = nk::kHostSampleRate;
    uint32_t byte_rate = rate * 2;
    uint32_t riff = 36 + data_size;
    uint32_t fmt_size = 16;
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

std::string narrow(const std::wstring& s) {
    std::string out;
    for (wchar_t c : s) out.push_back(c < 128 ? static_cast<char>(c) : '?');
    return out;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    nk::log_init("nk_speak");
    if (argc < 2) {
        fwprintf(stderr,
                 L"usage: nk_speak voices\n"
                 L"       nk_speak say <voice-id> <out.wav> <text> "
                 L"[rate] [pitch] [volume]\n"
                 L"       nk_speak ping | reload | shutdown\n");
        return 2;
    }

    nk::Client client;
    std::wstring verb = argv[1];

    if (verb == L"ping") {
        bool ok = client.ping();
        printf("%s\n", ok ? "pong" : "no reply");
        return ok ? 0 : 1;
    }
    if (verb == L"reload") {
        bool ok = client.reload();
        printf("%s\n", ok ? "reloaded" : "failed");
        return ok ? 0 : 1;
    }
    if (verb == L"shutdown") {
        bool ok = client.shutdown_host();
        printf("%s\n", ok ? "host stopped" : "failed");
        return ok ? 0 : 1;
    }

    if (verb == L"voices") {
        std::vector<nk::ClientVoice> voices;
        if (!client.get_voices(&voices)) {
            fprintf(stderr, "could not get the voice list: %s\n",
                    client.last_error().c_str());
            return 1;
        }
        printf("%zu voice(s)\n", voices.size());
        for (const auto& v : voices)
            printf("  %-18s lcid=%04x %-6s %s%s\n", v.id.c_str(), v.lcid,
                   v.female ? "female" : "male", narrow(v.label).c_str(),
                   v.custom ? "   [custom]" : "");
        return 0;
    }

    if (verb == L"cancel") {
        // How long it takes to stop speaking, which for a screen reader
        // matters more than how long it takes to start: pressing a key must
        // silence the previous line straight away.
        std::wstring text =
            argc > 2 ? argv[2]
                     : L"This is a deliberately long sentence, so that there "
                       L"is still plenty of it left to interrupt when the "
                       L"stop arrives partway through.";
        std::vector<uint8_t> pcm;
        std::string error;
        LARGE_INTEGER freq, stop_at, done;
        QueryPerformanceFrequency(&freq);
        stop_at.QuadPart = 0;
        bool stopped = false;

        client.speak("5320:1-male", text, 0, 0, 100,
                     [&](const uint8_t* data, size_t size) {
                         pcm.insert(pcm.end(), data, data + size);
                         if (!stopped && pcm.size() >= 4096) {
                             stopped = true;
                             QueryPerformanceCounter(&stop_at);
                             return false;  // ask to stop here
                         }
                         return true;
                     },
                     &error);
        QueryPerformanceCounter(&done);
        double ms = stop_at.QuadPart
                        ? 1000.0 * (done.QuadPart - stop_at.QuadPart) / freq.QuadPart
                        : 0.0;
        printf("stopped after %zu bytes (%.2fs of audio); the cancel took "
               "%.1f ms\n",
               pcm.size(), pcm.size() / 2.0 / nk::kHostSampleRate, ms);
        return 0;
    }

    if (verb == L"say") {
        if (argc < 5) {
            fwprintf(stderr, L"say needs a voice id, an output file and text\n");
            return 2;
        }
        std::string voice_id = narrow(argv[2]);
        std::wstring out = argv[3];
        std::wstring text = argv[4];
        int32_t rate = argc > 5 ? _wtoi(argv[5]) : 0;
        int32_t pitch = argc > 6 ? _wtoi(argv[6]) : 0;
        uint32_t volume = argc > 7 ? static_cast<uint32_t>(_wtoi(argv[7])) : 100;

        std::vector<uint8_t> pcm;
        std::string error;
        LARGE_INTEGER freq, t0, first, t1;
        QueryPerformanceFrequency(&freq);
        QueryPerformanceCounter(&t0);
        first.QuadPart = 0;

        bool ok = client.speak(
            voice_id, text, rate, pitch, volume,
            [&](const uint8_t* data, size_t size) {
                if (!first.QuadPart) QueryPerformanceCounter(&first);
                pcm.insert(pcm.end(), data, data + size);
                return true;
            },
            &error);
        QueryPerformanceCounter(&t1);

        if (!ok) {
            fprintf(stderr, "speak failed: %s\n", error.c_str());
            return 1;
        }
        write_wav(out, pcm);

        double total = double(t1.QuadPart - t0.QuadPart) / freq.QuadPart;
        double latency = first.QuadPart
                             ? double(first.QuadPart - t0.QuadPart) / freq.QuadPart
                             : 0.0;
        double seconds = pcm.size() / 2.0 / nk::kHostSampleRate;
        printf("%zu bytes  %.2fs audio  first audio %.3fs  total %.3fs  %.2fx\n",
               pcm.size(), seconds, latency, total,
               total > 0 ? seconds / total : 0.0);
        return pcm.empty() ? 1 : 0;
    }

    fwprintf(stderr, L"unknown command\n");
    return 2;
}

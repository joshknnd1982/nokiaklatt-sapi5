// NokiaKlattHost.exe - the process that owns the emulated speech engines.
//
// One host per logged-in user, started on demand by whichever SAPI DLL needs
// it first. Both the 32-bit and 64-bit engines talk to it, so the warm engine
// cache is shared across every application on the desktop.
//
// Each connection gets a thread that reads commands and a synthesis thread
// that writes audio. They are separate on purpose: a cancel has to be read
// while an utterance is still being written, and that is what makes stopping
// speech immediate rather than "at the end of this sentence".

#include <windows.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "engine_pool.h"
#include "nk/catalog.h"
#include "nk/dsp.h"
#include "nk/log.h"
#include "nk/pipe_io.h"
#include "nk/protocol.h"
#include "nk/settings.h"

namespace {

nk::EnginePool* g_pool = nullptr;
const nk::UcApi* g_api = nullptr;
std::mutex g_voices_mutex;
std::vector<nk::VoiceEntry> g_voices;
std::atomic<bool> g_shutdown{false};
std::atomic<int> g_live_clients{0};

std::wstring module_dir() {
    wchar_t buf[MAX_PATH] = {0};
    GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring p(buf);
    size_t cut = p.find_last_of(L'\\');
    return cut == std::wstring::npos ? L"." : p.substr(0, cut);
}

void refresh_voices() {
    std::lock_guard<std::mutex> lock(g_voices_mutex);
    g_voices = nk::enumerate_voices(nk::roms_root());
    NK_LOG("catalogue: %zu voice(s) from %ls", g_voices.size(),
           nk::roms_root().c_str());
}

// The voice a request names. "custom" is resolved through the settings file,
// so the configuration utility can repoint it without the voice list changing.
bool resolve_voice(const std::string& id, const nk::Settings& settings,
                   nk::VoiceEntry* out) {
    std::lock_guard<std::mutex> lock(g_voices_mutex);
    if (id == nk::kCustomVoiceId) {
        for (const auto& v : g_voices) {
            if (v.build == settings.custom_build &&
                v.language == settings.custom_language &&
                v.voice_name == settings.custom_voice_name) {
                *out = v;
                return true;
            }
        }
        // The build may not offer the requested named voice; take the same
        // build and language with whatever voice it does have.
        for (const auto& v : g_voices) {
            if (v.build == settings.custom_build &&
                v.language == settings.custom_language) {
                *out = v;
                return true;
            }
        }
        if (!g_voices.empty()) {
            *out = g_voices.front();
            return true;
        }
        return false;
    }
    for (const auto& v : g_voices) {
        if (v.id() == id) {
            *out = v;
            return true;
        }
    }
    return false;
}

// ---- framing -----------------------------------------------------------

// One writer at a time per connection: the synthesis thread streams audio
// while the command thread may answer a ping. Reads and writes overlap
// freely, which is the whole point of the overlapped channel - on a
// synchronous handle the command thread's blocking read would hold off every
// write of the utterance it is waiting to interrupt.
struct Connection {
    nk::PipeChannel channel;
    std::mutex write_mutex;
    std::atomic<nk::Engine*> speaking{nullptr};
    std::atomic<bool> cancel{false};
};

bool send_message(Connection& conn, uint32_t type, const void* payload,
                  uint32_t size) {
    std::lock_guard<std::mutex> lock(conn.write_mutex);
    nk::MessageHeader header{type, size};
    if (!conn.channel.write_all(&header, sizeof(header))) return false;
    if (size && !conn.channel.write_all(payload, size)) return false;
    return true;
}

bool send_error(Connection& conn, const std::string& message) {
    NK_LOG("replying with error: %s", message.c_str());
    return send_message(conn, nk::RESP_ERROR, message.data(),
                        static_cast<uint32_t>(message.size()));
}

// ---- commands ----------------------------------------------------------

void handle_get_voices(Connection& conn) {
    std::vector<nk::VoiceEntry> voices;
    {
        std::lock_guard<std::mutex> lock(g_voices_mutex);
        voices = g_voices;
    }

    std::vector<uint8_t> buffer(sizeof(nk::VoicesHeader) +
                                (voices.size() + 1) * sizeof(nk::VoiceRecord));
    auto* head = reinterpret_cast<nk::VoicesHeader*>(buffer.data());
    head->version = nk::kProtocolVersion;
    head->count = static_cast<uint32_t>(voices.size() + 1);

    auto* records = reinterpret_cast<nk::VoiceRecord*>(
        buffer.data() + sizeof(nk::VoicesHeader));
    auto fill = [](nk::VoiceRecord& r, const std::string& id,
                   const std::wstring& label, uint16_t lcid, bool female,
                   bool custom, uint32_t language, const std::string& build) {
        memset(&r, 0, sizeof(r));
        strncpy_s(r.id, id.c_str(), nk::kMaxVoiceId - 1);
        for (size_t i = 0; i < label.size() && i < nk::kMaxLabel - 1; ++i)
            r.label[i] = static_cast<uint16_t>(label[i]);
        r.lcid = lcid;
        r.gender = female ? 1 : 0;
        r.is_custom = custom ? 1 : 0;
        r.language = language;
        strncpy_s(r.build, build.c_str(), sizeof(r.build) - 1);
    };

    size_t at = 0;
    for (const auto& v : voices) {
        fill(records[at++], v.id(), L"Nokia Klatt " + v.label(),
             nk::language_lcid(v.language), v.suffix == "female", false,
             v.language, v.build);
    }
    // The Custom Voice is always offered, whatever it currently points at, so
    // an application that has selected it keeps working when the user
    // repoints it.
    nk::Settings settings = nk::Settings::load();
    fill(records[at++], nk::kCustomVoiceId, L"Nokia Klatt Custom Voice",
         nk::language_lcid(settings.custom_language),
         settings.custom_voice_name == "DefaultFemale", true,
         settings.custom_language, settings.custom_build);

    NK_LOG("sending %u voice(s)", head->count);
    send_message(conn, nk::RESP_VOICES, buffer.data(),
                 static_cast<uint32_t>(buffer.size()));
}

void handle_speak(Connection& conn, const std::vector<uint8_t>& payload,
                  bool preview) {
    if (payload.size() < sizeof(nk::SpeakRequest)) {
        send_error(conn, "malformed speak request");
        return;
    }
    nk::SpeakRequest request;
    memcpy(&request, payload.data(), sizeof(request));
    request.voice_id[nk::kMaxVoiceId - 1] = '\0';

    size_t text_bytes = static_cast<size_t>(request.text_chars) * 2;
    if (payload.size() < sizeof(nk::SpeakRequest) + text_bytes) {
        send_error(conn, "speak request is shorter than its text");
        return;
    }
    std::wstring text(request.text_chars, L'\0');
    if (request.text_chars)
        memcpy(&text[0], payload.data() + sizeof(nk::SpeakRequest), text_bytes);

    nk::Settings settings = nk::Settings::load();
    nk::VoiceEntry voice;
    if (!resolve_voice(request.voice_id, settings, &voice)) {
        send_error(conn, std::string("no such voice: ") + request.voice_id);
        return;
    }

    NK_LOG("speak: voice=%s (%s:%u) rate=%d pitch=%d volume=%u chars=%u",
           request.voice_id, voice.build.c_str(), voice.language, request.rate,
           request.pitch, request.volume, request.text_chars);

    nk::Engine* engine = nullptr;
    try {
        engine = g_pool->acquire(voice, settings);
    } catch (const nk::EngineError& e) {
        send_error(conn, std::string("could not start the engine: ") + e.what());
        return;
    } catch (const std::exception& e) {
        send_error(conn, std::string("could not start the engine: ") + e.what());
        return;
    }

    conn.speaking = engine;
    conn.cancel = false;

    nk::OutputChain::Settings chain;
    chain.rate = nk::rate_factor(settings, request.rate);
    chain.pitch = nk::pitch_factor(settings, request.pitch);
    chain.volume = nk::volume_factor(settings, request.volume);
    chain.trim_leading_silence = settings.trim_leading_silence;
    chain.trailing_silence_ms = settings.trailing_silence_ms;
    nk::OutputChain output(chain);

    uint32_t total = 0;
    bool ok = true;
    bool faulted = false;

    auto emit = [&](const std::vector<uint8_t>& bytes) {
        if (bytes.empty()) return true;
        std::lock_guard<std::mutex> lock(conn.write_mutex);
        nk::MessageHeader header{nk::RESP_AUDIO,
                                 static_cast<uint32_t>(sizeof(nk::AudioChunk) +
                                                       bytes.size())};
        nk::AudioChunk chunk{static_cast<uint32_t>(bytes.size())};
        if (!conn.channel.write_all(&header, sizeof(header))) return false;
        if (!conn.channel.write_all(&chunk, sizeof(chunk))) return false;
        if (!conn.channel.write_all(bytes.data(), bytes.size())) return false;
        total += static_cast<uint32_t>(bytes.size());
        return true;
    };

    try {
        engine->speak(text, [&](const uint8_t* data, size_t size) {
            if (conn.cancel) return false;
            std::vector<uint8_t> shaped = output.feed(data, size);
            if (!emit(shaped)) {
                ok = false;
                return false;
            }
            return true;
        });
        if (ok && !conn.cancel) {
            std::vector<uint8_t> tail = output.flush();
            if (!emit(tail)) ok = false;
        }
    } catch (const nk::EngineError& e) {
        NK_LOG("speak failed (%d): %s", static_cast<int>(e.kind), e.what());
        faulted = e.kind == nk::EngineFailure::kFault;
        if (!conn.cancel && total == 0) send_error(conn, e.what());
    } catch (const std::exception& e) {
        NK_LOG("speak failed: %s", e.what());
        faulted = true;
        if (!conn.cancel && total == 0) send_error(conn, e.what());
    }

    conn.speaking = nullptr;
    if (faulted) {
        // The emulator's state is unknown after a fault; it must not be
        // handed to the next utterance.
        g_pool->discard(engine);
    } else {
        g_pool->release(engine);
    }

    nk::AudioEnd end{total, conn.cancel ? 1u : 0u};
    send_message(conn, nk::RESP_AUDIO_END, &end, sizeof(end));
    NK_LOG("speak done: %u bytes%s", total, conn.cancel ? " (cancelled)" : "");
    (void)preview;
}

void client_thread(HANDLE pipe) {
    ++g_live_clients;
    Connection conn;
    conn.channel.reset(pipe);
    NK_LOG("client connected");

    std::thread synth;
    bool stop = false;
    while (!stop) {
        nk::MessageHeader header{};
        if (!conn.channel.read_all(&header, sizeof(header))) break;

        std::vector<uint8_t> payload;
        if (header.size) {
            if (header.size > (64u << 20)) break;
            payload.resize(header.size);
            if (!conn.channel.read_all(payload.data(), header.size)) break;
        }

        switch (header.type) {
            case nk::CMD_PING:
                send_message(conn, nk::RESP_PONG, nullptr, 0);
                break;

            case nk::CMD_GET_VOICES:
                handle_get_voices(conn);
                break;

            case nk::CMD_SPEAK:
            case nk::CMD_PREVIEW:
                // Synthesis runs on its own thread so this loop stays free to
                // read the stop that interrupts it.
                if (synth.joinable()) synth.join();
                synth = std::thread([&conn, payload,
                                     preview = header.type == nk::CMD_PREVIEW] {
                    handle_speak(conn, payload, preview);
                });
                break;

            case nk::CMD_STOP: {
                conn.cancel = true;
                nk::Engine* engine = conn.speaking.load();
                if (engine) engine->cancel();
                NK_LOG("stop requested");
                send_message(conn, nk::RESP_OK, nullptr, 0);
                break;
            }

            case nk::CMD_RELOAD:
                g_pool->clear();
                refresh_voices();
                send_message(conn, nk::RESP_OK, nullptr, 0);
                break;

            case nk::CMD_SHUTDOWN:
                send_message(conn, nk::RESP_OK, nullptr, 0);
                g_shutdown = true;
                NK_LOG("shutdown requested by a client");
                stop = true;
                break;

            default:
                send_error(conn, "unknown command");
                break;
        }
    }

    // The connection is over; stop anything still speaking on it before the
    // Connection this thread points at goes out of scope, then let the client
    // drain whatever has already been written.
    conn.cancel = true;
    if (nk::Engine* engine = conn.speaking.load()) engine->cancel();
    if (synth.joinable()) synth.join();
    if (conn.channel.valid()) {
        FlushFileBuffers(conn.channel.handle());
        DisconnectNamedPipe(conn.channel.handle());
    }
    --g_live_clients;
    NK_LOG("client disconnected");
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    nk::log_init("host");

    for (int i = 1; i < argc; ++i) {
        if (!wcscmp(argv[i], L"--log")) nk::log_set_enabled(true);
    }
    nk::Settings boot = nk::Settings::load();
    if (boot.logging) nk::log_set_enabled(true);

    // One host per session. A second copy exits quietly rather than fighting
    // over the pipe name.
    HANDLE mutex = CreateMutexW(nullptr, TRUE, NK_SERVER_MUTEX);
    if (!mutex) return 1;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(mutex);
        NK_LOG("another host is already running; exiting");
        return 0;
    }

    std::string error;
    g_api = nk::uc_load(module_dir().c_str(), &error);
    if (!g_api) {
        NK_LOG("fatal: %s", error.c_str());
        ReleaseMutex(mutex);
        CloseHandle(mutex);
        return 1;
    }

    nk::EnginePool pool(g_api);
    g_pool = &pool;
    refresh_voices();

    NK_LOG("host ready on %ls", NK_PIPE_NAME);

    std::vector<std::thread> threads;
    while (!g_shutdown) {
        HANDLE pipe = CreateNamedPipeW(
            NK_PIPE_NAME, PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED,
            PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT,
            PIPE_UNLIMITED_INSTANCES, 1 << 16, 1 << 16, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) {
            NK_LOG("CreateNamedPipe failed: %lu", GetLastError());
            Sleep(1000);
            continue;
        }

        if (nk::connect_overlapped(pipe)) {
            threads.emplace_back([pipe] {
                // client_thread hands the handle to a PipeChannel, which
                // closes it when the connection ends.
                client_thread(pipe);
            });
            // Reap finished threads so a long-lived host does not accumulate
            // them.
            for (size_t i = 0; i < threads.size();) {
                if (threads[i].joinable() && g_live_clients == 0 &&
                    threads.size() > 8) {
                    threads[i].join();
                    threads.erase(threads.begin() + i);
                } else {
                    ++i;
                }
            }
        } else {
            CloseHandle(pipe);
        }
    }

    for (auto& t : threads)
        if (t.joinable()) t.detach();

    NK_LOG("host exiting");
    ReleaseMutex(mutex);
    CloseHandle(mutex);
    return 0;
}

#include "client.h"

#include <shlwapi.h>

#include "catalog.h"
#include "pipe_io.h"
#include "log.h"

#pragma comment(lib, "shlwapi.lib")

namespace nk {
namespace {

std::wstring this_module_dir() {
    HMODULE mod = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&this_module_dir), &mod);
    wchar_t buf[MAX_PATH] = {0};
    GetModuleFileNameW(mod, buf, MAX_PATH);
    PathRemoveFileSpecW(buf);
    return buf;
}

}  // namespace

Client::Client() { InitializeCriticalSection(&lock_); }

Client::~Client() {
    disconnect();
    DeleteCriticalSection(&lock_);
}

// The host is x64 and lives beside the 64-bit engine. A 32-bit SAPI DLL
// installed under x86\ therefore has to look one directory up as well.
std::wstring Client::host_path() const {
    std::wstring dir = this_module_dir();
    const wchar_t* kNames[] = {L"\\NokiaKlattHost.exe",
                               L"\\..\\NokiaKlattHost.exe",
                               L"\\..\\..\\NokiaKlattHost.exe"};
    for (const wchar_t* name : kNames) {
        std::wstring candidate = dir + name;
        if (GetFileAttributesW(candidate.c_str()) != INVALID_FILE_ATTRIBUTES)
            return candidate;
    }
    std::wstring installed = install_root() + L"\\NokiaKlattHost.exe";
    if (GetFileAttributesW(installed.c_str()) != INVALID_FILE_ATTRIBUTES)
        return installed;
    return std::wstring();
}

bool Client::host_running() const {
    HANDLE mutex = OpenMutexW(SYNCHRONIZE, FALSE, NK_SERVER_MUTEX);
    if (!mutex) return false;
    CloseHandle(mutex);
    return true;
}

bool Client::launch_host() {
    if (host_running()) return true;

    std::wstring exe = host_path();
    if (exe.empty()) {
        last_error_ = "NokiaKlattHost.exe was not found beside the engine";
        NK_LOG("%s", last_error_.c_str());
        return false;
    }

    // Two applications starting to speak at once must not race to start two
    // hosts; the loser waits for the winner's.
    HANDLE launch = CreateMutexW(nullptr, FALSE, NK_LAUNCH_MUTEX);
    if (!launch) return false;
    DWORD waited = WaitForSingleObject(launch, 10000);
    if (waited != WAIT_OBJECT_0 && waited != WAIT_ABANDONED) {
        CloseHandle(launch);
        return false;
    }

    bool ok = host_running();
    if (!ok) {
        // The host's working directory has to be its own, so it finds
        // unicorn.dll and the ROMs beside it.
        wchar_t dir_buf[MAX_PATH] = {0};
        wcsncpy_s(dir_buf, exe.c_str(), _TRUNCATE);
        PathRemoveFileSpecW(dir_buf);
        std::wstring dir(dir_buf);

        STARTUPINFOW si = {sizeof(si)};
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
        PROCESS_INFORMATION pi = {};
        std::wstring command = L"\"" + exe + L"\"";
        NK_LOG("starting the host: %ls", exe.c_str());
        if (CreateProcessW(exe.c_str(), &command[0], nullptr, nullptr, FALSE,
                           CREATE_NO_WINDOW, nullptr, dir.c_str(), &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            for (int i = 0; i < 100 && !host_running(); ++i) Sleep(50);
            ok = host_running();
        } else {
            last_error_ = "could not start NokiaKlattHost.exe (error " +
                          std::to_string(GetLastError()) + ")";
            NK_LOG("%s", last_error_.c_str());
        }
    }

    ReleaseMutex(launch);
    CloseHandle(launch);
    return ok;
}

bool Client::connect() {
    if (pipe_ != INVALID_HANDLE_VALUE) return true;
    if (!launch_host()) return false;

    for (int attempt = 0; attempt < 40; ++attempt) {
        // Overlapped, to match the host: on a synchronous handle a
        // blocking read holds off every write on the other side of it.
        pipe_ = CreateFileW(NK_PIPE_NAME, GENERIC_READ | GENERIC_WRITE, 0,
                            nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED,
                            nullptr);
        if (pipe_ != INVALID_HANDLE_VALUE) {
            channel_.reset(pipe_);
            NK_LOG("connected to the host");
            return true;
        }
        if (GetLastError() == ERROR_PIPE_BUSY) {
            WaitNamedPipeW(NK_PIPE_NAME, 2000);
        } else {
            Sleep(50);
        }
    }
    last_error_ = "could not connect to NokiaKlattHost (error " +
                  std::to_string(GetLastError()) + ")";
    NK_LOG("%s", last_error_.c_str());
    return false;
}

void Client::disconnect() {
    EnterCriticalSection(&lock_);
    channel_.close();
    pipe_ = INVALID_HANDLE_VALUE;
    LeaveCriticalSection(&lock_);
}

bool Client::ensure_connected() {
    if (pipe_ != INVALID_HANDLE_VALUE) return true;
    return connect();
}

bool Client::send(uint32_t type, const void* payload, uint32_t size) {
    MessageHeader header{type, size};
    if (!channel_.write_all(&header, sizeof(header))) return false;
    if (size && !channel_.write_all(payload, size)) return false;
    return true;
}

bool Client::recv_header(MessageHeader* header) {
    return channel_.read_all(header, sizeof(*header));
}

bool Client::recv_payload(void* data, size_t size) {
    return channel_.read_all(data, size);
}

bool Client::ping() {
    EnterCriticalSection(&lock_);
    bool ok = false;
    if (ensure_connected() && send(CMD_PING, nullptr, 0)) {
        MessageHeader header{};
        ok = recv_header(&header) && header.type == RESP_PONG;
    }
    if (!ok) {
        // A stale handle from a host that has exited: drop it so the next call
        // starts a new one.
        channel_.close();
        pipe_ = INVALID_HANDLE_VALUE;
    }
    LeaveCriticalSection(&lock_);
    return ok;
}

bool Client::get_voices(std::vector<ClientVoice>* out) {
    out->clear();
    EnterCriticalSection(&lock_);
    bool ok = false;

    // Two attempts: a host that exited between calls leaves a handle that
    // fails once, and reconnecting is invisible to the caller.
    for (int attempt = 0; attempt < 2 && !ok; ++attempt) {
        if (!ensure_connected()) break;

        bool failed = true;
        do {
            if (!send(CMD_GET_VOICES, nullptr, 0)) break;

            MessageHeader header{};
            if (!recv_header(&header) || header.type != RESP_VOICES ||
                header.size < sizeof(VoicesHeader))
                break;

            std::vector<uint8_t> payload(header.size);
            if (!recv_payload(payload.data(), payload.size())) break;

            VoicesHeader head{};
            memcpy(&head, payload.data(), sizeof(head));
            size_t have =
                (payload.size() - sizeof(VoicesHeader)) / sizeof(VoiceRecord);
            size_t count = head.count < have ? head.count : have;

            const auto* records = reinterpret_cast<const VoiceRecord*>(
                payload.data() + sizeof(VoicesHeader));
            for (size_t i = 0; i < count; ++i) {
                ClientVoice v;
                v.id = records[i].id;
                for (uint32_t k = 0; k < kMaxLabel && records[i].label[k]; ++k)
                    v.label.push_back(static_cast<wchar_t>(records[i].label[k]));
                v.lcid = records[i].lcid;
                v.female = records[i].gender != 0;
                v.custom = records[i].is_custom != 0;
                v.language = records[i].language;
                v.build = records[i].build;
                out->push_back(std::move(v));
            }
            failed = false;
            ok = true;
        } while (false);

        if (failed) {
            out->clear();
            channel_.close();
            pipe_ = INVALID_HANDLE_VALUE;
        }
    }

    LeaveCriticalSection(&lock_);
    NK_LOG("get_voices -> %zu voice(s)%s", out->size(), ok ? "" : " (FAILED)");
    return ok;
}

bool Client::speak(const std::string& voice_id, const std::wstring& text,
                   int32_t rate, int32_t pitch, uint32_t volume,
                   const AudioCallback& on_audio, std::string* error) {
    EnterCriticalSection(&lock_);
    bool ok = false;
    bool cancelled = false;

    if (!ensure_connected()) {
        if (error) *error = last_error_;
        LeaveCriticalSection(&lock_);
        return false;
    }

    std::vector<uint8_t> request(sizeof(SpeakRequest) + text.size() * 2);
    auto* head = reinterpret_cast<SpeakRequest*>(request.data());
    memset(head, 0, sizeof(*head));
    strncpy_s(head->voice_id, voice_id.c_str(), kMaxVoiceId - 1);
    head->rate = rate;
    head->pitch = pitch;
    head->volume = volume;
    head->text_chars = static_cast<uint32_t>(text.size());
    if (!text.empty())
        memcpy(request.data() + sizeof(SpeakRequest), text.data(),
               text.size() * 2);

    if (!send(CMD_SPEAK, request.data(),
              static_cast<uint32_t>(request.size()))) {
        if (error) *error = "the host closed the connection";
        channel_.close();
        pipe_ = INVALID_HANDLE_VALUE;
        LeaveCriticalSection(&lock_);
        return false;
    }

    std::vector<uint8_t> buffer;
    for (;;) {
        MessageHeader header{};
        if (!recv_header(&header)) {
            if (error) *error = "the host stopped responding";
            channel_.close();
            pipe_ = INVALID_HANDLE_VALUE;
            LeaveCriticalSection(&lock_);
            return false;
        }

        if (header.type == RESP_AUDIO) {
            AudioChunk chunk{};
            if (!recv_payload(&chunk, sizeof(chunk))) break;
            size_t remaining = header.size - sizeof(chunk);
            if (chunk.size != remaining) break;
            buffer.resize(chunk.size);
            if (chunk.size && !recv_payload(buffer.data(), buffer.size()))
                break;

            if (!cancelled && !on_audio(buffer.data(), buffer.size())) {
                // Stop now, then keep reading until the host says it is done:
                // leaving audio in the pipe would desynchronise the next
                // utterance.
                cancelled = true;
                send(CMD_STOP, nullptr, 0);
            }
        } else if (header.type == RESP_AUDIO_END) {
            AudioEnd end{};
            if (header.size >= sizeof(end)) recv_payload(&end, sizeof(end));
            ok = true;
            break;
        } else if (header.type == RESP_OK) {
            // The acknowledgement of the stop we just sent.
            continue;
        } else if (header.type == RESP_ERROR) {
            std::string message(header.size, '\0');
            if (header.size) recv_payload(&message[0], header.size);
            if (error) *error = message;
            last_error_ = message;
            NK_LOG("host error: %s", message.c_str());
            // The host still sends an end marker after an error.
            continue;
        } else {
            std::vector<uint8_t> skip(header.size);
            if (header.size) recv_payload(skip.data(), skip.size());
        }
    }

    LeaveCriticalSection(&lock_);
    return ok;
}

bool Client::reload() {
    EnterCriticalSection(&lock_);
    bool ok = false;
    if (ensure_connected() && send(CMD_RELOAD, nullptr, 0)) {
        MessageHeader header{};
        ok = recv_header(&header) && header.type == RESP_OK;
    }
    LeaveCriticalSection(&lock_);
    return ok;
}

bool Client::shutdown_host() {
    EnterCriticalSection(&lock_);
    bool ok = false;
    if (pipe_ != INVALID_HANDLE_VALUE || host_running()) {
        if (ensure_connected() && send(CMD_SHUTDOWN, nullptr, 0)) {
            MessageHeader header{};
            ok = recv_header(&header) && header.type == RESP_OK;
        }
    } else {
        ok = true;  // nothing to shut down
    }
    LeaveCriticalSection(&lock_);
    disconnect();
    return ok;
}

}  // namespace nk

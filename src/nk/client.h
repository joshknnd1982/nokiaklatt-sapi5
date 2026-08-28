// The SAPI side of the connection to the Nokia Klatt host.
//
// Built for both x86 and x64: the emulator only exists in the x64 host, and
// this is how either SAPI engine reaches it.
#pragma once

#include <windows.h>
#include <stdint.h>

#include <functional>
#include <string>
#include <vector>

#include "pipe_io.h"
#include "protocol.h"

namespace nk {

struct ClientVoice {
    std::string id;
    std::wstring label;
    uint16_t lcid = 0x0409;
    bool female = false;
    bool custom = false;
    uint32_t language = 0;
    std::string build;
};

class Client {
  public:
    Client();
    ~Client();

    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    // Connects, starting the host if it is not already running.
    bool connect();
    void disconnect();
    bool connected() const { return pipe_ != INVALID_HANDLE_VALUE; }

    bool ping();
    bool get_voices(std::vector<ClientVoice>* out);

    // Called with each buffer of finished PCM. Return false to stop: the
    // client sends a cancel and drains what is already in flight, so the call
    // still returns cleanly.
    using AudioCallback = std::function<bool(const uint8_t* data, size_t size)>;

    // Synthesises `text` and streams the audio through `on_audio`.
    // `error` receives the host's message when this returns false.
    bool speak(const std::string& voice_id, const std::wstring& text,
               int32_t rate, int32_t pitch, uint32_t volume,
               const AudioCallback& on_audio, std::string* error);

    // Drops the host's warm engines and makes it re-read the settings file.
    bool reload();
    bool shutdown_host();

    const std::string& last_error() const { return last_error_; }

  private:
    bool send(uint32_t type, const void* payload, uint32_t size);
    bool recv_header(MessageHeader* header);
    bool recv_payload(void* data, size_t size);
    bool ensure_connected();
    bool host_running() const;
    bool launch_host();
    std::wstring host_path() const;

    // The channel owns the handle; pipe_ only records that one is open.
    PipeChannel channel_;
    HANDLE pipe_ = INVALID_HANDLE_VALUE;
    CRITICAL_SECTION lock_;
    std::string last_error_;
};

}  // namespace nk

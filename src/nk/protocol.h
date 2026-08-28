// The wire protocol between the SAPI engines and the Nokia Klatt host.
//
// The emulator is x64 only - the vendored unicorn.dll has no 32-bit build - so
// the engine lives in its own x64 process and both the 32-bit and 64-bit SAPI
// DLLs are thin clients of it. That also means one warm engine cache is shared
// by every application on the desktop, which is what makes switching back to a
// language someone has used before instant rather than a fresh ROM mapping.
//
// Every field is fixed-width and the structures are packed, because the two
// ends are built for different architectures.
#pragma once

#include <stdint.h>

namespace nk {

// Per-session, not Global\: one host per logged-in user, started on demand,
// with no privileges needed.
#define NK_PIPE_NAME L"\\\\.\\pipe\\NokiaKlattTTS"
#define NK_SERVER_MUTEX L"Local\\NokiaKlattHostMutex"
#define NK_LAUNCH_MUTEX L"Local\\NokiaKlattLaunchMutex"

constexpr uint32_t kProtocolVersion = 1;
constexpr uint32_t kMaxVoiceId = 64;
constexpr uint32_t kMaxLabel = 128;

enum Command : uint32_t {
    CMD_PING = 0,
    CMD_GET_VOICES = 1,
    CMD_SPEAK = 2,
    CMD_STOP = 3,
    CMD_SHUTDOWN = 4,
    // Drop the warm engines and re-read settings.ini. The host already
    // notices the file's timestamp per utterance; this is for the
    // configuration utility to force the point.
    CMD_RELOAD = 5,
    // Synthesise into the reply without playing: the utility's Test button.
    CMD_PREVIEW = 6,
};

enum Response : uint32_t {
    RESP_OK = 0,
    RESP_ERROR = 1,
    RESP_AUDIO = 2,
    RESP_AUDIO_END = 3,
    RESP_VOICES = 4,
    RESP_PONG = 5,
};

#pragma pack(push, 1)

struct MessageHeader {
    uint32_t type;
    uint32_t size;  // payload bytes that follow
};

struct SpeakRequest {
    char voice_id[kMaxVoiceId];  // "5320:1-male", or "custom"
    int32_t rate;                // SAPI rate, -10..10
    int32_t pitch;               // SAPI pitch, -10..10
    uint32_t volume;             // SAPI volume, 0..100
    uint32_t text_chars;         // UTF-16 code units following this struct
};

struct VoiceRecord {
    char id[kMaxVoiceId];
    uint16_t label[kMaxLabel];  // UTF-16, NUL-padded
    uint16_t lcid;
    uint8_t gender;   // 0 = male, 1 = female
    uint8_t is_custom;
    uint32_t language;          // Symbian TLanguage id
    char build[16];             // profile key
};

struct VoicesHeader {
    uint32_t version;
    uint32_t count;
};

struct AudioChunk {
    uint32_t size;  // PCM bytes that follow
};

struct AudioEnd {
    uint32_t total_bytes;
    uint32_t truncated;  // non-zero when the utterance was cancelled
};

#pragma pack(pop)

// The format the host always produces. The ROM engine has one sample rate and
// ignores every request to change it.
constexpr uint32_t kHostSampleRate = 16000;
constexpr uint16_t kHostBitsPerSample = 16;
constexpr uint16_t kHostChannels = 1;

// The id of the one voice that follows the configuration utility.
constexpr char kCustomVoiceId[] = "custom";

}  // namespace nk

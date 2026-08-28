// One emulated Nokia speech engine, driven synchronously.
//
// A CDevTTS instance speaks exactly one language: priming a second language on
// one instance leaves KErrUnknown, and constructing a second CDevTTS inside the
// same emulator faults. So an Engine is bound to a language at construction,
// and changing voice means building a new one.
//
// Audio is delivered as it is produced rather than in one lump: the framework
// hands over a buffer at a time, and speech should start before the whole
// utterance is ready.
#pragma once

#include <stdint.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "emu.h"
#include "f32.h"

namespace nk {

constexpr uint32_t kSampleRate = 16000;

// nssdevtts.dll ordinals (srsf/devtts/eabi/nssdevttsu.def)
enum DevOrdinal : uint32_t {
    DEV_SYNTHESIZE_L = 2,
    DEV_BUFFER_PROCESSED = 7,
    DEV_PRIME_SYNTHESIS_L = 9,
    DEV_NEW_L = 12,
    DEV_STOP = 13,
    DEV_ADD_STYLE_L = 17,
    DEV_IS_LANG_SUPPORTED = 26,
    DEV_NORMALIZE_SEGMENT = 37,
};

// nssttscommon.dll ordinals (srsf/ttscommon/eabi/nssttscommonu.def)
enum CommonOrdinal : uint32_t {
    SEG_SET_STYLE_ID = 1,
    SEG_SET_TEXT_PTR = 2,
    PT_ADD_SEGMENT_L = 7,
    PT_NEW_L = 11,
    PT_DELETE = 13,   // CTtsParsedText deleting destructor (D0)
    STYLE_CTOR = 16,
};

constexpr uint32_t UID_DEVTTS = 0x101FF928;
constexpr uint32_t UID_TTSCOMMON = 0x101FF927;

enum { EDevTTSSoundDeviceMode = 0, EDevTTSClientMode = 1 };

// TTtsStyle field offsets, confirmed against the constructor's own output.
// iRate and iSamplingRate default to -1, meaning "engine default" - writing a
// real number over those sentinels is not the same as leaving them alone.
// Probing every one of these against a real build showed the engine honours
// only iLanguage and iVoice: iRate, iSamplingRate, iQuality and iNlp change
// nothing, and iVolume and iDuration are rejected outright with
// KErrNotSupported. Rate, pitch and volume are therefore applied to the PCM
// afterwards; see dsp.h.
constexpr uint32_t S_LANGUAGE = 0, S_VOICE = 4, S_RATE = 112, S_VOLUME = 116;
constexpr uint32_t S_SAMPLING = 120, S_QUALITY = 124, S_DURATION = 128,
                   S_NLP = 132;

// The framework reads its audio buffer size out of nssdevtts.rsc. The stock
// 4096 bytes is 128 ms that must be generated before anything can play; 1024
// cuts the delay before speech starts by about a third and costs nothing in
// throughput.
constexpr uint32_t RSC_BUFFER_OFFSET = 0x20;
constexpr uint32_t kDefaultAudioBufferBytes = 1024;

// What the caller can still use after a failure.
enum class EngineFailure {
    kText,   // the engine refused this text; the engine is unharmed
    kFault,  // the emulator faulted or ran out of time; discard the engine
    kInit,   // the engine could not be built at all
};

struct EngineError : std::runtime_error {
    EngineFailure kind;
    EngineError(EngineFailure k, const std::string& what)
        : std::runtime_error(what), kind(k) {}
};

// Called with each buffer of 16-bit mono PCM as it is produced. Returning
// false asks the engine to stop.
using AudioSink = std::function<bool(const uint8_t* data, size_t size)>;

class Engine {
  public:
    struct Options {
        uint32_t language = 1;
        std::string voice;             // "DefaultMale", "DefaultFemale" or ""
        uint32_t buffer_bytes = kDefaultAudioBufferBytes;
        // Serve a different language's voice bank (srsf kind 2) than the one
        // the engine asks for. Zero leaves the engine's own choice alone.
        uint32_t voice_bank_override = 0;
    };

    Engine(const std::wstring& rom_path, const std::wstring& data_tree,
           const Options& options, const UcApi* api);
    ~Engine();

    uint32_t language() const { return options_.language; }
    // Whether this build honoured the requested voice name. Unset until the
    // first style is added.
    bool voice_applied() const { return voice_applied_; }
    bool voice_applied_known() const { return voice_applied_known_; }

    // Synthesise `text`, calling `sink` as audio appears. Returns the number
    // of bytes produced.
    size_t speak(const std::wstring& text, const AudioSink& sink);

    // Ask an in-progress speak() to stop as soon as possible. Safe to call
    // from another thread.
    void cancel();

    bool is_language_supported(uint32_t language);

  private:
    void patch_buffer_size(uint32_t bytes);
    uint32_t make_style(const std::string& voice);
    uint32_t add_style(const std::string& voice);
    uint32_t ensure_style();
    uint32_t make_ptrc16(const std::wstring& text);
    uint32_t make_ptrc8();
    void drain();
    size_t pump(const AudioSink& sink);
    void stop_engine();
    uint32_t build_observer();

    // host callbacks, reached from emulated code
    uint32_t on_configuration_data(const uint32_t args[4]);
    uint32_t on_event(const uint32_t args[4]);
    uint32_t on_process_buffer(const uint32_t args[4]);

    Options options_;
    std::shared_ptr<Rom> rom_;
    std::unique_ptr<Emu> emu_;
    std::unique_ptr<FileServer> fs_;

    std::vector<uint32_t> dev_eps_;
    std::vector<uint32_t> common_eps_;
    uint32_t observer_ = 0;
    uint32_t dev_ = 0;
    uint32_t style_id_ = 0;
    bool style_added_ = false;
    bool voice_applied_ = false;
    bool voice_applied_known_ = false;
    uint32_t scheduler_error_ = 0;

    // per-utterance state
    std::vector<uint8_t> pcm_;
    std::vector<uint32_t> pending_;
    bool done_ = false;
    bool abort_ = false;
    bool sink_failed_ = false;
};

}  // namespace nk

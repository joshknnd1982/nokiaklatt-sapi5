// Persistent settings, shared by the SAPI engine, the host and the
// configuration utility.
//
// Kept in an INI file under %APPDATA%\NokiaKlatt so the utility can write it
// without administrator rights and the SAPI DLL can re-read it per utterance.
// The file's last-write time is the change signal: the engine re-reads only
// when it has actually moved, so a user adjustment takes effect on the next
// utterance without costing a file open on every one.
#pragma once

#include <stdint.h>

#include <string>

namespace nk {

struct Settings {
    // ---- the Custom Voice --------------------------------------------
    // Which of the real voices the Custom Voice is built on.
    std::string custom_build = "5320";
    uint32_t custom_language = 1;
    std::string custom_voice_name = "DefaultMale";  // or DefaultFemale, or ""

    // ---- speech shaping, applied to every voice ----------------------
    // A multiplier on top of whatever rate the application asks for, so a
    // user who finds the whole engine too slow can move the middle of the
    // range instead of living at one end of the slider.
    double rate_scale = 1.0;      // 0.25 .. 4.0
    // How much one step of the application's rate control is worth, as a
    // power of two: 0.2 means five steps double the speed.
    double rate_step = 0.2;
    // Pitch shift in cents (100 cents = one semitone), and how much one step
    // of the application's pitch control is worth.
    double pitch_cents = 0.0;     // -1200 .. 1200
    double pitch_step_cents = 60.0;
    // A trim on the application's volume, in percent.
    double volume_scale = 1.0;    // 0.0 .. 2.0

    // ---- latency and shaping -----------------------------------------
    // The framework's audio buffer, in bytes. 4096 is the ROM's own value and
    // 128 ms of audio that must exist before anything plays; smaller starts
    // sooner and costs nothing in throughput.
    uint32_t buffer_bytes = 1024;
    // The ROM leaves about 80 ms of silence before the voice. That is the
    // audio device's run-up and is usually worth keeping.
    bool trim_leading_silence = false;
    uint32_t trailing_silence_ms = 0;

    // ---- engine behaviour --------------------------------------------
    // How many built engines to keep warm. Each holds one mapped ROM, and
    // building one costs a fraction of a second, so keeping the last few
    // makes switching back to a language instant.
    uint32_t engine_cache = 3;
    // Serve a different language's voice bank (srsf kind 2) than the one the
    // engine asks for. A real engine parameter, but a mismatched bank garbles
    // rather than re-timbres, so zero - the engine's own choice - is the
    // default.
    uint32_t custom_voice_bank = 0;

    bool logging = false;

    // ---- persistence --------------------------------------------------
    static std::wstring path();
    // Loads the file, falling back to defaults for anything missing.
    static Settings load();
    bool save() const;

    // Whether the file has changed since `stamp`, updating it if so. Cheap
    // enough to call per utterance.
    static bool changed_since(uint64_t* stamp);
};

// Turn an application's rate/pitch/volume into the multipliers the DSP wants.
double rate_factor(const Settings& s, int32_t sapi_rate);
double pitch_factor(const Settings& s, int32_t sapi_pitch);
double volume_factor(const Settings& s, uint32_t sapi_volume);

}  // namespace nk

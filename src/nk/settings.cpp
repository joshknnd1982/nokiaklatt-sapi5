#include "settings.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <vector>

#include "log.h"

namespace nk {
namespace {

constexpr wchar_t kSection[] = L"NokiaKlatt";

std::wstring settings_dir() {
    wchar_t* base = nullptr;
    size_t len = 0;
    if (_wdupenv_s(&base, &len, L"APPDATA") == 0 && base) {
        std::wstring dir(base);
        free(base);
        dir += L"\\NokiaKlatt";
        CreateDirectoryW(dir.c_str(), nullptr);
        return dir;
    }
    return L".";
}

int read_int(const wchar_t* key, int fallback, const std::wstring& file) {
    return GetPrivateProfileIntW(kSection, key, fallback, file.c_str());
}

double read_double(const wchar_t* key, double fallback,
                   const std::wstring& file) {
    wchar_t buf[64] = {0};
    GetPrivateProfileStringW(kSection, key, L"", buf, 64, file.c_str());
    if (!buf[0]) return fallback;
    wchar_t* end = nullptr;
    double v = wcstod(buf, &end);
    return end == buf ? fallback : v;
}

std::string read_string(const wchar_t* key, const std::string& fallback,
                        const std::wstring& file) {
    wchar_t buf[128] = {0};
    GetPrivateProfileStringW(kSection, key, L"", buf, 128, file.c_str());
    if (!buf[0]) return fallback;
    std::string out;
    for (wchar_t* p = buf; *p; ++p) out.push_back(static_cast<char>(*p));
    return out;
}

bool write_string(const wchar_t* key, const std::wstring& value,
                  const std::wstring& file) {
    return WritePrivateProfileStringW(kSection, key, value.c_str(),
                                      file.c_str()) != 0;
}

bool write_int(const wchar_t* key, long value, const std::wstring& file) {
    return write_string(key, std::to_wstring(value), file);
}

bool write_double(const wchar_t* key, double value, const std::wstring& file) {
    wchar_t buf[64];
    swprintf_s(buf, L"%.4f", value);
    return write_string(key, buf, file);
}

double clamp(double v, double lo, double hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

}  // namespace

std::wstring Settings::path() {
    return settings_dir() + L"\\settings.ini";
}

Settings Settings::load() {
    Settings s;
    std::wstring file = path();
    if (GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES) return s;

    s.custom_build = read_string(L"CustomBuild", s.custom_build, file);
    s.custom_language =
        static_cast<uint32_t>(read_int(L"CustomLanguage",
                                       static_cast<int>(s.custom_language),
                                       file));
    s.custom_voice_name =
        read_string(L"CustomVoiceName", s.custom_voice_name, file);

    s.rate_scale = clamp(read_double(L"RateScale", s.rate_scale, file), 0.25, 4.0);
    s.rate_step = clamp(read_double(L"RateStep", s.rate_step, file), 0.02, 0.5);
    s.pitch_cents =
        clamp(read_double(L"PitchCents", s.pitch_cents, file), -1200.0, 1200.0);
    s.pitch_step_cents = clamp(
        read_double(L"PitchStepCents", s.pitch_step_cents, file), 0.0, 200.0);
    s.volume_scale =
        clamp(read_double(L"VolumeScale", s.volume_scale, file), 0.0, 2.0);

    s.buffer_bytes = static_cast<uint32_t>(
        read_int(L"BufferBytes", static_cast<int>(s.buffer_bytes), file));
    if (s.buffer_bytes < 256 || s.buffer_bytes > 65536) s.buffer_bytes = 1024;
    s.trim_leading_silence = read_int(L"TrimLeadingSilence", 0, file) != 0;
    s.trailing_silence_ms = static_cast<uint32_t>(std::min(
        2000, read_int(L"TrailingSilenceMs",
                       static_cast<int>(s.trailing_silence_ms), file)));

    s.engine_cache = static_cast<uint32_t>(std::max(
        1, std::min(8, read_int(L"EngineCache",
                                static_cast<int>(s.engine_cache), file))));
    s.custom_voice_bank = static_cast<uint32_t>(
        read_int(L"CustomVoiceBank", static_cast<int>(s.custom_voice_bank),
                 file));
    s.logging = read_int(L"Logging", 0, file) != 0;
    return s;
}

bool Settings::save() const {
    std::wstring file = path();
    std::wstring build(custom_build.begin(), custom_build.end());
    std::wstring voice(custom_voice_name.begin(), custom_voice_name.end());

    bool ok = write_string(L"CustomBuild", build, file);
    ok &= write_int(L"CustomLanguage", static_cast<long>(custom_language), file);
    ok &= write_string(L"CustomVoiceName", voice, file);
    ok &= write_double(L"RateScale", rate_scale, file);
    ok &= write_double(L"RateStep", rate_step, file);
    ok &= write_double(L"PitchCents", pitch_cents, file);
    ok &= write_double(L"PitchStepCents", pitch_step_cents, file);
    ok &= write_double(L"VolumeScale", volume_scale, file);
    ok &= write_int(L"BufferBytes", static_cast<long>(buffer_bytes), file);
    ok &= write_int(L"TrimLeadingSilence", trim_leading_silence ? 1 : 0, file);
    ok &= write_int(L"TrailingSilenceMs",
                    static_cast<long>(trailing_silence_ms), file);
    ok &= write_int(L"EngineCache", static_cast<long>(engine_cache), file);
    ok &= write_int(L"CustomVoiceBank", static_cast<long>(custom_voice_bank),
                    file);
    ok &= write_int(L"Logging", logging ? 1 : 0, file);

    // WritePrivateProfileString caches; flush so another process reading the
    // file straight away sees what was just written.
    WritePrivateProfileStringW(nullptr, nullptr, nullptr, file.c_str());
    NK_LOG("settings saved to %ls (%s)", file.c_str(), ok ? "ok" : "FAILED");
    return ok;
}

bool Settings::changed_since(uint64_t* stamp) {
    WIN32_FILE_ATTRIBUTE_DATA fad;
    uint64_t now = 0;
    if (GetFileAttributesExW(path().c_str(), GetFileExInfoStandard, &fad)) {
        now = (static_cast<uint64_t>(fad.ftLastWriteTime.dwHighDateTime) << 32) |
              fad.ftLastWriteTime.dwLowDateTime;
    }
    if (now == *stamp) return false;
    *stamp = now;
    return true;
}

// ---- the application's controls, turned into multipliers ----------------

double rate_factor(const Settings& s, int32_t sapi_rate) {
    // SAPI rate runs -10..+10 with 0 as normal. Each step is worth
    // `rate_step` of a doubling, so the default of 0.2 makes +10 four times
    // normal speed and -10 a quarter of it.
    double factor = s.rate_scale * std::pow(2.0, sapi_rate * s.rate_step);
    return clamp(factor, 0.2, 8.0);
}

double pitch_factor(const Settings& s, int32_t sapi_pitch) {
    double cents = s.pitch_cents + sapi_pitch * s.pitch_step_cents;
    double factor = std::pow(2.0, clamp(cents, -1800.0, 1800.0) / 1200.0);
    return clamp(factor, 0.35, 3.0);
}

double volume_factor(const Settings& s, uint32_t sapi_volume) {
    double v = std::min<uint32_t>(sapi_volume, 100) / 100.0;
    return clamp(v * s.volume_scale, 0.0, 4.0);
}

}  // namespace nk

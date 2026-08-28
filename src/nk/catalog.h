// What voices exist, and where their files are.
//
// A phone ROM is not a voice and not a synthesizer: it is a source of voices.
// Each ROM carries its own build of the engine with its own languages, and the
// same language from two phones sounds different, so every installed build
// contributes its languages to one list and each voice names the phone it came
// from.
#pragma once

#include <stdint.h>

#include <string>
#include <vector>

namespace nk {

// A Symbian TLanguage id, and what it means to Windows.
struct LanguageInfo {
    uint32_t id;
    const wchar_t* name;    // English name, for the voice label
    const char* locale;     // ISO code, e.g. "en_GB"
    uint16_t lcid;          // what SAPI wants in the Language attribute
};

const LanguageInfo* language_info(uint32_t id);
std::wstring language_name(uint32_t id);
uint16_t language_lcid(uint32_t id);

// One build of the engine: a phone ROM plus the data tree beside it.
struct Profile {
    const char* key;          // directory name, e.g. "5320"
    const wchar_t* short_name;  // what a voice from this build is labelled with
    const wchar_t* description;
    // Languages whose packages are present and complete, and which fault the
    // emulator anyway. Offering a voice already known to be silent is not
    // worth the one utterance it costs to find out again.
    std::vector<uint32_t> blocked;
    // -1 = learn it on first use; 0/1 = this build's behaviour is known.
    int named_voices;
    const wchar_t* notes;
};

// The builds that reach speech, in the order their voices are offered when two
// of them speak the same language.
const std::vector<Profile>& profiles();
const Profile* profile(const std::string& key);

// One SAPI voice: a build, a language, and which of the build's named voices.
struct VoiceEntry {
    std::string build;        // profile key
    uint32_t language = 0;
    std::string voice_name;   // "DefaultMale", "DefaultFemale" or ""
    std::string suffix;       // "male", "female" or ""

    std::wstring rom_path;
    std::wstring data_tree;

    // Stable identity, e.g. "5320:1-male". This is what a SAPI token is keyed
    // by, so it must not change between releases.
    std::string id() const;
    // "English (UK) male (Nokia 5320)"
    std::wstring label() const;
};

// Every voice the installed ROMs offer, ordered by language name so the two
// builds that speak the same language sit next to each other.
//
// `root` is the directory holding one subdirectory per profile key. Nothing
// here starts an engine: probing every candidate would cost half a minute
// before the screen reader could say anything.
std::vector<VoiceEntry> enumerate_voices(const std::wstring& root);

// The ROM image in `directory`, chosen by content rather than extension - a
// dump may be called SYM.ROM, N95.ROM or romdumpplus.dmp - preferring the
// largest match.
std::wstring find_rom_image(const std::wstring& directory);

// The languages a build's packages promise: text-to-phoneme (srsf_0_*) and
// voice data (srsf_2_*), narrowed by the prosody package (srsf_4_*) on builds
// that ship any, without which PrimeSynthesisL leaves KErrUnknown.
std::vector<uint32_t> candidate_languages(const std::wstring& data_tree);

// The voice banks (srsf kind 2) a build has on disk. Serving one language's
// bank for another is an engine parameter of its own, though a mismatched pair
// garbles rather than re-timbres, so it is opt-in.
std::vector<uint32_t> available_voice_banks(const std::wstring& data_tree);

// Where the installed engine files live: the directory named by the
// InstallDir registry value, else the directory holding this module.
std::wstring install_root();
std::wstring roms_root();

}  // namespace nk

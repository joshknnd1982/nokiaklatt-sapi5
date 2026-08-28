#include "catalog.h"

#include <windows.h>

#include <algorithm>
#include <map>
#include <set>

#include "log.h"

namespace nk {
namespace {

// Symbian TLanguage ids. The engine identifies a voice only by this number, so
// the table is what turns "language 42" into "Bulgarian" for the voice list and
// into an LCID for SAPI's automatic language switching.
const LanguageInfo kLanguages[] = {
    {1, L"English (UK)", "en_GB", 0x0809},
    {2, L"French", "fr_FR", 0x040C},
    {3, L"German", "de_DE", 0x0407},
    {4, L"Spanish", "es_ES", 0x0C0A},
    {5, L"Italian", "it_IT", 0x0410},
    {6, L"Swedish", "sv_SE", 0x041D},
    {7, L"Danish", "da_DK", 0x0406},
    {8, L"Norwegian", "nb_NO", 0x0414},
    {9, L"Finnish", "fi_FI", 0x040B},
    {10, L"English (US)", "en_US", 0x0409},
    {11, L"French (Switzerland)", "fr_CH", 0x100C},
    {12, L"German (Switzerland)", "de_CH", 0x0807},
    {13, L"Portuguese", "pt_PT", 0x0816},
    {14, L"Turkish", "tr_TR", 0x041F},
    {15, L"Icelandic", "is_IS", 0x040F},
    {16, L"Russian", "ru_RU", 0x0419},
    {17, L"Hungarian", "hu_HU", 0x040E},
    {18, L"Dutch", "nl_NL", 0x0413},
    {19, L"Flemish", "nl_BE", 0x0813},
    {20, L"English (Australia)", "en_AU", 0x0C09},
    {21, L"French (Belgium)", "fr_BE", 0x080C},
    {22, L"German (Austria)", "de_AT", 0x0C07},
    {23, L"English (New Zealand)", "en_NZ", 0x1409},
    {24, L"French (International)", "fr", 0x040C},
    {25, L"Czech", "cs_CZ", 0x0405},
    {26, L"Slovak", "sk_SK", 0x041B},
    {27, L"Polish", "pl_PL", 0x0415},
    {28, L"Slovenian", "sl_SI", 0x0424},
    {29, L"Chinese (Taiwan)", "zh_TW", 0x0404},
    {30, L"Chinese (Hong Kong)", "zh_HK", 0x0C04},
    {31, L"Chinese (PRC)", "zh_CN", 0x0804},
    {32, L"Japanese", "ja_JP", 0x0411},
    {33, L"Thai", "th_TH", 0x041E},
    {34, L"Afrikaans", "af_ZA", 0x0436},
    {35, L"Albanian", "sq_AL", 0x041C},
    {36, L"Amharic", "am_ET", 0x045E},
    {37, L"Arabic", "ar", 0x0401},
    {38, L"Armenian", "hy_AM", 0x042B},
    {39, L"Tagalog", "tl_PH", 0x0464},
    {40, L"Belarusian", "be_BY", 0x0423},
    {41, L"Bengali", "bn_IN", 0x0445},
    {42, L"Bulgarian", "bg_BG", 0x0402},
    {43, L"Burmese", "my_MM", 0x0455},
    {44, L"Catalan", "ca_ES", 0x0403},
    {45, L"Croatian", "hr_HR", 0x041A},
    {46, L"English (Canada)", "en_CA", 0x1009},
    {47, L"English (International)", "en", 0x0409},
    {48, L"English (South Africa)", "en_ZA", 0x1C09},
    {49, L"Estonian", "et_EE", 0x0425},
    {50, L"Farsi", "fa_IR", 0x0429},
    {51, L"French (Canada)", "fr_CA", 0x0C0C},
    {52, L"Scots Gaelic", "gd_GB", 0x0491},
    {53, L"Georgian", "ka_GE", 0x0437},
    {54, L"Greek", "el_GR", 0x0408},
    {55, L"Greek (Cyprus)", "el_CY", 0x0408},
    {56, L"Gujarati", "gu_IN", 0x0447},
    {57, L"Hebrew", "he_IL", 0x040D},
    {58, L"Hindi", "hi_IN", 0x0439},
    {59, L"Indonesian", "id_ID", 0x0421},
    {60, L"Irish", "ga_IE", 0x083C},
    {61, L"Italian (Switzerland)", "it_CH", 0x0810},
    {62, L"Kannada", "kn_IN", 0x044B},
    {63, L"Kazakh", "kk_KZ", 0x043F},
    {64, L"Khmer", "km_KH", 0x0453},
    {65, L"Korean", "ko_KR", 0x0412},
    {66, L"Lao", "lo_LA", 0x0454},
    {67, L"Latvian", "lv_LV", 0x0426},
    {68, L"Lithuanian", "lt_LT", 0x0427},
    {69, L"Macedonian", "mk_MK", 0x042F},
    {70, L"Malay", "ms_MY", 0x043E},
    {71, L"Malayalam", "ml_IN", 0x044C},
    {72, L"Marathi", "mr_IN", 0x044E},
    {73, L"Moldavian", "ro_MD", 0x0818},
    {74, L"Mongolian", "mn_MN", 0x0450},
    {75, L"Norwegian Nynorsk", "nn_NO", 0x0814},
    {76, L"Portuguese (Brazil)", "pt_BR", 0x0416},
    {77, L"Punjabi", "pa_IN", 0x0446},
    {78, L"Romanian", "ro_RO", 0x0418},
    {79, L"Serbian", "sr_RS", 0x081A},
    {80, L"Sinhalese", "si_LK", 0x045B},
    {81, L"Somali", "so_SO", 0x0477},
    {82, L"Spanish (International)", "es", 0x0C0A},
    {83, L"Spanish (Latin America)", "es_419", 0x080A},
    {84, L"Swahili", "sw_KE", 0x0441},
    {85, L"Swedish (Finland)", "sv_FI", 0x081D},
    {87, L"Tamil", "ta_IN", 0x0449},
    {88, L"Telugu", "te_IN", 0x044A},
    {89, L"Tibetan", "bo_CN", 0x0451},
    {90, L"Tigrinya", "ti_ER", 0x0473},
    {91, L"Turkish (Cyprus)", "tr_CY", 0x041F},
    {92, L"Turkmen", "tk_TM", 0x0442},
    {93, L"Ukrainian", "uk_UA", 0x0422},
    {94, L"Urdu", "ur_PK", 0x0420},
    {96, L"Vietnamese", "vi_VN", 0x042A},
    {97, L"Welsh", "cy_GB", 0x0452},
    {98, L"Zulu", "zu_ZA", 0x0435},
    {129, L"English (Taiwan)", "en_TW", 0x0409},
    {157, L"English (India)", "en_IN", 0x4009},
    {158, L"English (Hong Kong)", "en_HK", 0x3C09},
    {159, L"Portuguese (Angola)", "pt_AO", 0x0816},
    {160, L"Malay (Brunei)", "ms_BN", 0x083E},
    {161, L"Chinese (Taiwan, Hong Kong)", "zh_TW", 0x0404},
    // Nokia's regional extensions, from the RM-409 Hispania pack.
    {401, L"Basque", "eu_ES", 0x042D},
    {402, L"Galician", "gl_ES", 0x0456},
};

std::wstring to_lower(std::wstring s) {
    for (auto& c : s) c = towlower(c);
    return s;
}

bool file_exists(const std::wstring& path) {
    DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES &&
           !(attr & FILE_ATTRIBUTE_DIRECTORY);
}

bool dir_exists(const std::wstring& path) {
    DWORD attr = GetFileAttributesW(path.c_str());
    return attr != INVALID_FILE_ATTRIBUTES &&
           (attr & FILE_ATTRIBUTE_DIRECTORY);
}

uint64_t file_size(const std::wstring& path) {
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad))
        return 0;
    return (static_cast<uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
}

// A cheap test that a file is an XIP ROM: it is large, and its first megabyte
// is full of E32 image headers.
bool looks_like_rom(const std::wstring& path) {
    if (file_size(path) < 4ull * 1024 * 1024) return false;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    std::vector<uint8_t> head(1u << 20);
    DWORD got = 0;
    ReadFile(h, head.data(), static_cast<DWORD>(head.size()), &got, nullptr);
    CloseHandle(h);

    int hits = 0;
    const uint32_t needle = 0x10000079;
    for (DWORD off = 0; off + 4 <= got; off += 4) {
        uint32_t v;
        memcpy(&v, head.data() + off, 4);
        if (v == needle && ++hits > 4) return true;
    }
    return false;
}

// Scan a data tree for srsf_<kind>_<language>.bin, returning kind -> languages.
std::map<uint32_t, std::set<uint32_t>> package_inventory(
    const std::wstring& tree) {
    std::map<uint32_t, std::set<uint32_t>> have;
    std::wstring data = tree + L"\\system\\data";
    if (!dir_exists(data)) {
        // An extracted dump may have kept the dump's own capitalisation.
        for (const wchar_t* alt : {L"\\System\\data", L"\\SYSTEM\\data"}) {
            if (dir_exists(tree + alt)) {
                data = tree + alt;
                break;
            }
        }
    }

    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((data + L"\\srsf_*.bin").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return have;
    do {
        std::wstring name = to_lower(fd.cFileName);
        // srsf_<kind>_<lang>.bin
        if (name.size() < 12) continue;
        std::wstring body = name.substr(5, name.size() - 9);
        size_t sep = body.find(L'_');
        if (sep == std::wstring::npos) continue;
        wchar_t* end = nullptr;
        unsigned long kind = wcstoul(body.substr(0, sep).c_str(), &end, 10);
        if (end && *end) continue;
        unsigned long lang = wcstoul(body.substr(sep + 1).c_str(), &end, 10);
        if (end && *end) continue;
        have[kind].insert(lang);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return have;
}

std::wstring module_directory() {
    HMODULE mod = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&module_directory), &mod);
    wchar_t buf[MAX_PATH] = {0};
    GetModuleFileNameW(mod, buf, MAX_PATH);
    std::wstring path(buf);
    size_t cut = path.find_last_of(L'\\');
    return cut == std::wstring::npos ? std::wstring(L".") : path.substr(0, cut);
}

std::wstring registry_install_dir() {
    for (HKEY root : {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER}) {
        for (DWORD view : {KEY_WOW64_64KEY, KEY_WOW64_32KEY}) {
            HKEY key;
            if (RegOpenKeyExW(root, L"SOFTWARE\\NokiaKlatt", 0,
                              KEY_QUERY_VALUE | view, &key) != ERROR_SUCCESS)
                continue;
            wchar_t buf[MAX_PATH] = {0};
            DWORD size = sizeof(buf), type = 0;
            LONG rc = RegQueryValueExW(key, L"InstallDir", nullptr, &type,
                                       reinterpret_cast<BYTE*>(buf), &size);
            RegCloseKey(key);
            if (rc == ERROR_SUCCESS && type == REG_SZ && buf[0]) return buf;
        }
    }
    return std::wstring();
}

}  // namespace

const LanguageInfo* language_info(uint32_t id) {
    for (const auto& info : kLanguages)
        if (info.id == id) return &info;
    return nullptr;
}

std::wstring language_name(uint32_t id) {
    const LanguageInfo* info = language_info(id);
    if (info) return info->name;
    return L"Language " + std::to_wstring(id);
}

uint16_t language_lcid(uint32_t id) {
    const LanguageInfo* info = language_info(id);
    return info ? info->lcid : 0x0409;
}

const std::vector<Profile>& profiles() {
    // The n95 is deliberately absent: it synthesises from a test harness but
    // not under a screen reader, and a voice that cannot speak is worse than
    // one that is not offered. The e5 and 5800 load their speech devices as
    // ECOM plugins, which this harness does not serve.
    static const std::vector<Profile> kProfiles = {
        {"5320", L"Nokia 5320", L"Nokia 5320 XpressMusic", {}, -1,
         L"Symbian 9.3 FP2. Native RM-409 05.16 regional data. 33 languages, "
         L"male and female."},
        {"e65", L"Nokia E65", L"Nokia E65", {}, 0,
         L"Symbian 9.1. Native EMEA data from RM-208 4.0633.74.00; its speech "
         L"modules load from ROFS. One voice per language."},
        {"n958gb", L"Nokia N95 8GB", L"Nokia N95 8GB", {}, 0,
         L"Symbian 9.2 FP1. Consistent RM-320 31.0.015 firmware with eight "
         L"regional language packs. One voice per language."},
        {"6650", L"Nokia 6650", L"Nokia 6650 Fold", {}, -1,
         L"Symbian 9.3 FP2. The Americas build, and the only source of "
         L"en-US, fr-CA, pt-BR and es-419."},
        {"n85", L"Nokia N85", L"Nokia N85", {1}, -1,
         L"Symbian 9.3 FP2. The only source of Tagalog and Vietnamese. Its "
         L"English package is complete and faults the emulator, so it is not "
         L"offered."},
    };
    return kProfiles;
}

const Profile* profile(const std::string& key) {
    for (const auto& p : profiles())
        if (key == p.key) return &p;
    return nullptr;
}

std::string VoiceEntry::id() const {
    std::string out = build + ":" + std::to_string(language);
    if (!suffix.empty()) out += "-" + suffix;
    return out;
}

std::wstring VoiceEntry::label() const {
    const Profile* p = profile(build);
    std::wstring out = language_name(language);
    if (!suffix.empty()) {
        out += L" ";
        out += (suffix == "male") ? L"male" : L"female";
    }
    out += L" (";
    out += p ? p->short_name : std::wstring(build.begin(), build.end());
    out += L")";
    return out;
}

std::wstring find_rom_image(const std::wstring& directory) {
    std::wstring best;
    uint64_t best_size = 0;
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((directory + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return best;
    do {
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;
        std::wstring name = to_lower(fd.cFileName);
        if (name.size() > 5 && name.compare(name.size() - 5, 5, L".rpkg") == 0)
            continue;
        std::wstring full = directory + L"\\" + fd.cFileName;
        uint64_t size = file_size(full);
        if (size <= best_size) continue;
        if (!looks_like_rom(full)) continue;
        best = full;
        best_size = size;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return best;
}

std::vector<uint32_t> candidate_languages(const std::wstring& data_tree) {
    auto have = package_inventory(data_tree);
    std::set<uint32_t> cand;
    auto ttp = have.find(0);
    auto voice = have.find(2);
    if (ttp == have.end() || voice == have.end()) return {};
    for (uint32_t l : ttp->second)
        if (voice->second.count(l)) cand.insert(l);

    auto prosody = have.find(4);
    if (prosody != have.end() && !prosody->second.empty()) {
        std::set<uint32_t> narrowed;
        for (uint32_t l : cand)
            if (prosody->second.count(l)) narrowed.insert(l);
        cand.swap(narrowed);
    }
    cand.erase(0);  // ELangTest, not a real voice
    return std::vector<uint32_t>(cand.begin(), cand.end());
}

std::vector<uint32_t> available_voice_banks(const std::wstring& data_tree) {
    auto have = package_inventory(data_tree);
    auto it = have.find(2);
    if (it == have.end()) return {};
    return std::vector<uint32_t>(it->second.begin(), it->second.end());
}

std::wstring install_root() {
    std::wstring dir = registry_install_dir();
    if (!dir.empty() && dir_exists(dir)) return dir;
    return module_directory();
}

std::wstring roms_root() {
    std::wstring root = install_root();
    if (dir_exists(root + L"\\roms")) return root + L"\\roms";
    // Running from the source tree, where the ROMs live under bin\roms and
    // the binaries under build\...\bin.
    for (const wchar_t* up : {L"\\..\\bin\\roms", L"\\..\\..\\bin\\roms",
                              L"\\..\\..\\..\\bin\\roms",
                              L"\\..\\..\\..\\..\\bin\\roms"}) {
        std::wstring candidate = root + up;
        if (dir_exists(candidate)) return candidate;
    }
    return root + L"\\roms";
}

std::vector<VoiceEntry> enumerate_voices(const std::wstring& root) {
    struct Row {
        std::wstring sort_name;
        VoiceEntry entry;
        int build_order;
        int voice_order;
    };
    std::vector<Row> rows;

    int build_order = 0;
    for (const Profile& p : profiles()) {
        std::wstring dir = root + L"\\" + std::wstring(p.key, p.key + strlen(p.key));
        std::wstring rom = find_rom_image(dir);
        std::wstring tree = dir + L"\\files";
        if (rom.empty() || !dir_exists(tree)) {
            NK_LOG("profile %s: no ROM or data tree under %ls", p.key,
                   dir.c_str());
            ++build_order;
            continue;
        }

        std::vector<uint32_t> langs = candidate_languages(tree);
        for (uint32_t blocked : p.blocked)
            langs.erase(std::remove(langs.begin(), langs.end(), blocked),
                        langs.end());

        // named_voices == 0 means this build is already known to ignore the
        // voice name, so male and female would be the same audio twice.
        struct NamedVoice {
            const char* suffix;
            const char* engine_name;
        };
        std::vector<NamedVoice> voices;
        if (p.named_voices == 0)
            voices = {{"", ""}};
        else
            voices = {{"male", "DefaultMale"}, {"female", "DefaultFemale"}};

        for (uint32_t lang : langs) {
            int voice_order = 0;
            for (const NamedVoice& v : voices) {
                VoiceEntry e;
                e.build = p.key;
                e.language = lang;
                e.suffix = v.suffix;
                e.voice_name = v.engine_name;
                e.rom_path = rom;
                e.data_tree = tree;
                rows.push_back({language_name(lang), e, build_order,
                                voice_order++});
            }
        }
        NK_LOG("profile %s: %zu language(s), %zu voice(s)", p.key, langs.size(),
               langs.size() * voices.size());
        ++build_order;
    }

    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) {
        if (a.sort_name != b.sort_name) return a.sort_name < b.sort_name;
        if (a.build_order != b.build_order) return a.build_order < b.build_order;
        return a.voice_order < b.voice_order;
    });

    std::vector<VoiceEntry> out;
    out.reserve(rows.size());
    for (auto& r : rows) out.push_back(std::move(r.entry));
    return out;
}

}  // namespace nk

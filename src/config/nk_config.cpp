// The Nokia Klatt configuration utility.
//
// Two things shape this dialog. Every setting takes effect on the next
// utterance, because the host re-reads the settings file per utterance, so
// there is no Apply button and nothing to restart. And every numeric setting
// is a drop-down list rather than a slider: a trackbar reports its position to
// MSAA as a percentage, so a nine-step control announces step 5 as "55" and
// gives a screen reader user no way to tell where they are.

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <mmsystem.h>

#include <string>
#include <vector>

#include "nk/catalog.h"
#include "nk/client.h"
#include "nk/log.h"
#include "nk/protocol.h"
#include "nk/settings.h"
#include "resource.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(lib, "winmm.lib")

namespace {

nk::Settings g_settings;
std::vector<nk::ClientVoice> g_voices;
nk::Client* g_client = nullptr;
std::vector<uint8_t> g_test_wav;  // kept alive while PlaySound is using it

// WM_COMMAND arrives before WM_INITDIALOG for controls created as part of the
// template, so this starts true: without it the first notifications save the
// dialog's empty state over the user's settings.
bool g_loading = true;

// ---- combo helpers ------------------------------------------------------

struct Choice {
    const wchar_t* label;
    double value;
};

void fill(HWND dialog, int id, const Choice* choices, size_t count,
          double current) {
    HWND combo = GetDlgItem(dialog, id);
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    int best = 0;
    double best_distance = 1e30;
    for (size_t i = 0; i < count; ++i) {
        int index = static_cast<int>(SendMessageW(
            combo, CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(choices[i].label)));
        SendMessageW(combo, CB_SETITEMDATA, index,
                     static_cast<LPARAM>(i));
        double distance = current - choices[i].value;
        if (distance < 0) distance = -distance;
        if (distance < best_distance) {
            best_distance = distance;
            best = index;
        }
    }
    SendMessageW(combo, CB_SETCURSEL, best, 0);
}

double chosen(HWND dialog, int id, const Choice* choices, size_t count) {
    int index = static_cast<int>(
        SendMessageW(GetDlgItem(dialog, id), CB_GETCURSEL, 0, 0));
    if (index < 0) return choices[0].value;
    auto slot = static_cast<size_t>(
        SendMessageW(GetDlgItem(dialog, id), CB_GETITEMDATA, index, 0));
    return slot < count ? choices[slot].value : choices[0].value;
}

// Speeds are named as well as numbered: "1.00x (normal)" tells a listener
// where they are without doing arithmetic.
const Choice kRates[] = {
    {L"0.50x (half speed)", 0.50},  {L"0.60x", 0.60},
    {L"0.70x", 0.70},               {L"0.80x", 0.80},
    {L"0.90x", 0.90},               {L"1.00x (normal)", 1.00},
    {L"1.10x", 1.10},               {L"1.25x", 1.25},
    {L"1.50x", 1.50},               {L"1.75x", 1.75},
    {L"2.00x (double speed)", 2.00}, {L"2.50x", 2.50},
    {L"3.00x", 3.00},               {L"3.50x", 3.50},
    {L"4.00x (quadruple speed)", 4.00},
};

const Choice kRateSteps[] = {
    {L"Very small - 20 steps to double", 0.05},
    {L"Small - 10 steps to double", 0.10},
    {L"Medium - 7 steps to double", 0.14},
    {L"Standard - 5 steps to double", 0.20},
    {L"Large - 4 steps to double", 0.25},
    {L"Very large - 3 steps to double", 0.33},
};

const Choice kPitches[] = {
    {L"-12 semitones (an octave down)", -1200.0},
    {L"-9 semitones", -900.0},
    {L"-6 semitones", -600.0},
    {L"-5 semitones", -500.0},
    {L"-4 semitones", -400.0},
    {L"-3 semitones", -300.0},
    {L"-2 semitones", -200.0},
    {L"-1 semitone", -100.0},
    {L"Normal", 0.0},
    {L"+1 semitone", 100.0},
    {L"+2 semitones", 200.0},
    {L"+3 semitones", 300.0},
    {L"+4 semitones", 400.0},
    {L"+5 semitones", 500.0},
    {L"+6 semitones", 600.0},
    {L"+9 semitones", 900.0},
    {L"+12 semitones (an octave up)", 1200.0},
};

const Choice kPitchSteps[] = {
    {L"Off - applications cannot change pitch", 0.0},
    {L"A quarter tone per step", 50.0},
    {L"Just over a semitone per step", 60.0},
    {L"A semitone per step", 100.0},
    {L"Two semitones per step", 200.0},
};

const Choice kVolumes[] = {
    {L"25%", 0.25},  {L"50%", 0.50},  {L"75%", 0.75},
    {L"100% (normal)", 1.00}, {L"125%", 1.25}, {L"150%", 1.50},
    {L"175%", 1.75}, {L"200% (may distort)", 2.00},
};

const Choice kBuffers[] = {
    {L"256 bytes - lowest latency", 256.0},
    {L"512 bytes", 512.0},
    {L"1024 bytes - recommended", 1024.0},
    {L"2048 bytes", 2048.0},
    {L"4096 bytes - the phone's own setting", 4096.0},
};

const Choice kCaches[] = {
    {L"1 - least memory", 1.0}, {L"2", 2.0},
    {L"3 - recommended", 3.0},  {L"4", 4.0},
    {L"5", 5.0},                {L"6", 6.0},
    {L"8 - fastest switching", 8.0},
};

const Choice kTrailing[] = {
    {L"None", 0.0},        {L"50 milliseconds", 50.0},
    {L"100 milliseconds", 100.0}, {L"200 milliseconds", 200.0},
    {L"400 milliseconds", 400.0}, {L"800 milliseconds", 800.0},
};

// ---- voice list ---------------------------------------------------------

// A voice id is "<build>:<language>-<suffix>"; the engine voice name follows
// from the suffix.
void parse_voice_id(const std::string& id, std::string* build,
                    uint32_t* language, std::string* voice_name) {
    size_t colon = id.find(':');
    if (colon == std::string::npos) return;
    *build = id.substr(0, colon);
    std::string rest = id.substr(colon + 1);
    size_t dash = rest.find('-');
    std::string suffix;
    if (dash == std::string::npos) {
        *language = static_cast<uint32_t>(atoi(rest.c_str()));
    } else {
        *language = static_cast<uint32_t>(atoi(rest.substr(0, dash).c_str()));
        suffix = rest.substr(dash + 1);
    }
    *voice_name = suffix == "male"     ? "DefaultMale"
                  : suffix == "female" ? "DefaultFemale"
                                       : "";
}

void set_status(HWND dialog, const std::wstring& text) {
    HWND status = GetDlgItem(dialog, IDC_STATUS);
    SetWindowTextW(status, text.c_str());
    // A screen reader is watching the focused control, not this one, so nudge
    // it: a name change on a visible static is what NVDA and Narrator pick up
    // as a live region here.
    NotifyWinEvent(EVENT_OBJECT_NAMECHANGE, status, OBJID_CLIENT,
                   CHILDID_SELF);
}

void fill_banks(HWND dialog) {
    HWND combo = GetDlgItem(dialog, IDC_BANK);
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);
    int index = static_cast<int>(SendMessageW(
        combo, CB_ADDSTRING, 0,
        reinterpret_cast<LPARAM>(L"Native - the voice the language uses")));
    SendMessageW(combo, CB_SETITEMDATA, index, 0);
    SendMessageW(combo, CB_SETCURSEL, 0, 0);

    std::wstring build(g_settings.custom_build.begin(),
                       g_settings.custom_build.end());
    std::wstring tree = nk::roms_root() + L"\\" + build + L"\\files";
    for (uint32_t bank : nk::available_voice_banks(tree)) {
        if (bank == 0) continue;
        std::wstring label = nk::language_name(bank) + L" (bank " +
                             std::to_wstring(bank) + L")";
        int at = static_cast<int>(SendMessageW(
            combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(label.c_str())));
        SendMessageW(combo, CB_SETITEMDATA, at, static_cast<LPARAM>(bank));
        if (bank == g_settings.custom_voice_bank)
            SendMessageW(combo, CB_SETCURSEL, at, 0);
    }
}

void fill_voices(HWND dialog) {
    HWND combo = GetDlgItem(dialog, IDC_VOICE);
    SendMessageW(combo, CB_RESETCONTENT, 0, 0);

    std::string want = g_settings.custom_build + ":" +
                       std::to_string(g_settings.custom_language);
    int select = 0;
    for (size_t i = 0; i < g_voices.size(); ++i) {
        const auto& v = g_voices[i];
        if (v.custom) continue;  // the Custom Voice cannot be built on itself
        int at = static_cast<int>(SendMessageW(
            combo, CB_ADDSTRING, 0,
            reinterpret_cast<LPARAM>(v.label.c_str())));
        SendMessageW(combo, CB_SETITEMDATA, at, static_cast<LPARAM>(i));

        std::string build;
        uint32_t language = 0;
        std::string voice_name;
        parse_voice_id(v.id, &build, &language, &voice_name);
        if (build == g_settings.custom_build &&
            language == g_settings.custom_language &&
            voice_name == g_settings.custom_voice_name)
            select = at;
    }
    SendMessageW(combo, CB_SETCURSEL, select, 0);
}

// ---- reading the dialog back --------------------------------------------

void harvest(HWND dialog) {
    if (g_loading) return;

    HWND voice_combo = GetDlgItem(dialog, IDC_VOICE);
    int index = static_cast<int>(SendMessageW(voice_combo, CB_GETCURSEL, 0, 0));
    if (index >= 0) {
        auto slot = static_cast<size_t>(
            SendMessageW(voice_combo, CB_GETITEMDATA, index, 0));
        if (slot < g_voices.size())
            parse_voice_id(g_voices[slot].id, &g_settings.custom_build,
                           &g_settings.custom_language,
                           &g_settings.custom_voice_name);
    }

    HWND bank_combo = GetDlgItem(dialog, IDC_BANK);
    int bank_index =
        static_cast<int>(SendMessageW(bank_combo, CB_GETCURSEL, 0, 0));
    g_settings.custom_voice_bank =
        bank_index < 0 ? 0
                       : static_cast<uint32_t>(SendMessageW(
                             bank_combo, CB_GETITEMDATA, bank_index, 0));

    g_settings.rate_scale =
        chosen(dialog, IDC_RATE, kRates, _countof(kRates));
    g_settings.rate_step =
        chosen(dialog, IDC_RATESTEP, kRateSteps, _countof(kRateSteps));
    g_settings.pitch_cents =
        chosen(dialog, IDC_PITCH, kPitches, _countof(kPitches));
    g_settings.pitch_step_cents =
        chosen(dialog, IDC_PITCHSTEP, kPitchSteps, _countof(kPitchSteps));
    g_settings.volume_scale =
        chosen(dialog, IDC_VOLUME, kVolumes, _countof(kVolumes));
    g_settings.buffer_bytes = static_cast<uint32_t>(
        chosen(dialog, IDC_BUFFER, kBuffers, _countof(kBuffers)));
    g_settings.engine_cache = static_cast<uint32_t>(
        chosen(dialog, IDC_CACHE, kCaches, _countof(kCaches)));
    g_settings.trailing_silence_ms = static_cast<uint32_t>(
        chosen(dialog, IDC_TRAILING, kTrailing, _countof(kTrailing)));

    g_settings.trim_leading_silence =
        IsDlgButtonChecked(dialog, IDC_TRIM) == BST_CHECKED;
    g_settings.logging = IsDlgButtonChecked(dialog, IDC_LOGGING) == BST_CHECKED;

    g_settings.save();
    nk::log_set_enabled(g_settings.logging);
}

void load_into(HWND dialog) {
    g_loading = true;

    fill_voices(dialog);
    fill_banks(dialog);
    fill(dialog, IDC_RATE, kRates, _countof(kRates), g_settings.rate_scale);
    fill(dialog, IDC_RATESTEP, kRateSteps, _countof(kRateSteps),
         g_settings.rate_step);
    fill(dialog, IDC_PITCH, kPitches, _countof(kPitches),
         g_settings.pitch_cents);
    fill(dialog, IDC_PITCHSTEP, kPitchSteps, _countof(kPitchSteps),
         g_settings.pitch_step_cents);
    fill(dialog, IDC_VOLUME, kVolumes, _countof(kVolumes),
         g_settings.volume_scale);
    fill(dialog, IDC_BUFFER, kBuffers, _countof(kBuffers),
         static_cast<double>(g_settings.buffer_bytes));
    fill(dialog, IDC_CACHE, kCaches, _countof(kCaches),
         static_cast<double>(g_settings.engine_cache));
    fill(dialog, IDC_TRAILING, kTrailing, _countof(kTrailing),
         static_cast<double>(g_settings.trailing_silence_ms));

    CheckDlgButton(dialog, IDC_TRIM,
                   g_settings.trim_leading_silence ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dialog, IDC_LOGGING,
                   g_settings.logging ? BST_CHECKED : BST_UNCHECKED);

    g_loading = false;
}

// ---- the test button ----------------------------------------------------

std::vector<uint8_t> wrap_wav(const std::vector<uint8_t>& pcm) {
    std::vector<uint8_t> out;
    auto push = [&out](const void* data, size_t n) {
        const auto* p = static_cast<const uint8_t*>(data);
        out.insert(out.end(), p, p + n);
    };
    uint32_t data_size = static_cast<uint32_t>(pcm.size());
    uint32_t riff = 36 + data_size;
    uint32_t rate = nk::kHostSampleRate;
    uint32_t byte_rate = rate * 2;
    uint32_t fmt_size = 16;
    uint16_t fmt = 1, channels = 1, align = 2, bits = 16;

    push("RIFF", 4);
    push(&riff, 4);
    push("WAVEfmt ", 8);
    push(&fmt_size, 4);
    push(&fmt, 2);
    push(&channels, 2);
    push(&rate, 4);
    push(&byte_rate, 4);
    push(&align, 2);
    push(&bits, 2);
    push("data", 4);
    push(&data_size, 4);
    if (data_size) push(pcm.data(), data_size);
    return out;
}

void run_test(HWND dialog) {
    harvest(dialog);

    wchar_t text[512] = {0};
    GetDlgItemTextW(dialog, IDC_TESTTEXT, text, _countof(text));
    std::wstring say = text[0]
                           ? text
                           : L"The quick brown fox jumps over the lazy dog.";

    set_status(dialog, L"Speaking...");
    HCURSOR previous = SetCursor(LoadCursorW(nullptr, IDC_WAIT));

    std::vector<uint8_t> pcm;
    std::string error;
    // The application-level rate, pitch and volume are left at neutral: this
    // is a test of the settings on this page, not of some caller's own
    // adjustments on top of them.
    bool ok = g_client->speak(nk::kCustomVoiceId, say, 0, 0, 100,
                              [&pcm](const uint8_t* data, size_t size) {
                                  pcm.insert(pcm.end(), data, data + size);
                                  return true;
                              },
                              &error);
    SetCursor(previous);

    if (!ok || pcm.empty()) {
        std::wstring message = L"The test voice could not speak.";
        if (!error.empty()) {
            message += L" ";
            message += std::wstring(error.begin(), error.end());
        }
        set_status(dialog, message);
        MessageBoxW(dialog, message.c_str(), L"Nokia Klatt",
                    MB_OK | MB_ICONWARNING);
        return;
    }

    PlaySoundW(nullptr, nullptr, 0);  // stop anything still playing
    g_test_wav = wrap_wav(pcm);
    PlaySoundW(reinterpret_cast<LPCWSTR>(g_test_wav.data()), nullptr,
               SND_MEMORY | SND_ASYNC | SND_NODEFAULT);

    wchar_t note[128];
    swprintf_s(note, L"Spoke %.1f seconds of audio.",
               pcm.size() / 2.0 / nk::kHostSampleRate);
    set_status(dialog, note);
}

void open_log_folder(HWND dialog) {
    std::wstring path = nk::log_path();
    size_t cut = path.find_last_of(L'\\');
    std::wstring folder = cut == std::wstring::npos ? path : path.substr(0, cut);
    if (folder.empty()) {
        set_status(dialog, L"There is no log folder yet.");
        return;
    }
    ShellExecuteW(dialog, L"open", folder.c_str(), nullptr, nullptr,
                  SW_SHOWNORMAL);
}

// ---- the dialog ---------------------------------------------------------

INT_PTR CALLBACK dialog_proc(HWND dialog, UINT message, WPARAM wparam,
                             LPARAM lparam) {
    switch (message) {
        case WM_INITDIALOG: {
            HICON icon = LoadIconW(GetModuleHandleW(nullptr),
                                   MAKEINTRESOURCEW(IDI_APP));
            if (icon) {
                SendMessageW(dialog, WM_SETICON, ICON_BIG,
                             reinterpret_cast<LPARAM>(icon));
                SendMessageW(dialog, WM_SETICON, ICON_SMALL,
                             reinterpret_cast<LPARAM>(icon));
            }
            SetDlgItemTextW(dialog, IDC_TESTTEXT,
                            L"The quick brown fox jumps over the lazy dog.");

            if (!g_client->get_voices(&g_voices)) {
                MessageBoxW(dialog,
                            L"The Nokia Klatt speech host could not be "
                            L"reached, so the voice list is empty. The rest "
                            L"of the settings on this page can still be "
                            L"changed.",
                            L"Nokia Klatt", MB_OK | MB_ICONWARNING);
            }
            load_into(dialog);
            set_status(dialog,
                       L"Changes take effect on the next thing spoken.");
            return TRUE;
        }

        case WM_COMMAND: {
            const int id = LOWORD(wparam);
            const int code = HIWORD(wparam);

            if (code == CBN_SELCHANGE) {
                harvest(dialog);
                if (id == IDC_VOICE) {
                    // A different build has a different set of voice banks.
                    g_loading = true;
                    g_settings.custom_voice_bank = 0;
                    fill_banks(dialog);
                    g_loading = false;
                    g_settings.save();
                }
                set_status(dialog, L"Saved. This applies to the next thing "
                                   L"spoken.");
                return TRUE;
            }
            if (code == BN_CLICKED) {
                switch (id) {
                    case IDC_TRIM:
                    case IDC_LOGGING:
                        harvest(dialog);
                        set_status(dialog, L"Saved.");
                        return TRUE;
                    case IDC_TEST:
                        run_test(dialog);
                        return TRUE;
                    case IDC_OPENLOG:
                        open_log_folder(dialog);
                        return TRUE;
                    case IDC_DEFAULTS: {
                        nk::Settings fresh;
                        // Keep the voice the user chose: "restore defaults"
                        // is about the speech settings, and silently moving
                        // someone to a different language would be a nasty
                        // surprise.
                        fresh.custom_build = g_settings.custom_build;
                        fresh.custom_language = g_settings.custom_language;
                        fresh.custom_voice_name = g_settings.custom_voice_name;
                        g_settings = fresh;
                        g_settings.save();
                        load_into(dialog);
                        set_status(dialog, L"Speech settings restored to "
                                           L"their defaults.");
                        return TRUE;
                    }
                    case IDOK:
                        harvest(dialog);
                        PlaySoundW(nullptr, nullptr, 0);
                        EndDialog(dialog, IDOK);
                        return TRUE;
                    case IDCANCEL:
                        // Everything has already been saved as it was
                        // changed, so this only closes the window. Saying so
                        // is better than pretending it undoes anything.
                        PlaySoundW(nullptr, nullptr, 0);
                        EndDialog(dialog, IDCANCEL);
                        return TRUE;
                    default:
                        break;
                }
            }
            return FALSE;
        }

        case WM_CLOSE:
            PlaySoundW(nullptr, nullptr, 0);
            EndDialog(dialog, IDCANCEL);
            return TRUE;

        default:
            return FALSE;
    }
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int) {
    nk::log_init("config");
    g_settings = nk::Settings::load();
    if (g_settings.logging) nk::log_set_enabled(true);

    INITCOMMONCONTROLSEX controls = {sizeof(controls), ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&controls);

    nk::Client client;
    g_client = &client;

    DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_CONFIG), nullptr,
                    dialog_proc, 0);

    PlaySoundW(nullptr, nullptr, 0);
    g_client = nullptr;
    return 0;
}

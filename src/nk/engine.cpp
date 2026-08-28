#include "engine.h"

#include <algorithm>

#include "e32load.h"
#include "log.h"

namespace nk {
namespace {

std::vector<uint8_t> read_whole_file(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);
    std::vector<uint8_t> out(static_cast<size_t>(size.QuadPart));
    DWORD got = 0;
    if (!out.empty())
        ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &got, nullptr);
    CloseHandle(h);
    out.resize(got);
    return out;
}

}  // namespace

Engine::Engine(const std::wstring& rom_path, const std::wstring& data_tree,
               const Options& options, const UcApi* api)
    : options_(options) {
    std::string error;
    rom_ = Rom::open(rom_path, &error);
    if (!rom_)
        throw EngineError(EngineFailure::kInit,
                          "could not map ROM: " + error);

    emu_ = std::make_unique<Emu>(rom_, api);
    fs_ = std::make_unique<FileServer>(data_tree);
    emu_->set_file_server(fs_.get());

    try {
        emu_->bootstrap();
    } catch (const std::exception& e) {
        throw EngineError(EngineFailure::kInit,
                          std::string("runtime bootstrap failed: ") + e.what());
    }

    patch_buffer_size(options_.buffer_bytes);

    // Symbian 9.1 (the E65) keeps nssdevtts.dll and its NLP helper in ROFS.
    // Load those two ordinary E32 DLLs only when the XIP ROM does not already
    // provide DevTTS, so every other profile keeps its original path.
    const RomImage* dev_img = rom_->image_by_uid3(UID_DEVTTS);
    if (dev_img) {
        dev_eps_ = rom_->exports(*dev_img);
    } else {
        try {
            const Emu::ExternalImage* ext =
                load_rofs_speech_modules(*emu_, *fs_);
            dev_eps_ = ext->exports;
        } catch (const std::exception& e) {
            throw EngineError(EngineFailure::kInit,
                              std::string("could not load DevTTS: ") + e.what());
        }
    }

    const RomImage* common_img = rom_->image_by_uid3(UID_TTSCOMMON);
    if (!common_img)
        throw EngineError(EngineFailure::kInit,
                          "this ROM has no nssttscommon.dll");
    common_eps_ = rom_->exports(*common_img);

    if (dev_eps_.size() < DEV_NORMALIZE_SEGMENT ||
        common_eps_.size() < STYLE_CTOR)
        throw EngineError(EngineFailure::kInit,
                          "the speech DLLs export fewer ordinals than this "
                          "engine needs");

    observer_ = build_observer();

    try {
        dev_ = emu_->call_l(dev_eps_[DEV_NEW_L - 1], {observer_});
    } catch (const std::exception& e) {
        std::string misses;
        for (const auto& p : fs_->probed())
            if (!p.second) misses += p.first + " ";
        throw EngineError(EngineFailure::kInit,
                          std::string("engine construction failed: ") +
                              e.what() +
                              (misses.empty() ? "" : "; missing " + misses));
    }

    // Reuse one scheduler error cell for the lifetime of this engine.
    // Allocating it inside pump() leaked one emulated heap cell per utterance.
    scheduler_error_ = emu_->alloc(4);
    if (!scheduler_error_)
        throw EngineError(EngineFailure::kInit,
                          "not enough emulated memory for the scheduler");

    NK_LOG("engine ready: language %u, voice \"%s\", buffer %u bytes",
           options_.language, options_.voice.c_str(), options_.buffer_bytes);
}

Engine::~Engine() {
    if (emu_ && scheduler_error_) {
        try {
            emu_->free(scheduler_error_);
        } catch (...) {
        }
    }
}

// Serve a copy of the framework's resource file with a smaller audio buffer,
// so the first audio arrives sooner.
void Engine::patch_buffer_size(uint32_t bytes) {
    std::wstring real = fs_->resolve("\\resource\\nssdevtts.rsc");
    if (real.empty()) return;
    std::vector<uint8_t> raw = read_whole_file(real);
    if (raw.size() < RSC_BUFFER_OFFSET + 4) return;
    memcpy(raw.data() + RSC_BUFFER_OFFSET, &bytes, 4);
    fs_->override_file(real, std::move(raw));
}

uint32_t Engine::build_observer() {
    std::map<int, std::pair<std::string, HostFn>> slots;
    slots[0] = {"MdtoConfigurationData",
                [this](const uint32_t* a) { return on_configuration_data(a); }};
    slots[1] = {"MdtoEvent", [this](const uint32_t* a) { return on_event(a); }};
    slots[2] = {"MdtoProcessBuffer",
                [this](const uint32_t* a) { return on_process_buffer(a); }};
    uint32_t vt = emu_->host_vtable(slots);
    uint32_t obj = emu_->alloc(16);
    emu_->write32(obj, vt);
    return obj;
}

// The engine asks the host for its data packages by (kind, language). Kind 0
// is text-to-phoneme, kind 2 the voice bank; the language it asks for is not
// always the one being spoken, because banks are shared between languages.
uint32_t Engine::on_configuration_data(const uint32_t args[4]) {
    uint32_t kind = args[1];
    uint32_t pid = args[2];
    uint32_t start = args[3];
    uint32_t end = emu_->stack_arg(0);

    if (kind == 2 && options_.voice_bank_override) {
        NK_LOG("voice bank %u overridden with %u", pid,
               options_.voice_bank_override);
        pid = options_.voice_bank_override;
    }

    char name[64];
    _snprintf_s(name, sizeof(name), _TRUNCATE, "\\system\\data\\srsf_%u_%u.bin",
                kind, pid);
    std::wstring path = fs_->resolve(name);
    if (path.empty()) {
        NK_LOG("package %s is not installed", name);
        return 0;
    }

    std::vector<uint8_t> blob = read_whole_file(path);
    if (end && end > start && start <= blob.size()) {
        size_t stop = std::min<size_t>(end, blob.size());
        blob.assign(blob.begin() + start, blob.begin() + stop);
    }

    uint32_t buf = emu_->alloc(static_cast<uint32_t>(blob.size()) + 8);
    if (!buf) {
        NK_LOG("no emulated memory for package %s (%zu bytes)", name,
               blob.size());
        return 0;
    }
    uint32_t len = static_cast<uint32_t>(blob.size());
    emu_->write32(buf, len);
    if (len) emu_->write(buf + 4, blob.data(), len);
    return buf;
}

uint32_t Engine::on_event(const uint32_t args[4]) {
    if (args[1] == 0) done_ = true;  // EDevTTSEventComplete
    return 0;
}

uint32_t Engine::on_process_buffer(const uint32_t args[4]) {
    std::string data = read_desc(*emu_, args[1], false);
    pcm_.insert(pcm_.end(), data.begin(), data.end());
    pending_.push_back(args[1]);
    return 0;
}

// ---- style -------------------------------------------------------------

uint32_t Engine::make_style(const std::string& voice) {
    uint32_t style = emu_->alloc(0x200);
    emu_->call(common_eps_[STYLE_CTOR - 1], {style});
    emu_->write32(style + S_LANGUAGE, options_.language);
    if (!voice.empty()) {
        // A TBuf: type 3 (EBuf) in the high nibble, then the max length.
        emu_->write32(style + S_VOICE,
                      (3u << 28) | static_cast<uint32_t>(voice.size()));
        emu_->write32(style + S_VOICE + 4, 50);
        std::wstring wide(voice.begin(), voice.end());
        emu_->write(style + S_VOICE + 8, wide.data(), wide.size() * 2);
    }
    return style;
}

uint32_t Engine::add_style(const std::string& voice) {
    return emu_->call_l(dev_eps_[DEV_ADD_STYLE_L - 1],
                        {dev_, make_style(voice)});
}

// Add the style, dropping the voice name if this build rejects it. Builds
// differ: some accept DefaultMale/DefaultFemale, others reject any named
// voice. Falling back keeps an unknown build speaking.
uint32_t Engine::ensure_style() {
    if (style_added_) return style_id_;
    if (!options_.voice.empty()) {
        try {
            style_id_ = add_style(options_.voice);
            style_added_ = true;
            voice_applied_ = true;
            voice_applied_known_ = true;
            return style_id_;
        } catch (const SymbianLeave&) {
            NK_LOG("this build rejected the voice name \"%s\"",
                   options_.voice.c_str());
            voice_applied_ = false;
            voice_applied_known_ = true;
        }
    }
    style_id_ = add_style(std::string());
    style_added_ = true;
    return style_id_;
}

// ---- descriptors -------------------------------------------------------

uint32_t Engine::make_ptrc16(const std::wstring& text) {
    size_t bytes = text.size() * 2;
    uint32_t data = emu_->alloc(static_cast<uint32_t>(std::max<size_t>(bytes, 4)));
    if (bytes) emu_->write(data, text.data(), bytes);
    uint32_t header = emu_->alloc(8);
    emu_->write32(header, (1u << 28) | static_cast<uint32_t>(text.size()));
    emu_->write32(header + 4, data);
    return header;
}

uint32_t Engine::make_ptrc8() {
    uint32_t data = emu_->alloc(4);
    uint32_t header = emu_->alloc(8);
    emu_->write32(header, 1u << 28);
    emu_->write32(header + 4, data);
    return header;
}

// ---- speech ------------------------------------------------------------

bool Engine::is_language_supported(uint32_t language) {
    return emu_->call(dev_eps_[DEV_IS_LANG_SUPPORTED - 1], {dev_, language}) != 0;
}

void Engine::cancel() {
    abort_ = true;
    // Unicorn documents emu_stop() as safe to request while emulation runs.
    // This also interrupts a runaway ROM call instead of waiting for the
    // deadline watcher.
    if (emu_) emu_->request_stop();
}

void Engine::stop_engine() {
    try {
        emu_->call(dev_eps_[DEV_STOP - 1], {dev_});
    } catch (...) {
    }
}

size_t Engine::speak(const std::wstring& raw_text, const AudioSink& sink) {
    // Trim: the engine treats a whitespace-only utterance as an error.
    size_t begin = raw_text.find_first_not_of(L" \t\r\n");
    if (begin == std::wstring::npos) return 0;
    size_t end = raw_text.find_last_not_of(L" \t\r\n");
    std::wstring text = raw_text.substr(begin, end - begin + 1);

    abort_ = false;
    sink_failed_ = false;

    std::vector<uint32_t> descriptors;
    uint32_t pt = 0, seg = 0;
    size_t produced = 0;
    bool rom_safe = true;
    std::unique_ptr<EngineError> failure;

    try {
        uint32_t sid = ensure_style();

        uint32_t txt = make_ptrc16(text);
        descriptors.push_back(txt);
        uint32_t empty8 = make_ptrc8();
        descriptors.push_back(empty8);
        uint32_t empty16 = make_ptrc16(std::wstring());
        descriptors.push_back(empty16);

        pt = emu_->call_l(common_eps_[PT_NEW_L - 1], {txt, empty8, empty16});
        seg = emu_->alloc(0x80);
        emu_->call(common_eps_[SEG_SET_STYLE_ID - 1], {seg, sid});
        emu_->call(common_eps_[SEG_SET_TEXT_PTR - 1], {seg, txt});
        emu_->call_l(common_eps_[PT_ADD_SEGMENT_L - 1], {pt, seg, 0});
        emu_->call_l(dev_eps_[DEV_PRIME_SYNTHESIS_L - 1], {dev_, pt});
        emu_->call_l(dev_eps_[DEV_SYNTHESIZE_L - 1],
                     {dev_, EDevTTSClientMode});

        produced = pump(sink);
    } catch (const SymbianLeave& e) {
        // A clean leave unwinds the Symbian call stack and leaves the engine
        // usable for the next utterance.
        failure = std::make_unique<EngineError>(
            EngineFailure::kText,
            "engine refused the text (leave " + std::to_string(e.code) + ")");
    } catch (const EmuIncomplete& e) {
        rom_safe = false;
        failure = std::make_unique<EngineError>(EngineFailure::kFault, e.what());
    } catch (const EngineError& e) {
        rom_safe = e.kind != EngineFailure::kFault;
        failure = std::make_unique<EngineError>(e);
    } catch (const std::exception& e) {
        rom_safe = false;
        failure = std::make_unique<EngineError>(
            EngineFailure::kFault,
            std::string("engine faulted: ") + e.what());
    }

    if (rom_safe) {
        // CTtsParsedText::NewL returns ownership to the caller.
        // PrimeSynthesisL mutates it but does not take ownership, so the
        // deleting destructor must run after a completed utterance or a clean
        // leave. Otherwise parsed-text internals accumulate in the emulated
        // heap.
        if (pt) {
            try {
                emu_->call(common_eps_[PT_DELETE - 1], {pt});
            } catch (...) {
            }
        }
        if (seg) emu_->free(seg);
        // make_ptrc8/16 allocate a header and a backing buffer; free both,
        // after the parsed text has been destroyed.
        for (uint32_t desc : descriptors) {
            try {
                emu_->free(emu_->read32(desc + 4));
                emu_->free(desc);
            } catch (...) {
            }
        }
    }

    if (failure) throw *failure;
    return produced;
}

void Engine::drain() {
    while (!pending_.empty()) {
        // Remove a buffer only after the ROM has accepted the
        // acknowledgement. Swallowing a fault here can otherwise turn a broken
        // scheduler into apparently successful but truncated speech.
        uint32_t buffer = pending_.front();
        emu_->call(dev_eps_[DEV_BUFFER_PROCESSED - 1], {dev_, buffer});
        pending_.erase(pending_.begin());
    }
}

// Turn the active scheduler, forwarding audio as the engine emits it.
size_t Engine::pump(const AudioSink& sink) {
    uint32_t run_if_ready = emu_->euser_export(kOrdRunIfReady);
    pcm_.clear();
    pending_.clear();
    done_ = false;
    size_t total = 0;

    auto flush = [&]() {
        if (pcm_.empty()) return;
        std::vector<uint8_t> chunk;
        chunk.swap(pcm_);
        total += chunk.size();
        if (!sink(chunk.data(), chunk.size())) {
            sink_failed_ = true;
            abort_ = true;
        }
    };

    for (;;) {
        try {
            drain();
        } catch (const std::exception& e) {
            throw EngineError(EngineFailure::kFault,
                              std::string("buffer acknowledgement failed: ") +
                                  e.what());
        }
        flush();
        if (done_ || abort_) break;

        uint32_t ran = 0;
        try {
            ran = emu_->call(run_if_ready,
                             {scheduler_error_, static_cast<uint32_t>(-100)});
        } catch (const std::exception& e) {
            if (abort_) break;   // a cancel stopped the machine on purpose
            throw EngineError(EngineFailure::kFault,
                              std::string("active scheduler failed: ") +
                                  e.what());
        }
        if (!ran && pending_.empty()) {
            if (!done_)
                throw EngineError(
                    EngineFailure::kFault,
                    "active scheduler stopped before the completion event");
            break;
        }
    }

    if (!abort_) {
        try {
            drain();
        } catch (...) {
        }
        flush();
    }

    if (abort_) {
        stop_engine();
    } else if (!done_) {
        throw EngineError(EngineFailure::kFault,
                          "synthesis ended without a completion event");
    }
    return total;
}

}  // namespace nk

// A cache of built engines, keyed by voice.
//
// Each engine maps one ROM and is bound to one language for its lifetime, so
// changing voice means building a new one. That costs a fraction of a second
// in C++ - much less than the Python original, because the ROM is mapped
// rather than copied - but it is still the difference between speech that
// starts immediately and speech that pauses first. Keeping the last few
// engines warm makes going back to a language someone actually uses free.
#pragma once

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "nk/catalog.h"
#include "nk/engine.h"
#include "nk/settings.h"

namespace nk {

class EnginePool {
  public:
    explicit EnginePool(const UcApi* api) : api_(api) {}

    // A ready engine for `voice`, built if it is not already warm. The engine
    // stays owned by the pool; `release` hands it back.
    //
    // Throws EngineError if it cannot be built.
    Engine* acquire(const VoiceEntry& voice, const Settings& settings);
    void release(Engine* engine);

    // Discard an engine whose state is no longer trustworthy: a fault leaves
    // the emulator in an unknown condition and it must not be reused.
    void discard(Engine* engine);

    // Throw everything away, e.g. when a setting that changes how engines are
    // built has moved.
    void clear();

    size_t warm_count();

  private:
    struct Entry {
        std::string key;
        std::unique_ptr<Engine> engine;
        bool in_use = false;
        uint64_t last_used = 0;
    };

    // Everything about a voice that changes how its engine is built. Two
    // requests that differ only in rate share an engine; two that differ in
    // buffer size must not.
    static std::string cache_key(const VoiceEntry& voice,
                                 const Settings& settings);
    void trim(size_t limit);

    const UcApi* api_;
    std::mutex mutex_;
    std::vector<Entry> entries_;
    uint64_t tick_ = 0;
};

}  // namespace nk

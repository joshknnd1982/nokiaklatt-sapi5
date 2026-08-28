#include "engine_pool.h"

#include <algorithm>

#include "nk/log.h"
#include "nk/protocol.h"

namespace nk {

std::string EnginePool::cache_key(const VoiceEntry& voice,
                                  const Settings& settings) {
    return voice.id() + "|b" + std::to_string(settings.buffer_bytes) + "|k" +
           std::to_string(settings.custom_voice_bank);
}

Engine* EnginePool::acquire(const VoiceEntry& voice, const Settings& settings) {
    const std::string key = cache_key(voice, settings);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        for (auto& e : entries_) {
            if (e.key == key && !e.in_use && e.engine) {
                e.in_use = true;
                e.last_used = ++tick_;
                NK_LOG("engine cache hit for %s", key.c_str());
                return e.engine.get();
            }
        }
    }

    // Built outside the lock: this takes long enough that holding the mutex
    // would serialise two applications starting different voices at once.
    Engine::Options options;
    options.language = voice.language;
    options.voice = voice.voice_name;
    options.buffer_bytes = settings.buffer_bytes;
    // The bank override belongs to the Custom Voice; the catalogued voices are
    // what the ROM says they are.
    if (voice.id() == kCustomVoiceId)
        options.voice_bank_override = settings.custom_voice_bank;

    LARGE_INTEGER freq, t0, t1;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&t0);
    auto engine = std::make_unique<Engine>(voice.rom_path, voice.data_tree,
                                           options, api_);
    QueryPerformanceCounter(&t1);
    NK_LOG("built engine %s in %.3fs", key.c_str(),
           double(t1.QuadPart - t0.QuadPart) / freq.QuadPart);

    std::lock_guard<std::mutex> lock(mutex_);
    Entry entry;
    entry.key = key;
    entry.engine = std::move(engine);
    entry.in_use = true;
    entry.last_used = ++tick_;
    Engine* raw = entry.engine.get();
    entries_.push_back(std::move(entry));
    trim(std::max<size_t>(1, settings.engine_cache));
    return raw;
}

void EnginePool::release(Engine* engine) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& e : entries_) {
        if (e.engine.get() == engine) {
            e.in_use = false;
            e.last_used = ++tick_;
            return;
        }
    }
}

void EnginePool::discard(Engine* engine) {
    std::lock_guard<std::mutex> lock(mutex_);
    for (size_t i = 0; i < entries_.size(); ++i) {
        if (entries_[i].engine.get() == engine) {
            NK_LOG("discarding engine %s after a fault",
                   entries_[i].key.c_str());
            entries_.erase(entries_.begin() + i);
            return;
        }
    }
}

void EnginePool::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    // Anything still speaking is left alone; it is removed when released.
    entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                  [](const Entry& e) { return !e.in_use; }),
                   entries_.end());
    NK_LOG("engine cache cleared, %zu still in use", entries_.size());
}

size_t EnginePool::warm_count() {
    std::lock_guard<std::mutex> lock(mutex_);
    return entries_.size();
}

// Called with the lock held. Evicts the least recently used idle engines.
void EnginePool::trim(size_t limit) {
    for (;;) {
        size_t idle = 0;
        for (const auto& e : entries_)
            if (!e.in_use) ++idle;
        if (entries_.size() <= limit || idle == 0) return;

        auto oldest = entries_.end();
        for (auto it = entries_.begin(); it != entries_.end(); ++it) {
            if (it->in_use) continue;
            if (oldest == entries_.end() || it->last_used < oldest->last_used)
                oldest = it;
        }
        if (oldest == entries_.end()) return;
        NK_LOG("evicting engine %s", oldest->key.c_str());
        entries_.erase(oldest);
    }
}

}  // namespace nk

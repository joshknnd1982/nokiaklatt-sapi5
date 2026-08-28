#include "dsp.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace nk {
namespace {

constexpr size_t FRAME = 480;      // 30 ms at 16 kHz: longer than a pitch period
constexpr size_t OVERLAP = FRAME / 2;
constexpr size_t SEARCH = 64;      // +/- samples of similarity search
constexpr size_t SEARCH_STEP = 4;
constexpr size_t CORR_LEN = 96;

inline int16_t clamp16(int32_t v) {
    return static_cast<int16_t>(v < -32768 ? -32768 : (v > 32767 ? 32767 : v));
}

}  // namespace

void apply_gain(int16_t* samples, size_t count, double gain) {
    if (gain == 1.0) return;
    for (size_t i = 0; i < count; ++i)
        samples[i] = clamp16(static_cast<int32_t>(
            std::lround(static_cast<double>(samples[i]) * gain)));
}

size_t first_audible(const int16_t* samples, size_t count, int16_t threshold) {
    for (size_t i = 0; i < count; ++i)
        if (samples[i] > threshold || samples[i] < -threshold) return i;
    return count;
}

// ---- Stretcher ---------------------------------------------------------

Stretcher::Stretcher(double factor)
    : factor_(factor), passthrough_(std::fabs(factor - 1.0) < 0.02) {}

void Stretcher::reset() {
    buf_.clear();
    pos_ = 0.0;
    tail_.clear();
    have_tail_ = false;
}

// Where near `ideal` the signal best continues the previous frame's tail.
size_t Stretcher::best_offset(size_t ideal) const {
    size_t best = ideal;
    int64_t best_score = 0;
    bool have_score = false;

    size_t lo = ideal > SEARCH ? ideal - SEARCH : 0;
    size_t hi = buf_.size() >= CORR_LEN
                    ? std::min(buf_.size() - CORR_LEN, ideal + SEARCH)
                    : 0;
    for (size_t cand = lo; cand <= hi; cand += SEARCH_STEP) {
        int64_t score = 0;
        for (size_t k = 0; k < CORR_LEN; k += 2)
            score += static_cast<int64_t>(tail_[k]) * buf_[cand + k];
        if (!have_score || score > best_score) {
            have_score = true;
            best_score = score;
            best = cand;
        }
    }
    return best;
}

std::vector<int16_t> Stretcher::feed(const int16_t* samples, size_t count) {
    if (passthrough_) return std::vector<int16_t>(samples, samples + count);
    if (count) buf_.insert(buf_.end(), samples, samples + count);

    std::vector<int16_t> out;
    // Off the end of an utterance there is nothing left to search, but in the
    // middle a frame is only ready once the whole search window has arrived -
    // otherwise the match is made against whatever merely turned up last.
    const size_t margin = FRAME + SEARCH + CORR_LEN;
    for (;;) {
        size_t ideal = static_cast<size_t>(pos_);
        if (ideal + margin > buf_.size()) break;
        size_t read = have_tail_ ? best_offset(ideal) : ideal;
        if (read + FRAME > buf_.size()) break;

        if (!have_tail_) {
            out.insert(out.end(), buf_.begin() + read,
                       buf_.begin() + read + OVERLAP);
        } else {
            for (size_t k = 0; k < OVERLAP; ++k) {
                int32_t mixed = (static_cast<int32_t>(tail_[k]) *
                                     static_cast<int32_t>(OVERLAP - k) +
                                 static_cast<int32_t>(buf_[read + k]) *
                                     static_cast<int32_t>(k)) /
                                static_cast<int32_t>(OVERLAP);
                out.push_back(clamp16(mixed));
            }
        }
        tail_.assign(buf_.begin() + read + OVERLAP, buf_.begin() + read + FRAME);
        have_tail_ = true;
        pos_ += OVERLAP * factor_;

        // Drop input the analysis has moved past, keeping the window it may
        // still look back into.
        int64_t keep = static_cast<int64_t>(pos_) -
                       static_cast<int64_t>(SEARCH) - 1;
        if (keep > 0 && static_cast<size_t>(keep) <= buf_.size()) {
            buf_.erase(buf_.begin(), buf_.begin() + static_cast<size_t>(keep));
            pos_ -= static_cast<double>(keep);
        }
    }
    return out;
}

std::vector<int16_t> Stretcher::flush() {
    if (passthrough_) return {};
    std::vector<int16_t> out;
    // A final pass with no lookahead margin, then whatever tail is left.
    for (;;) {
        size_t ideal = static_cast<size_t>(pos_);
        if (ideal + FRAME > buf_.size()) break;
        size_t read = ideal;
        if (!have_tail_) {
            out.insert(out.end(), buf_.begin() + read,
                       buf_.begin() + read + OVERLAP);
        } else {
            for (size_t k = 0; k < OVERLAP; ++k) {
                int32_t mixed = (static_cast<int32_t>(tail_[k]) *
                                     static_cast<int32_t>(OVERLAP - k) +
                                 static_cast<int32_t>(buf_[read + k]) *
                                     static_cast<int32_t>(k)) /
                                static_cast<int32_t>(OVERLAP);
                out.push_back(clamp16(mixed));
            }
        }
        tail_.assign(buf_.begin() + read + OVERLAP, buf_.begin() + read + FRAME);
        have_tail_ = true;
        pos_ += OVERLAP * factor_;
    }
    if (have_tail_) out.insert(out.end(), tail_.begin(), tail_.end());
    reset();
    return out;
}

// ---- Resampler ---------------------------------------------------------

Resampler::Resampler(double ratio)
    : ratio_(ratio), passthrough_(std::fabs(ratio - 1.0) < 0.001) {}

void Resampler::reset() {
    buf_.clear();
    pos_ = 0.0;
}

std::vector<int16_t> Resampler::feed(const int16_t* samples, size_t count) {
    if (passthrough_) return std::vector<int16_t>(samples, samples + count);
    if (count) buf_.insert(buf_.end(), samples, samples + count);

    std::vector<int16_t> out;
    // One sample of lookahead, so the interpolation always has both ends.
    while (static_cast<size_t>(pos_) + 1 < buf_.size()) {
        size_t i = static_cast<size_t>(pos_);
        double frac = pos_ - static_cast<double>(i);
        double value = buf_[i] * (1.0 - frac) + buf_[i + 1] * frac;
        out.push_back(clamp16(static_cast<int32_t>(std::lround(value))));
        pos_ += ratio_;
    }

    size_t keep = static_cast<size_t>(pos_);
    if (keep > 0) {
        keep = std::min(keep, buf_.size());
        buf_.erase(buf_.begin(), buf_.begin() + keep);
        pos_ -= static_cast<double>(keep);
    }
    return out;
}

std::vector<int16_t> Resampler::flush() {
    if (passthrough_) return {};
    std::vector<int16_t> out;
    while (static_cast<size_t>(pos_) < buf_.size()) {
        size_t i = static_cast<size_t>(pos_);
        double frac = pos_ - static_cast<double>(i);
        double next = (i + 1 < buf_.size()) ? buf_[i + 1] : buf_[i];
        double value = buf_[i] * (1.0 - frac) + next * frac;
        out.push_back(clamp16(static_cast<int32_t>(std::lround(value))));
        pos_ += ratio_;
    }
    reset();
    return out;
}

// ---- OutputChain -------------------------------------------------------

OutputChain::OutputChain(const Settings& settings)
    : settings_(settings),
      // Stretch to rate/pitch, then let the resampler put the duration back
      // and carry the pitch with it.
      stretcher_(settings.rate / (settings.pitch > 0 ? settings.pitch : 1.0)),
      resampler_(settings.pitch > 0 ? settings.pitch : 1.0) {}

std::vector<uint8_t> OutputChain::finish(std::vector<int16_t> samples,
                                         bool final) {
    std::vector<int16_t> resampled =
        resampler_.feed(samples.data(), samples.size());
    if (final) {
        std::vector<int16_t> tail = resampler_.flush();
        resampled.insert(resampled.end(), tail.begin(), tail.end());
    }

    int16_t* begin = resampled.data();
    size_t count = resampled.size();
    if (settings_.trim_leading_silence && !started_ && count) {
        size_t skip = first_audible(begin, count, 64);
        if (skip >= count) {
            // Still nothing but silence; hold the decision for the next
            // buffer rather than emitting it.
            return {};
        }
        begin += skip;
        count -= skip;
        started_ = true;
    } else if (count) {
        started_ = true;
    }

    apply_gain(begin, count, settings_.volume);

    std::vector<uint8_t> out(count * 2);
    if (count) memcpy(out.data(), begin, count * 2);

    if (final && settings_.trailing_silence_ms) {
        size_t extra = static_cast<size_t>(16000ull *
                                           settings_.trailing_silence_ms / 1000) *
                       2;
        out.insert(out.end(), extra, 0);
    }
    return out;
}

std::vector<uint8_t> OutputChain::feed(const uint8_t* data, size_t size) {
    size_t count = size / 2;
    std::vector<int16_t> in(count);
    if (count) memcpy(in.data(), data, count * 2);
    return finish(stretcher_.feed(in.data(), in.size()), false);
}

std::vector<uint8_t> OutputChain::flush() {
    return finish(stretcher_.flush(), true);
}

}  // namespace nk

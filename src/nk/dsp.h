// Rate, pitch and volume, applied to the PCM after synthesis.
//
// Probing every TTtsStyle field against a real build showed the engine honours
// only iLanguage and iVoice: it ignores iRate, iSamplingRate, iQuality and
// iNlp outright, and rejects any iVolume but 100 and any iDuration at all with
// KErrNotSupported. So none of these can be passed through to it, and all
// three are done here.
//
// Everything streams: an utterance arrives a buffer at a time and speech
// should start before the whole thing is ready, so each stage carries its own
// state from one feed to the next. Scaling each buffer independently is not
// the same as scaling the utterance - it leaves a seam and the wrong duration.
#pragma once

#include <stdint.h>

#include <vector>

namespace nk {

// Time scaling by overlap-add with a similarity search (WSOLA), which changes
// speed without changing pitch.
class Stretcher {
  public:
    // `factor` > 1 speaks faster.
    explicit Stretcher(double factor);

    bool passthrough() const { return passthrough_; }
    void reset();

    // Feed 16-bit mono PCM; returns whatever is ready.
    std::vector<int16_t> feed(const int16_t* samples, size_t count);
    // The last of the audio, and a fresh start.
    std::vector<int16_t> flush();

  private:
    size_t best_offset(size_t ideal) const;

    double factor_;
    bool passthrough_;
    std::vector<int16_t> buf_;
    double pos_ = 0.0;        // analysis position, in samples
    std::vector<int16_t> tail_;  // previous frame's second half
    bool have_tail_ = false;
};

// Linear resampling, which changes pitch and duration together. Paired with
// the stretcher above it becomes a pitch shift at constant duration.
class Resampler {
  public:
    // `ratio` > 1 consumes more input per output sample: higher pitch, shorter
    // output.
    explicit Resampler(double ratio);

    bool passthrough() const { return passthrough_; }
    void reset();

    std::vector<int16_t> feed(const int16_t* samples, size_t count);
    std::vector<int16_t> flush();

  private:
    double ratio_;
    bool passthrough_;
    std::vector<int16_t> buf_;
    double pos_ = 0.0;
};

// The whole output chain for one utterance.
//
// To shift pitch by `pitch` while ending at speed `rate`, the signal is
// stretched to speed rate/pitch and then resampled by pitch: the resampling
// puts the duration back and takes the pitch with it.
class OutputChain {
  public:
    struct Settings {
        double rate = 1.0;    // > 1 speaks faster
        double pitch = 1.0;   // > 1 sounds higher
        double volume = 1.0;  // 0..1 (and above, if the user wants gain)
        // Samples of silence to keep at the start of an utterance. The ROM
        // leaves about 80 ms there, which is the audio device's run-up and is
        // worth keeping after an idle period; trimming it can clip the first
        // phoneme onto an opening stream.
        bool trim_leading_silence = false;
        // Extra silence appended to every utterance, in milliseconds.
        uint32_t trailing_silence_ms = 0;
    };

    explicit OutputChain(const Settings& settings);

    // Feed raw engine PCM (bytes, 16-bit mono little-endian).
    std::vector<uint8_t> feed(const uint8_t* data, size_t size);
    std::vector<uint8_t> flush();

    // How many bytes of output this many bytes of input will roughly become,
    // used to place index events.
    double output_scale() const { return 1.0 / settings_.rate; }

  private:
    std::vector<uint8_t> finish(std::vector<int16_t> samples, bool final);

    Settings settings_;
    Stretcher stretcher_;
    Resampler resampler_;
    bool started_ = false;   // leading silence dealt with
};

// Scale 16-bit mono PCM in place. `gain` is a linear multiplier.
void apply_gain(int16_t* samples, size_t count, double gain);

// Where the audible signal starts, in samples, for leading-silence trimming.
size_t first_audible(const int16_t* samples, size_t count, int16_t threshold);

}  // namespace nk

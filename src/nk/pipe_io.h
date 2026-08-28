// Reading and writing one pipe from two threads at once.
//
// The host has to read a stop while it is still writing the utterance that
// stop interrupts. On a synchronous handle that is impossible: Windows
// serialises I/O per file object, so a thread parked in ReadFile holds off
// every WriteFile on the same handle, and the two sides sit waiting for each
// other until something times out.
//
// So both ends open the pipe with FILE_FLAG_OVERLAPPED and go through this,
// which gives each direction its own event and turns each overlapped
// operation back into a blocking call. Reads and writes then proceed
// independently; the caller still serialises multiple writers itself.
#pragma once

#include <windows.h>
#include <stdint.h>

namespace nk {

class PipeChannel {
  public:
    PipeChannel() = default;
    explicit PipeChannel(HANDLE pipe);
    ~PipeChannel();

    PipeChannel(const PipeChannel&) = delete;
    PipeChannel& operator=(const PipeChannel&) = delete;

    void reset(HANDLE pipe);
    HANDLE handle() const { return pipe_; }
    bool valid() const { return pipe_ && pipe_ != INVALID_HANDLE_VALUE; }

    bool read_all(void* data, size_t size);
    bool write_all(const void* data, size_t size);

    // Abandon anything in flight, so a blocked reader returns instead of
    // waiting for a peer that is never going to answer.
    void cancel();

    // Closes the handle as well as the events.
    void close();

  private:
    void ensure_events();

    HANDLE pipe_ = INVALID_HANDLE_VALUE;
    HANDLE read_event_ = nullptr;
    HANDLE write_event_ = nullptr;
};

// ConnectNamedPipe on an overlapped handle, waited to completion.
bool connect_overlapped(HANDLE pipe);

}  // namespace nk

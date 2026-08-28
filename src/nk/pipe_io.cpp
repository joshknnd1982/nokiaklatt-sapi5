#include "pipe_io.h"

namespace nk {

PipeChannel::PipeChannel(HANDLE pipe) { reset(pipe); }

PipeChannel::~PipeChannel() { close(); }

void PipeChannel::ensure_events() {
    // Manual-reset, initially unsignalled: GetOverlappedResult waits on these
    // and each direction needs its own, or two operations in flight would
    // wake each other.
    if (!read_event_) read_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!write_event_) write_event_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);
}

void PipeChannel::reset(HANDLE pipe) {
    pipe_ = pipe;
    ensure_events();
}

void PipeChannel::close() {
    if (pipe_ && pipe_ != INVALID_HANDLE_VALUE) {
        CloseHandle(pipe_);
        pipe_ = INVALID_HANDLE_VALUE;
    }
    if (read_event_) {
        CloseHandle(read_event_);
        read_event_ = nullptr;
    }
    if (write_event_) {
        CloseHandle(write_event_);
        write_event_ = nullptr;
    }
}

void PipeChannel::cancel() {
    if (valid()) CancelIoEx(pipe_, nullptr);
}

bool PipeChannel::read_all(void* data, size_t size) {
    if (!valid()) return false;
    ensure_events();
    auto* p = static_cast<uint8_t*>(data);

    while (size) {
        OVERLAPPED overlapped = {};
        overlapped.hEvent = read_event_;
        ResetEvent(read_event_);

        DWORD got = 0;
        DWORD want = size > 0x10000000u ? 0x10000000u
                                        : static_cast<DWORD>(size);
        if (!ReadFile(pipe_, p, want, &got, &overlapped)) {
            if (GetLastError() != ERROR_IO_PENDING) return false;
            if (!GetOverlappedResult(pipe_, &overlapped, &got, TRUE))
                return false;
        }
        if (got == 0) return false;
        p += got;
        size -= got;
    }
    return true;
}

bool PipeChannel::write_all(const void* data, size_t size) {
    if (!valid()) return false;
    ensure_events();
    const auto* p = static_cast<const uint8_t*>(data);

    while (size) {
        OVERLAPPED overlapped = {};
        overlapped.hEvent = write_event_;
        ResetEvent(write_event_);

        DWORD written = 0;
        DWORD want = size > 0x10000000u ? 0x10000000u
                                        : static_cast<DWORD>(size);
        if (!WriteFile(pipe_, p, want, &written, &overlapped)) {
            if (GetLastError() != ERROR_IO_PENDING) return false;
            if (!GetOverlappedResult(pipe_, &overlapped, &written, TRUE))
                return false;
        }
        if (written == 0) return false;
        p += written;
        size -= written;
    }
    return true;
}

bool connect_overlapped(HANDLE pipe) {
    HANDLE event = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    if (!event) return false;

    OVERLAPPED overlapped = {};
    overlapped.hEvent = event;

    bool ok = false;
    if (ConnectNamedPipe(pipe, &overlapped)) {
        ok = true;
    } else {
        DWORD error = GetLastError();
        if (error == ERROR_PIPE_CONNECTED) {
            // The client got in between CreateNamedPipe and this call, which
            // is a success rather than a race to retry.
            ok = true;
        } else if (error == ERROR_IO_PENDING) {
            DWORD transferred = 0;
            ok = GetOverlappedResult(pipe, &overlapped, &transferred, TRUE) != 0;
        }
    }
    CloseHandle(event);
    return ok;
}

}  // namespace nk

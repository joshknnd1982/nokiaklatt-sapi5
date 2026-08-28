// A minimal EPOC/EKA2 kernel surface for running Symbian ROM code under
// Unicorn.
//
// Everything above the kernel - euser, the C++ runtime, the speech engine - is
// real ROM code executing under emulation. This supplies only what sits below
// the SVC boundary, then bootstraps the runtime the way process startup does:
// build the cleanup stack and the active scheduler by calling euser's own
// exports.
#pragma once

#include <stdint.h>

#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

#include "rom.h"
#include "uc_api.h"

namespace nk {

class FileServer;

// Address-space layout. The ROM goes at its own base; everything else is
// somewhere the ROM will never be.
constexpr uint32_t kStackBase = 0x60000000;
constexpr uint32_t kStackSize = 0x100000;
constexpr uint32_t kHeapBase = 0x50000000;
constexpr uint32_t kHeapSize = 0x100000;
constexpr uint32_t kRetMagic = 0x7FFF0000;
constexpr uint32_t kVtBase = 0x51000000;   // fake RAllocator vtable + object
constexpr uint32_t kTrapBase = 0x52000000; // one word per host-implemented virtual
constexpr uint32_t kTrapSize = 0x10000;
constexpr uint32_t kPoolBase = 0x53000000; // bytes handed out by the allocator
constexpr uint32_t kPoolSize = 0x800000;   // ~30x what an utterance needs
constexpr uint32_t kExtBase = 0x54000000;  // RAM-loaded ROFS code
constexpr uint32_t kExtSize = 0x00400000;

// euser export ordinals (kernel/eka/eabi/euseru.def)
constexpr uint32_t kOrdTrapCleanupNew = 196;
constexpr uint32_t kOrdCleanupPrevLevel = 1265;
constexpr uint32_t kOrdCleanupPopN = 1268;
constexpr uint32_t kOrdCleanupNextLevel = 1278;
constexpr uint32_t kOrdSchedulerAdd = 424;
constexpr uint32_t kOrdSchedulerCurrent = 427;
constexpr uint32_t kOrdSchedulerInstall = 428;
constexpr uint32_t kOrdSchedulerCtor = 430;
constexpr uint32_t kOrdRunIfReady = 422;
constexpr uint32_t kEuserUid3 = 0x100039e5;

// A clean Symbian leave. The engine is unharmed and can take the next
// utterance.
struct SymbianLeave : std::runtime_error {
    int32_t code;
    explicit SymbianLeave(int32_t c)
        : std::runtime_error("Symbian leave " + std::to_string(c)), code(c) {}
};

// The emulator stopped without the call reaching its return address, and not
// because of a leave or a panic - so it ran out of time or wandered off. Raised
// loudly because a truncated call is indistinguishable from a successful one by
// return value alone.
struct EmuIncomplete : std::runtime_error {
    using std::runtime_error::runtime_error;
};

struct EmuFault : std::runtime_error {
    using std::runtime_error::runtime_error;
};

// A host function reachable from emulated code through a Thumb SVC.
// `args` holds r0..r3; the return value goes to r0.
using HostFn = std::function<uint32_t(const uint32_t args[4])>;

class Emu {
  public:
    Emu(std::shared_ptr<Rom> rom, const UcApi* api);
    ~Emu();

    // Create the cleanup stack and active scheduler using euser's own code.
    void bootstrap();

    // ---- memory ---------------------------------------------------
    uint32_t alloc(uint32_t size);
    void free(uint32_t address);
    uint32_t realloc(uint32_t address, uint32_t size);
    uint32_t alloc_size(uint32_t address) const;

    void read(uint32_t addr, void* out, size_t n) const;
    void write(uint32_t addr, const void* in, size_t n);
    uint32_t read32(uint32_t addr) const;
    void write32(uint32_t addr, uint32_t value);

    // The n-th argument passed on the stack, for host callbacks whose
    // signature runs past r0-r3.
    uint32_t stack_arg(int index) const;

    // ---- calling into the ROM -------------------------------------
    // Runs a call and refuses to let a truncated one pass as a result.
    uint32_t call(uint32_t addr, std::initializer_list<uint32_t> args = {});
    // Calls a leaving function inside its own TRAP level.
    uint32_t call_l(uint32_t addr, std::initializer_list<uint32_t> args = {});

    // Ask an in-progress call to stop as soon as possible. Safe from another
    // thread: Unicorn documents emu_stop() that way, and it is what makes a
    // cancel take milliseconds instead of the rest of the utterance.
    void request_stop();

    // ---- symbols --------------------------------------------------
    uint32_t euser_export(uint32_t ordinal) const;
    const RomImage* image_by_uid3(uint32_t uid3) const;
    std::vector<uint32_t> image_exports(const RomImage& img) const;

    // ---- host callbacks -------------------------------------------
    // Builds a C++ vtable whose slots call back into host code. EABI expects
    // offset-to-top and typeinfo words in front of the first slot.
    uint32_t host_vtable(const std::map<int, std::pair<std::string, HostFn>>& slots);

    // ---- wiring ---------------------------------------------------
    void set_file_server(FileServer* fs) { fs_ = fs; }
    FileServer* file_server() const { return fs_; }

    uc_engine* uc() const { return uc_; }
    const UcApi& api() const { return *api_; }
    Rom& rom() const { return *rom_; }

    // External (ROFS) images loaded into the RAM code area.
    struct ExternalImage {
        std::string name;
        uint32_t uid3 = 0;
        uint32_t code_addr = 0;
        uint32_t code_size = 0;
        std::vector<uint32_t> exports;
    };
    std::vector<ExternalImage>& external_images() { return external_; }
    const ExternalImage* external_by_uid3(uint32_t uid3) const;
    const ExternalImage* external_by_name(const std::string& lower_name) const;
    uint32_t external_alloc(uint32_t size);   // page-aligned code area
    bool external_region_mapped = false;

    // Timeout for one ROM call, in microseconds.
    uint64_t timeout_us = 60ull * 1000 * 1000;

    // Set while a leave is pending, cleared by call_l.
    bool has_pending_leave() const { return pending_leave_; }
    int32_t pending_leave_code() const { return pending_leave_code_; }
    bool has_pending_panic() const { return pending_panic_; }

  private:
    friend void svc_hook(uc_engine*, uint32_t, void*);
    friend bool fault_hook(uc_engine*, int, uint64_t, int, int64_t, void*);

    void map_memory();
    void install_allocator();
    void on_svc();
    void on_trap(uint32_t trap_addr);
    uint32_t exec_call(uint32_t n, bool fast);
    int32_t session_send();

    uint32_t reg(int id) const;
    void set_reg(int id, uint32_t value);

    // user-side thread data: heap, active scheduler, trap handler
    uint32_t td(uint32_t off) const;
    void set_td(uint32_t off, uint32_t value);
    uint32_t scheduler() const;
    uint32_t trap_handler() const;

    uint32_t cleanup_object();
    uint32_t cleanup_depth();
    void enter_trap();
    void leave_trap(uint32_t pushed);

    std::shared_ptr<Rom> rom_;
    const UcApi* api_ = nullptr;
    uc_engine* uc_ = nullptr;
    uc_hook intr_hook_ = 0, fault_hook_ = 0;

    FileServer* fs_ = nullptr;

    // synthetic heap
    struct FreeCell {
        uint32_t address, size;
    };
    uint32_t pool_next_ = kPoolBase + 16;
    std::map<uint32_t, uint32_t> sizes_;   // address -> cell size
    std::vector<FreeCell> free_cells_;
    void merge_free_cells();

    uint32_t allocator_ = 0;
    uint32_t thread_data_ = 0;
    uint32_t trap_next_ = kTrapBase + 0x200;
    std::map<uint32_t, std::pair<std::string, HostFn>> host_fns_;

    std::map<uint32_t, uint32_t> tls_;
    std::map<uint32_t, std::string> sessions_;
    uint32_t next_session_ = 0x40;
    std::map<uint64_t, int> unknown_execs_;

    const RomImage* euser_ = nullptr;
    std::vector<uint32_t> euser_exports_;

    bool pending_leave_ = false;
    int32_t pending_leave_code_ = 0;
    bool pending_panic_ = false;
    int32_t pending_panic_code_ = 0;
    bool truncated_ = false;
    std::atomic<bool> stop_requested_{false};

    std::vector<ExternalImage> external_;
    uint32_t external_next_ = kExtBase;

    // The deadline watcher. Unicorn's own `timeout=` costs a whole scheduler
    // tick per call on Windows - about 17 ms against 35 us without it, which
    // on a six-second utterance is four seconds spent asleep. One thread per
    // emulator checking a deadline twice a second stops the same runaway for
    // nothing per call.
    std::atomic<uint64_t> deadline_{0};
    std::atomic<bool> watcher_alive_{true};
    HANDLE watcher_ = nullptr;
    void start_watcher();
    void stop_watcher();
    static DWORD WINAPI watcher_main(void* self);
};

}  // namespace nk

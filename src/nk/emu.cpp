#include "emu.h"

#include <algorithm>

#include "f32.h"
#include "log.h"

namespace nk {
namespace {

// EKA2 keeps the user-side thread data - heap, active scheduler, trap handler -
// in one block. Most 9.3 ROMs reach it through fast executive calls, but some
// builds inline the access as a read of the CP15 user-RW thread id register.
// Backing both routes with the same block makes the two styles agree.
constexpr uint32_t TD_HEAP = 0, TD_SCHEDULER = 4, TD_TRAP = 8;

// RAllocator / MAllocator virtual order (e32cmn.h)
const char* const kVirtuals[] = {"Alloc",     "Free",      "ReAlloc",
                                 "AllocLen",  "Compress",  "Reset",
                                 "AllocSize", "Available", "DebugFunction",
                                 "Extension_"};
constexpr int kVirtualCount = 10;

// Fast executive calls, Symbian 9.3 numbering (cross-checked against EKA2L1)
constexpr uint32_t FAST_HEAP = 0x01;
constexpr uint32_t FAST_ACTIVE_SCHEDULER = 0x05;
constexpr uint32_t FAST_SET_ACTIVE_SCHEDULER = 0x06;
constexpr uint32_t FAST_TRAP_HANDLER = 0x08;
constexpr uint32_t FAST_SET_TRAP_HANDLER = 0x09;

// Slow executive calls
constexpr uint32_t SLOW_DLL_TLS = 0x4D;
constexpr uint32_t SLOW_SESSION_SEND_SYNC = 0x4C;
constexpr uint32_t SLOW_DLL_SET_TLS = 0x75;
constexpr uint32_t SLOW_DLL_FREE_TLS = 0x76;
constexpr uint32_t SLOW_THREAD_KILL = 0x72;
constexpr uint32_t SLOW_SESSION_CREATE = 0x7E;
constexpr uint32_t SLOW_LEAVE_START = 0xDE;

// svc #0 in Thumb: the host-callback trampoline.
const uint8_t kThumbSvc[2] = {0x00, 0xDF};

// Symbian 9.1's euser is built as ARM, and its exec stubs read
// `mov ip, lr; svc #n` with nothing after them: the kernel is expected to
// return to the caller. The 9.3 Thumb build writes `svc #n; bx lr` and returns
// itself, so resuming after the SVC is right there and wrong on 9.1 - it walks
// into the next stub in the table.
constexpr uint32_t MOV_IP_LR = 0xE1A0C00E;

inline int32_t as_signed(uint32_t v) {
    int32_t out;
    memcpy(&out, &v, 4);
    return out;
}

uint64_t now_us() {
    LARGE_INTEGER freq, counter;
    QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&counter);
    return static_cast<uint64_t>(counter.QuadPart * 1000000ull /
                                 freq.QuadPart);
}

}  // namespace

// ---- hook trampolines ------------------------------------------------

void svc_hook(uc_engine*, uint32_t, void* user) {
    static_cast<Emu*>(user)->on_svc();
}

bool fault_hook(uc_engine* uc, int access, uint64_t address, int, int64_t,
                void* user) {
    auto* self = static_cast<Emu*>(user);
    uint32_t pc = self->reg(UC_ARM_REG_PC);
    NK_LOG("unmapped memory access %d at %#llx from pc %#x", access,
           static_cast<unsigned long long>(address), pc);
    return false;  // let the emulation stop
}

// ---- construction ----------------------------------------------------

Emu::Emu(std::shared_ptr<Rom> rom, const UcApi* api)
    : rom_(std::move(rom)), api_(api) {
    int err = api_->open(UC_ARCH_ARM, UC_MODE_ARM, &uc_);
    if (err != UC_ERR_OK)
        throw EmuFault(std::string("uc_open failed: ") + api_->strerror(err));

    // Symbian user code expects the VFP unit enabled. Without this the English
    // path through DFPAEABI.dll dies with UC_ERR_INSN_INVALID.
    uint32_t fpexc = 0x40000000;
    api_->reg_write(uc_, UC_ARM_REG_FPEXC, &fpexc);

    map_memory();
    install_allocator();

    api_->hook_add(uc_, &intr_hook_, UC_HOOK_INTR,
                   reinterpret_cast<void*>(&svc_hook), this, 1, 0);
    api_->hook_add(uc_, &fault_hook_, UC_HOOK_MEM_UNMAPPED,
                   reinterpret_cast<void*>(&fault_hook), this, 1, 0);

    // More than one image can carry euser's UID3 - stubs and variants - so the
    // real one is the image that actually exports the whole API.
    euser_ = rom_->image_by_uid3(kEuserUid3);
    if (euser_) euser_exports_ = rom_->exports(*euser_);
    NK_LOG("euser: %s, %zu exports", euser_ ? "found" : "MISSING",
           euser_exports_.size());

    start_watcher();
}

Emu::~Emu() {
    stop_watcher();
    if (uc_ && api_) api_->close(uc_);
    uc_ = nullptr;
}

void Emu::map_memory() {
    // The ROM is mapped straight from the file view. Every emulator for the
    // same phone therefore shares one set of physical pages, and building an
    // engine no longer costs a 70 MB copy.
    size_t rom_size = (rom_->size() + 0xFFFFF) & ~static_cast<size_t>(0xFFFFF);
    int err = api_->mem_map_ptr(uc_, rom_->base(), rom_size,
                                UC_PROT_READ | UC_PROT_EXEC,
                                const_cast<uint8_t*>(rom_->data()));
    if (err != UC_ERR_OK) {
        // A view shorter than the rounded-up size cannot be mapped by pointer;
        // fall back to an owned copy, which is what the Python harness always
        // did.
        NK_LOG("mem_map_ptr failed (%s), copying the ROM instead",
               api_->strerror(err));
        err = api_->mem_map(uc_, rom_->base(), rom_size, UC_PROT_ALL);
        if (err != UC_ERR_OK)
            throw EmuFault(std::string("could not map ROM: ") +
                           api_->strerror(err));
        api_->mem_write(uc_, rom_->base(), rom_->data(), rom_->size());
    }

    api_->mem_map(uc_, kStackBase, kStackSize, UC_PROT_ALL);
    api_->mem_map(uc_, kHeapBase, kHeapSize, UC_PROT_ALL);
    api_->mem_map(uc_, kRetMagic & ~0xFFFu, 0x1000, UC_PROT_ALL);
    api_->mem_map(uc_, kVtBase, 0x1000, UC_PROT_ALL);
    api_->mem_map(uc_, kTrapBase, kTrapSize, UC_PROT_ALL);
    api_->mem_map(uc_, kPoolBase, kPoolSize, UC_PROT_ALL);
}

void Emu::install_allocator() {
    // An EABI vtable has offset-to-top and typeinfo in front. Entries are odd
    // so a blx into the trap keeps the CPU in Thumb state.
    uint32_t vt = kVtBase + 0x40;
    uint32_t zero[2] = {0, 0};
    write(vt - 8, zero, 8);
    for (int i = 0; i < kVirtualCount; ++i) {
        write32(vt + 4 * i, kTrapBase + 4 * i + 1);
        write(kTrapBase + 4 * i, kThumbSvc, 2);
    }
    allocator_ = kVtBase + 0x100;
    write32(allocator_, vt);
    std::vector<uint8_t> zeros(0x40, 0);
    write(allocator_ + 4, zeros.data(), zeros.size());

    thread_data_ = alloc(0x40);
    write32(thread_data_ + TD_HEAP, allocator_);
    write32(thread_data_ + TD_SCHEDULER, 0);
    write32(thread_data_ + TD_TRAP, 0);

    // TPIDRURW, written through the coprocessor interface: the plain
    // UC_ARM_REG_C13_C0_2 id is a no-op in current Unicorn.
    UcRegCP cp{};
    cp.cp = 15;
    cp.is64 = 0;
    cp.sec = 0;
    cp.crn = 13;
    cp.crm = 0;
    cp.opc1 = 0;
    cp.opc2 = 2;
    cp.val = thread_data_;
    api_->reg_write(uc_, UC_ARM_REG_CP_REG, &cp);
}

// ---- registers and memory --------------------------------------------

uint32_t Emu::reg(int id) const {
    uint32_t v = 0;
    api_->reg_read(uc_, id, &v);
    return v;
}

void Emu::set_reg(int id, uint32_t value) {
    api_->reg_write(uc_, id, &value);
}

void Emu::read(uint32_t addr, void* out, size_t n) const {
    if (n == 0) return;
    if (api_->mem_read(uc_, addr, out, n) != UC_ERR_OK)
        throw EmuFault("read from unmapped address " + std::to_string(addr));
}

void Emu::write(uint32_t addr, const void* in, size_t n) {
    if (n == 0) return;
    if (api_->mem_write(uc_, addr, in, n) != UC_ERR_OK)
        throw EmuFault("write to unmapped address " + std::to_string(addr));
}

uint32_t Emu::read32(uint32_t addr) const {
    uint32_t v = 0;
    read(addr, &v, 4);
    return v;
}

void Emu::write32(uint32_t addr, uint32_t value) { write(addr, &value, 4); }

uint32_t Emu::stack_arg(int index) const {
    return read32(reg(UC_ARM_REG_SP) + 4u * static_cast<uint32_t>(index));
}

uint32_t Emu::td(uint32_t off) const {
    return thread_data_ ? read32(thread_data_ + off) : 0;
}

void Emu::set_td(uint32_t off, uint32_t value) {
    if (thread_data_) write32(thread_data_ + off, value);
}

uint32_t Emu::scheduler() const { return td(TD_SCHEDULER); }
uint32_t Emu::trap_handler() const { return td(TD_TRAP); }

// ---- the synthetic heap ----------------------------------------------

namespace {
inline uint32_t heap_cell_size(uint32_t size) {
    return ((size < 4 ? 4 : size) + 15) & ~15u;
}
}  // namespace

void Emu::merge_free_cells() {
    if (free_cells_.empty()) return;
    std::sort(free_cells_.begin(), free_cells_.end(),
              [](const FreeCell& a, const FreeCell& b) {
                  return a.address < b.address;
              });
    std::vector<FreeCell> merged;
    merged.push_back(free_cells_.front());
    for (size_t i = 1; i < free_cells_.size(); ++i) {
        FreeCell& prev = merged.back();
        const FreeCell& cur = free_cells_[i];
        if (prev.address + prev.size == cur.address)
            prev.size += cur.size;
        else
            merged.push_back(cur);
    }
    free_cells_.swap(merged);
}

uint32_t Emu::alloc(uint32_t size) {
    // RHeap::Alloc(0) still returns a minimum-sized valid cell.
    if (size > kPoolSize) return 0;
    uint32_t need = heap_cell_size(size);

    uint32_t address = 0;
    for (size_t i = 0; i < free_cells_.size(); ++i) {
        if (free_cells_[i].size < need) continue;
        address = free_cells_[i].address;
        if (free_cells_[i].size == need) {
            free_cells_.erase(free_cells_.begin() + i);
        } else {
            free_cells_[i].address += need;
            free_cells_[i].size -= need;
        }
        break;
    }
    if (!address) {
        if (pool_next_ + need > kPoolBase + kPoolSize) {
            NK_LOG("emulated heap exhausted asking for %u bytes", size);
            return 0;
        }
        address = pool_next_;
        pool_next_ += need;
    }

    std::vector<uint8_t> zeros(need, 0);
    write(address, zeros.data(), need);
    sizes_[address] = need;
    return address;
}

void Emu::free(uint32_t address) {
    if (!address) return;
    auto it = sizes_.find(address);
    if (it == sizes_.end()) return;
    free_cells_.push_back({address, it->second});
    sizes_.erase(it);
    merge_free_cells();
}

uint32_t Emu::alloc_size(uint32_t address) const {
    auto it = sizes_.find(address);
    return it == sizes_.end() ? 0 : it->second;
}

uint32_t Emu::realloc(uint32_t address, uint32_t size) {
    if (!address) return alloc(size);
    if (size > kPoolSize) return 0;
    if (size == 0) {
        free(address);
        return 0;
    }
    auto it = sizes_.find(address);
    if (it == sizes_.end()) return 0;
    uint32_t old_size = it->second;
    uint32_t need = heap_cell_size(size);
    if (need <= old_size) {
        it->second = need;
        if (old_size > need) {
            free_cells_.push_back({address + need, old_size - need});
            merge_free_cells();
        }
        return address;
    }
    uint32_t fresh = alloc(size);
    if (!fresh) return 0;
    std::vector<uint8_t> buf(std::min(old_size, size));
    read(address, buf.data(), buf.size());
    write(fresh, buf.data(), buf.size());
    free(address);
    return fresh;
}

uint32_t Emu::external_alloc(uint32_t size) {
    constexpr uint32_t kPage = 0x1000;
    if (!external_region_mapped) {
        api_->mem_map(uc_, kExtBase, kExtSize, UC_PROT_ALL);
        external_region_mapped = true;
    }
    uint32_t run = (external_next_ + kPage - 1) & ~(kPage - 1);
    uint32_t end = run + ((size + kPage - 1) & ~(kPage - 1));
    if (end > kExtBase + kExtSize) return 0;
    external_next_ = end;
    return run;
}

// ---- host callbacks ---------------------------------------------------

uint32_t Emu::host_vtable(
    const std::map<int, std::pair<std::string, HostFn>>& slots) {
    int n = slots.empty() ? 0 : slots.rbegin()->first + 1;
    uint32_t vt = trap_next_ + 8;
    uint32_t code = vt + 4 * n + 16;
    uint32_t zero[2] = {0, 0};
    write(vt - 8, zero, 8);
    for (int i = 0; i < n; ++i) {
        uint32_t slot = code + 4 * i;
        auto it = slots.find(i);
        host_fns_[slot] = it != slots.end()
                              ? it->second
                              : std::make_pair(std::string("slot") +
                                                   std::to_string(i),
                                               HostFn());
        write(slot, kThumbSvc, 2);
        write32(vt + 4 * i, slot + 1);   // odd: stay in Thumb
    }
    trap_next_ = code + 4 * n + 16;
    return vt;
}

// ---- the SVC boundary -------------------------------------------------

void Emu::on_svc() {
    uint32_t pc = reg(UC_ARM_REG_PC);
    if (pc >= kTrapBase + 2 && pc - 2 < kTrapBase + kTrapSize) {
        on_trap(pc - 2);
        return;
    }
    uint32_t insn = 0;
    if (api_->mem_read(uc_, pc - 4, &insn, 4) != UC_ERR_OK) return;
    uint32_t num = insn & 0x00FFFFFF;
    uint32_t ret = exec_call(num & 0x7FFFFF, (num & 0x800000) != 0);
    set_reg(UC_ARM_REG_R0, ret);

    // On a 9.1 ARM stub the kernel is expected to return to the caller.
    uint32_t prev = 0;
    if (api_->mem_read(uc_, pc - 8, &prev, 4) == UC_ERR_OK && prev == MOV_IP_LR)
        set_reg(UC_ARM_REG_PC, reg(UC_ARM_REG_LR));
}

void Emu::on_trap(uint32_t trap_addr) {
    uint32_t args[4] = {reg(UC_ARM_REG_R0), reg(UC_ARM_REG_R1),
                        reg(UC_ARM_REG_R2), reg(UC_ARM_REG_R3)};

    auto host = host_fns_.find(trap_addr);
    if (host != host_fns_.end()) {
        uint32_t ret = host->second.second ? host->second.second(args) : 0;
        set_reg(UC_ARM_REG_R0, ret);
        // Unicorn takes ARM/Thumb state from bit 0 of a PC write, so hand it
        // LR whole - masking bit 0 drops Thumb callers into ARM state.
        set_reg(UC_ARM_REG_PC, reg(UC_ARM_REG_LR));
        return;
    }

    uint32_t idx = (trap_addr - kTrapBase) / 4;
    uint32_t ret = 0;
    if (idx < kVirtualCount) {
        const char* name = kVirtuals[idx];
        if (!strcmp(name, "Alloc")) {
            ret = alloc(args[1]);
        } else if (!strcmp(name, "Free")) {
            free(args[1]);
        } else if (!strcmp(name, "ReAlloc")) {
            ret = realloc(args[1], args[2]);
        } else if (!strcmp(name, "AllocLen")) {
            ret = alloc_size(args[1]);
        }
    }
    set_reg(UC_ARM_REG_R0, ret);
    set_reg(UC_ARM_REG_PC, reg(UC_ARM_REG_LR));
}

uint32_t Emu::exec_call(uint32_t n, bool fast) {
    if (fast) {
        switch (n) {
            case FAST_HEAP:
                return allocator_;
            case FAST_TRAP_HANDLER:
                return trap_handler();
            case FAST_SET_TRAP_HANDLER: {
                uint32_t prev = trap_handler();
                set_td(TD_TRAP, reg(UC_ARM_REG_R0));
                return prev;
            }
            case FAST_ACTIVE_SCHEDULER:
                return scheduler();
            case FAST_SET_ACTIVE_SCHEDULER: {
                uint32_t prev = scheduler();
                set_td(TD_SCHEDULER, reg(UC_ARM_REG_R0));
                return prev;
            }
            default:
                break;
        }
    } else {
        switch (n) {
            case SLOW_DLL_TLS: {
                auto it = tls_.find(reg(UC_ARM_REG_R0));
                return it == tls_.end() ? 0 : it->second;
            }
            case SLOW_DLL_SET_TLS:
                tls_[reg(UC_ARM_REG_R0)] = reg(UC_ARM_REG_R2);
                return 0;
            case SLOW_DLL_FREE_TLS:
                tls_.erase(reg(UC_ARM_REG_R0));
                return 0;
            case SLOW_THREAD_KILL:
                // User::Panic lands here; stop rather than run on through
                // wreckage.
                pending_panic_ = true;
                pending_panic_code_ = as_signed(reg(UC_ARM_REG_R2));
                NK_LOG("*** PANIC reason=%d", pending_panic_code_);
                api_->emu_stop(uc_);
                return 0;
            case SLOW_LEAVE_START: {
                // EExecLeaveStart only announces the leave; the unwind is done
                // by euser's own trap handler afterwards. Not stopping here
                // lets the ROM catch leaves it means to catch -
                // CTTSAlgorithmManager::ConstructL deliberately TRAPs the NLP
                // device - and only an uncaught leave reaches us, as a panic.
                uint32_t regs[7] = {
                    reg(UC_ARM_REG_R0), reg(UC_ARM_REG_R1), reg(UC_ARM_REG_R2),
                    reg(UC_ARM_REG_R3), reg(UC_ARM_REG_R4), reg(UC_ARM_REG_R5),
                    reg(UC_ARM_REG_R6)};
                // The leave code is a small negative int in one of the low
                // registers.
                int32_t code = as_signed(regs[0]);
                for (uint32_t r : regs) {
                    int32_t c = as_signed(r);
                    if (c > -60 && c < 0) {
                        code = c;
                        break;
                    }
                }
                pending_leave_ = true;
                pending_leave_code_ = code;
                NK_LOG("*** User::Leave %d", code);
                return 0;
            }
            case SLOW_SESSION_CREATE: {
                std::string name = read_desc(*this, reg(UC_ARM_REG_R0), false);
                uint32_t h = next_session_++;
                sessions_[h] = name;
                NK_LOG("connect to \"%s\" -> handle %#x", name.c_str(), h);
                return h;
            }
            case SLOW_SESSION_SEND_SYNC:
                return static_cast<uint32_t>(session_send());
            default:
                break;
        }
    }
    uint64_t key = (static_cast<uint64_t>(fast) << 32) | n;
    if (++unknown_execs_[key] < 4)
        NK_LOG("exec %s #%#x stubbed to 0", fast ? "fast" : "slow", n);
    return 0;
}

int32_t Emu::session_send() {
    uint32_t handle = reg(UC_ARM_REG_R0);
    int32_t fn = as_signed(reg(UC_ARM_REG_R1));
    uint32_t argp = reg(UC_ARM_REG_R2);
    uint32_t status = reg(UC_ARM_REG_R3);

    int32_t args[4] = {0, 0, 0, 0};
    if (argp) read(argp, args, 16);

    std::string name;
    auto it = sessions_.find(handle);
    if (it != sessions_.end()) {
        name = it->second;
    } else if (!sessions_.empty()) {
        // Subsession sends arrive on a handle the client derived from the
        // session, not one we issued. Every session opened so far is the file
        // server, so route by that rather than by handle.
        bool uniform = true;
        const std::string& first = sessions_.begin()->second;
        for (const auto& kv : sessions_)
            if (kv.second != first) uniform = false;
        if (uniform) name = first;
    }

    int32_t result = 0;
    if (fs_ && name == "!FileServer") result = fs_->send(*this, fn, args);

    if (status) {
        // Asynchronous form: the caller then spins in User::WaitForRequest
        // until TRequestStatus::iStatus stops reading KRequestPending.
        write(status, &result, 4);
        return 0;
    }
    return result;
}

// ---- calling ----------------------------------------------------------

uint32_t Emu::call(uint32_t addr, std::initializer_list<uint32_t> args) {
    static const int kArgRegs[4] = {UC_ARM_REG_R0, UC_ARM_REG_R1,
                                    UC_ARM_REG_R2, UC_ARM_REG_R3};
    size_t i = 0;
    for (uint32_t a : args) {
        if (i >= 4) break;
        set_reg(kArgRegs[i++], a);
    }

    uint32_t sp = kStackBase + kStackSize - 0x1000;
    if (args.size() > 4) {
        // AAPCS: the fifth argument onward goes on the stack.
        size_t extra = args.size() - 4;
        sp -= static_cast<uint32_t>((extra * 4 + 7) & ~7ull);
        size_t k = 0;
        for (uint32_t a : args) {
            if (k >= 4) write32(sp + 4 * static_cast<uint32_t>(k - 4), a);
            ++k;
        }
    }
    set_reg(UC_ARM_REG_SP, sp);
    set_reg(UC_ARM_REG_LR, kRetMagic);

    stop_requested_ = false;
    deadline_ = now_us() + timeout_us;
    // emu_start(count=N) makes Unicorn install a per-instruction hook to
    // decrement the counter, which costs about twenty times the throughput.
    // Calls are bounded by the watcher thread instead.
    int err = api_->emu_start(uc_, addr | (addr & 1), kRetMagic, 0, 0);
    deadline_ = 0;

    truncated_ = reg(UC_ARM_REG_PC) != kRetMagic;
    if (err != UC_ERR_OK && !pending_leave_ && !pending_panic_ &&
        !stop_requested_) {
        NK_LOG("emu_start error at pc=%#x: %s", reg(UC_ARM_REG_PC),
               api_->strerror(err));
    }
    if (truncated_ && !pending_leave_ && !pending_panic_ && !stop_requested_) {
        throw EmuIncomplete("call to " + std::to_string(addr) +
                            " stopped at pc=" +
                            std::to_string(reg(UC_ARM_REG_PC)) +
                            " without returning");
    }
    return reg(UC_ARM_REG_R0);
}

void Emu::request_stop() {
    stop_requested_ = true;
    if (uc_ && api_) api_->emu_stop(uc_);
}

// TCleanupTrapHandler::iCleanup sits right after the vptr.
uint32_t Emu::cleanup_object() { return read32(trap_handler() + 4); }

uint32_t Emu::cleanup_depth() {
    // CCleanup: vptr, TCleanupStackItem* iBase, iTop, iNext
    uint32_t c = cleanup_object();
    uint32_t base = read32(c + 4);
    uint32_t next = read32(c + 12);
    return (next - base) / 8;
}

// Opening a cleanup-stack level: calling a NewL outside one panics with
// E32USER-CBase 66 (EClnPushAtLevelZero).
void Emu::enter_trap() {
    call(euser_export(kOrdCleanupNextLevel), {cleanup_object()});
}

// Anything the callee left on the stack has to be popped first, or
// PreviousLevel panics with EClnLevelNotEmpty (71).
void Emu::leave_trap(uint32_t pushed) {
    if (pushed > 0)
        call(euser_export(kOrdCleanupPopN), {cleanup_object(), pushed});
    call(euser_export(kOrdCleanupPrevLevel), {cleanup_object()});
}

uint32_t Emu::call_l(uint32_t addr, std::initializer_list<uint32_t> args) {
    pending_leave_ = false;
    enter_trap();
    uint32_t before = cleanup_depth();
    uint32_t result = 0;
    try {
        result = call(addr, args);
    } catch (...) {
        uint32_t depth = 0;
        try {
            depth = cleanup_depth();
        } catch (...) {
        }
        try {
            leave_trap(depth > before ? depth - before : 0);
        } catch (...) {
        }
        throw;
    }
    uint32_t depth = cleanup_depth();
    leave_trap(depth > before ? depth - before : 0);
    if (pending_leave_) {
        pending_leave_ = false;
        throw SymbianLeave(pending_leave_code_);
    }
    return result;
}

// ---- symbols ----------------------------------------------------------

uint32_t Emu::euser_export(uint32_t ordinal) const {
    if (ordinal == 0 || ordinal > euser_exports_.size())
        throw EmuFault("euser ordinal " + std::to_string(ordinal) +
                       " is out of range");
    return euser_exports_[ordinal - 1];
}

const RomImage* Emu::image_by_uid3(uint32_t uid3) const {
    return rom_->image_by_uid3(uid3);
}

// A RAM-loaded ROFS image, when one has been supplied for this UID3. Callers
// ask for this first: profiles whose speech DLLs are all in XIP never load any
// and fall straight through to the ROM.
const Emu::ExternalImage* Emu::external_by_uid3(uint32_t uid3) const {
    for (const auto& ext : external_)
        if (ext.uid3 == uid3) return &ext;
    return nullptr;
}

const Emu::ExternalImage* Emu::external_by_name(
    const std::string& lower_name) const {
    for (const auto& ext : external_)
        if (ext.name == lower_name) return &ext;
    return nullptr;
}

std::vector<uint32_t> Emu::image_exports(const RomImage& img) const {
    return rom_->exports(img);
}

// ---- runtime bootstrap -------------------------------------------------

void Emu::bootstrap() {
    call(euser_export(kOrdTrapCleanupNew));
    uint32_t sched = alloc(0x40);  // sizeof(CActiveScheduler)
    call(euser_export(kOrdSchedulerCtor), {sched});
    call(euser_export(kOrdSchedulerInstall), {sched});
    call(euser_export(kOrdSchedulerCurrent));
    NK_LOG("runtime bootstrapped: scheduler %#x", sched);
}

// ---- the deadline watcher ----------------------------------------------

void Emu::start_watcher() {
    watcher_ = CreateThread(nullptr, 64 * 1024, &Emu::watcher_main, this, 0,
                            nullptr);
}

void Emu::stop_watcher() {
    watcher_alive_ = false;
    if (watcher_) {
        WaitForSingleObject(watcher_, 2000);
        CloseHandle(watcher_);
        watcher_ = nullptr;
    }
}

DWORD WINAPI Emu::watcher_main(void* param) {
    auto* self = static_cast<Emu*>(param);
    while (self->watcher_alive_) {
        Sleep(250);
        uint64_t due = self->deadline_;
        if (due && now_us() > due) {
            self->deadline_ = 0;
            NK_LOG("call exceeded its deadline; stopping the emulator");
            if (self->uc_ && self->api_) self->api_->emu_stop(self->uc_);
        }
    }
    return 0;
}

}  // namespace nk

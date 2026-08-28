// Unicorn, bound at run time.
//
// The engine ships one vendored unicorn.dll and no headers or import library,
// so the API is declared here and resolved with GetProcAddress. That also
// keeps the DLL's location a run-time decision: it sits next to the host
// executable in an installed copy and under bin/ in the source tree.
#pragma once

#include <windows.h>
#include <stdint.h>

namespace nk {

typedef void uc_engine;
typedef size_t uc_hook;

enum : int {
    UC_ERR_OK = 0,
    UC_ARCH_ARM = 1,
    UC_MODE_ARM = 0,
    UC_MODE_LITTLE_ENDIAN = 0,
    UC_HOOK_INTR = 1,
    UC_HOOK_MEM_UNMAPPED = 112,
    UC_PROT_NONE = 0,
    UC_PROT_READ = 1,
    UC_PROT_WRITE = 2,
    UC_PROT_EXEC = 4,
    UC_PROT_ALL = 7,
};

// ARM register ids, from arm_const.py of the vendored bindings.
enum : int {
    UC_ARM_REG_CPSR = 3,
    UC_ARM_REG_FPEXC = 4,
    UC_ARM_REG_LR = 10,
    UC_ARM_REG_PC = 11,
    UC_ARM_REG_SP = 12,
    UC_ARM_REG_R0 = 66,
    UC_ARM_REG_R1 = 67,
    UC_ARM_REG_R2 = 68,
    UC_ARM_REG_R3 = 69,
    UC_ARM_REG_R4 = 70,
    UC_ARM_REG_R5 = 71,
    UC_ARM_REG_R6 = 72,
    UC_ARM_REG_CP_REG = 139,
};

// The struct uc_reg_write expects for UC_ARM_REG_CP_REG.
#pragma pack(push, 4)
struct UcRegCP {
    uint32_t cp, is64, sec, crn, crm, opc1, opc2;
    uint64_t val;
};
#pragma pack(pop)

typedef void (*uc_cb_intr_t)(uc_engine*, uint32_t intno, void* user);
typedef bool (*uc_cb_eventmem_t)(uc_engine*, int type, uint64_t address,
                                 int size, int64_t value, void* user);

// The subset of the C API this port uses.
struct UcApi {
    int (*open)(int arch, int mode, uc_engine** uc);
    int (*close)(uc_engine* uc);
    int (*mem_map)(uc_engine*, uint64_t address, size_t size, uint32_t perms);
    int (*mem_map_ptr)(uc_engine*, uint64_t address, size_t size,
                       uint32_t perms, void* ptr);
    int (*mem_unmap)(uc_engine*, uint64_t address, size_t size);
    int (*mem_read)(uc_engine*, uint64_t address, void* bytes, size_t size);
    int (*mem_write)(uc_engine*, uint64_t address, const void* bytes,
                     size_t size);
    int (*reg_read)(uc_engine*, int regid, void* value);
    int (*reg_write)(uc_engine*, int regid, const void* value);
    int (*emu_start)(uc_engine*, uint64_t begin, uint64_t until,
                     uint64_t timeout, size_t count);
    int (*emu_stop)(uc_engine*);
    int (*hook_add)(uc_engine*, uc_hook* hh, int type, void* callback,
                    void* user_data, uint64_t begin, uint64_t end, ...);
    int (*hook_del)(uc_engine*, uc_hook hh);
    const char* (*strerror)(int code);
    unsigned int (*version)(unsigned int* major, unsigned int* minor);
};

// Resolves unicorn.dll once per process. `hint_dir`, when given, is searched
// before the ordinary DLL search order. Returns null and fills `error` if the
// library or any required export is missing.
const UcApi* uc_load(const wchar_t* hint_dir, std::string* error);

}  // namespace nk

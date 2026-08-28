// A Symbian XIP ROM image: mapped once per process, scanned for its
// executables.
#pragma once

#include <windows.h>
#include <stdint.h>

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace nk {

constexpr uint32_t kRomUid1 = 0x10000079;  // E32 DLL/EXE UID
constexpr uint32_t kRomBaseDefault = 0x80000000;
constexpr uint32_t kRomHeaderSize = 0x78;  // TRomImageHeader

// One ROM-resident executable, located by its header. The header sits
// immediately before the image's code, which is what makes a blind scan for
// the UID recoverable without parsing the ROM directory.
struct RomImage {
    uint32_t off = 0;
    uint32_t uid1 = 0, uid2 = 0, uid3 = 0, uidchk = 0;
    uint32_t entry = 0, code_addr = 0, data_addr = 0;
    uint32_t code_size = 0, text_size = 0, data_size = 0, bss_size = 0;
    uint32_t heap_min = 0, heap_max = 0, stack_size = 0;
    uint32_t dll_ref_table = 0, export_count = 0;
    uint32_t export_dir = 0;

    static bool parse(const uint8_t* blob, size_t len, size_t off,
                      RomImage* out);
};

// A read-only file mapping of one ROM, shared by every engine that uses it.
// A ROM is 20-70 MB; mapping it once and handing the same pages to each
// emulator through uc_mem_map_ptr is the difference between a quarter-second
// engine and an instant one.
class Rom {
  public:
    // Returns the cached mapping for `path`, opening it if needed.
    static std::shared_ptr<Rom> open(const std::wstring& path,
                                     std::string* error);

    ~Rom();

    const uint8_t* data() const { return data_; }
    size_t size() const { return size_; }
    uint32_t base() const { return base_; }
    const std::wstring& path() const { return path_; }

    const std::vector<RomImage>& images() const { return images_; }

    // The image exporting the whole API for a UID3. More than one image can
    // carry euser's UID3 - stubs and variants - so the real one is the one
    // with the most exports.
    const RomImage* image_by_uid3(uint32_t uid3) const;

    // Z:\sys\bin\<name> (lower case) -> image, from the ROM directory.
    const RomImage* image_by_name(const std::string& lower_name) const;

    // The export address table of an image, as absolute ROM addresses.
    std::vector<uint32_t> exports(const RomImage& img) const;

    bool read(uint32_t addr, void* out, size_t n) const;

  private:
    Rom() = default;
    void scan_images();
    void detect_base();
    void index_names();

    std::wstring path_;
    HANDLE file_ = nullptr;
    HANDLE mapping_ = nullptr;
    const uint8_t* data_ = nullptr;
    size_t size_ = 0;
    uint32_t base_ = kRomBaseDefault;
    std::vector<RomImage> images_;
    std::unordered_map<std::string, RomImage> by_name_;
};

}  // namespace nk

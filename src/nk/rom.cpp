#include "rom.h"

#include <algorithm>
#include <map>
#include <mutex>

#include "log.h"

namespace nk {
namespace {

std::mutex g_cache_mutex;
std::map<std::wstring, std::weak_ptr<Rom>> g_cache;

inline uint32_t rd32(const uint8_t* p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

std::wstring lower(const std::wstring& s) {
    std::wstring out(s);
    for (auto& c : out) c = towlower(c);
    return out;
}

}  // namespace

bool RomImage::parse(const uint8_t* blob, size_t len, size_t off,
                     RomImage* out) {
    if (off + 68 > len) return false;
    const uint8_t* p = blob + off;
    out->off = static_cast<uint32_t>(off);
    out->uid1 = rd32(p + 0);
    out->uid2 = rd32(p + 4);
    out->uid3 = rd32(p + 8);
    out->uidchk = rd32(p + 12);
    out->entry = rd32(p + 16);
    out->code_addr = rd32(p + 20);
    out->data_addr = rd32(p + 24);
    out->code_size = rd32(p + 28);
    out->text_size = rd32(p + 32);
    out->data_size = rd32(p + 36);
    out->bss_size = rd32(p + 40);
    out->heap_min = rd32(p + 44);
    out->heap_max = rd32(p + 48);
    out->stack_size = rd32(p + 52);
    out->dll_ref_table = rd32(p + 56);
    out->export_count = rd32(p + 60);
    out->export_dir = rd32(p + 64);
    return true;
}

std::shared_ptr<Rom> Rom::open(const std::wstring& path, std::string* error) {
    std::wstring key = lower(path);
    {
        std::lock_guard<std::mutex> lock(g_cache_mutex);
        auto it = g_cache.find(key);
        if (it != g_cache.end()) {
            if (auto live = it->second.lock()) return live;
        }
    }

    std::shared_ptr<Rom> rom(new Rom);
    rom->path_ = path;
    rom->file_ = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
                             nullptr, OPEN_EXISTING,
                             FILE_ATTRIBUTE_NORMAL, nullptr);
    if (rom->file_ == INVALID_HANDLE_VALUE) {
        rom->file_ = nullptr;
        if (error)
            *error = "could not open ROM (error " +
                     std::to_string(GetLastError()) + ")";
        return nullptr;
    }

    LARGE_INTEGER size{};
    if (!GetFileSizeEx(rom->file_, &size) || size.QuadPart <= 0) {
        if (error) *error = "ROM is empty";
        return nullptr;
    }
    rom->size_ = static_cast<size_t>(size.QuadPart);

    rom->mapping_ = CreateFileMappingW(rom->file_, nullptr, PAGE_READONLY, 0, 0,
                                       nullptr);
    if (!rom->mapping_) {
        if (error)
            *error = "could not map ROM (error " +
                     std::to_string(GetLastError()) + ")";
        return nullptr;
    }
    rom->data_ = static_cast<const uint8_t*>(
        MapViewOfFile(rom->mapping_, FILE_MAP_READ, 0, 0, 0));
    if (!rom->data_) {
        if (error)
            *error = "could not view ROM (error " +
                     std::to_string(GetLastError()) + ")";
        return nullptr;
    }

    rom->detect_base();
    rom->scan_images();
    rom->index_names();
    NK_LOG("ROM mapped: %zu bytes, base %#x, %zu images, %zu named",
           rom->size_, rom->base_, rom->images_.size(), rom->by_name_.size());

    std::lock_guard<std::mutex> lock(g_cache_mutex);
    g_cache[key] = rom;
    return rom;
}

Rom::~Rom() {
    if (data_) UnmapViewOfFile(data_);
    if (mapping_) CloseHandle(mapping_);
    if (file_) CloseHandle(file_);
}

// Not every phone maps its ROM at 0x80000000 - the 5500's sits at 0xF80F1000 -
// and with the base assumed wrong the self-consistency check below rejects
// every image. Rather than parse TRomHeader, whose layout moves between
// versions, let the images vote: each header sits immediately before its own
// code, so a real ROM has thousands agreeing on one base.
void Rom::detect_base() {
    std::map<uint32_t, int> votes;
    for (size_t off = 0; off + kRomHeaderSize <= size_; off += 4) {
        if (rd32(data_ + off) != kRomUid1) continue;
        RomImage img;
        if (!RomImage::parse(data_, size_, off, &img)) continue;
        if (img.code_size == 0 || img.code_size >= 0x400000) continue;
        votes[img.code_addr - kRomHeaderSize -
              static_cast<uint32_t>(off)] += 1;
    }
    uint32_t best = kRomBaseDefault;
    int agreed = 0;
    for (const auto& kv : votes) {
        if (kv.second > agreed) {
            agreed = kv.second;
            best = kv.first;
        }
    }
    if (agreed >= 8 && best > 0 && (best % 0x1000) == 0) base_ = best;
}

void Rom::scan_images() {
    for (size_t off = 0; off + kRomHeaderSize <= size_; off += 4) {
        if (rd32(data_ + off) != kRomUid1) continue;
        RomImage img;
        if (!RomImage::parse(data_, size_, off, &img)) continue;
        if (img.code_addr - base_ - kRomHeaderSize !=
            static_cast<uint32_t>(off))
            continue;
        if (img.code_size == 0 || img.code_size >= 0x400000) continue;
        images_.push_back(img);
    }
}

// Walk the ROM directory so external E32 images can resolve their imports by
// DLL name. Failure is not fatal: only the ROFS loader needs this, and every
// XIP-only profile works without it.
void Rom::index_names() {
    if (size_ < 0x98) return;
    uint32_t root_addr = rd32(data_ + 0x94);
    if (root_addr < base_) return;
    size_t root_off = root_addr - base_;
    if (root_off + 4 > size_) return;

    uint32_t count = rd32(data_ + root_off);
    if (count == 0 || count > 32) return;

    std::vector<uint32_t> roots;
    for (uint32_t i = 0; i < count; ++i) {
        size_t at = root_off + 4 + 8 * static_cast<size_t>(i);
        if (at + 8 > size_) return;
        roots.push_back(rd32(data_ + at + 4));
    }

    std::vector<size_t> seen;
    // Iterative walk with an explicit stack: some ROM directories nest deeply
    // enough that recursion here is a needless risk.
    struct Node {
        uint32_t addr;
        std::string prefix;
    };
    std::vector<Node> stack;
    for (uint32_t addr : roots) stack.push_back({addr, ""});

    while (!stack.empty()) {
        Node node = stack.back();
        stack.pop_back();
        if (node.addr < base_) continue;
        size_t off = node.addr - base_;
        if (off + 4 > size_) continue;
        if (std::find(seen.begin(), seen.end(), off) != seen.end()) continue;
        seen.push_back(off);

        uint32_t dir_size = rd32(data_ + off);
        if (dir_size < 4 || off + dir_size > size_) continue;
        size_t pos = off + 4, end = off + dir_size;

        while (pos + 10 <= end) {
            uint32_t addr = rd32(data_ + pos + 4);
            uint8_t attr = data_[pos + 8];
            uint8_t nlen = data_[pos + 9];
            pos += 10;
            if (nlen > 127 || pos + 2 * static_cast<size_t>(nlen) > end) break;

            std::string name;
            name.reserve(nlen);
            for (uint8_t i = 0; i < nlen; ++i) {
                wchar_t wc;
                memcpy(&wc, data_ + pos + 2 * i, 2);
                name.push_back(wc < 128 ? static_cast<char>(towlower(wc)) : '?');
            }
            pos += 2 * static_cast<size_t>(nlen);
            pos = (pos + 3) & ~static_cast<size_t>(3);

            std::string path = node.prefix + name;
            if (attr & 0x10) {
                stack.push_back({addr, path + "\\"});
            } else if (path.rfind("sys\\bin\\", 0) == 0 && addr >= base_) {
                RomImage img;
                if (RomImage::parse(data_, size_, addr - base_, &img))
                    by_name_[name] = img;
            }
        }
    }
}

const RomImage* Rom::image_by_uid3(uint32_t uid3) const {
    const RomImage* best = nullptr;
    for (const auto& img : images_) {
        if (img.uid3 != uid3) continue;
        if (!best || img.export_count > best->export_count) best = &img;
    }
    return best;
}

const RomImage* Rom::image_by_name(const std::string& lower_name) const {
    auto it = by_name_.find(lower_name);
    return it == by_name_.end() ? nullptr : &it->second;
}

std::vector<uint32_t> Rom::exports(const RomImage& img) const {
    std::vector<uint32_t> out;
    if (img.export_count == 0 || img.export_dir < base_) return out;
    size_t off = img.export_dir - base_;
    if (off + 4ull * img.export_count > size_) return out;
    out.resize(img.export_count);
    memcpy(out.data(), data_ + off, 4ull * img.export_count);
    return out;
}

bool Rom::read(uint32_t addr, void* out, size_t n) const {
    if (addr < base_) return false;
    size_t off = addr - base_;
    if (off + n > size_) return false;
    memcpy(out, data_ + off, n);
    return true;
}

}  // namespace nk

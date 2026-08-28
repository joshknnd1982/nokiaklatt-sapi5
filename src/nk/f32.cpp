#include "f32.h"

#include <windows.h>

#include <algorithm>

#include "emu.h"
#include "log.h"

namespace nk {
namespace {

constexpr uint32_t KMaxFileName = 256;

std::string to_lower(std::string s) {
    for (auto& c : s)
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}

// A Symbian path reduced to the form the index is keyed by: no drive letter,
// backslashes, lower case.
std::string canonical(const std::string& symbian_path) {
    std::string p = symbian_path;
    for (auto& c : p)
        if (c == '/') c = '\\';
    if (p.size() >= 2 && p[1] == ':') p = p.substr(2);
    if (p.empty() || p[0] != '\\') p.insert(p.begin(), '\\');
    return to_lower(p);
}

std::string narrow(const std::wstring& s) {
    std::string out;
    out.reserve(s.size());
    for (wchar_t c : s) out.push_back(c < 128 ? static_cast<char>(c) : '?');
    return out;
}

}  // namespace

// ---- descriptors ------------------------------------------------------

Descriptor read_desc_header(Emu& emu, uint32_t addr) {
    Descriptor d;
    if (!addr) return d;
    uint32_t w = emu.read32(addr);
    d.type = w >> 28;
    d.length = w & 0x0FFFFFFF;
    d.max = d.length;
    switch (d.type) {
        case 0:  // EBufC
            d.at = addr + 4;
            break;
        case 1:  // EPtrC
            d.at = emu.read32(addr + 4);
            break;
        case 2:  // EPtr
            d.max = emu.read32(addr + 4);
            d.at = emu.read32(addr + 8);
            break;
        case 3:  // EBuf
            d.max = emu.read32(addr + 4);
            d.at = addr + 8;
            break;
        case 4:  // EBufCPtr
            // word2 points at the HBufC itself; its data starts after its
            // length word, and that word is the authoritative length.
            d.max = emu.read32(addr + 4);
            d.owner = emu.read32(addr + 8);
            d.at = d.owner + 4;
            break;
        default:
            return d;
    }
    d.valid = true;
    return d;
}

std::string read_desc(Emu& emu, uint32_t addr, bool wide) {
    Descriptor d = read_desc_header(emu, addr);
    if (!d.valid || d.length == 0) return std::string();
    size_t n = static_cast<size_t>(d.length) * (wide ? 2 : 1);
    if (n > (16u << 20)) return std::string();
    std::string out(n, '\0');
    emu.read(d.at, &out[0], n);
    return out;
}

std::wstring read_desc_w(Emu& emu, uint32_t addr) {
    std::string raw = read_desc(emu, addr, true);
    std::wstring out(raw.size() / 2, L'\0');
    if (!out.empty()) memcpy(&out[0], raw.data(), out.size() * 2);
    return out;
}

size_t write_desc(Emu& emu, uint32_t addr, const void* data, size_t n) {
    if (!addr) return 0;
    uint32_t w = emu.read32(addr);
    uint32_t type = w >> 28;
    uint32_t max = 0, at = 0, owner = 0;
    switch (type) {
        case 2:
            max = emu.read32(addr + 4);
            at = emu.read32(addr + 8);
            break;
        case 3:
            max = emu.read32(addr + 4);
            at = addr + 8;
            break;
        case 4:
            max = emu.read32(addr + 4);
            owner = emu.read32(addr + 8);
            at = owner + 4;
            break;
        default:
            return 0;  // not a modifiable descriptor
    }
    size_t take = std::min<size_t>(n, max);
    if (take) emu.write(at, data, take);
    emu.write32(addr, (type << 28) | static_cast<uint32_t>(take));
    if (type == 4) {
        // The length must also land in the HBufC this TPtr was made from, or
        // the buffer still reads as empty to its owner.
        uint32_t ow = emu.read32(owner);
        emu.write32(owner,
                    (ow & 0xF0000000u) | static_cast<uint32_t>(take));
    }
    return take;
}

// ---- the file server ---------------------------------------------------

FileServer::FileServer(const std::wstring& root) : root_(root) {
    index(root, "");
    NK_LOG("file server rooted at %ls, %zu file(s) indexed", root.c_str(),
           index_.size());
}

void FileServer::index(const std::wstring& dir, const std::string& prefix) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW((dir + L"\\*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        std::wstring full = dir + L"\\" + name;
        std::string rel = prefix + "\\" + to_lower(narrow(name));
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
            index(full, rel);
        else
            index_[rel] = full;
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

std::wstring FileServer::resolve(const std::string& symbian_path) const {
    auto it = index_.find(canonical(symbian_path));
    return it == index_.end() ? std::wstring() : it->second;
}

void FileServer::override_file(const std::wstring& real_path,
                               std::vector<uint8_t> content) {
    overrides_[real_path] = std::move(content);
}

uint64_t FileServer::size_of(const std::wstring& path) const {
    auto it = overrides_.find(path);
    if (it != overrides_.end()) return it->second.size();
    WIN32_FILE_ATTRIBUTE_DATA fad;
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &fad))
        return 0;
    return (static_cast<uint64_t>(fad.nFileSizeHigh) << 32) | fad.nFileSizeLow;
}

std::vector<uint8_t> FileServer::read_at(const std::wstring& path,
                                         uint64_t pos, size_t length) const {
    auto it = overrides_.find(path);
    if (it != overrides_.end()) {
        const auto& blob = it->second;
        if (pos >= blob.size()) return {};
        size_t take = std::min(length, blob.size() - static_cast<size_t>(pos));
        return std::vector<uint8_t>(blob.begin() + static_cast<size_t>(pos),
                                    blob.begin() + static_cast<size_t>(pos) +
                                        take);
    }
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return {};
    LARGE_INTEGER at;
    at.QuadPart = static_cast<LONGLONG>(pos);
    SetFilePointerEx(h, at, nullptr, FILE_BEGIN);
    std::vector<uint8_t> buf(length);
    DWORD got = 0;
    ReadFile(h, buf.data(), static_cast<DWORD>(length), &got, nullptr);
    CloseHandle(h);
    buf.resize(got);
    return buf;
}

int32_t FileServer::send(Emu& emu, int32_t fn, const int32_t args[4]) {
    switch (fn) {
        case EFsConnect:
            return KErrNone;
        case EFsSessionPath:
            return op_session_path(emu, args);
        case EFsSetSessionPath:
            return op_set_session_path(emu, args);
        case EFsEntry:
            return op_entry(emu, args);
        case EFsFileOpen:
            return op_file_open(emu, args);
        case EFsFileRead:
            return op_file_read(emu, args);
        case EFsFileSeek:
            return op_file_seek(emu, args);
        case EFsFileSize:
            return op_file_size(emu, args);
        case EFsFileSubClose:
            return op_file_sub_close(emu, args);
        default:
            unhandled_[fn] += 1;
            return KErrNotSupported;
    }
}

int32_t FileServer::op_session_path(Emu& emu, const int32_t* args) {
    std::wstring wide(session_path_.begin(), session_path_.end());
    write_desc(emu, args[0], wide.data(), wide.size() * 2);
    return KErrNone;
}

int32_t FileServer::op_set_session_path(Emu& emu, const int32_t* args) {
    session_path_ = narrow(read_desc_w(emu, args[0]));
    return KErrNone;
}

int32_t FileServer::op_entry(Emu& emu, const int32_t* args) {
    std::string name = narrow(read_desc_w(emu, args[0]));
    std::wstring real = resolve(name);
    probed_.emplace_back(name, !real.empty());
    if (real.empty()) return KErrNotFound;

    // TEntry, delivered through a TPckg descriptor:
    //   TUint iAtt; TInt iSize; TTime iModified; TUidType iType;
    //   TBufC<KMaxFileName> iName
    std::string leaf = name;
    size_t cut = leaf.find_last_of('\\');
    if (cut != std::string::npos) leaf = leaf.substr(cut + 1);

    std::vector<uint8_t> entry;
    auto push32 = [&entry](uint32_t v) {
        entry.insert(entry.end(), reinterpret_cast<uint8_t*>(&v),
                     reinterpret_cast<uint8_t*>(&v) + 4);
    };
    push32(0x20);  // KEntryAttArchive
    push32(static_cast<uint32_t>(size_of(real)));
    entry.insert(entry.end(), 8, 0);   // iModified
    entry.insert(entry.end(), 12, 0);  // iType
    push32(static_cast<uint32_t>(leaf.size()));
    for (char c : leaf) {
        entry.push_back(static_cast<uint8_t>(c));
        entry.push_back(0);
    }
    entry.resize(4 + 4 + 8 + 12 + 4 + KMaxFileName * 2, 0);

    write_desc(emu, args[1], entry.data(), entry.size());
    return KErrNone;
}

int32_t FileServer::op_file_open(Emu& emu, const int32_t* args) {
    std::string name = narrow(read_desc_w(emu, args[0]));
    std::wstring real = resolve(name);
    probed_.emplace_back(name, !real.empty());
    if (real.empty()) {
        NK_LOG("RFile::Open(\"%s\") -> NOT FOUND", name.c_str());
        return KErrNotFound;
    }
    int32_t handle = next_handle_++;
    files_[handle] = OpenFile{real, 0};
    // DoCreateSubSession passes a TPckgBuf<TInt> in slot 3 and reads the
    // handle back out of it, so this is a descriptor write, not a raw poke.
    write_desc(emu, args[3], &handle, 4);
    return KErrNone;
}

int32_t FileServer::op_file_read(Emu& emu, const int32_t* args) {
    // MSG0 data, MSG1 length, MSG2 position, MSG3 subsession handle
    auto it = files_.find(args[3]);
    if (it == files_.end()) return KErrNotFound;
    OpenFile& f = it->second;
    int32_t length = args[1];
    uint64_t want = args[2] >= 0 ? static_cast<uint64_t>(args[2]) : f.position;
    size_t take = length > 0 ? static_cast<size_t>(length) : 0x10000;
    std::vector<uint8_t> blob = read_at(f.path, want, take);
    size_t n = write_desc(emu, args[0], blob.data(), blob.size());
    f.position = want + n;
    return KErrNone;
}

int32_t FileServer::op_file_seek(Emu& emu, const int32_t* args) {
    // MSG0 position, MSG1 mode, MSG2 new position (TPckgBuf<TInt>)
    auto it = files_.find(args[3]);
    if (it == files_.end()) return KErrNotFound;
    OpenFile& f = it->second;
    int64_t size = static_cast<int64_t>(size_of(f.path));
    int64_t pos = args[0];
    int64_t at;
    switch (args[1]) {
        case 1:
            at = static_cast<int64_t>(f.position) + pos;
            break;
        case 2:
            at = size + pos;
            break;
        default:
            at = pos;
            break;
    }
    at = std::max<int64_t>(0, std::min<int64_t>(at, size));
    f.position = static_cast<uint64_t>(at);
    if (args[2]) {
        int32_t out = static_cast<int32_t>(at);
        write_desc(emu, args[2], &out, 4);
    }
    return KErrNone;
}

int32_t FileServer::op_file_size(Emu& emu, const int32_t* args) {
    auto it = files_.find(args[3]);
    if (it == files_.end()) return KErrNotFound;
    int32_t size = static_cast<int32_t>(size_of(it->second.path));
    write_desc(emu, args[0], &size, 4);
    return KErrNone;
}

int32_t FileServer::op_file_sub_close(Emu&, const int32_t* args) {
    files_.erase(args[3]);
    return KErrNone;
}

}  // namespace nk

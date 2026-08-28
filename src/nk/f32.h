// Symbian descriptors, and a file server for the emulated client.
//
// The engine opens its data files through RFs, so the IPC boundary needs a
// server on the other side. Client and server share one address space here, so
// descriptors are read and written in place rather than copied through a
// kernel.
#pragma once

#include <stdint.h>

#include <map>
#include <string>
#include <unordered_map>
#include <vector>

namespace nk {

class Emu;

// F32 opcodes, in dispatch-table order from fileserver/sfile/sf_ops.h
enum : int32_t {
    EFsSessionPath = 0x0E,
    EFsSetSessionPath = 0x0F,
    EFsEntry = 0x16,
    EFsFileSubClose = 0x1C,
    EFsFileOpen = 0x1E,
    EFsFileCreate = 0x1F,
    EFsFileRead = 0x22,
    EFsFileSeek = 0x26,
    EFsFileSize = 0x28,
    EFsConnect = -1,
};

enum : int32_t {
    KErrNone = 0,
    KErrNotFound = -1,
    KErrNotSupported = -5,
};

// A decoded TDesC/TDes header.
struct Descriptor {
    uint32_t type = 0;
    uint32_t length = 0;   // in characters
    uint32_t max = 0;
    uint32_t at = 0;       // address of the data
    uint32_t owner = 0;    // EBufCPtr only: the HBufC the TPtr was made from
    bool valid = false;
};

Descriptor read_desc_header(Emu& emu, uint32_t addr);
// Reads the payload. `wide` doubles the character count for 16-bit text.
std::string read_desc(Emu& emu, uint32_t addr, bool wide);
std::wstring read_desc_w(Emu& emu, uint32_t addr);
// Writes into a modifiable descriptor and fixes up its length. Returns the
// number of bytes actually written, which is capped by the descriptor's max.
size_t write_desc(Emu& emu, uint32_t addr, const void* data, size_t n);

class FileServer {
  public:
    // `root` is the directory served as the whole Symbian drive tree.
    explicit FileServer(const std::wstring& root);

    // The real path behind a Symbian path, or an empty string.
    std::wstring resolve(const std::string& symbian_path) const;

    // Serve `content` in place of the file at `real_path`. Used to hand the
    // engine a patched copy of its own resource file.
    void override_file(const std::wstring& real_path, std::vector<uint8_t> content);

    int32_t send(Emu& emu, int32_t fn, const int32_t args[4]);

    // Diagnostics: every path the engine looked for, and whether it was there.
    const std::vector<std::pair<std::string, bool>>& probed() const {
        return probed_;
    }
    const std::map<int32_t, int>& unhandled() const { return unhandled_; }

  private:
    struct OpenFile {
        std::wstring path;
        uint64_t position = 0;
    };

    uint64_t size_of(const std::wstring& path) const;
    std::vector<uint8_t> read_at(const std::wstring& path, uint64_t pos,
                                 size_t length) const;
    void index(const std::wstring& dir, const std::string& prefix);

    int32_t op_session_path(Emu&, const int32_t*);
    int32_t op_set_session_path(Emu&, const int32_t*);
    int32_t op_entry(Emu&, const int32_t*);
    int32_t op_file_open(Emu&, const int32_t*);
    int32_t op_file_read(Emu&, const int32_t*);
    int32_t op_file_seek(Emu&, const int32_t*);
    int32_t op_file_size(Emu&, const int32_t*);
    int32_t op_file_sub_close(Emu&, const int32_t*);

    std::wstring root_;
    // Symbian-style lower-case path ("\resource\foo.rsc") -> real path.
    std::unordered_map<std::string, std::wstring> index_;
    std::unordered_map<std::wstring, std::vector<uint8_t>> overrides_;
    std::map<int32_t, OpenFile> files_;
    int32_t next_handle_ = 0x100;
    std::string session_path_ = "Z:\\";
    std::vector<std::pair<std::string, bool>> probed_;
    std::map<int32_t, int> unhandled_;
};

}  // namespace nk

#include "e32load.h"

#include <algorithm>
#include <unordered_map>

#include "f32.h"
#include "log.h"

namespace nk {
namespace {

constexpr uint32_t E32_UID_DLL = 0x10000079;
constexpr uint32_t E32_SIG = 0x434F5045;  // 'EPOC' little-endian
constexpr uint32_t COMP_NONE = 0;
constexpr uint32_t COMP_DEFLATE = 0x101F7AFC;

// Symbian's E32 "deflate" is not RFC1951. These constants and the fixed
// meta-Huffman tree describe the bitstream elf2e32 generates.
constexpr int HUFFMAN_MAX_CODELENGTH = 27;
constexpr int HUFFMAN_METACODE = HUFFMAN_MAX_CODELENGTH + 1;
constexpr int DEFLATE_LENGTH_MAG = 8;
constexpr int DEFLATE_DIST_MAG = 12;
constexpr int DEFLATE_MIN_LENGTH = 3;
constexpr int DEFLATE_DIST_CODE_BASE = 0x200;
constexpr int ENCODING_LITERALS = 256;
constexpr int ENCODING_LENGTHS = (DEFLATE_LENGTH_MAG - 1) * 4;
constexpr int ENCODING_SPECIALS = 1;
constexpr int ENCODING_DISTS = (DEFLATE_DIST_MAG - 1) * 4;
constexpr int ENCODING_LITERAL_LEN =
    ENCODING_LITERALS + ENCODING_LENGTHS + ENCODING_SPECIALS;
constexpr int ENCODING_EOS = ENCODING_LITERALS + ENCODING_LENGTHS;
constexpr int DEFLATE_CODES = ENCODING_LITERAL_LEN + ENCODING_DISTS;

const uint32_t kHuffmanMeta[] = {
    0x0004006c, 0x00040064, 0x0004005c, 0x00040050, 0x00040044, 0x0004003c,
    0x00040034, 0x00040021, 0x00040023, 0x00040025, 0x00040027, 0x00040029,
    0x00040014, 0x0004000c, 0x00040035, 0x00390037, 0x00330031, 0x0004002b,
    0x002f002d, 0x001f001d, 0x001b0019, 0x00040013, 0x00170015, 0x0004000d,
    0x0011000f, 0x000b0009, 0x00070003, 0x00050001,
};
constexpr int kHuffmanMetaCount =
    static_cast<int>(sizeof(kHuffmanMeta) / sizeof(kHuffmanMeta[0]));

// An MSB-first bit reader matching Symbian's E32 inflater, including its habit
// of reading 32-bit words in byte-stream (big-endian) order.
class BitInput {
  public:
    BitInput(const uint8_t* data, size_t size)
        : data_(data), size_(size) {
        int64_t bitlen = static_cast<int64_t>(size) * 8;
        if (bitlen == 0) {
            wordpos_ = 4;
            return;
        }
        bits_ = word(0);
        int64_t have = 32;
        bitlen -= have;
        if (bitlen < 0) have += bitlen;
        count_ = static_cast<int>(have);
        remain_ = bitlen;
        wordpos_ = 4;
    }

    uint32_t read1() {
        uint32_t t = bits_;
        int tcount = count_ - 1;
        if (tcount < 0) return read(1);
        count_ = tcount;
        bits_ = t << 1;
        return (t >> 31) & 1;
    }

    uint32_t read(int size) {
        if (size == 0) return 0;
        uint32_t val = 0;
        uint32_t tbits = bits_;
        count_ -= size;
        while (count_ < 0) {
            if (count_ + size != 0)
                val |= (tbits >> (32 - (count_ + size))) << (-count_);
            size = -count_;
            if (remain_ <= 0)
                throw E32LoadError("compressed E32 bitstream ended early");
            if (remain_ >= 32) {
                tbits = word(wordpos_);
            } else {
                size_t valid = static_cast<size_t>((remain_ + 7) / 8);
                tbits = 0;
                for (size_t i = 0; i < valid && wordpos_ + i < size_; ++i)
                    tbits |= static_cast<uint32_t>(data_[wordpos_ + i])
                             << (24 - i * 8);
            }
            wordpos_ += 4;
            count_ += 32;
            remain_ -= 32;
            if (remain_ < 0) count_ += static_cast<int>(remain_);
        }
        bits_ = size == 32 ? 0 : (tbits << size);
        return val | (tbits >> (32 - size));
    }

    // One symbol from Symbian's fixed meta-code tree.
    uint32_t meta() {
        int idx = 0;
        uint32_t huff = 0;
        for (;;) {
            idx += static_cast<int>((huff >> 16) / 4);
            if (idx < 0 || idx >= kHuffmanMetaCount)
                throw E32LoadError("invalid E32 meta-Huffman stream");
            huff = kHuffmanMeta[idx];
            if (read1() == 0) huff <<= 16;
            if (huff & 0x10000) return huff >> 17;
        }
    }

  private:
    uint32_t word(size_t pos) const {
        uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            uint8_t b = (pos + i < size_) ? data_[pos + i] : 0;
            v |= static_cast<uint32_t>(b) << (24 - i * 8);
        }
        return v;
    }

    const uint8_t* data_;
    size_t size_;
    uint32_t bits_ = 0;
    int count_ = 0;
    int64_t remain_ = 0;
    size_t wordpos_ = 0;
};

bool valid_lengths(const std::vector<int>& lengths) {
    int64_t remain = 1ll << HUFFMAN_MAX_CODELENGTH;
    int64_t total = 0;
    for (auto it = lengths.rbegin(); it != lengths.rend(); ++it) {
        int len = *it;
        if (len <= 0) continue;
        total += len;
        if (len > HUFFMAN_MAX_CODELENGTH) return false;
        int64_t cell = 1ll << (HUFFMAN_MAX_CODELENGTH - len);
        if (cell > remain) return false;
        remain -= cell;
    }
    return remain == 0 || total <= 1;
}

// Expand the run-length/move-to-front encoded Huffman length table.
std::vector<int> read_lengths(BitInput& bits, int count) {
    std::vector<int> mtf(HUFFMAN_METACODE);
    for (int i = 0; i < HUFFMAN_METACODE; ++i) mtf[i] = i;
    int last = 0;
    std::vector<int> out;
    out.reserve(count);
    int64_t run = 0;

    while (static_cast<int64_t>(out.size()) + run < count) {
        uint32_t c = bits.meta();
        if (c < 2) {
            run += run + c + 1;
            continue;
        }
        while (run) {
            out.push_back(last);
            --run;
            if (static_cast<int>(out.size()) > count)
                throw E32LoadError("E32 Huffman length table overflow");
        }
        int idx = static_cast<int>(c) - 1;
        if (idx >= static_cast<int>(mtf.size()))
            throw E32LoadError("invalid E32 Huffman MTF index");
        mtf[0] = last;
        last = mtf[idx];
        for (int j = idx; j > 0; --j) mtf[j] = mtf[j - 1];
        out.push_back(last);
    }
    while (run) {
        out.push_back(last);
        --run;
    }
    if (static_cast<int>(out.size()) != count)
        throw E32LoadError("wrong E32 Huffman length table size");
    return out;
}

// A canonical MSB-first Huffman decoder: maps[len][code] -> symbol.
struct Decoder {
    std::vector<std::unordered_map<uint32_t, int>> maps;
    int maxlen = 0;
};

Decoder make_decoder(const std::vector<int>& lengths) {
    Decoder d;
    for (int len : lengths) d.maxlen = std::max(d.maxlen, len);
    if (d.maxlen == 0) return d;

    std::vector<int> counts(d.maxlen, 0);
    for (int len : lengths)
        if (len) counts[len - 1] += 1;

    std::vector<uint32_t> next_code(d.maxlen, 0);
    uint32_t code = 0;
    for (int i = 0; i < d.maxlen; ++i) {
        code <<= 1;
        next_code[i] = code;
        code += counts[i];
    }

    d.maps.resize(d.maxlen + 1);
    for (size_t sym = 0; sym < lengths.size(); ++sym) {
        int len = lengths[sym];
        if (!len) continue;
        d.maps[len][next_code[len - 1]++] = static_cast<int>(sym);
    }
    return d;
}

int decode_symbol(BitInput& bits, const Decoder& d) {
    uint32_t code = 0;
    for (int len = 1; len <= d.maxlen; ++len) {
        code = (code << 1) | bits.read1();
        auto it = d.maps[len].find(code);
        if (it != d.maps[len].end()) return it->second;
    }
    throw E32LoadError("invalid E32 Huffman code");
}

inline uint32_t rd32(const uint8_t* p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

// The fixed 124-byte EKA2 E32Image header, plus the V-format size word.
struct E32Header {
    uint32_t uid1, uid2, uid3, signature, compression, flags;
    uint32_t code_size, data_size, bss_size, entry_point;
    uint32_t code_base, data_base, dll_count;
    uint32_t export_offset, export_count, text_size;
    uint32_t code_offset, data_offset, import_offset;
    uint32_t code_reloc_offset, data_reloc_offset;
    uint32_t header_format, uncompressed_size;

    explicit E32Header(const std::vector<uint8_t>& raw) {
        if (raw.size() < 124) throw E32LoadError("E32 image is too small");
        const uint8_t* b = raw.data();
        uid1 = rd32(b + 0);
        uid2 = rd32(b + 4);
        uid3 = rd32(b + 8);
        signature = rd32(b + 16);
        compression = rd32(b + 28);
        flags = rd32(b + 44);
        code_size = rd32(b + 48);
        data_size = rd32(b + 52);
        bss_size = rd32(b + 68);
        entry_point = rd32(b + 72);
        code_base = rd32(b + 76);
        data_base = rd32(b + 80);
        dll_count = rd32(b + 84);
        export_offset = rd32(b + 88);
        export_count = rd32(b + 92);
        text_size = rd32(b + 96);
        code_offset = rd32(b + 100);
        data_offset = rd32(b + 104);
        import_offset = rd32(b + 108);
        code_reloc_offset = rd32(b + 112);
        data_reloc_offset = rd32(b + 116);
        header_format = (flags >> 24) & 0xF;
        uncompressed_size =
            (header_format && raw.size() >= 128)
                ? rd32(b + 124)
                : static_cast<uint32_t>(raw.size() - code_offset);
        if (uid1 != E32_UID_DLL || signature != E32_SIG)
            throw E32LoadError("not an EKA2 DLL E32Image");
        if (code_size == 0 || code_size > 0x400000)
            throw E32LoadError("implausible E32 code size");
    }
};

std::vector<uint8_t> read_file(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        throw E32LoadError("could not open external DLL");
    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);
    std::vector<uint8_t> out(static_cast<size_t>(size.QuadPart));
    DWORD got = 0;
    ReadFile(h, out.data(), static_cast<DWORD>(out.size()), &got, nullptr);
    CloseHandle(h);
    out.resize(got);
    return out;
}

std::vector<uint8_t> expanded_image(const std::vector<uint8_t>& raw,
                                    const E32Header& h) {
    if (h.compression == COMP_NONE) return raw;
    if (h.compression != COMP_DEFLATE)
        throw E32LoadError("unsupported E32 compression");

    // V-format EKA2 images start the deflate stream at iCodeOffset. A few
    // older header variants put a four-byte size word there first, so try the
    // canonical position and then +4.
    std::string errors;
    for (uint32_t start : {h.code_offset, h.code_offset + 4}) {
        if (start >= raw.size()) continue;
        try {
            std::vector<uint8_t> dec = inflate_e32(
                raw.data() + start, raw.size() - start, h.uncompressed_size);
            std::vector<uint8_t> full(h.code_offset + dec.size());
            memcpy(full.data(), raw.data(), h.code_offset);
            memcpy(full.data() + h.code_offset, dec.data(), dec.size());
            return full;
        } catch (const E32LoadError& e) {
            errors += e.what();
            errors += "; ";
        }
    }
    throw E32LoadError("could not decompress E32 image: " + errors);
}

// Drop Symbian import decorations: foo{ver}[uid].dll -> foo.dll
std::string real_dll_name(const std::string& name) {
    size_t cut = name.size();
    for (char marker : {'{', '['}) {
        size_t p = name.find(marker);
        if (p != std::string::npos) cut = std::min(cut, p);
    }
    std::string base = name.substr(0, cut);
    for (auto& c : base)
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    static const char* kExts[] = {".dll", ".exe", ".ldd", ".pdd", ".csy"};
    bool has_ext = false;
    for (const char* ext : kExts) {
        size_t n = strlen(ext);
        if (base.size() >= n && base.compare(base.size() - n, n, ext) == 0)
            has_ext = true;
    }
    if (!has_ext) base += ".dll";
    return base;
}

uint32_t uid_from_import(const std::string& name) {
    size_t open = name.find('[');
    if (open == std::string::npos || open + 9 >= name.size()) return 0;
    if (name[open + 9] != ']') return 0;
    return static_cast<uint32_t>(
        strtoul(name.substr(open + 1, 8).c_str(), nullptr, 16));
}

struct ImportBlock {
    std::string name;
    std::vector<uint32_t> offsets;
};

std::vector<ImportBlock> read_imports(const std::vector<uint8_t>& full,
                                      const E32Header& h) {
    std::vector<ImportBlock> result;
    if (!h.import_offset || !h.dll_count) return result;
    if (h.import_offset + 4 > full.size())
        throw E32LoadError("E32 import table is outside image");

    size_t pos = h.import_offset + 4;
    for (uint32_t i = 0; i < h.dll_count; ++i) {
        if (pos + 8 > full.size())
            throw E32LoadError("truncated E32 import block");
        uint32_t name_off = rd32(full.data() + pos);
        int32_t count = static_cast<int32_t>(rd32(full.data() + pos + 4));
        pos += 8;
        if (count < 0 || count > 10000)
            throw E32LoadError("invalid E32 import count");

        size_t npos = h.import_offset + name_off;
        if (npos >= full.size())
            throw E32LoadError("invalid E32 import name offset");
        size_t end = npos;
        while (end < full.size() && full[end]) ++end;
        if (end >= full.size())
            throw E32LoadError("unterminated E32 import name");

        ImportBlock block;
        block.name.assign(reinterpret_cast<const char*>(full.data() + npos),
                          end - npos);
        size_t need = static_cast<size_t>(count) * 4;
        if (pos + need > full.size())
            throw E32LoadError("truncated E32 import offsets");
        block.offsets.resize(count);
        if (count) memcpy(block.offsets.data(), full.data() + pos, need);
        pos += need;
        result.push_back(std::move(block));
    }
    return result;
}

struct Relocation {
    uint32_t offset;
    uint32_t kind;
};

std::vector<Relocation> read_relocations(const std::vector<uint8_t>& full,
                                         uint32_t offset) {
    std::vector<Relocation> result;
    if (!offset) return result;
    if (offset + 8 > full.size())
        throw E32LoadError("E32 relocation section outside image");

    // E32RelocSection is two uint32s: iSize and iNumberOfRelocs. Symbian
    // defines iSize as the combined size of the page blocks that follow; it
    // does *not* include this 8-byte header. Treating it as a total section
    // size cuts the last block short on these images.
    uint32_t blocks_size = rd32(full.data() + offset);
    uint32_t declared = rd32(full.data() + offset + 4);
    if (!blocks_size) {
        if (declared)
            throw E32LoadError("empty E32 relocation section has relocs");
        return result;
    }
    size_t end = static_cast<size_t>(offset) + 8 + blocks_size;
    if (end > full.size())
        throw E32LoadError("truncated E32 relocation section");

    size_t pos = static_cast<size_t>(offset) + 8;
    uint32_t actual = 0;
    while (pos < end) {
        if (pos + 8 > end)
            throw E32LoadError("truncated E32 relocation block header");
        uint32_t page = rd32(full.data() + pos);
        uint32_t block_size = rd32(full.data() + pos + 4);
        if (page & 0xFFF)
            throw E32LoadError("E32 relocation page is not 4K aligned");
        if (block_size < 8 || (block_size & 3) || pos + block_size > end)
            throw E32LoadError("invalid E32 relocation block");

        size_t n = (block_size - 8) / 2;
        for (size_t i = 0; i < n; ++i) {
            uint16_t rel;
            memcpy(&rel, full.data() + pos + 8 + 2 * i, 2);
            if (rel == 0) continue;  // 32-bit alignment padding
            uint32_t kind = rel & 0xF000;
            if (kind != 0x1000 && kind != 0x2000 && kind != 0x3000)
                throw E32LoadError("unknown E32 relocation type");
            result.push_back({page + (rel & 0x0FFF), kind});
            ++actual;
        }
        pos += block_size;
    }
    if (pos != end) throw E32LoadError("mis-sized E32 relocation section");
    if (actual != declared)
        throw E32LoadError("E32 relocation count disagrees with the header");
    return result;
}

}  // namespace

std::vector<uint8_t> inflate_e32(const uint8_t* data, size_t size,
                                 size_t expected) {
    BitInput bits(data, size);
    std::vector<int> lengths = read_lengths(bits, DEFLATE_CODES);
    std::vector<int> lit(lengths.begin(), lengths.begin() + ENCODING_LITERAL_LEN);
    std::vector<int> dist(lengths.begin() + ENCODING_LITERAL_LEN, lengths.end());
    if (!valid_lengths(lit) || !valid_lengths(dist))
        throw E32LoadError("invalid E32 Huffman tables");

    Decoder lit_dec = make_decoder(lit);
    Decoder dist_dec = make_decoder(dist);

    std::vector<uint8_t> out;
    if (expected) out.reserve(expected);
    for (;;) {
        int sym = decode_symbol(bits, lit_dec);
        if (sym < ENCODING_LITERALS) {
            out.push_back(static_cast<uint8_t>(sym));
        } else if (sym == ENCODING_EOS) {
            break;
        } else {
            uint32_t code = static_cast<uint32_t>(sym - ENCODING_LITERALS) & 0xFF;
            if (code >= 8) {
                int extra = static_cast<int>(code >> 2) - 1;
                code -= static_cast<uint32_t>(extra) << 2;
                code = (code << extra) | bits.read(extra);
            }
            size_t length = code + DEFLATE_MIN_LENGTH;

            int dsym = decode_symbol(bits, dist_dec) + DEFLATE_DIST_CODE_BASE;
            uint32_t dcode =
                static_cast<uint32_t>(dsym - ENCODING_LITERALS) & 0xFF;
            if (dcode >= 8) {
                int extra = static_cast<int>(dcode >> 2) - 1;
                dcode -= static_cast<uint32_t>(extra) << 2;
                dcode = (dcode << extra) | bits.read(extra);
            }
            size_t distance = dcode + 1;
            if (distance > out.size())
                throw E32LoadError("invalid E32 back-reference");
            for (size_t i = 0; i < length; ++i)
                out.push_back(out[out.size() - distance]);
        }
        if (expected && out.size() > expected)
            throw E32LoadError("E32 stream expanded beyond advertised size");
    }
    if (expected && out.size() != expected)
        throw E32LoadError("E32 stream size disagrees with the header");
    return out;
}

const Emu::ExternalImage* load_e32(Emu& emu, const std::wstring& path,
                                   const std::string& lower_name) {
    if (const auto* already = emu.external_by_name(lower_name)) return already;

    std::vector<uint8_t> raw = read_file(path);
    E32Header h(raw);
    if (h.data_size || h.bss_size)
        throw E32LoadError(lower_name +
                           ": writable data/BSS is not supported by the "
                           "minimal loader");

    std::vector<uint8_t> full = expanded_image(raw, h);
    if (static_cast<size_t>(h.code_offset) + h.code_size > full.size())
        throw E32LoadError(lower_name + ": code extends past the E32 image");

    std::vector<uint8_t> code(full.begin() + h.code_offset,
                              full.begin() + h.code_offset + h.code_size);

    uint32_t run = emu.external_alloc(h.code_size);
    if (!run) throw E32LoadError("external E32 code area exhausted");

    // Imports are encoded as offsets into the code. The word at each offset
    // holds low16=ordinal, high16=adjustment until the kernel binds it to
    // dependency_export + adjustment.
    for (const ImportBlock& block : read_imports(full, h)) {
        std::string real = real_dll_name(block.name);
        std::vector<uint32_t> dep_exports;

        if (const auto* ext = emu.external_by_name(real)) {
            dep_exports = ext->exports;
        } else if (const RomImage* rom_img =
                       emu.rom().image_by_name(real)) {
            dep_exports = emu.rom().exports(*rom_img);
        } else if (uint32_t uid = uid_from_import(block.name)) {
            if (const auto* ext = emu.external_by_uid3(uid)) {
                dep_exports = ext->exports;
            } else if (const RomImage* img = emu.rom().image_by_uid3(uid)) {
                dep_exports = emu.rom().exports(*img);
            }
        }
        if (dep_exports.empty())
            throw E32LoadError("missing import dependency " + block.name);

        for (uint32_t off : block.offsets) {
            if (off + 4 > code.size())
                throw E32LoadError(lower_name + ": import offset outside code");
            uint32_t info = rd32(code.data() + off);
            uint32_t ordinal = info & 0xFFFF;
            uint32_t adjustment = info >> 16;
            if (ordinal == 0 || ordinal > dep_exports.size())
                throw E32LoadError(lower_name + ": import ordinal outside " +
                                   block.name);
            uint32_t target = dep_exports[ordinal - 1];
            if (!target)
                throw E32LoadError(lower_name + ": null import ordinal in " +
                                   block.name);
            uint32_t bound = target + adjustment;
            memcpy(code.data() + off, &bound, 4);
        }
    }

    uint32_t code_delta = run - h.code_base;
    uint32_t data_delta = 0u - h.data_base;
    for (const Relocation& rel : read_relocations(full, h.code_reloc_offset)) {
        if (rel.offset + 4 > code.size())
            throw E32LoadError(lower_name + ": relocation outside code");
        uint32_t value = rd32(code.data() + rel.offset);
        uint32_t delta;
        if (rel.kind == 0x1000) {
            delta = code_delta;
        } else if (rel.kind == 0x2000) {
            delta = data_delta;
        } else {  // 0x3000: infer from the link-time value
            if (value >= h.code_base && value <= h.code_base + h.code_size)
                delta = code_delta;
            else if (value >= h.data_base &&
                     value <= h.data_base + h.data_size + h.bss_size)
                delta = data_delta;
            else
                throw E32LoadError(lower_name +
                                   ": cannot infer a relocation base");
        }
        uint32_t fixed = value + delta;
        memcpy(code.data() + rel.offset, &fixed, 4);
    }

    emu.write(run, code.data(), code.size());

    Emu::ExternalImage image;
    image.name = lower_name;
    image.uid3 = h.uid3;
    image.code_addr = run;
    image.code_size = h.code_size;
    if (h.export_count) {
        size_t need =
            static_cast<size_t>(h.export_offset) + 4ull * h.export_count;
        if (need > full.size())
            throw E32LoadError(lower_name + ": export table outside image");
        for (uint32_t i = 0; i < h.export_count; ++i) {
            uint32_t addr = rd32(full.data() + h.export_offset + 4 * i);
            image.exports.push_back(addr ? run + (addr - h.code_base) : 0);
        }
    }

    NK_LOG("loaded %s at %#x, %u bytes, %zu exports", lower_name.c_str(), run,
           h.code_size, image.exports.size());
    emu.external_images().push_back(std::move(image));
    return &emu.external_images().back();
}

const Emu::ExternalImage* load_rofs_speech_modules(Emu& emu, FileServer& fs) {
    std::wstring nlp = fs.resolve("\\sys\\bin\\asrsnlphwdevice.dll");
    std::wstring dev = fs.resolve("\\sys\\bin\\nssdevtts.dll");
    if (nlp.empty() || dev.empty())
        throw E32LoadError(
            "nssdevtts is absent from ROM and the external speech DLLs are "
            "missing from files\\sys\\bin");
    load_e32(emu, nlp, "asrsnlphwdevice.dll");
    return load_e32(emu, dev, "nssdevtts.dll");
}

}  // namespace nk

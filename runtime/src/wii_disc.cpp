#include "wii_disc.h"

#include <cryptopp/aes.h>
#include <cryptopp/modes.h>

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <vector>

namespace WiiDisc {
namespace {

namespace fs = std::filesystem;

constexpr uint32_t kWiiMagic = 0x5D1C9EA3;
constexpr uint32_t kGameCubeMagic = 0xC2339F3D;
constexpr uint64_t kCluster = 0x8000;      // on the disc: 0x400 of hashes, then the data
constexpr uint64_t kClusterData = 0x7C00;
constexpr uint64_t kBatch = 64;            // clusters read and decrypted at a time (2 MiB)
constexpr size_t kChunk = 1 << 20;         // bytes written at a time

// The Wii's common keys, as WiiDisc.java and Dolphin hold them: a partition's title key is encrypted
// with one of these, as on the console.
constexpr uint8_t kCommonKeys[2][16] = {
    {0xeb, 0xe4, 0x2a, 0x22, 0x5e, 0x85, 0x93, 0xe4, 0x48, 0xd9, 0xc5, 0x45, 0x73, 0x81, 0xaa, 0xf7}, // retail
    {0x63, 0xb8, 0x2b, 0xb4, 0xf4, 0x61, 0x4e, 0x2e, 0x13, 0xf2, 0xfe, 0xfb, 0xba, 0x4c, 0x9b, 0x7e}, // Korean
};

const char* const kConvert = ". Dolphin can turn it into an .iso: right-click the game, Convert File..., Format: ISO";

uint32_t Be32(const uint8_t* b) {
    return uint32_t(b[0]) << 24 | uint32_t(b[1]) << 16 | uint32_t(b[2]) << 8 | uint32_t(b[3]);
}

uint16_t Be16(const uint8_t* b) {
    return uint16_t(b[0] << 8 | b[1]);
}

std::string Utf8(const fs::path& path) {
    const auto text = path.u8string();
    return std::string(text.begin(), text.end());
}

// One file or several in a row (a .wbfs split into .wbf1, .wbf2... for FAT32), read as one.
class Parts {
public:
    bool Open(const fs::path& first) {
        Add(first);
        if (m_parts.empty()) return false;
        // Dolphin's naming: the extension's last letter becomes 1, 2, 3...
        fs::path::string_type name = first.native();
        if (name.size() > 1 && (name.back() == 's' || name.back() == 'S')) {
            for (char part = '1'; part <= '9'; ++part) {
                name.back() = static_cast<fs::path::value_type>(part);
                const fs::path next(name);
                std::error_code ec;
                if (!fs::is_regular_file(next, ec) || !Add(next)) break;
            }
        }
        return true;
    }

    bool Read(uint64_t offset, uint8_t* out, size_t size) {
        for (Part& part : m_parts) {
            if (size == 0) break;
            if (offset >= part.size) {
                offset -= part.size;
                continue;
            }
            const size_t n = size_t(std::min<uint64_t>(size, part.size - offset));
            part.file.clear();
            part.file.seekg(std::streamoff(offset));
            part.file.read(reinterpret_cast<char*>(out), std::streamsize(n));
            if (size_t(part.file.gcount()) != n) return false;
            out += n;
            size -= n;
            offset = 0;
        }
        return size == 0;
    }

private:
    struct Part {
        std::ifstream file;
        uint64_t size = 0;
    };

    bool Add(const fs::path& path) {
        std::error_code ec;
        const uint64_t size = fs::file_size(path, ec);
        if (ec) return false;
        Part part;
        part.file.open(path, std::ios::binary);
        if (!part.file) return false;
        part.size = size;
        m_parts.push_back(std::move(part));
        return true;
    }

    std::vector<Part> m_parts;
};

// The disc's bytes, from whatever the image stores them in.
class Reader {
public:
    virtual ~Reader() = default;
    // `size` bytes at `offset` of the disc; false when the image ends first.
    virtual bool Read(uint64_t offset, uint8_t* out, size_t size) = 0;
};

class IsoReader final : public Reader {
public:
    explicit IsoReader(Parts parts) : m_parts(std::move(parts)) {}
    bool Read(uint64_t offset, uint8_t* out, size_t size) override { return m_parts.Read(offset, out, size); }

private:
    Parts m_parts;
};

// A .wbfs file: the disc in blocks, with a table saying where each block of the disc is stored (0 for a
// block that isn't stored, which reads as zeros).
class WbfsReader final : public Reader {
public:
    static std::unique_ptr<Reader> Open(Parts parts, std::string& error) {
        uint8_t head[12];
        if (!parts.Read(0, head, sizeof(head))) {
            error = "the disc image is cut short";
            return nullptr;
        }
        const unsigned sectorShift = head[8];   // the drive's sector size, log2
        const unsigned blockShift = head[9];    // the image's block size, log2
        if (sectorShift < 9 || sectorShift > 16 || blockShift < 15 || blockShift > 30) {
            error = "that .wbfs file is damaged";
            return nullptr;
        }
        const uint64_t sector = uint64_t(1) << sectorShift;
        // A dual-layer Wii disc has 143432 * 2 sectors of 0x8000 bytes.
        const size_t blocks = size_t((uint64_t(143432) * 2) >> (blockShift - 15));
        const uint64_t infoSize = (0x100 + blocks * 2 + sector - 1) / sector * sector;
        std::vector<uint8_t> slots(size_t(sector - 12));
        if (!parts.Read(12, slots.data(), slots.size())) {
            error = "the disc image is cut short";
            return nullptr;
        }
        const auto used = std::find_if(slots.begin(), slots.end(), [](uint8_t slot) { return slot != 0; });
        if (used == slots.end()) {
            error = "that .wbfs file holds no game";
            return nullptr;
        }
        const uint64_t info = sector + uint64_t(used - slots.begin()) * infoSize;
        std::vector<uint8_t> table(blocks * 2);
        if (!parts.Read(info + 0x100, table.data(), table.size())) {
            error = "the disc image is cut short";
            return nullptr;
        }
        auto reader = std::unique_ptr<WbfsReader>(new WbfsReader(std::move(parts), blockShift));
        reader->m_table.resize(blocks);
        for (size_t i = 0; i < blocks; ++i) reader->m_table[i] = Be16(&table[i * 2]);
        return reader;
    }

    bool Read(uint64_t offset, uint8_t* out, size_t size) override {
        const uint64_t block = uint64_t(1) << m_shift;
        while (size > 0) {
            const uint64_t index = offset >> m_shift;
            const uint64_t within = offset & (block - 1);
            const size_t n = size_t(std::min<uint64_t>(size, block - within));
            if (index >= m_table.size()) return false;
            if (m_table[index] == 0) {
                std::memset(out, 0, n);
            } else if (!m_parts.Read((uint64_t(m_table[index]) << m_shift) + within, out, n)) {
                return false;
            }
            offset += n;
            out += n;
            size -= n;
        }
        return true;
    }

private:
    WbfsReader(Parts parts, unsigned shift) : m_parts(std::move(parts)), m_shift(shift) {}

    Parts m_parts;
    unsigned m_shift;
    std::vector<uint16_t> m_table;
};

// An FST name as a path, refused when it could leave its folder or land elsewhere on Windows.
bool NameToPath(const std::string& name, fs::path& out) {
    if (name.empty() || name == "." || name == "..") return false;
    for (const char c : name) {
        const auto u = static_cast<unsigned char>(c);
        if (u < 0x20 || c == '/' || c == '\\') return false;
#if defined(_WIN32)
        if (std::strchr(":*?\"<>|", c) != nullptr) return false;
#endif
    }
#if defined(_WIN32)
    if (name.back() == '.' || name.back() == ' ') return false;
    std::string stem = name.substr(0, name.find('.'));
    std::transform(stem.begin(), stem.end(), stem.begin(), [](char c) { return char(std::toupper(static_cast<unsigned char>(c))); });
    static const char* const reserved[] = {"CON", "PRN", "AUX", "NUL", "COM1", "COM2", "COM3", "COM4", "COM5",
                                           "COM6", "COM7", "COM8", "COM9", "LPT1", "LPT2", "LPT3", "LPT4",
                                           "LPT5", "LPT6", "LPT7", "LPT8", "LPT9"};
    for (const char* r : reserved) {
        if (stem == r) return false;
    }
    // Latin-1 is the first 256 code points, so each byte becomes the wide character of the same value.
    std::wstring wide(name.size(), L'\0');
    std::transform(name.begin(), name.end(), wide.begin(), [](char c) { return wchar_t(static_cast<unsigned char>(c)); });
    out = fs::path(wide);
#else
    std::string utf8;
    for (const char c : name) {
        const auto u = static_cast<unsigned char>(c);
        if (u < 0x80) {
            utf8 += c;
        } else {
            utf8 += char(0xC0 | (u >> 6));
            utf8 += char(0x80 | (u & 0x3F));
        }
    }
    out = fs::path(utf8);
#endif
    return true;
}

} // namespace

struct Disc::State {
    std::unique_ptr<Reader> reader;
    std::string id;
    int revision = 0;
    uint64_t dataStart = 0, dataSize = 0;
    CryptoPP::CBC_Mode<CryptoPP::AES>::Decryption cipher;
    std::vector<uint8_t> raw = std::vector<uint8_t>(kCluster * kBatch);
    std::vector<uint8_t> plain = std::vector<uint8_t>(kClusterData * kBatch);
    uint64_t cachedFirst = UINT64_MAX;
    uint64_t cachedCount = 0;
    // The partition's layout (boot.bin): where main.dol and the file table are, and their sizes.
    uint64_t dolOffset = 0, dolSize = 0, fstOffset = 0, apploaderSize = 0, totalSize = 0;
    std::vector<uint8_t> fst;

    // Reads and decrypts up to kBatch clusters from `first`: each one's IV is in its hash area.
    bool Decrypt(uint64_t first) {
        const uint64_t clusters = (dataSize + kCluster - 1) / kCluster;
        if (first >= clusters) return false;
        const uint64_t count = std::min(kBatch, clusters - first);
        if (!reader->Read(dataStart + first * kCluster, raw.data(), size_t(count * kCluster))) return false;
        for (uint64_t c = 0; c < count; ++c) {
            const uint8_t* cluster = &raw[size_t(c * kCluster)];
            cipher.Resynchronize(cluster + 0x3D0, 16);
            cipher.ProcessData(&plain[size_t(c * kClusterData)], cluster + 0x400, size_t(kClusterData));
        }
        cachedFirst = first;
        cachedCount = count;
        return true;
    }

    // `size` bytes of the decrypted partition at `offset` (an offset in its data, hashes left out).
    bool ReadPartition(uint64_t offset, uint8_t* out, size_t size) {
        for (size_t at = 0; at < size;) {
            const uint64_t cluster = (offset + at) / kClusterData;
            const uint64_t within = (offset + at) % kClusterData;
            if (cluster < cachedFirst || cluster >= cachedFirst + cachedCount) {
                if (!Decrypt(cluster)) return false;
            }
            const uint64_t start = (cluster - cachedFirst) * kClusterData + within;
            const size_t n = size_t(std::min<uint64_t>(size - at, cachedCount * kClusterData - start));
            std::memcpy(out + at, &plain[size_t(start)], n);
            at += n;
        }
        return true;
    }

    std::string ReadLayout() {
        uint8_t boot[0x440];
        uint8_t apploader[0x20];
        uint8_t dol[0x100];
        if (!ReadPartition(0, boot, sizeof(boot))) return "the disc image is cut short";
        dolOffset = uint64_t(Be32(boot + 0x420)) << 2;
        fstOffset = uint64_t(Be32(boot + 0x424)) << 2;
        const uint64_t fstSize = uint64_t(Be32(boot + 0x428)) << 2;
        if (fstSize < 12 || fstSize > (64u << 20)) return "the disc's file table is damaged";
        if (!ReadPartition(0x2440, apploader, sizeof(apploader))) return "the disc image is cut short";
        apploaderSize = 0x20 + uint64_t(Be32(apploader + 0x14)) + Be32(apploader + 0x18);
        if (!ReadPartition(dolOffset, dol, sizeof(dol))) return "the disc image is cut short";
        dolSize = 0x100;
        for (int i = 0; i < 18; ++i) {
            dolSize = std::max<uint64_t>(dolSize, uint64_t(Be32(dol + i * 4)) + Be32(dol + 0x90 + i * 4));
        }
        fst.resize(size_t(fstSize));
        if (!ReadPartition(fstOffset, fst.data(), fst.size())) return "the disc image is cut short";
        const uint32_t entries = Be32(&fst[8]);
        if (entries == 0 || uint64_t(entries) * 12 > fstSize) return "the disc's file table is damaged";
        totalSize = 0x440 + 0x2000 + apploaderSize + dolSize + fstSize;
        for (uint32_t i = 1; i < entries; ++i) {
            if (fst[i * 12] == 0) totalSize += Be32(&fst[i * 12 + 8]);
        }
        return "";
    }

    std::string Write(const fs::path& target, uint64_t offset, uint64_t size, uint64_t& done,
                      const Progress& progress, const std::atomic<bool>& cancel) {
        std::error_code ec;
        fs::create_directories(target.parent_path(), ec);
        if (offset + size > dataSize / kCluster * kClusterData) return "the disc image is cut short";
        std::ofstream out(target, std::ios::binary | std::ios::trunc);
        if (!out) return "couldn't create " + Utf8(target);
        std::vector<uint8_t> chunk(size_t(std::min<uint64_t>(kChunk, std::max<uint64_t>(size, 1))));
        for (uint64_t at = 0; at < size;) {
            if (cancel.load(std::memory_order_relaxed)) return "cancelled";
            const size_t n = size_t(std::min<uint64_t>(chunk.size(), size - at));
            if (!ReadPartition(offset + at, chunk.data(), n)) return "the disc image is cut short";
            out.write(reinterpret_cast<const char*>(chunk.data()), std::streamsize(n));
            if (!out) return "couldn't write " + Utf8(target) + " (is the drive full?)";
            at += n;
            done += n;
            if (progress) progress(done, totalSize);
        }
        out.close();
        if (!out) return "couldn't write " + Utf8(target) + " (is the drive full?)";
        return "";
    }
};

std::unique_ptr<Disc> Disc::Open(const fs::path& path, std::string& error) {
    Parts parts;
    if (!parts.Open(path)) {
        error = "couldn't open " + Utf8(path);
        return nullptr;
    }
    uint8_t magic[4] = {};
    if (!parts.Read(0, magic, sizeof(magic))) {
        error = "that isn't a Wii disc image";
        return nullptr;
    }
    auto state = std::make_unique<State>();
    if (std::memcmp(magic, "WBFS", 4) == 0) {
        state->reader = WbfsReader::Open(std::move(parts), error);
        if (!state->reader) return nullptr;
    } else if (std::memcmp(magic, "RVZ\x01", 4) == 0 || std::memcmp(magic, "WIA\x01", 4) == 0) {
        error = std::string("that's a compressed disc image (.rvz or .wia)") + kConvert;
        return nullptr;
    } else if (std::memcmp(magic, "\x01\xC0\x0B\xB1", 4) == 0) {
        error = std::string("that's a compressed disc image (.gcz)") + kConvert;
        return nullptr;
    } else if (std::memcmp(magic, "CISO", 4) == 0) {
        error = std::string("that's a compressed disc image (.ciso)") + kConvert;
        return nullptr;
    } else {
        state->reader = std::make_unique<IsoReader>(std::move(parts));
    }

    uint8_t head[0x80];
    if (!state->reader->Read(0, head, sizeof(head))) {
        error = "that isn't a Wii disc image";
        return nullptr;
    }
    if (Be32(head + 0x1C) == kGameCubeMagic) {
        error = "that's a GameCube disc image, not a Wii one";
        return nullptr;
    }
    if (Be32(head + 0x18) != kWiiMagic) {
        error = "that isn't a Wii disc image";
        return nullptr;
    }
    uint8_t nkit[4] = {};
    if (state->reader->Read(0x200, nkit, sizeof(nkit)) && std::memcmp(nkit, "NKIT", 4) == 0) {
        error = "that's an NKit image; restore it to an .iso with NKit or Dolphin first";
        return nullptr;
    }
    if (head[0x61] != 0) {
        error = "that disc image isn't encrypted the way a Wii disc is";
        return nullptr;
    }
    state->id.assign(reinterpret_cast<const char*>(head), 6);
    state->revision = head[7];

    // The data partition (type 0) from the partition table at 0x40000.
    uint64_t partition = 0;
    uint8_t groups[32];
    if (!state->reader->Read(0x40000, groups, sizeof(groups))) {
        error = "the disc image is cut short";
        return nullptr;
    }
    for (int g = 0; g < 4 && partition == 0; ++g) {
        const uint32_t count = Be32(groups + g * 8);
        const uint64_t table = uint64_t(Be32(groups + g * 8 + 4)) << 2;
        if (count == 0 || count > 64) continue;
        std::vector<uint8_t> entries(count * 8);
        if (!state->reader->Read(table, entries.data(), entries.size())) continue;
        for (uint32_t i = 0; i < count; ++i) {
            if (Be32(&entries[i * 8 + 4]) == 0) {
                partition = uint64_t(Be32(&entries[i * 8])) << 2;
                break;
            }
        }
    }
    if (partition == 0) {
        error = "that disc image has no game partition";
        return nullptr;
    }

    uint8_t ticket[0x2C0];
    if (!state->reader->Read(partition, ticket, sizeof(ticket))) {
        error = "the disc image is cut short";
        return nullptr;
    }
    const unsigned keyIndex = ticket[0x1F1];
    if (keyIndex >= 2) {
        error = "that disc image isn't from a retail Wii disc";
        return nullptr;
    }
    state->dataStart = partition + (uint64_t(Be32(ticket + 0x2B8)) << 2);
    state->dataSize = uint64_t(Be32(ticket + 0x2BC)) << 2;
    uint8_t iv[16] = {};
    std::memcpy(iv, ticket + 0x1DC, 8); // the title ID
    uint8_t titleKey[16];
    CryptoPP::CBC_Mode<CryptoPP::AES>::Decryption keyCipher;
    keyCipher.SetKeyWithIV(kCommonKeys[keyIndex], 16, iv, 16);
    keyCipher.ProcessData(titleKey, ticket + 0x1BF, 16);
    state->cipher.SetKeyWithIV(titleKey, 16, iv, 16);

    error = state->ReadLayout();
    if (!error.empty()) return nullptr;
    return std::unique_ptr<Disc>(new Disc(std::move(state)));
}

Disc::Disc(std::unique_ptr<State> state) : m_state(std::move(state)) {}
Disc::~Disc() = default;

const std::string& Disc::Id() const {
    return m_state->id;
}

int Disc::Revision() const {
    return m_state->revision;
}

uint64_t Disc::ExtractedSize() const {
    return m_state->totalSize;
}

std::string Disc::Extract(const fs::path& root, const Progress& progress, const std::atomic<bool>& cancel,
                          int& files) {
    State& s = *m_state;
    files = 0;
    uint64_t done = 0;
    const fs::path sys = root / "sys";
    struct SysFile {
        const char* name;
        uint64_t offset, size;
    };
    const SysFile sysFiles[] = {
        {"boot.bin", 0, 0x440},
        {"bi2.bin", 0x440, 0x2000},
        {"apploader.img", 0x2440, s.apploaderSize},
        {"main.dol", s.dolOffset, s.dolSize},
        {"fst.bin", s.fstOffset, s.fst.size()},
    };
    for (const SysFile& file : sysFiles) {
        std::string error = s.Write(sys / file.name, file.offset, file.size, done, progress, cancel);
        if (!error.empty()) return error;
        ++files;
    }

    // The FST: entry 0 is the root; a directory's third word is the index past its last entry.
    const uint32_t entries = Be32(&s.fst[8]);
    const size_t strings = size_t(entries) * 12;
    std::vector<fs::path> dirs(entries + 1);
    std::vector<uint32_t> ends(entries + 1);
    size_t depth = 0;
    dirs[0] = root / "files";
    ends[0] = entries;
    std::error_code ec;
    fs::create_directories(dirs[0], ec);
    for (uint32_t i = 1; i < entries; ++i) {
        while (depth > 0 && i >= ends[depth]) --depth;
        const uint8_t* entry = &s.fst[size_t(i) * 12];
        const uint32_t word = Be32(entry);
        const size_t at = strings + (word & 0xFFFFFF);
        if (at >= s.fst.size()) return "the disc's file table is damaged";
        const char* begin = reinterpret_cast<const char*>(&s.fst[at]);
        const char* end = static_cast<const char*>(std::memchr(begin, 0, s.fst.size() - at));
        fs::path name;
        if (end == nullptr || !NameToPath(std::string(begin, end), name)) return "the disc's file table is damaged";
        const fs::path path = dirs[depth] / name;
        if ((word >> 24) != 0) {
            ++depth;
            dirs[depth] = path;
            ends[depth] = std::min(entries, Be32(entry + 8));
            fs::create_directories(path, ec);
            if (!fs::is_directory(path, ec)) return "couldn't create " + Utf8(path);
        } else {
            std::string error = s.Write(path, uint64_t(Be32(entry + 4)) << 2, Be32(entry + 8), done, progress, cancel);
            if (!error.empty()) return error;
            ++files;
        }
    }
    return "";
}

} // namespace WiiDisc

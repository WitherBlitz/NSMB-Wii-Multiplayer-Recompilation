#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

// A Wii disc image (.iso or .wbfs) extracted the way Dolphin's Extract Entire Disc does it: the game
// partition's sys/ (boot.bin, bi2.bin, apploader.img, main.dol, fst.bin) and files/. A port of the
// Android app's WiiDisc.java. Compressed images (.rvz, .wia, .gcz, .ciso, NKit) are recognised and
// refused with a sentence saying how to turn them into an .iso.
namespace WiiDisc {

class Disc {
public:
    // Opens the image at `path`. On failure returns null and sets `error` to why, in words for the
    // player ("that isn't a Wii disc image").
    static std::unique_ptr<Disc> Open(const std::filesystem::path& path, std::string& error);
    ~Disc();

    // The disc header's game ID (6 characters, such as SMNE01) and revision.
    const std::string& Id() const;
    int Revision() const;

    // How many bytes Extract() writes.
    uint64_t ExtractedSize() const;

    using Progress = std::function<void(uint64_t done, uint64_t total)>;

    // Writes sys/ and files/ into `root`, and sets `files` to how many files it wrote. Returns "" when
    // done, or why it stopped ("cancelled" once `cancel` is set). Whatever was written stays: the
    // caller extracts into a folder of its own and removes that folder on failure.
    std::string Extract(const std::filesystem::path& root, const Progress& progress,
                        const std::atomic<bool>& cancel, int& files);

    struct State;

private:
    explicit Disc(std::unique_ptr<State> state);
    std::unique_ptr<State> m_state;
};

} // namespace WiiDisc

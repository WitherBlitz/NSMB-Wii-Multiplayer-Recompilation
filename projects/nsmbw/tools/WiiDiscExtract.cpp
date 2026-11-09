// Dev-only: extracts a Wii disc image with WiiDisc, as the game's Open ISO does, to check the result
// against Dolphin's Extract Entire Disc. Usage: WiiDiscExtract <image> <output folder>
#include "wii_disc.h"

#include <chrono>
#include <cstdio>

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: WiiDiscExtract <image> <output folder>\n");
        return 2;
    }
    std::string error;
    const auto disc = WiiDisc::Disc::Open(argv[1], error);
    if (!disc) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }
    std::printf("%s revision %d, %llu bytes to write\n", disc->Id().c_str(), disc->Revision(),
                static_cast<unsigned long long>(disc->ExtractedSize()));
    const std::atomic<bool> cancel{false};
    int lastTenth = -1;
    int files = 0;
    const auto start = std::chrono::steady_clock::now();
    error = disc->Extract(argv[2], [&](uint64_t done, uint64_t total) {
        const int tenth = int(done * 10 / (total ? total : 1));
        if (tenth != lastTenth) {
            lastTenth = tenth;
            std::printf("  %d%%\n", tenth * 10);
            std::fflush(stdout);
        }
    }, cancel, files);
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    if (!error.empty()) {
        std::fprintf(stderr, "error: %s\n", error.c_str());
        return 1;
    }
    std::printf("%d files in %.1f s\n", files, seconds);
    return 0;
}

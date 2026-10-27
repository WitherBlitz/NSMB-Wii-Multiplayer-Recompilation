#pragma once

#include <string>

// The file select's Update button: the latest GitHub release, installed over this one.
// Windows and Linux: the release's archive is downloaded and checked against GitHub's SHA-256 for it,
// its files replace the installation's (UserData and the game files are never in it; the replaced
// files are kept as *.old until the next start), and the game starts again.
// Android: the app's own updater (Updater.java) does the work; the app reports here (android_bridge).
namespace Updater {

enum class State {
    Idle,         // not checked (yet), or this is the latest release
    Available,    // a newer release: the button shows
    Downloading,
    Installing,
    Restarting,
    Failed,       // the button stays, to try again; message() says why
};

struct Status {
    State state = State::Idle;
    std::string version;   // the newer release, "0.2.3"
    float progress = -1;   // 0..1 while downloading, -1 unknown
    std::string message;   // why it failed
};

// Once at startup: removes what the last update left behind and checks GitHub in the background.
// NSMBW_NO_UPDATE_CHECK=1 skips the check; NSMBW_UPDATE_URL replaces GitHub's latest-release address
// and NSMBW_UPDATE_TEST_VERSION this build's version (testing).
void Start();
Status Get();
// The button: downloads and installs the newer release in the background, then restarts the game.
void Install();

#if defined(__ANDROID__)
// From the app (GameActivity): what its updater found or is doing.
void SetFromApp(State state, const std::string& version, float progress, const std::string& message);
#endif

} // namespace Updater

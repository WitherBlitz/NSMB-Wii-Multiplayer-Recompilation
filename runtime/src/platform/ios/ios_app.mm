// ios_app.mm - the iOS app around the game: game import, the on-screen remote, motion, the menu and
// the app's lifecycle. The counterpart of the Android app's Java side (android/app/src/main/java/
// com/wither/nsmbw), calling the same runtime entry points android_bridge.cpp exposes over JNI.
#include "ios_app.h"

#if defined(__APPLE__) && TARGET_OS_IPHONE

#include "display_settings.h"
#include "game_folder.h"
#include "host_suspend.h"
#include "runtime_config.h"
#include "runtime_log.h"
#include "virtual_remote.h"

#include "netplay/net_socket.h"
#include "netplay_session.h"
#include "netplay_start.h"
#include "netplay_ui.h"
#include "settings_overlay.h"

#include <SDL3/SDL_hints.h>
#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_video.h>

#import <CoreMotion/CoreMotion.h>
#import <UIKit/UIKit.h>
#import <UniformTypeIdentifiers/UniformTypeIdentifiers.h>

#include <atomic>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <system_error>
#include <thread>

namespace fs = std::filesystem;

namespace {

constexpr const char* kTitle = "New Super Mario Bros. Wii";

// One short turn of the main run loop: UIKit delivers touches, alerts and notifications in it.
void TurnRunLoop(CFTimeInterval seconds) {
    CFRunLoopRunInMode(kCFRunLoopDefaultMode, seconds, true);
}

// The main thread waits in the run loop (it is also the game's thread, so it can't block).
void RunUntil(const std::function<bool()>& done) {
    while (!done()) {
        @autoreleasepool {
            TurnRunLoop(0.05);
        }
    }
}

NSString* ToNs(const std::string& text) { return [NSString stringWithUTF8String:text.c_str()]; }

// --- Settings --------------------------------------------------------------------------------
// The app's own settings, as the Android app keeps them (AppSettings.java); the game's settings
// window keeps the display and audio ones in Config.toml itself.
NSString* const kTouchControls = @"touchControls";
NSString* const kTouchOpacity = @"touchOpacity";
NSString* const kHaptics = @"haptics";
NSString* const kMotionTilt = @"motionTilt";
NSString* const kMotionShake = @"motionShake";

void RegisterDefaults() {
    [[NSUserDefaults standardUserDefaults] registerDefaults:@{
        kTouchControls : @YES, kTouchOpacity : @0.6, kHaptics : @YES, kMotionTilt : @YES, kMotionShake : @YES,
    }];
}
bool Setting(NSString* key) { return [[NSUserDefaults standardUserDefaults] boolForKey:key]; }
float Opacity() { return static_cast<float>([[NSUserDefaults standardUserDefaults] doubleForKey:kTouchOpacity]); }

// --- A window for questions before the game has one ---------------------------------------------

UIWindowScene* ActiveScene() {
    for (UIScene* scene in UIApplication.sharedApplication.connectedScenes) {
        if ([scene isKindOfClass:[UIWindowScene class]]) {
            return static_cast<UIWindowScene*>(scene);
        }
    }
    return nil;
}

UIWindow* g_promptWindow = nil;

UIViewController* PromptController() {
    if (g_promptWindow == nil) {
        // SDL connects its scene as the app finishes launching; give it a moment.
        for (int i = 0; i < 100 && ActiveScene() == nil; ++i) {
            TurnRunLoop(0.05);
        }
        UIWindowScene* scene = ActiveScene();
        g_promptWindow = scene != nil ? [[UIWindow alloc] initWithWindowScene:scene]
                                      : [[UIWindow alloc] initWithFrame:UIScreen.mainScreen.bounds];
        g_promptWindow.rootViewController = [[UIViewController alloc] init];
        g_promptWindow.rootViewController.view.backgroundColor = UIColor.blackColor;
        g_promptWindow.windowLevel = UIWindowLevelAlert;
    }
    [g_promptWindow makeKeyAndVisible];
    return g_promptWindow.rootViewController;
}

void ClosePrompts() {
    if (g_promptWindow != nil) {
        g_promptWindow.hidden = YES;
        g_promptWindow = nil;
    }
}

// Presents `controller` once `host` has finished with the last one (the document picker is still
// animating away when it reports the pick, and a presentation made then is silently dropped).
void Present(UIViewController* host, UIViewController* controller) {
    RunUntil([&] { return host.presentedViewController == nil; });
    [host presentViewController:controller animated:YES completion:nil];
}

// Shows an alert with `buttons` and waits; the index of the button pressed.
int Ask(UIViewController* host, NSString* title, NSString* message, NSArray<NSString*>* buttons) {
    auto chosen = std::make_shared<int>(-1);  // shared by the alert's blocks and the wait below
    UIAlertController* alert = [UIAlertController alertControllerWithTitle:title
                                                                   message:message
                                                            preferredStyle:UIAlertControllerStyleAlert];
    for (NSUInteger i = 0; i < buttons.count; ++i) {
        [alert addAction:[UIAlertAction actionWithTitle:buttons[i]
                                                  style:UIAlertActionStyleDefault
                                                handler:^(UIAlertAction*) { *chosen = static_cast<int>(i); }]];
    }
    Present(host, alert);
    RunUntil([&] { return *chosen >= 0; });
    return *chosen;
}

}  // namespace

// --- The folder picker ---------------------------------------------------------------------------

@interface NSMBWFolderPicker : NSObject <UIDocumentPickerDelegate>
@property(nonatomic) BOOL finished;
@property(nonatomic, strong) NSURL* url;
@end

@implementation NSMBWFolderPicker
- (void)documentPicker:(UIDocumentPickerViewController*)controller didPickDocumentsAtURLs:(NSArray<NSURL*>*)urls {
    self.url = urls.firstObject;
    self.finished = YES;
}
- (void)documentPickerWasCancelled:(UIDocumentPickerViewController*)controller {
    self.finished = YES;
}
@end

namespace {

constexpr const char* kSteps =
    "This port runs your own copy of New Super Mario Bros. Wii (USA, SMNE01, revision 1). Extract it "
    "once with Dolphin on a computer:\n\n"
    "1. In Dolphin, right-click the game and choose Properties, then the Filesystem tab.\n"
    "2. Right-click the disc at the top and choose Extract Entire Disc, into an empty folder.\n"
    "3. Copy that folder to this device (AirDrop, iCloud Drive, or Finder with the device connected).\n"
    "4. Tap Choose Folder and pick it. The game is copied into the app; you can delete the original.\n\n"
    "Or put the folder into this app's own folder (Files > On My iPhone > New Super Mario Bros. Wii) and tap Check Again.";

fs::path DataDirectory() { return RuntimeConfigFile::ApplicationDataDirectory(); }
fs::path GameDirectory() { return DataDirectory() / "game"; }

// A usable extracted game directly inside Documents, under any name ("game" first).
fs::path FindImportedGame() {
    if (fs::path root = GameFolder::Find(GameDirectory()); !root.empty() && GameFolder::Problem(root).empty()) {
        return root;
    }
    std::error_code ec;
    for (const auto& entry : fs::directory_iterator(DataDirectory(), ec)) {
        if (!entry.is_directory(ec) || entry.path().filename() == "game.importing") {
            continue;
        }
        if (fs::path root = GameFolder::Find(entry.path()); !root.empty() && GameFolder::Problem(root).empty()) {
            return root;
        }
    }
    return {};
}

// Copies the extracted game at `source` into Documents/game, with the progress in an alert.
// An empty string when it worked, else what went wrong.
std::string ImportGame(UIViewController* host, const fs::path& source) {
    const fs::path staging = DataDirectory() / "game.importing";
    std::error_code ec;
    fs::remove_all(staging, ec);

    std::atomic<uint64_t> total{0}, copied{0};
    std::atomic<bool> done{false};
    std::string failure;
    std::thread worker([&] {
        std::error_code error;
        for (const auto& entry : fs::recursive_directory_iterator(source, error)) {
            if (entry.is_regular_file(error)) total += entry.file_size(error);
        }
        for (auto it = fs::recursive_directory_iterator(source, error); !error && it != fs::recursive_directory_iterator();
             it.increment(error)) {
            const fs::path target = staging / fs::relative(it->path(), source, error);
            if (it->is_directory(error)) {
                fs::create_directories(target, error);
            } else if (it->is_regular_file(error)) {
                fs::create_directories(target.parent_path(), error);
                const uint64_t size = it->file_size(error);
                if (!error && fs::copy_file(it->path(), target, fs::copy_options::overwrite_existing, error)) {
                    copied += size;
                }
            }
        }
        if (error) {
            failure = "Copying the game failed: " + error.message() + ". Check that there is enough free space.";
        }
        done = true;
    });

    UIAlertController* progress = [UIAlertController alertControllerWithTitle:@"Copying the game"
                                                                      message:@"Starting…"
                                                               preferredStyle:UIAlertControllerStyleAlert];
    Present(host, progress);
    RunUntil([&] {
        if (total > 0) {
            progress.message = [NSString stringWithFormat:@"%llu of %llu MB", copied.load() >> 20, total.load() >> 20];
        }
        return done.load();
    });
    worker.join();
    auto dismissed = std::make_shared<bool>(false);
    [progress dismissViewControllerAnimated:YES completion:^{ *dismissed = true; }];
    RunUntil([&] { return *dismissed; });

    if (failure.empty()) {
        fs::remove_all(GameDirectory(), ec);
        fs::rename(staging, GameDirectory(), ec);
        if (ec) failure = "The copied game could not be put in place: " + ec.message();
    }
    if (!failure.empty()) fs::remove_all(staging, ec);
    return failure;
}

}  // namespace

namespace IosApp {

bool EnsureGameFolder() {
    RegisterDefaults();
    // The game is played sideways, like the remote; the home indicator stays out of the way.
    SDL_SetHint(SDL_HINT_ORIENTATIONS, "LandscapeLeft LandscapeRight");
    SDL_SetHint(SDL_HINT_IOS_HIDE_HOME_INDICATOR, "2");
    UIApplication.sharedApplication.idleTimerDisabled = YES;
    std::error_code ec;
    fs::create_directories(DataDirectory(), ec);

    std::string problem;
    for (;;) {
        if (const fs::path root = FindImportedGame(); !root.empty()) {
            RuntimeConfigFile::UseDvdRootForThisRun(root);
            RT_LOGF(RT_TAG_RUNTIME, "game folder: %s\n", RuntimeConfigFile::PathToUtf8(root).c_str());
            ClosePrompts();
            return true;
        }
        UIViewController* host = PromptController();
        const std::string message = problem.empty() ? kSteps : problem + "\n\n" + kSteps;
        if (Ask(host, ToNs(kTitle), ToNs(message), @[ @"Choose Folder…", @"Check Again" ]) == 1) {
            problem.clear();
            continue;
        }

        NSMBWFolderPicker* delegate = [[NSMBWFolderPicker alloc] init];
        UIDocumentPickerViewController* picker =
            [[UIDocumentPickerViewController alloc] initForOpeningContentTypes:@[ UTTypeFolder ]];
        picker.delegate = delegate;
        picker.allowsMultipleSelection = NO;
        Present(host, picker);
        RunUntil([&] { return delegate.finished; });
        if (delegate.url == nil) {
            problem.clear();
            continue;
        }

        NSURL* url = delegate.url;
        const bool scoped = [url startAccessingSecurityScopedResource];
        const fs::path picked(url.fileSystemRepresentation);
        const fs::path root = GameFolder::Find(picked);
        problem = GameFolder::Problem(root.empty() ? picked : root);
        if (problem.empty()) {
            problem = ImportGame(host, root);
        }
        if (scoped) [url stopAccessingSecurityScopedResource];
    }
}

void PumpWhilePaused() {
    @autoreleasepool {
        TurnRunLoop(0.1);
    }
}

[[noreturn]] void RestartForSession() {
    UIViewController* host = PromptController();
    Ask(host, @"LAN Game Ready",
        @"The game has to start again to join the room. Tap OK, then open New Super Mario Bros. Wii again from the Home Screen.",
        @[ @"OK" ]);
    std::_Exit(0);
}

}  // namespace IosApp

// --- The on-screen remote --------------------------------------------------------------------------
// TouchControlsView.java in UIKit: the same layout (everything scales with the short side), the same
// rules for sliding fingers, the 1/2 seam and the menu button, and taps on the game's menus.

namespace {
constexpr uint32_t kLeft = VirtualRemote::kTouchLeft, kRight = VirtualRemote::kTouchRight;
constexpr uint32_t kUp = VirtualRemote::kTouchUp, kDown = VirtualRemote::kTouchDown;
constexpr uint32_t kOne = VirtualRemote::kTouchOne, kTwo = VirtualRemote::kTouchTwo, kA = VirtualRemote::kTouchA;
constexpr uint32_t kPlus = VirtualRemote::kTouchPlus, kMinus = VirtualRemote::kTouchMinus;
constexpr uint32_t kShake = 0x1000, kMenu = 0x2000;
constexpr uint32_t kRemoteBits = 0x1FF;

enum Mode { kModeButtons = 1, kModePad = 2, kModeMenu = 3 };

struct Button {
    uint32_t bit;
    NSString* label;
    CGFloat reach;  // hit radius as a multiple of the drawn radius
    CGFloat x = 0, y = 0, r = 0;
    bool Hit(CGPoint p) const {
        const CGFloat dx = p.x - x, dy = p.y - y, radius = r * reach;
        return dx * dx + dy * dy <= radius * radius;
    }
};

struct Finger {
    Mode mode = kModeButtons;
    uint32_t bits = 0;
    CGPoint start{};
    NSTimeInterval startTime = 0;
    bool freeTap = false;
};
}  // namespace

@protocol NSMBWRemoteDelegate
- (void)remoteButtons:(uint32_t)buttons shake:(BOOL)shake;
- (void)remoteMenu;
@end

@interface NSMBWRemoteView : UIView
@property(nonatomic, weak) id<NSMBWRemoteDelegate> delegate;
@property(nonatomic) BOOL menuMode;          // the game's menus are up: no remote, taps go to the game
@property(nonatomic) BOOL controlsVisible;
@property(nonatomic) CGFloat opacity;
@property(nonatomic) BOOL haptics;
- (void)releaseAll;
@end

@implementation NSMBWRemoteView {
    Button _buttons[7];
    CGFloat _padX, _padY, _padR;
    NSMapTable<UITouch*, NSValue*>* _fingers;  // UITouch -> Finger*
    uint32_t _held;
    UITouch* _drag;  // the finger the game's menus follow, or nil
    UIImpactFeedbackGenerator* _feedback;
}

- (instancetype)initWithFrame:(CGRect)frame {
    if ((self = [super initWithFrame:frame])) {
        _buttons[0] = {kTwo, @"2", 1.4};
        _buttons[1] = {kOne, @"1", 1.4};
        _buttons[2] = {kA, @"A", 1.25};
        _buttons[3] = {kShake, @"SHAKE", 1.25};
        _buttons[4] = {kMinus, @"−", 1.3};
        _buttons[5] = {kPlus, @"+", 1.3};
        _buttons[6] = {kMenu, @"≡", 1.3};
        _fingers = [NSMapTable strongToStrongObjectsMapTable];
        _feedback = [[UIImpactFeedbackGenerator alloc] initWithStyle:UIImpactFeedbackStyleLight];
        _controlsVisible = YES;
        _opacity = 0.6;
        _haptics = YES;
        self.opaque = NO;
        self.backgroundColor = UIColor.clearColor;
        self.multipleTouchEnabled = YES;
        self.contentMode = UIViewContentModeRedraw;
        self.autoresizingMask = UIViewAutoresizingFlexibleWidth | UIViewAutoresizingFlexibleHeight;
    }
    return self;
}

- (void)dealloc {
    for (UITouch* touch in _fingers) delete static_cast<Finger*>([[_fingers objectForKey:touch] pointerValue]);
}

- (Finger*)finger:(UITouch*)touch { return static_cast<Finger*>([[_fingers objectForKey:touch] pointerValue]); }

- (BOOL)showsControls { return _controlsVisible && !_menuMode; }

- (void)setMenuMode:(BOOL)on {
    if (_menuMode == on) return;
    _menuMode = on;
    [self releaseAll];
}

- (void)setControlsVisible:(BOOL)visible {
    _controlsVisible = visible;
    [self releaseAll];
}

- (void)setOpacity:(CGFloat)opacity {
    _opacity = opacity;
    [self setNeedsDisplay];
}

- (void)releaseAll {
    for (UITouch* touch in _fingers) delete [self finger:touch];
    [_fingers removeAllObjects];
    if (_drag != nil) {
        _drag = nil;
        NetplayUi::Pointer(0.0f, 0.0f, false);
    }
    [self update:0];
    [self setNeedsDisplay];
}

- (void)place:(uint32_t)bit x:(CGFloat)x y:(CGFloat)y r:(CGFloat)r {
    for (Button& b : _buttons) {
        if (b.bit == bit) { b.x = x; b.y = y; b.r = r; }
    }
}

- (void)layoutSubviews {
    [super layoutSubviews];
    const CGFloat w = self.bounds.size.width, h = self.bounds.size.height;
    if (w == 0 || h == 0) return;
    // Keep the thumbs' controls clear of the camera housing on either short edge.
    const CGFloat left = self.safeAreaInsets.left, right = self.safeAreaInsets.right;
    const CGFloat u = h;
    _padR = 0.21 * u;
    _padX = left + 0.07 * u + _padR;
    _padY = 0.66 * u;
    [self place:kTwo x:w - right - 0.17 * u y:0.74 * u r:0.11 * u];
    [self place:kOne x:w - right - 0.43 * u y:0.80 * u r:0.11 * u];
    [self place:kA x:w - right - 0.43 * u y:0.52 * u r:0.075 * u];
    [self place:kShake x:w - right - 0.17 * u y:0.44 * u r:0.085 * u];
    [self place:kMinus x:w / 2 - 0.13 * u y:0.08 * u r:0.05 * u];
    [self place:kPlus x:w / 2 + 0.13 * u y:0.08 * u r:0.05 * u];
    [self place:kMenu x:w - right - 0.08 * u y:0.08 * u r:0.05 * u];
    [self setNeedsDisplay];
}

- (BOOL)onPad:(CGPoint)p {
    if (![self showsControls]) return NO;
    const CGFloat dx = p.x - _padX, dy = p.y - _padY, reach = _padR * 1.26;
    return dx * dx + dy * dy <= reach * reach;
}

- (BOOL)menuAt:(CGPoint)p { return _buttons[6].Hit(p); }

// Every button under the point (1 and 2 overlap on their seam); never the menu.
- (uint32_t)buttonsAt:(CGPoint)p {
    if (![self showsControls]) return 0;
    uint32_t bits = 0;
    for (const Button& b : _buttons) {
        if (b.bit != kMenu && b.Hit(p)) bits |= b.bit;
    }
    return bits;
}

// Eight-way: a direction counts within 67.5 degrees of its axis, so diagonals press two.
- (uint32_t)padDirection:(CGPoint)p {
    const CGFloat dx = p.x - _padX, dy = p.y - _padY;
    if (dx * dx + dy * dy < (0.22 * _padR) * (0.22 * _padR)) return 0;
    uint32_t bits = 0;
    const CGFloat slope = 0.4142136;  // tan(22.5 degrees)
    if (std::fabs(dx) >= slope * std::fabs(dy)) bits |= dx < 0 ? kLeft : kRight;
    if (std::fabs(dy) >= slope * std::fabs(dx)) bits |= dy < 0 ? kUp : kDown;
    return bits;
}

- (void)sendPointer:(CGPoint)p held:(bool)held {
    const CGSize size = self.bounds.size;
    if (size.width > 0 && size.height > 0) {
        NetplayUi::Pointer(static_cast<float>(p.x / size.width), static_cast<float>(p.y / size.height), held);
    }
}

- (void)touchesBegan:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    for (UITouch* touch in touches) {
        const CGPoint p = [touch locationInView:self];
        auto* finger = new Finger();
        finger->start = p;
        finger->startTime = touch.timestamp;
        if ([self menuAt:p]) {
            finger->mode = kModeMenu;
            finger->bits = kMenu;
        } else if ([self onPad:p]) {
            finger->mode = kModePad;
            finger->bits = [self padDirection:p];
        } else {
            finger->mode = kModeButtons;
            finger->bits = [self buttonsAt:p];
            finger->freeTap = finger->bits == 0;
        }
        [_fingers setObject:[NSValue valueWithPointer:finger] forKey:touch];
        if (_menuMode && _drag == nil && finger->mode != kModeMenu) {
            _drag = touch;
            [self sendPointer:p held:true];
        }
    }
    [self combine];
}

- (void)touchesMoved:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    for (UITouch* touch in touches) {
        Finger* finger = [self finger:touch];
        if (finger == nullptr) continue;
        const CGPoint p = [touch locationInView:self];
        if (finger->mode == kModePad) {
            finger->bits = [self padDirection:p];
        } else if (finger->mode == kModeButtons) {
            if ([self onPad:p]) {
                finger->mode = kModePad;
                finger->bits = [self padDirection:p];
            } else {
                finger->bits = [self buttonsAt:p];
            }
        } else if (finger->mode == kModeMenu) {
            finger->bits = [self menuAt:p] ? kMenu : 0;
        }
        if (touch == _drag) [self sendPointer:p held:true];
    }
    [self combine];
}

- (void)endTouches:(NSSet<UITouch*>*)touches cancelled:(BOOL)cancelled {
    bool openMenu = false;
    for (UITouch* touch in touches) {
        Finger* finger = [self finger:touch];
        if (finger == nullptr) continue;
        const CGPoint p = [touch locationInView:self];
        if (touch == _drag) {
            _drag = nil;
            [self sendPointer:p held:false];
        }
        if (!cancelled) {
            openMenu = openMenu || (finger->mode == kModeMenu && [self menuAt:p]);
            // Taps go to the game on its menus, and anywhere off the remote's buttons otherwise (the
            // settings gear over the pause menus).
            const bool tappable = _menuMode ? finger->mode != kModeMenu : finger->freeTap;
            const CGSize size = self.bounds.size;
            if (tappable && std::hypot(p.x - finger->start.x, p.y - finger->start.y) < 0.06 * size.height &&
                touch.timestamp - finger->startTime < 0.8 && size.width > 0 && size.height > 0) {
                NetplayUi::Tap(static_cast<float>(p.x / size.width), static_cast<float>(p.y / size.height));
            }
        }
        delete finger;
        [_fingers removeObjectForKey:touch];
    }
    [self combine];
    if (openMenu) [_delegate remoteMenu];
}

- (void)touchesEnded:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    [self endTouches:touches cancelled:NO];
}

- (void)touchesCancelled:(NSSet<UITouch*>*)touches withEvent:(UIEvent*)event {
    [self endTouches:touches cancelled:YES];
}

- (void)combine {
    uint32_t bits = 0;
    for (UITouch* touch in _fingers) bits |= [self finger:touch]->bits;
    [self update:bits];
}

- (void)update:(uint32_t)bits {
    if (bits == _held) return;
    const uint32_t pressed = bits & ~_held & ~kMenu;
    _held = bits;
    if (pressed != 0 && _haptics) [_feedback impactOccurred];
    [_delegate remoteButtons:bits & kRemoteBits shake:(bits & kShake) != 0];
    [self setNeedsDisplay];
}

- (void)drawRect:(CGRect)rect {
    const CGFloat base = _opacity;
    if ([self showsControls]) [self drawPad:base];
    for (const Button& b : _buttons) {
        if (![self showsControls] && b.bit != kMenu) continue;
        const bool down = (_held & b.bit) != 0;
        UIBezierPath* circle = [UIBezierPath bezierPathWithArcCenter:CGPointMake(b.x, b.y)
                                                              radius:b.r
                                                          startAngle:0
                                                            endAngle:2 * M_PI
                                                           clockwise:YES];
        [[UIColor colorWithWhite:1 alpha:down ? base * 0.75 : base / 3] setFill];
        [circle fill];
        circle.lineWidth = std::max<CGFloat>(1, b.r * 0.06);
        [[UIColor colorWithWhite:1 alpha:base] setStroke];
        [circle stroke];
        const CGFloat fontSize = b.label.length > 2 ? b.r * 0.42 : b.r * 0.9;
        NSDictionary* attributes = @{
            NSFontAttributeName : [UIFont boldSystemFontOfSize:fontSize],
            NSForegroundColorAttributeName : [UIColor colorWithWhite:down ? 0.12 : 1 alpha:std::min<CGFloat>(1, base + 0.25)],
        };
        const CGSize size = [b.label sizeWithAttributes:attributes];
        [b.label drawAtPoint:CGPointMake(b.x - size.width / 2, b.y - size.height / 2) withAttributes:attributes];
    }
}

- (void)drawPad:(CGFloat)base {
    const CGFloat arm = _padR * 0.36;  // half the width of each bar
    UIBezierPath* horizontal =
        [UIBezierPath bezierPathWithRoundedRect:CGRectMake(_padX - _padR, _padY - arm, 2 * _padR, 2 * arm)
                                   cornerRadius:arm * 0.4];
    UIBezierPath* vertical =
        [UIBezierPath bezierPathWithRoundedRect:CGRectMake(_padX - arm, _padY - _padR, 2 * arm, 2 * _padR)
                                   cornerRadius:arm * 0.4];
    [[UIColor colorWithWhite:1 alpha:base / 3] setFill];
    [[UIColor colorWithWhite:1 alpha:base] setStroke];
    for (UIBezierPath* bar in @[ horizontal, vertical ]) {
        bar.lineWidth = std::max<CGFloat>(1, _padR * 0.025);
        [bar fill];
        [bar stroke];
    }
    [self drawArrow:base bit:kLeft ux:-1 uy:0];
    [self drawArrow:base bit:kRight ux:1 uy:0];
    [self drawArrow:base bit:kUp ux:0 uy:-1];
    [self drawArrow:base bit:kDown ux:0 uy:1];
}

- (void)drawArrow:(CGFloat)base bit:(uint32_t)bit ux:(int)ux uy:(int)uy {
    const bool down = (_held & bit) != 0;
    const CGFloat tipX = _padX + ux * _padR * 0.86, tipY = _padY + uy * _padR * 0.86;
    const CGFloat backX = _padX + ux * _padR * 0.56, backY = _padY + uy * _padR * 0.56;
    const CGFloat half = _padR * 0.16;
    UIBezierPath* arrow = [UIBezierPath bezierPath];
    [arrow moveToPoint:CGPointMake(tipX, tipY)];
    [arrow addLineToPoint:CGPointMake(backX - uy * half, backY - ux * half)];
    [arrow addLineToPoint:CGPointMake(backX + uy * half, backY + ux * half)];
    [arrow closePath];
    [[UIColor colorWithWhite:1 alpha:down ? std::min<CGFloat>(1, base + 0.3) : base] setFill];
    [arrow fill];
}

@end

// --- The game around the remote ----------------------------------------------------------------------
// GameActivity.java and MotionInput.java: input from the remote and the phone's motion, the 100 ms
// menu poll that hides the remote on the game's own menus, the menu, and leaving the screen.

@interface NSMBWGame : NSObject <NSMBWRemoteDelegate>
- (instancetype)initWithWindow:(UIWindow*)window;
@end

@implementation NSMBWGame {
    UIWindow* _window;
    NSMBWRemoteView* _remote;
    CMMotionManager* _motion;
    NSTimer* _poll;
    NSTimeInterval _lastTapScreen;
    uint32_t _touchButtons;
    BOOL _touchShake, _motionShake;
    float _motionTilt;
    double _gravity[3];
    BOOL _haveGravity;
    NSTimeInterval _shakeUntil;
    float _lastTilt;
    BOOL _lastShake;
}

- (instancetype)initWithWindow:(UIWindow*)window {
    if ((self = [super init])) {
        _window = window;
        UIView* host = window.rootViewController.view ?: window;
        _remote = [[NSMBWRemoteView alloc] initWithFrame:host.bounds];
        _remote.delegate = self;
        [host addSubview:_remote];
        [self applyControlSettings];
        _motion = [[CMMotionManager alloc] init];
        _poll = [NSTimer scheduledTimerWithTimeInterval:0.1 target:self selector:@selector(pollMenu) userInfo:nil repeats:YES];
        NSNotificationCenter* center = NSNotificationCenter.defaultCenter;
        [center addObserver:self selector:@selector(willResignActive) name:UIApplicationWillResignActiveNotification object:nil];
        [center addObserver:self selector:@selector(didBecomeActive) name:UIApplicationDidBecomeActiveNotification object:nil];
        [self startMotion];
        Netplay::SetDeviceName(UIDevice.currentDevice.name.UTF8String ?: "iPhone");
    }
    return self;
}

- (void)applyControlSettings {
    _remote.controlsVisible = Setting(kTouchControls);
    _remote.opacity = Opacity();
    _remote.haptics = Setting(kHaptics);
}

// The game's menus take taps and the remote hides while they are up; it comes back a moment after
// they go, so it doesn't blink while one menu screen hands over to the next.
- (void)pollMenu {
    const NSTimeInterval now = NSProcessInfo.processInfo.systemUptime;
    if (NetplayUi::TapScreenUp()) {
        _lastTapScreen = now;
        _remote.menuMode = YES;
    } else if (now - _lastTapScreen > 0.6) {
        _remote.menuMode = NO;
    }
    // SDL may add views of its own above ours.
    [_remote.superview bringSubviewToFront:_remote];
}

- (void)pushInput {
    VirtualRemote::SetTouchState(_touchButtons, _touchShake || _motionShake, _motionTilt);
}

- (void)remoteButtons:(uint32_t)buttons shake:(BOOL)shake {
    _touchButtons = buttons;
    _touchShake = shake;
    [self pushInput];
}

// --- motion (MotionInput.java's thresholds, in Android's units and axes) ---

- (void)startMotion {
    [self stopMotion];
    const BOOL tilt = Setting(kMotionTilt), shake = Setting(kMotionShake);
    if (!_motion.accelerometerAvailable || (!tilt && !shake)) return;
    _haveGravity = NO;
    _motion.accelerometerUpdateInterval = 1.0 / 60.0;
    __weak NSMBWGame* weakSelf = self;
    [_motion startAccelerometerUpdatesToQueue:NSOperationQueue.mainQueue
                                  withHandler:^(CMAccelerometerData* data, NSError*) {
                                      if (data != nil) [weakSelf onAcceleration:data.acceleration tilt:tilt shake:shake];
                                  }];
}

- (void)stopMotion {
    [_motion stopAccelerometerUpdates];
    [self reportTilt:0 shake:NO];
}

- (void)onAcceleration:(CMAcceleration)a tilt:(BOOL)tiltOn shake:(BOOL)shakeOn {
    constexpr double kLowPass = 0.15, kShakeThreshold = 13.0, kShakeHold = 0.18;
    constexpr double kDeadZoneDeg = 4.0, kFullDeg = 30.0, kMinInPlane = 3.0;
    // Core Motion reports in g with gravity pulling negative; Android reports m/s^2 with the support
    // force positive, on the same device axes.
    const double v[3] = {-a.x * 9.81, -a.y * 9.81, -a.z * 9.81};
    if (!_haveGravity) {
        for (int i = 0; i < 3; ++i) _gravity[i] = v[i];
        _haveGravity = YES;
    } else {
        for (int i = 0; i < 3; ++i) _gravity[i] += (v[i] - _gravity[i]) * kLowPass;
    }
    const NSTimeInterval now = NSProcessInfo.processInfo.systemUptime;
    if (shakeOn) {
        const double lx = v[0] - _gravity[0], ly = v[1] - _gravity[1], lz = v[2] - _gravity[2];
        if (lx * lx + ly * ly + lz * lz > kShakeThreshold * kShakeThreshold) _shakeUntil = now + kShakeHold;
    }
    const BOOL shake = now < _shakeUntil;
    float tilt = 0;
    if (tiltOn) {
        // Gravity in screen coordinates (x right, y up) for the way the phone is turned. Android's
        // ROTATION_90 is UIKit's landscape-right (the top of the phone to the left).
        const double gx = _gravity[0], gy = _gravity[1];
        double sx = gx, sy = gy;
        switch (_window.windowScene.interfaceOrientation) {
            case UIInterfaceOrientationLandscapeRight: sx = -gy; sy = gx; break;
            case UIInterfaceOrientationPortraitUpsideDown: sx = -gx; sy = -gy; break;
            case UIInterfaceOrientationLandscapeLeft: sx = gy; sy = -gx; break;
            default: break;
        }
        if (std::hypot(sx, sy) >= kMinInPlane) {
            // Turning the phone clockwise moves "up" counter-clockwise on the screen.
            const double degrees = std::atan2(-sx, sy) * 180.0 / M_PI;
            const double magnitude = std::max(0.0, std::fabs(degrees) - kDeadZoneDeg);
            tilt = static_cast<float>(std::min(1.0, magnitude / (kFullDeg - kDeadZoneDeg)) * (degrees < 0 ? -1 : 1));
        }
    }
    [self reportTilt:tilt shake:shake];
}

- (void)reportTilt:(float)tilt shake:(BOOL)shake {
    if (std::fabs(tilt - _lastTilt) < 0.01f && shake == _lastShake) return;
    _lastTilt = tilt;
    _lastShake = shake;
    _motionTilt = tilt;
    _motionShake = shake;
    [self pushInput];
}

// --- leaving the screen (GameActivity.onPause / onResume) ---

- (void)willResignActive {
    [_remote releaseAll];
    [self stopMotion];
    // The game writes its backup Quick Save, then pauses at its next retrace (host_suspend.h).
    HostSuspend::RequestPause(true);
}

- (void)didBecomeActive {
    HostSuspend::RequestResume();
    [self startMotion];
}

// --- the menu (GameActivity.onMenu and the Android start menu's Controls settings) ---

- (void)remoteMenu {
    if (_window.rootViewController.presentedViewController != nil) return;
    [_remote releaseAll];
    const bool inSession = NetplaySession::Active();
    UIAlertController* sheet = [UIAlertController alertControllerWithTitle:@(kTitle)
                                                                   message:nil
                                                            preferredStyle:UIAlertControllerStyleActionSheet];
    __weak NSMBWGame* weakSelf = self;
    auto toggle = ^(NSString* title, NSString* key) {
        NSString* text = [NSString stringWithFormat:@"%@: %@", title, Setting(key) ? @"On" : @"Off"];
        [sheet addAction:[UIAlertAction actionWithTitle:text style:UIAlertActionStyleDefault handler:^(UIAlertAction*) {
                   [[NSUserDefaults standardUserDefaults] setBool:!Setting(key) forKey:key];
                   [weakSelf settingsChanged];
               }]];
    };
    // Aspect ratio, as the Android app's Display menu has it: Fill -> 16:9 -> 4:3 -> Fill. 16:9 and
    // Fill switch at once; the game reads 4:3 versus widescreen once at boot (display_settings.h).
    const int32_t aspect = settings_overlay::Aspect();
    NSString* aspectTitle = [NSString stringWithFormat:@"Aspect Ratio: %@",
        aspect == DisplaySettings::kAspect4x3 ? @"4:3" : aspect == DisplaySettings::kAspect16x9 ? @"16:9" : @"Fill"];
    [sheet addAction:[UIAlertAction actionWithTitle:aspectTitle style:UIAlertActionStyleDefault handler:^(UIAlertAction*) {
               const int32_t next = aspect == DisplaySettings::kAspectFill ? DisplaySettings::kAspect16x9
                                  : aspect == DisplaySettings::kAspect16x9 ? DisplaySettings::kAspect4x3
                                                                           : DisplaySettings::kAspectFill;
               settings_overlay::SetAspect(next);
               [weakSelf aspectChangedTo:next];
           }]];
    toggle(@"On-Screen Remote", kTouchControls);
    NSString* opacity = [NSString stringWithFormat:@"Remote Opacity: %d%%", static_cast<int>(std::lround(Opacity() * 100))];
    [sheet addAction:[UIAlertAction actionWithTitle:opacity style:UIAlertActionStyleDefault handler:^(UIAlertAction*) {
               // 20% steps, back to 20% after 100%.
               const double next = Opacity() >= 0.95 ? 0.2 : std::min(1.0, Opacity() + 0.2);
               [[NSUserDefaults standardUserDefaults] setDouble:next forKey:kTouchOpacity];
               [weakSelf settingsChanged];
           }]];
    toggle(@"Vibration", kHaptics);
    toggle(@"Tilt to Steer", kMotionTilt);
    toggle(@"Jolt to Shake", kMotionShake);
    if (inSession) {
        [sheet addAction:[UIAlertAction actionWithTitle:@"Leave LAN Game" style:UIAlertActionStyleDestructive handler:^(UIAlertAction*) {
                   // The host keeps the session's progress; the next start is an ordinary one.
                   NetplayStart::PrepareToLeave();
                   IosApp::RestartForSession();
               }]];
    }
    [sheet addAction:[UIAlertAction actionWithTitle:@"Back to the Game" style:UIAlertActionStyleCancel handler:nil]];
    sheet.popoverPresentationController.sourceView = _remote;
    sheet.popoverPresentationController.sourceRect = CGRectMake(_remote.bounds.size.width - 60, 0, 60, 60);
    [_window.rootViewController presentViewController:sheet animated:YES completion:nil];
}

// A switch between 4:3 and widescreen waits for the next start: say so, as the menu is the only
// place the player sees the choice.
- (void)aspectChangedTo:(int32_t)aspect {
    const bool wantsWide = aspect != DisplaySettings::kAspect4x3;
    if (wantsWide == DisplaySettings::GameWidescreen()) return;
    UIAlertController* note = [UIAlertController
        alertControllerWithTitle:@"Aspect Ratio"
                         message:wantsWide ? @"Widescreen takes effect the next time the game starts."
                                           : @"4:3 takes effect the next time the game starts."
                  preferredStyle:UIAlertControllerStyleAlert];
    [note addAction:[UIAlertAction actionWithTitle:@"OK" style:UIAlertActionStyleDefault handler:nil]];
    [_window.rootViewController presentViewController:note animated:YES completion:nil];
}

- (void)settingsChanged {
    [self applyControlSettings];
    [self startMotion];
}

@end

namespace {
NSMBWGame* g_game = nil;
}

namespace IosApp {

void AttachToWindow(SDL_Window* window) {
    if (window == nullptr || g_game != nil) return;
    auto* uiWindow = (__bridge UIWindow*)SDL_GetPointerProperty(SDL_GetWindowProperties(window),
                                                                 SDL_PROP_WINDOW_UIKIT_WINDOW_POINTER, nullptr);
    if (uiWindow == nil) {
        RT_LOGF(RT_TAG_RUNTIME, "ios: no UIKit window; the on-screen remote is unavailable\n");
        return;
    }
    ClosePrompts();
    g_game = [[NSMBWGame alloc] initWithWindow:uiWindow];
}

}  // namespace IosApp

#endif

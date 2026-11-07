// The Linux chooser (game_picker.h): a small SDL window drawn with Dear ImGui through SDL's renderer,
// before aurora creates the game window (and its own ImGui). It follows the desktop's light or dark
// theme and display scale, uses the desktop's font, and works with a gamepad (the Steam Deck). The
// pickers are SDL's: the desktop portal (GNOME's or KDE's own dialog), then zenity; kdialog and
// zenity through popen are the last resort.
#if defined(__linux__) && !defined(__ANDROID__)

#include "game_picker.h"

#include "runtime_config.h"
#include "runtime_log.h"

#include <SDL3/SDL.h>
#include <backends/imgui_impl_sdl3.h>
#include <backends/imgui_impl_sdlrenderer3.h>
#include <imgui.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace GamePicker {
namespace {

namespace fs = std::filesystem;

constexpr float kWidth = 560;        // the window's width at 100 %
constexpr float kButtonWidth = 340;  // Open ISO and Open Extracted Disc Folder

struct Window {
    SDL_Window* window = nullptr;
    SDL_Renderer* renderer = nullptr;
    ImGuiContext* context = nullptr;
    ImFont* head = nullptr;
    SDL_InitFlags subsystems = 0;
    float scale = 1;  // the display's scale, in window coordinates
    bool dark = false;
    int frames = 0;
};
Window g_window;
bool g_opened = false;

// A picker's answer, handed over from whichever thread the dialog calls back on.
struct Answer {
    std::mutex mutex;
    bool pending = false;  // a picker is open
    bool ready = false;
    bool failed = false;   // no picker could be shown
    fs::path path;         // empty when cancelled
};

std::string Quote(const std::string& text) {
    std::string quoted = "'";
    for (const char c : text) {
        quoted += c == '\'' ? std::string("'\\''") : std::string(1, c);
    }
    return quoted + "'";
}

bool HasCommand(const char* tool) {
    return std::system(("command -v " + std::string(tool) + " >/dev/null 2>&1").c_str()) == 0;
}

std::string Run(const std::string& command) {
    std::string output;
    if (FILE* pipe = popen(command.c_str(), "r")) {
        char buffer[512];
        while (std::fgets(buffer, sizeof(buffer), pipe) != nullptr) {
            output += buffer;
        }
        pclose(pipe);
    }
    while (!output.empty() && (output.back() == '\n' || output.back() == '\r')) {
        output.pop_back();
    }
    return output;
}

// zenity (GNOME and most others) or kdialog (KDE) run directly: empty when cancelled, nullopt when
// neither is installed.
std::optional<fs::path> PickWithTool(bool folder, const std::string& title) {
    if (HasCommand("zenity")) {
        const std::string kind = folder ? " --directory"
                                        : " --file-filter='Wii disc image (.iso, .wbfs) | *.iso *.wbfs *.ISO *.WBFS'"
                                          " --file-filter='All files | *'";
        return fs::path(Run("zenity --file-selection" + kind + " --title=" + Quote(title) + " 2>/dev/null"));
    }
    if (HasCommand("kdialog")) {
        const std::string kind = folder ? " --getexistingdirectory \"$HOME\""
                                        : " --getopenfilename \"$HOME\" 'Wii disc image (*.iso *.wbfs *.ISO *.WBFS)'";
        return fs::path(Run("kdialog" + kind + " --title " + Quote(title) + " 2>/dev/null"));
    }
    return std::nullopt;
}

std::string NoPickerText() {
    return "No file picker (a desktop portal, zenity or kdialog) is available. Put the extracted game next "
           "to this program in a folder named \"game\", or write its path into " +
           RuntimeConfigFile::PathToUtf8(RuntimeConfigFile::ResolveConfigPath()) +
           " under [paths] as dvd_root = '/path/to/the/folder', then start the game again.";
}

struct Request {
    std::shared_ptr<Answer> answer;
    bool folder;
    std::string title;
};

void Deliver(Answer& answer, std::optional<fs::path> path) {
    std::lock_guard<std::mutex> lock(answer.mutex);
    answer.failed = !path.has_value();
    answer.path = path.value_or(fs::path{});
    answer.ready = true;
}

void SDLCALL OnPicked(void* userdata, const char* const* files, int) {
    std::unique_ptr<Request> request(static_cast<Request*>(userdata));
    if (files == nullptr) {
        // No portal and no zenity: try the tools directly, off this thread (it may be the main one).
        RT_LOGF(RT_TAG_RUNTIME, "game picker: no file dialog (%s); trying zenity and kdialog\n", SDL_GetError());
        std::thread([request = std::move(request)] {
            Deliver(*request->answer, PickWithTool(request->folder, request->title));
        }).detach();
        return;
    }
    Deliver(*request->answer, files[0] != nullptr ? std::optional<fs::path>(fs::path(files[0])) : fs::path{});
}

void Ask(const std::shared_ptr<Answer>& answer, bool folder, const std::string& title) {
    {
        std::lock_guard<std::mutex> lock(answer->mutex);
        answer->pending = true;
        answer->ready = false;
    }
    static const SDL_DialogFileFilter kFilters[] = {
        {"Wii disc image (.iso, .wbfs)", "iso;wbfs;ISO;WBFS"},
        {"All files", "*"},
    };
    const SDL_PropertiesID props = SDL_CreateProperties();
    if (!folder) {
        SDL_SetPointerProperty(props, SDL_PROP_FILE_DIALOG_FILTERS_POINTER, const_cast<SDL_DialogFileFilter*>(kFilters));
        SDL_SetNumberProperty(props, SDL_PROP_FILE_DIALOG_NFILTERS_NUMBER, 2);
    }
    SDL_SetPointerProperty(props, SDL_PROP_FILE_DIALOG_WINDOW_POINTER, g_window.window);
    SDL_SetStringProperty(props, SDL_PROP_FILE_DIALOG_TITLE_STRING, title.c_str());
    SDL_ShowFileDialogWithProperties(folder ? SDL_FILEDIALOG_OPENFOLDER : SDL_FILEDIALOG_OPENFILE, OnPicked,
                                     new Request{answer, folder, title}, props);
    SDL_DestroyProperties(props);
}

// The desktop's sans-serif font file, or "" (ImGui's own font is then used).
std::string SystemFont() {
    const auto usable = [](const std::string& path) {
        std::error_code ec;
        const std::string extension = fs::path(path).extension().string();
        return !path.empty() && fs::is_regular_file(path, ec) &&
               (extension == ".ttf" || extension == ".otf" || extension == ".ttc");
    };
    if (HasCommand("fc-match")) {
        if (const std::string path = Run("fc-match -f '%{file}' sans-serif:style=Regular 2>/dev/null"); usable(path)) {
            return path;
        }
    }
    for (const char* path : {"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf", "/usr/share/fonts/TTF/DejaVuSans.ttf",
                             "/usr/share/fonts/dejavu/DejaVuSans.ttf", "/usr/share/fonts/noto/NotoSans-Regular.ttf",
                             "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
                             "/usr/share/fonts/google-noto/NotoSans-Regular.ttf",
                             "/usr/share/fonts/cantarell/Cantarell-Regular.otf"}) {
        if (usable(path)) {
            return path;
        }
    }
    return {};
}

// Brings the window up (hidden until the first frame has sized it). False when there's no display.
bool Open(const std::string& title) {
    if (g_window.window != nullptr) {
        return true;
    }
    if (g_opened) {
        return false;  // tried once already
    }
    g_opened = true;
    if (!SDL_InitSubSystem(SDL_INIT_VIDEO)) {
        RT_LOGF(RT_TAG_RUNTIME, "game picker: no video (%s)\n", SDL_GetError());
        return false;
    }
    g_window.subsystems = SDL_INIT_VIDEO;
    if (SDL_InitSubSystem(SDL_INIT_GAMEPAD)) {
        g_window.subsystems |= SDL_INIT_GAMEPAD;  // ImGui moves the focus with a gamepad
    }
    g_window.window = SDL_CreateWindow(title.c_str(), static_cast<int>(kWidth), 360,
                                       SDL_WINDOW_HIDDEN | SDL_WINDOW_HIGH_PIXEL_DENSITY);
    if (g_window.window != nullptr) {
        g_window.renderer = SDL_CreateRenderer(g_window.window, nullptr);
    }
    if (g_window.renderer == nullptr) {
        RT_LOGF(RT_TAG_RUNTIME, "game picker: no window (%s)\n", SDL_GetError());
        if (g_window.window != nullptr) {
            SDL_DestroyWindow(g_window.window);
            g_window.window = nullptr;
        }
        SDL_QuitSubSystem(g_window.subsystems);
        g_window.subsystems = 0;
        return false;
    }
    SDL_SetRenderVSync(g_window.renderer, 1);

    // The display's scale in pixels, and in window coordinates (they differ on Wayland, where a
    // window's size is in the compositor's logical units).
    const float display = std::max(1.0f, SDL_GetWindowDisplayScale(g_window.window));
    const float density = std::max(1.0f, SDL_GetWindowPixelDensity(g_window.window));
    g_window.scale = display / density;
    SDL_SetWindowSize(g_window.window, static_cast<int>(std::lround(kWidth * g_window.scale)),
                      static_cast<int>(std::lround(360 * g_window.scale)));

    g_window.context = ImGui::CreateContext();
    ImGui::SetCurrentContext(g_window.context);
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    if (const std::string font = SystemFont(); !font.empty()) {
        // Rasterised at the display's pixel size, laid out in window coordinates.
        io.Fonts->AddFontFromFileTTF(font.c_str(), 16.0f * display);
        g_window.head = io.Fonts->AddFontFromFileTTF(font.c_str(), 21.0f * display);
        io.FontGlobalScale = 1.0f / density;
    } else {
        io.Fonts->AddFontDefault();
        io.FontGlobalScale = std::round(g_window.scale * 1.25f);
    }

    g_window.dark = SDL_GetSystemTheme() == SDL_SYSTEM_THEME_DARK;
    if (g_window.dark) {
        ImGui::StyleColorsDark();
    } else {
        ImGui::StyleColorsLight();
    }
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowPadding = ImVec2(20, 18);
    style.FramePadding = ImVec2(12, 8);
    style.ItemSpacing = ImVec2(8, 8);
    style.FrameRounding = 6;
    style.WindowBorderSize = 0;
    style.ScaleAllSizes(g_window.scale);

    ImGui_ImplSDL3_InitForSDLRenderer(g_window.window, g_window.renderer);
    ImGui_ImplSDLRenderer3_Init(g_window.renderer);
    g_window.frames = 0;
    return true;
}

// Waits a little for input, then hands it to ImGui. False when the window was asked to close.
bool Pump(int timeoutMs) {
    bool open = true;
    SDL_Event event;
    if (SDL_WaitEventTimeout(&event, timeoutMs)) {
        do {
            ImGui_ImplSDL3_ProcessEvent(&event);
            if (event.type == SDL_EVENT_QUIT || event.type == SDL_EVENT_WINDOW_CLOSE_REQUESTED) {
                open = false;
            }
        } while (SDL_PollEvent(&event));
    }
    return open;
}

void Centred(float width) {
    ImGui::SetCursorPosX(std::max(0.0f, (ImGui::GetWindowWidth() - width) * 0.5f));
}

// One frame of `draw`, filling the window, which then fits its height to what was drawn.
template <class Draw>
void Frame(Draw&& draw) {
    ImGui_ImplSDLRenderer3_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    const ImGuiIO& io = ImGui::GetIO();
    ImGui::SetNextWindowPos(ImVec2(0, 0));
    ImGui::SetNextWindowSize(io.DisplaySize);
    ImGui::Begin("##picker", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                     ImGuiWindowFlags_NoBringToFrontOnFocus);
    draw();
    const ImGuiStyle& style = ImGui::GetStyle();
    const float used = ImGui::GetCursorPosY() - style.ItemSpacing.y + style.WindowPadding.y;
    ImGui::End();
    ImGui::Render();

    const ImVec4 background = style.Colors[ImGuiCol_WindowBg];
    SDL_SetRenderDrawColorFloat(g_window.renderer, background.x, background.y, background.z, 1.0f);
    SDL_RenderClear(g_window.renderer);
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), g_window.renderer);
    SDL_RenderPresent(g_window.renderer);

    int width = 0, height = 0;
    SDL_GetWindowSize(g_window.window, &width, &height);
    SDL_DisplayID display = SDL_GetDisplayForWindow(g_window.window);
    SDL_Rect bounds = {0, 0, 0, 100000};
    SDL_GetDisplayUsableBounds(display, &bounds);
    const int wanted = std::min(static_cast<int>(std::ceil(used)), bounds.h * 9 / 10);
    if (std::abs(wanted - height) > 1) {
        SDL_SetWindowSize(g_window.window, width, wanted);
    }
    // Shown once it has its size: the first frames only measure.
    if (++g_window.frames == 3) {
        SDL_SetWindowPosition(g_window.window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
        SDL_ShowWindow(g_window.window);
        SDL_RaiseWindow(g_window.window);
    }
}

void Wrapped(const std::string& text, const ImVec4* color = nullptr) {
    if (color != nullptr) {
        ImGui::PushStyleColor(ImGuiCol_Text, *color);
    }
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(text.c_str());
    ImGui::PopTextWrapPos();
    if (color != nullptr) {
        ImGui::PopStyleColor();
    }
}

ImVec4 ProblemColor() {
    return g_window.dark ? ImVec4(1.0f, 0.47f, 0.47f, 1.0f) : ImVec4(0.69f, 0.13f, 0.13f, 1.0f);
}

std::string Megabytes(uint64_t bytes) {
    return std::to_string((bytes + (1u << 20) - 1) >> 20);
}

// Without a window: a message box with the three choices, and zenity or kdialog.
Picked ShowWithoutWindow(const Text& text, const std::string& problem) {
    std::string message = (problem.empty() ? "" : problem + "\n\n") + text.body;
    for (;;) {
        const SDL_MessageBoxButtonData buttons[] = {
            {SDL_MESSAGEBOX_BUTTON_ESCAPEKEY_DEFAULT, 0, "Cancel"},
            {0, 2, "Open Extracted Disc Folder"},
            {SDL_MESSAGEBOX_BUTTON_RETURNKEY_DEFAULT, 1, "Open ISO"},
        };
        const std::string body = message + "\n\nOpen ISO, or Open Extracted Disc Folder.";
        const SDL_MessageBoxData data = {SDL_MESSAGEBOX_INFORMATION, nullptr, text.title.c_str(), body.c_str(), 3,
                                         buttons, nullptr};
        int pressed = 0;
        if (!SDL_ShowMessageBox(&data, &pressed) || pressed == 0) {
            return {};
        }
        const bool folder = pressed == 2;
        const std::optional<fs::path> path = PickWithTool(folder, folder ? text.folderTitle : text.isoTitle);
        if (!path) {
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_INFORMATION, text.title.c_str(), NoPickerText().c_str(), nullptr);
            return {};
        }
        if (!path->empty()) {
            return {folder ? Choice::Folder : Choice::Iso, *path};
        }
    }
}

} // namespace

Picked Show(const Text& text, const std::string& problem) {
    if (!Open(text.title)) {
        return ShowWithoutWindow(text, problem);
    }
    ImGui::SetCurrentContext(g_window.context);
    const auto answer = std::make_shared<Answer>();
    bool folderAsked = false;
    bool help = false;
    bool mods = false;   // the Mods page is up in place of the choices
    bool focus = false;  // the page just changed: its first button takes the keyboard and gamepad
    std::string notice;
    for (;;) {
        const bool open = Pump(50);
        bool pending;
        {
            std::lock_guard<std::mutex> lock(answer->mutex);
            if (answer->ready) {
                answer->ready = false;
                answer->pending = false;
                if (answer->failed) {
                    notice = NoPickerText();
                } else if (!answer->path.empty()) {
                    return {folderAsked ? Choice::Folder : Choice::Iso, answer->path};
                }
            }
            pending = answer->pending;
        }
        if (!pending && !open) {
            return {};
        }
        bool cancel = false;
        Frame([&] {
            const float s = g_window.scale;
            if (mods) {
                // Mods: nothing to manage yet. Back (or Escape) returns to the choices.
                if (g_window.head != nullptr) {
                    ImGui::PushFont(g_window.head);
                }
                ImGui::TextUnformatted("Mods");
                if (g_window.head != nullptr) {
                    ImGui::PopFont();
                }
                ImGui::Dummy(ImVec2(0, 30 * s));
                Centred(ImGui::CalcTextSize("Coming Soon!").x);
                ImGui::TextUnformatted("Coming Soon!");
                ImGui::Dummy(ImVec2(0, 30 * s));
                ImGui::Separator();
                ImGui::Spacing();
                if (focus) {
                    ImGui::SetKeyboardFocusHere();
                    focus = false;
                }
                if (ImGui::Button("Back") || ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
                    mods = false;
                    focus = true;
                }
                return;
            }
            if (!pending && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
                cancel = true;
            }
            if (g_window.head != nullptr) {
                ImGui::PushFont(g_window.head);
            }
            ImGui::TextUnformatted("Choose your game files");
            if (g_window.head != nullptr) {
                ImGui::PopFont();
            }
            ImGui::Spacing();
            Wrapped(text.body);
            const std::string& shown = notice.empty() ? problem : notice;
            if (!shown.empty()) {
                ImGui::Spacing();
                const ImVec4 color = ProblemColor();
                Wrapped(shown, &color);
            }
            ImGui::Dummy(ImVec2(0, 10 * s));

            ImGui::BeginDisabled(pending);
            const float buttonWidth = std::min(kButtonWidth * s, ImGui::GetContentRegionAvail().x);
            Centred(buttonWidth);
            if (g_window.frames == 0) {
                ImGui::SetKeyboardFocusHere();
            }
            if (ImGui::Button("Open ISO", ImVec2(buttonWidth, 0))) {
                folderAsked = false;
                notice.clear();
                Ask(answer, false, text.isoTitle);
            }
            Centred(ImGui::CalcTextSize("or").x);
            ImGui::TextDisabled("or");
            Centred(buttonWidth);
            if (ImGui::Button("Open Extracted Disc Folder", ImVec2(buttonWidth, 0))) {
                folderAsked = true;
                notice.clear();
                Ask(answer, true, text.folderTitle);
            }
            ImGui::Dummy(ImVec2(0, 4 * s));
            Centred(buttonWidth);
            if (focus) {
                ImGui::SetKeyboardFocusHere();  // back from the Mods page, on its button
                focus = false;
            }
            if (ImGui::Button("Mods", ImVec2(buttonWidth, 0))) {
                mods = true;
                focus = true;
            }
            ImGui::EndDisabled();
            ImGui::Dummy(ImVec2(0, 10 * s));

            if (help) {
                const float wrap = ImGui::GetContentRegionAvail().x - 2 * ImGui::GetStyle().WindowPadding.x;
                const float textHeight = ImGui::CalcTextSize(text.help.c_str(), nullptr, false, wrap).y;
                const float height = std::min(textHeight + 2 * ImGui::GetStyle().WindowPadding.y, 240 * s);
                ImGui::BeginChild("##help", ImVec2(0, height), ImGuiChildFlags_Borders);
                Wrapped(text.help);
                ImGui::EndChild();
            }
            ImGui::Separator();
            ImGui::Spacing();
            if (ImGui::Button(help ? "Hide Help" : "Help")) {
                help = !help;
            }
            const float cancelWidth = ImGui::CalcTextSize("Cancel").x + 2 * ImGui::GetStyle().FramePadding.x;
            ImGui::SameLine(ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x - cancelWidth);
            ImGui::BeginDisabled(pending);
            if (ImGui::Button("Cancel")) {
                cancel = true;
            }
            ImGui::EndDisabled();
        });
        if (cancel) {
            return {};
        }
    }
}

std::string RunWithProgress(const Text& text, const std::string& line, const Job& job) {
    std::atomic<bool> cancel{false};
    std::atomic<uint64_t> done{0}, total{0};
    std::atomic<bool> finished{false};
    std::string result;
    std::thread worker([&] {
        result = job(
            [&](uint64_t d, uint64_t t) {
                done.store(d, std::memory_order_relaxed);
                total.store(t, std::memory_order_relaxed);
            },
            cancel);
        finished.store(true);
    });
    if (Open(text.title)) {
        ImGui::SetCurrentContext(g_window.context);
        while (!finished.load()) {
            if (!Pump(33)) {
                cancel.store(true);
            }
            Frame([&] {
                if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
                    cancel.store(true);
                }
                const uint64_t d = done.load(std::memory_order_relaxed);
                const uint64_t t = total.load(std::memory_order_relaxed);
                const float fraction = t != 0 ? static_cast<float>(static_cast<double>(d) / static_cast<double>(t)) : 0.0f;
                ImGui::TextUnformatted(line.c_str());
                const std::string percent = std::to_string(static_cast<int>(fraction * 100)) + " %";
                ImGui::ProgressBar(fraction, ImVec2(-FLT_MIN, 0), percent.c_str());
                ImGui::TextUnformatted(
                    (cancel.load() ? std::string("Cancelling...") : Megabytes(d) + " of " + Megabytes(t) + " MB").c_str());
                ImGui::Spacing();
                const float cancelWidth = ImGui::CalcTextSize("Cancel").x + 2 * ImGui::GetStyle().FramePadding.x;
                ImGui::SetCursorPosX(ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x - cancelWidth);
                ImGui::BeginDisabled(cancel.load());
                if (ImGui::Button("Cancel")) {
                    cancel.store(true);
                }
                ImGui::EndDisabled();
            });
        }
    }
    worker.join();
    return result;
}

void Close() {
    if (g_window.context != nullptr) {
        ImGui::SetCurrentContext(g_window.context);
        ImGui_ImplSDLRenderer3_Shutdown();
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext(g_window.context);
    }
    if (g_window.renderer != nullptr) {
        SDL_DestroyRenderer(g_window.renderer);
    }
    if (g_window.window != nullptr) {
        SDL_DestroyWindow(g_window.window);
    }
    if (g_window.subsystems != 0) {
        SDL_QuitSubSystem(g_window.subsystems);
    }
    g_window = {};
}

} // namespace GamePicker

#endif

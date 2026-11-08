// The Windows chooser (game_picker.h): a native dialog built in memory, the shell's file and folder
// pickers, and the shell's progress dialog. Every font comes from the dialog's DS_SHELLFONT, so
// Windows scales it for the monitor's DPI (the manifest makes the process per-monitor aware).
#if defined(_WIN32)

#include "game_picker.h"

#include "runtime_log.h"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <commctrl.h>
#include <shlobj.h>
#include <shobjidl.h>

#include <cstring>
#include <memory>
#include <vector>

namespace GamePicker {
namespace {

namespace fs = std::filesystem;

enum : int {
    kIcon = 100,
    kHead,
    kBody,
    kProblem,
    kIso,
    kOr,
    kFolder,
    kMods,
    kLine,
    kHelp,
};

constexpr int kWidth = 260;    // the dialog's width, in dialog units
constexpr int kMargin = 7;
constexpr int kTextLeft = 36;  // the text column, right of the icon
constexpr int kBigButtonWidth = 150;

std::wstring Widen(const std::string& utf8) {
    const int length = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), nullptr, 0);
    std::wstring wide(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), wide.data(), length);
    return wide;
}

// COM for the pickers and the progress dialog, while one is up.
struct ComScope {
    HRESULT result = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    ~ComScope() {
        if (SUCCEEDED(result)) {
            CoUninitialize();
        }
    }
};

// The shell's picker for a disc image (.iso, .wbfs) or a folder; empty when cancelled.
fs::path PickPath(HWND owner, bool folder, const std::string& title) {
    ComScope com;
    fs::path result;
    IFileOpenDialog* dialog = nullptr;
    if (FAILED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog)))) {
        return result;
    }
    DWORD options = 0;
    dialog->GetOptions(&options);
    options |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST;
    options |= folder ? FOS_PICKFOLDERS : FOS_FILEMUSTEXIST;
    dialog->SetOptions(options);
    if (!folder) {
        const COMDLG_FILTERSPEC types[] = {
            {L"Wii disc image (*.iso, *.wbfs)", L"*.iso;*.wbfs"},
            {L"All files", L"*.*"},
        };
        dialog->SetFileTypes(2, types);
        dialog->SetFileTypeIndex(1);
    }
    dialog->SetTitle(Widen(title).c_str());
    if (SUCCEEDED(dialog->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item))) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path != nullptr) {
                result = fs::path(path);
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dialog->Release();
    return result;
}

// A DLGTEMPLATEEX and its items, written word by word.
class Template {
public:
    void Word(WORD value) { m_words.push_back(value); }
    void Dword(DWORD value) {
        Word(LOWORD(value));
        Word(HIWORD(value));
    }
    void String(const std::wstring& text) {
        for (const wchar_t c : text) {
            Word(static_cast<WORD>(c));
        }
        Word(0);
    }
    // Items start on a DWORD boundary.
    void Align() {
        if (m_words.size() % 2 != 0) {
            Word(0);
        }
    }
    void Item(int id, WORD windowClass, DWORD style, const std::wstring& text) {
        Align();
        Dword(0);  // help ID
        Dword(0);  // extended style
        Dword(WS_CHILD | WS_VISIBLE | style);
        Word(0);  // x, y, cx, cy: laid out in WM_INITDIALOG
        Word(0);
        Word(10);
        Word(10);
        Dword(static_cast<DWORD>(id));
        Word(0xFFFF);
        Word(windowClass);
        String(text);
        Word(0);  // no creation data
    }
    // A copy that starts on a DWORD boundary, as DialogBoxIndirectParamW requires.
    std::unique_ptr<DWORD[]> Build() const {
        auto copy = std::make_unique<DWORD[]>((m_words.size() + 1) / 2);
        std::memcpy(copy.get(), m_words.data(), m_words.size() * sizeof(WORD));
        return copy;
    }
    size_t Size() const { return m_words.size(); }

private:
    std::vector<WORD> m_words;
};

constexpr WORD kButtonClass = 0x0080;
constexpr WORD kStaticClass = 0x0082;

struct State {
    const Text* text = nullptr;
    const std::string* problem = nullptr;
    Picked picked;
    HFONT headFont = nullptr;
    HICON icon = nullptr;
    HICON smallIcon = nullptr;
};

// Dialog units to pixels for this dialog's font and DPI.
int Dlu(HWND dialog, int units, bool vertical) {
    RECT rect = {0, 0, vertical ? 0 : units, vertical ? units : 0};
    MapDialogRect(dialog, &rect);
    return vertical ? rect.bottom : rect.right;
}

int TextHeight(HWND control, HFONT font, int width) {
    const int length = GetWindowTextLengthW(control);
    std::wstring text(static_cast<size_t>(length) + 1, L'\0');
    GetWindowTextW(control, text.data(), length + 1);
    text.resize(static_cast<size_t>(length));
    HDC dc = GetDC(control);
    const HGDIOBJ old = SelectObject(dc, font);
    RECT rect = {0, 0, width, 0};
    DrawTextW(dc, text.c_str(), -1, &rect, DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX | DT_EDITCONTROL);
    SelectObject(dc, old);
    ReleaseDC(control, dc);
    return rect.bottom;
}

// The heading's font: the dialog's, larger, as Windows' own task dialogs head their text.
void MakeHeadFont(HWND dialog, State& state) {
    if (state.headFont != nullptr) {
        DeleteObject(state.headFont);
    }
    LOGFONTW font = {};
    GetObjectW(reinterpret_cast<HFONT>(SendMessageW(dialog, WM_GETFONT, 0, 0)), sizeof(font), &font);
    font.lfHeight = font.lfHeight * 4 / 3;
    state.headFont = CreateFontIndirectW(&font);
    SendDlgItemMessageW(dialog, kHead, WM_SETFONT, reinterpret_cast<WPARAM>(state.headFont), FALSE);
}

void LoadIcons(HWND dialog, State& state) {
    const HINSTANCE instance = GetModuleHandleW(nullptr);
    const int size = Dlu(dialog, 18, false);
    if (state.icon != nullptr) {
        DestroyIcon(state.icon);
    }
    // The .ico holds several sizes; LoadImageW picks the nearest one, so the icon stays sharp.
    state.icon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON, size, size, 0));
    SendDlgItemMessageW(dialog, kIcon, STM_SETICON, reinterpret_cast<WPARAM>(state.icon), 0);
    if (state.smallIcon == nullptr) {
        state.smallIcon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(1), IMAGE_ICON,
                                                        GetSystemMetrics(SM_CXSMICON),
                                                        GetSystemMetrics(SM_CYSMICON), 0));
        SendMessageW(dialog, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(state.smallIcon));
        SendMessageW(dialog, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(state.icon));
    }
}

// Places every control for the current font and DPI, and sizes the dialog to fit.
void Layout(HWND dialog, State& state) {
    const auto x = [&](int units) { return Dlu(dialog, units, false); };
    const auto y = [&](int units) { return Dlu(dialog, units, true); };
    const HFONT font = reinterpret_cast<HFONT>(SendMessageW(dialog, WM_GETFONT, 0, 0));
    const int width = x(kWidth);
    const int textLeft = x(kTextLeft);
    const int textWidth = width - textLeft - x(kMargin);
    const auto place = [&](int id, int left, int top, int w, int h) {
        SetWindowPos(GetDlgItem(dialog, id), nullptr, left, top, w, h, SWP_NOZORDER | SWP_NOACTIVATE);
    };

    int top = y(kMargin);
    const int iconSize = x(18);
    place(kIcon, x(kMargin), top, iconSize, iconSize);
    int height = TextHeight(GetDlgItem(dialog, kHead), state.headFont, textWidth);
    place(kHead, textLeft, top, textWidth, height);
    top += height + y(5);
    height = TextHeight(GetDlgItem(dialog, kBody), font, textWidth);
    place(kBody, textLeft, top, textWidth, height);
    top += height + y(6);
    if (!state.problem->empty()) {
        height = TextHeight(GetDlgItem(dialog, kProblem), font, textWidth);
        place(kProblem, textLeft, top, textWidth, height);
        top += height + y(6);
    }
    top += y(4);
    const int bigWidth = x(kBigButtonWidth);
    const int bigLeft = (width - bigWidth) / 2;
    const int bigHeight = y(16);
    place(kIso, bigLeft, top, bigWidth, bigHeight);
    top += bigHeight + y(3);
    height = TextHeight(GetDlgItem(dialog, kOr), font, bigWidth);
    place(kOr, bigLeft, top, bigWidth, height);
    top += height + y(3);
    place(kFolder, bigLeft, top, bigWidth, bigHeight);
    top += bigHeight + y(6);
    if (state.text->mods) {
        place(kMods, bigLeft, top, bigWidth, bigHeight);
        top += bigHeight + y(6);
    }
    top += y(5);
    place(kLine, x(kMargin), top, width - 2 * x(kMargin), 2);
    top += y(7);
    const int smallWidth = x(50);
    const int smallHeight = y(14);
    place(kHelp, x(kMargin), top, smallWidth, smallHeight);
    place(IDCANCEL, width - x(kMargin) - smallWidth, top, smallWidth, smallHeight);
    top += smallHeight + y(kMargin);

    // The window around that client area, kept centred where it is.
    RECT window, client;
    GetWindowRect(dialog, &window);
    GetClientRect(dialog, &client);
    const int outerWidth = width + (window.right - window.left) - client.right;
    const int outerHeight = top + (window.bottom - window.top) - client.bottom;
    MONITORINFO monitor = {sizeof(monitor)};
    GetMonitorInfoW(MonitorFromWindow(dialog, MONITOR_DEFAULTTONEAREST), &monitor);
    const RECT& area = monitor.rcWork;
    const int left = area.left + ((area.right - area.left) - outerWidth) / 2;
    const int windowTop = area.top + ((area.bottom - area.top) - outerHeight) / 2;
    SetWindowPos(dialog, nullptr, left, windowTop, outerWidth, outerHeight, SWP_NOZORDER | SWP_NOACTIVATE);
}

constexpr UINT kRelayout = WM_APP + 1;

INT_PTR CALLBACK DialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam) {
    State* state = reinterpret_cast<State*>(GetWindowLongPtrW(dialog, DWLP_USER));
    switch (message) {
    case WM_INITDIALOG:
        state = reinterpret_cast<State*>(lParam);
        SetWindowLongPtrW(dialog, DWLP_USER, lParam);
        ShowWindow(GetDlgItem(dialog, kProblem), state->problem->empty() ? SW_HIDE : SW_SHOW);
        ShowWindow(GetDlgItem(dialog, kMods), state->text->mods ? SW_SHOW : SW_HIDE);
        MakeHeadFont(dialog, *state);
        LoadIcons(dialog, *state);
        Layout(dialog, *state);
        SetFocus(GetDlgItem(dialog, kIso));
        return FALSE;  // focus set here
    case WM_DPICHANGED:
        // Windows rescales the dialog's own font and controls; the heading's font and the layout
        // follow once it has.
        PostMessageW(dialog, kRelayout, 0, 0);
        return FALSE;
    case kRelayout:
        MakeHeadFont(dialog, *state);
        LoadIcons(dialog, *state);
        Layout(dialog, *state);
        return TRUE;
    case WM_CTLCOLORSTATIC: {
        const int id = GetDlgCtrlID(reinterpret_cast<HWND>(lParam));
        if (id == kProblem || id == kHead) {
            const HDC dc = reinterpret_cast<HDC>(wParam);
            SetTextColor(dc, id == kProblem ? RGB(176, 32, 32) : RGB(0, 51, 153));
            SetBkMode(dc, TRANSPARENT);
            return reinterpret_cast<INT_PTR>(GetSysColorBrush(COLOR_BTNFACE));
        }
        return FALSE;
    }
    case WM_COMMAND:
        switch (LOWORD(wParam)) {
        case kIso:
        case kFolder: {
            const bool folder = LOWORD(wParam) == kFolder;
            const fs::path path = PickPath(dialog, folder, folder ? state->text->folderTitle : state->text->isoTitle);
            if (!path.empty()) {
                state->picked = {folder ? Choice::Folder : Choice::Iso, path};
                EndDialog(dialog, IDOK);
            }
            return TRUE;
        }
        case kHelp:
            MessageBoxW(dialog, Widen(state->text->help).c_str(), Widen(state->text->title).c_str(),
                        MB_OK | MB_ICONINFORMATION);
            return TRUE;
        case kMods:  // nothing to manage yet; OK goes back to the choices
            MessageBoxW(dialog, L"Coming Soon!", L"Mods", MB_OK | MB_ICONINFORMATION);
            return TRUE;
        case IDCANCEL:  // also Esc and the title bar's X
            state->picked = {};
            EndDialog(dialog, IDCANCEL);
            return TRUE;
        }
        break;
    }
    return FALSE;
}

std::string Megabytes(uint64_t bytes) {
    return std::to_string((bytes + (1u << 20) - 1) >> 20);
}

} // namespace

Picked Show(const Text& text, const std::string& problem) {
    static const bool commonControls = [] {
        INITCOMMONCONTROLSEX init = {sizeof(init), ICC_STANDARD_CLASSES};
        return InitCommonControlsEx(&init) != FALSE;
    }();
    (void)commonControls;

    Template t;
    t.Word(1);       // DLGTEMPLATEEX version
    t.Word(0xFFFF);  // signature
    t.Dword(0);      // help ID
    t.Dword(WS_EX_APPWINDOW);  // a taskbar button, though nothing owns it
    t.Dword(DS_SHELLFONT | DS_MODALFRAME | DS_SETFOREGROUND | WS_POPUP | WS_CAPTION | WS_SYSMENU);
    t.Word(11);      // items
    t.Word(0);       // x, y, cx, cy: Layout() sizes and centres it
    t.Word(0);
    t.Word(kWidth);
    t.Word(150);
    t.Word(0);       // no menu
    t.Word(0);       // the standard dialog class
    t.String(Widen(text.title));
    t.Word(9);       // 9 pt Segoe UI (MS Shell Dlg with DS_SHELLFONT)
    t.Word(FW_NORMAL);
    t.Word(MAKEWORD(FALSE, DEFAULT_CHARSET));
    t.String(L"MS Shell Dlg");
    t.Item(kIcon, kStaticClass, SS_ICON | SS_REALSIZECONTROL, L"");
    t.Item(kHead, kStaticClass, SS_LEFT | SS_NOPREFIX, L"Choose your game files");
    t.Item(kBody, kStaticClass, SS_LEFT | SS_NOPREFIX, Widen(text.body));
    t.Item(kProblem, kStaticClass, SS_LEFT | SS_NOPREFIX, Widen(problem));
    t.Item(kIso, kButtonClass, BS_DEFPUSHBUTTON | WS_TABSTOP | WS_GROUP, L"&Open ISO");
    t.Item(kOr, kStaticClass, SS_CENTER | SS_NOPREFIX, L"or");
    t.Item(kFolder, kButtonClass, BS_PUSHBUTTON | WS_TABSTOP | WS_GROUP, L"Open &Extracted Disc Folder");
    t.Item(kMods, kButtonClass, BS_PUSHBUTTON | WS_TABSTOP | WS_GROUP, L"&Mods");
    t.Item(kLine, kStaticClass, SS_ETCHEDHORZ, L"");
    t.Item(kHelp, kButtonClass, BS_PUSHBUTTON | WS_TABSTOP | WS_GROUP, L"&Help");
    t.Item(IDCANCEL, kButtonClass, BS_PUSHBUTTON | WS_TABSTOP | WS_GROUP, L"Cancel");
    const std::unique_ptr<DWORD[]> data = t.Build();

    State state;
    state.text = &text;
    state.problem = &problem;
    const INT_PTR result = DialogBoxIndirectParamW(GetModuleHandleW(nullptr),
                                                   reinterpret_cast<LPCDLGTEMPLATEW>(data.get()), nullptr,
                                                   DialogProc, reinterpret_cast<LPARAM>(&state));
    if (result == -1) {
        RT_LOGF(RT_TAG_RUNTIME, "game picker: the dialog didn't open (error %lu)\n", GetLastError());
    }
    if (state.headFont != nullptr) {
        DeleteObject(state.headFont);
    }
    if (state.icon != nullptr) {
        DestroyIcon(state.icon);
    }
    if (state.smallIcon != nullptr) {
        DestroyIcon(state.smallIcon);
    }
    return state.picked;
}

std::string RunWithProgress(const Text& text, const std::string& line, const Job& job) {
    ComScope com;
    IProgressDialog* dialog = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_ProgressDialog, nullptr, CLSCTX_INPROC_SERVER, IID_IProgressDialog,
                                   reinterpret_cast<void**>(&dialog)))) {
        dialog->SetTitle(Widen(text.title).c_str());
        dialog->SetLine(1, Widen(line).c_str(), FALSE, nullptr);
        dialog->SetCancelMsg(L"Cancelling...", nullptr);
        // Its own thread draws it, so the job runs here.
        dialog->StartProgressDialog(nullptr, nullptr, PROGDLG_NORMAL | PROGDLG_AUTOTIME | PROGDLG_NOMINIMIZE, nullptr);
        dialog->Timer(PDTIMER_RESET, nullptr);
    }
    std::atomic<bool> cancel{false};
    uint64_t shown = 0;
    bool first = true;
    const Progress progress = [&](uint64_t done, uint64_t total) {
        if (dialog == nullptr || (!first && done - shown < (1u << 20) && done != total)) {
            return;
        }
        first = false;
        shown = done;
        dialog->SetProgress64(done, total);
        dialog->SetLine(2, Widen(Megabytes(done) + " of " + Megabytes(total) + " MB").c_str(), FALSE, nullptr);
        if (dialog->HasUserCancelled()) {
            cancel.store(true, std::memory_order_relaxed);
        }
    };
    std::string result = job(progress, cancel);
    if (dialog != nullptr) {
        dialog->StopProgressDialog();
        dialog->Release();
    }
    return result;
}

void ShowError(const Text& text, const std::string& message) {
    MessageBoxW(nullptr, Widen(message).c_str(), Widen(text.title).c_str(), MB_OK | MB_ICONERROR | MB_SETFOREGROUND);
}

void Close() {}

} // namespace GamePicker

#endif

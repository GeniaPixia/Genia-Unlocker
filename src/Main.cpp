#include <windows.h>
#include <windowsx.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <objbase.h>

#include <algorithm>
#include <atomic>
#include <cwchar>
#include <cwctype>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "BatchScanWindow.h"
#include "LockScanner.h"
#include "ModernTheme.h"
#include "WindowsIntegration.h"
#include "resource.h"

#pragma comment(lib, "Comctl32.lib")
#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "Shell32.lib")

#ifndef FOFX_RECYCLEONDELETE
#define FOFX_RECYCLEONDELETE 0x00080000
#endif

namespace {

constexpr wchar_t kWindowClass[] = L"GeniaUnlockerWindow";
constexpr wchar_t kWindowTitle[] = L"Genia Unlocker";
constexpr wchar_t kAppVersionDisplay[] = L"0.6.0 Dev";
constexpr wchar_t kRepositoryUrl[] = L"https://github.com/GeniaPixia/Genia-Unlocker";
constexpr wchar_t kIssuesUrl[] = L"https://github.com/GeniaPixia/Genia-Unlocker/issues/new";
constexpr wchar_t kSettingsWindowClass[] = L"GeniaUnlockerSettingsWindow";
constexpr wchar_t kAboutWindowClass[] = L"GeniaUnlockerAboutWindow";
constexpr wchar_t kDetailsWindowClass[] = L"GeniaUnlockerDetailsWindow";
constexpr wchar_t kConfirmWindowClass[] = L"GeniaUnlockerConfirmWindow";
constexpr wchar_t kMutexName[] = L"Local\\GeniaUnlocker.Singleton.1";
constexpr UINT WM_APP_SCAN_DONE = WM_APP + 10;
constexpr UINT WM_APP_TRAY = WM_APP + 11;

constexpr int IDC_TARGET = 1001;
constexpr int IDC_FILE = 1002;
constexpr int IDC_FOLDER = 1003;
constexpr int IDC_LIST = 1004;
constexpr int IDC_UNLOCK = 1005;
constexpr int IDC_TERMINATE = 1006;
constexpr int IDC_RETRY = 1007;
constexpr int IDC_STATUS = 1010;
constexpr int IDC_APP_TITLE = 1012;
constexpr int IDC_TARGET_LABEL = 1014;
constexpr int IDC_DELETE = 1015;
constexpr int IDC_UNLOCK_DELETE = 1016;
constexpr int IDC_SETTINGS = 1017;
constexpr int IDC_DETAILS = 1018;
constexpr int IDC_FORCE_UNLOCK = 1019;
constexpr int IDC_BATCH = 1020;

constexpr int IDC_SETTINGS_TITLE = 2001;
constexpr int IDC_SETTINGS_SHELL = 2002;
constexpr int IDC_SETTINGS_AUTOSTART = 2003;
constexpr int IDC_SETTINGS_PERMANENT = 2004;
constexpr int IDC_SETTINGS_CLOSE = 2005;
constexpr int IDC_SETTINGS_ABOUT = 2006;
constexpr int IDC_SETTINGS_VERSION = 2007;
constexpr int IDC_SETTINGS_NOTE = 2008;

constexpr int IDC_ABOUT_CLOSE = 3001;
constexpr int IDC_ABOUT_GITHUB = 3002;
constexpr int IDC_ABOUT_ISSUE = 3003;
constexpr int IDC_ABOUT_COPY = 3004;

constexpr int IDC_DETAILS_CLOSE = 4001;
constexpr int IDC_DETAILS_COPY = 4002;
constexpr int IDC_DETAILS_EDIT = 4003;

constexpr int IDC_CONFIRM_CLOSE = 6001;
constexpr int IDC_CONFIRM_OK = 6002;
constexpr int IDC_CONFIRM_CANCEL = 6003;

constexpr UINT ID_TRAY_OPEN = 5001;
constexpr UINT ID_TRAY_FILE = 5002;
constexpr UINT ID_TRAY_EXIT = 5003;
constexpr UINT ID_TRAY_ABOUT = 5004;
constexpr UINT ID_PROCESS_FORCE_UNLOCK = 5101;
constexpr UINT ID_PROCESS_TERMINATE = 5102;
constexpr UINT ID_PROCESS_OPEN_EXE = 5103;
constexpr UINT ID_PROCESS_COPY_OBJECT = 5104;
constexpr UINT ID_PROCESS_COPY_PID = 5105;

// CreateWindow* uses the HMENU parameter as a child-control ID.
// On x64 HMENU is pointer-sized, so convert through INT_PTR explicitly.
HMENU ControlId(int id) noexcept {
    return reinterpret_cast<HMENU>(static_cast<INT_PTR>(id));
}

void SetButtonCheck(HWND button, UINT state) noexcept {
    SendMessageW(button, BM_SETCHECK, static_cast<WPARAM>(state), 0);
}

UINT GetButtonCheck(HWND button) noexcept {
    return static_cast<UINT>(SendMessageW(button, BM_GETCHECK, 0, 0));
}

struct ScanPayload {
    unsigned long long generation{};
    ULONGLONG elapsedMs{};
    ScanResult result;
};

enum class PendingAction {
    None,
    VerifyUnlock,
    VerifyForceUnlock,
    UnlockDeleteAfterUnlock,
    UnlockDeleteAfterForceUnlock,
    UnlockDeleteAfterTerminate
};

struct AppState {
    HWND hwnd{};
    HWND targetEdit{};
    HWND list{};
    HWND status{};
    HWND settingsWindow{};
    HWND aboutWindow{};
    HWND detailsWindow{};
    HWND targetTooltip{};
    HFONT font{};
    HFONT titleFont{};
    HBRUSH windowBrush{};
    HBRUSH surfaceBrush{};
    ModernTheme::Palette palette{};
    int dpi{96};
    HICON icon{};
    HIMAGELIST processImageList{};
    NOTIFYICONDATAW tray{};
    UINT taskbarCreated{};
    std::wstring target;
    std::vector<LockProcess> locks;
    std::atomic<unsigned long long> scanGeneration{0};
    DWORD lastInaccessibleProcessCount{};
    DWORD lastRestartManagerError{};
    DWORD lastInspectedDiskHandleCount{};
    DWORD lastDeleteShareProbeError{};
    bool lastHandleTypeFilterAvailable{};
    bool lastDeleteShareProbeSucceeded{};
    bool lastTargetExists{};
    bool lastTargetIsDirectory{};
    bool scanInProgress{};
    PendingAction pendingAction{PendingAction::None};
    std::wstring pendingUnlockError;
    std::wstring pendingForceUnlockDetails;
    std::wstring lastStatusDetails;
    ULONGLONG lastScanDurationMs{};
    int savedColumnWidths[4]{};
    bool savedColumnsValid{};
    bool columnsInitialized{};
    int restoredWindowWidth{780};
    int restoredWindowHeight{460};
    bool permanentDeleteDefault{};
    bool exiting{};
};

struct ParsedArgs {
    bool tray{};
    bool installShell{};
    bool uninstallShell{};
    bool enableAutostart{};
    bool disableAutostart{};
    bool showHelp{};
    bool elevatedInstance{};
    DWORD elevatedKillPid{};
    std::vector<DWORD> elevatedKillPids;
    std::wstring elevatedForceUnlockTarget;
    std::wstring target;
};

std::wstring ErrorMessage(DWORD error) {
    LPWSTR buffer = nullptr;
    DWORD len = FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                                   FORMAT_MESSAGE_IGNORE_INSERTS,
                               nullptr, error, 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::wstring result;
    if (len && buffer) {
        result.assign(buffer, len);
        while (!result.empty() && (result.back() == L'\r' || result.back() == L'\n')) {
            result.pop_back();
        }
        LocalFree(buffer);
    } else {
        result = L"Error " + std::to_wstring(error);
    }
    return result;
}

std::wstring GetPortableSettingsPath() {
    std::wstring exe = GetExecutablePath();
    const size_t slash = exe.find_last_of(L"\\/");
    if (slash == std::wstring::npos) {
        return L"GeniaUnlocker.ini";
    }
    return exe.substr(0, slash + 1) + L"GeniaUnlocker.ini";
}

bool LoadPermanentDeleteDefault() {
    const std::wstring ini = GetPortableSettingsPath();
    return GetPrivateProfileIntW(L"Actions", L"PermanentDelete", 0, ini.c_str()) != 0;
}

bool SavePermanentDeleteDefault(bool enabled) {
    const std::wstring ini = GetPortableSettingsPath();
    return WritePrivateProfileStringW(L"Actions", L"PermanentDelete",
                                      enabled ? L"1" : L"0", ini.c_str()) != FALSE;
}


int ClampPreference(int value, int minimum, int maximum) {
    return (std::max)(minimum, (std::min)(maximum, value));
}

void LoadLayoutPreferences(AppState& state) {
    const std::wstring ini = GetPortableSettingsPath();
    state.restoredWindowWidth = ClampPreference(
        GetPrivateProfileIntW(L"Window", L"Width", 780, ini.c_str()), 720, 2400);
    state.restoredWindowHeight = ClampPreference(
        GetPrivateProfileIntW(L"Window", L"Height", 460, ini.c_str()), 400, 1800);

    static constexpr const wchar_t* kColumnKeys[] = {
        L"ProcessWidth", L"PidWidth", L"MethodWidth", L"ObjectWidth"
    };
    bool valid = true;
    for (int i = 0; i < 4; ++i) {
        state.savedColumnWidths[i] =
            GetPrivateProfileIntW(L"Columns", kColumnKeys[i], 0, ini.c_str());
        if (state.savedColumnWidths[i] < 40 || state.savedColumnWidths[i] > 2400) {
            valid = false;
        }
    }
    state.savedColumnsValid = valid;
}

void CaptureColumnWidths(AppState* state) {
    if (!state || !state->list || state->dpi <= 0) return;
    bool valid = true;
    for (int i = 0; i < 4; ++i) {
        const int physical = ListView_GetColumnWidth(state->list, i);
        const int logical = MulDiv(physical, 96, state->dpi);
        state->savedColumnWidths[i] = logical;
        if (logical < 40 || logical > 2400) {
            valid = false;
        }
    }
    state->savedColumnsValid = valid;
}

void SaveLayoutPreferences(AppState* state) {
    if (!state) return;
    const std::wstring ini = GetPortableSettingsPath();

    auto writeInt = [&](const wchar_t* section, const wchar_t* key, int value) {
        wchar_t buffer[32]{};
        _itow_s(value, buffer, 10);
        WritePrivateProfileStringW(section, key, buffer, ini.c_str());
    };

    if (state->hwnd && IsWindow(state->hwnd)) {
        WINDOWPLACEMENT placement{};
        placement.length = sizeof(placement);
        if (GetWindowPlacement(state->hwnd, &placement)) {
            const RECT& rc = placement.rcNormalPosition;
            const int dpi = state->dpi > 0 ? state->dpi : 96;
            const int width = MulDiv(rc.right - rc.left, 96, dpi);
            const int height = MulDiv(rc.bottom - rc.top, 96, dpi);
            writeInt(L"Window", L"Width", ClampPreference(width, 720, 2400));
            writeInt(L"Window", L"Height", ClampPreference(height, 400, 1800));
        }
    }

    CaptureColumnWidths(state);
    if (state->savedColumnsValid) {
        static constexpr const wchar_t* kColumnKeys[] = {
            L"ProcessWidth", L"PidWidth", L"MethodWidth", L"ObjectWidth"
        };
        for (int i = 0; i < 4; ++i) {
            writeInt(L"Columns", kColumnKeys[i], state->savedColumnWidths[i]);
        }
    }
}

ParsedArgs ParseArguments() {
    ParsedArgs parsed;
    int argc = 0;
    LPWSTR* argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (!argv) {
        return parsed;
    }

    for (int i = 1; i < argc; ++i) {
        std::wstring arg = argv[i];
        if (_wcsicmp(arg.c_str(), L"--tray") == 0) {
            parsed.tray = true;
        } else if (_wcsicmp(arg.c_str(), L"--target") == 0 && i + 1 < argc) {
            parsed.target = argv[++i];
        } else if (_wcsicmp(arg.c_str(), L"--install-shell") == 0) {
            parsed.installShell = true;
        } else if (_wcsicmp(arg.c_str(), L"--uninstall-shell") == 0) {
            parsed.uninstallShell = true;
        } else if (_wcsicmp(arg.c_str(), L"--enable-autostart") == 0) {
            parsed.enableAutostart = true;
        } else if (_wcsicmp(arg.c_str(), L"--disable-autostart") == 0) {
            parsed.disableAutostart = true;
        } else if (_wcsicmp(arg.c_str(), L"--elevated-instance") == 0) {
            parsed.elevatedInstance = true;
        } else if (_wcsicmp(arg.c_str(), L"--elevated-kill") == 0 && i + 1 < argc) {
            parsed.elevatedKillPid = wcstoul(argv[++i], nullptr, 10);
        } else if (_wcsicmp(arg.c_str(), L"--elevated-kill-list") == 0 && i + 1 < argc) {
            std::wstring list = argv[++i];
            size_t start = 0;
            while (start < list.size()) {
                size_t comma = list.find(L',', start);
                std::wstring token = list.substr(
                    start,
                    comma == std::wstring::npos ? std::wstring::npos : comma - start);
                DWORD pid = wcstoul(token.c_str(), nullptr, 10);
                if (pid != 0) {
                    parsed.elevatedKillPids.push_back(pid);
                }
                if (comma == std::wstring::npos) {
                    break;
                }
                start = comma + 1;
            }
        } else if (_wcsicmp(arg.c_str(), L"--elevated-force-unlock") == 0 && i + 1 < argc) {
            parsed.elevatedForceUnlockTarget = argv[++i];
        } else if (_wcsicmp(arg.c_str(), L"--help") == 0 || _wcsicmp(arg.c_str(), L"-h") == 0) {
            parsed.showHelp = true;
        } else if (!arg.empty() && arg[0] != L'-' && parsed.target.empty()) {
            // Makes drag-and-drop onto GeniaUnlocker.exe useful too.
            parsed.target = arg;
        }
    }
    LocalFree(argv);
    return parsed;
}

void SetFont(HWND control, HFONT font) {
    if (control && font) {
        SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(font), TRUE);
    }
}

// A classic Win32 EDIT control does not vertically center its text when the
// control is made taller than the font. Genia Unlocker uses 38 logical-pixel
// input fields, so without a formatting rectangle the caret hugs the top edge.
// We use a one-line multiline EDIT and explicitly center its formatting area.
void CenterEditContent(HWND edit, int dpi) {
    if (!edit) return;

    RECT client{};
    if (!GetClientRect(edit, &client)) return;

    HDC dc = GetDC(edit);
    if (!dc) return;

    HFONT font = reinterpret_cast<HFONT>(SendMessageW(edit, WM_GETFONT, 0, 0));
    HGDIOBJ oldFont = nullptr;
    if (font) {
        oldFont = SelectObject(dc, font);
    }

    TEXTMETRICW tm{};
    GetTextMetricsW(dc, &tm);

    if (oldFont) {
        SelectObject(dc, oldFont);
    }
    ReleaseDC(edit, dc);

    const int clientHeight = client.bottom - client.top;
    const int lineHeight = (std::max)(1, static_cast<int>(tm.tmHeight));
    const int verticalPadding = (std::max)(0, (clientHeight - lineHeight) / 2);
    const int horizontalPadding = MulDiv(10, dpi > 0 ? dpi : 96, 96);

    RECT format{};
    format.left = horizontalPadding;
    format.top = verticalPadding;
    format.right = (std::max)(format.left + 1, client.right - horizontalPadding);
    format.bottom = (std::min)(client.bottom, format.top + lineHeight + 1);

    SendMessageW(edit, EM_SETRECTNP, 0, reinterpret_cast<LPARAM>(&format));
    InvalidateRect(edit, nullptr, TRUE);
}

void SetStatusWithDetails(AppState* state,
                          const std::wstring& visibleText,
                          const std::wstring& detailsText) {
    if (!state) return;
    state->lastStatusDetails = detailsText.empty() ? visibleText : detailsText;
    if (state->status) {
        SetWindowTextW(state->status, visibleText.c_str());
    }
    HWND details = state->hwnd ? GetDlgItem(state->hwnd, IDC_DETAILS) : nullptr;
    if (details) {
        EnableWindow(details, !state->lastStatusDetails.empty());
    }
}

void SetStatus(AppState* state, const std::wstring& text) {
    SetStatusWithDetails(state, text, text);
}

int Scale(const AppState* state, int logicalPixels) {
    return MulDiv(logicalPixels, state && state->dpi > 0 ? state->dpi : 96, 96);
}

HFONT CreateModernFont(int dpi, int pointSize, int weight, const wchar_t* face) {
    return CreateFontW(-MulDiv(pointSize, dpi, 72), 0, 0, 0, weight, FALSE, FALSE, FALSE,
                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                       CLEARTYPE_QUALITY, DEFAULT_PITCH | FF_DONTCARE, face);
}

void RecreateFonts(AppState* state) {
    if (!state) return;
    if (state->font) DeleteObject(state->font);
    if (state->titleFont) DeleteObject(state->titleFont);

    state->font = CreateModernFont(state->dpi, 9, FW_NORMAL, L"Segoe UI Variable Text");
    state->titleFont = CreateModernFont(state->dpi, 14, FW_SEMIBOLD, L"Segoe UI Variable Display");
}

void ApplyFonts(AppState* state) {
    if (!state || !state->hwnd) return;
    for (int id : {IDC_TARGET, IDC_FILE, IDC_FOLDER, IDC_LIST, IDC_UNLOCK, IDC_FORCE_UNLOCK,
                   IDC_TERMINATE, IDC_DELETE, IDC_UNLOCK_DELETE, IDC_RETRY, IDC_STATUS,
                   IDC_TARGET_LABEL, IDC_SETTINGS, IDC_DETAILS, IDC_BATCH}) {
        SetFont(GetDlgItem(state->hwnd, id), state->font);
    }
    SetFont(GetDlgItem(state->hwnd, IDC_APP_TITLE), state->titleFont);
}


LRESULT CALLBACK FlatHeaderSubclassProc(HWND hwnd,
                                        UINT msg,
                                        WPARAM wParam,
                                        LPARAM lParam,
                                        UINT_PTR subclassId,
                                        DWORD_PTR refData) {
    auto* state = reinterpret_cast<AppState*>(refData);

    switch (msg) {
    case WM_ERASEBKGND:
        return 1;

    case WM_PAINT:
        if (state) {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd, &ps);

            ModernTheme::Palette palette = state->palette;
            if (palette.text == 0) {
                palette = ModernTheme::QueryPalette();
            }

            RECT client{};
            GetClientRect(hwnd, &client);
            const COLORREF headerFill = palette.dark ? palette.surface : palette.window;
            HBRUSH background = CreateSolidBrush(headerFill);
            FillRect(dc, &client, background);
            DeleteObject(background);

            const int count = Header_GetItemCount(hwnd);
            HFONT font = state->font
                ? state->font
                : reinterpret_cast<HFONT>(SendMessageW(hwnd, WM_GETFONT, 0, 0));
            HGDIOBJ oldFont = font ? SelectObject(dc, font) : nullptr;
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, palette.text);

            HPEN separator = CreatePen(PS_SOLID, 1, palette.border);
            HGDIOBJ oldPen = SelectObject(dc, separator);

            for (int i = 0; i < count; ++i) {
                RECT itemRc{};
                if (!Header_GetItemRect(hwnd, i, &itemRc)) {
                    continue;
                }

                wchar_t textBuffer[256]{};
                HDITEMW item{};
                item.mask = HDI_TEXT | HDI_FORMAT;
                item.pszText = textBuffer;
                item.cchTextMax = static_cast<int>(_countof(textBuffer));
                Header_GetItem(hwnd, i, &item);

                HBRUSH itemBrush = CreateSolidBrush(headerFill);
                FillRect(dc, &itemRc, itemBrush);
                DeleteObject(itemBrush);

                MoveToEx(dc, itemRc.right - 1, itemRc.top, nullptr);
                LineTo(dc, itemRc.right - 1, itemRc.bottom);
                MoveToEx(dc, itemRc.left, itemRc.bottom - 1, nullptr);
                LineTo(dc, itemRc.right, itemRc.bottom - 1);

                RECT textRc = itemRc;
                textRc.left += Scale(state, 7);
                textRc.right -= Scale(state, 6);

                UINT format = DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS;
                if ((item.fmt & HDF_RIGHT) != 0) {
                    format |= DT_RIGHT;
                } else if ((item.fmt & HDF_CENTER) != 0) {
                    format |= DT_CENTER;
                } else {
                    format |= DT_LEFT;
                }
                DrawTextW(dc, textBuffer, -1, &textRc, format);
            }

            SelectObject(dc, oldPen);
            DeleteObject(separator);
            if (oldFont) SelectObject(dc, oldFont);

            EndPaint(hwnd, &ps);
            return 0;
        }
        break;

    case WM_THEMECHANGED:
    case WM_SETTINGCHANGE:
        InvalidateRect(hwnd, nullptr, TRUE);
        return 0;

    case WM_NCDESTROY:
        RemoveWindowSubclass(hwnd, FlatHeaderSubclassProc, subclassId);
        break;
    }

    return DefSubclassProc(hwnd, msg, wParam, lParam);
}

void UpdateListColumns(AppState* state, int width) {
    if (!state || !state->list || width <= 0 || state->columnsInitialized) return;

    if (state->savedColumnsValid) {
        for (int i = 0; i < 4; ++i) {
            ListView_SetColumnWidth(
                state->list, i, MulDiv(state->savedColumnWidths[i], state->dpi, 96));
        }
        state->columnsInitialized = true;
        return;
    }

    const int usable = width - Scale(state, 6);
    const int processW = (std::max)(Scale(state, 138), usable * 24 / 100);
    const int pidW = (std::max)(Scale(state, 52), usable * 8 / 100);
    const int detectedW = (std::max)(Scale(state, 92), usable * 14 / 100);
    const int executableW = (std::max)(Scale(state, 250), usable - processW - pidW - detectedW);
    ListView_SetColumnWidth(state->list, 0, processW);
    ListView_SetColumnWidth(state->list, 1, pidW);
    ListView_SetColumnWidth(state->list, 2, detectedW);
    ListView_SetColumnWidth(state->list, 3, executableW);
    state->columnsInitialized = true;
}

void UpdateTheme(AppState* state) {
    if (!state || !state->hwnd) return;

    state->palette = ModernTheme::QueryPalette();
    if (state->windowBrush) DeleteObject(state->windowBrush);
    if (state->surfaceBrush) DeleteObject(state->surfaceBrush);
    state->windowBrush = CreateSolidBrush(state->palette.window);
    state->surfaceBrush = CreateSolidBrush(state->palette.surface);

    ModernTheme::ApplyWindowChrome(state->hwnd, state->palette.dark);
    for (int id : {IDC_TARGET, IDC_LIST}) {
        ModernTheme::ApplyControlTheme(GetDlgItem(state->hwnd, id), state->palette.dark);
    }
    HWND header = ListView_GetHeader(state->list);
    ModernTheme::ApplyControlTheme(header, state->palette.dark);

    ListView_SetBkColor(state->list, state->palette.surface);
    ListView_SetTextBkColor(state->list, state->palette.surface);
    ListView_SetTextColor(state->list, state->palette.text);
    if (header) {
        InvalidateRect(header, nullptr, TRUE);
        UpdateWindow(header);
    }

    RedrawWindow(state->hwnd, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
}

std::wstring PickPath(HWND owner, bool folder) {
    IFileOpenDialog* dialog = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&dialog));
    if (FAILED(hr) || !dialog) {
        return {};
    }

    FILEOPENDIALOGOPTIONS options{};
    dialog->GetOptions(&options);
    options |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST;
    if (folder) {
        options |= FOS_PICKFOLDERS;
        dialog->SetTitle(L"Select folder to inspect");
    } else {
        options |= FOS_FILEMUSTEXIST;
        dialog->SetTitle(L"Select file to inspect");
    }
    dialog->SetOptions(options);

    std::wstring result;
    if (SUCCEEDED(dialog->Show(owner))) {
        IShellItem* item = nullptr;
        if (SUCCEEDED(dialog->GetResult(&item)) && item) {
            PWSTR path = nullptr;
            if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                result = path;
                CoTaskMemFree(path);
            }
            item->Release();
        }
    }
    dialog->Release();
    return result;
}

void RecreateProcessImageList(AppState* state) {
    if (!state || !state->list) return;

    if (state->processImageList) {
        ListView_SetImageList(state->list, nullptr, LVSIL_SMALL);
        ImageList_Destroy(state->processImageList);
        state->processImageList = nullptr;
    }

    const int iconSize = Scale(state, 18);
    state->processImageList = ImageList_Create(
        iconSize, iconSize, ILC_COLOR32 | ILC_MASK, 8, 8);
    if (state->processImageList) {
        ListView_SetImageList(state->list, state->processImageList, LVSIL_SMALL);
    }
}

int AddProcessIcon(AppState* state, const std::wstring& executablePath) {
    if (!state || !state->processImageList) return -1;

    HICON icon = nullptr;
    if (!executablePath.empty()) {
        SHFILEINFOW info{};
        if (SHGetFileInfoW(executablePath.c_str(), 0, &info, sizeof(info),
                           SHGFI_ICON | SHGFI_SMALLICON) != 0) {
            icon = info.hIcon;
        }
    }

    const HICON iconToAdd = icon ? icon : state->icon;
    const int imageIndex = iconToAdd
        ? ImageList_AddIcon(state->processImageList, iconToAdd)
        : -1;
    if (icon) {
        DestroyIcon(icon);
    }
    return imageIndex;
}

void RevealProcessExecutable(AppState* state, int row) {
    if (!state || row < 0 || static_cast<size_t>(row) >= state->locks.size()) return;

    const std::wstring& path = state->locks[static_cast<size_t>(row)].path;
    if (path.empty() || GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) {
        MessageBoxW(state->hwnd, L"The executable path is unavailable.",
                    kWindowTitle, MB_ICONINFORMATION);
        return;
    }

    std::wstring parameters = L"/select,\"" + path + L"\"";
    HINSTANCE result = ShellExecuteW(state->hwnd, L"open", L"explorer.exe",
                                     parameters.c_str(), nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) <= 32) {
        MessageBoxW(state->hwnd, L"Could not open the executable location in Explorer.",
                    kWindowTitle, MB_ICONERROR);
    }
}

std::wstring PrimaryLockedObject(const LockProcess& process) {
    if (process.lockedObjects.empty()) {
        return L"(target / object unavailable)";
    }
    std::wstring text = process.lockedObjects.front();
    if (process.lockedObjects.size() > 1) {
        text += L"  (+" + std::to_wstring(process.lockedObjects.size() - 1) + L")";
    }
    return text;
}

std::wstring BuildProcessInfoText(const LockProcess& process) {
    std::wstring text = process.name + L" (PID " + std::to_wstring(process.pid) + L")";
    if (!process.path.empty()) {
        text += L"\nExecutable: " + process.path;
    }
    if (!process.lockedObjects.empty()) {
        text += L"\n\nLocked objects:";
        const size_t limit = (std::min)(process.lockedObjects.size(), static_cast<size_t>(12));
        for (size_t i = 0; i < limit; ++i) {
            text += L"\n  • " + process.lockedObjects[i];
        }
        if (process.lockedObjects.size() > limit) {
            text += L"\n  • and " + std::to_wstring(process.lockedObjects.size() - limit) + L" more";
        }
    }
    return text;
}

bool CopyTextToClipboard(HWND owner, const std::wstring& text) {
    if (text.empty() || !OpenClipboard(owner)) {
        return false;
    }
    EmptyClipboard();
    const SIZE_T bytes = (text.size() + 1) * sizeof(wchar_t);
    HGLOBAL memory = GlobalAlloc(GMEM_MOVEABLE, bytes);
    if (!memory) {
        CloseClipboard();
        return false;
    }
    void* data = GlobalLock(memory);
    if (!data) {
        GlobalFree(memory);
        CloseClipboard();
        return false;
    }
    memcpy(data, text.c_str(), bytes);
    GlobalUnlock(memory);
    if (!SetClipboardData(CF_UNICODETEXT, memory)) {
        GlobalFree(memory);
        CloseClipboard();
        return false;
    }
    CloseClipboard();
    return true;
}

void OpenExternalUrl(HWND owner, const wchar_t* url) {
    if (!url || !*url) return;
    HINSTANCE result = ShellExecuteW(owner, L"open", url, nullptr, nullptr, SW_SHOWNORMAL);
    if (reinterpret_cast<INT_PTR>(result) <= 32) {
        MessageBoxW(owner, L"Windows could not open the requested link.", kWindowTitle, MB_ICONERROR);
    }
}

struct AboutState {
    AppState* app{};
    HWND closeButton{};
    HWND githubButton{};
    HWND issueButton{};
    HWND copyButton{};
    HFONT font{};
    HFONT titleFont{};
    HFONT versionFont{};
};

std::wstring BuildVersionInfoText() {
    return L"Genia Unlocker " + std::wstring(kAppVersionDisplay) +
           L"\r\nAuthor: GeniaPixia"
           L"\r\nLicense: MIT"
           L"\r\nPlatform: Windows x64 · Native Win32 C++"
           L"\r\nSource: " + std::wstring(kRepositoryUrl);
}

LRESULT CALLBACK AboutWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* about = reinterpret_cast<AboutState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        about = reinterpret_cast<AboutState*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(about));
    }

    AppState* app = about ? about->app : nullptr;
    switch (msg) {
    case WM_CREATE: {
        if (!about || !app) return -1;
        const int dpi = GetDpiForWindow(hwnd) > 0 ? GetDpiForWindow(hwnd) : 96;
        auto sc = [dpi](int px) { return MulDiv(px, dpi, 96); };

        about->font = CreateModernFont(dpi, 9, FW_NORMAL, L"Segoe UI Variable Text");
        about->titleFont = CreateModernFont(dpi, 15, FW_SEMIBOLD, L"Segoe UI Variable Display");
        about->versionFont = CreateModernFont(dpi, 9, FW_SEMIBOLD, L"Segoe UI Variable Text");

        about->closeButton = CreateWindowW(
            L"BUTTON", L"×",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            sc(408), sc(5), sc(24), sc(22), hwnd,
            ControlId(IDC_ABOUT_CLOSE), nullptr, nullptr);
        about->githubButton = CreateWindowW(
            L"BUTTON", L"GitHub",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            sc(16), sc(248), sc(72), sc(28), hwnd,
            ControlId(IDC_ABOUT_GITHUB), nullptr, nullptr);
        about->issueButton = CreateWindowW(
            L"BUTTON", L"Report issue",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            sc(96), sc(248), sc(92), sc(28), hwnd,
            ControlId(IDC_ABOUT_ISSUE), nullptr, nullptr);
        about->copyButton = CreateWindowW(
            L"BUTTON", L"Copy info",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            sc(196), sc(248), sc(86), sc(28), hwnd,
            ControlId(IDC_ABOUT_COPY), nullptr, nullptr);
        HWND close = CreateWindowW(
            L"BUTTON", L"Close",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            sc(350), sc(248), sc(74), sc(28), hwnd,
            ControlId(IDCANCEL), nullptr, nullptr);

        for (HWND button : {about->closeButton, about->githubButton,
                            about->issueButton, about->copyButton, close}) {
            SetFont(button, about->font);
            ModernTheme::ApplyControlTheme(button, app->palette.dark);
        }
        ModernTheme::ApplyWindowChrome(hwnd, app->palette.dark);
        return 0;
    }

    case WM_ERASEBKGND:
        if (app && app->windowBrush) {
            RECT rc{};
            GetClientRect(hwnd, &rc);
            FillRect(reinterpret_cast<HDC>(wParam), &rc, app->windowBrush);
            return 1;
        }
        break;

    case WM_PAINT:
        if (about && app) {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd, &ps);
            RECT rc{};
            GetClientRect(hwnd, &rc);
            const int dpi = GetDpiForWindow(hwnd) > 0 ? GetDpiForWindow(hwnd) : 96;
            auto sc = [dpi](int px) { return MulDiv(px, dpi, 96); };

            SetBkMode(dc, TRANSPARENT);

            RECT captionRc{sc(10), sc(3), rc.right - sc(48), sc(29)};
            SetTextColor(dc, app->palette.text);
            HGDIOBJ oldFont = about->font ? SelectObject(dc, about->font) : nullptr;
            DrawTextW(dc, L"About Genia Unlocker", -1, &captionRc,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            if (oldFont) SelectObject(dc, oldFont);

            if (app->icon) {
                DrawIconEx(dc, sc(18), sc(48), app->icon, sc(42), sc(42),
                           0, nullptr, DI_NORMAL);
            }

            RECT titleRc{sc(74), sc(46), rc.right - sc(18), sc(73)};
            oldFont = about->titleFont ? SelectObject(dc, about->titleFont) : nullptr;
            SetTextColor(dc, app->palette.text);
            DrawTextW(dc, L"Genia Unlocker", -1, &titleRc,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            if (oldFont) SelectObject(dc, oldFont);

            RECT versionRc{sc(74), sc(72), rc.right - sc(18), sc(94)};
            oldFont = about->versionFont ? SelectObject(dc, about->versionFont) : nullptr;
            SetTextColor(dc, app->palette.accent);
            std::wstring version = L"v" + std::wstring(kAppVersionDisplay);
            DrawTextW(dc, version.c_str(), -1, &versionRc,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            if (oldFont) SelectObject(dc, oldFont);

            oldFont = about->font ? SelectObject(dc, about->font) : nullptr;
            SetTextColor(dc, app->palette.text);

            RECT descriptionRc{sc(18), sc(105), rc.right - sc(18), sc(129)};
            DrawTextW(dc, L"Lightweight native Windows file/folder unlocker.",
                      -1, &descriptionRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

            RECT metaRc{sc(18), sc(136), rc.right - sc(18), sc(199)};
            const wchar_t* meta =
                L"Author: GeniaPixia\r\n"
                L"License: MIT\r\n"
                L"Platform: Windows x64 · Native Win32 C++";
            DrawTextW(dc, meta, -1, &metaRc, DT_LEFT | DT_TOP);

            SetTextColor(dc, app->palette.muted);
            RECT sourceRc{sc(18), sc(201), rc.right - sc(18), sc(222)};
            DrawTextW(dc, L"Source: github.com/GeniaPixia/Genia-Unlocker",
                      -1, &sourceRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

            RECT copyrightRc{sc(18), sc(220), rc.right - sc(18), sc(236)};
            DrawTextW(dc, L"Copyright © 2026 GeniaPixia",
                      -1, &copyrightRc, DT_LEFT | DT_VCENTER | DT_SINGLELINE);

            if (oldFont) SelectObject(dc, oldFont);

            HPEN pen = CreatePen(PS_SOLID, 1, app->palette.border);
            HGDIOBJ oldPen = SelectObject(dc, pen);
            MoveToEx(dc, sc(16), sc(242), nullptr);
            LineTo(dc, rc.right - sc(16), sc(242));
            SelectObject(dc, oldPen);
            DeleteObject(pen);

            EndPaint(hwnd, &ps);
            return 0;
        }
        break;

    case WM_DRAWITEM:
        if (app) {
            const auto* draw = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
            if (draw && draw->CtlType == ODT_BUTTON) {
                ModernTheme::ButtonKind kind =
                    draw->CtlID == IDC_ABOUT_GITHUB
                        ? ModernTheme::ButtonKind::Primary
                        : ModernTheme::ButtonKind::Secondary;
                ModernTheme::DrawButton(*draw, app->palette, kind);
                return TRUE;
            }
        }
        break;

    case WM_CTLCOLORBTN:
        if (app) {
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, app->palette.text);
            return reinterpret_cast<LRESULT>(app->windowBrush);
        }
        break;

    case WM_COMMAND:
        if (!about || !app) break;
        switch (LOWORD(wParam)) {
        case IDC_ABOUT_CLOSE:
        case IDCANCEL:
            if (HIWORD(wParam) == BN_CLICKED) {
                DestroyWindow(hwnd);
            }
            return 0;
        case IDC_ABOUT_GITHUB:
            if (HIWORD(wParam) == BN_CLICKED) {
                OpenExternalUrl(hwnd, kRepositoryUrl);
            }
            return 0;
        case IDC_ABOUT_ISSUE:
            if (HIWORD(wParam) == BN_CLICKED) {
                OpenExternalUrl(hwnd, kIssuesUrl);
            }
            return 0;
        case IDC_ABOUT_COPY:
            if (HIWORD(wParam) == BN_CLICKED) {
                if (CopyTextToClipboard(hwnd, BuildVersionInfoText())) {
                    SetWindowTextW(about->copyButton, L"Copied");
                    InvalidateRect(about->copyButton, nullptr, TRUE);
                } else {
                    MessageBoxW(hwnd, L"Could not copy version information.",
                                kWindowTitle, MB_ICONERROR);
                }
            }
            return 0;
        }
        break;

    case WM_DPICHANGED:
        if (about && app) {
            const int dpi = HIWORD(wParam) > 0 ? HIWORD(wParam) : 96;
            const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
            if (suggested) {
                SetWindowPos(hwnd, nullptr,
                             suggested->left, suggested->top,
                             suggested->right - suggested->left,
                             suggested->bottom - suggested->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            }
            auto sc = [dpi](int px) { return MulDiv(px, dpi, 96); };

            if (about->font) DeleteObject(about->font);
            if (about->titleFont) DeleteObject(about->titleFont);
            if (about->versionFont) DeleteObject(about->versionFont);
            about->font = CreateModernFont(dpi, 9, FW_NORMAL, L"Segoe UI Variable Text");
            about->titleFont = CreateModernFont(dpi, 15, FW_SEMIBOLD, L"Segoe UI Variable Display");
            about->versionFont = CreateModernFont(dpi, 9, FW_SEMIBOLD, L"Segoe UI Variable Text");

            MoveWindow(about->closeButton, sc(408), sc(5), sc(24), sc(22), TRUE);
            MoveWindow(about->githubButton, sc(16), sc(248), sc(72), sc(28), TRUE);
            MoveWindow(about->issueButton, sc(96), sc(248), sc(92), sc(28), TRUE);
            MoveWindow(about->copyButton, sc(196), sc(248), sc(86), sc(28), TRUE);
            MoveWindow(GetDlgItem(hwnd, IDCANCEL), sc(350), sc(248), sc(74), sc(28), TRUE);

            for (HWND button : {about->closeButton, about->githubButton,
                                about->issueButton, about->copyButton,
                                GetDlgItem(hwnd, IDCANCEL)}) {
                SetFont(button, about->font);
            }
            InvalidateRect(hwnd, nullptr, TRUE);
        }
        return 0;

    case WM_THEMECHANGED:
    case WM_SETTINGCHANGE:
        if (app) {
            ModernTheme::ApplyWindowChrome(hwnd, app->palette.dark);
            InvalidateRect(hwnd, nullptr, TRUE);
        }
        return 0;

    case WM_NCHITTEST: {
        POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (about && about->closeButton) {
            RECT closeRc{};
            GetWindowRect(about->closeButton, &closeRc);
            if (PtInRect(&closeRc, pt)) {
                return HTCLIENT;
            }
        }
        RECT windowRc{};
        GetWindowRect(hwnd, &windowRc);
        const int dpi = GetDpiForWindow(hwnd) > 0 ? GetDpiForWindow(hwnd) : 96;
        const int captionHeight = MulDiv(32, dpi, 96);
        if (pt.y >= windowRc.top && pt.y < windowRc.top + captionHeight) {
            return HTCAPTION;
        }
        break;
    }

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        if (app && app->aboutWindow == hwnd) {
            app->aboutWindow = nullptr;
        }
        if (about) {
            if (about->font) DeleteObject(about->font);
            if (about->titleFont) DeleteObject(about->titleFont);
            if (about->versionFont) DeleteObject(about->versionFont);
        }
        delete about;
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void ShowAboutDialog(AppState* state) {
    if (!state || !state->hwnd) return;

    if (state->aboutWindow && IsWindow(state->aboutWindow)) {
        ShowWindow(state->aboutWindow, SW_RESTORE);
        SetForegroundWindow(state->aboutWindow);
        return;
    }

    HINSTANCE instance = reinterpret_cast<HINSTANCE>(
        GetWindowLongPtrW(state->hwnd, GWLP_HINSTANCE));
    static bool classReady = false;
    if (!classReady) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = AboutWindowProc;
        wc.hInstance = instance;
        wc.hIcon = state->icon;
        wc.hIconSm = state->icon;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = kAboutWindowClass;
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            MessageBoxW(state->hwnd, L"Could not create the About window.",
                        kWindowTitle, MB_ICONERROR);
            return;
        }
        classReady = true;
    }

    const int dpi = GetDpiForWindow(state->hwnd) > 0 ? GetDpiForWindow(state->hwnd) : 96;
    const int width = MulDiv(440, dpi, 96);
    const int height = MulDiv(288, dpi, 96);

    HWND centerOwner =
        state->settingsWindow && IsWindowVisible(state->settingsWindow)
            ? state->settingsWindow
            : state->hwnd;
    RECT owner{};
    GetWindowRect(centerOwner, &owner);
    const int x = owner.left + ((owner.right - owner.left) - width) / 2;
    const int y = owner.top + ((owner.bottom - owner.top) - height) / 2;

    auto* about = new AboutState{};
    about->app = state;
    HWND window = CreateWindowExW(
        WS_EX_TOOLWINDOW,
        kAboutWindowClass,
        L"About Genia Unlocker",
        WS_POPUP | WS_BORDER,
        x, y, width, height,
        centerOwner, nullptr, instance, about);
    if (!window) {
        delete about;
        MessageBoxW(state->hwnd, L"Could not open About.",
                    kWindowTitle, MB_ICONERROR);
        return;
    }

    state->aboutWindow = window;
    ShowWindow(window, SW_SHOWNORMAL);
    UpdateWindow(window);
}


struct ConfirmState {
    AppState* app{};
    std::wstring title;
    std::wstring message;
    std::wstring confirmText;
    std::wstring cancelText;
    bool danger{};
    bool defaultConfirm{};
    bool result{};
    bool finished{};
    HWND closeButton{};
    HWND confirmButton{};
    HWND cancelButton{};
    HFONT font{};
    HFONT titleFont{};
};

LRESULT CALLBACK ConfirmWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* confirm = reinterpret_cast<ConfirmState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        confirm = reinterpret_cast<ConfirmState*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(confirm));
    }

    AppState* app = confirm ? confirm->app : nullptr;
    switch (msg) {
    case WM_CREATE: {
        if (!confirm || !app) return -1;
        const int dpi = GetDpiForWindow(hwnd) > 0 ? GetDpiForWindow(hwnd) : 96;
        auto sc = [dpi](int px) { return MulDiv(px, dpi, 96); };

        confirm->font = CreateModernFont(dpi, 9, FW_NORMAL, L"Segoe UI Variable Text");
        confirm->titleFont = CreateModernFont(dpi, 13, FW_SEMIBOLD, L"Segoe UI Variable Display");

        RECT rc{};
        GetClientRect(hwnd, &rc);
        confirm->closeButton = CreateWindowW(
            L"BUTTON", L"×",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            rc.right - sc(32), sc(5), sc(24), sc(22), hwnd,
            ControlId(IDC_CONFIRM_CLOSE), nullptr, nullptr);

        const int buttonY = rc.bottom - sc(46);
        const int cancelW = sc(82);
        const int confirmW = sc(128);
        confirm->cancelButton = CreateWindowW(
            L"BUTTON", confirm->cancelText.c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            rc.right - sc(16) - cancelW, buttonY, cancelW, sc(30), hwnd,
            ControlId(IDC_CONFIRM_CANCEL), nullptr, nullptr);
        confirm->confirmButton = CreateWindowW(
            L"BUTTON", confirm->confirmText.c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            rc.right - sc(24) - cancelW - confirmW, buttonY, confirmW, sc(30), hwnd,
            ControlId(IDC_CONFIRM_OK), nullptr, nullptr);

        for (HWND button : {confirm->closeButton, confirm->confirmButton, confirm->cancelButton}) {
            SetFont(button, confirm->font);
            ModernTheme::ApplyControlTheme(button, app->palette.dark);
        }
        ModernTheme::ApplyWindowChrome(hwnd, app->palette.dark);
        SetFocus(confirm->defaultConfirm ? confirm->confirmButton : confirm->cancelButton);
        return 0;
    }

    case WM_ERASEBKGND:
        if (app && app->windowBrush) {
            RECT rc{};
            GetClientRect(hwnd, &rc);
            FillRect(reinterpret_cast<HDC>(wParam), &rc, app->windowBrush);
            return 1;
        }
        break;

    case WM_PAINT:
        if (confirm && app) {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd, &ps);
            RECT rc{};
            GetClientRect(hwnd, &rc);
            const int dpi = GetDpiForWindow(hwnd) > 0 ? GetDpiForWindow(hwnd) : 96;
            auto sc = [dpi](int px) { return MulDiv(px, dpi, 96); };

            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, app->palette.text);

            HGDIOBJ oldFont = confirm->font ? SelectObject(dc, confirm->font) : nullptr;
            RECT captionRc{sc(10), sc(3), rc.right - sc(42), sc(29)};
            DrawTextW(dc, L"Genia Unlocker", -1, &captionRc,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            if (oldFont) SelectObject(dc, oldFont);

            // Flat warning mark: no stock 3-D system icon, just a compact
            // outlined status circle using the same palette as destructive actions.
            HPEN warningPen = CreatePen(PS_SOLID, (std::max)(1, sc(1)), app->palette.danger);
            HGDIOBJ oldWarningPen = SelectObject(dc, warningPen);
            HGDIOBJ oldWarningBrush = SelectObject(dc, GetStockObject(NULL_BRUSH));
            Ellipse(dc, sc(18), sc(47), sc(48), sc(77));
            SelectObject(dc, oldWarningBrush);
            SelectObject(dc, oldWarningPen);
            DeleteObject(warningPen);

            RECT warningTextRc{sc(18), sc(47), sc(48), sc(77)};
            HFONT warningFont = CreateModernFont(dpi, 13, FW_BOLD, L"Segoe UI Variable Display");
            oldFont = warningFont ? SelectObject(dc, warningFont) : nullptr;
            SetTextColor(dc, app->palette.danger);
            DrawTextW(dc, L"!", -1, &warningTextRc,
                      DT_CENTER | DT_VCENTER | DT_SINGLELINE);
            if (oldFont) SelectObject(dc, oldFont);
            if (warningFont) DeleteObject(warningFont);
            SetTextColor(dc, app->palette.text);

            oldFont = confirm->titleFont ? SelectObject(dc, confirm->titleFont) : nullptr;
            RECT titleRc{sc(62), sc(44), rc.right - sc(18), sc(79)};
            DrawTextW(dc, confirm->title.c_str(), -1, &titleRc,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            if (oldFont) SelectObject(dc, oldFont);

            oldFont = confirm->font ? SelectObject(dc, confirm->font) : nullptr;
            RECT messageRc{sc(18), sc(88), rc.right - sc(18), rc.bottom - sc(62)};
            DrawTextW(dc, confirm->message.c_str(), -1, &messageRc,
                      DT_LEFT | DT_TOP | DT_WORDBREAK | DT_NOPREFIX);
            if (oldFont) SelectObject(dc, oldFont);

            HPEN pen = CreatePen(PS_SOLID, 1, app->palette.border);
            HGDIOBJ oldPen = SelectObject(dc, pen);
            MoveToEx(dc, sc(16), rc.bottom - sc(58), nullptr);
            LineTo(dc, rc.right - sc(16), rc.bottom - sc(58));
            SelectObject(dc, oldPen);
            DeleteObject(pen);

            EndPaint(hwnd, &ps);
            return 0;
        }
        break;

    case WM_DRAWITEM:
        if (app && confirm) {
            const auto* draw = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
            if (draw && draw->CtlType == ODT_BUTTON) {
                ModernTheme::ButtonKind kind = ModernTheme::ButtonKind::Secondary;
                if (draw->CtlID == IDC_CONFIRM_OK) {
                    kind = confirm->danger
                        ? ModernTheme::ButtonKind::Danger
                        : ModernTheme::ButtonKind::Primary;
                }
                ModernTheme::DrawButton(*draw, app->palette, kind);
                return TRUE;
            }
        }
        break;

    case WM_CTLCOLORBTN:
        if (app) {
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, app->palette.text);
            return reinterpret_cast<LRESULT>(app->windowBrush);
        }
        break;

    case WM_COMMAND:
        if (!confirm) break;
        if (LOWORD(wParam) == IDC_CONFIRM_OK && HIWORD(wParam) == BN_CLICKED) {
            confirm->result = true;
            DestroyWindow(hwnd);
            return 0;
        }
        if ((LOWORD(wParam) == IDC_CONFIRM_CANCEL || LOWORD(wParam) == IDC_CONFIRM_CLOSE) &&
            HIWORD(wParam) == BN_CLICKED) {
            confirm->result = false;
            DestroyWindow(hwnd);
            return 0;
        }
        break;

    case WM_DPICHANGED:
        if (confirm && app) {
            const int dpi = HIWORD(wParam) > 0 ? HIWORD(wParam) : 96;
            const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
            if (suggested) {
                SetWindowPos(hwnd, nullptr,
                             suggested->left, suggested->top,
                             suggested->right - suggested->left,
                             suggested->bottom - suggested->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            }
            auto sc = [dpi](int px) { return MulDiv(px, dpi, 96); };
            RECT rc{};
            GetClientRect(hwnd, &rc);

            if (confirm->font) DeleteObject(confirm->font);
            if (confirm->titleFont) DeleteObject(confirm->titleFont);
            confirm->font = CreateModernFont(dpi, 9, FW_NORMAL, L"Segoe UI Variable Text");
            confirm->titleFont = CreateModernFont(dpi, 13, FW_SEMIBOLD, L"Segoe UI Variable Display");

            MoveWindow(confirm->closeButton, rc.right - sc(32), sc(5), sc(24), sc(22), TRUE);
            const int buttonY = rc.bottom - sc(46);
            const int cancelW = sc(82);
            const int confirmW = sc(128);
            MoveWindow(confirm->cancelButton, rc.right - sc(16) - cancelW,
                       buttonY, cancelW, sc(30), TRUE);
            MoveWindow(confirm->confirmButton, rc.right - sc(24) - cancelW - confirmW,
                       buttonY, confirmW, sc(30), TRUE);

            for (HWND button : {confirm->closeButton, confirm->confirmButton,
                                confirm->cancelButton}) {
                SetFont(button, confirm->font);
            }
            InvalidateRect(hwnd, nullptr, TRUE);
        }
        return 0;

    case WM_NCHITTEST: {
        POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (confirm && confirm->closeButton) {
            RECT closeRc{};
            GetWindowRect(confirm->closeButton, &closeRc);
            if (PtInRect(&closeRc, pt)) {
                return HTCLIENT;
            }
        }
        RECT windowRc{};
        GetWindowRect(hwnd, &windowRc);
        const int dpi = GetDpiForWindow(hwnd) > 0 ? GetDpiForWindow(hwnd) : 96;
        if (pt.y >= windowRc.top && pt.y < windowRc.top + MulDiv(32, dpi, 96)) {
            return HTCAPTION;
        }
        break;
    }

    case WM_CLOSE:
        if (confirm) confirm->result = false;
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        if (confirm) {
            confirm->finished = true;
            if (confirm->font) DeleteObject(confirm->font);
            if (confirm->titleFont) DeleteObject(confirm->titleFont);
            confirm->font = nullptr;
            confirm->titleFont = nullptr;
        }
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool ShowThemedConfirm(AppState* state,
                       const std::wstring& title,
                       const std::wstring& message,
                       const std::wstring& confirmText,
                       const std::wstring& cancelText = L"Cancel",
                       bool danger = false,
                       bool defaultConfirm = false) {
    if (!state || !state->hwnd) return false;

    HINSTANCE instance = reinterpret_cast<HINSTANCE>(
        GetWindowLongPtrW(state->hwnd, GWLP_HINSTANCE));
    static bool classReady = false;
    if (!classReady) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = ConfirmWindowProc;
        wc.hInstance = instance;
        wc.hIcon = state->icon;
        wc.hIconSm = state->icon;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = kConfirmWindowClass;
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            return MessageBoxW(state->hwnd, message.c_str(), title.c_str(),
                               MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES;
        }
        classReady = true;
    }

    const int dpi = GetDpiForWindow(state->hwnd) > 0 ? GetDpiForWindow(state->hwnd) : 96;
    const int width = MulDiv(540, dpi, 96);

    HDC dc = GetDC(state->hwnd);
    RECT measure{0, 0, width - MulDiv(36, dpi, 96), 0};
    HGDIOBJ oldFont = state->font ? SelectObject(dc, state->font) : nullptr;
    DrawTextW(dc, message.c_str(), -1, &measure,
              DT_CALCRECT | DT_WORDBREAK | DT_NOPREFIX);
    if (oldFont) SelectObject(dc, oldFont);
    ReleaseDC(state->hwnd, dc);

    const int minHeight = MulDiv(230, dpi, 96);
    const int maxHeight = MulDiv(470, dpi, 96);
    const int desiredHeight = measure.bottom + MulDiv(165, dpi, 96);
    const int height = (std::max)(minHeight, (std::min)(maxHeight, desiredHeight));

    RECT owner{};
    GetWindowRect(state->hwnd, &owner);
    const int x = owner.left + ((owner.right - owner.left) - width) / 2;
    const int y = owner.top + ((owner.bottom - owner.top) - height) / 2;

    ConfirmState confirm{};
    confirm.app = state;
    confirm.title = title;
    confirm.message = message;
    confirm.confirmText = confirmText;
    confirm.cancelText = cancelText;
    confirm.danger = danger;
    confirm.defaultConfirm = defaultConfirm;

    HWND window = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_DLGMODALFRAME,
        kConfirmWindowClass,
        title.c_str(),
        WS_POPUP | WS_BORDER,
        x, y, width, height,
        state->hwnd, nullptr, instance, &confirm);
    if (!window) {
        return MessageBoxW(state->hwnd, message.c_str(), title.c_str(),
                           MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) == IDYES;
    }

    EnableWindow(state->hwnd, FALSE);
    ShowWindow(window, SW_SHOWNORMAL);
    UpdateWindow(window);

    MSG msg{};
    while (!confirm.finished) {
        const BOOL status = GetMessageW(&msg, nullptr, 0, 0);
        if (status <= 0) {
            if (status == 0) PostQuitMessage(static_cast<int>(msg.wParam));
            break;
        }

        if ((msg.hwnd == window || IsChild(window, msg.hwnd)) && msg.message == WM_KEYDOWN) {
            if (msg.wParam == VK_ESCAPE) {
                confirm.result = false;
                DestroyWindow(window);
                continue;
            }
            if (msg.wParam == VK_RETURN) {
                confirm.result = confirm.defaultConfirm;
                if (!confirm.defaultConfirm && msg.hwnd == confirm.confirmButton) {
                    confirm.result = true;
                }
                DestroyWindow(window);
                continue;
            }
        }

        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    EnableWindow(state->hwnd, TRUE);
    SetForegroundWindow(state->hwnd);
    return confirm.result;
}

void PopulateList(AppState* state) {
    ListView_DeleteAllItems(state->list);
    if (state->processImageList) {
        ImageList_RemoveAll(state->processImageList);
    }

    for (size_t i = 0; i < state->locks.size(); ++i) {
        const auto& process = state->locks[i];
        LVITEMW item{};
        item.mask = LVIF_TEXT | LVIF_PARAM | LVIF_IMAGE;
        item.iItem = static_cast<int>(i);
        item.pszText = const_cast<LPWSTR>(process.name.c_str());
        item.lParam = static_cast<LPARAM>(i);
        item.iImage = AddProcessIcon(state, process.path);
        int row = ListView_InsertItem(state->list, &item);

        std::wstring pid = std::to_wstring(process.pid);
        ListView_SetItemText(state->list, row, 1, pid.data());

        std::wstring source;
        if (process.foundByRestartManager) {
            source = L"Restart Manager";
        }
        if (process.foundByHandleScan) {
            if (!source.empty()) source += L" + ";
            source += L"Handle";
        }
        if (process.foundByProcessImageScan) {
            if (!source.empty()) source += L" + ";
            source += L"Image/module";
        }
        if (source.empty()) {
            source = L"Process scan";
        }
        ListView_SetItemText(state->list, row, 2, source.data());

        std::wstring lockedObject = PrimaryLockedObject(process);
        ListView_SetItemText(state->list, row, 3, lockedObject.data());
    }
}

void UpdateActionButtons(AppState* state) {
    if (!state || !state->hwnd) {
        return;
    }

    const bool busy = state->scanInProgress;
    const bool exists = state->lastTargetExists;
    const bool hasLocks = !state->locks.empty();
    const bool verifiedClean =
        exists && !hasLocks && state->lastDeleteShareProbeSucceeded;
    const int selectedRow = state->list
        ? ListView_GetNextItem(state->list, -1, LVNI_SELECTED)
        : -1;
    const bool hasSelection = selectedRow >= 0;
    bool selectedUnsafe = false;
    if (hasSelection && static_cast<size_t>(selectedRow) < state->locks.size()) {
        std::wstring reason;
        selectedUnsafe = IsUnsafeSystemProcess(
            state->locks[static_cast<size_t>(selectedRow)].pid, reason);
    }
    const bool hasForceableHandles = std::any_of(
        state->locks.begin(), state->locks.end(),
        [](const LockProcess& process) { return process.foundByHandleScan; });

    EnableWindow(GetDlgItem(state->hwnd, IDC_UNLOCK), exists && hasLocks && !busy);
    EnableWindow(GetDlgItem(state->hwnd, IDC_FORCE_UNLOCK),
                 exists && hasForceableHandles && !busy);
    EnableWindow(GetDlgItem(state->hwnd, IDC_TERMINATE),
                 exists && hasLocks && hasSelection && !selectedUnsafe && !busy);
    EnableWindow(GetDlgItem(state->hwnd, IDC_DELETE), verifiedClean && !busy);
    EnableWindow(GetDlgItem(state->hwnd, IDC_UNLOCK_DELETE),
                 exists && !verifiedClean && !busy);
    EnableWindow(GetDlgItem(state->hwnd, IDC_RETRY),
                 !state->target.empty() && !busy);

    // Keep only one destructive action visible in the compact main toolbar.
    ShowWindow(GetDlgItem(state->hwnd, IDC_DELETE), verifiedClean ? SW_SHOW : SW_HIDE);
    ShowWindow(GetDlgItem(state->hwnd, IDC_UNLOCK_DELETE), verifiedClean ? SW_HIDE : SW_SHOW);
}

void StartScan(AppState* state) {
    if (!state || state->target.empty()) {
        if (state) {
            state->scanInProgress = false;
            state->lastTargetExists = false;
            state->lastTargetIsDirectory = false;
            state->locks.clear();
            PopulateList(state);
            SetStatus(state, L"Choose a file or folder, or drop it here.");
            UpdateActionButtons(state);
        }
        return;
    }

    const auto generation = ++state->scanGeneration;
    const std::wstring target = state->target;
    const HWND hwnd = state->hwnd;
    state->scanInProgress = true;
    SetStatusWithDetails(state, L"Scanning...",
        L"Scanning Restart Manager, process images/modules and system file handles...");
    SetWindowTextW(GetDlgItem(hwnd, IDC_RETRY), L"Scanning…");
    UpdateActionButtons(state);

    std::thread([hwnd, generation, target]() {
        auto payload = std::make_unique<ScanPayload>();
        payload->generation = generation;
        const ULONGLONG started = GetTickCount64();
        payload->result = ScanLocks(target);
        payload->elapsedMs = GetTickCount64() - started;
        if (PostMessageW(hwnd, WM_APP_SCAN_DONE, 0, reinterpret_cast<LPARAM>(payload.get()))) {
            payload.release();
        }
    }).detach();
}

void ActivateMainWindow(AppState* state) {
    if (!state || !state->hwnd || !IsWindow(state->hwnd)) {
        return;
    }

    const HWND hwnd = state->hwnd;
    if (IsIconic(hwnd)) {
        ShowWindow(hwnd, SW_RESTORE);
    } else {
        ShowWindow(hwnd, SW_SHOW);
    }

    // A context-menu launch can arrive while another application owns the
    // foreground. Pulse TOPMOST only long enough to bring Genia Unlocker to
    // the front, then immediately restore normal (non-always-on-top) behavior.
    const UINT flags = SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW | SWP_NOOWNERZORDER;
    SetWindowPos(hwnd, HWND_TOPMOST, 0, 0, 0, 0, flags);
    SetWindowPos(hwnd, HWND_NOTOPMOST, 0, 0, 0, 0, flags);
    BringWindowToTop(hwnd);
    SetForegroundWindow(hwnd);
    SetActiveWindow(hwnd);
}

void SetTarget(AppState* state, const std::wstring& target, bool showWindow = true) {
    if (target.empty()) {
        return;
    }
    state->target = target;
    state->pendingAction = PendingAction::None;
    state->pendingUnlockError.clear();
    state->pendingForceUnlockDetails.clear();
    state->lastTargetExists = false;
    state->lastTargetIsDirectory = false;
    state->lastInaccessibleProcessCount = 0;
    state->lastRestartManagerError = ERROR_SUCCESS;
    state->lastInspectedDiskHandleCount = 0;
    state->lastDeleteShareProbeError = ERROR_SUCCESS;
    state->lastHandleTypeFilterAvailable = false;
    state->lastDeleteShareProbeSucceeded = false;
    SetWindowTextW(state->targetEdit, target.c_str());
    if (showWindow) {
        ActivateMainWindow(state);
    }
    StartScan(state);
}

int SelectedLockIndex(AppState* state) {
    return ListView_GetNextItem(state->list, -1, LVNI_SELECTED);
}

void AddTrayIcon(AppState* state) {
    state->tray = {};
    state->tray.cbSize = sizeof(state->tray);
    state->tray.hWnd = state->hwnd;
    state->tray.uID = 1;

    // With NOTIFYICON_VERSION_4 Windows can suppress the classic szTip text
    // unless NIF_SHOWTIP is explicitly requested. Without it, Windows 11 may
    // still create the hover bubble but leave the content area empty.
    state->tray.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    state->tray.uCallbackMessage = WM_APP_TRAY;
    state->tray.hIcon = state->icon;
    wcscpy_s(state->tray.szTip, L"Genia Unlocker");

    if (!Shell_NotifyIconW(NIM_ADD, &state->tray)) {
        return;
    }

    state->tray.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &state->tray);

    // Reassert the tooltip after switching to version 4. Explorer can recreate
    // the notification area independently (TaskbarCreated), so AddTrayIcon is
    // also the single source of truth for restoring hover text.
    state->tray.uFlags = NIF_TIP | NIF_SHOWTIP;
    Shell_NotifyIconW(NIM_MODIFY, &state->tray);
}

void RemoveTrayIcon(AppState* state) {
    Shell_NotifyIconW(NIM_DELETE, &state->tray);
}

void ShowTrayMenu(AppState* state) {
    POINT pt{};
    GetCursorPos(&pt);
    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING, ID_TRAY_OPEN, L"Open Genia Unlocker");
    AppendMenuW(menu, MF_STRING, ID_TRAY_FILE, L"Select file...");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_TRAY_ABOUT, L"About Genia Unlocker");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_TRAY_EXIT, L"Exit");
    SetForegroundWindow(state->hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN | TPM_LEFTALIGN,
                   pt.x, pt.y, 0, state->hwnd, nullptr);
    DestroyMenu(menu);
}

struct SettingsState {
    AppState* app{};
    HWND closeButton{};
    HWND shellCheck{};
    HWND autostartCheck{};
    HWND permanentDeleteCheck{};
    HWND aboutButton{};
    HWND versionLabel{};
    HFONT font{};
    HFONT titleFont{};
};

void SyncSettingsControls(SettingsState* settings) {
    if (!settings) return;
    SetButtonCheck(settings->shellCheck,
                   IsShellIntegrationEnabled() ? BST_CHECKED : BST_UNCHECKED);
    SetButtonCheck(settings->autostartCheck,
                   IsAutostartEnabled() ? BST_CHECKED : BST_UNCHECKED);
    SetButtonCheck(settings->permanentDeleteCheck,
                   settings->app && settings->app->permanentDeleteDefault
                       ? BST_CHECKED : BST_UNCHECKED);
}

LRESULT CALLBACK SettingsWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* settings = reinterpret_cast<SettingsState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        settings = reinterpret_cast<SettingsState*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(settings));
    }

    AppState* app = settings ? settings->app : nullptr;
    switch (msg) {
    case WM_CREATE: {
        if (!settings || !app) return -1;
        const int dpi = GetDpiForWindow(hwnd) > 0 ? GetDpiForWindow(hwnd) : 96;
        auto sc = [dpi](int px) { return MulDiv(px, dpi, 96); };

        settings->font = CreateModernFont(dpi, 9, FW_NORMAL, L"Segoe UI Variable Text");
        settings->titleFont = CreateModernFont(dpi, 14, FW_SEMIBOLD, L"Segoe UI Variable Display");

        // Compact custom caption for the Settings tool window.  The native
        // caption always places its Close button flush against the right frame;
        // using a client caption lets us keep a small, DPI-aware inset.
        settings->closeButton = CreateWindowW(
            L"BUTTON", L"×",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            sc(368), sc(5), sc(24), sc(22), hwnd,
            ControlId(IDC_SETTINGS_CLOSE), nullptr, nullptr);

        HWND title = CreateWindowW(L"STATIC", L"Settings",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            sc(16), sc(42), sc(330), sc(28), hwnd,
            ControlId(IDC_SETTINGS_TITLE), nullptr, nullptr);
        settings->shellCheck = CreateWindowW(L"BUTTON", L"Explorer context menu",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
            sc(16), sc(84), sc(320), sc(24), hwnd,
            ControlId(IDC_SETTINGS_SHELL), nullptr, nullptr);
        settings->autostartCheck = CreateWindowW(L"BUTTON", L"Start with Windows (tray)",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
            sc(16), sc(112), sc(340), sc(24), hwnd,
            ControlId(IDC_SETTINGS_AUTOSTART), nullptr, nullptr);
        settings->permanentDeleteCheck = CreateWindowW(
            L"BUTTON", L"Permanently delete by default (skip Recycle Bin)",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_AUTOCHECKBOX,
            sc(16), sc(144), sc(350), sc(24), hwnd,
            ControlId(IDC_SETTINGS_PERMANENT), nullptr, nullptr);
        HWND note = CreateWindowW(
            L"STATIC", L"Portable preference is saved beside GeniaUnlocker.exe.",
            WS_CHILD | WS_VISIBLE | SS_LEFT,
            sc(16), sc(177), sc(350), sc(22), hwnd,
            ControlId(IDC_SETTINGS_NOTE), nullptr, nullptr);
        settings->aboutButton = CreateWindowW(
            L"BUTTON", L"About Genia Unlocker",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            sc(16), sc(204), sc(150), sc(28), hwnd,
            ControlId(IDC_SETTINGS_ABOUT), nullptr, nullptr);
        std::wstring versionText = L"v" + std::wstring(kAppVersionDisplay);
        settings->versionLabel = CreateWindowW(
            L"STATIC", versionText.c_str(),
            WS_CHILD | WS_VISIBLE | SS_RIGHT,
            sc(184), sc(209), sc(192), sc(20), hwnd,
            ControlId(IDC_SETTINGS_VERSION), nullptr, nullptr);

        SetFont(settings->closeButton, settings->font);
        SetFont(title, settings->titleFont);
        SetFont(settings->shellCheck, settings->font);
        SetFont(settings->autostartCheck, settings->font);
        SetFont(settings->permanentDeleteCheck, settings->font);
        SetFont(settings->aboutButton, settings->font);
        SetFont(settings->versionLabel, settings->font);
        SetFont(note, settings->font);
        ModernTheme::ApplyWindowChrome(hwnd, app->palette.dark);
        ModernTheme::ApplyControlTheme(settings->shellCheck, app->palette.dark);
        ModernTheme::ApplyControlTheme(settings->autostartCheck, app->palette.dark);
        ModernTheme::ApplyControlTheme(settings->permanentDeleteCheck, app->palette.dark);
        ModernTheme::ApplyControlTheme(settings->aboutButton, app->palette.dark);
        ModernTheme::ApplyControlTheme(settings->closeButton, app->palette.dark);
        SyncSettingsControls(settings);
        return 0;
    }
    case WM_ERASEBKGND:
        if (app && app->windowBrush) {
            RECT rc{};
            GetClientRect(hwnd, &rc);
            FillRect(reinterpret_cast<HDC>(wParam), &rc, app->windowBrush);
            return 1;
        }
        break;
    case WM_PAINT:
        if (settings && app) {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd, &ps);
            RECT rc{};
            GetClientRect(hwnd, &rc);
            const int dpi = GetDpiForWindow(hwnd) > 0 ? GetDpiForWindow(hwnd) : 96;
            RECT captionRc{MulDiv(10, dpi, 96), MulDiv(3, dpi, 96),
                           rc.right - MulDiv(48, dpi, 96), MulDiv(29, dpi, 96)};
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, app->palette.text);
            HGDIOBJ oldFont = settings->font ? SelectObject(dc, settings->font) : nullptr;
            DrawTextW(dc, L"Genia Unlocker Settings", -1, &captionRc,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
            if (oldFont) SelectObject(dc, oldFont);
            EndPaint(hwnd, &ps);
            return 0;
        }
        break;
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN:
        if (app) {
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetBkMode(dc, TRANSPARENT);
            const HWND control = reinterpret_cast<HWND>(lParam);
            SetTextColor(dc, GetDlgCtrlID(control) == IDC_SETTINGS_VERSION
                                 ? app->palette.muted
                                 : app->palette.text);
            return reinterpret_cast<LRESULT>(app->windowBrush);
        }
        break;
    case WM_DRAWITEM:
        if (app) {
            const auto* draw = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
            if (draw && draw->CtlType == ODT_BUTTON) {
                ModernTheme::DrawButton(*draw, app->palette, ModernTheme::ButtonKind::Secondary);
                return TRUE;
            }
        }
        break;
    case WM_COMMAND:
        if (!settings || !app) break;
        switch (LOWORD(wParam)) {
        case IDC_SETTINGS_CLOSE:
            if (HIWORD(wParam) == BN_CLICKED) {
                DestroyWindow(hwnd);
            }
            return 0;
        case IDC_SETTINGS_ABOUT:
            if (HIWORD(wParam) == BN_CLICKED) {
                ShowAboutDialog(app);
            }
            return 0;
        case IDC_SETTINGS_SHELL:
            if (HIWORD(wParam) == BN_CLICKED) {
                const bool enabled = GetButtonCheck(settings->shellCheck) == BST_CHECKED;
                std::wstring error;
                if (!SetShellIntegrationEnabled(enabled, error)) {
                    MessageBoxW(hwnd, error.c_str(), kWindowTitle, MB_ICONERROR);
                }
                SyncSettingsControls(settings);
            }
            return 0;
        case IDC_SETTINGS_AUTOSTART:
            if (HIWORD(wParam) == BN_CLICKED) {
                const bool enabled = GetButtonCheck(settings->autostartCheck) == BST_CHECKED;
                std::wstring error;
                if (!SetAutostartEnabled(enabled, error)) {
                    MessageBoxW(hwnd, error.c_str(), kWindowTitle, MB_ICONERROR);
                }
                SyncSettingsControls(settings);
            }
            return 0;
        case IDC_SETTINGS_PERMANENT:
            if (HIWORD(wParam) == BN_CLICKED) {
                const bool previous = app->permanentDeleteDefault;
                app->permanentDeleteDefault =
                    GetButtonCheck(settings->permanentDeleteCheck) == BST_CHECKED;
                if (!SavePermanentDeleteDefault(app->permanentDeleteDefault)) {
                    app->permanentDeleteDefault = previous;
                    MessageBoxW(
                        hwnd,
                        L"Could not write GeniaUnlocker.ini beside the executable. "
                        L"Move the portable EXE to a writable folder or adjust permissions.",
                        kWindowTitle,
                        MB_ICONERROR);
                }
                SyncSettingsControls(settings);
            }
            return 0;
        }
        break;
    case WM_DPICHANGED:
        if (settings && app) {
            const int dpi = HIWORD(wParam) > 0 ? HIWORD(wParam) : 96;
            const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
            if (suggested) {
                SetWindowPos(hwnd, nullptr,
                             suggested->left, suggested->top,
                             suggested->right - suggested->left,
                             suggested->bottom - suggested->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            }
            auto sc = [dpi](int px) { return MulDiv(px, dpi, 96); };

            if (settings->font) DeleteObject(settings->font);
            if (settings->titleFont) DeleteObject(settings->titleFont);
            settings->font = CreateModernFont(dpi, 9, FW_NORMAL, L"Segoe UI Variable Text");
            settings->titleFont = CreateModernFont(dpi, 14, FW_SEMIBOLD, L"Segoe UI Variable Display");

            MoveWindow(settings->closeButton, sc(368), sc(5), sc(24), sc(22), TRUE);
            MoveWindow(GetDlgItem(hwnd, IDC_SETTINGS_TITLE), sc(16), sc(42), sc(330), sc(28), TRUE);
            MoveWindow(settings->shellCheck, sc(16), sc(84), sc(320), sc(24), TRUE);
            MoveWindow(settings->autostartCheck, sc(16), sc(112), sc(340), sc(24), TRUE);
            MoveWindow(settings->permanentDeleteCheck, sc(16), sc(144), sc(350), sc(24), TRUE);
            MoveWindow(GetDlgItem(hwnd, IDC_SETTINGS_NOTE), sc(16), sc(177), sc(350), sc(22), TRUE);
            MoveWindow(settings->aboutButton, sc(16), sc(204), sc(150), sc(28), TRUE);
            MoveWindow(settings->versionLabel, sc(184), sc(209), sc(192), sc(20), TRUE);

            for (HWND control : {settings->closeButton, settings->shellCheck,
                                 settings->autostartCheck, settings->permanentDeleteCheck,
                                 settings->aboutButton, settings->versionLabel,
                                 GetDlgItem(hwnd, IDC_SETTINGS_NOTE)}) {
                SetFont(control, settings->font);
            }
            SetFont(GetDlgItem(hwnd, IDC_SETTINGS_TITLE), settings->titleFont);
            InvalidateRect(hwnd, nullptr, TRUE);
        }
        return 0;

    case WM_NCHITTEST: {
        // Make the custom caption draggable while keeping the inset Close
        // button clickable. Coordinates in lParam are screen coordinates.
        POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (settings && settings->closeButton) {
            RECT closeRc{};
            GetWindowRect(settings->closeButton, &closeRc);
            if (PtInRect(&closeRc, pt)) {
                return HTCLIENT;
            }
        }

        RECT windowRc{};
        GetWindowRect(hwnd, &windowRc);
        const int dpi = GetDpiForWindow(hwnd) > 0 ? GetDpiForWindow(hwnd) : 96;
        const int captionHeight = MulDiv(32, dpi, 96);
        if (pt.y >= windowRc.top && pt.y < windowRc.top + captionHeight) {
            return HTCAPTION;
        }
        break;
    }
    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;
    case WM_DESTROY:
        if (app && app->settingsWindow == hwnd) {
            app->settingsWindow = nullptr;
        }
        if (settings) {
            if (settings->font) DeleteObject(settings->font);
            if (settings->titleFont) DeleteObject(settings->titleFont);
        }
        delete settings;
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void ShowSettingsWindow(AppState* state) {
    if (!state || !state->hwnd) return;
    if (state->settingsWindow && IsWindow(state->settingsWindow)) {
        ShowWindow(state->settingsWindow, SW_RESTORE);
        SetForegroundWindow(state->settingsWindow);
        return;
    }

    HINSTANCE instance = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(state->hwnd, GWLP_HINSTANCE));
    static bool classReady = false;
    if (!classReady) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = SettingsWindowProc;
        wc.hInstance = instance;
        wc.hIcon = state->icon;
        wc.hIconSm = state->icon;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = kSettingsWindowClass;
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            MessageBoxW(state->hwnd, L"Could not create the settings window.", kWindowTitle, MB_ICONERROR);
            return;
        }
        classReady = true;
    }

    const int dpi = GetDpiForWindow(state->hwnd) > 0 ? GetDpiForWindow(state->hwnd) : 96;
    const int width = MulDiv(400, dpi, 96);
    const int height = MulDiv(250, dpi, 96);
    RECT owner{};
    GetWindowRect(state->hwnd, &owner);
    const int x = owner.left + ((owner.right - owner.left) - width) / 2;
    const int y = owner.top + ((owner.bottom - owner.top) - height) / 2;

    auto* settings = new SettingsState{};
    settings->app = state;
    HWND window = CreateWindowExW(
        WS_EX_TOOLWINDOW,
        kSettingsWindowClass,
        L"Genia Unlocker Settings",
        WS_POPUP | WS_BORDER,
        x, y, width, height,
        state->hwnd, nullptr, instance, settings);
    if (!window) {
        delete settings;
        MessageBoxW(state->hwnd, L"Could not open settings.", kWindowTitle, MB_ICONERROR);
        return;
    }
    state->settingsWindow = window;
    ShowWindow(window, SW_SHOWNORMAL);
    UpdateWindow(window);
}

std::wstring FormatHResult(HRESULT hr) {
    wchar_t code[32]{};
    swprintf_s(code, _countof(code), L"0x%08lX", static_cast<unsigned long>(hr));
    std::wstring message = ErrorMessage(HRESULT_CODE(hr));
    if (message.empty()) {
        return std::wstring(L"HRESULT ") + code;
    }
    return message + L" (HRESULT " + code + L")";
}

bool DeleteTargetWithShell(const std::wstring& target,
                           bool permanent,
                           std::wstring& errorText) {
    errorText.clear();

    IFileOperation* operation = nullptr;
    HRESULT hr = CoCreateInstance(
        CLSID_FileOperation, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&operation));
    if (FAILED(hr) || !operation) {
        errorText = L"Cannot initialize the Windows file operation service: " + FormatHResult(hr);
        return false;
    }

    DWORD flags = FOF_NOCONFIRMATION | FOF_NOERRORUI | FOF_SILENT;
    if (!permanent) {
        flags |= FOF_ALLOWUNDO | FOFX_RECYCLEONDELETE;
    }
    hr = operation->SetOperationFlags(flags);
    if (FAILED(hr)) {
        operation->Release();
        errorText = L"Cannot configure the delete operation: " + FormatHResult(hr);
        return false;
    }

    IShellItem* item = nullptr;
    hr = SHCreateItemFromParsingName(target.c_str(), nullptr, IID_PPV_ARGS(&item));
    if (FAILED(hr) || !item) {
        operation->Release();
        errorText = L"Cannot open the selected item for deletion: " + FormatHResult(hr);
        return false;
    }

    hr = operation->DeleteItem(item, nullptr);
    item->Release();
    if (FAILED(hr)) {
        operation->Release();
        errorText = L"Windows rejected the delete request: " + FormatHResult(hr);
        return false;
    }

    hr = operation->PerformOperations();
    BOOL aborted = FALSE;
    operation->GetAnyOperationsAborted(&aborted);
    operation->Release();

    if (FAILED(hr)) {
        errorText = L"Delete failed: " + FormatHResult(hr);
        return false;
    }
    if (aborted) {
        errorText = L"The delete operation was aborted.";
        return false;
    }

    const DWORD verifyAttributes = GetFileAttributesW(target.c_str());
    if (verifyAttributes != INVALID_FILE_ATTRIBUTES) {
        errorText = L"The item still exists after Windows completed the delete operation.";
        return false;
    }

    const DWORD verifyError = GetLastError();
    if (verifyError != ERROR_FILE_NOT_FOUND && verifyError != ERROR_PATH_NOT_FOUND) {
        errorText = L"Windows reported deletion, but Genia Unlocker could not verify that the item is gone: " +
                    ErrorMessage(verifyError);
        return false;
    }
    return true;
}

bool IsVolumeRootTarget(const std::wstring& target) {
    if (target.empty()) {
        return false;
    }

    wchar_t volumeRoot[32768]{};
    if (!GetVolumePathNameW(target.c_str(), volumeRoot, static_cast<DWORD>(_countof(volumeRoot)))) {
        return false;
    }

    auto normalize = [](std::wstring value) {
        std::replace(value.begin(), value.end(), L'/', L'\\');
        while (value.size() > 1 && value.back() == L'\\') {
            value.pop_back();
        }
        std::transform(value.begin(), value.end(), value.begin(), [](wchar_t ch) {
            return static_cast<wchar_t>(std::towlower(ch));
        });
        return value;
    };

    return normalize(target) == normalize(volumeRoot);
}

bool ConfirmDelete(AppState* state, const wchar_t* title) {
    if (!state || state->target.empty()) {
        return false;
    }

    DWORD attrs = GetFileAttributesW(state->target.c_str());
    const bool isDirectory = state->lastTargetExists
        ? state->lastTargetIsDirectory
        : (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0);

    if (isDirectory && IsVolumeRootTarget(state->target)) {
        MessageBoxW(state->hwnd,
                    L"Deleting a drive or volume root is intentionally disabled.",
                    kWindowTitle,
                    MB_ICONINFORMATION);
        return false;
    }

    std::wstring prompt;
    if (state->permanentDeleteDefault) {
        prompt = isDirectory
            ? L"Permanently delete this folder and everything inside it?\n\n"
            : L"Permanently delete this file?\n\n";
        prompt += state->target;
        prompt += L"\n\nThis action cannot be undone.";
    } else {
        prompt = isDirectory
            ? L"Move this folder and everything inside it to the Recycle Bin?\n\n"
            : L"Move this file to the Recycle Bin?\n\n";
        prompt += state->target;
        prompt += L"\n\nYou can change the default delete mode in Settings.";
    }

    return ShowThemedConfirm(
        state, title, prompt,
        state->permanentDeleteDefault ? L"Delete permanently" : L"Delete",
        L"Cancel", true, false);
}

bool ScheduleDeleteOnRebootRecursive(const std::wstring& path, std::wstring& errorText) {
    DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        return true;
    }

    if ((attrs & FILE_ATTRIBUTE_DIRECTORY) != 0 &&
        (attrs & FILE_ATTRIBUTE_REPARSE_POINT) == 0) {
        std::wstring pattern = path;
        if (!pattern.empty() && pattern.back() != L'\\') {
            pattern.push_back(L'\\');
        }
        pattern += L"*";

        WIN32_FIND_DATAW data{};
        HANDLE find = FindFirstFileW(pattern.c_str(), &data);
        if (find == INVALID_HANDLE_VALUE) {
            const DWORD findError = GetLastError();
            if (findError != ERROR_FILE_NOT_FOUND && findError != ERROR_PATH_NOT_FOUND) {
                errorText = L"Could not enumerate the folder for delete-on-reboot: " +
                            ErrorMessage(findError);
                return false;
            }
        } else {
            do {
                if (wcscmp(data.cFileName, L".") == 0 || wcscmp(data.cFileName, L"..") == 0) {
                    continue;
                }
                std::wstring child = path;
                if (!child.empty() && child.back() != L'\\') {
                    child.push_back(L'\\');
                }
                child += data.cFileName;
                if (!ScheduleDeleteOnRebootRecursive(child, errorText)) {
                    FindClose(find);
                    return false;
                }
            } while (FindNextFileW(find, &data));
            FindClose(find);
        }
    }

    if (!MoveFileExW(path.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT)) {
        const DWORD error = GetLastError();
        errorText = L"Could not schedule deletion at reboot: " + ErrorMessage(error);
        return false;
    }
    return true;
}

bool OfferDeleteOnReboot(AppState* state) {
    if (!state || state->target.empty() || IsVolumeRootTarget(state->target)) {
        return false;
    }
    if (!ShowThemedConfirm(
            state,
            L"Delete on reboot",
            L"Windows could not delete the item now.\n\n"
            L"Schedule it for permanent deletion at the next Windows startup?\n\n"
            L"This fallback does not use the Recycle Bin and cannot be undone after restart.",
            L"Schedule",
            L"Cancel",
            true,
            false)) {
        return false;
    }

    std::wstring error;
    if (!ScheduleDeleteOnRebootRecursive(state->target, error)) {
        if (!error.empty()) {
            MessageBoxW(state->hwnd, error.c_str(), L"Delete on reboot", MB_ICONERROR);
        }
        return false;
    }
    SetStatus(state, L"Scheduled for permanent deletion at the next Windows startup.");
    return true;
}

bool TryDeleteCurrentTarget(AppState* state, const std::wstring& successText) {
    if (!state || state->target.empty()) {
        return false;
    }

    SetStatus(state, state->permanentDeleteDefault
        ? L"Permanently deleting the selected item..."
        : L"Moving the selected item to the Recycle Bin...");
    std::wstring error;
    constexpr int kDeleteAttempts = 4;
    for (int attempt = 1; attempt <= kDeleteAttempts; ++attempt) {
        if (DeleteTargetWithShell(state->target, state->permanentDeleteDefault, error)) {
            state->lastTargetExists = false;
            state->lastTargetIsDirectory = false;
            state->lastInaccessibleProcessCount = 0;
            state->locks.clear();
            PopulateList(state);
            SetStatus(state, successText);
            UpdateActionButtons(state);
            return true;
        }

        if (attempt < kDeleteAttempts) {
            const DWORD attrs = GetFileAttributesW(state->target.c_str());
            if (attrs == INVALID_FILE_ATTRIBUTES) {
                const DWORD verifyError = GetLastError();
                if (verifyError == ERROR_FILE_NOT_FOUND || verifyError == ERROR_PATH_NOT_FOUND) {
                    state->lastTargetExists = false;
                    state->lastTargetIsDirectory = false;
                    state->lastInaccessibleProcessCount = 0;
                    state->locks.clear();
                    PopulateList(state);
                    SetStatus(state, successText);
                    UpdateActionButtons(state);
                    return true;
                }
            }

            // A process can release/reopen its final handle just after the rescan.
            // Give Windows a short grace period before escalating to delete-on-reboot.
            static constexpr DWORD kRetryDelaysMs[] = { 120, 220, 360 };
            Sleep(kRetryDelaysMs[attempt - 1]);
            error.clear();
        }
    }

    if (!error.empty()) {
        MessageBoxW(state->hwnd, error.c_str(), L"Delete failed", MB_ICONERROR);
    }
    if (OfferDeleteOnReboot(state)) {
        return false;
    }
    SetStatus(state, L"Delete failed. Rescanning to identify what is still blocking the item...");
    state->pendingAction = PendingAction::None;
    StartScan(state);
    return false;
}

std::wstring BuildBlockingProcessPrompt(const AppState* state) {
    std::wstring prompt =
        L"The graceful unlock request did not release the item.\n\n"
        L"Blocking processes:\n";

    const size_t limit = (std::min)(state->locks.size(), static_cast<size_t>(6));
    for (size_t i = 0; i < limit; ++i) {
        const auto& process = state->locks[i];
        prompt += L"  • " + process.name + L" (PID " + std::to_wstring(process.pid) + L")\n";
    }
    if (state->locks.size() > limit) {
        prompt += L"  • and " + std::to_wstring(state->locks.size() - limit) + L" more\n";
    }

    prompt += state->permanentDeleteDefault
        ? L"\nTerminate the detected blocking processes and continue with permanent deletion?\n\n"
          L"Unsaved data in those processes can be lost."
        : L"\nTerminate the detected blocking processes and continue by moving the item to the Recycle Bin?\n\n"
          L"Unsaved data in those processes can be lost.";
    return prompt;
}

bool TerminateDetectedForDelete(AppState* state, std::wstring& errorText) {
    errorText.clear();
    std::vector<DWORD> needElevation;

    for (const auto& process : state->locks) {
        std::wstring unsafeReason;
        if (IsUnsafeSystemProcess(process.pid, unsafeReason)) {
            errorText = L"Genia Unlocker refuses to terminate " + process.name +
                        L" (PID " + std::to_wstring(process.pid) + L"): " + unsafeReason + L".";
            return false;
        }

        DWORD error = ERROR_SUCCESS;
        if (TerminateProcessByPid(process.pid, error)) {
            continue;
        }

        // A process can disappear between the scan and the termination pass.
        if (error == ERROR_INVALID_PARAMETER || error == ERROR_NOT_FOUND) {
            continue;
        }

        if (error == ERROR_ACCESS_DENIED && !IsRunningElevated()) {
            needElevation.push_back(process.pid);
            continue;
        }

        errorText = L"Cannot terminate " + process.name + L" (PID " +
                    std::to_wstring(process.pid) + L"): " + ErrorMessage(error);
        return false;
    }

    if (!needElevation.empty()) {
        DWORD helperExitCode = ERROR_GEN_FAILURE;
        if (!RunElevatedKillListHelper(needElevation, helperExitCode, errorText)) {
            if (errorText.empty()) {
                errorText = L"Administrator-assisted termination failed: " +
                            ErrorMessage(helperExitCode);
            }
            return false;
        }
    }

    return true;
}

bool StartTerminateThenDelete(AppState* state) {
    const std::wstring prompt = BuildBlockingProcessPrompt(state);
    if (!ShowThemedConfirm(
            state, L"Unlock & Delete", prompt,
            L"Terminate & Delete", L"Cancel", true, false)) {
        state->pendingAction = PendingAction::None;
        SetStatus(state, L"Deletion cancelled. The item is still locked.");
        return false;
    }

    std::wstring error;
    if (!TerminateDetectedForDelete(state, error)) {
        state->pendingAction = PendingAction::None;
        if (!error.empty()) {
            MessageBoxW(state->hwnd, error.c_str(), L"Unlock & Delete", MB_ICONERROR);
        }
        SetStatus(state, L"Could not terminate all blocking processes. The item was not deleted.");
        return false;
    }

    SetStatus(state, L"Blocking processes terminated. Verifying the item before deletion...");
    state->pendingAction = PendingAction::UnlockDeleteAfterTerminate;
    Sleep(300);
    StartScan(state);
    return true;
}

void DeleteOnly(AppState* state) {
    if (!state || state->target.empty() || !state->lastTargetExists) {
        return;
    }
    if (!ConfirmDelete(state, L"Delete")) {
        return;
    }
    TryDeleteCurrentTarget(state, state->permanentDeleteDefault
        ? L"Permanently deleted successfully."
        : L"Moved to the Recycle Bin successfully.");
}

std::wstring FormatForceUnlockResult(const ForceUnlockResult& result) {
    std::wstring text = L"Force Unlock closed " + std::to_wstring(result.handlesClosed) +
        (result.handlesClosed == 1 ? L" matching file handle" : L" matching file handles") +
        L" in " + std::to_wstring(result.processesAffected) +
        (result.processesAffected == 1 ? L" process." : L" processes.");
    if (result.failedHandleCount != 0) {
        text += L" " + std::to_wstring(result.failedHandleCount) + L" matching handles could not be closed.";
    }
    if (result.inaccessibleProcessCount != 0) {
        text += L" " + std::to_wstring(result.inaccessibleProcessCount) +
            L" file-handle owners require higher permissions.";
    }
    if (result.skippedProtectedProcessCount != 0) {
        text += L" " + std::to_wstring(result.skippedProtectedProcessCount) +
            L" critical Windows processes were intentionally skipped.";
    }
    return text;
}

bool ExecuteForceUnlock(AppState* state, bool allowElevation, std::wstring& details) {
    details.clear();
    if (!state || state->target.empty()) {
        return false;
    }

    ForceUnlockResult result = ForceUnlockHandles(state->target);
    details = FormatForceUnlockResult(result);

    if (result.inaccessibleProcessCount != 0 && !IsRunningElevated() && allowElevation) {
        if (ShowThemedConfirm(
                state,
                L"Force Unlock",
                L"Some matching file-handle owners could not be modified with current permissions.\n\n"
                L"Run the Force Unlock pass once as administrator?",
                L"Run as Admin",
                L"Not now",
                false,
                true)) {
            DWORD helperCode = ERROR_GEN_FAILURE;
            std::wstring helperError;
            if (!RunElevatedForceUnlockHelper(state->target, helperCode, helperError)) {
                if (!helperError.empty()) {
                    MessageBoxW(state->hwnd, helperError.c_str(), L"Force Unlock", MB_ICONERROR);
                }
                return false;
            }
            details += L" Administrator-assisted Force Unlock pass completed.";
        }
    }
    return true;
}

void ForceUnlockCurrent(AppState* state) {
    if (!state || state->target.empty() || state->locks.empty()) {
        return;
    }
    if (IsVolumeRootTarget(state->target)) {
        MessageBoxW(
            state->hwnd,
            L"Force Unlock is intentionally disabled for an entire drive/volume root. "
            L"Choose a specific file or folder instead.",
            L"Force Unlock",
            MB_ICONWARNING);
        return;
    }

    bool hasHandleLocks = std::any_of(
        state->locks.begin(), state->locks.end(),
        [](const LockProcess& process) { return process.foundByHandleScan; });
    if (!hasHandleLocks) {
        MessageBoxW(
            state->hwnd,
            L"No matching file handles are currently available to close.\n\n"
            L"The item may be locked by a loaded executable/module mapping. In that case, "
            L"Terminate is the remaining user-mode option.",
            L"Force Unlock",
            MB_ICONINFORMATION);
        return;
    }

    if (!ShowThemedConfirm(
            state,
            L"Force Unlock",
            L"Force Unlock closes only matching file handles inside other processes instead of "
            L"terminating the whole process.\n\n"
            L"This is more invasive than normal Unlock: an application may become unstable if it "
            L"expects the handle to remain valid. Critical Windows processes are never modified.",
            L"Force Unlock",
            L"Cancel",
            false,
            false)) {
        return;
    }

    SetStatus(state, L"Force-closing matching file handles...");
    state->pendingAction = PendingAction::VerifyForceUnlock;
    std::wstring details;
    if (!ExecuteForceUnlock(state, true, details)) {
        state->pendingAction = PendingAction::None;
        SetStatus(state, L"Force Unlock was not completed.");
        return;
    }
    state->pendingForceUnlockDetails = std::move(details);
    Sleep(200);
    StartScan(state);
}

void GracefulUnlock(AppState* state) {
    if (state->target.empty()) {
        return;
    }
    if (state->locks.empty()) {
        MessageBoxW(state->hwnd,
                    L"No locking processes are currently detected.",
                    kWindowTitle,
                    MB_ICONINFORMATION);
        return;
    }

    if (!ShowThemedConfirm(
            state,
            L"Unlock",
            L"Genia Unlocker will ask applications locking this item to close gracefully.\n\n"
            L"After the request, Genia Unlocker will rescan the item and report whether the "
            L"lock was actually released.\n\n"
            L"Unsaved work in those applications may still be affected.",
            L"Unlock",
            L"Cancel",
            false,
            false)) {
        return;
    }

    SetStatus(state, L"Requesting applications to release the resource...");
    state->pendingAction = PendingAction::VerifyUnlock;
    state->pendingUnlockError.clear();
    std::wstring error;
    RequestGracefulUnlock(state->target, error);
    state->pendingUnlockError = std::move(error);
    Sleep(350);
    StartScan(state);
}

void UnlockAndDelete(AppState* state) {
    if (!state || state->target.empty() || !state->lastTargetExists) {
        return;
    }
    if (!ConfirmDelete(state, L"Unlock & Delete")) {
        return;
    }

    // If no blocker matching this target is currently detected, try deletion
    // immediately. inaccessibleProcessCount is intentionally NOT a veto here:
    // it means some system processes could not be inspected, not that they are
    // proven to hold this target. The Windows delete operation itself is the
    // authoritative final check and will fail safely if a hidden lock remains.
    if (state->locks.empty()) {
        TryDeleteCurrentTarget(state, state->permanentDeleteDefault
            ? L"Permanently deleted successfully."
            : L"Moved to the Recycle Bin successfully.");
        return;
    }

    SetStatus(state, L"Trying a graceful unlock before deletion...");
    state->pendingAction = PendingAction::UnlockDeleteAfterUnlock;
    state->pendingUnlockError.clear();
    std::wstring error;
    RequestGracefulUnlock(state->target, error);
    state->pendingUnlockError = std::move(error);
    Sleep(350);
    StartScan(state);
}

void TerminateSelected(AppState* state) {
    int row = SelectedLockIndex(state);
    if (row < 0 || static_cast<size_t>(row) >= state->locks.size()) {
        MessageBoxW(state->hwnd, L"Select a process first.", kWindowTitle, MB_ICONINFORMATION);
        return;
    }

    const LockProcess process = state->locks[static_cast<size_t>(row)];
    std::wstring unsafeReason;
    if (IsUnsafeSystemProcess(process.pid, unsafeReason)) {
        std::wstring message = L"Genia Unlocker will not terminate " + process.name +
            L" because it is a " + unsafeReason + L".";
        MessageBoxW(state->hwnd, message.c_str(), L"Protected process", MB_ICONWARNING);
        return;
    }

    std::wstring prompt = L"Terminate " + process.name + L" (PID " + std::to_wstring(process.pid) +
                          L")?\n\nUnsaved data in this process can be lost.";
    if (!ShowThemedConfirm(
            state, L"Terminate process", prompt,
            L"Terminate", L"Cancel", true, false)) {
        return;
    }

    DWORD error = ERROR_SUCCESS;
    if (!TerminateProcessByPid(process.pid, error)) {
        if (error == ERROR_ACCESS_DENIED && !IsRunningElevated()) {
            DWORD helperCode = ERROR_GEN_FAILURE;
            std::wstring elevateError;
            if (!RunElevatedKillHelper(process.pid, helperCode, elevateError)) {
                MessageBoxW(state->hwnd, elevateError.c_str(), kWindowTitle, MB_ICONERROR);
                return;
            }
        } else {
            std::wstring message = L"Cannot terminate the process: " + ErrorMessage(error);
            MessageBoxW(state->hwnd, message.c_str(), kWindowTitle, MB_ICONERROR);
            return;
        }
    }

    Sleep(250);
    StartScan(state);
}

void ShowProcessContextMenu(AppState* state, int row, POINT screenPoint) {
    if (!state || row < 0 || static_cast<size_t>(row) >= state->locks.size()) {
        return;
    }

    ListView_SetItemState(state->list, row,
        LVIS_SELECTED | LVIS_FOCUSED, LVIS_SELECTED | LVIS_FOCUSED);
    const LockProcess& process = state->locks[static_cast<size_t>(row)];

    HMENU menu = CreatePopupMenu();
    if (!menu) return;

    UINT forceFlags = MF_STRING;
    UINT terminateFlags = MF_STRING;
    std::wstring unsafeReason;
    if (!process.foundByHandleScan || IsUnsafeSystemProcess(process.pid, unsafeReason)) {
        forceFlags |= MF_GRAYED;
    }
    if (!unsafeReason.empty()) {
        terminateFlags |= MF_GRAYED;
    }

    AppendMenuW(menu, forceFlags, ID_PROCESS_FORCE_UNLOCK, L"Force Unlock target");
    AppendMenuW(menu, terminateFlags, ID_PROCESS_TERMINATE, L"Terminate process");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, ID_PROCESS_OPEN_EXE, L"Open executable location");
    AppendMenuW(menu, MF_STRING, ID_PROCESS_COPY_OBJECT, L"Copy locked object path(s)");
    AppendMenuW(menu, MF_STRING, ID_PROCESS_COPY_PID, L"Copy PID");

    SetForegroundWindow(state->hwnd);
    const UINT command = TrackPopupMenu(
        menu,
        TPM_RIGHTBUTTON | TPM_RETURNCMD | TPM_NONOTIFY,
        screenPoint.x, screenPoint.y, 0, state->hwnd, nullptr);
    DestroyMenu(menu);

    switch (command) {
    case ID_PROCESS_FORCE_UNLOCK:
        ForceUnlockCurrent(state);
        break;
    case ID_PROCESS_TERMINATE:
        TerminateSelected(state);
        break;
    case ID_PROCESS_OPEN_EXE:
        RevealProcessExecutable(state, row);
        break;
    case ID_PROCESS_COPY_OBJECT: {
        std::wstring text;
        for (const auto& object : process.lockedObjects) {
            if (!text.empty()) text += L"\r\n";
            text += object;
        }
        if (text.empty()) text = state->target;
        CopyTextToClipboard(state->hwnd, text);
        break;
    }
    case ID_PROCESS_COPY_PID:
        CopyTextToClipboard(state->hwnd, std::to_wstring(process.pid));
        break;
    default:
        break;
    }
}

void RetryScan(AppState* state) {
    if (!state || state->target.empty()) {
        StartScan(state);
        return;
    }

    const bool unidentifiedSharingLock =
        state->locks.empty() &&
        state->lastDeleteShareProbeError == ERROR_SHARING_VIOLATION;
    if (unidentifiedSharingLock && !IsRunningElevated()) {
        std::wstring prompt;
        if (unidentifiedSharingLock) {
            prompt =
                L"Windows confirms that this target is blocked by file sharing, "
                L"but the owning process was not identified with current permissions.\n\n"
                L"Restart Genia Unlocker as administrator and rescan this target?";
        } else {
            prompt =
                L"Some processes that own file handles could not be inspected with current permissions.\n\n"
                L"Restart Genia Unlocker as administrator and rescan this target?";
        }
        if (ShowThemedConfirm(
                state,
                L"Scan as administrator",
                prompt,
                L"Scan as Admin",
                L"Cancel",
                false,
                true)) {
            std::wstring error;
            if (RunElevatedScanInstance(state->target, error)) {
                state->exiting = true;
                DestroyWindow(state->hwnd);
                return;
            }
            if (!error.empty()) {
                MessageBoxW(state->hwnd, error.c_str(), kWindowTitle, MB_ICONERROR);
            }
            return;
        }
    }

    StartScan(state);
}

bool HandlePendingActionAfterScan(AppState* state, const ScanResult& result) {
    if (!state || state->pendingAction == PendingAction::None) {
        return false;
    }

    const PendingAction action = state->pendingAction;
    const bool noDetectedLocks = result.targetExists && state->locks.empty();
    // A global count of inaccessible file-handle owners is not evidence that
    // any of them owns this target. Verify the target itself with a harmless
    // DELETE-access/share probe instead.
    const bool verifiedUnlocked =
        noDetectedLocks &&
        result.deleteShareProbeSucceeded;

    if (action == PendingAction::VerifyUnlock) {
        state->pendingAction = PendingAction::None;

        if (!result.targetExists) {
            SetStatus(state, L"The selected path no longer exists.");
            return true;
        }
        if (verifiedUnlocked) {
            SetStatus(state, L"Unlocked successfully — verified by a clean rescan.");
            return true;
        }
        if (!state->locks.empty()) {
            std::wstring text = L"Unlock request completed, but the item is still locked by " +
                std::to_wstring(state->locks.size()) +
                (state->locks.size() == 1 ? L" process." : L" processes.");
            if (!state->pendingUnlockError.empty()) {
                text += L" Windows did not confirm a graceful release.";
            }
            SetStatus(state, text);
            return true;
        }

        std::wstring text =
            L"Unlock could not be verified because some file-handle owners could not be inspected.";
        if (!IsRunningElevated()) {
            text += L" Use Scan as Admin and try again.";
        }
        SetStatus(state, text);
        return true;
    }

    if (action == PendingAction::VerifyForceUnlock) {
        state->pendingAction = PendingAction::None;
        if (!result.targetExists) {
            SetStatusWithDetails(state, L"The selected path no longer exists.",
                                 state->pendingForceUnlockDetails);
            return true;
        }
        if (verifiedUnlocked) {
            SetStatusWithDetails(state, L"Force Unlock succeeded — verified by a clean rescan.",
                                 state->pendingForceUnlockDetails);
            return true;
        }

        std::wstring shortText;
        if (!state->locks.empty()) {
            shortText = L"Force Unlock completed, but " + std::to_wstring(state->locks.size()) +
                (state->locks.size() == 1 ? L" blocker remains." : L" blockers remain.");
        } else {
            shortText = L"Force Unlock completed, but the result needs an administrator rescan.";
        }
        std::wstring details = state->pendingForceUnlockDetails;
        if (!details.empty()) details += L" ";
        details += L"Loaded executable/module mappings cannot be released by closing ordinary file handles.";
        SetStatusWithDetails(state, shortText, details);
        return true;
    }

    if (action == PendingAction::UnlockDeleteAfterUnlock) {
        if (!result.targetExists) {
            state->pendingAction = PendingAction::None;
            SetStatus(state, L"The selected item no longer exists.");
            return true;
        }

        if (noDetectedLocks) {
            state->pendingAction = PendingAction::None;
            TryDeleteCurrentTarget(state, state->permanentDeleteDefault
                ? L"Unlocked and permanently deleted successfully."
                : L"Unlocked and moved to the Recycle Bin successfully.");
            return true;
        }

        const bool hasHandleLocks = std::any_of(
            state->locks.begin(), state->locks.end(),
            [](const LockProcess& process) { return process.foundByHandleScan; });
        if (hasHandleLocks) {
            if (ShowThemedConfirm(
                    state,
                    L"Unlock & Delete — Force Unlock",
                    L"Normal Unlock did not release the item.\n\n"
                    L"Try Force Unlock next? Genia Unlocker will close only file handles that match "
                    L"this target, without terminating the whole process. Critical Windows processes "
                    L"are skipped.\n\n"
                    L"An application can become unstable if it expects a closed handle to remain valid.",
                    L"Try Force Unlock",
                    L"Skip",
                    false,
                    true)) {
                state->pendingAction = PendingAction::UnlockDeleteAfterForceUnlock;
                SetStatus(state, L"Trying Force Unlock before process termination...");
                std::wstring details;
                if (!ExecuteForceUnlock(state, true, details)) {
                    state->pendingAction = PendingAction::None;
                    SetStatus(state, L"Force Unlock was not completed. The item was not deleted.");
                    return true;
                }
                state->pendingForceUnlockDetails = std::move(details);
                Sleep(250);
                StartScan(state);
                return true;
            }
        }

        StartTerminateThenDelete(state);
        return true;
    }

    if (action == PendingAction::UnlockDeleteAfterForceUnlock) {
        if (!result.targetExists) {
            state->pendingAction = PendingAction::None;
            SetStatusWithDetails(state, L"The selected item no longer exists.",
                                 state->pendingForceUnlockDetails);
            return true;
        }
        if (noDetectedLocks) {
            state->pendingAction = PendingAction::None;
            TryDeleteCurrentTarget(state, state->permanentDeleteDefault
                ? L"Force-unlocked and permanently deleted successfully."
                : L"Force-unlocked and moved to the Recycle Bin successfully.");
            return true;
        }
        state->lastStatusDetails = state->pendingForceUnlockDetails;
        StartTerminateThenDelete(state);
        return true;
    }

    if (action == PendingAction::UnlockDeleteAfterTerminate) {
        state->pendingAction = PendingAction::None;

        if (!result.targetExists) {
            SetStatus(state, L"The selected item no longer exists.");
            return true;
        }
        if (noDetectedLocks) {
            TryDeleteCurrentTarget(state, state->permanentDeleteDefault
                ? L"Blocking processes terminated and item permanently deleted."
                : L"Blocking processes terminated and item moved to the Recycle Bin.");
            return true;
        }

        std::wstring text =
            L"The item is still locked after process termination, so it was not deleted.";
        if (result.inaccessibleProcessCount != 0 && !IsRunningElevated()) {
            text += L" Use Scan as Admin for a deeper rescan.";
        }
        SetStatus(state, text);
        return true;
    }

    state->pendingAction = PendingAction::None;
    return false;
}

std::wstring BuildCurrentDetails(const AppState* state) {
    if (!state) return {};

    std::wstring text = L"TARGET\n";
    text += L"Path: " + (state->target.empty() ? std::wstring(L"(none)") : state->target);
    text += L"\nType: ";
    if (!state->lastTargetExists) {
        text += L"(not available)";
    } else {
        text += state->lastTargetIsDirectory ? L"Directory" : L"File";
    }

    text += L"\n\nSCAN RESULT\n";
    text += L"Status: " + (state->lastStatusDetails.empty()
        ? std::wstring(L"No additional scan details are available.")
        : state->lastStatusDetails);
    text += L"\nDuration: " + std::to_wstring(state->lastScanDurationMs) + L" ms";
    text += L"\nRestart Manager: ";
    text += state->lastRestartManagerError == ERROR_SUCCESS
        ? L"OK"
        : ErrorMessage(state->lastRestartManagerError);
    text += L"\nDisk handles inspected: " +
        std::to_wstring(state->lastInspectedDiskHandleCount);
    text += L"\nHandle type filter: ";
    text += state->lastHandleTypeFilterAvailable
        ? L"File ObjectTypeIndex detected"
        : L"Unavailable / partial scan";
    text += L"\nInaccessible file-handle owners (system-wide): " +
        std::to_wstring(state->lastInaccessibleProcessCount);

    text += L"\n\nBLOCKERS\n";
    if (state->locks.empty()) {
        text += L"None detected.";
    } else {
        text += std::to_wstring(state->locks.size()) +
            (state->locks.size() == 1 ? L" process detected." : L" processes detected.");
        for (const auto& process : state->locks) {
            text += L"\n\n" + BuildProcessInfoText(process);
        }
    }
    return text;
}

std::wstring BuildDiagnosticReport(const AppState* state) {
    if (!state) return {};

    std::wstring targetType = L"(unknown)";
    if (state->lastTargetExists) {
        targetType = state->lastTargetIsDirectory ? L"Directory" : L"File";
    }

    std::wstring deleteShare;
    if (state->lastDeleteShareProbeSucceeded) {
        deleteShare = L"Available";
    } else if (state->lastDeleteShareProbeError == ERROR_SHARING_VIOLATION) {
        deleteShare = L"Blocked (sharing violation)";
    } else if (state->lastTargetExists) {
        deleteShare = L"Unavailable: " + ErrorMessage(state->lastDeleteShareProbeError);
    } else {
        deleteShare = L"Not tested";
    }

    std::wstring report =
        L"Genia Unlocker diagnostic report\r\n"
        L"Version: " + std::wstring(kAppVersionDisplay) +
        L"\r\nElevated: " + std::wstring(IsRunningElevated() ? L"Yes" : L"No") +
        L"\r\nTarget: " + (state->target.empty() ? std::wstring(L"(none)") : state->target) +
        L"\r\nTarget exists: " + std::wstring(state->lastTargetExists ? L"Yes" : L"No") +
        L"\r\nTarget type: " + targetType +
        L"\r\nBlocking processes: " + std::to_wstring(state->locks.size()) +
        L"\r\nScan duration: " + std::to_wstring(state->lastScanDurationMs) + L" ms" +
        L"\r\nDelete-share probe: " + deleteShare +
        L"\r\nRestart Manager: " +
            std::wstring(state->lastRestartManagerError == ERROR_SUCCESS
                ? L"OK"
                : ErrorMessage(state->lastRestartManagerError)) +
        L"\r\nDisk handles inspected: " + std::to_wstring(state->lastInspectedDiskHandleCount) +
        L"\r\nHandle type filter: " +
            std::wstring(state->lastHandleTypeFilterAvailable ? L"File ObjectTypeIndex detected" : L"Unavailable / partial scan") +
        L"\r\nInaccessible file-handle owners (system-wide): " +
            std::to_wstring(state->lastInaccessibleProcessCount);

    if (!state->lastStatusDetails.empty()) {
        report += L"\r\n\r\nStatus details:\r\n" + state->lastStatusDetails;
    }

    if (!state->locks.empty()) {
        report += L"\r\n\r\nDetected blockers:";
        for (const auto& process : state->locks) {
            report += L"\r\n\r\n" + process.name +
                      L" (PID " + std::to_wstring(process.pid) + L")";
            if (!process.path.empty()) {
                report += L"\r\nExecutable: " + process.path;
            }

            std::wstring method;
            if (process.foundByRestartManager) method += L"Restart Manager";
            if (process.foundByHandleScan) {
                if (!method.empty()) method += L" + ";
                method += L"Handle";
            }
            if (process.foundByProcessImageScan) {
                if (!method.empty()) method += L" + ";
                method += L"Image/Module";
            }
            if (!method.empty()) {
                report += L"\r\nDetection: " + method;
            }

            if (!process.lockedObjects.empty()) {
                report += L"\r\nLocked objects:";
                for (const auto& object : process.lockedObjects) {
                    report += L"\r\n  - " + object;
                }
            }
        }
    }

    report += L"\r\n\r\nSource: " + std::wstring(kRepositoryUrl);
    return report;
}


struct DetailsState {
    AppState* app{};
    HWND closeButton{};
    HWND copyButton{};
    HWND edit{};
    HFONT font{};
    HFONT titleFont{};
    std::wstring detailsText;
    std::wstring reportText;
};

std::wstring ToEditText(std::wstring text) {
    std::wstring converted;
    converted.reserve(text.size() + 32);
    for (size_t i = 0; i < text.size(); ++i) {
        if (text[i] == L'\n' && (i == 0 || text[i - 1] != L'\r')) {
            converted += L'\r';
        }
        converted += text[i];
    }
    return converted;
}

LRESULT CALLBACK DetailsWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* details = reinterpret_cast<DetailsState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        details = reinterpret_cast<DetailsState*>(cs->lpCreateParams);
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(details));
    }

    AppState* app = details ? details->app : nullptr;
    switch (msg) {
    case WM_CREATE: {
        if (!details || !app) return -1;
        const int dpi = GetDpiForWindow(hwnd) > 0 ? GetDpiForWindow(hwnd) : 96;
        auto sc = [dpi](int px) { return MulDiv(px, dpi, 96); };

        details->font = CreateModernFont(dpi, 9, FW_NORMAL, L"Segoe UI Variable Text");
        details->titleFont = CreateModernFont(dpi, 13, FW_SEMIBOLD, L"Segoe UI Variable Display");

        details->closeButton = CreateWindowW(
            L"BUTTON", L"×",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            sc(548), sc(5), sc(24), sc(22), hwnd,
            ControlId(IDC_DETAILS_CLOSE), nullptr, nullptr);

        details->edit = CreateWindowExW(
            0, L"EDIT", ToEditText(details->detailsText).c_str(),
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_VSCROLL | WS_BORDER |
            ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
            sc(16), sc(58), sc(556), sc(260), hwnd,
            ControlId(IDC_DETAILS_EDIT), nullptr, nullptr);

        details->copyButton = CreateWindowW(
            L"BUTTON", L"Copy report",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            sc(16), sc(330), sc(104), sc(30), hwnd,
            ControlId(IDC_DETAILS_COPY), nullptr, nullptr);
        HWND close = CreateWindowW(
            L"BUTTON", L"Close",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
            sc(496), sc(330), sc(76), sc(30), hwnd,
            ControlId(IDCANCEL), nullptr, nullptr);

        for (HWND control : {details->closeButton, details->edit,
                             details->copyButton, close}) {
            SetFont(control, details->font);
            ModernTheme::ApplyControlTheme(control, app->palette.dark);
        }

        SendMessageW(details->edit, EM_SETSEL, 0, 0);
        ModernTheme::ApplyWindowChrome(hwnd, app->palette.dark);
        return 0;
    }

    case WM_ERASEBKGND:
        if (app && app->windowBrush) {
            RECT rc{};
            GetClientRect(hwnd, &rc);
            FillRect(reinterpret_cast<HDC>(wParam), &rc, app->windowBrush);
            return 1;
        }
        break;

    case WM_PAINT:
        if (details && app) {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd, &ps);
            RECT rc{};
            GetClientRect(hwnd, &rc);
            const int dpi = GetDpiForWindow(hwnd) > 0 ? GetDpiForWindow(hwnd) : 96;
            auto sc = [dpi](int px) { return MulDiv(px, dpi, 96); };

            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, app->palette.text);
            HGDIOBJ oldFont = details->font ? SelectObject(dc, details->font) : nullptr;

            RECT captionRc{sc(10), sc(3), rc.right - sc(48), sc(29)};
            DrawTextW(dc, L"Genia Unlocker — Details", -1, &captionRc,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);

            if (oldFont) SelectObject(dc, oldFont);
            oldFont = details->titleFont ? SelectObject(dc, details->titleFont) : nullptr;
            RECT titleRc{sc(16), sc(34), rc.right - sc(16), sc(56)};
            DrawTextW(dc, L"Scan details", -1, &titleRc,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE);
            if (oldFont) SelectObject(dc, oldFont);

            EndPaint(hwnd, &ps);
            return 0;
        }
        break;

    case WM_CTLCOLORSTATIC:
        if (app) {
            HDC dc = reinterpret_cast<HDC>(wParam);
            HWND control = reinterpret_cast<HWND>(lParam);
            if (control && GetDlgCtrlID(control) == IDC_DETAILS_EDIT) {
                SetTextColor(dc, app->palette.text);
                SetBkColor(dc, app->palette.surface);
                return reinterpret_cast<LRESULT>(app->surfaceBrush);
            }
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, app->palette.text);
            return reinterpret_cast<LRESULT>(app->windowBrush);
        }
        break;

    case WM_CTLCOLOREDIT:
        if (app) {
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetTextColor(dc, app->palette.text);
            SetBkColor(dc, app->palette.surface);
            return reinterpret_cast<LRESULT>(app->surfaceBrush);
        }
        break;

    case WM_CTLCOLORBTN:
        if (app) {
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, app->palette.text);
            return reinterpret_cast<LRESULT>(app->windowBrush);
        }
        break;

    case WM_DRAWITEM:
        if (app) {
            const auto* draw = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
            if (draw && draw->CtlType == ODT_BUTTON) {
                ModernTheme::ButtonKind kind =
                    draw->CtlID == IDC_DETAILS_COPY
                        ? ModernTheme::ButtonKind::Primary
                        : ModernTheme::ButtonKind::Secondary;
                ModernTheme::DrawButton(*draw, app->palette, kind);
                return TRUE;
            }
        }
        break;

    case WM_COMMAND:
        if (!details || !app) break;
        switch (LOWORD(wParam)) {
        case IDC_DETAILS_CLOSE:
        case IDCANCEL:
            if (HIWORD(wParam) == BN_CLICKED) {
                DestroyWindow(hwnd);
            }
            return 0;
        case IDC_DETAILS_COPY:
            if (HIWORD(wParam) == BN_CLICKED) {
                if (CopyTextToClipboard(hwnd, details->reportText)) {
                    SetWindowTextW(details->copyButton, L"Copied");
                    SetStatus(app, L"Diagnostic report copied to the clipboard.");
                    InvalidateRect(details->copyButton, nullptr, TRUE);
                } else {
                    MessageBoxW(hwnd, L"Could not copy the diagnostic report.",
                                kWindowTitle, MB_ICONERROR);
                }
            }
            return 0;
        }
        break;

    case WM_DPICHANGED:
        if (details && app) {
            const int dpi = HIWORD(wParam) > 0 ? HIWORD(wParam) : 96;
            const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
            if (suggested) {
                SetWindowPos(hwnd, nullptr,
                             suggested->left, suggested->top,
                             suggested->right - suggested->left,
                             suggested->bottom - suggested->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            }
            auto sc = [dpi](int px) { return MulDiv(px, dpi, 96); };

            if (details->font) DeleteObject(details->font);
            if (details->titleFont) DeleteObject(details->titleFont);
            details->font = CreateModernFont(dpi, 9, FW_NORMAL, L"Segoe UI Variable Text");
            details->titleFont = CreateModernFont(dpi, 13, FW_SEMIBOLD, L"Segoe UI Variable Display");

            MoveWindow(details->closeButton, sc(548), sc(5), sc(24), sc(22), TRUE);
            MoveWindow(details->edit, sc(16), sc(58), sc(556), sc(260), TRUE);
            MoveWindow(details->copyButton, sc(16), sc(330), sc(104), sc(30), TRUE);
            MoveWindow(GetDlgItem(hwnd, IDCANCEL), sc(496), sc(330), sc(76), sc(30), TRUE);

            for (HWND control : {details->closeButton, details->edit,
                                 details->copyButton, GetDlgItem(hwnd, IDCANCEL)}) {
                SetFont(control, details->font);
            }
            InvalidateRect(hwnd, nullptr, TRUE);
        }
        return 0;

    case WM_THEMECHANGED:
    case WM_SETTINGCHANGE:
        if (app) {
            ModernTheme::ApplyWindowChrome(hwnd, app->palette.dark);
            ModernTheme::ApplyControlTheme(details->edit, app->palette.dark);
            InvalidateRect(hwnd, nullptr, TRUE);
        }
        return 0;

    case WM_NCHITTEST: {
        POINT pt{GET_X_LPARAM(lParam), GET_Y_LPARAM(lParam)};
        if (details && details->closeButton) {
            RECT closeRc{};
            GetWindowRect(details->closeButton, &closeRc);
            if (PtInRect(&closeRc, pt)) {
                return HTCLIENT;
            }
        }
        RECT windowRc{};
        GetWindowRect(hwnd, &windowRc);
        const int dpi = GetDpiForWindow(hwnd) > 0 ? GetDpiForWindow(hwnd) : 96;
        const int captionHeight = MulDiv(32, dpi, 96);
        if (pt.y >= windowRc.top && pt.y < windowRc.top + captionHeight) {
            return HTCAPTION;
        }
        break;
    }

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        if (app && app->detailsWindow == hwnd) {
            app->detailsWindow = nullptr;
        }
        if (details) {
            if (details->font) DeleteObject(details->font);
            if (details->titleFont) DeleteObject(details->titleFont);
        }
        delete details;
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

void ShowDetailsWindow(AppState* state) {
    if (!state || !state->hwnd) return;

    if (state->detailsWindow && IsWindow(state->detailsWindow)) {
        auto* details = reinterpret_cast<DetailsState*>(
            GetWindowLongPtrW(state->detailsWindow, GWLP_USERDATA));
        if (details) {
            details->detailsText = BuildCurrentDetails(state);
            details->reportText = BuildDiagnosticReport(state);
            if (details->edit) {
                const std::wstring editText = ToEditText(details->detailsText);
                SetWindowTextW(details->edit, editText.c_str());
                SendMessageW(details->edit, EM_SETSEL, 0, 0);
            }
            if (details->copyButton) {
                SetWindowTextW(details->copyButton, L"Copy report");
            }
        }
        ShowWindow(state->detailsWindow, SW_RESTORE);
        SetForegroundWindow(state->detailsWindow);
        return;
    }

    HINSTANCE instance = reinterpret_cast<HINSTANCE>(
        GetWindowLongPtrW(state->hwnd, GWLP_HINSTANCE));
    static bool classReady = false;
    if (!classReady) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = DetailsWindowProc;
        wc.hInstance = instance;
        wc.hIcon = state->icon;
        wc.hIconSm = state->icon;
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = nullptr;
        wc.lpszClassName = kDetailsWindowClass;
        if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
            MessageBoxW(state->hwnd, L"Could not create the Details window.",
                        kWindowTitle, MB_ICONERROR);
            return;
        }
        classReady = true;
    }

    const int dpi = GetDpiForWindow(state->hwnd) > 0 ? GetDpiForWindow(state->hwnd) : 96;
    const int width = MulDiv(588, dpi, 96);
    const int height = MulDiv(374, dpi, 96);
    RECT owner{};
    GetWindowRect(state->hwnd, &owner);
    const int x = owner.left + ((owner.right - owner.left) - width) / 2;
    const int y = owner.top + ((owner.bottom - owner.top) - height) / 2;

    auto* details = new DetailsState{};
    details->app = state;
    details->detailsText = BuildCurrentDetails(state);
    details->reportText = BuildDiagnosticReport(state);

    HWND window = CreateWindowExW(
        WS_EX_TOOLWINDOW,
        kDetailsWindowClass,
        L"Genia Unlocker — Details",
        WS_POPUP | WS_BORDER,
        x, y, width, height,
        state->hwnd, nullptr, instance, details);
    if (!window) {
        delete details;
        MessageBoxW(state->hwnd, L"Could not open scan details.",
                    kWindowTitle, MB_ICONERROR);
        return;
    }

    state->detailsWindow = window;
    ShowWindow(window, SW_SHOWNORMAL);
    UpdateWindow(window);
}

void LayoutControls(AppState* state, int width, int height) {
    // Flat UX uses tighter rhythm and lets whitespace, rather than raised
    // containers, separate the title, target, process list and actions.
    const int margin = Scale(state, 12);
    const int gap = Scale(state, 6);
    const int buttonH = Scale(state, 28);
    const int chooseFileW = Scale(state, 72);
    const int chooseFolderW = Scale(state, 80);
    const int batchW = Scale(state, 72);
    const int settingsW = Scale(state, 78);

    int y = Scale(state, 9);
    MoveWindow(GetDlgItem(state->hwnd, IDC_APP_TITLE), margin, y,
               width - margin * 2 - settingsW - gap, Scale(state, 24), TRUE);
    MoveWindow(GetDlgItem(state->hwnd, IDC_SETTINGS), width - margin - settingsW, y,
               settingsW, Scale(state, 24), TRUE);

    y += Scale(state, 29);
    MoveWindow(GetDlgItem(state->hwnd, IDC_TARGET_LABEL), margin, y,
               Scale(state, 80), Scale(state, 15), TRUE);
    y += Scale(state, 16);

    int targetW = width - margin * 2 - chooseFileW - chooseFolderW - batchW - gap * 3;
    if (targetW < Scale(state, 210)) targetW = Scale(state, 210);
    MoveWindow(state->targetEdit, margin, y, targetW, buttonH, TRUE);
    CenterEditContent(state->targetEdit, state->dpi);
    MoveWindow(GetDlgItem(state->hwnd, IDC_FILE), margin + targetW + gap, y,
               chooseFileW, buttonH, TRUE);
    MoveWindow(GetDlgItem(state->hwnd, IDC_FOLDER),
               margin + targetW + gap + chooseFileW + gap, y,
               chooseFolderW, buttonH, TRUE);
    MoveWindow(GetDlgItem(state->hwnd, IDC_BATCH),
               margin + targetW + chooseFileW + chooseFolderW + gap * 3, y,
               batchW, buttonH, TRUE);

    const int listTop = y + buttonH + Scale(state, 7);
    const int bottomArea = Scale(state, 66);
    int listHeight = height - listTop - bottomArea;
    if (listHeight < Scale(state, 145)) listHeight = Scale(state, 145);
    const int listW = width - margin * 2;
    MoveWindow(state->list, margin, listTop, listW, listHeight, TRUE);
    UpdateListColumns(state, listW);

    const int actionsY = listTop + listHeight + Scale(state, 6);
    const int unlockW = Scale(state, 70);
    const int forceW = Scale(state, 92);
    const int terminateW = Scale(state, 80);
    const int destructiveW = Scale(state, 114);
    const int retryW = Scale(state, 88);

    int actionX = margin;
    MoveWindow(GetDlgItem(state->hwnd, IDC_UNLOCK), actionX, actionsY,
               unlockW, buttonH, TRUE);
    actionX += unlockW + gap;
    MoveWindow(GetDlgItem(state->hwnd, IDC_FORCE_UNLOCK), actionX, actionsY,
               forceW, buttonH, TRUE);
    actionX += forceW + gap;
    MoveWindow(GetDlgItem(state->hwnd, IDC_TERMINATE), actionX, actionsY,
               terminateW, buttonH, TRUE);
    actionX += terminateW + gap;
    MoveWindow(GetDlgItem(state->hwnd, IDC_DELETE), actionX, actionsY,
               destructiveW, buttonH, TRUE);
    MoveWindow(GetDlgItem(state->hwnd, IDC_UNLOCK_DELETE), actionX, actionsY,
               destructiveW, buttonH, TRUE);
    MoveWindow(GetDlgItem(state->hwnd, IDC_RETRY), width - margin - retryW, actionsY,
               retryW, buttonH, TRUE);

    const int statusY = actionsY + buttonH + Scale(state, 5);
    const int detailsW = Scale(state, 60);
    MoveWindow(state->status, margin, statusY,
               width - margin * 2 - detailsW - gap, Scale(state, 17), TRUE);
    MoveWindow(GetDlgItem(state->hwnd, IDC_DETAILS), width - margin - detailsW,
               statusY - Scale(state, 2), detailsW, Scale(state, 21), TRUE);
}

void CreateControls(AppState* state) {
    state->dpi = GetDpiForWindow(state->hwnd);
    if (state->dpi <= 0) state->dpi = 96;
    RecreateFonts(state);

    CreateWindowW(L"STATIC", L"Genia Unlocker",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        0, 0, 0, 0, state->hwnd, ControlId(IDC_APP_TITLE), nullptr, nullptr);
    CreateWindowW(L"BUTTON", L"Settings",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        0, 0, 0, 0, state->hwnd, ControlId(IDC_SETTINGS), nullptr, nullptr);
    CreateWindowW(L"STATIC", L"Target",
        WS_CHILD | WS_VISIBLE | SS_LEFT,
        0, 0, 0, 0, state->hwnd, ControlId(IDC_TARGET_LABEL), nullptr, nullptr);

    state->targetEdit = CreateWindowExW(0, L"EDIT", L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER |
        ES_MULTILINE | ES_AUTOHSCROLL | ES_READONLY,
        0, 0, 0, 0, state->hwnd, ControlId(IDC_TARGET), nullptr, nullptr);

    state->targetTooltip = CreateWindowExW(
        WS_EX_TOPMOST, TOOLTIPS_CLASSW, nullptr,
        WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
        state->hwnd, nullptr, nullptr, nullptr);
    if (state->targetTooltip) {
        TOOLINFOW tool{};
        tool.cbSize = sizeof(tool);
        tool.uFlags = TTF_IDISHWND | TTF_SUBCLASS;
        tool.hwnd = state->hwnd;
        tool.uId = reinterpret_cast<UINT_PTR>(state->targetEdit);
        tool.lpszText = LPSTR_TEXTCALLBACKW;
        SendMessageW(state->targetTooltip, TTM_ADDTOOLW, 0, reinterpret_cast<LPARAM>(&tool));
        SendMessageW(state->targetTooltip, TTM_SETMAXTIPWIDTH, 0, Scale(state, 760));
    }

    CreateWindowW(L"BUTTON", L"File...", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        0, 0, 0, 0, state->hwnd, ControlId(IDC_FILE), nullptr, nullptr);
    CreateWindowW(L"BUTTON", L"Folder...", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        0, 0, 0, 0, state->hwnd, ControlId(IDC_FOLDER), nullptr, nullptr);
    CreateWindowW(L"BUTTON", L"Batch...", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        0, 0, 0, 0, state->hwnd, ControlId(IDC_BATCH), nullptr, nullptr);

    state->list = CreateWindowExW(0, WC_LISTVIEWW, L"",
        WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER |
        LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
        0, 0, 0, 0, state->hwnd, ControlId(IDC_LIST), nullptr, nullptr);
    ListView_SetExtendedListViewStyle(state->list,
        LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP | LVS_EX_INFOTIP | LVS_EX_HEADERDRAGDROP);
    RecreateProcessImageList(state);

    struct Column { const wchar_t* text; int width; } columns[] = {
        {L"Process", 135}, {L"PID", 60}, {L"Method", 105}, {L"Locked object", 330}
    };
    for (int i = 0; i < 4; ++i) {
        LVCOLUMNW col{};
        col.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
        col.pszText = const_cast<LPWSTR>(columns[i].text);
        col.cx = Scale(state, columns[i].width);
        col.iSubItem = i;
        ListView_InsertColumn(state->list, i, &col);
    }

    if (HWND header = ListView_GetHeader(state->list)) {
        SetWindowSubclass(header, FlatHeaderSubclassProc, 1,
                          reinterpret_cast<DWORD_PTR>(state));
    }

    CreateWindowW(L"BUTTON", L"Unlock", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        0, 0, 0, 0, state->hwnd, ControlId(IDC_UNLOCK), nullptr, nullptr);
    CreateWindowW(L"BUTTON", L"Force Unlock", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        0, 0, 0, 0, state->hwnd, ControlId(IDC_FORCE_UNLOCK), nullptr, nullptr);
    CreateWindowW(L"BUTTON", L"Terminate", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        0, 0, 0, 0, state->hwnd, ControlId(IDC_TERMINATE), nullptr, nullptr);
    CreateWindowW(L"BUTTON", L"Delete", WS_CHILD | WS_TABSTOP | BS_OWNERDRAW,
        0, 0, 0, 0, state->hwnd, ControlId(IDC_DELETE), nullptr, nullptr);
    CreateWindowW(L"BUTTON", L"Unlock && Delete", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        0, 0, 0, 0, state->hwnd, ControlId(IDC_UNLOCK_DELETE), nullptr, nullptr);
    CreateWindowW(L"BUTTON", L"Rescan", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        0, 0, 0, 0, state->hwnd, ControlId(IDC_RETRY), nullptr, nullptr);

    state->status = CreateWindowW(L"STATIC", L"Choose a file or folder, or drop it here.",
        WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS,
        0, 0, 0, 0, state->hwnd, ControlId(IDC_STATUS), nullptr, nullptr);
    CreateWindowW(L"BUTTON", L"Details", WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
        0, 0, 0, 0, state->hwnd, ControlId(IDC_DETAILS), nullptr, nullptr);

    ApplyFonts(state);
    UpdateTheme(state);
    DragAcceptFiles(state->hwnd, TRUE);
    SetStatus(state, L"Choose a file or folder, or drop it here.");
    UpdateActionButtons(state);
}

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    AppState* state = reinterpret_cast<AppState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));

    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        state = reinterpret_cast<AppState*>(cs->lpCreateParams);
        state->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(state));
    }

    if (state && msg == state->taskbarCreated && state->taskbarCreated != 0) {
        AddTrayIcon(state);
        return 0;
    }

    switch (msg) {
    case WM_CREATE:
        CreateControls(state);
        AddTrayIcon(state);
        return 0;

    case WM_ERASEBKGND:
        if (state && state->windowBrush) {
            RECT rc{};
            GetClientRect(hwnd, &rc);
            FillRect(reinterpret_cast<HDC>(wParam), &rc, state->windowBrush);
            return 1;
        }
        break;

    case WM_CTLCOLORSTATIC: {
        if (!state) break;
        HDC dc = reinterpret_cast<HDC>(wParam);
        HWND control = reinterpret_cast<HWND>(lParam);
        const int id = GetDlgCtrlID(control);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, (id == IDC_STATUS)
                             ? state->palette.muted
                             : state->palette.text);
        if (id == IDC_TARGET) {
            SetBkMode(dc, OPAQUE);
            SetBkColor(dc, state->palette.surface);
            return reinterpret_cast<LRESULT>(state->surfaceBrush);
        }
        return reinterpret_cast<LRESULT>(state->windowBrush);
    }

    case WM_CTLCOLOREDIT: {
        if (!state) break;
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetTextColor(dc, state->palette.text);
        SetBkColor(dc, state->palette.surface);
        return reinterpret_cast<LRESULT>(state->surfaceBrush);
    }

    case WM_CTLCOLORBTN: {
        if (!state) break;
        HDC dc = reinterpret_cast<HDC>(wParam);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, state->palette.text);
        return reinterpret_cast<LRESULT>(state->windowBrush);
    }

    case WM_DRAWITEM: {
        if (!state) break;
        const auto* draw = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
        if (!draw || draw->CtlType != ODT_BUTTON) break;
        ModernTheme::ButtonKind kind = ModernTheme::ButtonKind::Secondary;
        if (draw->CtlID == IDC_UNLOCK) {
            kind = ModernTheme::ButtonKind::Primary;
        } else if (draw->CtlID == IDC_TERMINATE ||
                   draw->CtlID == IDC_DELETE ||
                   draw->CtlID == IDC_UNLOCK_DELETE) {
            kind = ModernTheme::ButtonKind::Danger;
        }
        ModernTheme::DrawButton(*draw, state->palette, kind);
        return TRUE;
    }

    case WM_GETMINMAXINFO:
        if (state) {
            auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
            info->ptMinTrackSize.x = Scale(state, 700);
            info->ptMinTrackSize.y = Scale(state, 380);
            return 0;
        }
        break;

    case WM_SIZE:
        if (state) {
            LayoutControls(state, LOWORD(lParam), HIWORD(lParam));
        }
        return 0;

    case WM_DPICHANGED:
        if (state) {
            CaptureColumnWidths(state);
            state->columnsInitialized = false;
            state->dpi = HIWORD(wParam);
            if (state->dpi <= 0) state->dpi = 96;
            RecreateFonts(state);
            ApplyFonts(state);
            RecreateProcessImageList(state);
            PopulateList(state);
            const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
            if (suggested) {
                SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                             suggested->right - suggested->left, suggested->bottom - suggested->top,
                             SWP_NOZORDER | SWP_NOACTIVATE);
            }
            RECT client{};
            GetClientRect(hwnd, &client);
            LayoutControls(state, client.right - client.left, client.bottom - client.top);
            UpdateTheme(state);
        }
        return 0;

    case WM_THEMECHANGED:
    case WM_SETTINGCHANGE:
        if (state) {
            UpdateTheme(state);
        }
        return 0;

    case WM_DROPFILES: {
        HDROP drop = reinterpret_cast<HDROP>(wParam);
        const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
        std::vector<std::wstring> paths;
        paths.reserve((std::min)(count, 256u));
        for (UINT i = 0; i < count && i < 256u; ++i) {
            const UINT length = DragQueryFileW(drop, i, nullptr, 0);
            if (length == 0 || length >= 32768) continue;
            std::wstring path(length + 1, L'\0');
            if (DragQueryFileW(drop, i, path.data(), length + 1) == length) {
                path.resize(length);
                paths.push_back(std::move(path));
            }
        }
        DragFinish(drop);
        if (paths.size() == 1) {
            SetTarget(state, paths.front());
        } else if (!paths.empty()) {
            ShowBatchScanWindow(hwnd, paths);
        }
        return 0;
    }

    case WM_COMMAND: {
        const int id = LOWORD(wParam);
        if (id == IDC_FILE || id == ID_TRAY_FILE) {
            std::wstring path = PickPath(hwnd, false);
            if (!path.empty()) SetTarget(state, path);
        } else if (id == IDC_FOLDER) {
            std::wstring path = PickPath(hwnd, true);
            if (!path.empty()) SetTarget(state, path);
        } else if (id == IDC_BATCH) {
            ShowBatchScanWindow(hwnd);
        } else if (id == IDC_UNLOCK) {
            GracefulUnlock(state);
        } else if (id == IDC_FORCE_UNLOCK) {
            ForceUnlockCurrent(state);
        } else if (id == IDC_TERMINATE) {
            TerminateSelected(state);
        } else if (id == IDC_DELETE) {
            DeleteOnly(state);
        } else if (id == IDC_UNLOCK_DELETE) {
            UnlockAndDelete(state);
        } else if (id == IDC_RETRY) {
            RetryScan(state);
        } else if (id == IDC_SETTINGS) {
            ShowSettingsWindow(state);
        } else if (id == IDC_DETAILS) {
            ShowDetailsWindow(state);
        } else if (id == ID_TRAY_ABOUT) {
            ShowAboutDialog(state);
        } else if (id == ID_TRAY_EXIT) {
            state->exiting = true;
            DestroyWindow(hwnd);
        } else if (id == ID_TRAY_OPEN) {
            ActivateMainWindow(state);
        }
        return 0;
    }

    case WM_NOTIFY: {
        if (state) {
            const auto* header = reinterpret_cast<const NMHDR*>(lParam);

            if (header && header->hwndFrom == state->targetTooltip &&
                header->code == TTN_GETDISPINFOW) {
                auto* tip = reinterpret_cast<NMTTDISPINFOW*>(lParam);
                tip->lpszText = state->target.empty()
                    ? const_cast<LPWSTR>(L"No target selected")
                    : const_cast<LPWSTR>(state->target.c_str());
                return 0;
            }

            const HWND listHeader = state->list ? ListView_GetHeader(state->list) : nullptr;
            if (header && listHeader && header->hwndFrom == listHeader &&
                (header->code == HDN_ENDTRACKW || header->code == HDN_ENDTRACKA)) {
                CaptureColumnWidths(state);
                SaveLayoutPreferences(state);
            }
            if (header && listHeader && header->hwndFrom == listHeader &&
                header->code == NM_CUSTOMDRAW) {
                auto* custom = reinterpret_cast<NMCUSTOMDRAW*>(lParam);
                if (custom->dwDrawStage == CDDS_PREPAINT) {
                    return CDRF_NOTIFYITEMDRAW;
                }
                if (custom->dwDrawStage == CDDS_ITEMPREPAINT) {
                    const int itemIndex = static_cast<int>(custom->dwItemSpec);
                    wchar_t textBuffer[128]{};
                    HDITEMW item{};
                    item.mask = HDI_TEXT;
                    item.pszText = textBuffer;
                    item.cchTextMax = static_cast<int>(_countof(textBuffer));
                    Header_GetItem(listHeader, itemIndex, &item);

                    const COLORREF fill = (custom->uItemState & CDIS_SELECTED)
                        ? state->palette.surfacePressed
                        : state->palette.surface;
                    HBRUSH brush = CreateSolidBrush(fill);
                    FillRect(custom->hdc, &custom->rc, brush);
                    DeleteObject(brush);

                    HPEN pen = CreatePen(PS_SOLID, 1, state->palette.border);
                    HGDIOBJ oldPen = SelectObject(custom->hdc, pen);
                    MoveToEx(custom->hdc, custom->rc.right - 1, custom->rc.top, nullptr);
                    LineTo(custom->hdc, custom->rc.right - 1, custom->rc.bottom);
                    MoveToEx(custom->hdc, custom->rc.left, custom->rc.bottom - 1, nullptr);
                    LineTo(custom->hdc, custom->rc.right, custom->rc.bottom - 1);
                    SelectObject(custom->hdc, oldPen);
                    DeleteObject(pen);

                    SetBkMode(custom->hdc, TRANSPARENT);
                    SetTextColor(custom->hdc, state->palette.text);
                    HGDIOBJ oldFont = state->font ? SelectObject(custom->hdc, state->font) : nullptr;
                    RECT textRc = custom->rc;
                    textRc.left += Scale(state, 8);
                    textRc.right -= Scale(state, 6);
                    DrawTextW(custom->hdc, textBuffer, -1, &textRc,
                              DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                    if (oldFont) SelectObject(custom->hdc, oldFont);
                    return CDRF_SKIPDEFAULT;
                }
            }

            if (header && header->idFrom == IDC_LIST) {
                if (header->code == LVN_ITEMCHANGED) {
                    UpdateActionButtons(state);
                } else if (header->code == NM_DBLCLK) {
                    const auto* activate = reinterpret_cast<const NMITEMACTIVATE*>(lParam);
                    if (activate && activate->iItem >= 0) {
                        RevealProcessExecutable(state, activate->iItem);
                    }
                } else if (header->code == NM_RCLICK) {
                    const auto* activate = reinterpret_cast<const NMITEMACTIVATE*>(lParam);
                    if (activate && activate->iItem >= 0) {
                        POINT pt{};
                        GetCursorPos(&pt);
                        ShowProcessContextMenu(state, activate->iItem, pt);
                    }
                } else if (header->code == LVN_GETINFOTIPW) {
                    auto* tip = reinterpret_cast<NMLVGETINFOTIPW*>(lParam);
                    if (tip && tip->iItem >= 0 &&
                        static_cast<size_t>(tip->iItem) < state->locks.size() &&
                        tip->pszText && tip->cchTextMax > 0) {
                        const std::wstring text = BuildProcessInfoText(
                            state->locks[static_cast<size_t>(tip->iItem)]);
                        wcsncpy_s(tip->pszText, static_cast<size_t>(tip->cchTextMax),
                                  text.c_str(), _TRUNCATE);
                    }
                }
            }
        }
        break;
    }

    case WM_APP_SCAN_DONE: {
        std::unique_ptr<ScanPayload> payload(reinterpret_cast<ScanPayload*>(lParam));
        if (!payload || payload->generation != state->scanGeneration.load()) {
            return 0;
        }

        ScanResult& result = payload->result;
        state->scanInProgress = false;
        state->lastScanDurationMs = payload->elapsedMs;
        state->lastTargetExists = result.targetExists;
        state->lastTargetIsDirectory = result.targetIsDirectory;
        state->lastInaccessibleProcessCount = result.inaccessibleProcessCount;
        state->lastRestartManagerError = result.restartManagerError;
        state->lastInspectedDiskHandleCount = result.inspectedDiskHandleCount;
        state->lastDeleteShareProbeError = result.deleteShareProbeError;
        state->lastHandleTypeFilterAvailable = result.handleTypeFilterAvailable;
        state->lastDeleteShareProbeSucceeded = result.deleteShareProbeSucceeded;
        state->locks = std::move(result.processes);
        PopulateList(state);

        const bool unidentifiedSharingLock =
            state->locks.empty() &&
            result.deleteShareProbeError == ERROR_SHARING_VIOLATION;
        if (unidentifiedSharingLock && !IsRunningElevated()) {
            SetWindowTextW(GetDlgItem(hwnd, IDC_RETRY), L"Scan as Admin");
        } else {
            SetWindowTextW(GetDlgItem(hwnd, IDC_RETRY), L"Rescan");
        }
        UpdateActionButtons(state);

        if (HandlePendingActionAfterScan(state, result)) {
            UpdateActionButtons(state);
            return 0;
        }

        if (!result.targetExists) {
            SetStatus(state, L"The selected path no longer exists.");
        } else if (state->locks.empty()) {
            if (result.deleteShareProbeError == ERROR_SHARING_VIOLATION) {
                std::wstring details =
                    L"Windows confirms that delete sharing is blocked for this target, "
                    L"but the owning process was not identified by Restart Manager or the native handle scan.";
                if (result.inaccessibleProcessCount != 0) {
                    details += L" " + std::to_wstring(result.inaccessibleProcessCount) +
                        (result.inaccessibleProcessCount == 1
                            ? L" file-handle owner could not be inspected."
                            : L" file-handle owners could not be inspected.");
                }
                if (!IsRunningElevated()) {
                    details += L" Scan as Admin may identify an elevated owner.";
                    SetStatusWithDetails(state, L"Locked — blocker not identified. Administrator scan recommended.", details);
                } else {
                    SetStatusWithDetails(state, L"Locked — blocker not identified.", details);
                }
            } else if (result.deleteShareProbeSucceeded) {
                std::wstring details;
                if (result.inaccessibleProcessCount != 0) {
                    details = std::to_wstring(result.inaccessibleProcessCount) +
                        (result.inaccessibleProcessCount == 1
                            ? L" system-wide file-handle owner could not be inspected."
                            : L" system-wide file-handle owners could not be inspected.") +
                        L" Delete sharing is available for the selected target, so these are not treated as target blockers.";
                }
                if (!result.handleTypeFilterAvailable) {
                    if (!details.empty()) details += L" ";
                    details += L"Native File ObjectTypeIndex filtering was unavailable, so handle-scan coverage is partial.";
                }
                if (details.empty()) {
                    SetStatus(state, L"✓ No locking processes detected · " +
                        std::to_wstring(state->lastScanDurationMs) + L" ms");
                } else {
                    SetStatusWithDetails(state,
                        L"✓ No locking processes detected · " +
                            std::to_wstring(state->lastScanDurationMs) + L" ms",
                        details);
                }
            } else {
                std::wstring details = L"No blocker was detected, but delete-share verification failed: " +
                    ErrorMessage(result.deleteShareProbeError);
                if (!result.handleTypeFilterAvailable) {
                    details += L" Native handle-scan coverage is partial.";
                }
                SetStatusWithDetails(state, L"No blocker detected — verification incomplete.", details);
            }
        } else {
            std::wstring shortText = L"\u25CF " + std::to_wstring(state->locks.size()) +
                (state->locks.size() == 1 ? L" locking process" : L" locking processes") +
                L" · " + std::to_wstring(state->lastScanDurationMs) + L" ms";
            size_t lockedObjectCount = 0;
            for (const auto& process : state->locks) {
                lockedObjectCount += process.lockedObjects.size();
            }
            std::wstring details = std::to_wstring(state->locks.size()) +
                (state->locks.size() == 1 ? L" locking process detected." : L" locking processes detected.");
            if (lockedObjectCount != 0) {
                details += L" " + std::to_wstring(lockedObjectCount) +
                    (lockedObjectCount == 1 ? L" locked object path identified."
                                            : L" locked object paths identified.");
            }
            if (result.restartManagerError != ERROR_SUCCESS) {
                details += L" Handle scan completed; Restart Manager did not accept this resource.";
            }
            SetStatusWithDetails(state, shortText, details);
        }
        return 0;
    }

    case WM_APP_TRAY:
        if (LOWORD(lParam) == WM_LBUTTONDBLCLK) {
            ActivateMainWindow(state);
        } else if (LOWORD(lParam) == WM_RBUTTONUP || LOWORD(lParam) == WM_CONTEXTMENU) {
            ShowTrayMenu(state);
        }
        return 0;

    case WM_COPYDATA: {
        const COPYDATASTRUCT* copy = reinterpret_cast<const COPYDATASTRUCT*>(lParam);
        if (copy && copy->lpData && copy->cbData >= sizeof(wchar_t)) {
            const wchar_t* incoming = reinterpret_cast<const wchar_t*>(copy->lpData);
            if (*incoming) {
                SetTarget(state, incoming);
            } else {
                ActivateMainWindow(state);
            }
        }
        return TRUE;
    }

    case WM_CLOSE:
        // Portable tray behavior: closing the window does not terminate the helper.
        ShowWindow(hwnd, SW_HIDE);
        return 0;

    case WM_DESTROY:
        if (state) {
            SaveLayoutPreferences(state);
            if (state->detailsWindow && IsWindow(state->detailsWindow)) {
                DestroyWindow(state->detailsWindow);
                state->detailsWindow = nullptr;
            }
            if (state->aboutWindow && IsWindow(state->aboutWindow)) {
                DestroyWindow(state->aboutWindow);
                state->aboutWindow = nullptr;
            }
            if (state->settingsWindow && IsWindow(state->settingsWindow)) {
                DestroyWindow(state->settingsWindow);
                state->settingsWindow = nullptr;
            }
            RemoveTrayIcon(state);
            if (state->processImageList) {
                ListView_SetImageList(state->list, nullptr, LVSIL_SMALL);
                ImageList_Destroy(state->processImageList);
                state->processImageList = nullptr;
            }
            if (state->font) DeleteObject(state->font);
            if (state->titleFont) DeleteObject(state->titleFont);
            if (state->windowBrush) DeleteObject(state->windowBrush);
            if (state->surfaceBrush) DeleteObject(state->surfaceBrush);
        }
        PostQuitMessage(0);
        return 0;
    }

    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

bool ForwardToExistingInstance(const std::wstring& target) {
    HWND existing = FindWindowW(kWindowClass, nullptr);
    if (!existing) {
        return false;
    }

    // When Explorer starts this short-lived forwarding instance, transfer the
    // foreground permission to the already-running Genia Unlocker process.
    DWORD existingPid = 0;
    GetWindowThreadProcessId(existing, &existingPid);
    if (existingPid != 0) {
        AllowSetForegroundWindow(existingPid);
    }

    std::wstring payload = target;
    payload.push_back(L'\0');
    COPYDATASTRUCT copy{};
    copy.dwData = 1;
    copy.cbData = static_cast<DWORD>(payload.size() * sizeof(wchar_t));
    copy.lpData = payload.data();
    SendMessageTimeoutW(existing, WM_COPYDATA, 0, reinterpret_cast<LPARAM>(&copy),
                        SMTO_ABORTIFHUNG, 2000, nullptr);
    return true;
}

int HandleCommandOnlyActions(const ParsedArgs& args) {
    std::wstring error;
    if (args.installShell) {
        if (!SetShellIntegrationEnabled(true, error)) {
            MessageBoxW(nullptr, error.c_str(), kWindowTitle, MB_ICONERROR);
            return 1;
        }
        return 0;
    }
    if (args.uninstallShell) {
        if (!SetShellIntegrationEnabled(false, error)) {
            MessageBoxW(nullptr, error.c_str(), kWindowTitle, MB_ICONERROR);
            return 1;
        }
        return 0;
    }
    if (args.enableAutostart) {
        if (!SetAutostartEnabled(true, error)) {
            MessageBoxW(nullptr, error.c_str(), kWindowTitle, MB_ICONERROR);
            return 1;
        }
        return 0;
    }
    if (args.disableAutostart) {
        if (!SetAutostartEnabled(false, error)) {
            MessageBoxW(nullptr, error.c_str(), kWindowTitle, MB_ICONERROR);
            return 1;
        }
        return 0;
    }
    return -1;
}

} // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int showCommand) {
    ParsedArgs args = ParseArguments();

    if (!args.elevatedForceUnlockTarget.empty()) {
        const ForceUnlockResult result = ForceUnlockHandles(args.elevatedForceUnlockTarget);
        if (!result.targetExists) {
            return ERROR_FILE_NOT_FOUND;
        }
        // The caller always performs a verification rescan. An elevated pass may
        // still encounter PPL/protected processes; that is not a helper-launch
        // failure and should not discard handles that were successfully closed.
        return ERROR_SUCCESS;
    }

    if (!args.elevatedKillPids.empty()) {
        for (DWORD pid : args.elevatedKillPids) {
            DWORD error = ERROR_SUCCESS;
            if (!TerminateProcessByPid(pid, error)) {
                // The process may have exited while UAC was being approved.
                if (error == ERROR_INVALID_PARAMETER || error == ERROR_NOT_FOUND) {
                    continue;
                }
                return static_cast<int>(error);
            }
        }
        return ERROR_SUCCESS;
    }

    if (args.elevatedKillPid != 0) {
        DWORD error = ERROR_SUCCESS;
        return TerminateProcessByPid(args.elevatedKillPid, error)
            ? ERROR_SUCCESS
            : static_cast<int>(error);
    }

    int commandResult = HandleCommandOnlyActions(args);
    if (commandResult >= 0) {
        return commandResult;
    }

    if (args.showHelp) {
        MessageBoxW(nullptr,
            L"Genia Unlocker\n\n"
            L"--target <path>\n"
            L"--tray\n"
            L"--install-shell / --uninstall-shell\n"
            L"--enable-autostart / --disable-autostart\n"
            L"Portable build: one native EXE, optional GeniaUnlocker.ini beside it.\n"
            L"Force Unlock closes matching file handles and skips critical Windows processes.",
            kWindowTitle, MB_ICONINFORMATION);
        return 0;
    }

    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_LISTVIEW_CLASSES | ICC_STANDARD_CLASSES};
    InitCommonControlsEx(&icc);
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);

    // An elevated rescan deliberately uses a separate singleton scope. The
    // non-elevated instance launches it and immediately exits, avoiding a race
    // where the elevated child would forward its target back to the process
    // that lacks permission to inspect the blocker.
    const wchar_t* mutexName = args.elevatedInstance
        ? L"Local\\GeniaUnlocker.Singleton.Elevated.1"
        : kMutexName;
    HANDLE mutex = CreateMutexW(nullptr, FALSE, mutexName);
    bool alreadyRunning = mutex && GetLastError() == ERROR_ALREADY_EXISTS;
    if (alreadyRunning) {
        if (!args.tray || !args.target.empty()) {
            ForwardToExistingInstance(args.target);
        }
        if (mutex) CloseHandle(mutex);
        CoUninitialize();
        return 0;
    }

    AppState state;
    state.permanentDeleteDefault = LoadPermanentDeleteDefault();
    LoadLayoutPreferences(state);
    state.icon = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_APP_ICON), IMAGE_ICON,
                                               0, 0, LR_DEFAULTSIZE));
    if (!state.icon) {
        state.icon = LoadIconW(nullptr, IDI_APPLICATION);
    }
    state.taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");

    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = instance;
    wc.hIcon = state.icon;
    wc.hIconSm = state.icon;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;
    wc.lpszClassName = kWindowClass;
    if (!RegisterClassExW(&wc)) {
        if (mutex) CloseHandle(mutex);
        CoUninitialize();
        return 1;
    }

    const int startupDpi = GetDpiForSystem() > 0 ? static_cast<int>(GetDpiForSystem()) : 96;
    const int startupWidth = MulDiv(state.restoredWindowWidth, startupDpi, 96);
    const int startupHeight = MulDiv(state.restoredWindowHeight, startupDpi, 96);
    HWND hwnd = CreateWindowExW(0, kWindowClass, kWindowTitle,
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        CW_USEDEFAULT, CW_USEDEFAULT, startupWidth, startupHeight,
        nullptr, nullptr, instance, &state);
    if (!hwnd) {
        if (mutex) CloseHandle(mutex);
        CoUninitialize();
        return 1;
    }

    if (!args.target.empty()) {
        SetTarget(&state, args.target, !args.tray);
    }

    if (!args.tray) {
        ShowWindow(hwnd, showCommand == 0 ? SW_SHOWNORMAL : showCommand);
        UpdateWindow(hwnd);
    }

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (mutex) CloseHandle(mutex);
    CoUninitialize();
    return static_cast<int>(msg.wParam);
}

#include "BatchScanWindow.h"
#include "LockScanner.h"
#include "ModernTheme.h"

#include <commctrl.h>
#include <shobjidl.h>
#include <shellapi.h>

#include <algorithm>
#include <atomic>
#include <cwchar>
#include <memory>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#pragma comment(lib, "Comctl32.lib")
#pragma comment(lib, "Ole32.lib")
#pragma comment(lib, "Shell32.lib")

namespace {

constexpr wchar_t kBatchClass[] = L"GeniaUnlocker.BatchScanWindow";
constexpr UINT WM_BATCH_DONE = WM_APP + 72;
constexpr int kMaxTargets = 64;

enum : int {
    IDC_BATCH_LIST = 7101,
    IDC_BATCH_STATUS = 7102,
    IDC_BATCH_ADD_FILES = 7103,
    IDC_BATCH_ADD_FOLDERS = 7104,
    IDC_BATCH_SCAN = 7105,
    IDC_BATCH_OPEN = 7106,
    IDC_BATCH_CLOSE = 7107,
    IDC_BATCH_TITLE = 7108,
};

struct BatchEntry {
    ScanResult scan;
    ULONGLONG elapsedMs{};
};

struct BatchPayload {
    unsigned long long generation{};
    std::vector<BatchEntry> entries;
};

struct BatchState {
    HWND hwnd{};
    HWND owner{};
    HWND list{};
    HWND status{};
    HFONT font{};
    HFONT titleFont{};
    HBRUSH background{};
    HBRUSH surface{};
    ModernTheme::Palette palette{};
    int dpi{96};
    std::vector<std::wstring> targets;
    std::vector<BatchEntry> results;
    unsigned long long generation{};
    bool busy{};
    std::shared_ptr<std::atomic<bool>> cancelled;
};

HWND gBatchWindow = nullptr;
std::atomic<unsigned long long> gNextGeneration{0};

HMENU BatchId(int id) {
    return reinterpret_cast<HMENU>(static_cast<INT_PTR>(id));
}

int S(const BatchState* b, int v) {
    return MulDiv(v, b->dpi, 96);
}

void ApplyBatchFonts(BatchState* b) {
    if (b->font) DeleteObject(b->font);
    if (b->titleFont) DeleteObject(b->titleFont);
    b->font = CreateFontW(-S(b, 13), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    b->titleFont = CreateFontW(-S(b, 19), 0, 0, 0, FW_SEMIBOLD, FALSE, FALSE, FALSE,
        DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
        CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
    for (int id : {IDC_BATCH_LIST, IDC_BATCH_STATUS, IDC_BATCH_ADD_FILES,
                   IDC_BATCH_ADD_FOLDERS, IDC_BATCH_SCAN, IDC_BATCH_OPEN,
                   IDC_BATCH_CLOSE}) {
        SendMessageW(GetDlgItem(b->hwnd, id), WM_SETFONT,
                     reinterpret_cast<WPARAM>(b->font), TRUE);
    }
    SendMessageW(GetDlgItem(b->hwnd, IDC_BATCH_TITLE), WM_SETFONT,
                 reinterpret_cast<WPARAM>(b->titleFont), TRUE);
}

void ApplyBatchTheme(BatchState* b) {
    b->palette = ModernTheme::QueryPalette();
    if (b->background) DeleteObject(b->background);
    if (b->surface) DeleteObject(b->surface);
    b->background = CreateSolidBrush(b->palette.window);
    b->surface = CreateSolidBrush(b->palette.surface);
    ModernTheme::ApplyWindowChrome(b->hwnd, b->palette.dark);
    ModernTheme::ApplyControlTheme(b->list, b->palette.dark);
    ModernTheme::ApplyControlTheme(ListView_GetHeader(b->list), b->palette.dark);
    ListView_SetBkColor(b->list, b->palette.surface);
    ListView_SetTextBkColor(b->list, b->palette.surface);
    ListView_SetTextColor(b->list, b->palette.text);
    RedrawWindow(b->hwnd, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN | RDW_FRAME);
}

void LayoutBatch(BatchState* b, int w, int h) {
    const int m = S(b, 14);
    const int gap = S(b, 8);
    const int buttonH = S(b, 29);
    const int titleH = S(b, 27);
    MoveWindow(GetDlgItem(b->hwnd, IDC_BATCH_TITLE), m, S(b, 11),
               w - 2 * m, titleH, TRUE);
    int listTop = S(b, 48);
    int footerTop = h - S(b, 87);
    int listH = (std::max)(S(b, 140), footerTop - listTop);
    MoveWindow(b->list, m, listTop, (std::max)(1, w - 2 * m), listH, TRUE);
    int buttonY = listTop + listH + gap;
    int x = m;
    for (const auto& spec : {std::pair<int, int>{IDC_BATCH_ADD_FILES, 92},
                             {IDC_BATCH_ADD_FOLDERS, 104},
                             {IDC_BATCH_SCAN, 80},
                             {IDC_BATCH_OPEN, 117}}) {
        MoveWindow(GetDlgItem(b->hwnd, spec.first), x, buttonY,
                   S(b, spec.second), buttonH, TRUE);
        x += S(b, spec.second) + gap;
    }
    MoveWindow(GetDlgItem(b->hwnd, IDC_BATCH_CLOSE),
               w - m - S(b, 80), buttonY, S(b, 80), buttonH, TRUE);
    MoveWindow(b->status, m, buttonY + buttonH + S(b, 5),
               w - 2 * m, S(b, 18), TRUE);
}

std::wstring StatusFor(const ScanResult& result) {
    if (!result.targetExists) return L"Missing";
    if (!result.processes.empty()) return L"Locked";
    if (result.deleteShareProbeError == ERROR_SHARING_VIOLATION)
        return L"Locked (unknown)";
    if (result.deleteShareProbeSucceeded) return L"Clean";
    return L"Review";
}

std::wstring BlockersFor(const ScanResult& result) {
    if (!result.targetExists) return L"Path does not exist";
    if (result.processes.empty()) {
        if (result.deleteShareProbeError == ERROR_SHARING_VIOLATION)
            return L"Sharing violation — owner not identified";
        if (!result.deleteShareProbeSucceeded)
            return L"Delete-share check returned " +
                std::to_wstring(result.deleteShareProbeError);
        return L"—";
    }
    std::wstring text;
    for (size_t i = 0; i < result.processes.size() && i < 3; ++i) {
        if (!text.empty()) text += L", ";
        text += result.processes[i].name + L" (" +
            std::to_wstring(result.processes[i].pid) + L")";
    }
    if (result.processes.size() > 3) text += L", ...";
    return text;
}

void RefreshBatchRows(BatchState* b) {
    if (!b || !b->list) return;
    ListView_DeleteAllItems(b->list);
    for (size_t i = 0; i < b->targets.size(); ++i) {
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = static_cast<int>(i);
        item.pszText = const_cast<LPWSTR>(b->targets[i].c_str());
        int row = ListView_InsertItem(b->list, &item);
        std::wstring status = b->busy ? L"Scanning..." : L"Not scanned";
        std::wstring blockers = L"—";
        std::wstring duration;
        if (!b->busy && i < b->results.size()) {
            status = StatusFor(b->results[i].scan);
            blockers = BlockersFor(b->results[i].scan);
            duration = std::to_wstring(b->results[i].elapsedMs) + L" ms";
        }
        ListView_SetItemText(b->list, row, 1, status.data());
        ListView_SetItemText(b->list, row, 2, blockers.data());
        ListView_SetItemText(b->list, row, 3, duration.data());
    }
    EnableWindow(GetDlgItem(b->hwnd, IDC_BATCH_OPEN),
                 !b->busy && ListView_GetNextItem(b->list, -1, LVNI_SELECTED) >= 0);
}

void SetBatchBusy(BatchState* b, bool busy) {
    b->busy = busy;
    for (int id : {IDC_BATCH_ADD_FILES, IDC_BATCH_ADD_FOLDERS,
                   IDC_BATCH_SCAN, IDC_BATCH_OPEN}) {
        EnableWindow(GetDlgItem(b->hwnd, id), !busy);
    }
}

void StartBatchScan(BatchState* b) {
    if (!b || b->busy || b->targets.empty()) return;
    if (b->cancelled) b->cancelled->store(true);
    b->cancelled = std::make_shared<std::atomic<bool>>(false);
    auto ticket = b->cancelled;
    auto targets = b->targets;
    const HWND hwnd = b->hwnd;
    const auto generation = ++gNextGeneration;
    b->generation = generation;
    b->results.clear();
    SetBatchBusy(b, true);
    RefreshBatchRows(b);
    const std::wstring status = L"Scanning " +
        std::to_wstring(targets.size()) + L" targets (read-only)...";
    SetWindowTextW(b->status, status.c_str());

    std::thread([targets = std::move(targets), ticket, hwnd, generation]() {
        auto payload = std::make_unique<BatchPayload>();
        payload->generation = generation;
        payload->entries.reserve(targets.size());
        for (const auto& path : targets) {
            if (ticket->load()) return;
            BatchEntry entry;
            const ULONGLONG started = GetTickCount64();
            entry.scan = ScanLocks(path);
            entry.elapsedMs = GetTickCount64() - started;
            payload->entries.push_back(std::move(entry));
        }
        if (!ticket->load() &&
            PostMessageW(hwnd, WM_BATCH_DONE, 0,
                         reinterpret_cast<LPARAM>(payload.get()))) {
            payload.release();
        }
    }).detach();
}

void AddBatchTargets(BatchState* b, const std::vector<std::wstring>& paths) {
    if (!b || b->busy) return;
    size_t ignored = 0;
    bool added = false;
    for (const auto& path : paths) {
        if (path.empty()) continue;
        bool present = std::any_of(b->targets.begin(), b->targets.end(),
            [&](const std::wstring& existing) {
                return _wcsicmp(existing.c_str(), path.c_str()) == 0;
            });
        if (present) continue;
        if (b->targets.size() >= kMaxTargets) {
            ++ignored;
            continue;
        }
        b->targets.push_back(path);
        added = true;
    }
    if (added) StartBatchScan(b);
    if (ignored != 0) {
        MessageBoxW(b->hwnd, L"Batch scan is limited to 64 unique targets. "
            L"Some dropped paths were not added.", L"Genia Unlocker — Batch",
            MB_OK | MB_ICONINFORMATION);
    }
}

std::vector<std::wstring> PickBatchPaths(HWND owner, bool folders) {
    std::vector<std::wstring> paths;
    IFileOpenDialog* dialog = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                   IID_PPV_ARGS(&dialog));
    if (FAILED(hr) || !dialog) return paths;

    FILEOPENDIALOGOPTIONS options{};
    dialog->GetOptions(&options);
    options |= FOS_FORCEFILESYSTEM | FOS_PATHMUSTEXIST | FOS_ALLOWMULTISELECT;
    if (folders) {
        options |= FOS_PICKFOLDERS;
        dialog->SetTitle(L"Select folders to scan");
    } else {
        options |= FOS_FILEMUSTEXIST;
        dialog->SetTitle(L"Select files to scan");
    }
    dialog->SetOptions(options);
    if (SUCCEEDED(dialog->Show(owner))) {
        IShellItemArray* results = nullptr;
        if (SUCCEEDED(dialog->GetResults(&results)) && results) {
            DWORD count = 0;
            results->GetCount(&count);
            for (DWORD i = 0; i < count && i < static_cast<DWORD>(kMaxTargets); ++i) {
                IShellItem* item = nullptr;
                if (SUCCEEDED(results->GetItemAt(i, &item)) && item) {
                    PWSTR path = nullptr;
                    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                        paths.emplace_back(path);
                        CoTaskMemFree(path);
                    }
                    item->Release();
                }
            }
            results->Release();
        }
    }
    dialog->Release();
    return paths;
}

std::vector<std::wstring> QueryDroppedFiles(HDROP drop) {
    std::vector<std::wstring> paths;
    const UINT count = DragQueryFileW(drop, 0xFFFFFFFF, nullptr, 0);
    for (UINT i = 0; i < count && i < static_cast<UINT>(kMaxTargets); ++i) {
        const UINT length = DragQueryFileW(drop, i, nullptr, 0);
        if (length == 0) continue;
        std::wstring path(length + 1, L'\0');
        if (DragQueryFileW(drop, i, path.data(), length + 1) == length) {
            path.resize(length);
            paths.push_back(std::move(path));
        }
    }
    return paths;
}

void OpenSelectedTarget(BatchState* b) {
    int row = ListView_GetNextItem(b->list, -1, LVNI_SELECTED);
    if (row < 0 || static_cast<size_t>(row) >= b->targets.size() ||
        !IsWindow(b->owner)) return;
    std::wstring target = b->targets[static_cast<size_t>(row)];
    target.push_back(L'\0');
    COPYDATASTRUCT message{};
    message.dwData = 1;
    message.cbData = static_cast<DWORD>(target.size() * sizeof(wchar_t));
    message.lpData = target.data();
    SendMessageW(b->owner, WM_COPYDATA,
                 reinterpret_cast<WPARAM>(b->hwnd),
                 reinterpret_cast<LPARAM>(&message));
    ShowWindow(b->hwnd, SW_HIDE);
}

LRESULT CALLBACK BatchWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    auto* b = reinterpret_cast<BatchState*>(GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    if (msg == WM_NCCREATE) {
        const auto* create = reinterpret_cast<const CREATESTRUCTW*>(lParam);
        b = reinterpret_cast<BatchState*>(create->lpCreateParams);
        b->hwnd = hwnd;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(b));
    }
    switch (msg) {
    case WM_CREATE: {
        b->dpi = static_cast<int>(GetDpiForWindow(hwnd));
        if (b->dpi <= 0) b->dpi = 96;
        CreateWindowW(L"STATIC", L"Batch scan · Read-only", WS_CHILD | WS_VISIBLE,
            0, 0, 0, 0, hwnd, BatchId(IDC_BATCH_TITLE), nullptr, nullptr);
        b->list = CreateWindowExW(0, WC_LISTVIEWW, L"",
            WS_CHILD | WS_VISIBLE | WS_TABSTOP | WS_BORDER |
            LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
            0, 0, 0, 0, hwnd, BatchId(IDC_BATCH_LIST), nullptr, nullptr);
        ListView_SetExtendedListViewStyle(b->list,
            LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
        const wchar_t* headers[] = {L"Target", L"Status", L"Locking processes", L"Time"};
        const int widths[] = {350, 125, 280, 90};
        for (int i = 0; i < 4; ++i) {
            LVCOLUMNW column{};
            column.mask = LVCF_TEXT | LVCF_WIDTH | LVCF_SUBITEM;
            column.pszText = const_cast<LPWSTR>(headers[i]);
            column.cx = S(b, widths[i]);
            column.iSubItem = i;
            ListView_InsertColumn(b->list, i, &column);
        }
        const std::pair<int, const wchar_t*> buttons[] = {
            {IDC_BATCH_ADD_FILES, L"Add files..."},
            {IDC_BATCH_ADD_FOLDERS, L"Add folders..."},
            {IDC_BATCH_SCAN, L"Scan all"},
            {IDC_BATCH_OPEN, L"Open selected"},
            {IDC_BATCH_CLOSE, L"Close"}
        };
        for (const auto& button : buttons) {
            CreateWindowW(L"BUTTON", button.second,
                WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_OWNERDRAW,
                0, 0, 0, 0, hwnd, BatchId(button.first), nullptr, nullptr);
        }
        b->status = CreateWindowW(L"STATIC",
            L"Add files or folders, or drag and drop targets here.",
            WS_CHILD | WS_VISIBLE | SS_ENDELLIPSIS,
            0, 0, 0, 0, hwnd, BatchId(IDC_BATCH_STATUS), nullptr, nullptr);
        ApplyBatchFonts(b);
        ApplyBatchTheme(b);
        RECT rc{};
        GetClientRect(hwnd, &rc);
        LayoutBatch(b, rc.right, rc.bottom);
        DragAcceptFiles(hwnd, TRUE);
        RefreshBatchRows(b);
        return 0;
    }

    case WM_SIZE:
        if (b) LayoutBatch(b, LOWORD(lParam), HIWORD(lParam));
        return 0;

    case WM_GETMINMAXINFO:
        if (b) {
            auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
            info->ptMinTrackSize.x = S(b, 690);
            info->ptMinTrackSize.y = S(b, 340);
        }
        return 0;

    case WM_ERASEBKGND:
        if (b && b->background) {
            RECT rc{};
            GetClientRect(hwnd, &rc);
            FillRect(reinterpret_cast<HDC>(wParam), &rc, b->background);
            return 1;
        }
        break;

    case WM_CTLCOLORSTATIC:
        if (b) {
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, b->palette.text);
            return reinterpret_cast<LRESULT>(b->background);
        }
        break;

    case WM_DRAWITEM:
        if (b) {
            const auto* draw = reinterpret_cast<const DRAWITEMSTRUCT*>(lParam);
            if (draw && draw->CtlType == ODT_BUTTON) {
                const auto kind = draw->CtlID == IDC_BATCH_OPEN
                    ? ModernTheme::ButtonKind::Primary
                    : ModernTheme::ButtonKind::Secondary;
                ModernTheme::DrawButton(*draw, b->palette, kind);
                return TRUE;
            }
        }
        break;

    case WM_DPICHANGED:
        if (b) {
            const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
            b->dpi = HIWORD(wParam) > 0 ? HIWORD(wParam) : 96;
            ApplyBatchFonts(b);
            if (suggested) {
                SetWindowPos(hwnd, nullptr, suggested->left, suggested->top,
                    suggested->right - suggested->left, suggested->bottom - suggested->top,
                    SWP_NOZORDER | SWP_NOACTIVATE);
            }
            RECT rc{};
            GetClientRect(hwnd, &rc);
            LayoutBatch(b, rc.right, rc.bottom);
            ApplyBatchTheme(b);
        }
        return 0;

    case WM_THEMECHANGED:
    case WM_SETTINGCHANGE:
        if (b) ApplyBatchTheme(b);
        return 0;

    case WM_COMMAND:
        if (!b) return 0;
        switch (LOWORD(wParam)) {
        case IDC_BATCH_ADD_FILES:
        case IDC_BATCH_ADD_FOLDERS:
            if (!b->busy) AddBatchTargets(b,
                PickBatchPaths(hwnd, LOWORD(wParam) == IDC_BATCH_ADD_FOLDERS));
            return 0;
        case IDC_BATCH_SCAN:
            StartBatchScan(b);
            return 0;
        case IDC_BATCH_OPEN:
            if (!b->busy) OpenSelectedTarget(b);
            return 0;
        case IDC_BATCH_CLOSE:
            DestroyWindow(hwnd);
            return 0;
        }
        break;

    case WM_NOTIFY:
        if (b) {
            const auto* hdr = reinterpret_cast<const NMHDR*>(lParam);
            if (hdr && hdr->hwndFrom == b->list) {
                if (hdr->code == LVN_ITEMCHANGED) {
                    EnableWindow(GetDlgItem(hwnd, IDC_BATCH_OPEN),
                        !b->busy &&
                        ListView_GetNextItem(b->list, -1, LVNI_SELECTED) >= 0);
                } else if (hdr->code == NM_DBLCLK && !b->busy) {
                    OpenSelectedTarget(b);
                }
            }
        }
        break;

    case WM_DROPFILES: {
        HDROP drop = reinterpret_cast<HDROP>(wParam);
        auto paths = QueryDroppedFiles(drop);
        DragFinish(drop);
        if (b) {
            if (b->busy) {
                SetWindowTextW(b->status, L"Scan in progress; add more targets after it finishes.");
            } else {
                AddBatchTargets(b, paths);
            }
        }
        return 0;
    }

    case WM_BATCH_DONE: {
        std::unique_ptr<BatchPayload> payload(reinterpret_cast<BatchPayload*>(lParam));
        if (!b || !payload || payload->generation != b->generation) return 0;
        b->results = std::move(payload->entries);
        SetBatchBusy(b, false);
        RefreshBatchRows(b);
        size_t locked = 0, clean = 0, missing = 0, review = 0;
        for (const auto& entry : b->results) {
            const auto status = StatusFor(entry.scan);
            if (status == L"Clean") ++clean;
            else if (status == L"Missing") ++missing;
            else if (status == L"Review") ++review;
            else ++locked;
        }
        std::wstring summary = std::to_wstring(b->results.size()) +
            L" scanned · " + std::to_wstring(clean) + L" clean · " +
            std::to_wstring(locked) + L" locked · " +
            std::to_wstring(missing) + L" missing · " +
            std::to_wstring(review) + L" review";
        SetWindowTextW(b->status, summary.c_str());
        return 0;
    }

    case WM_CLOSE:
        DestroyWindow(hwnd);
        return 0;

    case WM_DESTROY:
        if (b) {
            if (b->cancelled) b->cancelled->store(true);
            b->generation = 0;
            DragAcceptFiles(hwnd, FALSE);
            if (b->font) DeleteObject(b->font);
            if (b->titleFont) DeleteObject(b->titleFont);
            if (b->background) DeleteObject(b->background);
            if (b->surface) DeleteObject(b->surface);
            if (gBatchWindow == hwnd) gBatchWindow = nullptr;
            SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
            delete b;
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
}

} // namespace

void ShowBatchScanWindow(HWND owner, const std::vector<std::wstring>& paths) {
    if (!owner || !IsWindow(owner)) return;
    if (gBatchWindow && IsWindow(gBatchWindow)) {
        auto* b = reinterpret_cast<BatchState*>(
            GetWindowLongPtrW(gBatchWindow, GWLP_USERDATA));
        if (b && !paths.empty()) AddBatchTargets(b, paths);
        ShowWindow(gBatchWindow, SW_RESTORE);
        SetForegroundWindow(gBatchWindow);
        return;
    }
    HINSTANCE instance = reinterpret_cast<HINSTANCE>(
        GetWindowLongPtrW(owner, GWLP_HINSTANCE));
    WNDCLASSEXW wc{};
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = BatchWindowProc;
    wc.hInstance = instance;
    wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    wc.lpszClassName = kBatchClass;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return;

    const int dpi = static_cast<int>(GetDpiForWindow(owner));
    const int width = MulDiv(930, dpi > 0 ? dpi : 96, 96);
    const int height = MulDiv(470, dpi > 0 ? dpi : 96, 96);
    RECT parent{};
    GetWindowRect(owner, &parent);
    auto* b = new BatchState{};
    b->owner = owner;
    HWND hwnd = CreateWindowExW(WS_EX_TOOLWINDOW, kBatchClass,
        L"Genia Unlocker — Batch scan",
        WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN,
        parent.left + (parent.right - parent.left - width) / 2,
        parent.top + (parent.bottom - parent.top - height) / 2,
        width, height, owner, nullptr, instance, b);
    if (!hwnd) {
        delete b;
        return;
    }
    gBatchWindow = hwnd;
    ShowWindow(hwnd, SW_SHOWNORMAL);
    UpdateWindow(hwnd);
    if (!paths.empty()) AddBatchTargets(b, paths);
}

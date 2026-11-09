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
constexpr UINT WM_BATCH_PROGRESS = WM_APP + 73;
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
    IDC_BATCH_DETAILS = 7109,
};

struct BatchEntry {
    ScanResult scan;
    ULONGLONG elapsedMs{};
    bool completed{};
};

struct BatchPayload {
    unsigned long long generation{};
    std::vector<BatchEntry> entries;
};

struct BatchProgressPayload {
    unsigned long long generation{};
    size_t index{};
    BatchEntry entry;
};

struct BatchState {
    HWND hwnd{};
    HWND owner{};
    HWND list{};
    HWND status{};
    HWND details{};
    HFONT font{};
    HFONT titleFont{};
    HBRUSH background{};
    HBRUSH surface{};
    ModernTheme::Palette palette{};
    int dpi{96};
    std::vector<std::wstring> targets;
    std::vector<BatchEntry> results;
    size_t completedCount{};
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


// Draw the header with the same Flat palette as the stable single-target UI.
// SetWindowTheme alone does not prevent a white native header in dark mode.
LRESULT CALLBACK BatchHeaderSubclassProc(HWND hwnd, UINT message, WPARAM wParam,
                                         LPARAM lParam, UINT_PTR id, DWORD_PTR context) {
    auto* b = reinterpret_cast<BatchState*>(context);
    switch (message) {
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT:
        if (b) {
            PAINTSTRUCT ps{};
            HDC dc = BeginPaint(hwnd, &ps);
            RECT client{};
            GetClientRect(hwnd, &client);
            const COLORREF fill = b->palette.dark ? b->palette.surface
                                                   : b->palette.window;
            HBRUSH bg = CreateSolidBrush(fill);
            FillRect(dc, &client, bg);
            DeleteObject(bg);

            HFONT font = b->font ? b->font :
                reinterpret_cast<HFONT>(SendMessageW(hwnd, WM_GETFONT, 0, 0));
            HGDIOBJ oldFont = font ? SelectObject(dc, font) : nullptr;
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, b->palette.text);
            HPEN separator = CreatePen(PS_SOLID, 1, b->palette.border);
            HGDIOBJ oldPen = SelectObject(dc, separator);

            for (int i = 0; i < Header_GetItemCount(hwnd); ++i) {
                RECT rc{};
                if (!Header_GetItemRect(hwnd, i, &rc)) continue;
                wchar_t name[256]{};
                HDITEMW item{};
                item.mask = HDI_TEXT | HDI_FORMAT;
                item.pszText = name;
                item.cchTextMax = static_cast<int>(_countof(name));
                Header_GetItem(hwnd, i, &item);
                MoveToEx(dc, rc.right - 1, rc.top, nullptr);
                LineTo(dc, rc.right - 1, rc.bottom);
                MoveToEx(dc, rc.left, rc.bottom - 1, nullptr);
                LineTo(dc, rc.right, rc.bottom - 1);
                RECT text = rc;
                text.left += S(b, 7);
                text.right -= S(b, 6);
                UINT format = DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS;
                format |= (item.fmt & HDF_RIGHT) ? DT_RIGHT :
                          (item.fmt & HDF_CENTER) ? DT_CENTER : DT_LEFT;
                DrawTextW(dc, name, -1, &text, format);
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
        RemoveWindowSubclass(hwnd, BatchHeaderSubclassProc, id);
        break;
    }
    return DefSubclassProc(hwnd, message, wParam, lParam);
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
                   IDC_BATCH_CLOSE, IDC_BATCH_DETAILS}) {
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
    ModernTheme::ApplyControlTheme(b->details, b->palette.dark);
    ModernTheme::ApplyControlTheme(ListView_GetHeader(b->list), b->palette.dark);
    ListView_SetBkColor(b->list, b->palette.surface);
    ListView_SetTextBkColor(b->list, b->palette.surface);
    ListView_SetTextColor(b->list, b->palette.text);
    if (HWND header = ListView_GetHeader(b->list)) {
        InvalidateRect(header, nullptr, TRUE);
    }
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
    const int listTop = S(b, 48);
    const int detailH = S(b, 64);
    const int footerH = S(b, 6 + 64 + 8 + 29 + 5 + 18 + 12);
    const int listH = (std::max)(S(b, 120), h - listTop - footerH);
    MoveWindow(b->list, m, listTop, (std::max)(1, w - 2 * m), listH, TRUE);
    const int detailY = listTop + listH + S(b, 6);
    MoveWindow(b->details, m, detailY, (std::max)(1, w - 2 * m), detailH, TRUE);
    const int buttonY = detailY + detailH + gap;
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

std::wstring BuildBatchDetails(const BatchState* b, size_t index) {
    if (index >= b->targets.size()) return L"Select a target to view its details.";
    std::wstring text = L"Target: " + b->targets[index];
    if (index >= b->results.size() || !b->results[index].completed) {
        if (b->busy)
            text += (index == b->completedCount) ? L"\r\nScanning..."
                                                  : L"\r\nQueued for scanning";
        else
            text += L"\r\nNot scanned";
        return text;
    }

    const BatchEntry& entry = b->results[index];
    const ScanResult& scan = entry.scan;
    text += L"\r\nStatus: " + StatusFor(scan);
    text += L"  |  Scan: " + std::to_wstring(entry.elapsedMs) + L" ms";
    if (!scan.targetExists) return text;
    text += L"\r\nDelete-share: ";
    text += scan.deleteShareProbeSucceeded ? L"available" :
        L"not verified (Win32 " + std::to_wstring(scan.deleteShareProbeError) + L")";
    if (scan.processes.empty()) {
        text += L"\r\n" + BlockersFor(scan);
    } else {
        text += L"\r\nKnown blocking processes:";
        for (const auto& process : scan.processes) {
            text += L"\r\n  " + process.name +
                    L" (PID " + std::to_wstring(process.pid) + L")";
            for (const auto& locked : process.lockedObjects) {
                text += L"\r\n    " + locked;
            }
        }
    }
    if (scan.inaccessibleProcessCount)
        text += L"\r\nUninspected file-handle owners: " +
                std::to_wstring(scan.inaccessibleProcessCount);
    return text;
}

void RefreshBatchSelection(BatchState* b) {
    if (!b || !b->details) return;
    const int row = ListView_GetNextItem(b->list, -1, LVNI_SELECTED);
    std::wstring detail = row >= 0 ?
        BuildBatchDetails(b, static_cast<size_t>(row)) :
        L"Select a target to view its full path and scan details.";
    SetWindowTextW(b->details, detail.c_str());
    EnableWindow(GetDlgItem(b->hwnd, IDC_BATCH_OPEN), !b->busy && row >= 0);
}

void RefreshBatchRow(BatchState* b, size_t i) {
    if (!b || i >= b->targets.size()) return;
    std::wstring status = b->busy ?
        (i == b->completedCount ? L"Scanning..." : L"Queued") :
        L"Not scanned";
    std::wstring blockers = L"—";
    std::wstring duration;
    if (i < b->results.size() && b->results[i].completed) {
        status = StatusFor(b->results[i].scan);
        blockers = BlockersFor(b->results[i].scan);
        duration = std::to_wstring(b->results[i].elapsedMs) + L" ms";
    }
    ListView_SetItemText(b->list, static_cast<int>(i), 1, status.data());
    ListView_SetItemText(b->list, static_cast<int>(i), 2, blockers.data());
    ListView_SetItemText(b->list, static_cast<int>(i), 3, duration.data());
}

void RefreshBatchRows(BatchState* b) {
    if (!b || !b->list) return;
    const int oldSelection = ListView_GetNextItem(b->list, -1, LVNI_SELECTED);
    ListView_DeleteAllItems(b->list);
    for (size_t i = 0; i < b->targets.size(); ++i) {
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = static_cast<int>(i);
        item.pszText = const_cast<LPWSTR>(b->targets[i].c_str());
        ListView_InsertItem(b->list, &item);
        RefreshBatchRow(b, i);
    }
    if (!b->targets.empty()) {
        const int selected = oldSelection >= 0 &&
            static_cast<size_t>(oldSelection) < b->targets.size() ?
            oldSelection : 0;
        ListView_SetItemState(b->list, selected,
                              LVIS_SELECTED, LVIS_SELECTED);
    }
    RefreshBatchSelection(b);
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
    b->results.resize(targets.size());
    b->completedCount = 0;
    SetBatchBusy(b, true);
    RefreshBatchRows(b);
    const std::wstring status = L"Scanning 0 / " +
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
            entry.completed = true;
            if (ticket->load()) return;
            auto progress = std::make_unique<BatchProgressPayload>();
            progress->generation = generation;
            progress->index = payload->entries.size();
            progress->entry = entry;
            if (PostMessageW(hwnd, WM_BATCH_PROGRESS, 0,
                             reinterpret_cast<LPARAM>(progress.get()))) {
                progress.release();
            }
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
            LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER |
            LVS_EX_LABELTIP | LVS_EX_INFOTIP);
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
        if (HWND header = ListView_GetHeader(b->list)) {
            SetWindowSubclass(header, BatchHeaderSubclassProc, 1,
                              reinterpret_cast<DWORD_PTR>(b));
        }
        if (HWND tooltip = ListView_GetToolTips(b->list)) {
            SendMessageW(tooltip, TTM_SETMAXTIPWIDTH, 0, S(b, 720));
        }
        b->details = CreateWindowExW(0, L"EDIT",
            L"Select a target to view its full path and scan details.",
            WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL |
            ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL,
            0, 0, 0, 0, hwnd, BatchId(IDC_BATCH_DETAILS), nullptr, nullptr);
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
            if (reinterpret_cast<HWND>(lParam) == b->details) {
                SetTextColor(dc, b->palette.text);
                SetBkColor(dc, b->palette.surface);
                return reinterpret_cast<LRESULT>(b->surface);
            }
            SetBkMode(dc, TRANSPARENT);
            SetTextColor(dc, b->palette.text);
            return reinterpret_cast<LRESULT>(b->background);
        }
        break;

    case WM_CTLCOLOREDIT:
        if (b) {
            HDC dc = reinterpret_cast<HDC>(wParam);
            SetTextColor(dc, b->palette.text);
            SetBkColor(dc, b->palette.surface);
            return reinterpret_cast<LRESULT>(b->surface);
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
                    RefreshBatchSelection(b);
                } else if (hdr->code == LVN_GETINFOTIPW) {
                    auto* info = reinterpret_cast<NMLVGETINFOTIPW*>(lParam);
                    if (info && info->iItem >= 0 && info->pszText &&
                        info->cchTextMax > 0) {
                        const std::wstring tooltip =
                            BuildBatchDetails(b, static_cast<size_t>(info->iItem));
                        wcsncpy_s(info->pszText,
                                  static_cast<size_t>(info->cchTextMax),
                                  tooltip.c_str(), _TRUNCATE);
                    }
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

    case WM_BATCH_PROGRESS: {
        std::unique_ptr<BatchProgressPayload> progress(
            reinterpret_cast<BatchProgressPayload*>(lParam));
        if (!b || !progress || progress->generation != b->generation ||
            progress->index >= b->targets.size()) return 0;
        b->results[progress->index] = std::move(progress->entry);
        b->completedCount = progress->index + 1;
        RefreshBatchRow(b, progress->index);
        if (b->completedCount < b->targets.size()) {
            RefreshBatchRow(b, b->completedCount);
        }
        RefreshBatchSelection(b);
        const std::wstring status = L"Scanning " +
            std::to_wstring(b->completedCount) + L" / " +
            std::to_wstring(b->targets.size()) + L" targets (read-only)...";
        SetWindowTextW(b->status, status.c_str());
        return 0;
    }

    case WM_BATCH_DONE: {
        std::unique_ptr<BatchPayload> payload(reinterpret_cast<BatchPayload*>(lParam));
        if (!b || !payload || payload->generation != b->generation) return 0;
        b->results = std::move(payload->entries);
        b->completedCount = b->results.size();
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
            if (HWND header = ListView_GetHeader(b->list)) {
                RemoveWindowSubclass(header, BatchHeaderSubclassProc, 1);
            }
            // Release completed worker messages that were posted before close.
            MSG pending{};
            while (PeekMessageW(&pending, hwnd, WM_BATCH_DONE,
                                WM_BATCH_PROGRESS, PM_REMOVE)) {
                if (pending.message == WM_BATCH_DONE)
                    delete reinterpret_cast<BatchPayload*>(pending.lParam);
                else if (pending.message == WM_BATCH_PROGRESS)
                    delete reinterpret_cast<BatchProgressPayload*>(pending.lParam);
            }
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

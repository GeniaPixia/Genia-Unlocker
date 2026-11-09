#pragma once

#include <windows.h>
#include <string>
#include <vector>

// Non-destructive multi-target inspector. It deliberately never invokes
// Unlock, Force Unlock, Terminate or Delete.
void ShowBatchScanWindow(HWND owner, const std::vector<std::wstring>& paths = {});

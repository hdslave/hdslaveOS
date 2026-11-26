// SPDX-License-Identifier: AGPL-3.0-only
#pragma once
#include "melodee/shell/sample_library.h"
#include <string>
#include <vector>

// The Samples browser's listing as the page draws it and its keys act on it (Push, Erase, Shift + pad, B1, Shift + B3):
// the library's, which its service thread lists, taken in by Context::browser_tick() only, with the cursor moved in
// the same step (shell/sample_files.cpp). So a press never lands on a row of a listing the screen has not shown yet,
// at a cursor meant for the listing before (B1 then Push: the folder above's row).
namespace melodee {

struct FilesView {
    std::vector<SampleFile> files;
    std::string root, folder;
    bool top = true;
    int came_from = -1;
    Drive drive = Drive::Card;
};

} // namespace melodee

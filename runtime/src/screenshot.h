// SPDX-License-Identifier: GPL-3.0-or-later
// A screenshot asked for by a key (F12) or a pad's Share button: the platform sets the flag, the next present reads
// the frame back and writes it as a PNG to <data root>/screenshots (nvn_debug.cpp, frame_dump_due/frame_dump_write).
#pragma once

#include <atomic>

namespace nx {

inline std::atomic<bool> g_screenshot_request{false};

}  // namespace nx

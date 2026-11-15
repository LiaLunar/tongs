#pragma once

#include "protocol.hpp"

#include <filesystem>
#include <string_view>

namespace tongs {

// Captures the client area of the game window into a PNG file in `folder`
// (created when missing), named <name>.png or shot-<time>.png, and keeps at
// most kMaxScreenshots files there by deleting the oldest. Under Wine it
// copies the game's back buffer at its next Present first (game_present.hpp):
// GDI sees no frame there. Otherwise, and when that fails, PrintWindow with
// PW_RENDERFULLCONTENT (works while the window is covered), then a copy from
// the screen, then on Windows the frame at Present when both were all black.
// Any thread but the main thread may call it.
ScreenshotResult TakeScreenshot(const std::filesystem::path& folder, std::string_view name);

inline constexpr int kMaxScreenshots = 100;

}  // namespace tongs

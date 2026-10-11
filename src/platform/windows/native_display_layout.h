/** @file src/platform/windows/native_display_layout.h
 * @brief Typed CCD snapshots and deterministic virtual display construction.
 */
#pragma once
#include <algorithm>
#include <cwctype>
#include <stdexcept>
#include <string>
#include <vector>
#include <windows.h>

namespace native_display::win {
  /** @brief Match the actual CCD monitor-device-path format, not a PnP instance-path prefix. */
  inline bool is_mtt_target(std::wstring name) {
    std::transform(name.begin(), name.end(), name.begin(), towupper);
    return name.find(L"DISPLAY#MTT1337#") != std::wstring::npos;
  }

  /** @brief A bounded, typed Windows display snapshot. */
  struct layout {
    std::vector<DISPLAYCONFIG_PATH_INFO> paths;  ///< Active or available paths.
    std::vector<DISPLAYCONFIG_MODE_INFO> modes;  ///< Associated CCD modes.
  };

  /** @brief Validate dimensions without silently downgrading client requests. */
  inline bool valid_mode(int width, int height, int fps) {
    return width >= 320 && width <= 7680 && height >= 320 && height <= 7680 && fps >= 1 && fps <= 240;
  }

  /** @brief Construct one active virtual source without enabling other available displays.
   * @param path Verified virtual target. @param width Width. @param height Height. @param fps Refresh rate.
   * @return A standalone topology with the sole display at the origin.
   */
  inline layout virtual_layout(DISPLAYCONFIG_PATH_INFO path, int width, int height, int fps) {
    if (!valid_mode(width, height, fps)) {
      throw std::runtime_error("Unsupported native display mode (320..7680 pixels, 1..240 Hz)");
    }
    DISPLAYCONFIG_MODE_INFO mode {};
    mode.infoType = DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE;
    mode.id = path.sourceInfo.id;
    mode.adapterId = path.sourceInfo.adapterId;
    mode.sourceMode.width = width;
    mode.sourceMode.height = height;
    mode.sourceMode.pixelFormat = DISPLAYCONFIG_PIXELFORMAT_32BPP;
    path.flags = DISPLAYCONFIG_PATH_ACTIVE;
    path.sourceInfo.modeInfoIdx = 0;
    path.targetInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
    path.targetInfo.rotation = DISPLAYCONFIG_ROTATION_IDENTITY;
    path.targetInfo.scaling = DISPLAYCONFIG_SCALING_IDENTITY;
    path.targetInfo.refreshRate = {static_cast<UINT32>(fps), 1};
    path.targetInfo.scanLineOrdering = DISPLAYCONFIG_SCANLINE_ORDERING_UNSPECIFIED;
    return {{path}, {mode}};
  }

  /** @brief Check source mode indices before reading journal or API data. */
  inline bool valid(const layout &value) {
    if (value.paths.empty() || value.paths.size() > 64 || value.modes.size() > 256) {
      return false;
    }
    for (const auto &path : value.paths) {
      const auto index = path.sourceInfo.modeInfoIdx;
      if (index >= value.modes.size() || value.modes[index].infoType != DISPLAYCONFIG_MODE_INFO_TYPE_SOURCE) {
        return false;
      }
    }
    return true;
  }
}  // namespace native_display::win

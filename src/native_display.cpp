/** @file src/native_display.cpp
 * @brief Native Windows privacy transaction, recovery journal and session lifecycle.
 */
#include "native_display.h"

#include <stdexcept>

#ifdef _WIN32
  #include "platform/windows/native_display_layout.h"

  #include <cfgmgr32.h>
  #include <chrono>
  #include <condition_variable>
  #include <cwctype>
  #include <devguid.h>
  #include <fstream>
  #include <future>
  #include <mutex>
  #include <queue>
  #include <setupapi.h>
  #include <thread>
  #include <wtsapi32.h>
#endif

namespace native_display {
  policy parse(const std::string &text) {
    if (text.empty() || text == "disabled") {
      return policy::disabled;
    }
    if (text == "physical") {
      return policy::physical;
    }
    if (text == "privacy") {
      return policy::privacy;
    }
    throw std::invalid_argument("Unknown native-display policy: " + text);
  }
#ifdef _WIN32
  namespace {
    using namespace std::chrono_literals;
    using win::layout;

    /** @brief Throw an actionable Win32 failure. */
    void check(LONG code, const char *operation) {
      if (code != ERROR_SUCCESS) {
        throw std::runtime_error(std::string(operation) + ": Windows error " + std::to_string(code));
      }
    }

    /** @brief Read CCD, bounded against hotplug races. */
    layout read(UINT32 flags = QDC_ONLY_ACTIVE_PATHS) {
      for (int attempt = 0; attempt < 5; ++attempt) {
        UINT32 pc {}, mc {};
        check(GetDisplayConfigBufferSizes(flags, &pc, &mc), "Display enumeration");
        if (pc > 64 || mc > 256) {
          throw std::runtime_error("Unexpected display topology size");
        }
        layout value {std::vector<DISPLAYCONFIG_PATH_INFO>(pc), std::vector<DISPLAYCONFIG_MODE_INFO>(mc)};
        auto result = QueryDisplayConfig(flags, &pc, value.paths.data(), &mc, value.modes.data(), nullptr);
        if (result == ERROR_INSUFFICIENT_BUFFER) {
          continue;
        }
        check(result, "Display query (unlock the console desktop and retry)");
        value.paths.resize(pc);
        value.modes.resize(mc);
        return value;
      }
      throw std::runtime_error("Display topology is still changing; retry connection");
    }

    /** @brief Read stable target identity. */
    std::wstring identity(const DISPLAYCONFIG_PATH_INFO &path) {
      DISPLAYCONFIG_TARGET_DEVICE_NAME name {};
      name.header = {DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME, sizeof(name), path.targetInfo.adapterId, path.targetInfo.id};
      check(DisplayConfigGetDeviceInfo(&name.header), "Display target identity");
      return name.monitorDevicePath;
    }

    /** @brief Only the explicitly supported MTT monitor may become the privacy target. */
    bool is_virtual(const DISPLAYCONFIG_PATH_INFO &path) {
      auto name = identity(path);
      std::transform(name.begin(), name.end(), name.begin(), towupper);
      return name.find(L"MONITOR#MTT1337#") != std::wstring::npos;
    }

    /** @brief Apply a validated topology and save its mode for the next activation. */
    void apply(layout value) {
      if (!win::valid(value)) {
        throw std::runtime_error("Invalid display snapshot");
      }
      constexpr UINT32 common = SDC_USE_SUPPLIED_DISPLAY_CONFIG | SDC_ALLOW_CHANGES;
      check(SetDisplayConfig(static_cast<UINT32>(value.paths.size()), value.paths.data(), static_cast<UINT32>(value.modes.size()), value.modes.data(), common | SDC_VALIDATE), "Display validation");
      check(SetDisplayConfig(static_cast<UINT32>(value.paths.size()), value.paths.data(), static_cast<UINT32>(value.modes.size()), value.modes.data(), common | SDC_APPLY | SDC_SAVE_TO_DATABASE), "Display application");
    }

    /** @brief Test target, dimensions and refresh before doing a modeset. */
    bool matches(const layout &value, const std::wstring &target, int width, int height, int fps) {
      if (value.paths.size() != 1 || !win::valid(value) || identity(value.paths.front()) != target) {
        return false;
      }
      const auto &path = value.paths.front();
      const auto &mode = value.modes[path.sourceInfo.modeInfoIdx].sourceMode;
      const auto &rate = path.targetInfo.refreshRate;
      return mode.width == static_cast<UINT32>(width) && mode.height == static_cast<UINT32>(height) && mode.position.x == 0 && mode.position.y == 0 && rate.Denominator && rate.Numerator == static_cast<UINT32>(fps) * rate.Denominator;
    }

    /** @brief Enable/disable only a single verified Root\\MttVDD adapter using Config Manager. */
    void device(bool enable) {
      HDEVINFO set = SetupDiGetClassDevsW(&GUID_DEVCLASS_DISPLAY, nullptr, nullptr, DIGCF_PRESENT);
      if (set == INVALID_HANDLE_VALUE) {
        throw std::runtime_error("Cannot enumerate display adapters");
      }
      std::vector<DEVINST> candidates;
      SP_DEVINFO_DATA info {};
      info.cbSize = sizeof(info);
      for (DWORD index = 0; SetupDiEnumDeviceInfo(set, index, &info); ++index) {
        wchar_t hardware[4096] {};
        DWORD type {}, size {};
        if (!SetupDiGetDeviceRegistryPropertyW(set, &info, SPDRP_HARDWAREID, &type, reinterpret_cast<BYTE *>(hardware), sizeof(hardware) - sizeof(wchar_t), &size) || type != REG_MULTI_SZ) {
          continue;
        }
        for (const wchar_t *id = hardware; *id; id += wcslen(id) + 1) {
          if (_wcsicmp(id, L"Root\\MttVDD") == 0) {
            candidates.push_back(info.DevInst);
            break;
          }
        }
      }
      SetupDiDestroyDeviceInfoList(set);
      if (candidates.size() != 1) {
        throw std::runtime_error("Exactly one installed Root\\MttVDD adapter is required");
      }
      ULONG status {}, problem {};
      if (CM_Get_DevNode_Status(&status, &problem, candidates.front(), 0) != CR_SUCCESS) {
        throw std::runtime_error("Cannot read virtual adapter state");
      }
      if (enable && problem == 0) {
        return;
      }
      if (!enable && problem == CM_PROB_DISABLED) {
        return;
      }
      auto result = enable ? CM_Enable_DevNode(candidates.front(), 0) : CM_Disable_DevNode(candidates.front(), CM_DISABLE_PERSIST);
      if (result != CR_SUCCESS) {
        throw std::runtime_error("Virtual adapter operation failed: Config Manager " + std::to_string(result) + "; administrative rights may be required");
      }
    }

    /** @brief Serialize all Windows operations on the thread that owns the legacy guard mutex. */
    class controller {
    public:
      std::filesystem::path journal;  ///< Persistent rollback snapshot.
      std::function<void(const std::string &)> log;  ///< Diagnostic callback.
      std::mutex mutex;  ///< Protects command queue.
      std::condition_variable cv;  ///< Wakes command processing.
      std::queue<std::function<void()>> jobs;  ///< Synchronous commands.
      bool quit {};  ///< Worker exit requested.
      std::thread worker;  ///< Owns all display state below.
      HANDLE gate {};  ///< Shared mutex prevents the retained script keeper racing this controller.
      bool owns_gate {};  ///< Worker has acquired gate.
      bool armed {};  ///< Journal must be restored even if preparation failed.
      bool live {};  ///< At least one stream has started.
      policy mode {policy::disabled};  ///< Current policy.
      layout baseline;  ///< Exact pre-session physical topology, including primary and positions.
      std::wstring target;  ///< Verified private target.
      std::string selector;  ///< GDI capture selector for the prepared target.
      std::chrono::steady_clock::time_point deadline;  ///< Expiry for an abandoned handshake.
      int width {}, height {}, fps {};  ///< Verified private mode, reapplied after physical hotplug.
      HHOOK keyboard {};  ///< Physical keyboard filter; injected Moonlight input is allowed.
      int escape_taps {};  ///< Local emergency release counter.
      ULONGLONG last_escape {};  ///< Last emergency key timestamp.
      bool emergency {};  ///< Defer recovery until after the hook callback returns.
      static inline controller *hook_owner {};  ///< Worker-thread keyboard callback owner.

      /** @brief Start a worker that owns the cross-process mutex. */
      controller() {
        worker = std::thread([this] {
          loop();
        });
      }

      /** @brief Block physical keyboard input while allowing injected input and triple F12 recovery. */
      static LRESULT CALLBACK keyboard_event(int code, WPARAM message, LPARAM data) {
        auto *owner = hook_owner;
        if (code >= 0 && owner && owner->mode == policy::privacy && owner->armed) {
          const auto *key = reinterpret_cast<const KBDLLHOOKSTRUCT *>(data);
          if (!(key->flags & LLKHF_INJECTED)) {
            if (message == WM_KEYUP && key->vkCode == VK_F12) {
              auto now = GetTickCount64();
              owner->escape_taps = now - owner->last_escape > 2000 ? 1 : owner->escape_taps + 1;
              owner->last_escape = now;
              if (owner->escape_taps >= 3) {
                owner->emergency = true;
              }
            }
            return 1;
          }
        }
        return CallNextHookEx(nullptr, code, message, data);
      }

      /** @brief Join cleanly. */
      ~controller() {
        {
          std::lock_guard lock(mutex);
          quit = true;
        }
        cv.notify_one();
        worker.join();
      }

      /** @brief Dispatch a command and propagate its exception. */
      template<class F>
      auto call(F fn) {
        using result = decltype(fn());
        auto job = std::make_shared<std::packaged_task<result()>>(std::move(fn));
        auto future = job->get_future();
        {
          std::lock_guard lock(mutex);
          jobs.emplace([job] {
            (*job)();
          });
        }
        cv.notify_one();
        return future.get();
      }

      /** @brief Acquire the same gate as PrivacyGuard and DisplayKeeper. */
      void acquire() {
        if (owns_gate) {
          return;
        }
        if (!gate) {
          gate = CreateMutexW(nullptr, FALSE, L"Local\\SunshinePrivacyGuard");
        }
        if (!gate) {
          throw std::runtime_error("Cannot create privacy coordination mutex");
        }
        auto result = WaitForSingleObject(gate, 0);
        if (result != WAIT_OBJECT_0 && result != WAIT_ABANDONED) {
          throw std::runtime_error("Legacy display controller is busy; do not combine script and native privacy");
        }
        owns_gate = true;
      }

      /** @brief Write and flush rollback data atomically before the first mutation. */
      void save() {
        std::filesystem::create_directories(journal.parent_path());
        auto temp = journal;
        temp += ".tmp";
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);
        UINT32 header[] {0x4B594C31, static_cast<UINT32>(baseline.paths.size()), static_cast<UINT32>(baseline.modes.size())};
        file.write(reinterpret_cast<char *>(header), sizeof(header));
        file.write(reinterpret_cast<const char *>(baseline.paths.data()), baseline.paths.size() * sizeof(DISPLAYCONFIG_PATH_INFO));
        file.write(reinterpret_cast<const char *>(baseline.modes.data()), baseline.modes.size() * sizeof(DISPLAYCONFIG_MODE_INFO));
        file.flush();
        if (!file) {
          throw std::runtime_error("Cannot persist native display rollback journal");
        }
        file.close();
        if (!MoveFileExW(temp.c_str(), journal.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
          throw std::runtime_error("Cannot commit native display rollback journal");
        }
        armed = true;
      }

      /** @brief Load bounded journal data; invalid files never reach SetDisplayConfig. */
      void recover() {
        if (!std::filesystem::exists(journal)) {
          return;
        }
        acquire();
        std::ifstream file(journal, std::ios::binary);
        UINT32 header[3] {};
        file.read(reinterpret_cast<char *>(header), sizeof(header));
        if (!file || header[0] != 0x4B594C31 || !header[1] || header[1] > 64 || header[2] > 256) {
          throw std::runtime_error("Invalid native display recovery journal; manual recovery required");
        }
        baseline.paths.resize(header[1]);
        baseline.modes.resize(header[2]);
        file.read(reinterpret_cast<char *>(baseline.paths.data()), baseline.paths.size() * sizeof(DISPLAYCONFIG_PATH_INFO));
        file.read(reinterpret_cast<char *>(baseline.modes.data()), baseline.modes.size() * sizeof(DISPLAYCONFIG_MODE_INFO));
        if (!file || file.peek() != EOF || !win::valid(baseline)) {
          throw std::runtime_error("Truncated native display recovery journal");
        }
        armed = true;
        mode = policy::privacy;
        restore();
      }

      /** @brief Restore exact physical topology before disabling the managed adapter. */
      void restore() {
        if (keyboard) {
          UnhookWindowsHookEx(keyboard);
          keyboard = nullptr;
          hook_owner = nullptr;
        }
        if (armed) {
          // Lock before making the private desktop visible on physical monitors.
          if (!LockWorkStation()) {
            throw std::runtime_error("Cannot lock desktop; retaining private display and journal");
          }
          apply(baseline);
          auto current = read();
          if (current.paths.empty() || std::any_of(current.paths.begin(), current.paths.end(), is_virtual)) {
            throw std::runtime_error("Physical restore not verified; retaining virtual adapter");
          }
          device(false);
          std::filesystem::remove(journal);
          armed = false;
          if (log) {
            log("Native privacy: restored physical topology and disabled virtual adapter");
          }
        } else if (mode == policy::physical && live) {
          if (!LockWorkStation() && log) {
            log("Native desktop: LockWorkStation failed");
          }
        }
        mode = policy::disabled;
        live = false;
        selector.clear();
        target.clear();
        if (owns_gate) {
          ReleaseMutex(gate);
          owns_gate = false;
        }
      }

      /** @brief Prepare one application without polling Sunshine logs or UDP listeners. */
      std::string prepare_mode(policy requested, int width, int height, int fps) {
        try {
          if (requested == policy::disabled) {
            return {};
          }
          if (mode != policy::disabled || armed) {
            restore();
          } else if (std::filesystem::exists(journal)) {
            recover();
          }
          acquire();
          mode = requested;
          live = false;
          deadline = std::chrono::steady_clock::now() + 30s;
          if (requested == policy::physical) {
            return {};
          }
          if (!win::valid_mode(width, height, fps)) {
            throw std::runtime_error("Invalid requested native display resolution/refresh rate");
          }
          baseline = read();
          if (!win::valid(baseline) || std::any_of(baseline.paths.begin(), baseline.paths.end(), is_virtual)) {
            throw std::runtime_error("Start native privacy from a clean physical desktop; legacy or headless virtual topology is still active");
          }
          save();
          this->width = width;
          this->height = height;
          this->fps = fps;
          device(true);
          bool found = false;
          for (int attempt = 0; attempt < 40; ++attempt) {
            auto all = read(QDC_ALL_PATHS);
            auto it = std::find_if(all.paths.begin(), all.paths.end(), [](const auto &path) {
              return path.targetInfo.targetAvailable && is_virtual(path);
            });
            if (it != all.paths.end()) {
              target = identity(*it);
              auto current = read();
              if (!matches(current, target, width, height, fps)) {
                apply(win::virtual_layout(*it, width, height, fps));
              }
              found = true;
              break;
            }
            std::this_thread::sleep_for(100ms);
          }
          if (!found) {
            throw std::runtime_error("Virtual display did not become available within four seconds");
          }
          int stable = 0;
          for (int attempt = 0; attempt < 30 && stable < 3; ++attempt) {
            auto current = read();
            stable = matches(current, target, width, height, fps) ? stable + 1 : 0;
            if (stable == 3) {
              DISPLAYCONFIG_SOURCE_DEVICE_NAME name {};
              const auto &path = current.paths.front();
              name.header = {DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME, sizeof(name), path.sourceInfo.adapterId, path.sourceInfo.id};
              check(DisplayConfigGetDeviceInfo(&name.header), "Capture output selection");
              for (const wchar_t *c = name.viewGdiDeviceName; *c; ++c) {
                selector += static_cast<char>(*c);
              }
              hook_owner = this;
              keyboard = SetWindowsHookExW(WH_KEYBOARD_LL, keyboard_event, GetModuleHandleW(nullptr), 0);
              if (!keyboard) {
                hook_owner = nullptr;
                throw std::runtime_error("Cannot install native privacy keyboard filter");
              }
              deadline = std::chrono::steady_clock::now() + 30s;
              if (log) {
                log("Native privacy: final display verified before encoder probing");
              }
              return {};
            }
            std::this_thread::sleep_for(100ms);
          }
          throw std::runtime_error("Virtual display did not stabilize at the requested mode");
        } catch (const std::exception &error) {
          std::string result = error.what();
          try {
            restore();
          } catch (const std::exception &rollback) {
            result += "; recovery pending: " + std::string(rollback.what());
          }
          if (log) {
            log(result);
          }
          return result;
        }
      }

      /** @brief Run queued commands and bounded abandoned-handshake recovery. */
      void loop() {
        for (;;) {
          std::function<void()> job;
          {
            std::unique_lock lock(mutex);
            cv.wait_for(lock, 100ms, [this] {
              return quit || !jobs.empty();
            });
            if (quit && jobs.empty()) {
              break;
            }
            if (!jobs.empty()) {
              job = std::move(jobs.front());
              jobs.pop();
            }
          }
          if (job) {
            job();
          }
          MSG message;
          while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&message);
            DispatchMessageW(&message);
          }
          if (emergency) {
            emergency = false;
            try {
              restore();
            } catch (const std::exception &error) {
              if (log) {
                log(error.what());
              }
            }
          }
          if (live && mode == policy::privacy && armed) {
            try {
              auto current = read();
              if (!matches(current, target, width, height, fps)) {
                auto all = read(QDC_ALL_PATHS);
                auto it = std::find_if(all.paths.begin(), all.paths.end(), [this](const auto &path) {
                  return path.targetInfo.targetAvailable && identity(path) == target;
                });
                if (it == all.paths.end()) {
                  throw std::runtime_error("Privacy target disconnected");
                }
                apply(win::virtual_layout(*it, width, height, fps));
                if (log) {
                  log("Native privacy: corrected display topology after hotplug");
                }
              }
            } catch (const std::exception &error) {
              LockWorkStation();
              if (log) {
                log(error.what());
              }
              live = false;
              deadline = std::chrono::steady_clock::now() + 5s;
            }
          }
          if (!live && mode != policy::disabled && std::chrono::steady_clock::now() >= deadline) {
            try {
              restore();
            } catch (const std::exception &error) {
              if (log) {
                log(error.what());
              }
              deadline = std::chrono::steady_clock::now() + 5s;
            }
          }
        }
        try {
          restore();
        } catch (const std::exception &error) {
          if (log) {
            log(error.what());
          }
        }
        if (gate) {
          CloseHandle(gate);
        }
      }
    };

    std::unique_ptr<controller> instance;  ///< Initialized before HTTP workers and joined after them.
  }  // namespace

  void init(const std::filesystem::path &path, std::function<void(const std::string &)> log) {
    instance = std::make_unique<controller>();
    instance->call([&] {
      instance->journal = path;
      instance->log = std::move(log);
      try {
        instance->recover();
      } catch (const std::exception &error) {
        instance->log(error.what());
      }
    });
  }

  void shutdown() {
    instance.reset();
  }

  std::string prepare(policy mode, int width, int height, int fps) {
    if (mode == policy::disabled) {
      return {};
    }
    if (!instance) {
      return "Native display controller is not initialized";
    }
    return instance->call([=] {
      return instance->prepare_mode(mode, width, height, fps);
    });
  }

  void connected() {
    if (instance) {
      instance->call([] {
        instance->live = instance->mode != policy::disabled;
      });
    }
  }

  void release() {
    if (instance) {
      instance->call([] {
        try {
          instance->restore();
        } catch (const std::exception &error) {
          instance->live = false;
          instance->deadline = std::chrono::steady_clock::now() + 5s;
          instance->log(error.what());
        }
      });
    }
  }

  std::string output(const std::string &fallback) {
    return instance ? instance->call([&] {
      return instance->selector.empty() ? fallback : instance->selector;
    }) :
                      fallback;
  }
#else
  void init(const std::filesystem::path &, std::function<void(const std::string &)>) {}

  void shutdown() {}

  std::string prepare(policy mode, int, int, int) {
    return mode == policy::disabled ? "" : "Native display policies require Windows";
  }

  void connected() {}

  void release() {}

  std::string output(const std::string &fallback) {
    return fallback;
  }
#endif
}  // namespace native_display

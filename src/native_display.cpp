/** @file src/native_display.cpp
 * @brief Native Windows privacy transaction, recovery journal and session lifecycle.
 */
#include "native_display.h"

#include "native_display_state.h"

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
  #include <sddl.h>
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
        if (!win::valid_query_capacity(flags, pc, mc)) {
          throw std::runtime_error("Unexpected display query capacity: flags=" + std::to_string(flags) + ", paths=" + std::to_string(pc) + ", modes=" + std::to_string(mc));
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
      return win::is_mtt_target(identity(path));
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

    /** @brief Resolve the current source name instead of retaining a stale GDI alias. */
    std::string source_name(const DISPLAYCONFIG_PATH_INFO &path) {
      DISPLAYCONFIG_SOURCE_DEVICE_NAME name {};
      name.header = {DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME, sizeof(name), path.sourceInfo.adapterId, path.sourceInfo.id};
      check(DisplayConfigGetDeviceInfo(&name.header), "Capture output selection");
      std::string result;
      for (const wchar_t *c = name.viewGdiDeviceName; *c; ++c) {
        result += static_cast<char>(*c);
      }
      return result;
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

    /** @brief Read console lock state; unknown is never interpreted as locked. */
    int lock_state() {
      DWORD session {};
      if (!ProcessIdToSessionId(GetCurrentProcessId(), &session) || session != WTSGetActiveConsoleSessionId()) {
        return -1;
      }
      LPWSTR buffer {};
      DWORD bytes {};
      if (!WTSQuerySessionInformationW(WTS_CURRENT_SERVER_HANDLE, session, WTSSessionInfoEx, &buffer, &bytes)) {
        return -1;
      }
      int result = -1;
      if (bytes >= sizeof(WTSINFOEXW)) {
        const auto *info = reinterpret_cast<const WTSINFOEXW *>(buffer);
        if (info->Level == 1 && info->Data.WTSInfoExLevel1.SessionId == session) {
          result = info->Data.WTSInfoExLevel1.SessionFlags;
        }
      }
      WTSFreeMemory(buffer);
      return result;
    }

    /** @brief Attach only the worker thread to the current input desktop for CCD access. */
    class input_desktop {
      HDESK previous {GetThreadDesktop(GetCurrentThreadId())};  ///< Borrowed desktop.
      HDESK current {};  ///< Owned desktop handle.

    public:
      /** @brief Open the current desktop without displaying or unlocking it. */
      input_desktop() {
        current = OpenInputDesktop(0, FALSE, GENERIC_ALL);
        if (!current || !SetThreadDesktop(current)) {
          if (current) {
            CloseDesktop(current);
          }
          throw std::runtime_error("Cannot access input desktop for display recovery; journal retained");
        }
      }

      /** @brief Restore worker desktop and release only the owned handle. */
      ~input_desktop() {
        SetThreadDesktop(previous);
        CloseDesktop(current);
      }
    };

    /** @brief Recover onto connected physical targets if a saved external monitor was removed. */
    void physical_fallback(const layout &baseline) {
      auto all = read(QDC_ALL_PATHS);
      std::vector<DISPLAYCONFIG_PATH_INFO> targets;
      for (const auto &path : all.paths) {
        if (!path.targetInfo.targetAvailable || is_virtual(path)) {
          continue;
        }
        const auto same = [&](const auto &other) {
          return other.targetInfo.id == path.targetInfo.id && other.targetInfo.adapterId.LowPart == path.targetInfo.adapterId.LowPart && other.targetInfo.adapterId.HighPart == path.targetInfo.adapterId.HighPart;
        };
        if (std::any_of(targets.begin(), targets.end(), same)) {
          continue;
        }
        if (std::any_of(baseline.paths.begin(), baseline.paths.end(), same)) {
          targets.push_back(path);
        }
      }
      if (targets.empty()) {
        auto it = std::find_if(all.paths.begin(), all.paths.end(), [](const auto &path) {
          return path.targetInfo.targetAvailable && !is_virtual(path);
        });
        if (it == all.paths.end()) {
          throw std::runtime_error("No physical display is available; retaining private output");
        }
        targets.push_back(*it);
      }
      for (auto &path : targets) {
        path.flags = DISPLAYCONFIG_PATH_ACTIVE;
        path.sourceInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
        path.targetInfo.modeInfoIdx = DISPLAYCONFIG_PATH_MODE_IDX_INVALID;
      }
      check(SetDisplayConfig(static_cast<UINT32>(targets.size()), targets.data(), 0, nullptr, SDC_TOPOLOGY_SUPPLIED | SDC_APPLY | SDC_ALLOW_PATH_ORDER_CHANGES), "Restore connected physical topology");
    }

    /** @brief Serialize all Windows operations on the thread that owns the legacy guard mutex. */
    class controller {
    public:
      std::filesystem::path journal;  ///< Persistent rollback snapshot.
      std::function<void(const std::string &)> log;  ///< Diagnostic callback.
      std::function<void()> stop_streams;  ///< Nonblocking request to stop RTSP sessions.
      setup_lease lease;  ///< Generation-bound preparation.
      bool abort_requested {};  ///< Prevent repeated stream termination requests.
      std::mutex mutex;  ///< Protects command queue.
      std::condition_variable cv;  ///< Wakes command processing.
      std::queue<std::function<void()>> jobs;  ///< Synchronous commands.
      bool quit {};  ///< Worker exit requested.
      std::thread worker;  ///< Owns all display state below.
      HANDLE gate {};  ///< Shared mutex prevents the retained script keeper racing this controller.
      bool owns_gate {};  ///< Worker has acquired gate.
      bool armed {};  ///< Journal must be restored even if preparation failed.
      bool live {};  ///< At least one stream has started.
      bool pending {};  ///< A successful preparation is awaiting its RTSP connection.
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
          PSECURITY_DESCRIPTOR descriptor {};
          if (!ConvertStringSecurityDescriptorToSecurityDescriptorW(L"D:(A;;GA;;;SY)(A;;GA;;;BA)(A;;GA;;;IU)", SDDL_REVISION_1, &descriptor, nullptr)) {
            throw std::runtime_error("Cannot construct display coordination security descriptor");
          }
          SECURITY_ATTRIBUTES attributes {sizeof(attributes), descriptor, FALSE};
          gate = CreateMutexW(&attributes, FALSE, L"Local\\SunshinePrivacyGuard");
          LocalFree(descriptor);
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
        file.close();
        acquire();
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
          if (!confirm_lock(lock_state, [] {
                return LockWorkStation() != FALSE;
              },
                            [] {
                              std::this_thread::sleep_for(100ms);
                            })) {
            throw std::runtime_error("Cannot lock desktop; retaining private display and journal");
          }
          input_desktop desktop;
          try {
            apply(baseline);
          } catch (const std::exception &error) {
            if (log) {
              log(std::string("Saved topology unavailable: ") + error.what());
            }
            physical_fallback(baseline);
          }
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
        pending = false;
        lease.id.reset();
        abort_requested = false;
        selector.clear();
        target.clear();
        if (owns_gate) {
          ReleaseMutex(gate);
          owns_gate = false;
        }
      }

      /** @brief Prepare one application without polling Sunshine logs or UDP listeners. */
      std::string prepare_mode(std::uint32_t session_id, policy requested, int width, int height, int fps) {
        if (pending || live) {
          return "A native display session is already prepared or active";
        }
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
            pending = true;
            lease = {session_id, deadline};
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
              pending = true;
              lease = {session_id, deadline};
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
            if (live) {
              abort_requested = true;
              stop_streams();
            } else {
              deadline = std::chrono::steady_clock::now();
            }
          }
          if (live && !abort_requested && mode == policy::privacy && armed) {
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
              abort_requested = true;
              stop_streams();
            }
          }
          if (live && !abort_requested && std::chrono::steady_clock::now() >= deadline) {
            abort_requested = true;
            stop_streams();
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

  void init(const std::filesystem::path &path, std::function<void(const std::string &)> log, std::function<void()> stop_streams) {
    instance = std::make_unique<controller>();
    instance->call([&] {
      instance->journal = path;
      instance->log = std::move(log);
      instance->stop_streams = std::move(stop_streams);
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

  std::string prepare(std::uint32_t session_id, policy mode, int width, int height, int fps) {
    if (mode == policy::disabled) {
      return {};
    }
    if (!instance) {
      return "Native display controller is not initialized";
    }
    return instance->call([=] {
      return instance->prepare_mode(session_id, mode, width, height, fps);
    });
  }

  bool connected(std::uint32_t session_id) {
    if (instance) {
      return instance->call([=] {
        if (!instance->pending || instance->mode == policy::disabled || instance->lease.id != session_id) {
          return false;
        }
        try {
          if (instance->mode == policy::privacy && !matches(read(), instance->target, instance->width, instance->height, instance->fps)) {
            throw std::runtime_error("Private display changed before stream startup");
          }
          if (!instance->lease.commit(session_id, std::chrono::steady_clock::now())) {
            throw std::runtime_error("Native setup lease expired");
          }
        } catch (const std::exception &error) {
          instance->log(error.what());
          instance->pending = false;
          instance->lease.id.reset();
          try {
            instance->restore();
          } catch (const std::exception &recovery) {
            instance->log(recovery.what());
            instance->deadline = std::chrono::steady_clock::now() + 5s;
          }
          return false;
        }
        instance->live = true;
        instance->pending = false;
        instance->deadline = std::chrono::steady_clock::now() + 8h;
        return true;
      });
    }
    return false;
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
      if (instance->mode == policy::privacy) {
        try {
          const auto current = read();
          if (matches(current, instance->target, instance->width, instance->height, instance->fps)) {
            instance->selector = source_name(current.paths.front());
            return instance->selector;
          }
        } catch (...) {}
        // A disconnected GDI name can be reassigned to a physical monitor. Never reuse it blindly.
        return std::string("\\\\.\\SUNSHINE_PRIVACY_UNAVAILABLE");
      }
      return instance->selector.empty() ? fallback : instance->selector;
    }) :
                      fallback;
  }

  bool exclusive() {
    return instance && instance->call([] {
      return instance->mode == policy::privacy;
    });
  }

  bool capture_allowed(const std::string &name, std::uint32_t adapter_low, std::int32_t adapter_high) {
    return !instance || instance->call([&] {
      if (instance->mode != policy::privacy) {
        return true;
      }
      try {
        const auto current = read();
        if (!matches(current, instance->target, instance->width, instance->height, instance->fps)) {
          return false;
        }
        const auto &path = current.paths.front();
        return path.sourceInfo.adapterId.LowPart == adapter_low && path.sourceInfo.adapterId.HighPart == adapter_high && source_name(path) == name;
      } catch (...) {
        return false;
      }
    });
  }

  void cancel_setup(std::uint32_t session_id) {
    if (instance) {
      instance->call([=] {
        if (!instance->pending || instance->lease.id != session_id) {
          return;
        }
        instance->pending = false;
        instance->lease.id.reset();
        try {
          instance->restore();
        } catch (const std::exception &error) {
          instance->log(error.what());
          instance->deadline = std::chrono::steady_clock::now() + 5s;
        }
      });
    }
  }
#else
  void init(const std::filesystem::path &, std::function<void(const std::string &)>, std::function<void()>) {}

  void shutdown() {}

  std::string prepare(std::uint32_t, policy mode, int, int, int) {
    return mode == policy::disabled ? "" : "Native display policies require Windows";
  }

  bool connected(std::uint32_t) {
    return false;
  }

  bool exclusive() {
    return false;
  }

  bool capture_allowed(const std::string &, std::uint32_t, std::int32_t) {
    return true;
  }

  void cancel_setup(std::uint32_t) {}

  void release() {}

  std::string output(const std::string &fallback) {
    return fallback;
  }
#endif
}  // namespace native_display

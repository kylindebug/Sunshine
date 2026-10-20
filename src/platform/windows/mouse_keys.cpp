/**
 * @file src/platform/windows/mouse_keys.cpp
 * @brief Keep mouse-less desktops usable after streaming and restore settings on local mouse input.
 */
#include "mouse_keys_guard.h"
#include "src/logging.h"

#include <chrono>
#include <future>
#include <mutex>
#include <thread>

namespace platf::cursor {
  namespace {
    /** @brief Process-local owner of Mouse Keys settings and a passive raw mouse observer. */
    class service_t {
    public:
      /** @brief Create a message-only observer without intercepting or recording input. */
      service_t() {
        std::promise<HWND> ready;
        auto future = ready.get_future();
        worker_ = std::thread([this, ready = std::move(ready)]() mutable {
          run(std::move(ready));
        });
        window_ = future.get();
      }

      /**
       * @brief Stop observing without removing the last runtime cursor provider.
       * @details No settings are written to the user profile; a mouse-less fallback may
       * remain enabled until local mouse input restores it or the Windows session ends.
       */
      ~service_t() {
        if (window_) {
          PostMessageW(window_, WM_CLOSE, 0, 0);
        }
        if (worker_.joinable()) {
          worker_.join();
        }
      }

      /** @brief Start a stream while retaining any deferred original snapshot. */
      void start() {
        std::lock_guard lock(mutex_);
        streaming_ = true;
        last_report_.reset();
        guard_.begin_session();
        report(guard_.ensure(api_));
      }

      /** @brief Restore on the last disconnect, or retain the fallback without hardware. */
      void stop() {
        std::lock_guard lock(mutex_);
        streaming_ = false;
        last_report_.reset();
        report(guard_.restore(api_));
      }

      /** @brief Re-enable the cursor if a mouse disappears during a stream. */
      void ensure() {
        std::lock_guard lock(mutex_);
        if (streaming_) {
          report(guard_.ensure(api_));
        }
      }

    private:
      /**
       * @brief Emit transition-specific diagnostics without repeating errors every frame.
       * @param result Cursor protection transition result.
       */
      void report(result_e result) {
        if (result == result_e::unchanged || last_report_ == result) {
          return;
        }
        last_report_ = result;
        switch (result) {
          case result_e::enabled:
            BOOST_LOG(info) << "[CURSOR_FALLBACK_ENABLED] No usable mouse; enabled runtime Mouse Keys without changing speeds or hotkeys";
            break;
          case result_e::deferred:
            BOOST_LOG(info) << "[CURSOR_RESTORE_DEFERRED] No usable mouse after disconnect; retaining runtime cursor fallback until local mouse input";
            break;
          case result_e::restored:
            BOOST_LOG(info) << "[CURSOR_RESTORE_COMPLETE] Original Mouse Keys settings restored; a mouse remains available";
            break;
          case result_e::user_changed:
            BOOST_LOG(info) << "[CURSOR_USER_OVERRIDE] Mouse Keys changed externally; leaving the user's settings untouched";
            break;
          case result_e::read_failed:
          case result_e::write_failed:
            BOOST_LOG(warning) << "[CURSOR_SETTINGS_RETRY] Mouse Keys operation failed; retaining restoration state for retry";
            break;
          default:
            break;
        }
      }

      /**
       * @brief Retry restoration after actual mouse activity or a device arrival.
       * @param local_mouse True for a raw mouse event or device arrival; false for periodic maintenance.
       */
      void refresh(bool local_mouse) {
        std::lock_guard lock(mutex_);
        if (local_mouse && streaming_) {
          return;
        }
        const auto now = std::chrono::steady_clock::now();
        if (local_mouse && now - last_probe_ < std::chrono::seconds(1)) {
          return;
        }
        report(guard_.observe(api_));
        if (streaming_ || (!guard_.needs_retry() && !(local_mouse && guard_.deferred()))) {
          return;
        }
        if (now - last_probe_ < std::chrono::seconds(1)) {
          return;
        }
        last_probe_ = now;
        report(guard_.restore(api_));
      }

      /**
       * @brief Receive passive raw mouse notifications without suppressing normal input.
       * @param window Message-only observer window.
       * @param message Windows message identifier.
       * @param wparam Message-specific first parameter.
       * @param lparam Message-specific second parameter.
       * @return Standard Windows message-processing result.
       */
      static LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
        if (message == WM_NCCREATE) {
          auto create = reinterpret_cast<CREATESTRUCTW *>(lparam);
          SetWindowLongPtrW(window, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(create->lpCreateParams));
        }
        auto self = reinterpret_cast<service_t *>(GetWindowLongPtrW(window, GWLP_USERDATA));
        if (self) {
          if (message == WM_INPUT) {
            RAWINPUT raw {};
            UINT size = sizeof(raw);
            if (GetRawInputData(reinterpret_cast<HRAWINPUT>(lparam), RID_INPUT, &raw, &size, sizeof(RAWINPUTHEADER)) != static_cast<UINT>(-1) && raw.header.dwType == RIM_TYPEMOUSE && raw.header.hDevice) {
              self->refresh(true);
            }
          } else if (message == WM_INPUT_DEVICE_CHANGE && wparam == GIDC_ARRIVAL) {
            self->refresh(true);
          } else if (message == WM_TIMER) {
            self->refresh(false);
          } else if (message == WM_DESTROY) {
            PostQuitMessage(0);
          }
        }
        return DefWindowProcW(window, message, wparam, lparam);
      }

      /**
       * @brief Run the observer's message loop, using INPUTSINK without NOLEGACY.
       * @param ready Receives the window handle, including null if creation fails.
       */
      void run(std::promise<HWND> ready) {
        constexpr auto name = L"SunshineCursorFallbackObserver";
        WNDCLASSW wc {};
        wc.lpfnWndProc = window_proc;
        wc.hInstance = GetModuleHandleW(nullptr);
        wc.lpszClassName = name;
        const auto atom = RegisterClassW(&wc);
        const auto window = atom ? CreateWindowExW(0, name, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, wc.hInstance, this) : nullptr;
        if (!window) {
          BOOST_LOG(warning) << "[CURSOR_OBSERVER_UNAVAILABLE] Local mouse restoration will be retried on a later stream transition";
          ready.set_value(nullptr);
          if (atom) {
            UnregisterClassW(name, wc.hInstance);
          }
          return;
        }
        RAWINPUTDEVICE device {1, 2, RIDEV_INPUTSINK | RIDEV_DEVNOTIFY, window};
        if (!RegisterRawInputDevices(&device, 1, sizeof(device))) {
          BOOST_LOG(warning) << "[CURSOR_OBSERVER_UNAVAILABLE] Unable to subscribe to local mouse notifications: " << GetLastError();
        }
        SetTimer(window, 1, 1000, nullptr);
        ready.set_value(window);
        MSG message;
        while (GetMessageW(&message, nullptr, 0, 0) > 0) {
          TranslateMessage(&message);
          DispatchMessageW(&message);
        }
        RAWINPUTDEVICE remove {1, 2, RIDEV_REMOVE, nullptr};
        RegisterRawInputDevices(&remove, 1, sizeof(remove));
        UnregisterClassW(name, wc.hInstance);
      }

      std::mutex mutex_;  ///< Serializes frame, session and observer transitions.
      mouse_keys_guard_t guard_;  ///< Owns only settings written by this process.
      mouse_keys_api_t api_ {
        [] {
          return GetSystemMetrics(SM_MOUSEPRESENT) != 0;
        },
        [](MOUSEKEYS &keys) {
          keys.cbSize = sizeof(keys);
          return SystemParametersInfoW(SPI_GETMOUSEKEYS, 0, &keys, 0) != 0;
        },
        [](const MOUSEKEYS &keys) {
          auto copy = keys;
          return SystemParametersInfoW(SPI_SETMOUSEKEYS, 0, &copy, 0) != 0;
        }
      };  ///< Native settings backend; never uses SPIF_UPDATEINIFILE.
      bool streaming_ {false};  ///< The final stream has not ended yet.
      std::optional<result_e> last_report_;  ///< Last diagnostic transition.
      std::chrono::steady_clock::time_point last_probe_ {};  ///< Rate limit failed hardware restoration probes.
      std::thread worker_;  ///< Passive raw mouse event thread.
      HWND window_ {nullptr};  ///< Message-only window used for graceful worker shutdown.
    };

    /** @brief Return the process-local cursor fallback service. */
    service_t &service() {
      static service_t instance;
      return instance;
    }
  }  // namespace

  void streaming_started() {
    service().start();
  }

  void streaming_stopped() {
    service().stop();
  }

  void ensure_mouse_cursor() {
    service().ensure();
  }
}  // namespace platf::cursor

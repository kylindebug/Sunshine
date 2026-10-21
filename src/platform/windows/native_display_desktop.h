/** @file src/platform/windows/native_display_desktop.h
 * @brief Serialize CCD access on a thread that can follow the console input desktop.
 */
#pragma once

#include <condition_variable>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <queue>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <windows.h>

namespace native_display::win {
  /** @brief Injectable desktop operations; no desktop security or lock state is changed. */
  struct desktop_api {
    std::function<HDESK()> previous;  ///< Borrow the calling thread's desktop.
    std::function<HDESK()> open;  ///< Open the current input desktop.
    std::function<bool(HDESK)> attach;  ///< Attach only the calling thread.
    std::function<void(HDESK)> close;  ///< Release an owned, detached handle.
    std::function<DWORD()> error;  ///< Read the failing operation's Win32 error.
  };

  /** @brief Construct the production Win32 desktop operations.
   * @return Operations using existing process permissions without unlocking Windows.
   */
  inline desktop_api system_desktop_api() {
    return {
      [] {
        return GetThreadDesktop(GetCurrentThreadId());
      },
      [] {
        return OpenInputDesktop(0, FALSE, GENERIC_ALL);
      },
      [](HDESK desktop) {
        return SetThreadDesktop(desktop) != FALSE;
      },
      [](HDESK desktop) {
        CloseDesktop(desktop);
      },
      [] {
        return GetLastError();
      }
    };
  }

  /** @brief Bind one CCD operation to the input desktop and restore the borrowed desktop. */
  class input_desktop {
    const desktop_api &api;  ///< Operations outlive this scope.
    HDESK previous {};  ///< Borrowed desktop, never closed here.
    HDESK current {};  ///< Owned input desktop handle.

  public:
    /** @brief Attach the calling thread or fail before invoking any display operation.
     * @param api Desktop operations to use.
     */
    explicit input_desktop(const desktop_api &api):
        api(api),
        previous(api.previous()) {
      if (!previous) {
        throw failure("GetThreadDesktop", api.error());
      }
      current = api.open();
      if (!current) {
        throw failure("OpenInputDesktop", api.error());
      }
      if (!api.attach(current)) {
        const auto code = api.error();
        api.close(current);
        throw failure("SetThreadDesktop", code);
      }
    }

    /** @brief Detach before releasing the owned handle; never close the borrowed handle. */
    ~input_desktop() {
      api.attach(previous);
      api.close(current);
    }

    input_desktop(const input_desktop &) = delete;
    input_desktop &operator=(const input_desktop &) = delete;

  private:
    /** @brief Describe the exact permission/desktop stage without suggesting a port change.
     * @param operation Failed Win32 operation.
     * @param code Captured Win32 error before cleanup.
     * @return An actionable error that preserves fail-closed privacy behavior.
     */
    static std::runtime_error failure(const char *operation, DWORD code) {
      return std::runtime_error("[ND_INPUT_DESKTOP] " + std::string(operation) + ": Windows error " + std::to_string(code) + "; cannot access the current console desktop. Run Sunshine through its system service in the active console session; physical capture remains blocked.");
    }
  };

  /** @brief One reusable thread for CCD, deliberately owning no windows or keyboard hooks.
   * SetThreadDesktop fails on threads owning hooks/windows. The privacy controller owns
   * a keyboard hook, so it must not perform CCD on its own inherited Default desktop.
   */
  class desktop_executor {
    desktop_api api;  ///< Native operations or a test implementation.
    std::mutex mutex;  ///< Protects the queue and shutdown flag.
    std::condition_variable cv;  ///< Signals queued work and shutdown.
    std::queue<std::function<void()>> jobs;  ///< Synchronous callers' packaged tasks.
    bool quit {};  ///< Drain accepted work before joining.
    std::thread worker;  ///< Never creates a USER window or hook.

  public:
    /** @brief Start one reusable worker without touching desktop state.
     * @param api Desktop operations, defaulting to Win32.
     */
    explicit desktop_executor(desktop_api api = system_desktop_api()):
        api(std::move(api)),
        worker([this] {
          loop();
        }) {}

    /** @brief Finish accepted jobs and join before releasing API state. */
    ~desktop_executor() {
      {
        std::lock_guard lock(mutex);
        quit = true;
      }
      cv.notify_one();
      worker.join();
    }

    desktop_executor(const desktop_executor &) = delete;
    desktop_executor &operator=(const desktop_executor &) = delete;

    /** @brief Execute a CCD operation on the current input desktop, propagating failures.
     * @param fn Operation; must not recursively call this executor or create USER objects.
     * @return The operation's result after its desktop scope has been released.
     */
    template<class F>
    auto call(F fn) {
      using result = std::invoke_result_t<F>;
      auto task = std::make_shared<std::packaged_task<result()>>([this, fn = std::move(fn)]() mutable -> result {
        input_desktop desktop(api);
        return fn();
      });
      auto future = task->get_future();
      {
        std::lock_guard lock(mutex);
        if (quit) {
          throw std::runtime_error("Native display desktop worker is stopping");
        }
        jobs.emplace([task] {
          (*task)();
        });
      }
      cv.notify_one();
      return future.get();
    }

  private:
    /** @brief Drain queued work; packaged tasks isolate individual failures. */
    void loop() {
      for (;;) {
        std::function<void()> job;
        {
          std::unique_lock lock(mutex);
          cv.wait(lock, [this] {
            return quit || !jobs.empty();
          });
          if (jobs.empty() && quit) {
            return;
          }
          job = std::move(jobs.front());
          jobs.pop();
        }
        job();
      }
    }
  };
}  // namespace native_display::win

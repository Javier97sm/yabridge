// yabridge: a Wine VST bridge
// Copyright (C) 2020  Robbert van der Helm
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <https://www.gnu.org/licenses/>.

#pragma once

#include "boost-fix.h"

#include <memory>
#include <optional>

#define NOMINMAX
#define NOSERVICE
#define NOMCX
#define NOIMM
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <boost/asio/io_context.hpp>

/**
 * The delay between calls to the event loop at an even more than cinematic 30
 * fps.
 */
constexpr std::chrono::duration event_loop_interval =
    std::chrono::milliseconds(1000) / 30;

/**
 * A wrapper around `boost::asio::io_context()`. A single instance is shared for
 * all plugins in a plugin group so that most events can be handled on the main
 * thread, which can be required because all GUI related operations have to be
 * handled from the same thread. If during the Win32 message loop the plugin
 * performs a host callback and the host then calls a function on the plugin in
 * response, then this IO context will still be busy with the message loop
 * which. To prevent a deadlock in this situation, we'll allow different threads
 * to handle `dispatch()` calls while the message loop is running.
 */
class PluginContext {
   public:
    PluginContext();

    /**
     * Run the IO context. This rest of this class assumes that this is only
     * done from a single thread.
     */
    void run();

    /**
     * Drop all future work from the IO context. This does not necessarily mean
     * that the thread that called `plugin_context.run()` immediatly returns.
     */
    void stop();

    /**
     * Start a timer to handle events every `event_loop_interval` milliseconds.
     * `message_loop_active()` will return `true` while `handler` is being
     * executed.
     *
     * @param handler The function that should be executed in the IO context
     *   when the timer ticks. This should be a function that handles both the
     *   X11 events and the Win32 message loop.
     */
    template <typename F>
    void async_handle_events(F handler) {
        // Try to keep a steady framerate, but add in delays to let other events
        // get handled if the GUI message handling somehow takes very long.
        events_timer.expires_at(std::max(
            events_timer.expiry() + event_loop_interval,
            std::chrono::steady_clock::now() + std::chrono::milliseconds(5)));
        events_timer.async_wait(
            [&, handler](const boost::system::error_code& error) {
                if (error.failed()) {
                    return;
                }

                event_loop_active = true;
                handler();
                event_loop_active = false;

                async_handle_events(handler);
            });
    }

    /**
     * Is `true` if the context is currently handling the Win32 message loop and
     * incoming `dispatch()` events should be handled on their own thread (as
     * posting them to the IO context will thus block).
     */
    std::atomic_bool event_loop_active;

    /**
     * The raw IO context. Can and should be used directly for everything that's
     * not the event handling loop.
     */
    boost::asio::io_context context;

   private:
    /**
     * The timer used to periodically handle X11 events and Win32 messages.
     */
    boost::asio::steady_timer events_timer;
};

/**
 * A proxy function that calls `Win32Thread::entry_point` since `CreateThread()`
 * is not usable with lambdas directly. Calling the passed function will invoke
 * the lambda with the arguments passed during `Win32Thread`'s constructor. This
 * function deallocates the function after it's finished executing.
 *
 * We can't store the function pointer in the `Win32Thread` object because
 * moving a `Win32Thread` object would then cause issues.
 *
 * @param win32_thread_trampoline A `std::function<void()>*` pointer to a
 *   function pointer, great.
 */
uint32_t WINAPI win32_thread_trampoline(std::function<void()>* entry_point);

/**
 * Taken from the C++ reference:
 * https://en.cppreference.com/w/cpp/thread/jthread
 */
template <class T>
std::decay_t<T> decay_copy(T&& v) {
    return std::forward<T>(v);
}

/**
 * A simple RAII wrapper around the Win32 thread API that imitates
 * `std::thread`.
 *
 * `std::thread` directly uses pthreads. This means that, like with
 * `CreateThread()`, some thread local information does not get initialized
 * which can lead to memory errors.
 *
 * TODO: Once these changes are complete, check if we can drop `PluginContext`
 *       again and execute all 'safe' opcodes on the calling thread.
 *
 * @note This should be used instead of `std::thread` or `std::jthread` whenever
 *   the thread directly calls third party library code, i.e. `LoadLibrary()`,
 *   `FreeLibrary()`, the plugin's entry point, or any of the `AEffect::*()`
 *   functions.
 */
class Win32Thread {
   public:
    /**
     * Constructor that does not start any thread yet.
     */
    Win32Thread();

    /**
     * Constructor that immediately starts running the thread. This works
     * equivalently to `std::thread`.
     *
     * @param entry_point The thread entry point that should be run.
     * @param parameter The parameter passed to the entry point function.
     */
    template <class Function, class... Args>
    Win32Thread(Function&& f, Args&&... args)
        : handle(
              CreateThread(nullptr,
                           0,
                           reinterpret_cast<LPTHREAD_START_ROUTINE>(
                               win32_thread_trampoline),
                           // We'll capture the function by move since the
                           // lambda will often go out of scope in the time that
                           // the thread is starting. Alternatively we could
                           // wait for the thread to be up before continuing,
                           // that may be a bit safer.
                           new std::function<void()>([&, f = std::move(f)]() {
                               std::invoke(
                                   f, decay_copy(std::forward<Args>(args))...);
                           }),
                           0,
                           nullptr),
              CloseHandle) {}

    Win32Thread(const Win32Thread&) = delete;
    Win32Thread& operator=(const Win32Thread&) = delete;

    Win32Thread(Win32Thread&&);
    Win32Thread& operator=(Win32Thread&&);

   private:
    /**
     * The handle for the thread that is running, will be a null pointer if this
     * class was constructed with the default constructor.
     */
    std::unique_ptr<std::remove_pointer_t<HANDLE>, decltype(&CloseHandle)>
        handle;
};

/**
 * A simple RAII wrapper around `SetTimer`. Does not support timer procs since
 * we don't use them.
 */
class Win32Timer {
   public:
    Win32Timer(HWND window_handle, size_t timer_id, unsigned int interval_ms);
    ~Win32Timer();

    Win32Timer(const Win32Timer&) = delete;
    Win32Timer& operator=(const Win32Timer&) = delete;

    Win32Timer(Win32Timer&&);
    Win32Timer& operator=(Win32Timer&&);

   private:
    HWND window_handle;
    std::optional<size_t> timer_id;
};

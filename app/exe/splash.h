#pragma once
// The splash: VanaCore's logo on the card, a line saying what's happening and a progress bar, on a thread of its own so it
// keeps drawing while the main thread checks for updates and loads VanaCoreApp.dll. Its X cancels the start.

#include <windows.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

#include "ui.h"

namespace vcapp {

class Splash {
public:
    explicit Splash(HINSTANCE instance) : instance_(instance) {}
    ~Splash();
    Splash(const Splash&) = delete;
    Splash& operator=(const Splash&) = delete;

    // Starts its thread and returns once it's on the screen (or couldn't be: no Direct2D, then there's no splash)
    void show();
    // Its line and progress (0-1; below 0, a sweep that doesn't end), from any thread; a `note` (something that didn't
    // work out, not stopping anything) in amber
    void status(const std::wstring& text, float progress, bool note = false);
    // Closed, from any thread (nothing once it is)
    void close();
    // The player closed it with its X
    bool cancelled() const { return cancelled_; }

private:
    enum Hit { kNone, kClose };
    void run();
    void render();
    static LRESULT CALLBACK proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT handle(UINT message, WPARAM wparam, LPARAM lparam);

    HINSTANCE instance_;
    std::thread thread_;
    HANDLE ready_ = nullptr;
    std::atomic<HWND> window_{nullptr};
    std::atomic<bool> cancelled_{false};
    ui::Canvas canvas_;
    ui::Hits<Hit> hits_;
    Hit hover_ = kNone;
    bool tracking_ = false;
    float sweep_ = 0; // the sweep's place, 0-1

    std::mutex mutex_; // what status() set, read by render()
    std::wstring text_ = L"Starting\x2026";
    float progress_ = -1;
    bool note_ = false;
};

} // namespace vcapp

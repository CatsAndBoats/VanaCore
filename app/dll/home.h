#pragma once
// The app's first view: VanaCore's card with Play (VanaCore.exe --retail, from the app's own folder) and Close. A start to
// build the real views on.

#include <windows.h>

#include <string>

#include "app_api.h"
#include "ui.h"

namespace vcapp {

class Home {
public:
    explicit Home(const VanaCoreAppHost& host);
    Home(const Home&) = delete;
    Home& operator=(const Home&) = delete;

    // Shows it (closing the splash once it's up) and waits until it's shut: the exit code
    int run();

private:
    enum Hit { kNone, kClose, kPlay, kQuit };
    void render();
    void press(Hit hit);
    bool play();
    static LRESULT CALLBACK proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT handle(UINT message, WPARAM wparam, LPARAM lparam);
    void mouse(UINT message, LPARAM lparam);

    const VanaCoreAppHost& host_;
    std::wstring game_;  // VanaCore.exe beside the app, "" when it isn't there
    std::wstring error_; // what went wrong starting it, shown under the facts
    HWND window_ = nullptr;
    ui::Canvas canvas_;
    ui::Hits<Hit> hits_;
    Hit hover_ = kNone, pressed_ = kNone;
    bool tracking_ = false;
};

} // namespace vcapp

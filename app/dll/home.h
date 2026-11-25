#pragma once
// The app's first view: VanaCore's card over the background picture, its head (the installed VanaCore's version, who made it),
// and a glass bar at the bottom: the status on the left (with a progress bar for downloads to come), Play on the right
// (VanaCore.exe --retail, from the app's own folder). A start to build the real views on.

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
    enum Hit { kNone, kClose, kPlay };
    // What the status panel says: an icon in its colour, a line, a smaller one under it, and a progress bar (0..1; < 0: none)
    struct Status {
        wchar_t icon = 0;
        std::uint32_t colour = 0;
        std::wstring title, detail;
        float progress = -1;
    };
    Status status() const;
    void draw_status(const Status& s, float x, float cy, float room);
    void render();
    void press(Hit hit);
    bool play();
    static LRESULT CALLBACK proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT handle(UINT message, WPARAM wparam, LPARAM lparam);
    void mouse(UINT message, LPARAM lparam);

    const VanaCoreAppHost& host_;
    std::wstring game_;     // VanaCore.exe beside the app, "" when it isn't there
    std::wstring vanacore_; // its version (VanaCore.Core.dll's ProductVersion), "" when it has none
    std::wstring error_; // what went wrong starting it, shown under the facts
    HWND window_ = nullptr;
    ui::Canvas canvas_;
    ui::Hits<Hit> hits_;
    Hit hover_ = kNone, pressed_ = kNone;
    bool tracking_ = false;
};

} // namespace vcapp

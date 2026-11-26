#pragma once
// VanaCoreApp as VanaCore's installer: run from a folder without VanaCore.exe (a download on its own), it shows a card that
// asks where to put VanaCore (a folder of the player's: VanaCore writes its settings and logs beside itself, so not Program
// Files), takes the newest VanaCore release's zip from GitHub, unpacks it there, puts VanaCoreApp beside it, makes the
// shortcuts and an Apps & features entry, then opens the installed App. VanaCoreApp.exe --uninstall (that entry's) takes it
// all away again.

#include <windows.h>

#include <atomic>
#include <mutex>
#include <string>
#include <thread>

#include "app_api.h"
#include "ui.h"

namespace vcapp {

class Installer {
public:
    explicit Installer(const VanaCoreAppHost& host);
    ~Installer();
    Installer(const Installer&) = delete;
    Installer& operator=(const Installer&) = delete;

    // Shows it (closing the splash once it's up) and waits until it's shut: the exit code
    int run();

private:
    enum Hit : int { kNone = 0, kClose, kInstall, kChange, kDesktop, kStartMenu, kOpenFolder };
    enum Stage { kReady, kFinding, kDownloading, kUnpacking, kFinishing, kDone, kFailed };

    void render();
    void draw_progress(const D2D1_RECT_F& r);
    void press(int hit);
    void start();
    void work();
    bool finish(const std::wstring& version, std::wstring& why);
    void say(Stage stage, const std::wstring& text, float progress = -1);
    static LRESULT CALLBACK proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT handle(UINT message, WPARAM wparam, LPARAM lparam);
    void mouse(UINT message, LPARAM lparam);

    const VanaCoreAppHost& host_;
    std::wstring target_; // where VanaCore goes
    bool desktop_ = true, start_menu_ = true;
    std::mutex lock_; // the worker's news: stage_, text_, progress_
    Stage stage_ = kReady;
    std::wstring text_;
    float progress_ = -1;
    std::thread worker_;
    std::atomic<bool> stopping_{false};
    HWND window_ = nullptr;
    ui::Canvas canvas_;
    ui::Hits<int> hits_;
    int hover_ = kNone, pressed_ = kNone;
    bool tracking_ = false;
};

// VanaCoreApp.exe --uninstall: asked first, then the shortcuts, the Apps & features entry and the folder (after this
// process has gone) are taken away. The exit code.
int uninstall(const VanaCoreAppHost& host);

// Whether `dir` holds a VanaCore install (VanaCore.exe)
bool has_vanacore(const std::wstring& dir);

} // namespace vcapp

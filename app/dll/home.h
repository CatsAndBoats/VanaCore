#pragma once
// The app's first view: VanaCore's card over the background picture, its head (the installed VanaCore's version, who made it),
// tabs under it (FFXI, Graphics, Pivot, Profiles, About: home_pages.cpp, home_profiles.cpp; Errors, red, when something's
// wrong), and a glass bar at the bottom: the profile and START side by side in its middle (VanaCore.exe --retail with the
// profile's settings file).

#include <windows.h>

#include <string>
#include <vector>

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
    // What's under the mouse: the card's controls; a page's list rows from kRow, four ids a row (kRow + row * 4 + part)
    enum Hit : int {
        kNone = 0,
        kClose,
        kPlay,
        kProfile, // the footer's profile button
        kTab = 10, // + the tab
        kLocate = 20,
        kPad,
        kFfxiFolder,
        kRenderer = 30, // + the choice
        kEffects = 40,
        kPivotOn = 50,
        kPivotRoot,
        kPivotOpen,
        kGithub = 60,
        kNewProfile = 70,
        kMenu = 80, // the profile pick-list: + the profile; kMenuManage its last row
        kMenuManage = 99,
        kRow = 100,
        kProfileRow = 1000, // the Profiles tab's rows: + row * 4 + part (0 pick, 1 rename, 2 delete)
    };
    enum Tab { kFfxi, kGraphics, kPivot, kProfiles, kAbout, kErrors, kTabs };
    // The FFXI install: its folder and PlayOnline's (the registry's, else what Locate picked), the region's tools
    struct Install {
        std::wstring ffxi, pol, region, pad; // (pad: FFXiPadConfig.exe, "" when there's none)
        bool registry = false;
    };
    // A Pivot overlay: its name as [pivot.order] has it (a folder under the root, or a path), on when it's in the order
    struct Overlay {
        std::wstring name;
        bool on = false;
    };
    // A profile: its name, and its settings file in the app's folder (vanacore.ini, or vanacore.profile-<n>.ini)
    struct Profile {
        std::wstring name, file;
    };
    // Something wrong, on the Errors tab: a line, more about it, and what fixes it (a hit, kNone: nothing to press)
    struct Error {
        std::wstring title, detail;
        int fix = kNone;
        std::wstring fix_label;
    };

    void draw_tabs(float x, float y);
    void draw_footer();
    void draw_menu();
    void render();
    void press(int hit);
    bool play();
    bool can_play() const;
    static LRESULT CALLBACK proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam);
    LRESULT handle(UINT message, WPARAM wparam, LPARAM lparam);
    void mouse(UINT message, LPARAM lparam);

    // home_pages.cpp: what's installed, the settings files, and the FFXI, Graphics, Pivot and About pages
    void detect();
    void load_pivot();
    void save_pivot();
    std::wstring ini_get(const wchar_t* section, const wchar_t* key, const wchar_t* fallback = L"") const;
    void ini_set(const wchar_t* section, const wchar_t* key, const std::wstring* value);
    void set_in(const std::wstring& file, const wchar_t* section, const wchar_t* key, const std::wstring* value);
    std::wstring pick_folder(const wchar_t* title) const;
    void locate();
    void open(const std::wstring& what, const wchar_t* args = nullptr);
    void draw_page(const D2D1_RECT_F& r);
    void page_ffxi(const D2D1_RECT_F& r);
    void page_graphics(const D2D1_RECT_F& r);
    void page_pivot(const D2D1_RECT_F& r);
    void page_about(const D2D1_RECT_F& r);
    void fact(float x, float cy, bool ok, const std::wstring& title, const std::wstring& detail, float room);
    bool press_page(int hit);
    float t_of(int hit) const;

    // home_profiles.cpp: the profiles, the Profiles and Errors pages
    void load_profiles();
    void save_profiles();
    void use_profile(int index);
    void new_profile();
    void delete_profile(int index);
    void start_rename(int index);
    void end_rename(bool keep);
    bool rename_key(UINT message, WPARAM wparam);
    std::wstring unique_name(const std::wstring& name, int except) const;
    std::vector<Error> errors() const;
    void page_profiles(const D2D1_RECT_F& r);
    void page_errors(const D2D1_RECT_F& r);
    bool press_profiles(int hit);

    const VanaCoreAppHost& host_;
    std::wstring dir_, app_ini_, ini_; // the app's folder, its vanacore.ini (the app's own [app] keys), the profile's file
    std::wstring game_;                // VanaCore.exe beside the app, "" when it isn't there
    std::wstring vanacore_;            // its version (VanaCore.Core.dll's ProductVersion), "" when it has none
    std::wstring error_;               // what went wrong starting it (the Errors tab says)
    Install install_;
    std::wstring ffxi_date_; // FFXiMain.dll's date, "" without it
    std::wstring pivot_root_;
    bool pivot_on_ = false;
    std::vector<Overlay> overlays_;
    int pivot_scroll_ = 0;
    std::vector<Profile> profiles_;
    int profile_ = 0, profile_scroll_ = 0;
    int renaming_ = -1; // the profile whose name is being typed, -1: none
    std::wstring typed_;
    bool menu_ = false; // the footer's pick-list is open
    Tab tab_ = kFfxi;
    HWND window_ = nullptr;
    ui::Canvas canvas_;
    ui::Hits<int> hits_;
    int hover_ = kNone, pressed_ = kNone;
    bool tracking_ = false;
};

} // namespace vcapp

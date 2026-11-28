// The Addons tab: what VanaCore has (its Ashita and Windower addons and plugins, what boot.txt loads), and From Ashita / From
// Windower: the player picks the folder they played with and its addons, plugins, scripts and boot script come over
// (migrate.h), in the background with a bar, asked first with what was found.
#include "home.h"

#include <shellapi.h>

#include <thread>

namespace vcapp {

namespace {

int folders_in(const std::wstring& folder, bool folders, const wchar_t* ext)
{
    int n = 0;
    WIN32_FIND_DATAW found{};
    const HANDLE find = FindFirstFileW((folder + L"\\*").c_str(), &found);
    if (find == INVALID_HANDLE_VALUE)
        return 0;
    const std::size_t el = wcslen(ext);
    do {
        const std::wstring name = found.cFileName;
        const bool folder_here = (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (name == L"." || name == L".." || folder_here != folders)
            continue;
        if (folders ? _wcsicmp(name.c_str(), L"libs") != 0 && _wcsicmp(name.c_str(), L"res") != 0
                    : name.size() > el && _wcsicmp(name.c_str() + name.size() - el, ext) == 0)
            ++n;
    } while (FindNextFileW(find, &found));
    FindClose(find);
    return n;
}

std::wstring plural(int n, const wchar_t* one, const wchar_t* many)
{
    return std::to_wstring(n) + L" " + (n == 1 ? one : many);
}

} // namespace

// The tab's counts: VanaCore's Ashita and Windower addons (folders) and plugins (.dll), and the addons and plugins boot.txt loads
void Home::count_addons()
{
    static const wchar_t* const kKinds[] = {L"ashita", L"windower"};
    for (int k = 0; k < 2; ++k) {
        have_addons_[k] = folders_in(dir_ + L"\\addons\\" + kKinds[k], true, L"");
        have_plugins_[k] = folders_in(dir_ + L"\\plugins\\" + kKinds[k], false, L".dll");
    }
    boot_loads_ = 0;
    FILE* file = nullptr;
    if (_wfopen_s(&file, (dir_ + L"\\scripts\\boot.txt").c_str(), L"rb") != 0 || file == nullptr)
        return;
    char line[1024];
    while (fgets(line, sizeof line, file) != nullptr) {
        const char* c = line;
        while (*c == ' ' || *c == '\t')
            ++c;
        if (*c == '/' && c[1] == '/')
            ++c;
        if (_strnicmp(c, "/addon load ", 12) == 0 || _strnicmp(c, "/plugin load ", 13) == 0)
            ++boot_loads_;
    }
    fclose(file);
}

// From Ashita / From Windower: the folder picked, what's in it said and asked, then copied on a thread of its own
void Home::start_migrate(migrate::Kind kind)
{
    if (migration_->running)
        return;
    const wchar_t* name = migrate::name_of(kind);
    const std::wstring picked = pick_folder(kind == migrate::Kind::Ashita ? L"Your Ashita folder (with Ashita-cli.exe, addons and plugins)"
                                                                          : L"Your Windower folder (with Windower.exe, addons and plugins)");
    if (picked.empty())
        return;
    auto found = std::make_shared<migrate::Found>();
    std::wstring why;
    if (!migrate::find(kind, picked, dir_, *found, why)) {
        MessageBoxW(window_, why.c_str(), L"VanaCore", MB_OK | MB_ICONWARNING);
        return;
    }
    std::wstring ask = std::wstring(name) + L" in " + found->root + L" has:\n\n" + plural(static_cast<int>(found->addons.size()), L"addon", L"addons") +
                       L", " + plural(static_cast<int>(found->plugins.size()), L"plugin", L"plugins") + L", " +
                       plural(static_cast<int>(found->scripts.size()), L"script", L"scripts");
    if (!found->configs.empty())
        ask += L", their settings (config\\)";
    if (!found->script.empty())
        ask += L"\n" + found->script.substr(found->script.find_last_of(L'\\') + 1) + L": " + plural(found->loads, L"addon or plugin", L"addons and plugins") +
               L" it loads, and its other lines, for boot.txt";
    ask += std::wstring(L"\n\nCopy them into VanaCore? They go in its ") + (kind == migrate::Kind::Ashita ? L"addons\\ashita and plugins\\ashita" : L"addons\\windower and plugins\\windower") +
           L" folders. Nothing VanaCore has already is replaced, and boot.txt is kept as it was in boot.txt.before-migrate.";
    if (MessageBoxW(window_, ask.c_str(), L"VanaCore", MB_OKCANCEL | MB_ICONQUESTION) != IDOK)
        return;
    {
        std::lock_guard<std::mutex> lock(migration_->mutex);
        migration_->running = true;
        migration_->done = false;
        migration_->kind = kind;
        migration_->now.clear();
        migration_->progress = 0;
    }
    auto state = migration_;
    const std::wstring dir = dir_;
    const HWND window = window_;
    std::thread([state, found, dir, window] {
        const migrate::Result result = migrate::run(*found, dir, [&](const std::wstring& now, int done, int total) {
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                state->now = now;
            }
            state->progress = total > 0 ? static_cast<float>(done) / static_cast<float>(total) : 1.0f;
            PostMessageW(window, ui::kRedraw, 0, 0);
        });
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->result = result;
            state->done = true;
            state->fresh = true;
        }
        state->running = false;
        PostMessageW(window, ui::kRedraw, 0, 0);
    }).detach();
}

void Home::page_addons(const D2D1_RECT_F& r)
{
    bool done = false, fresh = false;
    migrate::Kind kind{};
    std::wstring now;
    migrate::Result result;
    {
        std::lock_guard<std::mutex> lock(migration_->mutex);
        done = migration_->done;
        fresh = migration_->fresh;
        migration_->fresh = false;
        kind = migration_->kind;
        now = migration_->now;
        result = migration_->result;
    }
    if (fresh)
        count_addons();
    const bool running = migration_->running;
    // the head: the tab's name and line; Open boot.txt and the addons folder at its right
    float y = r.top + 8;
    const std::wstring boot = L"boot.txt";
    const float bw = canvas_.text_width(boot, ui::Font::Button) + 24 + 28, ow = 30;
    const D2D1_RECT_F folder = D2D1::RectF(r.right - ow, y - 14, r.right, y + 14), open = D2D1::RectF(folder.left - 6 - bw, y - 14, folder.left - 6, y + 14);
    canvas_.button(open, boot, ui::Button::Secondary, t_of(kOpenBoot), true, ui::icon::open);
    canvas_.button(folder, L"", ui::Button::Ghost, t_of(kOpenAddons), true, ui::icon::folder);
    hits_.add(kOpenBoot, open);
    hits_.add(kOpenAddons, folder);
    tip(kOpenBoot, L"Open scripts\\boot.txt: what loads at every start");
    tip(kOpenAddons, L"Open VanaCore's addons folder");
    canvas_.text(L"ADDONS", ui::Font::Label, r.left, y, ui::rgb(ui::pal::dim));
    canvas_.text(canvas_.fit(L"Bring yours over: pick the folder you played with", ui::Font::Small, open.left - 12 - (r.left + 64), false), ui::Font::Small,
                 r.left + 64, y, ui::rgb(ui::pal::faint));
    y += 24;
    canvas_.line(r.left, y, r.right, y, ui::rgb(ui::pal::white, 0.08f));
    // From Ashita, From Windower: a card each
    const float gap = 12, cw = (r.right - r.left - gap) / 2, ch = 56, top = y + 10;
    static const wchar_t* const kTitles[] = {L"From Ashita\x2026", L"From Windower\x2026"};
    static const wchar_t* const kLines[] = {L"Addons, plugins, settings, default.txt", L"Addons, plugins, scripts, init.txt"};
    for (int k = 0; k < 2; ++k) {
        const int id = kFromAshita + k;
        const float x = r.left + k * (cw + gap);
        const bool over = hover_ == id && !running, down = over && pressed_ == id;
        canvas_.fill(x, top, cw, ch, 6, ui::rgb(over ? ui::pal::accent : ui::pal::white, down ? 0.2f : over ? 0.12f : 0.035f));
        canvas_.outline(x, top, cw, ch, 6, ui::rgb(over ? ui::pal::accent : ui::pal::white, over ? 0.7f : 0.12f));
        canvas_.glyph(ui::icon::download, ui::Font::IconBig, x + 24, top + ch / 2, ui::rgb(running ? ui::pal::faint : over ? ui::pal::white : ui::pal::accent2));
        canvas_.text(kTitles[k], ui::Font::Body, x + 46, top + 19, ui::rgb(running ? ui::pal::dim : ui::pal::white));
        canvas_.text(canvas_.fit(kLines[k], ui::Font::Small, cw - 56, false), ui::Font::Small, x + 46, top + 38, ui::rgb(ui::pal::dim));
        if (!running)
            hits_.add(id, D2D1::RectF(x, top, x + cw, top + ch));
        // under the card: what VanaCore has of this kind already (a check when there's some)
        const int addons = have_addons_[k], plugins = have_plugins_[k];
        const float hy = top + ch + 16;
        if (addons + plugins > 0) {
            canvas_.glyph(ui::icon::check, ui::Font::Icon, x + 8, hy, ui::rgb(ui::pal::teal));
            canvas_.text(canvas_.fit(plural(addons, L"addon", L"addons") + L", " + plural(plugins, L"plugin", L"plugins") + L" in VanaCore", ui::Font::Small, cw - 22, false),
                         ui::Font::Small, x + 22, hy, ui::rgb(ui::pal::text2));
        } else {
            canvas_.text(L"None in VanaCore yet", ui::Font::Small, x + 4, hy, ui::rgb(ui::pal::faint));
        }
    }
    // under them: the copy going on (a bar), what it did, or what boot.txt loads
    y = top + ch + 46;
    const float room = r.right - r.left;
    if (running) {
        canvas_.text(canvas_.fit(L"Copying " + now + L"\x2026", ui::Font::Small, room, true), ui::Font::Small, r.left, y, ui::rgb(ui::pal::text2));
        canvas_.bar(r.left, y + 14, room, migration_->progress);
        return;
    }
    if (done) {
        const bool any = result.addons + result.plugins + result.configs + result.scripts + result.lines > 0;
        const bool bad = result.failed > 0 || result.boot_failed;
        canvas_.glyph(bad ? ui::icon::error : ui::icon::check, ui::Font::IconBig, r.left + 10, y, ui::rgb(bad ? ui::pal::amber : ui::pal::teal));
        std::wstring head = std::wstring(migrate::name_of(kind)) + L": ";
        if (!any) {
            head += L"nothing new, VanaCore had it all already";
        } else {
            head += plural(result.addons, L"addon", L"addons") + L", " + plural(result.plugins, L"plugin", L"plugins");
            if (result.scripts > 0)
                head += L", " + plural(result.scripts, L"script", L"scripts");
            head += L" copied";
        }
        canvas_.text(canvas_.fit(head, ui::Font::Body, room - 32, false), ui::Font::Body, r.left + 32, y - 1, ui::rgb(ui::pal::white));
        std::wstring more = result.boot_failed ? L"scripts\\boot.txt couldn't be written"
                            : result.lines > 0 ? L"boot.txt: " + plural(result.lines, L"line", L"lines") + L" added (" + plural(result.loads, L"load", L"loads") +
                                                     L"), for the next start"
                                               : L"boot.txt had its lines already";
        if (result.kept > 0)
            more += L"; " + plural(result.kept, L"file", L"files") + L" VanaCore had kept";
        if (result.failed > 0)
            more += L"; " + plural(result.failed, L"file", L"files") + L" couldn't be copied";
        canvas_.text(canvas_.fit(more, ui::Font::Small, room - 32, false), ui::Font::Small, r.left + 32, y + 18, ui::rgb(bad ? ui::pal::amber : ui::pal::dim));
        return;
    }
    const std::wstring have = L"boot.txt loads " + plural(boot_loads_, L"addon or plugin", L"addons and plugins") + L" at every start";
    canvas_.text(canvas_.fit(have, ui::Font::Small, room, false), ui::Font::Small, r.left, y, ui::rgb(ui::pal::dim));
    canvas_.text(L"Use /vana to manage your addons", ui::Font::Small, r.left, y + 18, ui::rgb(ui::pal::dim));
}

// A click on the Addons tab's controls: true when there's something new to draw
bool Home::press_addons(int hit)
{
    if (hit == kFromAshita || hit == kFromWindower) {
        start_migrate(hit == kFromAshita ? migrate::Kind::Ashita : migrate::Kind::Windower);
        return true;
    }
    if (hit == kOpenBoot) {
        const std::wstring boot = dir_ + L"\\scripts\\boot.txt";
        if (GetFileAttributesW(boot.c_str()) != INVALID_FILE_ATTRIBUTES)
            ShellExecuteW(window_, nullptr, L"notepad.exe", (L"\"" + boot + L"\"").c_str(), nullptr, SW_SHOWNORMAL);
        return true;
    }
    if (hit == kOpenAddons) {
        open(dir_ + L"\\addons");
        return true;
    }
    return false;
}

} // namespace vcapp

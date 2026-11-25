#pragma once

#include <string>
#include <vector>

namespace vanacore {

// What the --gui start screen begins with (from vanacore.ini) and what it hands back.
struct GuiChoice {
    bool retail = false;      // PlayOnline as installed (--retail), else VanaCore's own login
    std::wstring server;      // --server, private server only
    std::wstring ini;         // --ini, "" for vanacore.ini
};

// The start screen: a modal window with Retail / Private server, the server, a settings file and Start Game.
// true on Start Game (`choice` is what was picked), false when the window was closed.
bool show_start_screen(GuiChoice& choice);

} // namespace vanacore

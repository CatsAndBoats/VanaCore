#include "folders.hpp"

#include <windows.h>

#include <string>

#include "log.hpp"

namespace vanacore::folders {
namespace {

struct Folder {
    const wchar_t* path; // under VanaCore's folder
    const char* readme;  // README.txt's text, \n line ends (written as \r\n)
};

constexpr Folder kFolders[] = {
    {L"addons\\ashita",
     "Ashita addons go here, one folder per addon, as in Ashita's own addons folder:\n"
     "\n"
     "    addons\\ashita\\<name>\\<name>.lua\n"
     "\n"
     "Load one in game with /addon load <name> (or /addon load <name> ashita when a Windower addon has the same\n"
     "name). To load it at every start, put that line in scripts\\boot.txt.\n"
     "\n"
     "Ashita's libraries (common, settings, imgui, ...) come with VanaCore: Ashita's addons\\libs isn't needed here.\n"
     "Ashita plugins (.dll) go in plugins\\ashita, not here.\n"},
    {L"addons\\windower",
     "Windower addons go here, one folder per addon, as in Windower's own addons folder:\n"
     "\n"
     "    addons\\windower\\<name>\\<name>.lua\n"
     "\n"
     "Load one in game with /addon load <name> (or /addon load <name> windower when an Ashita addon has the same\n"
     "name). Commands take one slash in VanaCore: /gs, not //gs. To load it at every start, put that line in\n"
     "scripts\\boot.txt.\n"
     "\n"
     "This folder stands for Windower's own folder too: what Windower keeps in Windower\\res and Windower\\addons\\libs\n"
     "is addons\\windower\\res and addons\\windower\\libs here. Neither is needed: Windower's libraries and resources\n"
     "come with VanaCore (addons\\vanacore\\libs\\windower), and a file an addon reads from libs\\ or res\\ that isn't\n"
     "here is read from VanaCore's copy. A file you put in addons\\windower\\libs or res is used instead of VanaCore's.\n"
     "\n"
     "Windower plugins (.dll) go in plugins\\windower, not here.\n"},
    {L"plugins\\ashita",
     "Ashita plugins go here: the .dll itself, not in a folder of its own.\n"
     "\n"
     "    plugins\\ashita\\<name>.dll\n"
     "\n"
     "Load one in game with /plugin load <name> (or /plugin load <name> ashita). To load it at every start, put that\n"
     "line in scripts\\boot.txt. Ashita addons (.lua) go in addons\\ashita, not here.\n"},
    {L"plugins\\windower",
     "Windower plugins go here: the .dll itself, not in a folder of its own.\n"
     "\n"
     "    plugins\\windower\\<name>.dll\n"
     "\n"
     "Load one in game with /plugin load <name> windower. To load it at every start, put that line in\n"
     "scripts\\boot.txt. Windower addons (.lua) go in addons\\windower, not here.\n"},
};

// README.txt in `folder`, unless it has one (the player's own, or one they changed)
void write_readme(const std::wstring& folder, const char* text)
{
    const std::wstring file = folder + L"\\README.txt";
    const HANDLE handle = CreateFileW(file.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (handle == INVALID_HANDLE_VALUE)
        return; // there already, or the folder can't be written: either way, nothing to do
    std::string out;
    for (const char* c = text; *c != '\0'; ++c) {
        if (*c == '\n')
            out += '\r';
        out += *c;
    }
    DWORD written = 0;
    const BOOL ok = WriteFile(handle, out.data(), static_cast<DWORD>(out.size()), &written, nullptr);
    CloseHandle(handle);
    if (ok == FALSE || written != out.size()) {
        DeleteFileW(file.c_str()); // not half a README
        return;
    }
    log::write(L"Folders: wrote %ls", file.c_str());
}

} // namespace

void install(const wchar_t* install_dir)
{
    const std::wstring root = std::wstring(install_dir) + L"\\";
    CreateDirectoryW((root + L"addons").c_str(), nullptr);
    CreateDirectoryW((root + L"plugins").c_str(), nullptr);
    for (const Folder& folder : kFolders) {
        const std::wstring path = root + folder.path;
        CreateDirectoryW(path.c_str(), nullptr);
        write_readme(path, folder.readme);
    }
}

} // namespace vanacore::folders

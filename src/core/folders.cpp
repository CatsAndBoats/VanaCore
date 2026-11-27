#include "folders.hpp"

#include <string>

#include "folders_make.h"
#include "log.hpp"

namespace vanacore::folders {

void install(const wchar_t* install_dir)
{
    make(install_dir, [](const std::wstring& file) { log::write(L"Folders: wrote %ls", file.c_str()); });
}

} // namespace vanacore::folders

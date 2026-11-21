#pragma once

#include <string>

// [client] window.image: a picture (.png, .jpg, .bmp, .gif: anything WIC reads) drawn in place of the game's window
// backgrounds, with their opacity (window.opacity); a path, from VanaCore's folder when it isn't a whole one (/vana's Browse
// copies the picture into <VanaCore>\images\ and sets images\<name>). [client] window.image.fit: how each window shows it,
// fill (scaled to cover it, the rest cropped; the default), stretch, tile (at its own pixels) or screen (one picture over the
// whole interface, every window showing the part behind it). The chat windows take it with chat.match=1, the chat line with
// input.match=windows (or chat, the chat windows taking it). The drawing is d3d8_winimage.h's.
namespace vanacore::winimage {

// `ini` (the settings file) and VanaCore's folder: the settings read now, and again by reload whenever the file changes.
void install(const wchar_t* ini, const wchar_t* install_dir);
void reload(const wchar_t* ini);

// A picture picked from anywhere (/vana's Browse, vana.ui.add_image): copied into <VanaCore>\images\ (over a file of that
// name there) once it's known to decode. Its path for [client] window.image into `kept` (images\<name>); false, and why, when
// it can't be. The picture is read again if window.image is that path already.
bool add(const std::wstring& path, std::wstring& kept, std::wstring& why);

} // namespace vanacore::winimage

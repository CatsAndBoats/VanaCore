#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <string>

// vanacore:// links, outside the game (addonpkg_win.cpp): vanacore://install/<https URL> from a browser names a package
// on the web, downloaded here and then asked about as a double-clicked .vcaddon is. WinHTTP is loaded only here.
namespace vanacore::addonpkg {
// `text` begins vanacore: (any case)
bool is_link(const std::wstring& text);
// The https URL a link names: vanacore://install/<URL>, the URL as it is or percent-encoded, or
// vanacore://install?url=<percent-encoded URL>. Empty when it names none.
std::wstring link_target(const std::wstring& link);
// The URL's file name, kept to what's safe in a file name (package.vcaddon when it has none)
std::wstring download_name(const std::wstring& url);
// %TEMP%\VanaCore\<this process>\, made: where a link's package waits while it's asked about
std::wstring download_dir();
// Downloads `url` to `path` (TLS 1.2 and up, redirects never from https to http, 200 only, at most what a package can
// be), `progress(got, total)` as it comes (total 0 when the site doesn't say), stopping when `cancel` is set, and marks
// it as downloaded from `url` (Windows' Zone.Identifier, which the card's From shows). Empty when it's all there, else
// why not ("Cancelled" when cancelled), and no file.
std::wstring download(const std::wstring& url, const std::wstring& path, const std::atomic<bool>& cancel,
                      const std::function<void(std::uint64_t, std::uint64_t)>& progress);
} // namespace vanacore::addonpkg

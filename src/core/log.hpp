#pragma once

#include <string>

namespace vanacore::log {

// Opens <install_dir>\logs\vanacore.log for appending (shared with VanaCore.exe), and turns logging on: until this is
// called ([logging] enabled=1, on by default) write() does nothing at all - no file, no OutputDebugString, no console
// line, no tap. Safe to call more than once.
void open(const wchar_t* install_dir);

// Remembers VanaCore's folder (where logs\ is) without turning logging on, so set_enabled() can later.
void install(const wchar_t* install_dir);

// Turns logging on or off while the game runs (/vana logs, or [logging] enabled changed in vanacore.ini): on opens
// logs\vanacore.log and writes from here on, off writes one last line and closes it. What starts with the game stays as it
// started (crash dumps, the DLSS helper's log, [logging] frametimes) until the next start. False if there's no folder to
// open it in.
bool set_enabled(bool on);

// Whether logging is on (open() has been called). What writes its own files (the DLSS helper's log, crash dumps, a
// frame recording) asks this first.
bool enabled();

// From now on, also prints each line to the game's console, which VanaCore.exe shares (attached=1).
void mirror_to_console();

// printf-style; each line is timestamped, written straight to disk, and also sent to
// OutputDebugString.
void write(const wchar_t* format, ...);

// write() for crash handlers: doesn't wait for a lock the crashed thread may hold.
void write_from_crash(const wchar_t* format, ...);
// Each line written from now on (its text, without the timestamp) also to `tap`, from whichever thread writes it;
// nullptr: no more. For /vana debug's recording.
void set_tap(void (*tap)(const std::wstring& line));

// The local time as file names carry it: "2026-10-05-142300".
std::wstring file_stamp();

// A report of its own, written whether or not logging is on ([logging] enabled): what the player asked for with
// /vana debug and /vana abtest. <logs_dir>\<name>-<stamp>.log (UTF-8; a line a write(), timestamped, straight to
// disk). It's not the log: nothing goes to OutputDebugString, the console or the tap, and [logging] enabled leaves it
// alone.
class Report {
public:
    Report() = default;
    Report(const Report&) = delete;
    Report& operator=(const Report&) = delete;
    ~Report();

    // Makes the file (and `logs_dir`, with its trailing backslash, if it isn't there). False if it can't be made.
    bool open(const std::wstring& logs_dir, const std::wstring& name, const std::wstring& stamp);
    bool is_open() const { return file_ != nullptr; }
    void write(const std::string& line); // UTF-8
    void close();
    const std::wstring& path() const { return path_; }

private:
    void* file_ = nullptr; // a HANDLE
    std::wstring path_;
};

} // namespace vanacore::log

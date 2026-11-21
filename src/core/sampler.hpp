#pragma once

#include <string>
#include <vector>

// A sampling profiler for the game's render thread (the thread that draws a frame, and does most of the game's work in
// it): a thread of its own looks at where it is about every millisecond (suspends it, reads its instruction pointer and
// the top of its stack, lets it go: a few microseconds each). From the stack it finds the calls the thread is inside of
// - the return addresses in FFXiMain.dll, taken as calls where a call instruction sits just before them and, for a
// direct call, a function starts where it goes - so each sample is a path of functions, from the game's main loop
// down to where the thread is. /vana stats shows them as a call tree with the time each function and all it calls took
// (inclusive, as Unreal's profilers do), by function, by what it's labelled as, and by module.
//
// The functions come from a table of FFXiMain.dll's function starts (ffximain_functions.bin in VanaCore's
// addons/vanacore/vana/data folder, from the Ghidra export), and are named by ffximain_labels.txt there where a line
// names them (one per line: the address as in the Ghidra export, hexadecimal, then the name, and ` | group` after it
// to count what's inside it under a heading: "10086020 Nameplates | Interface"; # starts a comment). A sample belongs
// to the group of the innermost labelled function it's inside of, so labelling the few functions that run a system
// (animation, physics, effects) makes the view say what each costs.
//
// Off until /vana stats asks (Record Stats Report): it costs a little FPS while it runs.
namespace vanacore::sampler {

// From VanaCoreInstall: VanaCore's folder, where data\ is looked for (under addons\vanacore\vana\).
void install(const wchar_t* install_dir);

// Starts looking at the calling thread (the render thread): a recording, the last one's samples dropped. Or stops, which
// keeps them (result() still has them) until the next start.
bool start();
void stop();
bool running();

// A frame, by QueryPerformanceCounter: when it started and ended
struct Span {
    long long start = 0, end = 0;
};

// The samples taken before `until` (by QueryPerformanceCounter) and not yet taken, added to the recording; those taken
// inside one of the `slow` frames (in time order) to the slow frames' own as well. On the thread that started it.
// `dip`: the frames under 15 fps among them, whose samples (this call's only) dip() has.
void take(const std::vector<Span>& slow, long long until, const std::vector<Span>& dip = {});

// Where the time went over the recording so far, as shares of the samples taken: all of them, or the slow frames'.
struct Share {
    std::string name; // a module, a heading, or a function
    double share = 0; // 0-1 of all the samples
    unsigned samples = 0;
    std::vector<Share> children;
};
struct Result {
    unsigned samples = 0;
    bool have_functions = false; // the table of function starts was found: functions, not just places
    Share tree;                  // the call tree (FFXiMain.dll's functions, outermost first), each with all it calls
    std::vector<Share> groups;   // by the innermost labelled function the thread was inside of; the rest as "(not labelled yet)"
    std::vector<Share> hot;      // the functions the thread was in itself (self time), most first
    std::vector<Share> modules;  // by module the thread was in
};
Result result(bool slow = false);
// The last take()'s dip frames' samples alone.
Result dip();
// All of the last take()'s samples: one look's, for how the thread's time went over the recording.
Result look();

} // namespace vanacore::sampler

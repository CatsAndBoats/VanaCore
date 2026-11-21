#pragma once

#include <windows.h>

#include <cstdint>

// Windower 4's plugin interface (GetInterfaceVersion() == 0x04070300), which Windower never
// published: the slot order and arguments here come from Hook.dll's classes (their RTTI vtables and
// what each slot reads and writes) and from how LuaCore.dll and the plugins call them. The member
// names are VanaCore's.
//
// Every method is a virtual __stdcall member (`this` passed on the stack) except the scalar
// deleting destructors, which are __thiscall. Names are all different on purpose: MSVC groups
// overloaded virtuals in its own order.
namespace vanacore::windower::abi {

inline constexpr std::uint32_t kInterfaceVersion = 0x04070300;

// What GetSettings() returns: Windower's settings (its launcher's profile) as the plugin sees
// them. WindowerPath is where plugins find plugins\settings\ and their own folders.
struct Settings {
    char ProfileName[256];
    char Branch[32];
    char Pad0[16];
    char LauncherVersion[16];
    char HookVersion[16];
    char PlayOnlinePath[4096];
    char FfxiPath[4096];
    char WindowerPath[4096];
    char ConsoleKey[16];
    std::uint32_t Region;
    int Width;
    int Height;
    int UIWidth;
    int UIHeight;
    int XPosition;
    int YPosition;
    bool Fullscreen;
    bool Borderless;
    bool Debug;
    bool Loaded;
    bool AlwaysEnableGamepad;
    bool AllowWinKey;
    std::uint8_t Pad1[2];
};
static_assert(offsetof(Settings, WindowerPath) == 0x2150);

// Windower's console: VanaCore has none, so its lines go to the chat log.
class Console {
public:
    virtual void __stdcall Open(bool open) = 0;
    virtual bool __stdcall IsVisible() = 0;
    virtual void __stdcall SetPosition(float x, float y) = 0;
    virtual void __stdcall Write(const char* text) = 0;
    virtual void __stdcall Clear() = 0;
    virtual void __stdcall SendCommand(const char* command, bool echo) = 0;
};

// A text box on the screen. Text is UTF-8, "\cs(r,g,b)" colours what follows and "\cr" resets it.
// Colours are (alpha, red, green, blue); sizes in points.
class TextObject {
public:
    virtual void __stdcall SetText(const char* text) = 0;                                  // 0
    virtual void __stdcall SetVisibility(bool visible) = 0;                                // 1
    virtual void __stdcall SetFonts(std::uint32_t count, const char** fonts) = 0;          // 2: first installed wins
    virtual void __stdcall SetFont(const char* font) = 0;                                  // 3
    virtual void __stdcall SetFontSize(float points) = 0;                                  // 4
    virtual void __stdcall SetFontWeight(int weight) = 0;                                  // 5: 400 normal, 700 bold
    virtual void __stdcall SetColor(std::uint8_t a, std::uint8_t r, std::uint8_t g, std::uint8_t b) = 0; // 6
    virtual void __stdcall SetStrokeWidth(float width) = 0;                                // 7
    virtual void __stdcall SetStrokeColor(std::uint8_t a, std::uint8_t r, std::uint8_t g, std::uint8_t b) = 0; // 8
    virtual void __stdcall SetExtents(float width, float height) = 0;                      // 9: fixed box size; negative: fit the text
    virtual void __stdcall SetLocation(float x, float y) = 0;                              // 10
    virtual void __stdcall GetLocation(float* x, float* y) = 0;                            // 11
    virtual void __stdcall GetExtents(float* width, float* height) = 0;                    // 12: the box's size
    virtual void __stdcall GetTextExtents(float* width, float* height) = 0;                // 13
    virtual void __stdcall SetBgColor(std::uint8_t a, std::uint8_t r, std::uint8_t g, std::uint8_t b) = 0; // 14
    virtual void __stdcall SetBgVisibility(bool visible) = 0;                              // 15
    virtual void __stdcall SetFontAndSize(const char* font, int points) = 0;               // 16
    virtual void __stdcall SetBold(bool bold) = 0;                                         // 17
    virtual void __stdcall SetItalic(bool italic) = 0;                                     // 18
    virtual void __stdcall SetRightJustified(bool right) = 0;                              // 19: x is the right edge
    virtual void __stdcall SetBgBorderSize(float size) = 0;                                // 20: padding on every side
};

class TextHandler {
public:
    virtual TextObject* __stdcall Create(const char* name) = 0;
    virtual TextObject* __stdcall CreateUnnamed() = 0; // Hook makes up its name
    virtual TextObject* __stdcall Get(const char* name) = 0;
    virtual void __stdcall DeleteObject(TextObject* object) = 0;
    virtual void __stdcall DeleteByName(const char* name) = 0;
};

// A rectangle on the screen: a colour, or an image (tinted by the colour) stretched or tiled over it.
class PrimitiveObject {
public:
    virtual void __stdcall SetVisibility(bool visible) = 0;                                // 0
    virtual void __stdcall SetColor(std::uint8_t a, std::uint8_t r, std::uint8_t g, std::uint8_t b) = 0; // 1
    virtual void __stdcall SetTexture(const char* path) = 0;                               // 2: empty/null: none
    virtual void __stdcall SetTextureFromResource(HMODULE module, const char* name) = 0;   // 3
    virtual void __stdcall SetRepeat(float x, float y) = 0;                                // 4: tiles across the rectangle
    virtual void __stdcall SetFitToTexture(bool fit) = 0;                                  // 5: size = the image's
    virtual void __stdcall SetSize(float width, float height) = 0;                         // 6
    virtual void __stdcall SetPosition(float x, float y) = 0;                              // 7
    virtual void __stdcall GetPosition(float* x, float* y) = 0;                            // 8
    virtual PrimitiveObject* __thiscall DeletingDestructor(std::uint8_t flags) = 0;        // 9
};

class PrimitiveHandler {
public:
    virtual PrimitiveObject* __stdcall Create(const char* name) = 0;
    virtual PrimitiveObject* __stdcall CreateUnnamed() = 0; // Hook makes up its name
    virtual PrimitiveObject* __stdcall Get(const char* name) = 0;
    virtual void __stdcall DeleteObject(PrimitiveObject* object) = 0;
    virtual void __stdcall DeleteByName(const char* name) = 0;
    virtual PrimitiveHandler* __thiscall DeletingDestructor(std::uint8_t flags) = 0;
};

// Packets are whole: the 4-byte header (id, size in 4-byte units, sequence) first.
class PacketStreamHandler {
public:
    virtual void __stdcall InjectOutgoing(const std::uint8_t* packet) = 0;
    virtual void __stdcall InjectIncoming(const std::uint8_t* packet) = 0;
    // The last packet with `id` seen going out (in), and when; its 0x208 bytes stay valid.
    virtual const std::uint8_t* __stdcall GetLastOutgoing(std::uint16_t id, std::uint64_t* time) = 0;
    virtual const std::uint8_t* __stdcall GetLastIncoming(std::uint16_t id, std::uint64_t* time) = 0;
    // Windower's game state, kept from the server's packets: 14 pointers into it (logged in and
    // zone, server clock, player, merits, key items, items, treasure pool, equipment, party, job
    // extra data, equipment bags, job points, missions, jobs). See state.cpp.
    virtual const void* const* __stdcall GetStreamState() = 0;
};

class FFXI {
public:
    virtual void __stdcall AddToChat(const char* text, int mode) = 0;
};

class PluginManager {
public:
    virtual Settings* __stdcall GetSettings() = 0;
    virtual HWND __stdcall GetHWND() = 0;
    virtual void* __stdcall GetDirect3DDevice() = 0;
    virtual Console* __stdcall GetConsole() = 0;
    virtual TextHandler* __stdcall GetTextHandler() = 0;
    virtual PrimitiveHandler* __stdcall GetPrimitiveHandler() = 0;
    virtual PacketStreamHandler* __stdcall GetPacketStreamHandler() = 0;
    virtual FFXI* __stdcall GetFFXI() = 0;
    virtual PluginManager* __thiscall DeletingDestructor(std::uint8_t flags) = 0;
};

// The plugin, as CreateInstance() makes it.
class Plugin {
public:
    virtual const char* __stdcall GetAuthor() = 0;                                         // 0
    virtual const char* __stdcall GetName() = 0;                                           // 1
    virtual void __stdcall Load(PluginManager* manager) = 0;                               // 2
    virtual void __stdcall Unload() = 0;                                                   // 3: and delete itself
    virtual bool __stdcall IgnoreUnload() = 0;                                             // 4
    virtual void __stdcall PreRender() = 0;                                                // 5
    virtual void __stdcall PostRender() = 0;                                               // 6
    // "//<name> args" typed: " args" (FFXIDB skips one leading space).
    virtual void __stdcall PluginCommand(const char* args) = 0;                            // 7
    // Any other command line no one handled, without its "//". True: handled.
    virtual bool __stdcall UnhandledCommand(const char* line) = 0;                         // 8
    virtual void __stdcall IncomingText(void* a, void* b, void* c) = 0;                    // 9
    virtual void __stdcall OutgoingText(void* a, void* b, void* c) = 0;                    // 10
    // Each packet: as it came (went), as it is now (0x200 bytes are read from both), whether a
    // plugin added it, whether it's blocked already. True: block it.
    virtual bool __stdcall IncomingChunk(const std::uint8_t* original, std::uint8_t* modified, bool injected, bool blocked) = 0; // 11
    virtual bool __stdcall OutgoingChunk(const std::uint8_t* original, std::uint8_t* modified, bool injected, bool blocked) = 0; // 12
    // Windower's mouse event: type = the window message - 0x200 (0 move, 1 left down, 2 left up,
    // 10 wheel, ...), where (screen pixels), the wheel's turn. True: the game doesn't see it.
    virtual bool __stdcall Mouse(int type, int x, int y, int delta, bool blocked) = 0;     // 13
    // VanaCore passes the key's DirectInput code and 1 down, 0 up (as Windower addons' 'keyboard' event has them; the
    // plugins seen so far ignore them). True: block it.
    virtual bool __stdcall Keyboard(void* a, void* b, bool blocked) = 0;                   // 14
    virtual void __stdcall AddItem(void* a, void* b, void* c, void* d) = 0;                // 15
    virtual void __stdcall RemoveItem(void* a, void* b, void* c, void* d) = 0;             // 16
    virtual Plugin* __thiscall DeletingDestructor(std::uint8_t flags) = 0;                 // 17
};

using GetInterfaceVersion_f = std::uint32_t (*)();
using CreateInstance_f = Plugin* (*)();

// Windower 3's plugin interface (GetInterfaceVersion() 0x0004xxxx), which Windower 4 refuses ("out
// of date"). Only what StatusTimer uses is known, read from its code: the plugin's slots (their
// argument counts from each slot's `ret`), and the host objects' slots it calls. Everything is
// __stdcall with `this` on the stack except the destructor.
namespace legacy {

inline bool is_version(std::uint32_t version)
{
    return (version >> 16) == 0x0004;
}

// What GetInfo fills: the author at +0x100, the commands the plugin answers to at +0x308
// ("st|statustimer").
struct Info {
    char unknown0[0x100];
    char author[0x100];
    char unknown1[0x108];
    char commands[0x20];
    char unknown2[0xCD8]; // room to spare
};
static_assert(offsetof(Info, commands) == 0x308);

// The settings host slot 1 copies into the caller's block (0x10048 bytes) and returns: Windower's
// folder at +0x4000 (wide); +0x10004 non-zero for English (StatusTimer then takes names from the
// resources' element text, as Windower 4's files have them), zero for Japanese.
struct Settings {
    char unknown0[0x4000];
    wchar_t WindowerPath[0x100]; // wide: StatusTimer converts it with wcstombs
    char unknown1[0xBE04];
    std::uint32_t English;
};
static_assert(offsetof(Settings, English) == 0x10004);

class Host;

class Plugin {
public:
    virtual void __stdcall Load(Host* host) = 0;                                          // 0
    virtual void __stdcall Unload() = 0;                                                  // 1
    virtual void __stdcall Release() = 0;                                                 // 2: deletes itself
    virtual Info* __stdcall GetInfo(Info* info) = 0;                                      // 3
    virtual void __stdcall Command(const char* args, int unknown) = 0;                    // 4: " args", as PluginCommand
    virtual bool __stdcall UnhandledCommand(const char* line) = 0;                        // 5
    virtual void __stdcall Event6() = 0;                                                  // 6
    virtual void __stdcall Event7() = 0;                                                  // 7
    virtual void __stdcall Frame() = 0;                                                   // 8: each frame (StatusTimer draws here)
    virtual void __stdcall Event9() = 0;                                                  // 9
    virtual void __stdcall IncomingText(void* a, void* b, void* c) = 0;                   // 10
    virtual void __stdcall OutgoingText(void* a, void* b, void* c) = 0;                   // 11
    virtual bool __stdcall IncomingChunk(const std::uint8_t* original, std::uint8_t* modified, bool injected, bool blocked) = 0; // 12
    virtual bool __stdcall OutgoingChunk(const std::uint8_t* original, std::uint8_t* modified, bool injected, bool blocked) = 0; // 13
};

using CreateInstance_f = Plugin* (*)();

} // namespace legacy

} // namespace vanacore::windower::abi

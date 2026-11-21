// The host objects Windower 3 plugins (interface 0x0004xxxx, such as StatusTimer) are given. Only
// the slots StatusTimer calls are known (abi.hpp, legacy); any other slot logs that it was called
// and returns, taking no arguments. Their text objects are Windower 4 text objects underneath
// (objects.cpp), so they draw the same way.

#include <windows.h>

#include <algorithm>
#include <cstring>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "../../log.hpp"
#include "objects.hpp"

namespace vanacore::windower {
namespace {

namespace legacy = abi::legacy;

std::mutex g_unknown_lock;
std::set<std::string> g_unknown;

void unknown(const char* object, int slot)
{
    char key[64];
    sprintf_s(key, "%s slot %d", object, slot);
    {
        const std::lock_guard lock(g_unknown_lock);
        if (!g_unknown.insert(key).second)
            return;
    }
    const std::string caller(calling());
    log::write(L"Windower plugins: %hs called Windower 3's %hs, which VanaCore doesn't know (it did nothing)",
               caller.empty() ? "a plugin" : caller.c_str(), key);
}

// A slot nothing is known about: `this` only.
#define UNKNOWN(object, n) \
    virtual void __stdcall Unknown##n() { unknown(object, n); }

class LegacyText {
public:
    // No virtual destructor: every virtual here is a slot, in order.
    LegacyText(std::string name, std::string owner, abi::TextObject* inner) : name(std::move(name)), owner(std::move(owner)), inner(inner) {}

    UNKNOWN("TextObject", 0)
    virtual void __stdcall SetText(const char* text) { inner->SetText(text); }                  // 1
    virtual void __stdcall SetVisibility(bool visible) { inner->SetVisibility(visible); }       // 2
    UNKNOWN("TextObject", 3)
    UNKNOWN("TextObject", 4)
    UNKNOWN("TextObject", 5)
    UNKNOWN("TextObject", 6)
    UNKNOWN("TextObject", 7)
    UNKNOWN("TextObject", 8)
    virtual void __stdcall SetColor(std::uint8_t a, std::uint8_t r, std::uint8_t g, std::uint8_t b) { inner->SetColor(a, r, g, b); } // 9
    UNKNOWN("TextObject", 10)
    UNKNOWN("TextObject", 11)
    UNKNOWN("TextObject", 12)
    virtual void __stdcall SetLocation(float x, float y) { inner->SetLocation(x, y); }          // 13
    UNKNOWN("TextObject", 14)
    UNKNOWN("TextObject", 15)
    UNKNOWN("TextObject", 16)
    UNKNOWN("TextObject", 17)
    UNKNOWN("TextObject", 18)
    UNKNOWN("TextObject", 19)
    virtual void __stdcall SetBgColor(std::uint8_t a, std::uint8_t r, std::uint8_t g, std::uint8_t b) { inner->SetBgColor(a, r, g, b); } // 20
    virtual void __stdcall SetBgVisibility(bool visible) { inner->SetBgVisibility(visible); }   // 21
    UNKNOWN("TextObject", 22)
    UNKNOWN("TextObject", 23)
    UNKNOWN("TextObject", 24)
    UNKNOWN("TextObject", 25)
    UNKNOWN("TextObject", 26)
    UNKNOWN("TextObject", 27)
    virtual void __stdcall SetFont(const char* font, int points) { inner->SetFontAndSize(font, points); } // 28
    virtual void __stdcall SetBold(bool bold) { inner->SetBold(bold); }                         // 29
    virtual void __stdcall SetItalic(bool italic) { inner->SetItalic(italic); }                 // 30
    UNKNOWN("TextObject", 31)
    virtual void __stdcall SetBgBorderSize(float size) { inner->SetBgBorderSize(size); }        // 32
    UNKNOWN("TextObject", 33)
    UNKNOWN("TextObject", 34)
    UNKNOWN("TextObject", 35)
    UNKNOWN("TextObject", 36)
    UNKNOWN("TextObject", 37)
    UNKNOWN("TextObject", 38)
    UNKNOWN("TextObject", 39)

    std::string name;
    std::string owner;
    abi::TextObject* inner;
};

std::mutex g_texts_lock;
std::vector<std::unique_ptr<LegacyText>> g_texts;

class LegacyTextHandler {
public:
    virtual LegacyText* __stdcall Create(const char* name) // 0
    {
        if (name == nullptr)
            return nullptr;
        const std::lock_guard lock(g_texts_lock);
        for (auto& text : g_texts)
            if (_stricmp(text->name.c_str(), name) == 0)
                return text.get();
        abi::TextObject* inner = manager()->GetTextHandler()->Create(name);
        if (inner == nullptr)
            return nullptr;
        g_texts.push_back(std::make_unique<LegacyText>(name, std::string(calling()), inner));
        return g_texts.back().get();
    }
    UNKNOWN("TextHandler", 1)
    UNKNOWN("TextHandler", 2)
    UNKNOWN("TextHandler", 3)
    UNKNOWN("TextHandler", 4)
    virtual void __stdcall DeleteByName(const char* name) // 5
    {
        if (name == nullptr)
            return;
        const std::lock_guard lock(g_texts_lock);
        for (auto it = g_texts.begin(); it != g_texts.end(); ++it) {
            if (_stricmp((*it)->name.c_str(), name) == 0) {
                manager()->GetTextHandler()->DeleteObject((*it)->inner);
                g_texts.erase(it);
                return;
            }
        }
    }
    UNKNOWN("TextHandler", 6)
    UNKNOWN("TextHandler", 7)
    UNKNOWN("TextHandler", 8)
    UNKNOWN("TextHandler", 9)
};

class LegacyConsole {
public:
    UNKNOWN("Console", 0)
    UNKNOWN("Console", 1)
    UNKNOWN("Console", 2)
    UNKNOWN("Console", 3)
    virtual void __stdcall Write(const char* text) // 4
    {
        if (text != nullptr)
            say(calling(), console_text(text));
    }
    UNKNOWN("Console", 5)
    UNKNOWN("Console", 6)
    UNKNOWN("Console", 7)
    UNKNOWN("Console", 8)
    UNKNOWN("Console", 9)
};

class LegacyFFXI {
public:
    UNKNOWN("FFXI", 0)
    UNKNOWN("FFXI", 1)
    UNKNOWN("FFXI", 2)
    UNKNOWN("FFXI", 3)
    // Windower's game state: the same table PacketStreamHandler::GetStreamState returns.
    virtual const void* const* __stdcall GetPointers() { return state_pointers(); } // 4
    UNKNOWN("FFXI", 5)
    UNKNOWN("FFXI", 6)
    UNKNOWN("FFXI", 7)
    UNKNOWN("FFXI", 8)
    UNKNOWN("FFXI", 9)
};

LegacyTextHandler g_text_handler;
LegacyConsole g_console;
LegacyFFXI g_ffxi;

class LegacyHost {
public:
    UNKNOWN("Host", 0)
    // Copies the settings into the caller's block (0x10048 bytes in StatusTimer) and returns it.
    virtual legacy::Settings* __stdcall GetSettings(legacy::Settings* out) // 1
    {
        if (out == nullptr)
            return nullptr;
        std::memset(out, 0, sizeof(*out));
        MultiByteToWideChar(CP_ACP, 0, install_path().c_str(), -1, out->WindowerPath, static_cast<int>(std::size(out->WindowerPath)) - 1);
        out->English = 1;
        return out;
    }
    UNKNOWN("Host", 2)
    UNKNOWN("Host", 3)
    UNKNOWN("Host", 4)
    virtual LegacyConsole* __stdcall GetConsole() { return &g_console; }             // 5
    virtual LegacyTextHandler* __stdcall GetTextHandler() { return &g_text_handler; } // 6
    UNKNOWN("Host", 7)
    virtual LegacyFFXI* __stdcall GetFFXI() { return &g_ffxi; }                      // 8
    UNKNOWN("Host", 9)
    UNKNOWN("Host", 10)
    UNKNOWN("Host", 11)
    UNKNOWN("Host", 12)
    UNKNOWN("Host", 13)
    UNKNOWN("Host", 14)
    UNKNOWN("Host", 15)
};

LegacyHost g_host;

} // namespace

abi::legacy::Host* legacy_host()
{
    return reinterpret_cast<abi::legacy::Host*>(&g_host);
}

void legacy_release(std::string_view owner)
{
    if (owner.empty())
        return;
    const std::lock_guard lock(g_texts_lock);
    std::erase_if(g_texts, [&](const auto& text) { return text->owner == owner; });
}

} // namespace vanacore::windower

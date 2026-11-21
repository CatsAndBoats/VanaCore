// The objects Windower plugins are given: the plugin manager and what it hands out (console, text,
// primitives, packets, chat), over VanaCore's chat, text renderer, images and packet handling.

#include "objects.hpp"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "../../chat.hpp"
#include "../../dats.hpp"
#include "../../image.hpp"
#include "../../log.hpp"
#include "../../packets.hpp"
#include "../../text.hpp"
#include "../../ui.hpp"
#include "../../window.hpp"
#include "host.hpp"

namespace vanacore::windower {
namespace {

std::recursive_mutex g_lock; // text and primitives: plugins may call from their own threads
void* g_device = nullptr;
std::uint64_t g_order = 0; // creation order, which is drawing order

std::wstring wide(std::string_view text, UINT code_page = CP_UTF8)
{
    if (text.empty())
        return {};
    const DWORD flags = code_page == CP_UTF8 ? MB_ERR_INVALID_CHARS : 0;
    int length = MultiByteToWideChar(code_page, flags, text.data(), static_cast<int>(text.size()), nullptr, 0);
    if (length == 0 && code_page == CP_UTF8)
        return wide(text, CP_ACP); // not UTF-8 after all
    std::wstring out(static_cast<size_t>(length), L'\0');
    MultiByteToWideChar(code_page, flags, text.data(), static_cast<int>(text.size()), out.data(), length);
    return out;
}

std::string text_of(const char* text)
{
    return text != nullptr ? std::string(text) : std::string();
}

std::uint32_t argb(std::uint8_t a, std::uint8_t r, std::uint8_t g, std::uint8_t b)
{
    return static_cast<std::uint32_t>(a) << 24 | static_cast<std::uint32_t>(r) << 16 | static_cast<std::uint32_t>(g) << 8 | b;
}

bool same_name(const std::string& a, const char* b)
{
    return b != nullptr && _stricmp(a.c_str(), b) == 0;
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// Text

class Text final : public abi::TextObject {
public:
    std::string name;
    std::string owner;
    std::uint64_t order = 0;

    void __stdcall SetText(const char* text) override { set(text_, text_of(text)); }
    void __stdcall SetVisibility(bool visible) override { set(visible_, visible); }
    void __stdcall SetFonts(std::uint32_t count, const char** fonts) override
    {
        const std::lock_guard lock(g_lock);
        std::vector<std::string> list;
        for (std::uint32_t i = 0; fonts != nullptr && i < count && i < 16; ++i)
            if (fonts[i] != nullptr && fonts[i][0] != '\0')
                list.emplace_back(fonts[i]);
        if (list.empty())
            return;
        // The first one installed (Windows would stand another font in for a missing one); with
        // one, no need to ask.
        std::string chosen = list.front();
        if (list.size() > 1) {
            for (const std::string& font : list) {
                text::Style probe;
                probe.family = wide(font);
                if (_wcsicmp(text::installed_face(probe).c_str(), probe.family.c_str()) == 0) {
                    chosen = font;
                    break;
                }
            }
        }
        set(font_, chosen);
    }
    void __stdcall SetFont(const char* font) override { SetFonts(1, &font); }
    void __stdcall SetFontSize(float points) override
    {
        if (points > 0.0f && points < 500.0f)
            set(points_, points);
    }
    void __stdcall SetFontWeight(int weight) override { set(weight_, std::clamp(weight, 100, 900)); }
    void __stdcall SetColor(std::uint8_t a, std::uint8_t r, std::uint8_t g, std::uint8_t b) override { set(color_, argb(a, r, g, b)); }
    void __stdcall SetStrokeWidth(float width) override { set(stroke_width_, std::clamp(width, 0.0f, 16.0f)); }
    void __stdcall SetStrokeColor(std::uint8_t a, std::uint8_t r, std::uint8_t g, std::uint8_t b) override
    {
        set(stroke_color_, argb(a, r, g, b));
    }
    void __stdcall SetExtents(float width, float height) override
    {
        const std::lock_guard lock(g_lock);
        fixed_width_ = width;
        fixed_height_ = height;
    }
    void __stdcall SetLocation(float x, float y) override
    {
        const std::lock_guard lock(g_lock);
        x_ = x;
        y_ = y;
    }
    void __stdcall GetLocation(float* x, float* y) override
    {
        const std::lock_guard lock(g_lock);
        if (x != nullptr)
            *x = x_;
        if (y != nullptr)
            *y = y_;
    }
    void __stdcall GetExtents(float* width, float* height) override
    {
        const std::lock_guard lock(g_lock);
        refresh();
        if (width != nullptr)
            *width = fixed_width_ >= 0.0f ? fixed_width_ : static_cast<float>(sprite_.width());
        if (height != nullptr)
            *height = fixed_height_ >= 0.0f ? fixed_height_ : static_cast<float>(sprite_.height());
    }
    void __stdcall GetTextExtents(float* width, float* height) override
    {
        const std::lock_guard lock(g_lock);
        refresh();
        if (width != nullptr)
            *width = static_cast<float>(sprite_.width());
        if (height != nullptr)
            *height = static_cast<float>(sprite_.height());
    }
    void __stdcall SetBgColor(std::uint8_t a, std::uint8_t r, std::uint8_t g, std::uint8_t b) override { set(background_, argb(a, r, g, b)); }
    void __stdcall SetBgVisibility(bool visible) override { set(background_visible_, visible); }
    void __stdcall SetFontAndSize(const char* font, int points) override
    {
        const std::lock_guard lock(g_lock);
        SetFonts(1, &font);
        SetFontSize(static_cast<float>(points));
    }
    void __stdcall SetBold(bool bold) override { SetFontWeight(bold ? FW_BOLD : FW_NORMAL); }
    void __stdcall SetItalic(bool italic) override { set(italic_, italic); }
    void __stdcall SetRightJustified(bool right) override { set(right_, right); }
    void __stdcall SetBgBorderSize(float size) override { set(padding_, std::clamp(size, 0.0f, 256.0f)); }

    void draw(void* device)
    {
        if (!visible_ || text_.empty())
            return;
        refresh();
        if (!sprite_.upload(device))
            return;
        // Right-justified: x is the right edge.
        const float x = right_ ? x_ - static_cast<float>(sprite_.width()) : x_;
        sprite_.draw(device, static_cast<int>(std::lround(x)), static_cast<int>(std::lround(y_)));
    }
    void release() { sprite_.release(); }

private:
    template <typename T>
    void set(T& field, const T& value)
    {
        const std::lock_guard lock(g_lock);
        if (field == value)
            return;
        field = value;
        dirty_ = true;
    }

    // Renders the text if anything changed, so its size is known.
    void refresh()
    {
        // Points at 96 dpi, times the display's scale: on a 150% display Windower's text was stretched with
        // the window, and VanaCore's game draws at the display's own pixels.
        const float scale = window::dpi_scale();
        if (!dirty_ && scale == scale_)
            return;
        scale_ = scale;
        text::Style style;
        style.family = wide(font_);
        style.size = std::max(4, static_cast<int>(std::lround(points_ * 4.0f / 3.0f * scale)));
        style.weight = weight_;
        style.italic = italic_;
        text::argb_colour(color_, style.color);
        style.stroke = static_cast<int>(std::lround(stroke_width_));
        text::argb_colour(stroke_color_, style.stroke_color);
        text::argb_colour(background_, style.background);
        if (!background_visible_)
            style.background[3] = 0.0f;
        style.padding = static_cast<int>(std::lround(padding_));
        style.align_right = right_;
        style.color_codes = true;
        sprite_.set(wide(text_), style);
        dirty_ = false;
    }

    std::string text_;
    bool visible_ = true;
    std::string font_ = "Arial";
    float points_ = 12.0f;
    float scale_ = 1.0f; // window::dpi_scale() as last drawn
    int weight_ = FW_NORMAL;
    bool italic_ = false;
    std::uint32_t color_ = 0xFFFFFFFF;
    float stroke_width_ = 0.0f;
    std::uint32_t stroke_color_ = 0xFF000000;
    float fixed_width_ = -1.0f;
    float fixed_height_ = -1.0f;
    float x_ = 0.0f;
    float y_ = 0.0f;
    std::uint32_t background_ = 0xFF000000;
    bool background_visible_ = false;
    bool right_ = false;
    float padding_ = 0.0f;
    bool dirty_ = true;
    text::Sprite sprite_;
};

std::vector<std::unique_ptr<Text>> g_texts;

////////////////////////////////////////////////////////////////////////////////////////////////////
// Primitives

// Decoded images by file (or module resource), so a texture set again isn't decoded again.
std::map<std::wstring, std::shared_ptr<const image::Pixels>> g_images;

std::shared_ptr<const image::Pixels> cached(const std::wstring& key, bool (*decode)(const std::wstring&, void*, image::Pixels&),
                                            void* context)
{
    if (auto found = g_images.find(key); found != g_images.end())
        return found->second;
    auto pixels = std::make_shared<image::Pixels>();
    if (!decode(key, context, *pixels))
        pixels.reset();
    if (g_images.size() >= 256)
        g_images.clear();
    g_images[key] = pixels; // failures too: a missing file isn't looked for every frame
    return pixels;
}

class Primitive final : public abi::PrimitiveObject {
public:
    std::string name;
    std::string owner;
    std::uint64_t order = 0;

    void __stdcall SetVisibility(bool visible) override
    {
        const std::lock_guard lock(g_lock);
        visible_ = visible;
    }
    void __stdcall SetColor(std::uint8_t a, std::uint8_t r, std::uint8_t g, std::uint8_t b) override
    {
        const std::lock_guard lock(g_lock);
        color_ = argb(a, r, g, b);
    }
    void __stdcall SetTexture(const char* path) override
    {
        const std::lock_guard lock(g_lock);
        if (path == nullptr || path[0] == '\0') {
            texture_key_.clear();
            textured_ = false;
            texture_.set({});
            return;
        }
        const std::wstring file = redirect(wide(path, CP_ACP));
        if (textured_ && file == texture_key_)
            return;
        use(file, cached(file, [](const std::wstring& key, void*, image::Pixels& out) { return image::decode_file(key, out); }, nullptr));
    }
    void __stdcall SetTextureFromResource(HMODULE module, const char* resource) override
    {
        const std::lock_guard lock(g_lock);
        if (module == nullptr || resource == nullptr) {
            SetTexture(nullptr);
            return;
        }
        wchar_t key[96];
        if (IS_INTRESOURCE(resource))
            swprintf_s(key, L"res:%p:#%u", static_cast<void*>(module), static_cast<unsigned>(reinterpret_cast<uintptr_t>(resource)));
        else
            swprintf_s(key, L"res:%p:%.60hs", static_cast<void*>(module), resource);
        if (textured_ && texture_key_ == key)
            return;
        struct Source {
            HMODULE module;
            const char* name;
        } source{module, resource};
        use(key, cached(key,
                        [](const std::wstring&, void* context, image::Pixels& out) {
                            const auto* s = static_cast<const Source*>(context);
                            return image::decode_resource(s->module, s->name, out);
                        },
                        &source));
    }
    void __stdcall SetRepeat(float x, float y) override
    {
        const std::lock_guard lock(g_lock);
        repeat_x_ = x;
        repeat_y_ = y;
    }
    void __stdcall SetFitToTexture(bool fit) override
    {
        const std::lock_guard lock(g_lock);
        fit_ = fit;
        fit_size();
    }
    void __stdcall SetSize(float width, float height) override
    {
        const std::lock_guard lock(g_lock);
        width_ = width;
        height_ = height;
    }
    void __stdcall SetPosition(float x, float y) override
    {
        const std::lock_guard lock(g_lock);
        x_ = x;
        y_ = y;
    }
    void __stdcall GetPosition(float* x, float* y) override
    {
        const std::lock_guard lock(g_lock);
        if (x != nullptr)
            *x = x_;
        if (y != nullptr)
            *y = y_;
    }
    abi::PrimitiveObject* __thiscall DeletingDestructor(std::uint8_t flags) override;

    void draw(void* device)
    {
        if (!visible_)
            return;
        if (textured_) {
            if (!texture_.empty())
                texture_.draw(device, x_, y_, width_, height_, color_, repeat_x_, repeat_y_);
            return; // an image that wouldn't load: nothing (as a plain rectangle it would cover the screen in white)
        }
        text::fill(device, static_cast<int>(std::lround(x_)), static_cast<int>(std::lround(y_)), static_cast<int>(std::lround(width_)),
                   static_cast<int>(std::lround(height_)), color_);
    }
    void release() { texture_.release(); }

private:
    void use(const std::wstring& key, const std::shared_ptr<const image::Pixels>& pixels)
    {
        texture_key_ = key;
        textured_ = true;
        if (pixels == nullptr) {
            texture_.set({});
            log::write(L"Windower plugins: %hs couldn't use image %ls for primitive %hs", owner.c_str(), key.c_str(), name.c_str());
            return;
        }
        texture_.set(*pixels);
        fit_size();
    }
    void fit_size()
    {
        if (fit_ && textured_ && !texture_.empty()) {
            width_ = static_cast<float>(texture_.width());
            height_ = static_cast<float>(texture_.height());
        }
    }

    bool visible_ = true;
    std::uint32_t color_ = 0xFFFFFFFF;
    float x_ = 0.0f;
    float y_ = 0.0f;
    float width_ = 10.0f;
    float height_ = 10.0f;
    float repeat_x_ = 1.0f;
    float repeat_y_ = 1.0f;
    bool fit_ = false;
    bool textured_ = false;
    std::wstring texture_key_;
    image::Texture texture_;
};

std::vector<std::unique_ptr<Primitive>> g_primitives;

template <typename T>
T* find_named(std::vector<std::unique_ptr<T>>& list, const char* name)
{
    for (auto& object : list)
        if (same_name(object->name, name))
            return object.get();
    return nullptr;
}

template <typename T, typename Base>
bool erase(std::vector<std::unique_ptr<T>>& list, Base* object)
{
    for (auto it = list.begin(); it != list.end(); ++it) {
        if (static_cast<Base*>(it->get()) == object) {
            list.erase(it);
            return true;
        }
    }
    return false;
}

template <typename T>
T* create(std::vector<std::unique_ptr<T>>& list, const char* name, const char* kind)
{
    if (name == nullptr)
        return nullptr;
    if (T* existing = find_named(list, name)) {
        log::write(L"Windower plugins: %hs made %hs %hs, which exists already: it gets that one", std::string(calling()).c_str(),
                   kind, name);
        return existing;
    }
    auto object = std::make_unique<T>();
    object->name = name;
    object->owner = std::string(calling());
    object->order = ++g_order;
    list.push_back(std::move(object));
    return list.back().get();
}

// A name no plugin would pick, for an object made without one.
std::string unnamed()
{
    static unsigned next = 0;
    return "__vanacore_" + std::to_string(++next);
}

class TextHandlerImpl final : public abi::TextHandler {
public:
    abi::TextObject* __stdcall Create(const char* name) override
    {
        const std::lock_guard lock(g_lock);
        return create(g_texts, name, "text");
    }
    abi::TextObject* __stdcall CreateUnnamed() override
    {
        const std::lock_guard lock(g_lock);
        return create(g_texts, unnamed().c_str(), "text");
    }
    abi::TextObject* __stdcall Get(const char* name) override
    {
        const std::lock_guard lock(g_lock);
        return find_named(g_texts, name);
    }
    void __stdcall DeleteObject(abi::TextObject* object) override
    {
        const std::lock_guard lock(g_lock);
        erase(g_texts, object);
    }
    void __stdcall DeleteByName(const char* name) override
    {
        const std::lock_guard lock(g_lock);
        if (Text* text = find_named(g_texts, name))
            erase(g_texts, static_cast<abi::TextObject*>(text));
    }
};

class PrimitiveHandlerImpl final : public abi::PrimitiveHandler {
public:
    abi::PrimitiveObject* __stdcall Create(const char* name) override
    {
        const std::lock_guard lock(g_lock);
        return create(g_primitives, name, "primitive");
    }
    abi::PrimitiveObject* __stdcall CreateUnnamed() override
    {
        const std::lock_guard lock(g_lock);
        return create(g_primitives, unnamed().c_str(), "primitive");
    }
    abi::PrimitiveObject* __stdcall Get(const char* name) override
    {
        const std::lock_guard lock(g_lock);
        return find_named(g_primitives, name);
    }
    void __stdcall DeleteObject(abi::PrimitiveObject* object) override
    {
        const std::lock_guard lock(g_lock);
        erase(g_primitives, object);
    }
    void __stdcall DeleteByName(const char* name) override
    {
        const std::lock_guard lock(g_lock);
        if (Primitive* primitive = find_named(g_primitives, name))
            erase(g_primitives, static_cast<abi::PrimitiveObject*>(primitive));
    }
    abi::PrimitiveHandler* __thiscall DeletingDestructor(std::uint8_t) override { return this; } // VanaCore's own
};

abi::PrimitiveObject* __thiscall Primitive::DeletingDestructor(std::uint8_t)
{
    // A plugin deleting its primitive itself: as DeleteObject.
    const std::lock_guard lock(g_lock);
    erase(g_primitives, static_cast<abi::PrimitiveObject*>(this));
    return nullptr;
}

////////////////////////////////////////////////////////////////////////////////////////////////////
// Console, chat, packets

class ConsoleImpl final : public abi::Console {
public:
    void __stdcall Open(bool) override {}
    bool __stdcall IsVisible() override { return false; }
    void __stdcall SetPosition(float, float) override {}
    void __stdcall Write(const char* text) override
    {
        if (text != nullptr)
            say(calling(), console_text(text));
    }
    void __stdcall Clear() override {}
    // A Windower console command: "input <line>" runs a game command line; anything else is a
    // Windower command, typed as //<command>.
    void __stdcall SendCommand(const char* command, bool) override
    {
        if (command == nullptr)
            return;
        std::string line(command);
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t'))
            line.erase(line.begin());
        if (line.empty())
            return;
        if (_strnicmp(line.c_str(), "input ", 6) == 0)
            line.erase(0, 6);
        else if (line.front() != '/')
            line = "//" + line;
        chat::run(1, line);
    }
};

class FFXIImpl final : public abi::FFXI {
public:
    void __stdcall AddToChat(const char* text, int mode) override
    {
        if (text != nullptr)
            chat::write(mode > 0 && mode < 0x100 ? mode : chat::kDefaultMode, text,
                        calling().empty() ? std::string_view("Windower plugin") : calling());
    }
};

// Each packet id's last one seen, per direction, as Hook keeps them: a time, then 0x208 bytes.
struct LastPacket {
    std::uint64_t time = 0;
    std::uint8_t data[0x208] = {};
};
std::vector<LastPacket> g_last[2]; // [incoming]

std::uint16_t packet_size(const std::uint8_t* packet)
{
    const std::uint16_t head = static_cast<std::uint16_t>(packet[0] | packet[1] << 8);
    return static_cast<std::uint16_t>((head >> 9) * 4);
}

class PacketsImpl final : public abi::PacketStreamHandler {
public:
    void __stdcall InjectOutgoing(const std::uint8_t* packet) override { inject(false, packet); }
    void __stdcall InjectIncoming(const std::uint8_t* packet) override { inject(true, packet); }
    const std::uint8_t* __stdcall GetLastOutgoing(std::uint16_t id, std::uint64_t* time) override { return last(false, id, time); }
    const std::uint8_t* __stdcall GetLastIncoming(std::uint16_t id, std::uint64_t* time) override { return last(true, id, time); }
    const void* const* __stdcall GetStreamState() override { return state_pointers(); }

private:
    static void inject(bool incoming, const std::uint8_t* packet)
    {
        if (packet == nullptr)
            return;
        const std::uint16_t size = packet_size(packet);
        if (size < 4)
            return;
        packets::inject(incoming, static_cast<std::uint16_t>((packet[0] | packet[1] << 8) & 0x1FF), size, packet);
    }
    static const std::uint8_t* last(bool incoming, std::uint16_t id, std::uint64_t* time)
    {
        auto& table = g_last[incoming ? 1 : 0];
        if (table.empty())
            table.resize(0x200);
        const LastPacket& entry = table[id & 0x1FF];
        if (time != nullptr)
            *time = entry.time;
        return entry.data;
    }
};

////////////////////////////////////////////////////////////////////////////////////////////////////
// The manager

void copy(char* out, size_t size, std::string_view text)
{
    const size_t length = std::min(text.size(), size - 1);
    std::memcpy(out, text.data(), length);
    out[length] = '\0';
}

std::string ansi(const std::wstring& text)
{
    const int length = WideCharToMultiByte(CP_ACP, 0, text.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string out(length > 1 ? static_cast<size_t>(length) : 1, '\0');
    WideCharToMultiByte(CP_ACP, 0, text.c_str(), -1, out.data(), length, nullptr, nullptr);
    out.resize(out.size() - 1);
    return out;
}

ConsoleImpl g_console;
TextHandlerImpl g_text_handler;
PrimitiveHandlerImpl g_primitive_handler;
PacketsImpl g_packets;
FFXIImpl g_ffxi;

class ManagerImpl final : public abi::PluginManager {
public:
    abi::Settings* __stdcall GetSettings() override
    {
        static abi::Settings settings{};
        static bool filled = false;
        if (!filled) {
            filled = true;
            copy(settings.ProfileName, sizeof(settings.ProfileName), "VanaCore");
            copy(settings.Branch, sizeof(settings.Branch), "live");
            copy(settings.LauncherVersion, sizeof(settings.LauncherVersion), "4.3.0.0");
            copy(settings.HookVersion, sizeof(settings.HookVersion), "4.7.3.0");
            std::string ffxi = ansi(dats::ffxi_folder());
            if (!ffxi.empty() && ffxi.back() != '\\')
                ffxi += '\\';
            copy(settings.FfxiPath, sizeof(settings.FfxiPath), ffxi);
            if (ffxi.size() > 1) {
                // PlayOnline's folder is usually FFXI's parent's parent (...\PlayOnline\SquareEnix\FINAL FANTASY XI\).
                std::string pol = ffxi.substr(0, ffxi.size() - 1);
                for (int i = 0; i < 2 && pol.find_last_of('\\') != std::string::npos; ++i)
                    pol.resize(pol.find_last_of('\\'));
                copy(settings.PlayOnlinePath, sizeof(settings.PlayOnlinePath), pol + "\\");
            }
            copy(settings.WindowerPath, sizeof(settings.WindowerPath), install_path());
            copy(settings.ConsoleKey, sizeof(settings.ConsoleKey), "Insert");
            settings.Region = 1;
            settings.Loaded = true;
        }
        unsigned width = 0, height = 0;
        ui::game_screen(width, height);
        if (width == 0 || height == 0) {
            RECT client{};
            if (GetClientRect(window::handle(), &client)) {
                width = static_cast<unsigned>(client.right - client.left);
                height = static_cast<unsigned>(client.bottom - client.top);
            }
        }
        settings.Width = settings.UIWidth = static_cast<int>(width);
        settings.Height = settings.UIHeight = static_cast<int>(height);
        RECT frame{};
        if (GetWindowRect(window::handle(), &frame)) {
            settings.XPosition = frame.left;
            settings.YPosition = frame.top;
        }
        return &settings;
    }
    HWND __stdcall GetHWND() override { return window::handle(); }
    void* __stdcall GetDirect3DDevice() override { return g_device; }
    abi::Console* __stdcall GetConsole() override { return &g_console; }
    abi::TextHandler* __stdcall GetTextHandler() override { return &g_text_handler; }
    abi::PrimitiveHandler* __stdcall GetPrimitiveHandler() override { return &g_primitive_handler; }
    abi::PacketStreamHandler* __stdcall GetPacketStreamHandler() override { return &g_packets; }
    abi::FFXI* __stdcall GetFFXI() override { return &g_ffxi; }
    abi::PluginManager* __thiscall DeletingDestructor(std::uint8_t) override { return this; } // VanaCore's own
};

ManagerImpl g_manager;

} // namespace

abi::PluginManager* manager()
{
    return &g_manager;
}

void set_device(void* device)
{
    g_device = device;
}

void say(std::string_view source, std::string_view text)
{
    chat::write(chat::kDefaultMode, text, source.empty() ? std::string_view("Windower plugin") : source);
}

std::string console_text(std::string_view text)
{
    std::string out;
    out.reserve(text.size());
    for (size_t i = 0; i < text.size();) {
        if (text.substr(i, 3) == "\\cr") {
            out += "\x1E\x01";
            i += 3;
            continue;
        }
        int r = 0, g = 0, b = 0, used = 0;
        if (text.substr(i, 4) == "\\cs(" &&
            sscanf_s(std::string(text.substr(i, 24)).c_str(), "\\cs(%d,%d,%d)%n", &r, &g, &b, &used) == 3 && used > 0) {
            // The nearest of the chat log's colours.
            int code = 106; // cream
            if (r > 200 && g > 200 && b > 200)
                code = 1; // white
            else if (r > 200 && g > 200)
                code = chat::kWarning; // yellow
            else if (r > 180 && g < 120 && b < 120)
                code = chat::kError; // red
            else if (g > 180 && r < 150 && b < 150)
                code = chat::kSuccess; // green
            else if (b > 180 && r < 150)
                code = 6; // blue
            out += '\x1E';
            out += static_cast<char>(code);
            i += static_cast<size_t>(used);
            continue;
        }
        out += text[i++];
    }
    return out;
}

void release_objects(std::string_view owner)
{
    if (owner.empty())
        return;
    const std::lock_guard lock(g_lock);
    std::erase_if(g_texts, [&](const auto& text) { return text->owner == owner; });
    std::erase_if(g_primitives, [&](const auto& primitive) { return primitive->owner == owner; });
}

void draw_objects(void* device)
{
    const std::lock_guard lock(g_lock);
    // A new device: textures made on the old one are gone.
    static void* drawn_on = nullptr;
    if (device != drawn_on) {
        for (auto& text : g_texts)
            text->release();
        for (auto& primitive : g_primitives)
            primitive->release();
        drawn_on = device;
    }
    // Texts and primitives together, in the order they were made.
    size_t t = 0, p = 0;
    while (t < g_texts.size() || p < g_primitives.size()) {
        const bool text_next = p >= g_primitives.size() || (t < g_texts.size() && g_texts[t]->order < g_primitives[p]->order);
        if (text_next)
            g_texts[t++]->draw(device);
        else
            g_primitives[p++]->draw(device);
    }
}

void record_packet(bool incoming, std::uint16_t id, const std::uint8_t* data, std::uint32_t size)
{
    auto& table = g_last[incoming ? 1 : 0];
    if (table.empty())
        table.resize(0x200);
    LastPacket& entry = table[id & 0x1FF];
    entry.time = GetTickCount64();
    const size_t length = std::min<size_t>(size, sizeof(entry.data));
    std::memcpy(entry.data, data, length);
    std::memset(entry.data + length, 0, sizeof(entry.data) - length);
}

} // namespace vanacore::windower

#pragma once

#include <cstddef>

// Dear ImGui 1.81 WIP's structures (6487860a, the version Ashita interface 4.16 wraps), their data only, laid out as
// 1.81 lays them out: what interface 4.16 plugins read and write through the pointers IGuiManager gives them
// (GetIO, GetStyle, GetFont) and pass back (StyleColorsDark, AddFontFromFileTTF's ImFontConfig, InputText's
// callback). VanaCore's ImGui is 1.92, whose structures are laid out otherwise: compat/ashita/gui.cpp keeps these as
// copies of 1.92's and carries what plugins change back. Every field's offset and every size checked against 1.81's
// own imgui.h (x86, MSVC).
namespace vanacore::ashita::v181 {

struct Vec2 {
    float x, y;
};
struct Vec4 {
    float x, y, z, w;
};

template <class T>
struct Vector {
    int Size;
    int Capacity;
    T* Data;
};

inline constexpr int kColCount = 53;      // ImGuiCol_COUNT
inline constexpr int kKeyCount = 22;      // ImGuiKey_COUNT
inline constexpr int kNavInputCount = 21; // ImGuiNavInput_COUNT
inline constexpr int kTexLinesWidthMax = 63; // IM_DRAWLIST_TEX_LINES_WIDTH_MAX

struct Font;
struct FontAtlas;

struct Style {
    float Alpha;
    Vec2 WindowPadding;
    float WindowRounding;
    float WindowBorderSize;
    Vec2 WindowMinSize;
    Vec2 WindowTitleAlign;
    int WindowMenuButtonPosition;
    float ChildRounding;
    float ChildBorderSize;
    float PopupRounding;
    float PopupBorderSize;
    Vec2 FramePadding;
    float FrameRounding;
    float FrameBorderSize;
    Vec2 ItemSpacing;
    Vec2 ItemInnerSpacing;
    Vec2 CellPadding;
    Vec2 TouchExtraPadding;
    float IndentSpacing;
    float ColumnsMinSpacing;
    float ScrollbarSize;
    float ScrollbarRounding;
    float GrabMinSize;
    float GrabRounding;
    float LogSliderDeadzone;
    float TabRounding;
    float TabBorderSize;
    float TabMinWidthForCloseButton;
    int ColorButtonPosition;
    Vec2 ButtonTextAlign;
    Vec2 SelectableTextAlign;
    Vec2 DisplayWindowPadding;
    Vec2 DisplaySafeAreaPadding;
    float MouseCursorScale;
    bool AntiAliasedLines;
    bool AntiAliasedLinesUseTex;
    bool AntiAliasedFill;
    float CurveTessellationTol;
    float CircleSegmentMaxError;
    Vec4 Colors[kColCount];
};

struct IO {
    int ConfigFlags;
    int BackendFlags;
    Vec2 DisplaySize;
    float DeltaTime;
    float IniSavingRate;
    const char* IniFilename;
    const char* LogFilename;
    float MouseDoubleClickTime;
    float MouseDoubleClickMaxDist;
    float MouseDragThreshold;
    int KeyMap[kKeyCount];
    float KeyRepeatDelay;
    float KeyRepeatRate;
    void* UserData;
    FontAtlas* Fonts;
    float FontGlobalScale;
    bool FontAllowUserScaling;
    Font* FontDefault;
    Vec2 DisplayFramebufferScale;
    bool MouseDrawCursor;
    bool ConfigMacOSXBehaviors;
    bool ConfigInputTextCursorBlink;
    bool ConfigDragClickToInputText;
    bool ConfigWindowsResizeFromEdges;
    bool ConfigWindowsMoveFromTitleBarOnly;
    float ConfigMemoryCompactTimer;
    const char* BackendPlatformName;
    const char* BackendRendererName;
    void* BackendPlatformUserData;
    void* BackendRendererUserData;
    void* BackendLanguageUserData;
    const char* (*GetClipboardTextFn)(void* user_data);
    void (*SetClipboardTextFn)(void* user_data, const char* text);
    void* ClipboardUserData;
    void (*ImeSetInputScreenPosFn)(int x, int y);
    void* ImeWindowHandle;
    Vec2 MousePos;
    bool MouseDown[5];
    float MouseWheel;
    float MouseWheelH;
    bool KeyCtrl;
    bool KeyShift;
    bool KeyAlt;
    bool KeySuper;
    bool KeysDown[512];
    float NavInputs[kNavInputCount];
    bool WantCaptureMouse;
    bool WantCaptureKeyboard;
    bool WantTextInput;
    bool WantSetMousePos;
    bool WantSaveIniSettings;
    bool NavActive;
    bool NavVisible;
    float Framerate;
    int MetricsRenderVertices;
    int MetricsRenderIndices;
    int MetricsRenderWindows;
    int MetricsActiveWindows;
    int MetricsActiveAllocations;
    Vec2 MouseDelta;
    int KeyMods;
    Vec2 MousePosPrev;
    Vec2 MouseClickedPos[5];
    double MouseClickedTime[5];
    bool MouseClicked[5];
    bool MouseDoubleClicked[5];
    bool MouseReleased[5];
    bool MouseDownOwned[5];
    bool MouseDownWasDoubleClick[5];
    float MouseDownDuration[5];
    float MouseDownDurationPrev[5];
    Vec2 MouseDragMaxDistanceAbs[5];
    float MouseDragMaxDistanceSqr[5];
    float KeysDownDuration[512];
    float KeysDownDurationPrev[512];
    float NavInputsDownDuration[kNavInputCount];
    float NavInputsDownDurationPrev[kNavInputCount];
    float PenPressure;
    unsigned short InputQueueSurrogate;
    Vector<unsigned short> InputQueueCharacters;
};

struct InputTextCallbackData {
    int EventFlag;
    int Flags;
    void* UserData;
    unsigned short EventChar;
    int EventKey;
    char* Buf;
    int BufTextLen;
    int BufSize;
    bool BufDirty;
    int CursorPos;
    int SelectionStart;
    int SelectionEnd;
};

struct FontConfig {
    void* FontData;
    int FontDataSize;
    bool FontDataOwnedByAtlas;
    int FontNo;
    float SizePixels;
    int OversampleH;
    int OversampleV;
    bool PixelSnapH;
    Vec2 GlyphExtraSpacing;
    Vec2 GlyphOffset;
    const unsigned short* GlyphRanges;
    float GlyphMinAdvanceX;
    float GlyphMaxAdvanceX;
    bool MergeMode;
    unsigned int FontBuilderFlags;
    float RasterizerMultiply;
    unsigned short EllipsisChar;
    char Name[40];
    Font* DstFont;
};

struct FontGlyph {
    unsigned int Codepoint : 31;
    unsigned int Visible : 1;
    float AdvanceX;
    float X0, Y0, X1, Y1;
    float U0, V0, U1, V1;
};

struct FontAtlasCustomRect {
    unsigned short Width, Height;
    unsigned short X, Y;
    unsigned int GlyphID;
    float GlyphAdvanceX;
    Vec2 GlyphOffset;
    Font* Font;
};

struct FontAtlas {
    bool Locked;
    int Flags;
    void* TexID;
    int TexDesiredWidth;
    int TexGlyphPadding;
    unsigned char* TexPixelsAlpha8;
    unsigned int* TexPixelsRGBA32;
    int TexWidth;
    int TexHeight;
    Vec2 TexUvScale;
    Vec2 TexUvWhitePixel;
    Vector<Font*> Fonts;
    Vector<FontAtlasCustomRect> CustomRects;
    Vector<FontConfig> ConfigData;
    Vec4 TexUvLines[kTexLinesWidthMax + 1];
    const void* FontBuilderIO;
    unsigned int FontBuilderFlags;
    int PackIdMouseCursors;
    int PackIdLines;
};

struct Font {
    Vector<float> IndexAdvanceX;
    float FallbackAdvanceX;
    float FontSize;
    Vector<unsigned short> IndexLookup;
    Vector<FontGlyph> Glyphs;
    const FontGlyph* FallbackGlyph;
    FontAtlas* ContainerAtlas;
    const FontConfig* ConfigData;
    short ConfigDataCount;
    unsigned short FallbackChar;
    unsigned short EllipsisChar;
    bool DirtyLookupTables;
    float Scale;
    float Ascent, Descent;
    int MetricsTotalSurface;
    unsigned char Used4kPagesMap[2];
};

} // namespace vanacore::ashita::v181

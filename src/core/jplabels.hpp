#pragma once

// True Font's menu labels in a game in Japanese. Its menu DAT (ROM\91\15) draws each label ("ステータス", "装備変更") as one or
// more parts of a sprite, pictures of whole words on its own sheets ("menu    menufont", "menu    mn6font"...), not the English
// labels' letters. Each label's words were read off its pictures once (tools\jplabels: labels.tsv, jplabels_data.h). As a
// shape is drawn its label parts are put together into labels as the tool does (each pass apart, side by side on one row) and
// looked up; as each part's quad is drawn the graphics layer is told what it is (vanacore_gfx_label_word: d3d8_chatfont_jp.h):
// a label's first part is its words in the labels' True Font, its later parts left out; a picture of those sheets that isn't
// a label known stays the game's. Only while a game in Japanese has True Font's labels on (chatfont::labels_on).
namespace vanacore::jplabels {

// Once a frame on the game's thread (chatfont.cpp's): whether the game is in Japanese with the labels' True Font on
void on_frame();

// It is (itemslot.cpp hooks the game's quad drawing for it, buttons.cpp its shape drawing for the labels' True Font)
bool on();

// Around the game's drawing of a shape (buttons.cpp's hooks of both shape draws): its label parts put together into labels
void shape_begin(const void* shape);
void shape_end(const void* shape);

// Around the game's drawing of one quad (itemslot.cpp's hook): the graphics layer told what it is while it's drawn
class Quad {
public:
    explicit Quad(const char* quad);
    ~Quad();
    Quad(const Quad&) = delete;
    Quad& operator=(const Quad&) = delete;

private:
    bool told_ = false;
};

// Label `index`'s words (its glyph's code less VANA_LABEL_WORDS), UTF-8; null past the last
const char* words(int index);

} // namespace vanacore::jplabels

// text.cpp's: a text's box grown to hold the ink that leaves it. Included in text.cpp's unnamed namespace, after Coverage and box_size.
//
// A letter's ink goes past its advance where the font says so: an italic's f, j, J, p and A lean left of where the pen starts and
// its capitals' tops lean right (Constantia or Georgia bold italic at 15 px: f, j and J three pixels, at 37 px up to eight), a
// swash's tail (Viner Hand's J: a third of the size). The box has the style's padding round the text and `lean` on the right of
// an italic, so a picture with padding 0 or 1 (a zone name's, each letter of a window's font sheet text: chatfont.cpp's style_of)
// lost that ink at its edge: the letter's side was cut (reported 2026-11-13, Cresc: a bold italic font). The text is drawn
// `margin` in from the edges of a canvas that much bigger, then the box grows till its ink is the outline's width clear of every
// edge; a text whose ink is that far in already is as it was.

// How far in from its canvas's edges a text is drawn: the ink past the box (a size's worth of it) and the outline
int ink_margin(const Style& style)
{
    const int stroke = style.stroke < 0 ? 0 : style.stroke > 8 ? 8 : style.stroke;
    const float size = style.exact_size > static_cast<float>(style.size) ? style.exact_size : static_cast<float>(style.size);
    return static_cast<int>(std::ceil(size * 0.6f)) + stroke + 4;
}

// `canvas`, (width + 2 * margin) x (height + 2 * margin) coverage bytes with the text's box at (margin, margin), into `out`: its
// box grown on a side until the ink is `stroke` pixels (the outline round it) clear of that edge, at most `margin`; the runs and
// the first line's top moved with the left and top. A box whose ink is that far in already is as it was.
void fit_to_ink(Coverage& out, const std::vector<std::uint8_t>& canvas, int margin, int stroke)
{
    const int box_width = out.width, box_height = out.height;
    const int canvas_width = box_width + 2 * margin, canvas_height = box_height + 2 * margin;
    int x0 = canvas_width, y0 = canvas_height, x1 = -1, y1 = -1; // the ink's columns and rows
    for (int y = 0; y < canvas_height; ++y) {
        const std::uint8_t* row = &canvas[static_cast<size_t>(y) * canvas_width];
        int first = 0;
        while (first < canvas_width && row[first] == 0)
            ++first;
        if (first == canvas_width)
            continue;
        int last = canvas_width - 1;
        while (row[last] == 0)
            --last;
        x0 = (std::min)(x0, first);
        x1 = (std::max)(x1, last);
        y0 = (std::min)(y0, y);
        y1 = y;
    }
    const int room = stroke < 0 ? 0 : stroke > 8 ? 8 : stroke;
    // each side by how much the ink is short of `room` pixels clear of the box's edge (past it: all of it and `room`)
    const auto grown = [&](int short_by) { return short_by > 0 ? (std::min)(short_by, margin) : 0; };
    const int left = grown(margin + room - x0), top = grown(margin + room - y0);
    const int right = grown(x1 + 1 + room - (margin + box_width)), bottom = grown(y1 + 1 + room - (margin + box_height));
    out.width = box_width + left + right;
    out.height = box_height + top + bottom;
    out.values.resize(static_cast<size_t>(out.width) * out.height);
    for (int y = 0; y < out.height; ++y)
        std::memcpy(&out.values[static_cast<size_t>(y) * out.width],
                    &canvas[static_cast<size_t>(margin - top + y) * canvas_width + (margin - left)], static_cast<size_t>(out.width));
    out.pad_y += top;
    for (auto& line : out.placed)
        for (Placed& run : line)
            run.x += static_cast<float>(left);
}

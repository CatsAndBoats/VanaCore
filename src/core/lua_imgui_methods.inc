// Included in lua_imgui.cpp after lua_imgui_objects.inc: the methods of ImGui's structures, what addons written for Ashita
// 4.16 (ImGui 1.81) read that 1.92 dropped, and the structures addons make themselves (ImGuiListClipper.new()).

constexpr std::uint32_t kWhite = 0xFFFFFFFF;

// ---- ImDrawList ------------------------------------------------------------------------------------------------------
// Points as {x, y}, colours ImU32. 1.81's corner flags (ImDrawCornerFlags: 0 no corner rounded) for an addon passing
// 1.81's numbers.

ImDrawList* dl(lua_State* L)
{
    return self_of<ImDrawList>(L, view_ImDrawList());
}

ImDrawFlags corners(lua_State* L, int i)
{
    if (!lua_isnumber(L, i))
        return 0;
    const int v = opt_int(L, i, 0);
    if (!legacy_numbers(L))
        return v;
    return v == 0 ? ImDrawFlags_RoundCornersNone : legacy::in_ImDrawCornerFlags(v);
}

// A table of points ({ {x, y}, ... }), at most `limit` of them (-1: all).
std::vector<ImVec2> points(lua_State* L, int i, int limit = -1)
{
    std::vector<ImVec2> out;
    if (!lua_istable(L, i))
        return out;
    int n = static_cast<int>(lua_objlen(L, i));
    if (limit >= 0 && limit < n)
        n = limit;
    for (int k = 1; k <= n; ++k) {
        lua_rawgeti(L, i, k);
        out.push_back(opt_vec2(L, lua_gettop(L), ImVec2(0, 0)));
        lua_pop(L, 1);
    }
    return out;
}

int dl_push_clip_rect(lua_State* L) { dl(L)->PushClipRect(opt_vec2(L, 2, {}), opt_vec2(L, 3, {}), opt_bool(L, 4, false)); return 0; }
int dl_push_clip_rect_full_screen(lua_State* L) { dl(L)->PushClipRectFullScreen(); return 0; }
int dl_pop_clip_rect(lua_State* L)
{
    ImDrawList* list = dl(L);
    if (list->_ClipRectStack.Size > 1) // one the list didn't push: ImGui would read past its stack
        list->PopClipRect();
    return 0;
}
int dl_push_texture(lua_State* L) { dl(L)->PushTexture(opt_texture(L, 2)); return 0; }
int dl_pop_texture(lua_State* L)
{
    ImDrawList* list = dl(L);
    if (list->_TextureStack.Size > 1)
        list->PopTexture();
    return 0;
}
int dl_get_clip_rect_min(lua_State* L) { return push_vec2(L, dl(L)->GetClipRectMin()); }
int dl_get_clip_rect_max(lua_State* L) { return push_vec2(L, dl(L)->GetClipRectMax()); }
int dl_add_line(lua_State* L) { dl(L)->AddLine(opt_vec2(L, 2, {}), opt_vec2(L, 3, {}), opt_u32(L, 4, 0), opt_float(L, 5, 1.0f)); return 0; }
int dl_add_rect(lua_State* L)
{
    dl(L)->AddRect(opt_vec2(L, 2, {}), opt_vec2(L, 3, {}), opt_u32(L, 4, 0), opt_float(L, 5, 0.0f), corners(L, 6), opt_float(L, 7, 1.0f));
    return 0;
}
int dl_add_rect_filled(lua_State* L)
{
    dl(L)->AddRectFilled(opt_vec2(L, 2, {}), opt_vec2(L, 3, {}), opt_u32(L, 4, 0), opt_float(L, 5, 0.0f), corners(L, 6));
    return 0;
}
int dl_add_rect_filled_multi_color(lua_State* L)
{
    dl(L)->AddRectFilledMultiColor(opt_vec2(L, 2, {}), opt_vec2(L, 3, {}), opt_u32(L, 4, 0), opt_u32(L, 5, 0), opt_u32(L, 6, 0), opt_u32(L, 7, 0));
    return 0;
}
int dl_add_quad(lua_State* L)
{
    dl(L)->AddQuad(opt_vec2(L, 2, {}), opt_vec2(L, 3, {}), opt_vec2(L, 4, {}), opt_vec2(L, 5, {}), opt_u32(L, 6, 0), opt_float(L, 7, 1.0f));
    return 0;
}
int dl_add_quad_filled(lua_State* L)
{
    dl(L)->AddQuadFilled(opt_vec2(L, 2, {}), opt_vec2(L, 3, {}), opt_vec2(L, 4, {}), opt_vec2(L, 5, {}), opt_u32(L, 6, 0));
    return 0;
}
int dl_add_triangle(lua_State* L)
{
    dl(L)->AddTriangle(opt_vec2(L, 2, {}), opt_vec2(L, 3, {}), opt_vec2(L, 4, {}), opt_u32(L, 5, 0), opt_float(L, 6, 1.0f));
    return 0;
}
int dl_add_triangle_filled(lua_State* L)
{
    dl(L)->AddTriangleFilled(opt_vec2(L, 2, {}), opt_vec2(L, 3, {}), opt_vec2(L, 4, {}), opt_u32(L, 5, 0));
    return 0;
}
int dl_add_circle(lua_State* L)
{
    dl(L)->AddCircle(opt_vec2(L, 2, {}), opt_float(L, 3, 0), opt_u32(L, 4, 0), opt_int(L, 5, 0), opt_float(L, 6, 1.0f));
    return 0;
}
int dl_add_circle_filled(lua_State* L)
{
    dl(L)->AddCircleFilled(opt_vec2(L, 2, {}), opt_float(L, 3, 0), opt_u32(L, 4, 0), opt_int(L, 5, 0));
    return 0;
}
int dl_add_ngon(lua_State* L)
{
    dl(L)->AddNgon(opt_vec2(L, 2, {}), opt_float(L, 3, 0), opt_u32(L, 4, 0), opt_int(L, 5, 3), opt_float(L, 6, 1.0f));
    return 0;
}
int dl_add_ngon_filled(lua_State* L)
{
    dl(L)->AddNgonFilled(opt_vec2(L, 2, {}), opt_float(L, 3, 0), opt_u32(L, 4, 0), opt_int(L, 5, 3));
    return 0;
}
int dl_add_ellipse(lua_State* L)
{
    dl(L)->AddEllipse(opt_vec2(L, 2, {}), opt_vec2(L, 3, {}), opt_u32(L, 4, 0), opt_float(L, 5, 0), opt_int(L, 6, 0), opt_float(L, 7, 1.0f));
    return 0;
}
int dl_add_ellipse_filled(lua_State* L)
{
    dl(L)->AddEllipseFilled(opt_vec2(L, 2, {}), opt_vec2(L, 3, {}), opt_u32(L, 4, 0), opt_float(L, 5, 0), opt_int(L, 6, 0));
    return 0;
}
// AddText(pos, col, text), or AddText(font, size, pos, col, text, wrap_width, cpu_fine_clip_rect)
int dl_add_text(lua_State* L)
{
    ImDrawList* list = dl(L);
    if (ImFont* font = opt_font(L, 2)) {
        float clip[4];
        const bool clipped = read_vec(L, 8, clip, 4);
        const ImVec4 rect(clip[0], clip[1], clip[2], clip[3]);
        list->AddText(font, opt_float(L, 3, font->LegacySize), opt_vec2(L, 4, {}), opt_u32(L, 5, 0), opt_string(L, 6, ""), nullptr,
                      opt_float(L, 7, 0.0f), clipped ? &rect : nullptr);
    } else {
        list->AddText(opt_vec2(L, 2, {}), opt_u32(L, 3, 0), opt_string(L, 4, ""));
    }
    return 0;
}
int dl_add_bezier_cubic(lua_State* L)
{
    dl(L)->AddBezierCubic(opt_vec2(L, 2, {}), opt_vec2(L, 3, {}), opt_vec2(L, 4, {}), opt_vec2(L, 5, {}), opt_u32(L, 6, 0),
                          opt_float(L, 7, 1.0f), opt_int(L, 8, 0));
    return 0;
}
int dl_add_bezier_quadratic(lua_State* L)
{
    dl(L)->AddBezierQuadratic(opt_vec2(L, 2, {}), opt_vec2(L, 3, {}), opt_vec2(L, 4, {}), opt_u32(L, 5, 0), opt_float(L, 6, 1.0f), opt_int(L, 7, 0));
    return 0;
}
// AddPolyline(points, num_points, col, flags, thickness) (4.30), or (points, col, closed, thickness) (4.16)
int dl_add_polyline(lua_State* L)
{
    ImDrawList* list = dl(L);
    if (lua_type(L, 4) == LUA_TBOOLEAN || (legacy_numbers(L) && !lua_isnumber(L, 5))) {
        const std::vector<ImVec2> p = points(L, 2);
        if (!p.empty())
            list->AddPolyline(p.data(), static_cast<int>(p.size()), opt_u32(L, 3, 0), opt_bool(L, 4, false) ? ImDrawFlags_Closed : 0,
                              opt_float(L, 5, 1.0f));
        return 0;
    }
    const std::vector<ImVec2> p = points(L, 2, opt_int(L, 3, -1));
    if (!p.empty())
        list->AddPolyline(p.data(), static_cast<int>(p.size()), opt_u32(L, 4, 0), opt_int(L, 5, 0), opt_float(L, 6, 1.0f));
    return 0;
}
// (points, num_points, col) (4.30), or (points, col) (4.16)
template <void (ImDrawList::*Fill)(const ImVec2*, int, ImU32)>
int dl_add_poly_filled(lua_State* L)
{
    ImDrawList* list = dl(L);
    const bool counted = lua_isnumber(L, 4);
    const std::vector<ImVec2> p = points(L, 2, counted ? opt_int(L, 3, -1) : -1);
    if (!p.empty())
        (list->*Fill)(p.data(), static_cast<int>(p.size()), opt_u32(L, counted ? 4 : 3, 0));
    return 0;
}
int dl_add_image(lua_State* L)
{
    dl(L)->AddImage(opt_texture(L, 2), opt_vec2(L, 3, {}), opt_vec2(L, 4, {}), opt_vec2(L, 5, ImVec2(0, 0)), opt_vec2(L, 6, ImVec2(1, 1)),
                    opt_u32(L, 7, kWhite));
    return 0;
}
int dl_add_image_quad(lua_State* L)
{
    dl(L)->AddImageQuad(opt_texture(L, 2), opt_vec2(L, 3, {}), opt_vec2(L, 4, {}), opt_vec2(L, 5, {}), opt_vec2(L, 6, {}),
                        opt_vec2(L, 7, ImVec2(0, 0)), opt_vec2(L, 8, ImVec2(1, 0)), opt_vec2(L, 9, ImVec2(1, 1)),
                        opt_vec2(L, 10, ImVec2(0, 1)), opt_u32(L, 11, kWhite));
    return 0;
}
int dl_add_image_rounded(lua_State* L)
{
    dl(L)->AddImageRounded(opt_texture(L, 2), opt_vec2(L, 3, {}), opt_vec2(L, 4, {}), opt_vec2(L, 5, ImVec2(0, 0)),
                           opt_vec2(L, 6, ImVec2(1, 1)), opt_u32(L, 7, kWhite), opt_float(L, 8, 0.0f), corners(L, 9));
    return 0;
}
int dl_path_clear(lua_State* L) { dl(L)->PathClear(); return 0; }
int dl_path_line_to(lua_State* L) { dl(L)->PathLineTo(opt_vec2(L, 2, {})); return 0; }
int dl_path_line_to_merge(lua_State* L) { dl(L)->PathLineToMergeDuplicate(opt_vec2(L, 2, {})); return 0; }
int dl_path_fill_convex(lua_State* L) { dl(L)->PathFillConvex(opt_u32(L, 2, 0)); return 0; }
int dl_path_fill_concave(lua_State* L) { dl(L)->PathFillConcave(opt_u32(L, 2, 0)); return 0; }
int dl_path_stroke(lua_State* L)
{
    ImDrawList* list = dl(L);
    const ImDrawFlags flags = lua_type(L, 3) == LUA_TBOOLEAN ? (lua_toboolean(L, 3) ? ImDrawFlags_Closed : 0) : opt_int(L, 3, 0);
    list->PathStroke(opt_u32(L, 2, 0), flags, opt_float(L, 4, 1.0f));
    return 0;
}
int dl_path_arc_to(lua_State* L)
{
    dl(L)->PathArcTo(opt_vec2(L, 2, {}), opt_float(L, 3, 0), opt_float(L, 4, 0), opt_float(L, 5, 0), opt_int(L, 6, 0));
    return 0;
}
int dl_path_arc_to_fast(lua_State* L)
{
    dl(L)->PathArcToFast(opt_vec2(L, 2, {}), opt_float(L, 3, 0), opt_int(L, 4, 0), opt_int(L, 5, 0));
    return 0;
}
int dl_path_elliptical_arc_to(lua_State* L)
{
    dl(L)->PathEllipticalArcTo(opt_vec2(L, 2, {}), opt_vec2(L, 3, {}), opt_float(L, 4, 0), opt_float(L, 5, 0), opt_float(L, 6, 0), opt_int(L, 7, 0));
    return 0;
}
int dl_path_bezier_cubic(lua_State* L)
{
    dl(L)->PathBezierCubicCurveTo(opt_vec2(L, 2, {}), opt_vec2(L, 3, {}), opt_vec2(L, 4, {}), opt_int(L, 5, 0));
    return 0;
}
int dl_path_bezier_quadratic(lua_State* L)
{
    dl(L)->PathBezierQuadraticCurveTo(opt_vec2(L, 2, {}), opt_vec2(L, 3, {}), opt_int(L, 4, 0));
    return 0;
}
int dl_path_rect(lua_State* L) { dl(L)->PathRect(opt_vec2(L, 2, {}), opt_vec2(L, 3, {}), opt_float(L, 4, 0), corners(L, 5)); return 0; }
int dl_channels_split(lua_State* L) { dl(L)->ChannelsSplit(opt_int(L, 2, 1)); return 0; }
int dl_channels_merge(lua_State* L) { dl(L)->ChannelsMerge(); return 0; }
int dl_channels_set_current(lua_State* L) { dl(L)->ChannelsSetCurrent(opt_int(L, 2, 0)); return 0; }
int dl_add_draw_cmd(lua_State* L) { dl(L)->AddDrawCmd(); return 0; }
int dl_prim_reserve(lua_State* L) { dl(L)->PrimReserve(opt_int(L, 2, 0), opt_int(L, 3, 0)); return 0; }
int dl_prim_unreserve(lua_State* L) { dl(L)->PrimUnreserve(opt_int(L, 2, 0), opt_int(L, 3, 0)); return 0; }
int dl_prim_rect(lua_State* L) { dl(L)->PrimRect(opt_vec2(L, 2, {}), opt_vec2(L, 3, {}), opt_u32(L, 4, 0)); return 0; }
int dl_prim_rect_uv(lua_State* L)
{
    dl(L)->PrimRectUV(opt_vec2(L, 2, {}), opt_vec2(L, 3, {}), opt_vec2(L, 4, {}), opt_vec2(L, 5, {}), opt_u32(L, 6, 0));
    return 0;
}
int dl_prim_quad_uv(lua_State* L)
{
    dl(L)->PrimQuadUV(opt_vec2(L, 2, {}), opt_vec2(L, 3, {}), opt_vec2(L, 4, {}), opt_vec2(L, 5, {}), opt_vec2(L, 6, {}), opt_vec2(L, 7, {}),
                      opt_vec2(L, 8, {}), opt_vec2(L, 9, {}), opt_u32(L, 10, 0));
    return 0;
}
// (writes past a PrimReserve are refused: ImGui would write outside the list's buffers)
bool reserved(ImDrawList* list, bool vertex)
{
    return vertex ? list->_VtxWritePtr < list->VtxBuffer.Data + list->VtxBuffer.Size
                  : list->_IdxWritePtr < list->IdxBuffer.Data + list->IdxBuffer.Size;
}
int dl_prim_write_vtx(lua_State* L)
{
    ImDrawList* list = dl(L);
    if (reserved(list, true))
        list->PrimWriteVtx(opt_vec2(L, 2, {}), opt_vec2(L, 3, {}), opt_u32(L, 4, 0));
    return 0;
}
int dl_prim_write_idx(lua_State* L)
{
    ImDrawList* list = dl(L);
    if (reserved(list, false))
        list->PrimWriteIdx(static_cast<ImDrawIdx>(opt_int(L, 2, 0)));
    return 0;
}
int dl_prim_vtx(lua_State* L)
{
    ImDrawList* list = dl(L);
    if (reserved(list, true) && reserved(list, false))
        list->PrimVtx(opt_vec2(L, 2, {}), opt_vec2(L, 3, {}), opt_u32(L, 4, 0));
    return 0;
}

const luaL_Reg kDrawListMethods[] = {
    {"PushClipRect", &dl_push_clip_rect},
    {"PushClipRectFullScreen", &dl_push_clip_rect_full_screen},
    {"PopClipRect", &dl_pop_clip_rect},
    {"PushTexture", &dl_push_texture},
    {"PopTexture", &dl_pop_texture},
    {"PushTextureID", &dl_push_texture}, // their names before 1.92
    {"PopTextureID", &dl_pop_texture},
    {"GetClipRectMin", &dl_get_clip_rect_min},
    {"GetClipRectMax", &dl_get_clip_rect_max},
    {"AddLine", &dl_add_line},
    {"AddRect", &dl_add_rect},
    {"AddRectFilled", &dl_add_rect_filled},
    {"AddRectFilledMultiColor", &dl_add_rect_filled_multi_color},
    {"AddQuad", &dl_add_quad},
    {"AddQuadFilled", &dl_add_quad_filled},
    {"AddTriangle", &dl_add_triangle},
    {"AddTriangleFilled", &dl_add_triangle_filled},
    {"AddCircle", &dl_add_circle},
    {"AddCircleFilled", &dl_add_circle_filled},
    {"AddNgon", &dl_add_ngon},
    {"AddNgonFilled", &dl_add_ngon_filled},
    {"AddEllipse", &dl_add_ellipse},
    {"AddEllipseFilled", &dl_add_ellipse_filled},
    {"AddText", &dl_add_text},
    {"AddBezierCubic", &dl_add_bezier_cubic},
    {"AddBezierCurve", &dl_add_bezier_cubic}, // its name before 1.80
    {"AddBezierQuadratic", &dl_add_bezier_quadratic},
    {"AddPolyline", &dl_add_polyline},
    {"AddConvexPolyFilled", &dl_add_poly_filled<&ImDrawList::AddConvexPolyFilled>},
    {"AddConcavePolyFilled", &dl_add_poly_filled<&ImDrawList::AddConcavePolyFilled>},
    {"AddImage", &dl_add_image},
    {"AddImageQuad", &dl_add_image_quad},
    {"AddImageRounded", &dl_add_image_rounded},
    {"PathClear", &dl_path_clear},
    {"PathLineTo", &dl_path_line_to},
    {"PathLineToMergeDuplicate", &dl_path_line_to_merge},
    {"PathFillConvex", &dl_path_fill_convex},
    {"PathFillConcave", &dl_path_fill_concave},
    {"PathStroke", &dl_path_stroke},
    {"PathArcTo", &dl_path_arc_to},
    {"PathArcToFast", &dl_path_arc_to_fast},
    {"PathEllipticalArcTo", &dl_path_elliptical_arc_to},
    {"PathBezierCubicCurveTo", &dl_path_bezier_cubic},
    {"PathBezierQuadraticCurveTo", &dl_path_bezier_quadratic},
    {"PathRect", &dl_path_rect},
    {"ChannelsSplit", &dl_channels_split},
    {"ChannelsMerge", &dl_channels_merge},
    {"ChannelsSetCurrent", &dl_channels_set_current},
    {"AddDrawCmd", &dl_add_draw_cmd},
    {"PrimReserve", &dl_prim_reserve},
    {"PrimUnreserve", &dl_prim_unreserve},
    {"PrimRect", &dl_prim_rect},
    {"PrimRectUV", &dl_prim_rect_uv},
    {"PrimQuadUV", &dl_prim_quad_uv},
    {"PrimWriteVtx", &dl_prim_write_vtx},
    {"PrimWriteIdx", &dl_prim_write_idx},
    {"PrimVtx", &dl_prim_vtx},
    {nullptr, nullptr},
};

// ---- Fonts -----------------------------------------------------------------------------------------------------------

ImFont* font_self(lua_State* L) { return self_of<ImFont>(L, view_ImFont()); }
int font_is_glyph_in_font(lua_State* L) { lua_pushboolean(L, font_self(L)->IsGlyphInFont(static_cast<ImWchar>(opt_int(L, 2, 0)))); return 1; }
int font_is_loaded(lua_State* L) { lua_pushboolean(L, font_self(L)->IsLoaded()); return 1; }
int font_get_debug_name(lua_State* L) { return push_string(L, font_self(L)->GetDebugName()); }
int font_get_font_baked(lua_State* L)
{
    ImFont* font = font_self(L);
    if (!gui::in_frame() && ImGui::GetCurrentContext() == nullptr)
        return 0;
    push_object(L, font->GetFontBaked(opt_float(L, 2, font->LegacySize), opt_float(L, 3, -1.0f)), &view_ImFontBaked());
    return 1;
}
// CalcTextSizeA(size, max_width, wrap_width, text): its size, and what's left of the text past max_width
int font_calc_text_size_a(lua_State* L)
{
    ImFont* font = font_self(L);
    std::size_t length = 0;
    const char* text = lua_type(L, 5) == LUA_TSTRING ? lua_tolstring(L, 5, &length) : "";
    const char* remaining = nullptr;
    const ImVec2 size = font->CalcTextSizeA(opt_float(L, 2, font->LegacySize), opt_float(L, 3, FLT_MAX), opt_float(L, 4, 0.0f), text,
                                            text + length, &remaining);
    push_vec2(L, size);
    if (remaining != nullptr && remaining < text + length)
        lua_pushlstring(L, remaining, static_cast<std::size_t>(text + length - remaining));
    else
        lua_pushnil(L);
    return 3;
}
// CalcWordWrapPosition(size, text, text_end, wrap_width): the text from where it wraps
int font_calc_word_wrap_position(lua_State* L)
{
    ImFont* font = font_self(L);
    std::size_t length = 0;
    const char* text = lua_type(L, 3) == LUA_TSTRING ? lua_tolstring(L, 3, &length) : "";
    const float wrap = lua_isnumber(L, 5) ? opt_float(L, 5, 0) : opt_float(L, 4, 0);
    const char* at = font->CalcWordWrapPosition(opt_float(L, 2, font->LegacySize), text, text + length, wrap);
    lua_pushstring(L, at != nullptr ? at : "");
    return 1;
}
int font_render_char(lua_State* L)
{
    ImFont* font = font_self(L);
    auto* list = static_cast<ImDrawList*>(object_of(L, 2, view_ImDrawList()));
    if (list == nullptr)
        return 0;
    float clip[4];
    const bool clipped = read_vec(L, 7, clip, 4);
    const ImVec4 rect(clip[0], clip[1], clip[2], clip[3]);
    font->RenderChar(list, opt_float(L, 3, font->LegacySize), opt_vec2(L, 4, {}), opt_u32(L, 5, 0), static_cast<ImWchar>(opt_int(L, 6, 0)),
                     clipped ? &rect : nullptr);
    return 0;
}
int font_render_text(lua_State* L)
{
    ImFont* font = font_self(L);
    auto* list = static_cast<ImDrawList*>(object_of(L, 2, view_ImDrawList()));
    if (list == nullptr)
        return 0;
    std::size_t length = 0;
    const char* text = lua_type(L, 7) == LUA_TSTRING ? lua_tolstring(L, 7, &length) : "";
    font->RenderText(list, opt_float(L, 3, font->LegacySize), opt_vec2(L, 4, {}), opt_u32(L, 5, 0), opt_vec4(L, 6, ImVec4(0, 0, FLT_MAX, FLT_MAX)),
                     text, text + length, opt_float(L, 9, 0.0f), opt_bool(L, 10, false));
    return 0;
}
int font_add_remap_char(lua_State* L)
{
    font_self(L)->AddRemapChar(static_cast<ImWchar>(opt_int(L, 2, 0)), static_cast<ImWchar>(opt_int(L, 3, 0)));
    return 0;
}
int font_is_glyph_range_unused(lua_State* L)
{
    lua_pushboolean(L, font_self(L)->IsGlyphRangeUnused(opt_u32(L, 2, 0), opt_u32(L, 3, 0)));
    return 1;
}
// (the atlas every window shares: its fonts' glyphs aren't an addon's to throw away)
int refused(lua_State*)
{
    return 0;
}

// The baked size a 1.81 addon asks font.Ascent and the like of: the font's own size.
ImFontBaked* legacy_baked(const ImFont* font)
{
    if (ImGui::GetCurrentContext() == nullptr || !GImGui->WithinFrameScope)
        return nullptr;
    return const_cast<ImFont*>(font)->GetFontBaked(font->LegacySize);
}

ImFontBaked* baked_self(lua_State* L) { return self_of<ImFontBaked>(L, view_ImFontBaked()); }
int baked_find_glyph(lua_State* L)
{
    push_object(L, baked_self(L)->FindGlyph(static_cast<ImWchar>(opt_int(L, 2, 0))), &view_ImFontGlyph());
    return 1;
}
int baked_find_glyph_no_fallback(lua_State* L)
{
    push_object(L, baked_self(L)->FindGlyphNoFallback(static_cast<ImWchar>(opt_int(L, 2, 0))), &view_ImFontGlyph());
    return 1;
}
int baked_get_char_advance(lua_State* L) { lua_pushnumber(L, baked_self(L)->GetCharAdvance(static_cast<ImWchar>(opt_int(L, 2, 0)))); return 1; }
int baked_is_glyph_loaded(lua_State* L) { lua_pushboolean(L, baked_self(L)->IsGlyphLoaded(static_cast<ImWchar>(opt_int(L, 2, 0)))); return 1; }

ImFontAtlas* atlas_self(lua_State* L) { return self_of<ImFontAtlas>(L, view_ImFontAtlas()); }
// Lua's paths are ANSI; ImGui opens UTF-8 ones
std::string utf8_path(const char* path)
{
    wchar_t wide[MAX_PATH * 2] = {};
    char file[MAX_PATH * 4] = {};
    if (MultiByteToWideChar(CP_ACP, 0, path, -1, wide, MAX_PATH * 2) == 0 || WideCharToMultiByte(CP_UTF8, 0, wide, -1, file, sizeof file, nullptr, nullptr) == 0 ||
        GetFileAttributesW(wide) == INVALID_FILE_ATTRIBUTES)
        return {};
    return file;
}
const ImFontConfig* opt_font_config(lua_State* L, int i)
{
    return static_cast<const ImFontConfig*>(object_of(L, i, view_ImFontConfig()));
}
int atlas_add_font(lua_State* L) { return push_font(L, opt_font_config(L, 2) != nullptr ? atlas_self(L)->AddFont(opt_font_config(L, 2)) : nullptr); }
int atlas_add_font_default(lua_State* L) { return push_font(L, atlas_self(L)->AddFontDefault(opt_font_config(L, 2))); }
int atlas_add_font_from_file_ttf(lua_State* L)
{
    ImFontAtlas* atlas = atlas_self(L);
    const std::string file = utf8_path(opt_string(L, 2, ""));
    if (file.empty()) {
        lua_pushnil(L);
        return 1;
    }
    return push_font(L, atlas->AddFontFromFileTTF(file.c_str(), opt_float(L, 3, 0.0f), opt_font_config(L, 4)));
}
// AddFontFromMemoryTTF(data, size, size_pixels, cfg): data a string, or a pointer the atlas copies from
int atlas_add_font_from_memory(lua_State* L, bool compressed)
{
    ImFontAtlas* atlas = atlas_self(L);
    std::size_t length = 0;
    const void* data = lua_type(L, 2) == LUA_TSTRING ? lua_tolstring(L, 2, &length) : opt_pointer(L, 2);
    const int size = lua_isnumber(L, 3) ? opt_int(L, 3, 0) : static_cast<int>(length);
    if (data == nullptr || size <= 0) {
        lua_pushnil(L);
        return 1;
    }
    ImFontConfig config;
    if (const ImFontConfig* given = opt_font_config(L, 5))
        config = *given;
    if (compressed)
        return push_font(L, atlas->AddFontFromMemoryCompressedTTF(data, size, opt_float(L, 4, 0.0f), &config));
    void* copy = IM_ALLOC(static_cast<std::size_t>(size)); // the atlas owns it (FontDataOwnedByAtlas)
    std::memcpy(copy, data, static_cast<std::size_t>(size));
    config.FontDataOwnedByAtlas = true;
    return push_font(L, atlas->AddFontFromMemoryTTF(copy, size, opt_float(L, 4, 0.0f), &config));
}
int atlas_add_font_from_memory_ttf(lua_State* L) { return atlas_add_font_from_memory(L, false); }
int atlas_add_font_from_memory_compressed_ttf(lua_State* L) { return atlas_add_font_from_memory(L, true); }
int atlas_add_font_from_memory_base85(lua_State* L)
{
    return push_font(L, atlas_self(L)->AddFontFromMemoryCompressedBase85TTF(opt_string(L, 2, ""), opt_float(L, 3, 0.0f), opt_font_config(L, 4)));
}
// An addon's own font can go; the fonts every window starts with (VanaCore's, Ashita's Agave) stay
int atlas_remove_font(lua_State* L)
{
    ImFontAtlas* atlas = atlas_self(L);
    ImFont* font = opt_font(L, 2);
    if (font != nullptr && atlas->Fonts.Size > 3 && font != atlas->Fonts[0] && font != atlas->Fonts[1] && font != atlas->Fonts[2] &&
        font != ImGui::GetIO().FontDefault)
        atlas->RemoveFont(font);
    return 0;
}
int atlas_compact_cache(lua_State* L) { atlas_self(L)->CompactCache(); return 0; }
int atlas_get_glyph_ranges_default(lua_State* L)
{
    const ImWchar* ranges = atlas_self(L)->GetGlyphRangesDefault();
    lua_pushnumber(L, static_cast<lua_Number>(reinterpret_cast<std::uintptr_t>(ranges)));
    return 1;
}
int atlas_add_custom_rect(lua_State* L)
{
    ImFontAtlasRect* out = static_cast<ImFontAtlasRect*>(object_of(L, 4, view_ImFontAtlasRect()));
    lua_pushnumber(L, atlas_self(L)->AddCustomRect(opt_int(L, 2, 0), opt_int(L, 3, 0), out));
    return 1;
}
int atlas_remove_custom_rect(lua_State* L) { atlas_self(L)->RemoveCustomRect(opt_int(L, 2, -1)); return 0; }
// GetCustomRect(id, out_r): true and out_r filled, or with no out_r given, a new rect (nil if there's none)
int atlas_get_custom_rect(lua_State* L)
{
    ImFontAtlas* atlas = atlas_self(L);
    if (auto* out = static_cast<ImFontAtlasRect*>(object_of(L, 3, view_ImFontAtlasRect()))) {
        lua_pushboolean(L, atlas->GetCustomRect(opt_int(L, 2, -1), out));
        return 1;
    }
    ImFontAtlasRect r;
    if (!atlas->GetCustomRect(opt_int(L, 2, -1), &r))
        return 0;
    *push_owned<ImFontAtlasRect>(L, &view_ImFontAtlasRect()) = r;
    return 1;
}

// ---- Textures --------------------------------------------------------------------------------------------------------

ImTextureData* tex_self(lua_State* L) { return self_of<ImTextureData>(L, view_ImTextureData()); }
int push_tex_id(lua_State* L, ImTextureID id)
{
    lua_pushnumber(L, static_cast<lua_Number>(id));
    return 1;
}
int tex_get_pixels(lua_State* L) { lua_pushnumber(L, static_cast<lua_Number>(reinterpret_cast<std::uintptr_t>(tex_self(L)->GetPixels()))); return 1; }
int tex_get_pixels_at(lua_State* L)
{
    ImTextureData* tex = tex_self(L);
    const int x = opt_int(L, 2, 0), y = opt_int(L, 3, 0);
    if (tex->Pixels == nullptr || x < 0 || y < 0 || x >= tex->Width || y >= tex->Height)
        return 0;
    lua_pushnumber(L, static_cast<lua_Number>(reinterpret_cast<std::uintptr_t>(tex->GetPixelsAt(x, y))));
    return 1;
}
int tex_get_size_in_bytes(lua_State* L) { lua_pushnumber(L, tex_self(L)->GetSizeInBytes()); return 1; }
int tex_get_pitch(lua_State* L) { lua_pushnumber(L, tex_self(L)->GetPitch()); return 1; }
int tex_get_tex_id(lua_State* L) { return push_tex_id(L, tex_self(L)->GetTexID()); }
int tex_get_tex_ref(lua_State* L)
{
    *push_owned<ImTextureRef>(L, &view_ImTextureRef()) = tex_self(L)->GetTexRef();
    return 1;
}
int texref_get_tex_id(lua_State* L)
{
    const ImTextureRef* ref = self_of<ImTextureRef>(L, view_ImTextureRef());
    return push_tex_id(L, ref->_TexData != nullptr ? ref->_TexData->TexID : ref->_TexID);
}
int drawcmd_get_tex_id(lua_State* L)
{
    const ImDrawCmd* cmd = self_of<ImDrawCmd>(L, view_ImDrawCmd());
    return push_tex_id(L, cmd->TexRef._TexData != nullptr ? cmd->TexRef._TexData->TexID : cmd->TexRef._TexID);
}

// ---- Viewports, style, IO ----------------------------------------------------------------------------------------------

int viewport_get_center(lua_State* L) { return push_vec2(L, self_of<ImGuiViewport>(L, view_ImGuiViewport())->GetCenter()); }
int viewport_get_work_center(lua_State* L) { return push_vec2(L, self_of<ImGuiViewport>(L, view_ImGuiViewport())->GetWorkCenter()); }

int style_scale_all_sizes(lua_State* L)
{
    self_of<ImGuiStyle>(L, view_ImGuiStyle())->ScaleAllSizes(opt_float(L, 2, 1.0f));
    return 0;
}

ImGuiIO* io_self(lua_State* L) { return self_of<ImGuiIO>(L, view_ImGuiIO()); }
int io_add_key_event(lua_State* L) { io_self(L)->AddKeyEvent(opt_key(L, 2, ImGuiKey_None), opt_bool(L, 3, false)); return 0; }
int io_add_key_analog_event(lua_State* L)
{
    io_self(L)->AddKeyAnalogEvent(opt_key(L, 2, ImGuiKey_None), opt_bool(L, 3, false), opt_float(L, 4, 0));
    return 0;
}
int io_add_mouse_pos_event(lua_State* L) { io_self(L)->AddMousePosEvent(opt_float(L, 2, 0), opt_float(L, 3, 0)); return 0; }
int io_add_mouse_button_event(lua_State* L) { io_self(L)->AddMouseButtonEvent(opt_int(L, 2, 0), opt_bool(L, 3, false)); return 0; }
int io_add_mouse_wheel_event(lua_State* L) { io_self(L)->AddMouseWheelEvent(opt_float(L, 2, 0), opt_float(L, 3, 0)); return 0; }
int io_add_mouse_source_event(lua_State* L) { io_self(L)->AddMouseSourceEvent(static_cast<ImGuiMouseSource>(opt_int(L, 2, 0))); return 0; }
int io_add_mouse_viewport_event(lua_State* L) { io_self(L)->AddMouseViewportEvent(opt_u32(L, 2, 0)); return 0; }
int io_add_focus_event(lua_State* L) { io_self(L)->AddFocusEvent(opt_bool(L, 2, true)); return 0; }
int io_add_input_character(lua_State* L) { io_self(L)->AddInputCharacter(opt_u32(L, 2, 0)); return 0; }
int io_add_input_character_utf16(lua_State* L) { io_self(L)->AddInputCharacterUTF16(static_cast<ImWchar16>(opt_int(L, 2, 0))); return 0; }
int io_add_input_characters_utf8(lua_State* L) { io_self(L)->AddInputCharactersUTF8(opt_string(L, 2, "")); return 0; }
int io_set_key_event_native_data(lua_State* L)
{
    io_self(L)->SetKeyEventNativeData(opt_key(L, 2, ImGuiKey_None), opt_int(L, 3, 0), opt_int(L, 4, 0), opt_int(L, 5, -1));
    return 0;
}
int io_set_app_accepting_events(lua_State* L) { io_self(L)->SetAppAcceptingEvents(opt_bool(L, 2, true)); return 0; }
int io_clear_events_queue(lua_State* L) { io_self(L)->ClearEventsQueue(); return 0; }
int io_clear_input_keys(lua_State* L) { io_self(L)->ClearInputKeys(); return 0; }
int io_clear_input_mouse(lua_State* L) { io_self(L)->ClearInputMouse(); return 0; }

// ---- InputText's callback data ------------------------------------------------------------------------------------------

ImGuiInputTextCallbackData* input_self(lua_State* L) { return self_of<ImGuiInputTextCallbackData>(L, view_ImGuiInputTextCallbackData()); }
int input_delete_chars(lua_State* L)
{
    ImGuiInputTextCallbackData* d = input_self(L);
    const int pos = opt_int(L, 2, 0), count = opt_int(L, 3, 0);
    if (pos >= 0 && count >= 0 && pos + count <= d->BufTextLen)
        d->DeleteChars(pos, count);
    return 0;
}
int input_insert_chars(lua_State* L)
{
    ImGuiInputTextCallbackData* d = input_self(L);
    const int pos = opt_int(L, 2, 0);
    if (pos >= 0 && pos <= d->BufTextLen)
        d->InsertChars(pos, opt_string(L, 3, ""));
    return 0;
}
int input_select_all(lua_State* L) { input_self(L)->SelectAll(); return 0; }
int input_clear_selection(lua_State* L) { input_self(L)->ClearSelection(); return 0; }
int input_has_selection(lua_State* L) { lua_pushboolean(L, input_self(L)->HasSelection()); return 1; }

// ---- Payloads, clippers, storage, text buffers and filters, selection --------------------------------------------------

const ImGuiPayload* payload_self(lua_State* L) { return self_of<ImGuiPayload>(L, view_ImGuiPayload()); }
int payload_is_data_type(lua_State* L) { lua_pushboolean(L, payload_self(L)->IsDataType(opt_string(L, 2, ""))); return 1; }
int payload_is_preview(lua_State* L) { lua_pushboolean(L, payload_self(L)->IsPreview()); return 1; }
int payload_is_delivery(lua_State* L) { lua_pushboolean(L, payload_self(L)->IsDelivery()); return 1; }
// The payload's bytes as a string (SetDragDropPayload took a string or a number)
int payload_data_string(lua_State* L)
{
    const ImGuiPayload* p = payload_self(L);
    if (p->Data == nullptr || p->DataSize <= 0)
        return 0;
    lua_pushlstring(L, static_cast<const char*>(p->Data), static_cast<std::size_t>(p->DataSize));
    return 1;
}

ImGuiListClipper* clipper_self(lua_State* L) { return self_of<ImGuiListClipper>(L, view_ImGuiListClipper()); }
int clipper_begin(lua_State* L)
{
    if (gui::in_frame())
        clipper_self(L)->Begin(opt_int(L, 2, 0), opt_float(L, 3, -1.0f));
    return 0;
}
int clipper_end(lua_State* L)
{
    ImGuiListClipper* c = clipper_self(L);
    if (c->Ctx != nullptr)
        c->End();
    return 0;
}
int clipper_step(lua_State* L)
{
    ImGuiListClipper* c = clipper_self(L);
    lua_pushboolean(L, gui::in_frame() && c->Ctx != nullptr && c->Step());
    return 1;
}
int clipper_include_item(lua_State* L) { clipper_self(L)->IncludeItemByIndex(opt_int(L, 2, 0)); return 0; }
int clipper_include_items(lua_State* L) { clipper_self(L)->IncludeItemsByIndex(opt_int(L, 2, 0), opt_int(L, 3, 0)); return 0; }
int clipper_seek_cursor(lua_State* L)
{
    if (gui::in_frame() && clipper_self(L)->Ctx != nullptr)
        clipper_self(L)->SeekCursorForItem(opt_int(L, 2, 0));
    return 0;
}

ImGuiStorage* storage_self(lua_State* L) { return self_of<ImGuiStorage>(L, view_ImGuiStorage()); }
int storage_clear(lua_State* L) { storage_self(L)->Clear(); return 0; }
int storage_get_int(lua_State* L) { lua_pushnumber(L, storage_self(L)->GetInt(opt_u32(L, 2, 0), opt_int(L, 3, 0))); return 1; }
int storage_set_int(lua_State* L) { storage_self(L)->SetInt(opt_u32(L, 2, 0), opt_int(L, 3, 0)); return 0; }
int storage_get_bool(lua_State* L) { lua_pushboolean(L, storage_self(L)->GetBool(opt_u32(L, 2, 0), opt_bool(L, 3, false))); return 1; }
int storage_set_bool(lua_State* L) { storage_self(L)->SetBool(opt_u32(L, 2, 0), opt_bool(L, 3, false)); return 0; }
int storage_get_float(lua_State* L) { lua_pushnumber(L, storage_self(L)->GetFloat(opt_u32(L, 2, 0), opt_float(L, 3, 0))); return 1; }
int storage_set_float(lua_State* L) { storage_self(L)->SetFloat(opt_u32(L, 2, 0), opt_float(L, 3, 0)); return 0; }
int storage_get_void_ptr(lua_State* L)
{
    lua_pushnumber(L, static_cast<lua_Number>(reinterpret_cast<std::uintptr_t>(storage_self(L)->GetVoidPtr(opt_u32(L, 2, 0)))));
    return 1;
}
int storage_set_void_ptr(lua_State* L) { storage_self(L)->SetVoidPtr(opt_u32(L, 2, 0), opt_pointer(L, 3)); return 0; }
// (a reference can't reach Lua: the value, inserted if it wasn't there, as ImGui's does)
int storage_get_int_ref(lua_State* L) { lua_pushnumber(L, *storage_self(L)->GetIntRef(opt_u32(L, 2, 0), opt_int(L, 3, 0))); return 1; }
int storage_get_bool_ref(lua_State* L) { lua_pushboolean(L, *storage_self(L)->GetBoolRef(opt_u32(L, 2, 0), opt_bool(L, 3, false))); return 1; }
int storage_get_float_ref(lua_State* L) { lua_pushnumber(L, *storage_self(L)->GetFloatRef(opt_u32(L, 2, 0), opt_float(L, 3, 0))); return 1; }
int storage_build_sort_by_key(lua_State* L) { storage_self(L)->BuildSortByKey(); return 0; }
int storage_set_all_int(lua_State* L) { storage_self(L)->SetAllInt(opt_int(L, 2, 0)); return 0; }

ImGuiTextBuffer* textbuf_self(lua_State* L) { return self_of<ImGuiTextBuffer>(L, view_ImGuiTextBuffer()); }
int textbuf_begin(lua_State* L) { lua_pushstring(L, textbuf_self(L)->c_str()); return 1; }
int textbuf_end(lua_State* L) { (void)textbuf_self(L); lua_pushstring(L, ""); return 1; }
int textbuf_size(lua_State* L) { lua_pushnumber(L, textbuf_self(L)->size()); return 1; }
int textbuf_empty(lua_State* L) { lua_pushboolean(L, textbuf_self(L)->empty()); return 1; }
int textbuf_clear(lua_State* L) { textbuf_self(L)->clear(); return 0; }
int textbuf_resize(lua_State* L)
{
    const int n = opt_int(L, 2, 0);
    if (n >= 0)
        textbuf_self(L)->resize(n);
    return 0;
}
int textbuf_reserve(lua_State* L)
{
    const int n = opt_int(L, 2, 0);
    if (n >= 0)
        textbuf_self(L)->reserve(n);
    return 0;
}
int textbuf_append(lua_State* L)
{
    std::size_t length = 0;
    const char* text = lua_type(L, 2) == LUA_TSTRING || lua_type(L, 2) == LUA_TNUMBER ? lua_tolstring(L, 2, &length) : "";
    textbuf_self(L)->append(text, text + length);
    return 0;
}

ImGuiTextFilter* filter_self(lua_State* L) { return self_of<ImGuiTextFilter>(L, view_ImGuiTextFilter()); }
int filter_draw(lua_State* L)
{
    ImGuiTextFilter* f = filter_self(L);
    lua_pushboolean(L, gui::in_frame() && f->Draw(opt_string(L, 2, "Filter (inc,-exc)"), opt_float(L, 3, 0.0f)));
    return 1;
}
int filter_pass_filter(lua_State* L) { lua_pushboolean(L, filter_self(L)->PassFilter(opt_string(L, 2, ""))); return 1; }
int filter_build(lua_State* L) { filter_self(L)->Build(); return 0; }
int filter_clear(lua_State* L) { filter_self(L)->Clear(); return 0; }
int filter_is_active(lua_State* L) { lua_pushboolean(L, filter_self(L)->IsActive()); return 1; }
// InputBuf as a string, and set from one (then built)
void filter_input_buf(lua_State* L, const void* base) { lua_pushstring(L, static_cast<const ImGuiTextFilter*>(base)->InputBuf); }
void filter_set_input_buf(lua_State* L, void* base, int i)
{
    auto* f = static_cast<ImGuiTextFilter*>(base);
    ImStrncpy(f->InputBuf, opt_string(L, i, ""), IM_ARRAYSIZE(f->InputBuf));
    f->Build();
}

ImGuiMultiSelectIO* opt_ms_io(lua_State* L, int i) { return static_cast<ImGuiMultiSelectIO*>(object_of(L, i, view_ImGuiMultiSelectIO())); }
ImGuiSelectionBasicStorage* selection_self(lua_State* L) { return self_of<ImGuiSelectionBasicStorage>(L, view_ImGuiSelectionBasicStorage()); }
int selection_apply_requests(lua_State* L)
{
    ImGuiSelectionBasicStorage* s = selection_self(L);
    if (ImGuiMultiSelectIO* io = opt_ms_io(L, 2))
        s->ApplyRequests(io);
    return 0;
}
int selection_contains(lua_State* L) { lua_pushboolean(L, selection_self(L)->Contains(opt_u32(L, 2, 0))); return 1; }
int selection_clear(lua_State* L) { selection_self(L)->Clear(); return 0; }
int selection_swap(lua_State* L)
{
    ImGuiSelectionBasicStorage* s = selection_self(L);
    if (auto* other = static_cast<ImGuiSelectionBasicStorage*>(object_of(L, 2, view_ImGuiSelectionBasicStorage())))
        s->Swap(*other);
    return 0;
}
int selection_set_item_selected(lua_State* L) { selection_self(L)->SetItemSelected(opt_u32(L, 2, 0), opt_bool(L, 3, true)); return 0; }
// GetNextSelectedItem(it, out_id): it and out_id tables ({}: start); it[1] carries the position
int selection_get_next_selected_item(lua_State* L)
{
    ImGuiSelectionBasicStorage* s = selection_self(L);
    void* it = nullptr;
    if (lua_istable(L, 2)) {
        lua_rawgeti(L, 2, 1);
        it = lua_isnumber(L, -1) ? reinterpret_cast<void*>(static_cast<std::uintptr_t>(lua_tonumber(L, -1))) : nullptr;
        lua_pop(L, 1);
    }
    ImGuiID id = 0;
    const bool found = s->GetNextSelectedItem(&it, &id);
    if (lua_istable(L, 2)) {
        lua_pushnumber(L, static_cast<lua_Number>(reinterpret_cast<std::uintptr_t>(it)));
        lua_rawseti(L, 2, 1);
    }
    if (lua_istable(L, 3)) {
        lua_pushnumber(L, id);
        lua_rawseti(L, 3, 1);
    }
    lua_pushboolean(L, found);
    lua_pushnumber(L, id);
    return 2;
}
int selection_get_storage_id_from_index(lua_State* L) { lua_pushnumber(L, selection_self(L)->GetStorageIdFromIndex(opt_int(L, 2, 0))); return 1; }

// ImGuiSelectionExternalStorage: AdapterSetItemSelected is a Lua function (self, index, selected), kept by the object
struct ExternalSelection {
    ImGuiSelectionExternalStorage storage;
    lua_State* L = nullptr;
    int function = LUA_NOREF;
};
void external_set_item_selected(ImGuiSelectionExternalStorage* self, int index, bool selected)
{
    auto* e = reinterpret_cast<ExternalSelection*>(self);
    if (e->L == nullptr || e->function == LUA_NOREF)
        return;
    lua_rawgeti(e->L, LUA_REGISTRYINDEX, e->function);
    lua_pushinteger(e->L, index);
    lua_pushboolean(e->L, selected);
    if (lua_pcall(e->L, 2, 0, 0) != 0)
        lua_pop(e->L, 1);
}
int external_apply_requests(lua_State* L)
{
    auto* s = self_of<ImGuiSelectionExternalStorage>(L, view_ImGuiSelectionExternalStorage());
    if (ImGuiMultiSelectIO* io = opt_ms_io(L, 2))
        s->ApplyRequests(io);
    return 0;
}
void external_set_adapter(lua_State* L, void* base, int i)
{
    auto* e = static_cast<ExternalSelection*>(base);
    if (e->function != LUA_NOREF)
        luaL_unref(L, LUA_REGISTRYINDEX, e->function);
    e->function = LUA_NOREF;
    if (lua_isfunction(L, i)) {
        lua_pushvalue(L, i);
        e->function = luaL_ref(L, LUA_REGISTRYINDEX);
        e->L = L;
    }
}

ImFontGlyphRangesBuilder* ranges_self(lua_State* L) { return self_of<ImFontGlyphRangesBuilder>(L, view_ImFontGlyphRangesBuilder()); }
int ranges_clear(lua_State* L) { ranges_self(L)->Clear(); return 0; }
int ranges_get_bit(lua_State* L) { lua_pushboolean(L, ranges_self(L)->GetBit(opt_u32(L, 2, 0) & 0xFFFF)); return 1; }
int ranges_set_bit(lua_State* L) { ranges_self(L)->SetBit(opt_u32(L, 2, 0) & 0xFFFF); return 0; }
int ranges_add_char(lua_State* L) { ranges_self(L)->AddChar(static_cast<ImWchar>(opt_int(L, 2, 0))); return 0; }
int ranges_add_text(lua_State* L) { ranges_self(L)->AddText(opt_string(L, 2, "")); return 0; }
// AddRanges({first, last, first, last, ...}) or a pointer to ImWchar pairs ending in 0
int ranges_add_ranges(lua_State* L)
{
    ImFontGlyphRangesBuilder* b = ranges_self(L);
    if (lua_istable(L, 2)) {
        std::vector<ImWchar> list;
        const int n = static_cast<int>(lua_objlen(L, 2));
        for (int k = 1; k <= n; ++k) {
            lua_rawgeti(L, 2, k);
            list.push_back(static_cast<ImWchar>(lua_isnumber(L, -1) ? lua_tonumber(L, -1) : 0));
            lua_pop(L, 1);
        }
        list.push_back(0);
        b->AddRanges(list.data());
    } else if (const auto* ranges = static_cast<const ImWchar*>(opt_pointer(L, 2))) {
        b->AddRanges(ranges);
    }
    return 0;
}
// BuildRanges(): the ranges as a table of numbers (first, last, ...)
int ranges_build_ranges(lua_State* L)
{
    ImVector<ImWchar> out;
    ranges_self(L)->BuildRanges(&out);
    lua_createtable(L, out.Size, 0);
    for (int k = 0; k < out.Size && out[k] != 0; ++k) {
        lua_pushnumber(L, out[k]);
        lua_rawseti(L, -2, k + 1);
    }
    return 1;
}

ImDrawData* drawdata_self(lua_State* L) { return self_of<ImDrawData>(L, view_ImDrawData()); }
int drawdata_scale_clip_rects(lua_State* L) { drawdata_self(L)->ScaleClipRects(opt_vec2(L, 2, ImVec2(1, 1))); return 0; }
int drawdata_de_index_all_buffers(lua_State* L) { drawdata_self(L)->DeIndexAllBuffers(); return 0; }

ImDrawListSplitter* splitter_self(lua_State* L) { return self_of<ImDrawListSplitter>(L, view_ImDrawListSplitter()); }
int splitter_clear(lua_State* L) { splitter_self(L)->Clear(); return 0; }
int splitter_clear_free_memory(lua_State* L) { splitter_self(L)->ClearFreeMemory(); return 0; }
int splitter_split(lua_State* L)
{
    ImDrawListSplitter* s = splitter_self(L);
    if (auto* list = static_cast<ImDrawList*>(object_of(L, 2, view_ImDrawList())))
        s->Split(list, opt_int(L, 3, 1));
    return 0;
}
int splitter_merge(lua_State* L)
{
    ImDrawListSplitter* s = splitter_self(L);
    if (auto* list = static_cast<ImDrawList*>(object_of(L, 2, view_ImDrawList())))
        s->Merge(list);
    return 0;
}
int splitter_set_current_channel(lua_State* L)
{
    ImDrawListSplitter* s = splitter_self(L);
    auto* list = static_cast<ImDrawList*>(object_of(L, 2, view_ImDrawList()));
    const int n = opt_int(L, 3, 0);
    if (list != nullptr && n >= 0 && n < s->_Count)
        s->SetCurrentChannel(list, n);
    return 0;
}

// ---- What 1.81 addons read that 1.92 dropped or renamed ----------------------------------------------------------------

void io_key_map(lua_State* L, const void*)
{
    lua_pushnumber(L, legacy::legacy_key_vk(static_cast<int>(lua_tointeger(L, -1))));
}
void io_keys_down(lua_State* L, const void*)
{
    const ImGuiKey key = legacy::vk_key(static_cast<int>(lua_tointeger(L, -1)));
    lua_pushboolean(L, key != ImGuiKey_None && ImGui::GetCurrentContext() != nullptr && ImGui::IsKeyDown(key));
}
void io_keys_down_duration(lua_State* L, const void*)
{
    const ImGuiKey key = legacy::vk_key(static_cast<int>(lua_tointeger(L, -1)));
    const ImGuiKeyData* data = key != ImGuiKey_None && ImGui::GetCurrentContext() != nullptr ? ImGui::GetKeyData(key) : nullptr;
    lua_pushnumber(L, data != nullptr ? data->DownDuration : -1.0f);
}
void zero(lua_State* L, const void*) { lua_pushnumber(L, 0); }

void font_size(lua_State* L, const void* b) { lua_pushnumber(L, static_cast<const ImFont*>(b)->LegacySize); }
void font_ascent(lua_State* L, const void* b)
{
    const ImFontBaked* baked = legacy_baked(static_cast<const ImFont*>(b));
    lua_pushnumber(L, baked != nullptr ? baked->Ascent : 0.0f);
}
void font_descent(lua_State* L, const void* b)
{
    const ImFontBaked* baked = legacy_baked(static_cast<const ImFont*>(b));
    lua_pushnumber(L, baked != nullptr ? baked->Descent : 0.0f);
}
void font_fallback_advance(lua_State* L, const void* b)
{
    const ImFontBaked* baked = legacy_baked(static_cast<const ImFont*>(b));
    lua_pushnumber(L, baked != nullptr ? baked->FallbackAdvanceX : 0.0f);
}
void font_config_count(lua_State* L, const void* b) { lua_pushnumber(L, static_cast<const ImFont*>(b)->Sources.Size); }
void atlas_tex_width(lua_State* L, const void* b)
{
    const auto* a = static_cast<const ImFontAtlas*>(b);
    lua_pushnumber(L, a->TexData != nullptr ? a->TexData->Width : 0);
}
void atlas_tex_height(lua_State* L, const void* b)
{
    const auto* a = static_cast<const ImFontAtlas*>(b);
    lua_pushnumber(L, a->TexData != nullptr ? a->TexData->Height : 0);
}
void atlas_tex_id(lua_State* L, const void* b)
{
    const ImTextureRef& ref = static_cast<const ImFontAtlas*>(b)->TexRef;
    lua_pushnumber(L, static_cast<lua_Number>(ref._TexData != nullptr ? ref._TexData->TexID : ref._TexID));
}
void style_tab_min_width(lua_State* L, const void* b) { lua_pushnumber(L, static_cast<const ImGuiStyle*>(b)->TabCloseButtonMinWidthUnselected); }
void style_set_tab_min_width(lua_State* L, void* b, int i) { static_cast<ImGuiStyle*>(b)->TabCloseButtonMinWidthUnselected = opt_float(L, i, 0); }
void style_circle_error(lua_State* L, const void* b) { lua_pushnumber(L, static_cast<const ImGuiStyle*>(b)->CircleTessellationMaxError); }
void style_set_circle_error(lua_State* L, void* b, int i) { static_cast<ImGuiStyle*>(b)->CircleTessellationMaxError = opt_float(L, i, 0.3f); }
void sort_direction(lua_State* L, const void* b) { lua_pushinteger(L, static_cast<const ImGuiTableColumnSortSpecs*>(b)->SortDirection); }

Field* field_of(View& view, const char* name)
{
    for (Field& f : view.fields)
        if (std::strcmp(f.name, name) == 0)
            return &f;
    return nullptr;
}
void add(View& view, const luaL_Reg* methods)
{
    for (const luaL_Reg* m = methods; m->name != nullptr; ++m)
        view.fields.push_back(method(m->name, m->func));
}
void legacy_enum(View& view, const char* name, int (*from)(int), int (*to)(int))
{
    if (Field* f = field_of(view, name))
        f->from_181 = from, f->to_181 = to;
}
int key_in_int(int v) { return legacy::key_in(v); }
int key_out_int(int v) { return legacy::legacy_key_of(static_cast<ImGuiKey>(v)); }

// The views' methods and 1.81's fields, once.
void build_views()
{
    static bool built = false;
    if (built)
        return;
    built = true;
    fill_views();

    View& io = view_ImGuiIO();
    legacy_enum(io, "ConfigFlags", &legacy::in_ImGuiConfigFlags, &legacy::out_ImGuiConfigFlags);
    legacy_enum(io, "BackendFlags", &legacy::in_ImGuiBackendFlags, &legacy::out_ImGuiBackendFlags);
    legacy_enum(io, "KeyMods", &legacy::in_ImGuiKeyModFlags, &legacy::out_ImGuiKeyModFlags);
    io.fields.push_back(virtual_array("KeyMap", legacy::kLegacyKeyCount, &io_key_map));
    io.fields.push_back(virtual_array("KeysDown", 512, &io_keys_down));
    io.fields.push_back(virtual_array("KeysDownDuration", 512, &io_keys_down_duration));
    io.fields.push_back(virtual_array("NavInputs", 21, &zero));
    io.fields.push_back(getter("MetricsActiveAllocations", &zero));
    static const luaL_Reg kIo[] = {
        {"AddKeyEvent", &io_add_key_event}, {"AddKeyAnalogEvent", &io_add_key_analog_event},
        {"AddMousePosEvent", &io_add_mouse_pos_event}, {"AddMouseButtonEvent", &io_add_mouse_button_event},
        {"AddMouseWheelEvent", &io_add_mouse_wheel_event}, {"AddMouseSourceEvent", &io_add_mouse_source_event},
        {"AddMouseViewportEvent", &io_add_mouse_viewport_event}, {"AddFocusEvent", &io_add_focus_event},
        {"AddInputCharacter", &io_add_input_character}, {"AddInputCharacterUTF16", &io_add_input_character_utf16},
        {"AddInputCharactersUTF8", &io_add_input_characters_utf8}, {"SetKeyEventNativeData", &io_set_key_event_native_data},
        {"SetAppAcceptingEvents", &io_set_app_accepting_events}, {"ClearEventsQueue", &io_clear_events_queue},
        {"ClearInputKeys", &io_clear_input_keys}, {"ClearInputMouse", &io_clear_input_mouse}, {nullptr, nullptr},
    };
    add(io, kIo);

    View& style = view_ImGuiStyle();
    if (Field* colours = field_of(style, "Colors"))
        colours->from_181 = &legacy::in_ImGuiCol; // Colors[ImGuiCol_X + 1] with 1.81's ImGuiCol_X
    style.fields.push_back(getter("TabMinWidthForCloseButton", &style_tab_min_width, &style_set_tab_min_width));
    style.fields.push_back(getter("CircleSegmentMaxError", &style_circle_error, &style_set_circle_error));
    static const luaL_Reg kStyle[] = {{"ScaleAllSizes", &style_scale_all_sizes}, {nullptr, nullptr}};
    add(style, kStyle);

    View& font = view_ImFont();
    font.fields.push_back(getter("FontSize", &font_size));
    font.fields.push_back(getter("Ascent", &font_ascent));
    font.fields.push_back(getter("Descent", &font_descent));
    font.fields.push_back(getter("FallbackAdvanceX", &font_fallback_advance));
    font.fields.push_back(getter("ConfigDataCount", &font_config_count));
    static const luaL_Reg kFont[] = {
        {"IsGlyphInFont", &font_is_glyph_in_font}, {"IsLoaded", &font_is_loaded}, {"GetDebugName", &font_get_debug_name},
        {"GetFontBaked", &font_get_font_baked}, {"CalcTextSizeA", &font_calc_text_size_a},
        {"CalcWordWrapPosition", &font_calc_word_wrap_position}, {"RenderChar", &font_render_char}, {"RenderText", &font_render_text},
        {"ClearOutputData", &refused}, {"AddRemapChar", &font_add_remap_char}, {"IsGlyphRangeUnused", &font_is_glyph_range_unused},
        {nullptr, nullptr},
    };
    add(font, kFont);

    static const luaL_Reg kBaked[] = {
        {"ClearOutputData", &refused}, {"FindGlyph", &baked_find_glyph}, {"FindGlyphNoFallback", &baked_find_glyph_no_fallback},
        {"GetCharAdvance", &baked_get_char_advance}, {"IsGlyphLoaded", &baked_is_glyph_loaded}, {nullptr, nullptr},
    };
    add(view_ImFontBaked(), kBaked);

    View& atlas = view_ImFontAtlas();
    atlas.fields.push_back(getter("TexWidth", &atlas_tex_width));
    atlas.fields.push_back(getter("TexHeight", &atlas_tex_height));
    atlas.fields.push_back(getter("TexID", &atlas_tex_id));
    atlas.fields.push_back(getter("TexDesiredWidth", &zero));
    static const luaL_Reg kAtlas[] = {
        {"AddFont", &atlas_add_font}, {"AddFontDefault", &atlas_add_font_default}, {"AddFontFromFileTTF", &atlas_add_font_from_file_ttf},
        {"AddFontFromMemoryTTF", &atlas_add_font_from_memory_ttf}, {"AddFontFromMemoryCompressedTTF", &atlas_add_font_from_memory_compressed_ttf},
        {"AddFontFromMemoryCompressedBase85TTF", &atlas_add_font_from_memory_base85}, {"RemoveFont", &atlas_remove_font},
        {"Clear", &refused}, {"CompactCache", &atlas_compact_cache}, {"SetFontLoader", &refused}, {"ClearInputData", &refused},
        {"ClearFonts", &refused}, {"ClearTexData", &refused}, {"GetGlyphRangesDefault", &atlas_get_glyph_ranges_default},
        {"AddCustomRect", &atlas_add_custom_rect}, {"RemoveCustomRect", &atlas_remove_custom_rect}, {"GetCustomRect", &atlas_get_custom_rect},
        {nullptr, nullptr},
    };
    add(atlas, kAtlas);

    static const luaL_Reg kTexture[] = {
        {"Create", &refused}, {"DestroyPixels", &refused}, {"GetPixels", &tex_get_pixels}, {"GetPixelsAt", &tex_get_pixels_at},
        {"GetSizeInBytes", &tex_get_size_in_bytes}, {"GetPitch", &tex_get_pitch}, {"GetTexRef", &tex_get_tex_ref},
        {"GetTexID", &tex_get_tex_id}, {"SetTexID", &refused}, {"SetStatus", &refused}, {nullptr, nullptr},
    };
    add(view_ImTextureData(), kTexture);
    static const luaL_Reg kTexRef[] = {{"GetTexID", &texref_get_tex_id}, {nullptr, nullptr}};
    add(view_ImTextureRef(), kTexRef);
    static const luaL_Reg kDrawCmd[] = {{"GetTexID", &drawcmd_get_tex_id}, {nullptr, nullptr}};
    add(view_ImDrawCmd(), kDrawCmd);
    static const luaL_Reg kDrawData[] = {
        {"Clear", &refused}, {"AddDrawList", &refused}, {"DeIndexAllBuffers", &drawdata_de_index_all_buffers},
        {"ScaleClipRects", &drawdata_scale_clip_rects}, {nullptr, nullptr},
    };
    add(view_ImDrawData(), kDrawData);
    static const luaL_Reg kSplitter[] = {
        {"Clear", &splitter_clear}, {"ClearFreeMemory", &splitter_clear_free_memory}, {"Split", &splitter_split},
        {"Merge", &splitter_merge}, {"SetCurrentChannel", &splitter_set_current_channel}, {nullptr, nullptr},
    };
    add(view_ImDrawListSplitter(), kSplitter);
    add(view_ImDrawList(), kDrawListMethods);

    static const luaL_Reg kViewport[] = {{"GetCenter", &viewport_get_center}, {"GetWorkCenter", &viewport_get_work_center}, {nullptr, nullptr}};
    add(view_ImGuiViewport(), kViewport);

    View& input = view_ImGuiInputTextCallbackData();
    legacy_enum(input, "EventFlag", &legacy::in_ImGuiInputTextFlags, &legacy::out_ImGuiInputTextFlags);
    legacy_enum(input, "Flags", &legacy::in_ImGuiInputTextFlags, &legacy::out_ImGuiInputTextFlags);
    legacy_enum(input, "EventKey", &key_in_int, &key_out_int);
    static const luaL_Reg kInput[] = {
        {"DeleteChars", &input_delete_chars}, {"InsertChars", &input_insert_chars}, {"SelectAll", &input_select_all},
        {"ClearSelection", &input_clear_selection}, {"HasSelection", &input_has_selection}, {nullptr, nullptr},
    };
    add(input, kInput);

    static const luaL_Reg kPayload[] = {
        {"Clear", &refused}, {"IsDataType", &payload_is_data_type}, {"IsPreview", &payload_is_preview},
        {"IsDelivery", &payload_is_delivery}, {"GetDataString", &payload_data_string}, {nullptr, nullptr},
    };
    add(view_ImGuiPayload(), kPayload);
    static const luaL_Reg kClipper[] = {
        {"Begin", &clipper_begin}, {"End", &clipper_end}, {"Step", &clipper_step}, {"IncludeItemByIndex", &clipper_include_item},
        {"IncludeItemsByIndex", &clipper_include_items}, {"IncludeRangeByIndices", &clipper_include_items},
        {"SeekCursorForItem", &clipper_seek_cursor}, {nullptr, nullptr},
    };
    add(view_ImGuiListClipper(), kClipper);
    static const luaL_Reg kStorage[] = {
        {"Clear", &storage_clear}, {"GetInt", &storage_get_int}, {"SetInt", &storage_set_int}, {"GetBool", &storage_get_bool},
        {"SetBool", &storage_set_bool}, {"GetFloat", &storage_get_float}, {"SetFloat", &storage_set_float},
        {"GetVoidPtr", &storage_get_void_ptr}, {"SetVoidPtr", &storage_set_void_ptr}, {"GetIntRef", &storage_get_int_ref},
        {"GetBoolRef", &storage_get_bool_ref}, {"GetFloatRef", &storage_get_float_ref}, {"BuildSortByKey", &storage_build_sort_by_key},
        {"SetAllInt", &storage_set_all_int}, {nullptr, nullptr},
    };
    add(view_ImGuiStorage(), kStorage);
    static const luaL_Reg kTextBuffer[] = {
        {"begin", &textbuf_begin}, {"end", &textbuf_end}, {"size", &textbuf_size}, {"empty", &textbuf_empty},
        {"clear", &textbuf_clear}, {"resize", &textbuf_resize}, {"reserve", &textbuf_reserve}, {"c_str", &textbuf_begin},
        {"append", &textbuf_append}, {nullptr, nullptr},
    };
    add(view_ImGuiTextBuffer(), kTextBuffer);
    View& filter = view_ImGuiTextFilter();
    for (Field& f : filter.fields)
        if (std::strcmp(f.name, "InputBuf") == 0)
            f = getter("InputBuf", &filter_input_buf, &filter_set_input_buf);
    static const luaL_Reg kFilter[] = {
        {"Draw", &filter_draw}, {"PassFilter", &filter_pass_filter}, {"Build", &filter_build}, {"Clear", &filter_clear},
        {"IsActive", &filter_is_active}, {nullptr, nullptr},
    };
    add(filter, kFilter);
    static const luaL_Reg kSelection[] = {
        {"ApplyRequests", &selection_apply_requests}, {"Contains", &selection_contains}, {"Clear", &selection_clear},
        {"Swap", &selection_swap}, {"SetItemSelected", &selection_set_item_selected},
        {"GetNextSelectedItem", &selection_get_next_selected_item}, {"GetStorageIdFromIndex", &selection_get_storage_id_from_index},
        {nullptr, nullptr},
    };
    add(view_ImGuiSelectionBasicStorage(), kSelection);
    View& external = view_ImGuiSelectionExternalStorage();
    external.fields.push_back(getter("AdapterSetItemSelected", [](lua_State* L, const void*) { lua_pushnil(L); }, &external_set_adapter));
    static const luaL_Reg kExternal[] = {{"ApplyRequests", &external_apply_requests}, {nullptr, nullptr}};
    add(external, kExternal);
    static const luaL_Reg kRanges[] = {
        {"Clear", &ranges_clear}, {"GetBit", &ranges_get_bit}, {"SetBit", &ranges_set_bit}, {"AddChar", &ranges_add_char},
        {"AddText", &ranges_add_text}, {"AddRanges", &ranges_add_ranges}, {"BuildRanges", &ranges_build_ranges}, {nullptr, nullptr},
    };
    add(view_ImFontGlyphRangesBuilder(), kRanges);
    if (Field* f = field_of(view_ImGuiTableColumnSortSpecs(), "SortDirection"))
        *f = getter("SortDirection", &sort_direction);
}

// ---- The structures addons make: ImGuiListClipper.new() and the like ------------------------------------------------------

template <class T>
int make(lua_State* L, View& view)
{
    push_owned<T>(L, &view);
    return 1;
}
int new_list_clipper(lua_State* L) { return make<ImGuiListClipper>(L, view_ImGuiListClipper()); }
int new_storage(lua_State* L) { return make<ImGuiStorage>(L, view_ImGuiStorage()); }
int new_text_buffer(lua_State* L) { return make<ImGuiTextBuffer>(L, view_ImGuiTextBuffer()); }
int new_window_class(lua_State* L) { return make<ImGuiWindowClass>(L, view_ImGuiWindowClass()); }
int new_selection_basic(lua_State* L) { return make<ImGuiSelectionBasicStorage>(L, view_ImGuiSelectionBasicStorage()); }
int new_font_config(lua_State* L) { return make<ImFontConfig>(L, view_ImFontConfig()); }
int new_glyph_ranges_builder(lua_State* L) { return make<ImFontGlyphRangesBuilder>(L, view_ImFontGlyphRangesBuilder()); }
int new_atlas_rect(lua_State* L) { return make<ImFontAtlasRect>(L, view_ImFontAtlasRect()); }
int new_text_filter(lua_State* L)
{
    push_owned<ImGuiTextFilter>(L, &view_ImGuiTextFilter(), opt_string(L, 1, ""));
    return 1;
}
int new_selection_external(lua_State* L)
{
    auto* e = push_owned<ExternalSelection>(L, &view_ImGuiSelectionExternalStorage());
    e->storage.AdapterSetItemSelected = &external_set_item_selected;
    return 1;
}
// ImVec2.new(x, y), ImVec4.new(x, y, z, w): tables, as every function takes them
int new_vec(lua_State* L, int n)
{
    static const char* const kNames[] = {"x", "y", "z", "w"};
    lua_createtable(L, 0, n);
    for (int k = 0; k < n; ++k) {
        lua_pushnumber(L, opt_float(L, k + 1, 0));
        lua_setfield(L, -2, kNames[k]);
    }
    return 1;
}
int new_vec2(lua_State* L) { return new_vec(L, 2); }
int new_vec4(lua_State* L) { return new_vec(L, 4); }

struct Constructor {
    const char* type = nullptr;
    lua_CFunction make = nullptr;
};
const Constructor kConstructors[] = {
    {"ImGuiListClipper", &new_list_clipper}, {"ImGuiStorage", &new_storage}, {"ImGuiTextBuffer", &new_text_buffer},
    {"ImGuiTextFilter", &new_text_filter}, {"ImGuiWindowClass", &new_window_class},
    {"ImGuiSelectionBasicStorage", &new_selection_basic}, {"ImGuiSelectionExternalStorage", &new_selection_external},
    {"ImFontConfig", &new_font_config}, {"ImFontGlyphRangesBuilder", &new_glyph_ranges_builder},
    {"ImFontAtlasRect", &new_atlas_rect}, {"ImVec2", &new_vec2}, {"ImVec4", &new_vec4},
};

// vana.imgui.types: { ImGuiListClipper = { new = ... }, ... } (compat/ashita.lua makes them globals, as Ashita's are)
void push_types(lua_State* L)
{
    lua_newtable(L);
    for (const Constructor& c : kConstructors) {
        lua_newtable(L);
        lua_pushcfunction(L, c.make);
        lua_setfield(L, -2, "new");
        lua_setfield(L, -2, c.type);
    }
}

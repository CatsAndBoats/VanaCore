/* True Font's state in one line for the log (chatfont.cpp's report, [client] truefont.debug): the interface's target and scale,
 * the fonts on, and the counts since the last report, which it starts again. Included by d3d8_chatfont.h after cf_name. */
#pragma once

void d3d8_chat_font_report(char* text, unsigned size)
{
    char target[48], told[48];
    const Obj* bb = obj(g_dev.backbuffer);
    int fonts = 0;
    for (int f = 0; f < VANA_FONTS; ++f)
        fonts |= g_cf.font[f].on << f;
    snprintf(text, size,
        "target %s (k %.3f x %.3f), the game says %s, screen %ux%u, scale %.3f x %.3f, fonts on %02x, chat drawn %u frames ago, "
        "unlaid %u, assumed %d, lost %u frames; draws %u (VanaCore's own %u, no target %u, the scene's %u, not the game's target %u, "
        "the chat's elsewhere %u, another %u), the chat's %u, letters %u (no font %u), labels %u, icons %u, kept %u, held off %u; "
        "found from the chat %u, the game %u, letters %u; covered: drawn as the game's %u, flushed to the screen %u; "
        "laid: candidates %u, followed %u, unreadable %u, elsewhere %u, assumed %u, flushed %u, lost %u; the printer's letters %u, "
        "their draws not seen %u, kept by its word alone %u",
        cf_name(g_cf.target, target, sizeof target), g_cf.k[0], g_cf.k[1],
        g_cf.told_known ? cf_name(g_cf.told ? g_cf.told : g_dev.backbuffer, told, sizeof told) : "(not read)",
        bb ? (bb->pw ? bb->pw : bb->width) : 0, bb ? (bb->ph ? bb->ph : bb->height) : 0, g_cf.sx, g_cf.sy, fonts, g_cf.chat_ago,
        g_cf.unlaid, g_cf.assume, g_cf.lost, g_cf.n.draws, g_cf.n.own, g_cf.n.no_rt, g_cf.n.scene, g_cf.n.told_other,
        g_cf.n.chat_other, g_cf.n.other, g_cf.n.chat, g_cf.n.cells, g_cf.n.cell_nofont, g_cf.n.labels, g_cf.n.icons, g_cf.n.kept,
        g_cf.n.rest, g_cf.n.learn_chat, g_cf.n.learn_told, g_cf.n.learn_cell, g_cf.n.cover_game, g_cf.n.cover_screen,
        g_cf.n.lay_seen, g_cf.n.lay_ok, g_cf.n.lay_nomap, g_cf.n.lay_elsewhere, g_cf.n.assumed, g_cf.n.flushed, g_cf.n.lost,
        g_cf.n.printed, g_cf.n.printer_nodraw, g_cf.n.rescued);
    memset(&g_cf.n, 0, sizeof g_cf.n);
    g_cf.n.say_cell = g_cf.n.say_lay = g_cf.n.say_other = g_cf.n.say_printer = g_cf.debug ? 3 : 0;
}

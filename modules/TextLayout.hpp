#pragma once

// Pure geometry shared by the dialog and its standalone regression tests.
struct SmallFontLayout_ {
    int text_height;
    int item_height;
    int log_y;
    int list_y;
    int list_height;
    int full_y;
    int ok_y;
    int window_height;
    bool fits;
};

static inline int FontTextHeight_(int requested, int font_height)
{
    const int line_height = font_height > 0 ? font_height : 17;
    return requested > line_height ? requested : line_height;
}

static inline SmallFontLayout_ MakeSmallFontLayout_(int font_height)
{
    SmallFontLayout_ layout = {};
    layout.text_height = FontTextHeight_(24, font_height);
    layout.item_height = FontTextHeight_(20, font_height);
    layout.log_y = 12 + layout.text_height + 8;
    if (layout.log_y < 44) layout.log_y = 44;
    layout.list_y = layout.log_y + layout.text_height + 2;
    layout.list_height = 5 * layout.item_height + 4;
    layout.full_y = layout.log_y + layout.text_height + 16;
    if (layout.full_y < 84) layout.full_y = 84;
    layout.ok_y = layout.full_y + layout.text_height + 32;
    if (layout.ok_y < 140) layout.ok_y = 140;
    layout.window_height = layout.list_y + layout.list_height + 16;
    if (layout.window_height < layout.ok_y + 50)
        layout.window_height = layout.ok_y + 50;
    if (layout.window_height < 190) layout.window_height = 190;
    // Preserve a bounded tool window; larger replacement fonts need new assets/layout.
    layout.fits = layout.text_height <= 32 && layout.window_height <= 272;
    return layout;
}

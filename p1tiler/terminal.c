



 static void draw_text(int x, int y, const char *shortcut,
                      const char *description, uint16_t color)
{
    while (*shortcut) {
        draw_char(x, y, *shortcut++, color);
        x += 8;
    }

    x += 24;  // 3-character gap

    while (*description) {
        draw_char(x, y, *description++, color);
        x += 8;
    }
}


static void erase_to_end(struct window *win)
{
    for (int r = win->ty; r < win->h; r++) {
        int start_c = (r == win->ty) ? win->tx : 0;
        for (int c = start_c; c < win->w; c++)
            clear_cell(win, r, c);
    }
}

static void scroll_up(struct window *win)
{
    int x0 = win->x;
    int x1 = win->x + win->w - 1;

    for (int r = 0; r < win->h; r++) {
        int y = win->y + r;

        for (int w = x0 >> 5; w <= x1 >> 5; w++) {
            uint32_t mask = ~0u;
            if (w == (x0 >> 5)) mask &= ~0u << (x0 & 31);
            if (w == (x1 >> 5)) mask &= ~0u >> (31 - (x1 & 31));

            uint32_t old_glyph = glyph_mask[workspace][y][w];
            uint32_t old_inverse = inverse_mask[workspace][y][w];
            uint32_t next_glyph = (r + 1 < win->h) ? glyph_mask[workspace][y + 1][w] : 0;
            uint32_t next_inverse = (r + 1 < win->h) ? inverse_mask[workspace][y + 1][w] : 0;

            if (workspace == tab) {
                uint32_t bits = (old_glyph | next_glyph | old_inverse | next_inverse) & mask;
                while (bits) {
                    int x = (w << 5) + __builtin_ctz(bits);
                    int c = x - win->x;
                    uint8_t ch = win->text[r][c];
                    uint8_t next_ch = (r + 1 < win->h) ? win->text[r + 1][c] : ' ';
                    uint8_t color = win->colors[r][c];
                    uint8_t next_color = (r + 1 < win->h) ? win->colors[r + 1][c] : 0;
                    uint32_t bit = 1u << (x & 31);

                    if (ch != next_ch || color != next_color ||
                        ((old_inverse ^ next_inverse) & bit))
                        mark_dirty_cell(x, y);
                    bits &= bits - 1;
                }
            }
            glyph_mask[workspace][y][w] = (old_glyph & ~mask) | (next_glyph & mask);
            inverse_mask[workspace][y][w] = (old_inverse & ~mask) | (next_inverse & mask);
        }
    }

    memmove(win->text[0], win->text[1], (win->h - 1) * sizeof *win->text);
    memmove(win->colors[0], win->colors[1], (win->h - 1) * sizeof *win->colors);
    memset(win->text[win->h - 1], ' ', win->w);
    memset(win->colors[win->h - 1], 0, win->w);
}

static void check_scroll(struct window *win)
{
    // If the cursor goes below the buttom row ,scroll up
    if (win->ty >= win->h) {
        scroll_up(win);
        win->ty = win->h - 1;
    }
}

static void terminal_byte(struct window *win, unsigned char c)       //reads keyboard/text chars and escape sequnces for terminal and handles them.
{




    switch (win->state) {
        case TEXT:
            handle_text(win, c);
            break;

        case ESC:
            handle_esc(win, c);
            break;

        case CSI:
            handle_csi(win, c);
            break;
    }
}

static void terminal_output(struct window *win, char *buf, int n)      //output text
{
    int i;

    for (i = 0; i < n; i++)
        terminal_byte(win, (unsigned char)buf[i]);


}

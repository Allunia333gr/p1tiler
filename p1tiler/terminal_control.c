static void erase_to_end(struct window *win);
static void clear_window(struct window *win);
static void check_scroll(struct window *win);
static void erase_to_start(struct window *win);
static void erase_line_to_end(struct window *win);
static void erase_start_line_to_cursor(struct window *win);
static void erase_entire_line(struct window *win);


// Clears text, color and the inverse bit together. Inverse spaces are visible.
static void clear_cell(struct window *win, int r, int c)
{
    int x = win->x + c;
    int y = win->y + r;
    uint32_t bit = 1u << (x & 31);
    bool inv = (inverse_mask[workspace][y][x >> 5] & bit) != 0;

    if (win->text[r][c] != ' ' || win->colors[r][c] || inv) {
        win->text[r][c] = ' ';
        win->colors[r][c] = 0;
        mark_glyph_cell(workspace, x, y, false);
        mark_inverted_cell(workspace, x, y, false);
        if (workspace == tab)
            mark_dirty_cell(x, y);
    }
}



static void handle_text(struct window *win, unsigned char c)
{
    if (c == 0x1b) {
        win->state = ESC;
        return;
    }

    if (c == '\t') {
        win->tx = (win->tx + 8) & ~7;

        if (win->tx >= win->w)
            win->tx = win->w - 1;

        return;
    }

    if (c == '\r') {
        win->tx = 0;
        return;
    }

    if (c == '\n') {
        win->tx = 0;
        win->ty++;
        check_scroll(win);
        return;
    }

    if (c == '\b' || c == 0x7f) {




        if (win->tx) {
            win->tx--;
            win->text[win->ty][win->tx] = ' ';
            win->colors[win->ty][win->tx] = 0;
            mark_glyph_cell(workspace,
                            win->x + win->tx,
                            win->y + win->ty, false);

            mark_inverted_cell( workspace, win->x + win->tx, win->y + win->ty, 0);
            if ( workspace == tab) mark_dirty_cell(win->x + win->tx,win->y + win->ty);
        }
        return;
    }


    // limit ASCI glyph range
    if (c >= 32 && c <= 126) {
        uint8_t color = win->current_color;
        if (win->bold && color)
            color += 8;

        int x = win->x + win->tx;
        int y = win->y + win->ty;

        if (win->text[win->ty][win->tx] != c ||
            win->colors[win->ty][win->tx] != color ||
            (((inverse_mask[workspace][y][x >> 5] &
            (1u << (x & 31))) != 0) != win->inverse)) {

            win->text[win->ty][win->tx] = c;
        win->colors[win->ty][win->tx] = color;

        mark_glyph_cell(workspace, x, y, c > 32);
        mark_inverted_cell(workspace, x, y, win->inverse);

        if (workspace == tab)
            mark_dirty_cell(x, y);
            }
            win->tx++;

        // Wrap to the next line if text exceeds window width
        if (win->tx >= win->w) {
            win->tx = 0;
            win->ty++;
            check_scroll(win);

        }

    }

}






static void handle_esc(struct window *win, unsigned char c)
{
    if (c == '[') {
        win->state = CSI;
        memset(win->params, 0, sizeof win->params);
        win->param_idx = 0;
        return;
    }

    if (c == '7')
        win->c_pos = (win->ty << 8) | win->tx;

    if (c == '8') {
        win->ty = win->c_pos >> 8;
        win->tx = win->c_pos & 0xFF;

        if (win->tx >= win->w)
            win->tx = win->w - 1;

        if (win->ty >= win->h)
            win->ty = win->h - 1;
    }

    win->state = TEXT;
    return;

}






static void handle_csi(struct window *win, unsigned char c)
{


    if (c >= '0' && c <= '9') {
        if (win->param_idx < 8) {
            win->params[win->param_idx] =
            win->params[win->param_idx] * 10 + (c - '0');
        }
        return;
    }

    if (c == ';') {
        if (win->param_idx < 8)
            win->param_idx++;
        return;
    }

    int p0 = win->params[0];
    int p1 = win->params[1];


    if (p0 == 0) {
        p0 = 1;
    }


    if (p1 == 0) {
        p1 = 1;
    }

    if (c < 0x40 || c > 0x7e)
        return;

    switch (c) {

        case 'H':
        case 'f':
            if (p0 > win->h){
                win->ty = win->h - 1;
            }else {
                win->ty = p0 -1;
            }
            if (p1>win->w){
                win->tx = win->w -1;
            } else{
                win->tx = p1 -1;
            }
            break;

        case 'G':
            if (p0 > win->w)
            {win->tx = win->w - 1;
            } else {
                win->tx = p0 -1;
            }
            break;

        case 'A':
            if (win->ty >= p0)
            { win->ty -= p0;
            } else {
                win->ty = 0;
            }
            break;

        case 'B':
            win->ty += p0;
            if (win->ty >= win->h) {
                win->ty = win->h - 1;
            }
            break;

        case 'C':
            win->tx += p0;
            if (win->tx >= win->w){
                win->tx = win->w - 1;
            }
            break;

        case 'D':
            if (win->tx >= p0) {
                win->tx -= p0;
            } else {
                win->tx = 0;
            }
            break;

        case 'E':
            win->ty += p0;
            if (win->ty >= win->h) {
                win->ty = win->h - 1;
            }
            win->tx = 0;
            break;

        case 'F':
            if (win->ty >= p0) {
                win->ty -= p0;
            } else {
                win->ty = 0;
            }
            win->tx = 0;
            break;

        case 'K':
            if (win->params[0] == 1) { erase_start_line_to_cursor(win);
            } else if (win->params[0] == 2) {erase_entire_line(win);
            } else { erase_line_to_end(win);
            }
            break;

        case 'J':
            if (win->params[0] == 1) {erase_to_start(win);
            } else if (win->params[0] == 2) {clear_window(win);
            } else { erase_to_end(win);
            }
            break;

        case 'd' :
            if (p0 > win->h) {
                win->ty = win->h - 1;
            } else {
                win->ty = p0 - 1;
            }
            break;


        case 'h':
            if (p0 == 9002){
                fullscreen_master = win->master;
            break;

            }
            if (p0 == 25) {
                win->cursor_visible = 1;
            }
            break;

        case 'l':
            if (p0 == 9002 && fullscreen_master == win->master) {
                fullscreen_master = -1;
                mark_dirty_rect(0, 16, var_screen.xres,
                                var_screen.yres - 16);
            }

            if (p0 == 25) {
                win->cursor_visible = 0;
            }
            break;

        case 'm':
            for (int i = 0; i <= win->param_idx && i <8; i++) {
                int value = win->params[i];


                if (value == 0) {
                    win->bold  = 0;
                    win->inverse = 0;
                    win->current_color=0;
                } else if (value == 1) {
                    win->bold  = 1;
                } else if (value == 22) {
                    win->bold  = 0;
                } else if (value == 27) {
                    win->inverse = 0;
                } else if (value == 7) {
                    win->inverse = 1;
                } else if (value >= 30 && value <= 37) {
                    win->current_color = value - 29;      //reduce to the 8 ansi colors
                } else if (value == 39) {
                    win->current_color=0;
                }
            }
            break;
    }
    memset(win->params, 0, sizeof win->params);
    win->param_idx = 0;
    win->state = TEXT;
}


static void clear_window(struct window *win)
{
    for (int r = 0; r < win->h; r++)
        for (int c = 0; c < win->w; c++)
            clear_cell(win, r, c);
}

static void erase_to_start(struct window *win)
{
    for (int r = 0; r <= win->ty; r++) {
        int end_c = (r == win->ty) ? win->tx + 1 : win->w;
        for (int c = 0; c < end_c; c++)
            clear_cell(win, r, c);
    }
}

static void erase_line_to_end(struct window *win)
{
    for (int c = win->tx; c < win->w; c++)
        clear_cell(win, win->ty, c);
}

static void erase_start_line_to_cursor(struct window *win)
{
    for (int c = 0; c <= win->tx; c++)
        clear_cell(win, win->ty, c);
}

static void erase_entire_line(struct window *win)
{
    for (int c = 0; c < win->w; c++)
        clear_cell(win, win->ty, c);
}

static void draw_cursor(int x, int y, uint16_t color)
{
    for (int r = 14; r < 16; r++) {
        uint16_t *dst = (uint16_t *)(fb +
            ((y << 4) + r) * fixed_screen.line_length + (x << 4));
        for (int i = 0; i < 8; i++)
            dst[i] = color;
    }
}

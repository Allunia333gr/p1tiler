static void cpu_usage(void);
static void ram_usage(void);
static void mark_dirty_cell(int x, int y);
static void draw_cursor(int x, int y, uint16_t color);
static inline uint16_t rgb555_to_565(uint16_t c);

//helper function for background_load to handle errors to a black background
static void load_solid_black_background(void)
{
    background = calloc(1, sizeof(struct wallpaper));
    if (!background) return;

    for (int y = 0; y < MAX_ROWS; y++) {
        for (int x = 0; x < MAX_COLS; x++) {
            background->cell_index[y][x] = 0x8000; //solid cells (black = 0x0000)
        }
    }

    if (TOP_BAR) {
        for (uint16_t x = 0; x < MAX_COLS; x++) {
            background->cell_index[0][x] = 0x8000 | 0x1468; //top bar color
        }
    }
}





#if windows_shadow == 1
static void mark_shadow_cell(uint8_t  workspace, int x, int y, bool occupied)
{
    uint32_t *word = &shadow_mask[workspace][y][x >> 5];
    uint32_t bit = 1u << (x & 31);

    if (occupied)
        *word |= bit;
    else
        *word &= ~bit;
}
#endif


static int background_load(const char *filename)        //load background on initialization
{
    //no filename provided, black fallback immediately
    if (!filename) {
        load_solid_black_background();
        return 0;
    }

    int fd;

    fd = open(filename, O_RDONLY);
    if (fd < 0) {
        //no file or it cant  be read so black bg
        load_solid_black_background();
        return 0;
    }


    off_t size = lseek(fd, 0, SEEK_END); // finds the size of file
    lseek(fd, 0, SEEK_SET);
    if (size == (off_t)-1 || lseek(fd, 0, SEEK_SET) == (off_t)-1) {   //fallback to balck bg
        close(fd);
        load_solid_black_background();
        return 0;
    }

    if (size < (off_t)sizeof(struct wallpaper)) {
        close(fd);
        load_solid_black_background();
        return 0;
    }

    background = malloc((size_t)size);
    if (!background) {
        close(fd);
        load_solid_black_background();
        return 0;
    }





    size_t done = 0;

    // Read the wallpaper binary file into memory in chunks
    while (done < (size_t)size) {
        ssize_t n = read(fd, (uint8_t *)background + done,
                         (size_t)size - done);

        if (n <= 0)
            break;

         done += n;
    }
    if (done != (size_t)size) {
        free(background);
        background = NULL;
        close(fd);
        load_solid_black_background();
        return 0;
    }

    posix_fadvise(fd, 0, size, POSIX_FADV_DONTNEED);
    close(fd);

    if (TOP_BAR) {                                             //if the top bar exists set it to a solid color
        for (uint16_t x = 0; x < MAX_COLS; x++) {
            background->cell_index[0][x] =
            0x8000 | 0x1468;    //(0x208C + ((x >> 5) << 10)  );
        }
    }

    return 0;
}

static void background_draw(int x, int y)    //background image decompressor
{
    int cx = x << 3;        // Convert cell x to pixel coordinate (x * 8)
    int cy = y << 4;        // Convert cell y to pixel coordinate (y * 16)
    int py;
    int b;
    uint16_t entry = background->cell_index[y][x];

    //check if the cell is a solid color block else decompress
    if (entry & 0x8000) {          //bit 15 true for marking a cell as being all pixels the same color so we can avoid decompression
         uint16_t color = entry & 0x7FFF;

        #if windows_shadow == 1
         if (shadows_enabled ){
         uint32_t word = shadow_mask[tab][y][x >> 5];
         uint32_t bit = 1u << (x & 31);
           if (word & bit)
          color = (color >> 1) & 0x3DEF;
         }
        #endif

        if (rgb565)
            color = rgb555_to_565(color);

        uint32_t pair = (uint32_t)color | ((uint32_t)color << 16);

        // Fill all 16 pixel rows with the solid color
        for (py = 0; py < 16; py++) {
            uint32_t *dst = (uint32_t *)
            (fb + (cy + py) * fixed_screen.line_length + cx * 2);

            dst[0] = pair;
            dst[1] = pair;
            dst[2] = pair;
            dst[3] = pair;
        }
        return;
    }
    else {
        //decompress the 4bpp cell pixels with the color table
        struct bg_cell *cell =
        (struct bg_cell *)((uint8_t *)background->colored_cells + (entry << 3));

        const uint16_t *colors = cell->color;

        #if windows_shadow == 1

            uint16_t dark_colors[4];
            uint32_t word = shadow_mask[tab][y][x >> 5];
            uint32_t bit = 1u << (x & 31);

            if (shadows_enabled && (word & bit)) {
                for (int i = 0; i < 4; i++)
                    dark_colors[i] = (cell->color[i] >> 1) & 0x3DEF;

                colors = dark_colors;
            }
        #endif

        uint16_t converted_colors[4];

        if (rgb565) {
            for (int i = 0; i < 4; i++)
                converted_colors[i] = rgb555_to_565(colors[i]);

            colors = converted_colors;
        }

        for (py = 0; py < 16; py++) {
            uint16_t *dst = (uint16_t *)
            (fb + ((cy + py) * fixed_screen.line_length) + (cx * 2));



            /*
             * 2bit per pixel
             * 8 pixels per row.
             * 16 rows per cell
             */
            for (b = 0; b < 2; b++) {
                uint8_t packed = cell->pixels[(py << 1) + b];

                dst[0] = colors[(packed >> 6) & 3];
                dst[1] = colors[(packed >> 4) & 3];
                dst[2] = colors[(packed >> 2) & 3];
                dst[3] = colors[ packed       & 3];
                dst += 4;

            }
        }
    }
}

static void draw_char(int x, int y, unsigned char ch, uint16_t color)   //draws a glyph on the terminals text output
{
    if (rgb565)
        color = rgb555_to_565(color);

    int row;
    int col;

    // Walk through each row of the 8x16 font glyph
    for (row = 0; row < 16; row++) {

        uint8_t bits = font[ch - 32][row];;

        uint16_t *dst = (uint16_t *)(fb + (y + row) * fixed_screen.line_length + (x * 2));



        for (col = 0; col < 8; col++) {
            if (bits & (0x80 >> col))
                dst[col] = color ; //0x1F28 ;   // 0x33CC    ;  //0b1 11011 00111 11000 ;   /* white bg 0x7fff  0b1 10001 00101 01000 */ 0x7E83
                // else
                //     dst[col] = 0x0000;   /* black bg */
        }
    }


}


static void redraw_cell(int x, int y)
{

    // Draw the background wallpaper at this cell coordinate
    background_draw(x, y);



    //Go through active windows to find any window overlapping this cell
    for (int j = 0; j < current_windows[tab]; j++) {

        struct window *win = &term_win[tab][j];

        int wx = x - win->x;
        int wy = y - win->y;

        // Check if the cell falls within the window boundaries
        if ((unsigned)wx >= (unsigned)win->w ||
            (unsigned)wy >= (unsigned)win->h)
            continue;

        unsigned char ch = win->text[wy][wx];

        uint16_t color = (j == focused[tab]) ? focused_win_text : unfocused_win_text;

        uint8_t index = win->colors[wy][wx];
        if (index)
            color = ansi_palette[index];

        bool inverted = (inverse_mask[tab][y][x >> 5] & (1u << (x & 31))) != 0;

        int cx = x << 3;
        int cy = y << 4;

        if (inverted) {
             uint16_t fill_color = color;
            if (rgb565)
                fill_color = rgb555_to_565(fill_color);


            uint32_t pair = (uint32_t)fill_color | ((uint32_t)fill_color << 16);
            for (int py = 0; py < 16; py++) {
                uint32_t *dst = (uint32_t *)(fb + (cy + py) * fixed_screen.line_length + cx * 2);
                dst[0] = pair; dst[1] = pair; dst[2] = pair; dst[3] = pair;
            }


            if (ch >= 32 && ch <= 126 && ch != ' ') {
                draw_char(cx, cy, ch, 0x0000);
            }
        } else {

            if (ch >= 32 && ch <= 126 && ch != ' ') {
                draw_char(cx, cy, ch, color);
            }
        }

        if (j == focused[tab] && win->cursor_visible &&
            wx == win->tx && wy == win->ty) {

            uint16_t cursor_color = color;

            if (rgb565)
                cursor_color = rgb555_to_565(cursor_color);

            draw_cursor(x, y, cursor_color);
        }
        break;
    }
}


static void mark_dirty_cell(int x, int y)        // sets dirty bit on for a cell
{
    int word = x >> 5;                        // x divide by 32
    dirty[y][word] |= 1u << (x & 31);        // masking bit as 1 on 32bit integer

    dirty_words[y] |= 1u << word;            // Mark word row as dirty
}

static void mark_dirty_rect(int x, int y, int w, int h)     //rectangle function for setting dirty area
{
    int x0 = x >> 3;              /* pixels -> 8px cells */
    int y0 = y >> 4;              /* pixels -> 16px cells */

    int x1 = (x + w - 1) >> 3;
    int y1 = (y + h - 1) >> 4;

    int cy, cx;

    // Mark all cells inside the rectangle region as dirty
    for (cy = y0; cy <= y1; cy++)
        for (cx = x0; cx <= x1; cx++)
            mark_dirty_cell(cx, cy);
}

static void redraw_dirty(void)      // finds cells to be redrawn
{


    for (int y = TOP_BAR ? 1 : 0; y < screen_rows; y++) {

        uint32_t words = dirty_words[y];

        while (words) {

            int word = __builtin_ctz(words);   //CTZ scans for the first bit set to 1, so it is our first nonempty dirty word in the index

            uint32_t bits = dirty[y][word];

            while (bits) {

                int bit = __builtin_ctz(bits);
                int x = (word << 5) + bit;

                redraw_cell(x, y);

                bits &= bits - 1;     // Clear lowest set bit
            }

            dirty[y][word] = 0;

            words &= words - 1;       // Clear lowest set word
        }

        dirty_words[y] = 0;

    }




}






static void pixel(int x, int y, uint16_t color)  //probably i dont need this one
{
    uint16_t *p = (uint16_t *)(fb +
    y * fixed_screen.line_length +
    x * 2);
    *p = color;
}


static void borders(int x, int y, int w, int h, uint16_t color)
{
    if (rgb565)
        color = rgb555_to_565(color);

    int stride = fixed_screen.line_length >> 1;

    uint16_t *top = (uint16_t *)(fb + y * fixed_screen.line_length + x * 2);
    uint16_t *bottom = top + (h - 1) * stride;

    // Horizontal lines
    for (int i = 0; i < w; i++) {
        top[i] = color;
        bottom[i] = color;
    }

    // Vertical lines
    for (int i = 1; i < h - 1; i++) {
        top[i * stride ] = color;
        top[i * stride + w -1 ] = color;
    }
}

static void use_top_bar(const char *text, uint8_t x,  uint16_t color)  //chars to apear   , position , color of text
{
    for (int i = 0; text[i] && x + i < screen_cols; i++) {
        int pos = x + i;
        unsigned char ch = text[i];

        //Restore background only when character changes
        if (bar_text[pos] != ch) {
            background_draw(pos, 0);
            bar_text[pos] = ch;
         }

        //Redraw foreground pixels
        if (ch > 32 && ch <= 126)
            draw_char(pos << 3, 0, ch, color);

    }

}


static void recolor_window_text(int i, uint16_t color)
{
    struct window *win = &term_win[tab][i];

    for (int y = 0; y < win->h; y++) {
        for (int x = 0; x < win->w; x++) {

            unsigned char ch = win->text[y][x];

            if (ch <= 32 || ch > 126)
                continue;

            int sx = win->x + x;
            int sy = win->y + y;

            bool inverted = (inverse_mask[tab][sy][sx >> 5] & (1u << (sx & 31))) != 0;

            if (inverted)
                continue;

            uint16_t cell_color = color;

            if (win->colors[y][x])cell_color = ansi_palette[win->colors[y][x]];

            draw_char(sx << 3, sy << 4, ch, cell_color);
        }
    }
}



static void mark_inverted_cell(uint8_t  workspace, int x, int y, bool invert)     //marks true/false the inverted  mask table
{
uint32_t bit = 1u << (x & 31);
uint32_t *word = &inverse_mask[workspace][y][x >> 5];

if (invert)
    *word |= bit;
else
    *word &= ~bit;
}


static void mark_glyph_cell(uint8_t  workspace, int x, int y, bool occupied)          //marks true/false the glyph mask table
{
    uint32_t *word = &glyph_mask[workspace][y][x >> 5];
    uint32_t bit = 1u << (x & 31);

    if (occupied)
        *word |= bit;
    else
        *word &= ~bit;
}


static void bar_timer(){                           //time and date on the bar display

    struct tm local;
    char date[40];
    static time_t last_time = 0;

    time_t now = time(NULL);

    if (now != last_time) {
        last_time = now;


    localtime_r(&now, &local);

    strftime(date, sizeof date,
             "%a %d %b %H:%M:%S", &local);
    use_top_bar(date, screen_cols - strlen(date), 0x2B9C);

    ram_usage();
    cpu_usage();
    if (help_visible) {
        static unsigned tick = 0;

        if (help_visible) {
            if (++tick >= 3) {
                help_visible = false;
                tick = 0;
             mark_dirty_rect(HELP_X << 3, HELP_Y << 4, HELP_W << 3, HELP_H << 4);
            }
           } else {
            tick = 0;
        }
      }
    }

}


static void ram_usage(){       //shows ram usage on the bar

    uint16_t color;
    char buf[64];
    FILE *fp;

    fp = fopen("/proc/meminfo", "r");

    if (fp) {
        unsigned long  free = 0;
        char line[80];

        while (fgets(line, sizeof line, fp)) {

            sscanf(line, "MemFree: %lu kB", &free);

            if (free)

                break;
        }

        fclose(fp);

       //     if (free <=1000) color = 0xF400;
          //  else if (free <=2350) color = 0xFE20;
           // else  if (free <=6000) color = 0x8B00;



            snprintf(buf, sizeof buf, "%lu/%lu",
                     (total_ram - free) >> ram_shift,
                     total_ram >> ram_shift
                      );

          use_top_bar(buf, bar_ram_x + 4 , 0x8B00);
          use_top_bar(ram_unit,bar_ram_x + 4 + strlen(buf) ,  0xFEE0);
        }

}

static void cpu_usage(void)              //shows cpu usage on the bar
{
    static unsigned cpu=0;
    static unsigned long last_total = 0;
    static unsigned long last_idle = 0;

    unsigned long user, nice, system, idle;
    char data[128], buf[6];

    int fd = open("/proc/stat", O_RDONLY);
    if (fd < 0) return;

    int n = read(fd, data, sizeof data - 1);
    close(fd);

    if (n <= 0) return;
    data[n] = 0;

    sscanf(data, "cpu %lu %lu %lu %lu",
           &user, &nice, &system, &idle);

    unsigned long total = user + nice + system + idle;

    if (last_total && total != last_total) {
        unsigned long dt = total - last_total;
        unsigned long di = idle - last_idle;


          cpu = 100 * (dt - di) / dt;

        snprintf(buf, sizeof buf, "%3u", cpu);
        use_top_bar(buf, bar_cpu_x + 4, 0x2B9C);
    }

    last_total = total;
    last_idle = idle;
}






static void background_change()
{
  //  0x3575   nice shadowy purple
  //  0x75F2   nice shadowy  redish
    char path[128];

    bg_id++;

    if (bg_id >= background_count)
        bg_id = 0;


    snprintf(path, sizeof path,
             "/p1tiler/backgrounds/%ux%u/%s",
             var_screen.xres, var_screen.yres,
             backgrounds[bg_id]);

    free(background);
    background_load(path);

    focused_win_text = focused_theme_colors[bg_id];
    unfocused_win_text = unfocused_theme_colors[bg_id];

   /* if (rgb565) {
        focused_win_text = rgb555_to_565(focused_win_text);
        unfocused_win_text = rgb555_to_565(unfocused_win_text);
    }*/


    mark_dirty_rect(0, 0, screen_cols << 3, screen_rows << 4);

}

static void update_cursor(void)             //updates cursor written by a wizard
{
    int x = -1, y = -1;
    struct window *win = NULL;

    if (current_windows[tab]) {
        win =   &term_win[tab][focused[tab]];

        x = win->x + win->tx;
        y = win->y + win->ty;
    }

    if (win && win->cursor_visible){
    if (x != cursor_x || y != cursor_y ||
        cursor_tab != tab || cursor_focus != focused[tab]) {

        if (cursor_x >= 0)
            mark_dirty_cell(cursor_x, cursor_y);

        if (x >= 0)
            mark_dirty_cell(x, y);
        }

    }

    cursor_x = x;
    cursor_y = y;
    cursor_tab = tab;
    cursor_focus = focused[tab];
}

static unsigned long physical_ram(void)
{
    unsigned long ram = 0;
    unsigned long start, end, top = 0;
    char line[128];

    FILE *fp = fopen("/proc/iomem", "r");

    if (fp) {
        while (fgets(line, sizeof line, fp)) {
            if (strstr(line, "System RAM") &&
                sscanf(line, "%lx-%lx", &start, &end) == 2) {
                if (end > top)
                    top = end;
                }
        }

        fclose(fp);
        ram = (top + 1) >> 10;
    }

    // Fallback for unprivileged users
    if (!ram) {
        fp = fopen("/proc/meminfo", "r");

        if (fp) {
            while (fgets(line, sizeof line, fp)) {
                if (sscanf(line, "MemTotal: %lu kB", &ram) == 1)
                    break;
            }

            fclose(fp);
        }
    }

    if (ram >= 16384) {
        ram_shift = 10;
        ram_unit = "MB";
    } else {
        ram_shift = 0;
        ram_unit = "KB";
    }

    return ram;
}

static inline uint16_t rgb555_to_565(uint16_t c)
{
    return ((c & 0x7FE0) << 1) |
    ((c & 0x0200) >> 4) |
    (c & 0x001F);
}



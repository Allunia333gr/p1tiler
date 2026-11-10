static void scroll_up(struct window *win);
static void change_workspace(uint8_t new_tab);
static void help_menu();
static void draw_text(int x, int y, const char *shortcut, const char *description, uint16_t color);


static void layout_function(uint8_t w, uint8_t index, int x, int y, int width, int height)
{      // walks the tree and calcualtes term_win[i].x, .y, .w, .h.

    static uint32_t old_inverse[MAX_ROWS][DIRTY_WORDS];


    if (index == root[w]){
        memset(glyph_mask[w], 0, sizeof glyph_mask[w]);
        memcpy(old_inverse, inverse_mask[w], sizeof old_inverse);
        memset(inverse_mask[w], 0, sizeof inverse_mask[w]);
        #if windows_shadow == 1
        memset(shadow_mask[w], 0, sizeof shadow_mask[w]);
        #endif

    }


    if (!index) return;         //recursive function to make the gap and widnow calculation

    struct tree_node *node  = &tree[w][index];

    if (!node ->active) return;

    if (index == redraw_node) {
        mark_dirty_rect(x << 3, y << 4, width << 3,height << 4);

        redraw_node = 0;
    }

    if (!node->left &&  !node->right) {


        uint8_t  dgap = gap + gap;

        struct window *win = &term_win[w][node->window_index];
        bool resized = (win->w != width - dgap || win->h != height - dgap);




        int old_x = win->x;
        int old_y = win->y;
        int old_w = win->w;
        int old_h = win->h;
        int shift = 0;


        win->x = x + gap;
        win->y = y + gap;
        win->w = width - dgap;
        win->h = height - dgap;




        win->x_px = win->x << 3;
        win->y_px = win->y << 4;

        win->ws.ws_col = win->w;
        win->ws.ws_row = win->h;
        win->ws.ws_xpixel = win->w << 3;
        win->ws.ws_ypixel = win->h << 4;



        if (resized) {
            //scroll up and update cursor of the win that gets split


            if (win->ty >= win->h) {
                  shift = win->ty - win->h + 1;

                memmove(win->text, win->text + shift,
                        (old_h - shift) * sizeof *win->text);

                memmove(win->colors, win->colors + shift,
                        (old_h - shift) * sizeof *win->colors);

                memset(win->text + old_h - shift, ' ',
                       shift * sizeof *win->text);

                memset(win->colors + old_h - shift, 0,
                       shift * sizeof *win->colors);

                win->ty -= shift;

            }

            if (win->tx >= win->w)
                win->tx = win->w -1;



        }


        for (int r = 0; r < win->h; r++) {
            for (int c = 0; c < win->w; c++) {
                unsigned char ch = win->text[r][c];
                mark_glyph_cell(w, win->x + c, win->y + r, ch >= 32 && ch != ' ');

                bool inverted = false;

                if (c < old_w && r +shift < old_h) {
                    int sx = old_x + c;
                    int sy = old_y + r+ shift;

                    inverted = (old_inverse[sy][sx >> 5] &
                    (1u << (sx & 31))) != 0;
                }

                mark_inverted_cell(w, win->x + c, win->y + r, inverted);



                #if windows_shadow == 1
                mark_shadow_cell(w, win->x + c, win->y + r, 1);
                #endif
            }

        }

        int w_px = win->w << 3;
        int h_px = win->h << 4;



         ioctl(win->master, TIOCSWINSZ, &win->ws);




        return;

    }


    if (node->vertical_split) {
        int half_w = width >> 1;
        layout_function(w,node->left,  x,          y, half_w,     height);
        layout_function(w,node->right, x + half_w, y, width - half_w, height);

    } else {
        int half_h = height >> 1;
        layout_function(w,node->left,  x, y,          width, half_h);
        layout_function(w,node->right, x, y + half_h, width, height - half_h);
    }

}






void init_window(struct window *win, int x, int y, int w, int h, uint8_t (*text_buf)[MAX_COLS],uint8_t (*color_buf)[MAX_COLS])
{

    win->x = x;
    win->y = y;
    win->w = w;
    win->h = h;
    win->w = 0;
    win->h = 0;
    win->tx = 0;
    win->ty = 0;
    win->state=0;
    win->c_pos=0;

    win->x_px = x << 3;
    win->y_px = y << 4;
    win->inverse = false;
    win->text = text_buf ;
    win->colors = color_buf;
    win->ws.ws_col = w;
    win->ws.ws_row = h;
    win->ws.ws_xpixel = w << 3;
    win->ws.ws_ypixel = h << 4;
    win->cursor_visible = 1;

}

static void terminal_closer( struct pollfd fds[MAX_WORKSPACES][MAX_WINDOWS + 1],int w, int i) {        //closes a terminal/window






    if (fds[w][i + 1].fd == fullscreen_master) {
        fullscreen_master = -1;
        mark_dirty_rect(0, 16,
                        screen_cols << 3,
                        (screen_rows << 4) - 16);
    }




    // If closing the last window, reset root and clear state
    if (current_windows[w] <= 1){
        close(fds[w][i + 1].fd);
        fds[w][i + 1].fd = -1;
        current_windows[w] = 0;
        root[w] = 0;
        layout_function(w,root[w], 0, 0, screen_cols, screen_rows);
        mark_dirty_rect(0, 0, var_screen.xres, var_screen.yres);

        if (term_win[w][i].text != text[w]){
            free(term_win[w][i].text);
           free(term_win[w][i].colors);
        }
        memset(tree[w], 0, sizeof tree[w]);
        focused[w] = 0;
        return;
    }
    int parent =0;
    int leaf = 0;
          //leaf search and find the closed
        for (int n = 1; n <= MAX_NODES; n++) {
            if (tree[w][n].active && !tree[w][n].left && !tree[w][n].right && tree[w][n].window_index == i) {
                leaf = n;
                break;

            }
        }
         //parent node search
        for (int n = 1; n <= MAX_NODES; n++) {
            if (tree[w][n].active && (tree[w][n].left == leaf || tree[w][n].right == leaf)) {
                parent = n;
                break;
            }
        }

        redraw_node = parent;



        int sibling;

        if (tree[w][parent].left == leaf) {
            sibling = tree[w][parent].right;
        } else {
            sibling = tree[w][parent].left;
        }




        // Collapse tree by replacing parent node with its sibling
        tree[w][parent] = tree[w][sibling];
        tree[w][leaf].active= false;
        tree[w][sibling].active = false;

         close(fds[w][i + 1].fd);
         if (term_win[w][i].text != text[w]){
             free(term_win[w][i].text);
            free(term_win[w][i].colors);
            }

         for (int j = i; j < current_windows[w] - 1; j++) {
             term_win[w][j] = term_win[w][j + 1];
         }

         for (int j = i + 1; j < current_windows[w]; j++) {
             fds[w][j] = fds[w][j + 1];
         }

          for (int n = 1; n <= MAX_NODES; n++) {
              if (tree[w][n].active &&
                  !tree[w][n].left && !tree[w][n].right &&
                  tree[w][n].window_index > i)
                  tree[w][n].window_index--;
          }
          term_win[w][current_windows[w] - 1] = (struct window){0};
          fds[w][current_windows[w]].fd = -1;
          current_windows[w]--;
          if (focused[w] > i)
              focused[w]--;
              else if (focused[w] == i && focused[w] >= current_windows[w])
                 focused[w]--;


          layout_function(w,root[w], 0, 0, screen_cols, screen_rows);


}


static int split_focused(uint8_t new_win, bool vertical)
{
    uint8_t leaf = 0, a = 0, b = 0;

    // Find the active leaf for the focused window and two available node slots
    for (uint8_t i = 1; i <= MAX_NODES; i++) {

        if (tree[tab][i].active &&
            !tree[tab][i].left && !tree[tab][i].right &&
            tree[tab][i].window_index == focused[tab])
            leaf = i;

        else if (!tree[tab][i].active) {
            if (!a) a = i;
            else if (!b) b = i;
        }
    }

    redraw_node = leaf;


    if (!leaf || !a || !b) return -1;


    // Convert leaf into a split node with two new child leaves
    tree[tab][a] = (struct tree_node){ .active = true,
        .window_index = focused[tab] };

        tree[tab][b] = (struct tree_node){ .active = true,
            .window_index = new_win };

            tree[tab][leaf].left = a;
            tree[tab][leaf].right = b;





            tree[tab][leaf].vertical_split =
            term_win[tab][focused[tab]].w >= (term_win[tab][focused[tab]].h << 1);

            return 0;
}




static int terminal_spawner(struct pollfd fds[MAX_WORKSPACES][MAX_WINDOWS + 1])   //creates terminal/windows
{
    int idx = current_windows[tab];
    if (idx >= MAX_WINDOWS) return -1;
    uint8_t (*text_buf )[MAX_COLS] = calloc(MAX_ROWS, sizeof *text_buf );
    uint8_t (*colors)[MAX_COLS] = calloc(MAX_ROWS, sizeof *colors );
    if (!colors) return -1;
    if (!text_buf) return -1;


    init_window(&term_win[tab][idx],  0, 0, screen_cols, screen_rows, text_buf,colors);

    term_win[tab][idx].child = forkpty(&term_win[tab][idx].master, NULL, NULL, &term_win[tab][idx].ws);


    if (term_win[tab][idx].child < 0) {
        perror("forkpty");
        return -1;
    }

    if (term_win[tab][idx].child == 0) {
        setenv("TERM", "linux", 1);
        execl("/bin/sh", "sh", NULL);
        _exit(127);
    }

    fds[tab][idx + 1].fd = term_win[tab][idx].master;
    fds[tab][idx + 1].events = POLLIN;
    fds[tab][idx + 1].revents = 0;

    if (current_windows[tab] == 0) {            //if root was empty cause last shell closed init root
        root[tab] = 1;
        tree[tab][root[tab]].active = true;
        tree[tab][root[tab]].window_index = idx;
    } else {
        split_focused(idx, true);          //Split focused window to fit the new one
    }

    current_windows[tab]++;

    layout_function(tab,root[tab], 0, 0, screen_cols, screen_rows);



    #if windows_shadow == 1
    if (current_windows[tab] == 1) {
        struct window *win = &term_win[tab][0];

        mark_dirty_rect(win->x_px, win->y_px,
                        win->w << 3, win->h << 4);
    }
    #endif

    return idx;
}



static void keyboard_input_handler(struct pollfd fds[MAX_WORKSPACES][MAX_WINDOWS + 1], char *buf, int n)        //handles the input of alt + 1-4 and alt q,w,e,h
{




    uint8_t new_tab = tab;

    // Check for Alt+number shortcuts to switch focus or spawn new windows
    if (n >= 2 && buf[0] == '\x1b' && buf[1] >= '1' && buf[1] <= '4') {
        uint8_t target = buf[1] - '1';

        if (target < current_windows[tab] && target != focused[tab]) {
            recolor_window_text(focused[tab], unfocused_win_text );
            focused[tab] = target;
            recolor_window_text(focused[tab], focused_win_text );
        }
        else if (target == current_windows[tab] && current_windows[tab] < MAX_WINDOWS) {
            terminal_spawner(fds );
            focused[tab] = target;
        }
    }
    else if (n >= 2 && buf[0] == '\x1b' &&
        (buf[1] == 'q' || buf[1] == 'w' || buf[1] == 'e'|| buf[1] == 'h' )) {
        if (buf[1] == 'q')  new_tab  = 0;
        if (buf[1] == 'w')  new_tab  = 1;
        if (buf[1] == 'e')  new_tab  = 2;
        if (buf[1] == 'h') help_menu();

        if (new_tab != tab)
            change_workspace(new_tab );
         }


#if CONFIGURATOR ==1
        else if (n >= 2 && buf[0] == '\x1b' && buf[1] =='\'') {
            configurator();

            for (int w = 0; w < MAX_WORKSPACES; w++)
                layout_function(w, root[w], 0, 0, screen_cols, screen_rows);

            mark_dirty_rect(0, 0, screen_cols << 3, screen_rows << 4);
        }
#endif







        else if (n >= 2 && buf[0] == '\x1b' &&
            (buf[1] == '-')) {
            background_change();
            }

        else if (n > 0) {
        //Forward standard keyboard input to the active window
        write(term_win[tab][focused[tab]].master, buf, n);






    }

}

static void change_workspace(uint8_t new_tab){      //the function that changes current workspace

    if (  new_tab >= MAX_WORKSPACES || new_tab == tab)
        return;

    uint8_t old_tab = tab;
    tab = new_tab;
    workspace = new_tab;



       //the following code sets up in red the current workspace in the bars
    if (new_tab == 0){
        use_top_bar("[1]  [2]  [3]",bar_workspace_x,0x2B9C);
        use_top_bar("[1]", bar_workspace_x, 0x5C88);
    }
    else if (new_tab == 1){
        use_top_bar("[1]  [2]  [3]",bar_workspace_x,0x2B9C);
        use_top_bar("[2]", bar_workspace_x + 5, 0x5C88);
        }
        else{
        use_top_bar("[1]  [2]  [3]",bar_workspace_x,0x2B9C);
        use_top_bar("[3]",bar_workspace_x+ 10,0x5C88);
            }



            for (int y = TOP_BAR; y < screen_rows; y++) {

                for (int w = 0; w < (screen_cols + 31) >> 5; w++) {


                    uint32_t old_mask =

                    glyph_mask[old_tab][y][w] |

                    inverse_mask[old_tab][y][w];


                    uint32_t new_mask =

                    glyph_mask[new_tab][y][w] |

                    inverse_mask[new_tab][y][w];


                    uint32_t erase = old_mask & ~new_mask;


                    #if windows_shadow == 1

                    uint32_t shadow_diff =

                    shadow_mask[old_tab][y][w] ^

                    shadow_mask[new_tab][y][w];


                    erase |= shadow_diff & ~new_mask;

                    #endif


                    uint32_t bits = erase;


                    while (bits) {

                        int bit = __builtin_ctz(bits);

                        int x = (w << 5) + bit;


                        background_draw(x, y);


                        bits &= bits - 1;

                    }


                    bits = new_mask;


                    while (bits) {

                        int bit = __builtin_ctz(bits);

                        int x = (w << 5) + bit;


                        redraw_cell(x, y);


                        bits &= bits - 1;

                    }

                }

            }




}


static void help_menu()
{


    int text_x = (HELP_X + 5) << 3;
    int text_y = (HELP_Y << 4) + 2;
    //coordinates :
    int x0 = HELP_X;
    int y0 = HELP_Y;

    int x1 = x0 + HELP_W - 1;
    int y1 = y0 + HELP_H - 1;

    uint16_t color = 0x1468;

    if (rgb565)
        color = rgb555_to_565(color);

    uint32_t pair = (uint32_t)color | ((uint32_t)color << 16);

    for (int y = HELP_Y << 4; y < (HELP_Y + HELP_H) << 4; y++) {

        uint32_t *dst = (uint32_t *)
        (fb + y * fixed_screen.line_length + (HELP_X << 4));

        for (int x = 0; x < (HELP_W << 2); x++)
            dst[x] = pair;
    }

    char term_shortcut[] = "Alt + 1-4";
    char workspace_shortcut[] = "Alt + Q/W/E";
    char change_wallpaper[] = "Alt and -";
    char help_menu_key[] =  "Alt and H";
  //  char close_help[]= "ESC";
    char dummy[]= " ";
    char shadow_disable[] = "Alt + '";


    draw_text(text_x, text_y , dummy,"P1Tiler - HELP", 0x7FFF);
    draw_text(text_x, text_y +20 , term_shortcut, " Focus/spawn terminal", 0x7FFF);
    draw_text(text_x, text_y +40 , workspace_shortcut, " Switch workspace", 0x7FFF);
    draw_text(text_x, text_y +60, change_wallpaper," Change wallpaper", 0x7FFF);
    draw_text(text_x, text_y +80, help_menu_key," Help menu", 0x7FFF);
    draw_text(text_x, text_y +100 , shadow_disable ,"Reload cfg file", 0x7FFF);

    help_visible = true;


}

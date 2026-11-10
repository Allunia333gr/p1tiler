/*
 * P1Tiler - framebuffer tiling terminal manager
 *
 * Target:
 *   Intel Pentium 120 MHz, non-MMX
 *   8 MiB system RAM
 *   1280x1024, 16-bit framebuffer
 *   8x16 pixel character cells
 *
 * Coordinate system:
 *   Screen: 160x64 cells
 *   Window x/y/w/h: screen-cell coordinates
 *   Window tx/ty: local terminal coordinates
 *   Framebuffer: pixel coordinates
 *
 * Rendering:
 *   1. Terminal output modifies window text.
 *   2. Modified screen cells are marked dirty.
 *   3. redraw_dirty() visits marked cells.
 *   4. background_draw() restores wallpaper.
 *   5. draw_char() overlays terminal glyphs.
 *
 * Layout:
 *   Binary split tree, up to four terminal leaves.
 *   Each leaf references one terminal window.
 *   Windows do not overlap.
 *
 * Display:
 *   Single framebuffer, no backbuffer.
 *   All drawing happens directly in video memory.
 */
/*
 * WINDOW / TREE INVARIANTS
 *
 * current_windows:
 *   Number of live terminal windows.
 *
 * term_win[0 .. current_windows-1]:
 *   Must contain consecutive, valid windows.
 *
 * fds[i+1]:
 *   Must reference term_win[i].master.
 *
 * focused:
 *   Must be < current_windows when windows exist.
 *
 * tree:
 *   Every active leaf references a live window.
 *   Internal nodes contain left/right child IDs.
 *
 * root:
 *   0 means no active window.
 *   Otherwise references the active tree root.
 *
 * Text buffers:
 *   Each window owns its own terminal text.
 *   Coordinates are local to that window.
 */

#include <fcntl.h>
#include <string.h>
#include <linux/fb.h>
#include <linux/kd.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include <pty.h>
#include <poll.h>
#include <termios.h>
#include <sys/types.h>
#include "font_8x16.h"
#include <time.h>
#include <stdbool.h>

//-------------------------------------------------



//help menu coordiantes and size
#define HELP_X (screen_cols - HELP_W)
#define HELP_Y 3
#define HELP_W 40
#define HELP_H 12


#define DRAW_BORDERS 0        //disable borders
#define windows_shadow 1      //disable windows shadows
#define CONFIGURATOR 1                // disable/enable  /etc/p1tiler.config



#define MAX_COLS 160
#define MAX_ROWS 64                  //for  the fixed 1280x1024 resolution for 8x16 pixels cell = 10240 cells =1280 bytes

#define DIRTY_WORDS 5                // 160 / 32
#define MAX_WINDOWS 4                 //max shell/terminals/windows to exist
#define  TOP_BAR 1                         //if true the top bar exists

#define MAX_WORKSPACES 3              //max workspaces to exist
uint8_t gap = 1;                      //variable for windows gap
bool help_visible=0;                   //variable for timing the help menu (DO NOT TOUCH)

static char bar_text[MAX_COLS];    //the text of the bar output (on icons or imgs just old school text)

static uint8_t dirty_words[MAX_ROWS];              //array to optimize dirty bit reading/writing
static uint32_t dirty[MAX_ROWS][DIRTY_WORDS];   //dirty table bitmask tracking individual cells
static uint32_t  glyph_mask[MAX_WORKSPACES]      //table that holds the position of the text glyphs
[MAX_ROWS][DIRTY_WORDS];
static uint32_t inverse_mask[MAX_WORKSPACES][MAX_ROWS][DIRTY_WORDS];   //detect inverse ASCII chat for terminal and apps


#if windows_shadow == 1
static bool shadows_enabled= 1;
static uint32_t  shadow_mask[MAX_WORKSPACES][MAX_ROWS][DIRTY_WORDS];
#endif


static uint8_t *fb;                             //framebuffer memory map pointer
static struct fb_fix_screeninfo fixed_screen; //are determined entirely by the system hardware driver once a video mode is set and cant be directly modified by user-space apps
static struct fb_var_screeninfo var_screen;   //holds user-defined, runtime-changeable configurations like resolution and pixel depth

static int screen_cols;
static int screen_rows;

static uint8_t glyph_color_table[MAX_WORKSPACES][MAX_ROWS][MAX_COLS];  //hold the color for each glyph
static uint8_t text[MAX_WORKSPACES][MAX_ROWS][MAX_COLS];    //text glyph coordiantes

static int fullscreen_master = -1; //used with ./p1full app for giving the fb full control  to an app
static unsigned ram_shift;
static const char *ram_unit;

static const uint16_t ansi_palette[17] = {
    0x0000,  // 0  default (sentinel)

    0x0000,  // 1  Black
    0x5000,  // 2  Red
    0x0280,  // 3  Green
    0x5280,  // 4  Yellow
    0x0014,  // 5  Blue
    0x5014,  // 6  Magenta
    0x0294,  // 7  Cyan
    0x5AD6,  // 8  White

    0x4210,  // 9  Bright Black (Gray)
    0x7C00,  // 10 Bright Red
    0x03E0,  // 11 Bright Green
    0x7FE0,  // 12 Bright Yellow
    0x001F,  // 13 Bright Blue
    0x7C1F,  // 14 Bright Magenta
    0x03FF,  // 15 Bright Cyan
    0x7FFF   // 16 Bright White
};

static const char *const backgrounds[] = {            //backgroun names in folder
    "default00.bin",
    "default01.bin",
    "default02.bin",
    "default03.bin",
    "default04.bin",
    "default05.bin",
    "default06.bin",
    "default07.bin",

};


static   uint16_t focused_theme_colors[8] = {
    0x627F,0x37AA, 0x627F, 0x37AA,
    0x37AA, 0x76A4, 0x37AA, 0x37AA
};

static   uint16_t unfocused_theme_colors[8] = {
    0x6273, 0x65D2,  0x6273, 0x65D2,
    0x65D2, 0x19E9, 0x65D2, 0x65D2 3
};

static uint8_t bg_id = 0;       //current background
static uint8_t background_count;


static bool rgb565;


#define MAX_NODES 7
struct tree_node {              //Binary tree node for window layout splitting
    bool active;       
    bool vertical_split;
    uint8_t left;
    uint8_t right;
    uint8_t window_index;
};
static struct tree_node tree[MAX_WORKSPACES][MAX_NODES + 1];

static uint8_t root[MAX_WORKSPACES] = {1, 1, 1};          //the first root

uint8_t current_windows[MAX_WORKSPACES] = {0, 0, 0};  //index of which window recieves input

uint8_t focused[MAX_WORKSPACES] = {0, 0, 0};        //total count of  windows active

//Background index structure mapping cells to background blocks
struct bg_cell {
    uint16_t color[4];
    uint8_t pixels[32];
};

//the wallpaper index 
struct wallpaper {
    uint16_t cell_index[MAX_ROWS][MAX_COLS];
    struct bg_cell colored_cells[];
};

static struct wallpaper *background;

enum term_state{               //terminal control /escape state
TEXT =0,
ESC,
CSI
};

uint16_t focused_win_text = 0x37AA;   //color the focused shell will have
uint16_t unfocused_win_text = 0x65D2;  //color the rest of the shells will have
static int cursor_x = -1, cursor_y = -1;          //cursor global position
static int cursor_tab = -1, cursor_focus = -1;     //cursor magic

struct window {
    int params[8];            // csi params
    uint8_t param_idx;              // Tracks current parameter index (0 or 1)
    uint8_t state;                //state for the text terminal escape parser
    uint8_t x, y;                 //screen cells coordinations
    uint8_t w, h;                 //dimension in cell unitsf
    int tx, ty;                //terminal cursor coordinates
    int old_tx,old_ty;
    bool cursor;
    uint8_t current_color;
    uint16_t c_pos;           //cursor position coordiantes
    bool bold;                 //bold text
    bool inverse;              //inverse flag for future use on terminal graphichs mode
    int master;                   //PTY descriptor
    pid_t child;                  //shell proccess
    uint16_t x_px, y_px;          //some apps will need it
    uint8_t (*text)[MAX_COLS];    //windows text buffer pointer
    uint8_t (*colors)[MAX_COLS];   //pointer for index table glyph colors(the above one)
    struct winsize ws;            //window size structure for terminal resizes
    bool cursor_visible;          //for esc sequnces in terminal apps

};

static struct window term_win[MAX_WORKSPACES][MAX_WINDOWS];
uint8_t tab=0;                    //set to 0 for Initialization
  unsigned long total_ram;
uint8_t workspace =0;             //set to 0 for Initialization
static uint8_t redraw_node = 0; //which node to redraw the text for

//bars widgets
static uint8_t bar_title_x;
static uint8_t bar_workspace_x;
static uint8_t bar_help_x;
static uint8_t bar_ram_x;
static uint8_t bar_cpu_x;
static uint8_t bar_date_x;

#if CONFIGURATOR == 1
static void configurator(void);   //function to  enable configuration from file
#endif

// Module implementations
#include "render.c"
#include "windows.c"
#include "terminal_control.c"
#include "terminal.c"






// THE LOVELY MAIN FUNCTION ,where my code starts!!!!!!!!!!!!!-----------------------------------
int main(void)
{
    #if CONFIGURATOR == 1
     configurator();
#endif

    size_t size;
    int fd;
    int tty;
    int i=0;
    tree[tab][root[tab]].active = true;
    tree[tab][root[tab]].window_index = 0;

    struct window *win;

    fd = open("/dev/fb0", O_RDWR);     //open framebuffer device
    if (fd < 0) {
        perror("/dev/fb0");
        return 1;
    }

    if (ioctl(fd, FBIOGET_FSCREENINFO, &fixed_screen) < 0 ||
        ioctl(fd, FBIOGET_VSCREENINFO, &var_screen) < 0) {
        perror("fb ioctl");
    close(fd);
    return 1;
        }

        screen_cols = var_screen.xres >> 3;
        screen_rows = var_screen.yres >> 4;

        if (var_screen.bits_per_pixel != 16) {
            fprintf(stderr, "P1Tiler requires 16bpp framebuffer\n");
            return 1;
        }

        if (var_screen.red.offset == 11 &&
            var_screen.green.length == 6 &&
            var_screen.blue.offset == 0) {

            rgb565 = true;

            } else if (var_screen.red.offset == 10 &&
                var_screen.green.length == 5 &&
                var_screen.blue.offset == 0) {

                rgb565 = false;

                } else {
                    fprintf(stderr, "Unsupported framebuffer color format\n");
                    return 1;
                }

        size = fixed_screen.line_length * var_screen.yres;

        // Memory-map the framebuffer into user space
        fb = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (fb == MAP_FAILED) {
            perror("mmap");
            return 1;
        }

        tty = open("/dev/tty1", O_RDWR);
        if (tty >= 0)
            ioctl(tty, KDSETMODE, KD_GRAPHICS);

    struct termios old_term;
    struct termios raw_term;

    tcgetattr(tty, &old_term);

    raw_term = old_term;
    cfmakeraw(&raw_term);

    tcsetattr(tty, TCSANOW, &raw_term);
    ioctl(tty, KDSETMODE, KD_GRAPHICS);

    if (screen_cols > MAX_COLS || screen_rows > MAX_ROWS) {
        fprintf(stderr, "Framebuffer resolution exceeds P1Tiler limits\n");
        return 1;
    }

    background_count = 9;

    if (var_screen.xres == 1280 && var_screen.yres == 1024)
        background_count =7;

    rgb565 = var_screen.bits_per_pixel == 16 &&
    var_screen.red.offset == 11 &&
    var_screen.green.length == 6 &&
    var_screen.blue.offset == 0;


    struct winsize ws;

    ws.ws_row = MAX_ROWS;
    ws.ws_col = MAX_COLS;
    ws.ws_xpixel = var_screen.xres;
    ws.ws_ypixel = var_screen.yres;
    current_windows[tab] = 1;

    // Initialize initial window and fork shell PTY
    init_window(&term_win[tab][i], 0, 0, screen_cols , screen_rows , text[tab], glyph_color_table[tab]);
    term_win[tab][i].child = forkpty(&term_win[tab][i].master, NULL, NULL, &term_win[tab][i].ws);

    if (term_win[tab][i].child < 0) {
        perror("forkpty");
        return 1;
    }

    if (term_win[tab][i].child == 0) {
        setenv("TERM", "linux", 1);
        execl("/bin/sh", "sh", NULL);
        _exit(127);
    }


    //load background
   char background_path[128];

    snprintf(background_path, sizeof background_path,
             "/p1tiler/backgrounds/%ux%u/default00.bin",
             var_screen.xres, var_screen.yres);

    background_load(background_path);




     total_ram = physical_ram();


    struct pollfd fds[MAX_WORKSPACES][1+ MAX_WINDOWS];
    for (int w = 0; w < MAX_WORKSPACES; w++)
        for (int i = 0; i <= MAX_WINDOWS; i++)
            fds[w][i].fd = -1;

    char buf[256];




    fds[tab][0].fd = tty;
    fds[tab][0].events = POLLIN;

    for (i = 0; i < current_windows[tab]; i++) {
        fds[tab][i+1].fd = term_win[tab][i].master;
        fds[tab][i+1].events = POLLIN;
    }

    if (TOP_BAR) {

        for (int x = 0; x < screen_cols; x++)
            background_draw(x, 0);
    }

    mark_dirty_rect(0, 0, screen_cols << 3, screen_rows << 4);




    //top bar distances set up for calculations
    int spacing = (screen_cols - (10 + 9 + 11 + 11 + 8 + 25)  ) / 5;

    bar_title_x = 1;
    bar_workspace_x = bar_title_x + 9;
    bar_help_x = bar_title_x + 22 + spacing;
    bar_ram_x = bar_help_x + 11 + spacing;
    bar_cpu_x = bar_ram_x + spacing +15 ;

    use_top_bar("P1Tiler  [1]  [2]  [3]",bar_title_x, 0x2B9C);
    use_top_bar("[1]", bar_title_x + 9 , 0x5C88);
    use_top_bar("Alt+h: Help" , bar_help_x, 0x627F);
    use_top_bar("RAM:",  bar_ram_x , 0x2B9C);
  //  use_top_bar("/8192", 86, 0x2B9C);
  //  use_top_bar("KB", 91, 0xFEE0);

    use_top_bar("CPU:", bar_cpu_x , 0x2B9C);
    use_top_bar("%", bar_cpu_x + 7 , 0x2B9C);



    layout_function(tab,root[tab], 0, 0, screen_cols, screen_rows   );

    redraw_dirty();


    for (;;) {


        if (poll(&fds[0][0], MAX_WORKSPACES * (MAX_WINDOWS + 1), 1000) < 0){
            break;
        }
          bar_timer();




        // keyboard input  -> shell
        if (fds[0][0].revents & POLLIN) {
            int n = read(tty, buf, sizeof(buf));
            if (n > 0) {
                if (fullscreen_master >= 0)
                    write(fullscreen_master, buf, n);
                else
                    keyboard_input_handler(fds, buf, n);
            }
        }


        // shell PTY output -> terminal renderer
        for (int w = 0; w < MAX_WORKSPACES; w++) {
            workspace = w;
            for (int i = 0; i < current_windows[w]; i++) {

                if (fds[w][i + 1].revents & POLLIN) {
                    int n = read(fds[w][i + 1].fd, buf, sizeof buf);

                    if (n > 0) {

                        struct window *win = &term_win[w][i];

                        win->old_tx = win->tx;
                        win->old_ty = win->ty;

                        terminal_output(win, buf, n);

                        if (w == tab && i == focused[tab] &&
                            (win->old_tx != win->tx || win->old_ty != win->ty)) {

                            mark_dirty_cell(win->x + win->old_tx, win->y + win->old_ty);
                            mark_dirty_cell(win->x + win->tx, win->y + win->ty);

                        }

                    }


            }

            if (fds[w][i + 1].revents & POLLHUP) {
                terminal_closer(fds,w,i);
                break;
            }

            }
        }
        if (fullscreen_master < 0){
        update_cursor();
          redraw_dirty();
        }


    }


    //clean up
    tcsetattr(tty, TCSANOW, &old_term);
    ioctl(tty, KDSETMODE, KD_TEXT);
    close(tty);
    munmap(fb, size);
    close(fd);

    return 0;
}


static void fullscreen_start(struct window *win)
{
    if (fullscreen_master >= 0)
        return;
      fullscreen_master = win->master;




}


static void fullscreen_end(struct window *win)
{
    if (fullscreen_master != win->master)
        return;

    fullscreen_master = -1;

    mark_dirty_rect(0, 16,
                     screen_cols << 3,
                   ( screen_rows << 4) - 16);
}










#if CONFIGURATOR == 1

  void configurator(void)
{

    FILE *f = fopen("/p1tiler/p1tiler.conf", "r");
    if (!f) return;

    char line[256];

    while (fgets(line, sizeof line, f)) {
        char *value = strchr(line, '=');

        if (!value) continue;

        *value++ = '\0';



        if (!strcmp(line, "gap")){
          int new_gap = atoi(value);
        if (new_gap < 0 || new_gap > 3) new_gap = 0;

        gap = new_gap;

        }

#if windows_shadow == 1
        else if (!strcmp(line, "shadows"))
            shadows_enabled = atoi(value);
#endif
        else if (!strcmp(line, "focused_color"))
            focused_win_text = strtoul(value, NULL, 0);

        else if (!strcmp(line, "unfocused_color"))
            unfocused_win_text = strtoul(value, NULL, 0);


        else {
            int i, n = 0;
            char type;

            if (sscanf(line, "wallpaper%d_color%c%n", &i, &type, &n) == 2 &&
                line[n] == '\0' && (unsigned)i < 9) {

                char *end;
            uint16_t color = strtoul(value, &end, 0);

            if (end != value) {
                if (type == 'F')
                    focused_theme_colors[i] = color;
                else if (type == 'U')
                    unfocused_theme_colors[i] = color;
                     }

               }

         }

        }

        fclose(f);
}

#endif



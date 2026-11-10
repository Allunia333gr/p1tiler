#include <fcntl.h>
#include <unistd.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>



/* The renderer uses a fixed 160 x 64 cell index grid, regardless of the
 * actual framebuffer resolution. Only active cells are populated. */
#define WALL_COLS 160
#define WALL_ROWS 64

int solid_blocks_percent = 36;   //warning threshold, no longer discards usable wallpapers


struct bg_cell {
    uint16_t color[4];      /* 8 bytes: C0 C1 C2 C3 */
    uint8_t pixels[32];     /* 128 pixels, 2 bits each */
};

typedef struct {
    uint16_t pal[4];   /* 4 RGB555 colors */
    uint8_t  pix[32];  /* 128 pixels * 2 bits */
} Cell8x16;

static int r5(uint16_t c) { return (c >> 10) & 31; }
static int g5(uint16_t c) { return (c >> 5)  & 31; }
static int b5(uint16_t c) { return c & 31; }

static int color_dist(uint16_t a, uint16_t b)
{
    int dr = r5(a) - r5(b);
    int dg = g5(a) - g5(b);
    int db = b5(a) - b5(b);
  //  return dr*dr + dg*dg + db*db;
    return (3 * dr * dr) + (3 * dg * dg) + (4 * db * db);
}

static int nearest_palette_index(uint16_t color, uint16_t pal[4])
{
    int best = 0;
    int best_d = color_dist(color, pal[0]);

    for (int i = 1; i < 4; i++) {
        int d = color_dist(color, pal[i]);
        if (d < best_d) {
            best_d = d;
            best = i;
        }
    }

    return best;
}



static int fourth_matches_neighbor(
    int cell_x,
    int cell_y,
    int cells_x,
    int cells_y,
    uint16_t (*palettes)[4])
{
    int id = cell_y * cells_x + cell_x;
    uint16_t color = palettes[id][3];

    /* left */
    if (cell_x > 0 &&
        palettes[id - 1][3] == color)
        return 1;

    /* right */
    if (cell_x + 1 < cells_x &&
        palettes[id + 1][3] == color)
        return 1;

    /* above */
    if (cell_y > 0 &&
        palettes[id - cells_x][3] == color)
        return 1;

    /* below */
    if (cell_y + 1 < cells_y &&
        palettes[id + cells_x][3] == color)
        return 1;

    return 0;
}





/* Preview/statistics implementation is appended after main(). */
static void preview_and_stats(const char *bin_path, const uint16_t *original,
                              int width, int height, int target_width,
                              int target_height, int total_cells, int solid_count);

int main(int argc, char **argv)
{
    int fd_in, fd_out;
    uint8_t header[54];
    uint32_t data_offset;
    int32_t width;
    int32_t height;
    uint16_t bpp;
    int y;
    int row_bytes;
    int bmp_stride;
    int target_width, target_height;

    if (argc != 3 && argc != 5) {
        fprintf(stderr, "Usage: %s <input.bmp> <output.bin> [fb_width fb_height]\n", argv[0]);
        return 1;
    }


    fd_in = open(argv[1], O_RDONLY);
    if (fd_in < 0) {
        perror("Open BMP failed");
        return 1;
    }

    if (read(fd_in, header, 54) != 54) {
        close(fd_in);
        return 1;
    }

    data_offset = header[10] | (header[11] << 8) | (header[12] << 16) | (header[13] << 24);
    width       = header[18] | (header[19] << 8) | (header[20] << 16) | (header[21] << 24);
    height      = header[22] | (header[23] << 8) | (header[24] << 16) | (header[25] << 24);
    bpp         = header[28] | (header[29] << 8);

    if (header[0] != 'B' || header[1] != 'M' || bpp != 16) {
        fprintf(stderr, "Error: Expected 16bpp RGB555 BMP\n");
        close(fd_in);
        return 1;
    }

    fd_out = open(argv[2], O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (fd_out < 0) {
        perror("Open output raw file failed");
        close(fd_in);
        return 1;
    }



    if (width <= 0 || height <= 0) {
        fprintf(stderr, "Converter expects a normal bottom-up BMP\n");
        close(fd_in);
        close(fd_out);
        unlink(argv[2]);
        return 1;
    }

    if ((width & 7) || (height & 15) ||
        width > WALL_COLS * 8 || height > WALL_ROWS * 16) {
        fprintf(stderr,
                "Image must fit 160x64 cells; width multiple of 8, height multiple of 16\n");
        close(fd_in);
        close(fd_out);
        unlink(argv[2]);
        return 1;
    }

    target_width = width;
    target_height = height;
    if (argc == 5) {
        target_width = atoi(argv[3]);
        target_height = atoi(argv[4]);
        if (target_width <= 0 || target_height <= 0 ||
            target_width / 8 * 8 != width ||
            target_height / 16 * 16 != height) {
            fprintf(stderr, "BMP must match the drawable area of the target resolution\n");
            close(fd_in);
            close(fd_out);
            unlink(argv[2]);
            return 1;
        }
    }

    row_bytes = width * 2;
    // BMP standard requires rows to be padded to a multiple of 4 bytes
    bmp_stride = (row_bytes + 3) & ~3;

    uint16_t (*image)[width];
    image = malloc((size_t)height * sizeof *image);
    if (!image) {
        perror("malloc image");
        close(fd_in);
        close(fd_out);
        unlink(argv[2]);
        return 1;
    }

    for (y = 0; y < height; y++) {
        /* BMP row 0 is normally the bottom of the picture; flip it right-side up */
        int bmp_y = height - 1 - y;



        if (lseek(fd_in, (off_t)data_offset + (off_t)bmp_y * bmp_stride, SEEK_SET) < 0 ||
            read(fd_in, image[y], (size_t)row_bytes) != row_bytes) {
            fprintf(stderr, "BMP read failed at row %d\n", y);
            close(fd_in);
            close(fd_out);
            unlink(argv[2]);
            free(image);
            return 1;
        }


    }
    close (fd_in);


    int cells_x = width / 8;
    int cells_y = height / 16;
    int total_cells = cells_x * cells_y;

    Cell8x16 *cells = malloc((size_t)total_cells * sizeof(Cell8x16));
    if (!cells) {
        perror("malloc cells");
        free(image);
        return 1;
    }

    for (int cy = 0; cy < cells_y; cy++) {
        for (int cx = 0; cx < cells_x; cx++) {

            int cell_id = cy * cells_x + cx;
            Cell8x16 *cell = &cells[cell_id];

            memset(cell, 0, sizeof(*cell));


            uint16_t cell_colors[128];
            int cell_counts[128];
            int cell_unique_count = 0;

            for (int i = 0; i < 128; i++)
                cell_counts[i] = 0;

            for (int py = 0; py < 16; py++) {
                for (int px = 0; px < 8; px++) {
                    int x = cx * 8 + px;
                    int y = cy * 16 + py;
                    uint16_t color = image[y][x];

                    int found = -1;
                    for (int i = 0; i < cell_unique_count; i++) {
                        if (cell_colors[i] == color) {
                            found = i;
                            break;
                        }
                    }

                    if (found >= 0) {
                        cell_counts[found]++;
                    } else {
                        cell_colors[cell_unique_count] = color;
                        cell_counts[cell_unique_count] = 1;
                        cell_unique_count++;
                    }
                }
            }


            uint16_t chosen_pal[4] = {0, 0, 0, 0};

            if (cell_unique_count <= 4) {

                for (int i = 0; i < cell_unique_count; i++) {
                    chosen_pal[i] = cell_colors[i];
                }

                for (int i = cell_unique_count; i < 4; i++) {
                    chosen_pal[i] = (cell_unique_count > 0) ? cell_colors[cell_unique_count - 1] : 0;
                }
            }
         //   else if (cell_unique_count <= 16) {



       //     }
            else {

                int top_indices[4] = {-1, -1, -1, -1};
                int top_counts[4] = {-1, -1, -1, -1};

                for (int i = 0; i < cell_unique_count; i++) {
                    int count = cell_counts[i];
                    for (int j = 0; j < 4; j++) {
                        if (count > top_counts[j]) {
                            for (int k = 3; k > j; k--) {
                                top_counts[k] = top_counts[k - 1];
                                top_indices[k] = top_indices[k - 1];
                            }
                            top_counts[j] = count;
                            top_indices[j] = i;
                            break;
                        }
                    }
                }

                for (int i = 0; i < 4; i++) {
                    if (top_indices[i] >= 0) {
                        chosen_pal[i] = cell_colors[top_indices[i]];
                    } else {
                        chosen_pal[i] = chosen_pal[i - 1]; // Safe fallback padding
                    }
                }
            }


            for (int i = 0; i < 4; i++) {
                cell->pal[i] = chosen_pal[i];
            }


            for (int py = 0; py < 16; py++) {
                for (int px = 0; px < 8; px++) {
                    int x = cx * 8 + px;
                    int y = cy * 16 + py;
                    uint16_t color = image[y][x];

                    int idx = nearest_palette_index(color, cell->pal);

                    int n = py * 8 + px;
                    int byte_index = n >> 2;
                    int shift = (3 - (n & 3)) * 2;



                    cell->pix[byte_index] |= (idx & 3) << shift;
                }
            }
        }
    }

/* Count solid cells. Always write the indexed format that P1Tiler expects.
 * The 15-bit byte_offset/8 field has room for 6554 multicolor cells. */
int solid_count = 0;
for (int k = 0; k < total_cells; k++) {
    Cell8x16 *cell = &cells[k];
    uint8_t p = cell->pix[0];
    int solid = (p == 0x00 || p == 0x55 || p == 0xAA || p == 0xFF);
    for (int b = 1; b < 32 && solid; b++) {
        if (cell->pix[b] != p)
            solid = 0;
    }
    if (solid)
        solid_count++;
}

if ((long long)solid_count * 100 <=
    (long long)total_cells * solid_blocks_percent) {
    printf("NOTE: Only %d%% solid cells; keeping wallpaper if it fits.\n",
           solid_count * 100 / total_cells);
}

if (total_cells - solid_count > (0x7FFF / 5) + 1) {
    fprintf(stderr, "Too many multicolor cells for 15-bit indexed offsets: %d\n",
            total_cells - solid_count);
    close(fd_out);
    unlink(argv[2]);
    free(cells);
    free(image);
    return 1;
}

int cct = 0;
const size_t index_bytes = WALL_COLS * WALL_ROWS * sizeof(uint16_t);
uint16_t *cell_ind = malloc(index_bytes);
Cell8x16 *colored_cells = malloc((size_t)(total_cells - solid_count) * sizeof(Cell8x16) + 1);
if (!cell_ind || !colored_cells) {
    perror("malloc indexed wallpaper");
    close(fd_out);
    unlink(argv[2]);
    free(cell_ind);
    free(colored_cells);
    free(cells);
    free(image);
    return 1;
}

/* All non-drawable cells remain solid black. The 160-column stride matches
 * the renderer's struct wallpaper.cell_index[MAX_ROWS][MAX_COLS]. */
for (int k = 0; k < WALL_COLS * WALL_ROWS; k++)
    cell_ind[k] = 0x8000;

for (int k = 0; k < total_cells; k++) {
    Cell8x16 *cell = &cells[k];
    int cy = k / cells_x;
    int cx = k % cells_x;
    int pos = cy * WALL_COLS + cx;
    uint8_t p = cell->pix[0];
    int solid = (p == 0x00 || p == 0x55 || p == 0xAA || p == 0xFF);
    for (int b = 1; b < 32 && solid; b++) {
        if (cell->pix[b] != p)
            solid = 0;
    }

    if (solid) {
        cell_ind[pos] = 0x8000 | (cell->pal[p & 3] & 0x7FFF);
    } else {
        cell_ind[pos] = (uint16_t)(cct * 5);
        colored_cells[cct++] = *cell;
    }
}

/* Verify the same index encoding the runtime decoder uses. */
int errors = 0;
for (int k = 0; k < total_cells; k++) {
    int pos = (k / cells_x) * WALL_COLS + k % cells_x;
    uint16_t entry = cell_ind[pos];
    if (entry & 0x8000) {
        uint16_t color = entry & 0x7FFF;
        for (int p = 0; p < 128; p++) {
            int shift = (3 - (p & 3)) * 2;
            int idx = (cells[k].pix[p >> 2] >> shift) & 3;
            if (cells[k].pal[idx] != color) {
                errors++;
                break;
            }
        }
    } else {
        int multicolor_pos = entry / 5;
        if (entry % 5 || multicolor_pos >= cct ||
            memcmp(&cells[k], &colored_cells[multicolor_pos],
                   sizeof(Cell8x16)))
            errors++;
    }
}
printf("Verification errors: %d\n", errors);

size_t color_bytes = (size_t)cct * sizeof(Cell8x16);
if (errors ||
    write(fd_out, cell_ind, index_bytes) != (ssize_t)index_bytes ||
    write(fd_out, colored_cells, color_bytes) != (ssize_t)color_bytes) {
    fprintf(stderr, "Indexed wallpaper verification/write failed\n");
    close(fd_out);
    unlink(argv[2]);
    free(cell_ind);
    free(colored_cells);
    free(cells);
    free(image);
    return 1;
}

printf("Target framebuffer: %d x %d\n", target_width, target_height);
printf("Drawable wallpaper: %d x %d (%d x %d cells)\n",
       width, height, cells_x, cells_y);
printf("Padded cell index: %d x %d, multicolor cells: %d\n",
       WALL_COLS, WALL_ROWS, cct);

close(fd_out);
preview_and_stats(argv[2], &image[0][0], width, height,
                  target_width, target_height, total_cells, solid_count);

free(cell_ind);
free(colored_cells);
free(cells);
free(image);
return 0;
}

/* -------------------------------------------------------------------------
 * Offline preview and statistics: read back the indexed .bin we wrote.
 * ------------------------------------------------------------------------- */
static void put16le(uint8_t *p, uint16_t value)
{
    p[0] = value;
    p[1] = value >> 8;
}

static void put32le(uint8_t *p, uint32_t value)
{
    p[0] = value;
    p[1] = value >> 8;
    p[2] = value >> 16;
    p[3] = value >> 24;
}

static void preview_and_stats(const char *bin_path, const uint16_t *original,
                              int width, int height, int target_width,
                              int target_height, int total_cells, int solid_count)
{
    FILE *in = fopen(bin_path, "rb");
    if (!in) {
        perror("Preview: open compressed file");
        return;
    }

    fseek(in, 0, SEEK_END);
    long file_len = ftell(in);
    rewind(in);

    size_t first_pass_bytes = (size_t)total_cells * sizeof(Cell8x16);
    size_t expected_bytes = (size_t)WALL_COLS * WALL_ROWS * sizeof(uint16_t)
        + (size_t)(total_cells - solid_count) * sizeof(Cell8x16);

    if (file_len < 0 || (size_t)file_len != expected_bytes) {
        fprintf(stderr, "Preview: unexpected compressed size (%ld; expected %zu)\n",
                file_len, expected_bytes);
        fclose(in);
        return;
    }

    uint8_t *data = malloc(expected_bytes);
    if (!data) {
        fclose(in);
        return;
    }
    if (fread(data, 1, expected_bytes, in) != expected_bytes) {
        fprintf(stderr, "Preview: could not read compressed file\n");
        fclose(in);
        free(data);
        return;
    }
    fclose(in);

    char *preview_path = malloc(strlen(bin_path) + sizeof(".preview.bmp"));
    if (!preview_path) {
        free(data);
        return;
    }
    sprintf(preview_path, "%s.preview.bmp", bin_path);

    FILE *out = fopen(preview_path, "wb");
    if (!out) {
        perror("Preview: open BMP output");
        free(preview_path);
        free(data);
        return;
    }

    /* Standard bottom-up 16-bit BMP with explicit RGB555 bit masks. */
    size_t stride = ((size_t)width * 2 + 3) & ~(size_t)3;
    uint8_t bmp[66] = {0};
    bmp[0] = 'B';
    bmp[1] = 'M';
    put32le(bmp + 2, (uint32_t)(66 + stride * height));
    put32le(bmp + 10, 66);
    put32le(bmp + 14, 40);                 /* BITMAPINFOHEADER */
    put32le(bmp + 18, (uint32_t)width);
    put32le(bmp + 22, (uint32_t)height);
    put16le(bmp + 26, 1);                 /* planes */
    put16le(bmp + 28, 16);                /* bits per pixel */
    put32le(bmp + 30, 3);                 /* BI_BITFIELDS */
    put32le(bmp + 34, (uint32_t)(stride * height));
    put32le(bmp + 54, 0x7C00);            /* red: 5 bits */
    put32le(bmp + 58, 0x03E0);            /* green: 5 bits */
    put32le(bmp + 62, 0x001F);            /* blue: 5 bits */
    fwrite(bmp, 1, sizeof bmp, out);

    uint8_t *row = calloc(1, stride);
    if (!row) {
        fclose(out);
        free(preview_path);
        free(data);
        return;
    }

    uint64_t exact_pixels = 0;
    uint64_t absolute_error = 0;
    int cells_x = width / 8;
    const size_t table_bytes = (size_t)WALL_COLS * WALL_ROWS * sizeof(uint16_t);

    for (int y = height - 1; y >= 0; y--) {
        for (int cx = 0; cx < cells_x; cx++) {
            int cell_y = y & 15;
            const Cell8x16 *cell = NULL;
            uint16_t solid_color = 0;
            int is_solid = 0;

            const uint8_t *p = data + ((size_t)(y / 16) * WALL_COLS + cx) * 2;
            uint16_t entry = (uint16_t)p[0] | ((uint16_t)p[1] << 8);
            if (entry & 0x8000) {
                is_solid = 1;
                solid_color = entry & 0x7FFF;
            } else {
                /* Same mapping as P1Tiler: entry is a byte offset / 8. */
                cell = (const Cell8x16 *)(data + table_bytes
                                              + ((size_t)entry << 3));
            }

            for (int px = 0; px < 8; px++) {
                uint16_t color;
                if (is_solid) {
                    color = solid_color;
                } else {
                    int n = cell_y * 8 + px;
                    int shift = (3 - (n & 3)) * 2;
                    int idx = (cell->pix[n >> 2] >> shift) & 3;
                    color = cell->pal[idx] & 0x7FFF;
                }

                int x = cx * 8 + px;
                put16le(row + 2 * x, color);
                uint16_t orig = original[(size_t)y * width + x] & 0x7FFF;
                if (orig == color) exact_pixels++;
                absolute_error += abs(r5(orig) - r5(color));
                absolute_error += abs(g5(orig) - g5(color));
                absolute_error += abs(b5(orig) - b5(color));
            }
        }
        fwrite(row, 1, stride, out);
    }

    fclose(out);
    free(row);
    free(data);

    size_t raw_bytes = (size_t)width * height * 2;
    size_t pixels = (size_t)width * height;
    printf("\n--- Wallpaper compression stats ---\n");
    printf("Target fb:       %d x %d\n", target_width, target_height);
    printf("Image data:      %d x %d, RGB555\n", width, height);
    printf("Cells:           %d (%d x %d, 8x16 pixels)\n",
           total_cells, cells_x, height / 16);
    printf("Solid cells:     %d (%.1f%%, after palette reduction)\n",
           solid_count, 100.0 * solid_count / total_cells);
    printf("Multicolor:      %d\n", total_cells - solid_count);
    printf("Output format:   fixed 160x64 index + 40-byte multicolor cells\n");
    printf("Raw RGB555:      %zu bytes (%.2f KiB)\n", raw_bytes, raw_bytes / 1024.0);
    printf("First pass:      %zu bytes (%.2f KiB)\n",
           first_pass_bytes, first_pass_bytes / 1024.0);
    printf("Final binary:    %zu bytes (%.2f KiB)\n",
           expected_bytes, expected_bytes / 1024.0);
    printf("Size saved:      %.1f%% vs raw, %.1f%% vs first pass\n",
           100.0 * (1.0 - (double)expected_bytes / raw_bytes),
           100.0 * (1.0 - (double)expected_bytes / first_pass_bytes));
    printf("Bits per pixel:  %.3f\n", 8.0 * expected_bytes / pixels);
    printf("Exact pixels:    %.2f%%\n", 100.0 * exact_pixels / pixels);
    printf("Avg RGB error:   %.3f / 31 per color channel\n",
           (double)absolute_error / (3.0 * pixels));
    printf("Preview:         %s\n", preview_path);

    free(preview_path);
}

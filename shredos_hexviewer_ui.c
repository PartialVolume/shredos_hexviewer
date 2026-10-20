#include "shredos_hexviewer.h"

void init_ncurses_ui(void) {
    initscr();
    cbreak();
    noecho();
    curs_set(0);
    start_color();
    use_default_colors();

    if (COLORS >= 256) {
        init_pair(CP_BORDER, 33, -1);   // Vibrant Blue for borders
        init_pair(CP_TITLE, 205, -1);   // Vivid Hot Pink for titles
        init_pair(CP_GREEN, 46, -1);    // Bright Green
    } else {
        init_pair(CP_BORDER, COLOR_BLUE, -1);
        init_pair(CP_TITLE, COLOR_MAGENTA, -1);
        init_pair(CP_GREEN, COLOR_GREEN, -1);
    }
}

int get_dynamic_bytes_per_row(WINDOW *win) {
    int maxx;
    maxx = getmaxx(win);

    int bytes = (maxx - 23) / 4;

    if (bytes < 4)  bytes = 4;   // Safe floor for narrow 80-col terminals
    if (bytes > 80) bytes = 80;  // Raised ceiling for ultra-wide 300+ col displays
    return bytes;
}

void show_message_popup(const char *title, const char *msg) {
    int h = 9;
    int w = 72;
    int y = (LINES - h) / 2;
    int x = (COLS - w) / 2;
    WINDOW *win = newwin(h, w, y, x);
    box(win, 0, 0);
    wattron(win, COLOR_PAIR(CP_TITLE) | A_BOLD);
    mvwprintw(win, 0, 2, " %s ", title);
    wattroff(win, COLOR_PAIR(CP_TITLE) | A_BOLD);

    char line_buf[256];
    int r = 2;
    const char *p = msg;
    while (*p && r < h - 3) {
        const char *nl = strchr(p, '\n');
        size_t len = nl ? (size_t)(nl - p) : strlen(p);
        if (len >= sizeof(line_buf)) len = sizeof(line_buf) - 1;
        memcpy(line_buf, p, len);
        line_buf[len] = '\0';

        mvwprintw(win, r, 3, "%s", line_buf);
        r++;
        if (!nl) break;
        p = nl + 1;
    }

    mvwprintw(win, h - 2, (w - 27) / 2, "Press any key to continue");
    wrefresh(win);
    wgetch(win);
    delwin(win);
    touchwin(stdscr);
}

void format_human_size(off_t bytes, char *buf, size_t buf_size) {
    double size = (double)bytes;
    if (size >= 1000.0 * 1000.0 * 1000.0 * 1000.0) {
        snprintf(buf, buf_size, "%.2f TB", size / (1000.0 * 1000.0 * 1000.0 * 1000.0));
    } else if (size >= 1000.0 * 1000.0 * 1000.0) {
        snprintf(buf, buf_size, "%.2f GB", size / (1000.0 * 1000.0 * 1000.0));
    } else if (size >= 1000.0 * 1000.0) {
        snprintf(buf, buf_size, "%.2f MB", size / (1000.0 * 1000.0));
    } else if (size >= 1000.0) {
        snprintf(buf, buf_size, "%.2f KB", size / 1000.0);
    } else {
        snprintf(buf, buf_size, "%llu B", (unsigned long long)bytes);
    }
}

ssize_t safe_read(int fd, void *buf, size_t count, const char *dev_name, off_t offset) {
    if (lseek(fd, offset, SEEK_SET) == (off_t)-1) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Seek Error on %s at offset 0x%012lX:\n%s (errno: %d)",
                 dev_name, (unsigned long)offset, strerror(errno), errno);
        show_message_popup("Hardware I/O Error", msg);
        return -1;
    }
    ssize_t n = read(fd, buf, count);
    if (n == -1) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Read Error on %s at offset 0x%012lX:\n%s (errno: %d)",
                 dev_name, (unsigned long)offset, strerror(errno), errno);
        show_message_popup("Hardware I/O Error", msg);
    }
    return n;
}

void render_hex_view(WINDOW *win, drive_info_t *drive) {
    int rows;
    int cols;
    getmaxyx(win, rows, cols);
    int display_rows = rows - 2;
    if (display_rows < 1) display_rows = 1;

    int bytes_per_row = get_dynamic_bytes_per_row(win);

    unsigned char *buffer = malloc(display_rows * bytes_per_row);
    if (!buffer) return;
    memset(buffer, 0, display_rows * bytes_per_row);

    if (drive->fd >= 0) {
        ssize_t bytes_read = safe_read(drive->fd, buffer, display_rows * bytes_per_row, drive->name, drive->offset);
        (void)bytes_read;
    }

    // NOTE: Removed box(win, 0, 0) here so borders are static and don't flicker on scroll!

    wattron(win, COLOR_PAIR(CP_TITLE) | A_BOLD);
    mvwprintw(win, 0, 2, " Hex View: %s [%s] ", drive->name, drive->signature);
    wattroff(win, COLOR_PAIR(CP_TITLE) | A_BOLD);

    unsigned long long block_no = (unsigned long long)(drive->offset / 512);
    unsigned long long block_start = block_no * 512;
    unsigned long long block_end = block_start + 511;

    char bottom_str[256];
    int len_with_range = snprintf(bottom_str, sizeof(bottom_str),
                                  " Block: %llu | Range: 0x%012lX - 0x%012lX | Offset: 0x%012lX ",
                                  block_no,
                                  (unsigned long)block_start,
                                  (unsigned long)block_end,
                                  (unsigned long)drive->offset);

    int max_len = cols - 4;
    if (len_with_range > max_len) {
        snprintf(bottom_str, sizeof(bottom_str),
                 " Block: %llu | Offset: 0x%012lX ",
                 block_no,
                 (unsigned long)drive->offset);
    }

    mvwprintw(win, rows - 1, 2, "%-*s", cols - 4, "");

    wattron(win, COLOR_PAIR(CP_TITLE) | A_BOLD);
    mvwprintw(win, rows - 1, 2, "%s", bottom_str);
    wattroff(win, COLOR_PAIR(CP_TITLE) | A_BOLD);

    char *hex_buf = malloc(bytes_per_row * 3 + 1);
    char *ascii_buf = malloc(bytes_per_row + 1);
    if (!hex_buf || !ascii_buf) {
        free(buffer);
        free(hex_buf);
        free(ascii_buf);
        return;
    }

    int inner_width = cols - 4;

    for (int i = 0; i < display_rows; i++) {
        off_t row_addr = drive->offset + (i * bytes_per_row);

        if (row_addr < drive->size) {
            hex_buf[0] = '\0';
            ascii_buf[0] = '\0';

            for (int j = 0; j < bytes_per_row; j++) {
                int idx = (i * bytes_per_row) + j;
                unsigned char c = buffer[idx];
                char byte_str[4];
                sprintf(byte_str, "%02X ", c);
                strcat(hex_buf, byte_str);

                if (c >= 32 && c <= 126) {
                    sprintf(ascii_buf + strlen(ascii_buf), "%c", c);
                } else {
                    strcat(ascii_buf, ".");
                }
            }

            // START AT COLUMN 1 with a leading space to overwrite column 1 padding!
            mvwprintw(win, i + 1, 1, " %012llX:  %-*s   %s",
                      (unsigned long long)row_addr,
                      bytes_per_row * 3,
                      hex_buf,
                      ascii_buf);
        } else {
            // Clear past end of drive starting at column 1
            mvwprintw(win, i + 1, 1, " %-*s", inner_width, "");
        }
    }

    free(hex_buf);
    free(ascii_buf);
    free(buffer);
    wrefresh(win);
}

void render_help_view(WINDOW *win, int scroll_offset) {
    int rows, cols;
    getmaxyx(win, rows, cols);
    int display_rows = rows - 2;
    if (display_rows < 1) display_rows = 1;
    int inner_width = cols - 4;
    if (inner_width < 20) inner_width = 20;

    wclear(win);
    wattron(win, COLOR_PAIR(CP_BORDER));
    box(win, 0, 0);
    wattroff(win, COLOR_PAIR(CP_BORDER));

    wattron(win, COLOR_PAIR(CP_TITLE) | A_BOLD);
    mvwprintw(win, 0, 2, " Help & Reference (Press 'h' or Esc to Exit) ");
    wattroff(win, COLOR_PAIR(CP_TITLE) | A_BOLD);

    typedef struct {
        const char *key;
        const char *desc;
    } help_item_t;

    typedef struct {
        const char *title;
        help_item_t items[8];
        int count;
    } help_sec_t;

    help_sec_t secs[] = {
        {
            "NAVIGATION & GENERAL",
            {
                {"Up / Down", "Move drive selection (Left Pane)"},
                {"PgUp / PgDn", "Scroll Hex View forward/backward by page"},
                {"< / >", "Jump to Start (0x0) or End of disk"},
                {"g / G", "Open Goto Menu (Percentage, Byte Offset, or Block Number)"},
                {"r / R", "Refresh / Reload drive list"},
                {"h / H", "Toggle this Help Screen"},
                {"q / Q", "Quit application"}
            },
            7
        },
        {
            "PATTERN & DATA SEARCH ('s')",
            {
                {"[1] Non-Zero", "Scan for any byte != 0x00"},
                {"[2] Non-0xFF", "Scan for any byte != 0xFF"},
                {"[3] Hex Pattern", "Match hex bytes with '\\?\\?' wildcards (e.g. 4D 5A \?\? 00)"},
                {"[4] ASCII (Case-S)", "Search exact text string"},
                {"[5] ASCII (Case-I)", "Search text ignoring case"},
                {"* Continuation", "Press 'n' on any hit to find next match"}
            },
            6
        },
        {
            "ENTROPY & ANALYSIS ('e')",
            {
                {"[1] Global Entropy", "Macro average across scope (0.0=Zeros, 8.0=Encrypted)"},
                {"[2] Block Outlier", "Scan 4KB blocks for structured data (Flags <5.0). Supports 'n'."},
                {"[3] Multi-Word Search", "Scan for sequences of 3+ words (ignoring apostrophes/quotes). Supports 'n'."},
                {"[4] Single Block Entropy", "Analyze Shannon entropy of a specific 512-byte block or offset."}
            },
            4
        },
        {
            "SEARCH SCOPE SYNTAX",
            {
                {"Percentages", "1%, 50%, 100% (100%=Full Scan; <100%=1000-Chunk Scatter)"},
                {"Byte Units", "500KB, 100MB, 8GB, 1TB"}
            },
            2
        }
    };

    typedef struct {
        int type;
        char text1[64];
        char text2[128];
    } render_line_t;

    render_line_t rlines[128];
    int rline_count = 0;

    for (int s = 0; s < 4; s++) {
        if (s > 0 && rline_count < 128) {
            rlines[rline_count].type = 0;
            rline_count++;
        }
        if (rline_count < 128) {
            rlines[rline_count].type = 1;
            snprintf(rlines[rline_count].text1, sizeof(rlines[rline_count].text1), "[ %s ]", secs[s].title);
            rline_count++;
        }

        for (int i = 0; i < secs[s].count && rline_count < 128; i++) {
            const char *k = secs[s].items[i].key;
            const char *d = secs[s].items[i].desc;

            if (inner_width >= 87) {
                rlines[rline_count].type = 2;
                snprintf(rlines[rline_count].text1, sizeof(rlines[rline_count].text1), "%s", k);
                snprintf(rlines[rline_count].text2, sizeof(rlines[rline_count].text2), "%s", d);
                rline_count++;
            } else {
                rlines[rline_count].type = 3;
                snprintf(rlines[rline_count].text1, sizeof(rlines[rline_count].text1), "%s", k);
                rline_count++;

                int max_w = inner_width - 6;
                if (max_w < 15) max_w = 15;

                const char *p = d;
                while (*p && rline_count < 128) {
                    while (*p == ' ') p++;
                    if (!*p) break;

                    char chunk[128] = {0};
                    int len = 0;
                    const char *word_start = p;

                    while (*p && len < max_w) {
                        chunk[len++] = *p++;
                    }
                    chunk[len] = '\0';

                    if (*p && *p != ' ' && strrchr(chunk, ' ')) {
                        char *last_space = strrchr(chunk, ' ');
                        int take_len = last_space - chunk;
                        chunk[take_len] = '\0';
                        p = word_start + take_len;
                        while (*p == ' ') p++;
                    }

                    rlines[rline_count].type = 4;
                    snprintf(rlines[rline_count].text2, sizeof(rlines[rline_count].text2), "%s", chunk);
                    rline_count++;
                }
            }
        }
    }

    if (scroll_offset < 0) scroll_offset = 0;
    if (scroll_offset > rline_count - display_rows) {
        scroll_offset = rline_count - display_rows;
        if (scroll_offset < 0) scroll_offset = 0;
    }

    for (int i = 0; i < display_rows; i++) {
        int idx = scroll_offset + i;
        if (idx >= rline_count) break;

        int y = i + 1;
        render_line_t *rl = &rlines[idx];

        if (rl->type == 1) {
            wattron(win, COLOR_PAIR(CP_TITLE) | A_BOLD);
            mvwprintw(win, y, 3, "%s", rl->text1);
            wattroff(win, COLOR_PAIR(CP_TITLE) | A_BOLD);
        }
        else if (rl->type == 2) {
            mvwprintw(win, y, 3, "   ");
            wattron(win, COLOR_PAIR(CP_GREEN) | A_BOLD);
            wprintw(win, "%-16s", rl->text1);
            wattroff(win, COLOR_PAIR(CP_GREEN) | A_BOLD);
            wprintw(win, " : %s", rl->text2);
        }
        else if (rl->type == 3) {
            mvwprintw(win, y, 3, "   ");
            wattron(win, COLOR_PAIR(CP_GREEN) | A_BOLD);
            wprintw(win, "%s", rl->text1);
            wattroff(win, COLOR_PAIR(CP_GREEN) | A_BOLD);
            wprintw(win, " :");
        }
        else if (rl->type == 4) {
            mvwprintw(win, y, 3, "     %s", rl->text2);
        }
    }

    if (rline_count > display_rows) {
        wattron(win, COLOR_PAIR(CP_TITLE));
        mvwprintw(win, rows - 1, cols - 18, "[Scroll: Up/Dn]");
        wattroff(win, COLOR_PAIR(CP_TITLE));
    }

    wrefresh(win);
}

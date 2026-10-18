#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <dirent.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <linux/fs.h>
#include <ncurses.h>
#include <time.h>
#include <sys/types.h>
#include <ctype.h>
#include <math.h>

#define MAX_PATH 512

#define CP_BORDER 1
#define CP_TITLE  2
#define CP_GREEN  3

typedef enum {
    SEARCH_NON_ZERO = 1,
    SEARCH_NON_FF,
    SEARCH_HEX_PATTERN,
    SEARCH_ASCII_PATTERN,
    SEARCH_ASCII_PATTERN_CI
} search_type_t;

typedef struct {
    unsigned char value;
    int is_wildcard; // 1 if wildcard (??), 0 if fixed value
} search_pattern_element_t;

typedef struct {
    char name[64];
    char path[MAX_PATH];
    char model_serial[128];
    char signature[32];
    int fd;
    off_t size;
    off_t offset;
    // Persistent search state for "Find Next" (n = next)
    int last_search_valid;
    search_type_t last_search_type;
    search_pattern_element_t last_hex_pat[64];
    unsigned char last_ascii_pat[64];
    int last_pat_len;
    off_t last_found_offset;
    unsigned long long last_target_bytes;
    int last_is_full_scan;
} drive_info_t;

// Function declarations
void init_ncurses_ui(void);
void get_model_and_serial(const char *dev_name, char *dest, size_t max_len);
void detect_drive_signature(drive_info_t *drive);
int check_drives_changed(drive_info_t drives[], int drive_count);
int get_drive_list(drive_info_t drives[], int max_drives);
void close_drives(drive_info_t drives[], int count);
void render_hex_view(WINDOW *win, drive_info_t *drive);
void render_help_view(WINDOW *win, int scroll_offset);
int get_dynamic_bytes_per_row(WINDOW *win);
off_t handle_goto_popup(drive_info_t *drive, int bytes_per_row);
off_t handle_random_search(drive_info_t *drive, int bytes_per_row);
off_t execute_search(drive_info_t *drive, int bytes_per_row, search_type_t search_type, search_pattern_element_t *hex_pattern, unsigned char *ascii_pattern, int pat_len, unsigned long long target_bytes, int is_full_scan, off_t start_offset, int is_find_next);
void handle_entropy_menu(drive_info_t *drive, int bytes_per_row);
void handle_entropy_check(drive_info_t *drive, int bytes_per_row);
void handle_single_block_entropy(drive_info_t *drive, int bytes_per_row);
off_t execute_entropy_outlier_search(drive_info_t *drive, int bytes_per_row, double threshold, unsigned long long target_bytes, int is_full_scan, off_t start_offset, int is_find_next);
off_t execute_phrase_text_search(drive_info_t *drive, int bytes_per_row, int min_word_size, unsigned long long target_bytes, int is_full_scan, off_t start_offset, int is_find_next);
void show_message_popup(const char *title, const char *msg);
int parse_hex_pattern(const char *str, search_pattern_element_t *pattern, int max_len);
int parse_ascii_pattern(const char *str, unsigned char *buf, int max_len, int case_insensitive);
int is_valid_word(const unsigned char *word_buf, int len);

int main(int argc, char *argv[]) {
    (void)argc;
    (void)argv;

    drive_info_t drives[64];
    int drive_count = 0;
    int selected_drive = 0;
    int scroll_offset = 0;
    int show_help = 0;
    int help_scroll_offset = 0;
    int ch;

    // Seed random generator for search
    srand((unsigned int)time(NULL));

    // Initialize ncurses & colors
    init_ncurses_ui();

    // Check for root privileges
    if (geteuid() != 0) {
        show_message_popup("Permission Denied", "Root privileges required! Please run with sudo.");
        endwin();
        fprintf(stderr, "Error: shredos_hexviewer must be run as root (sudo).\n");
        return 1;
    }

    // Initial drive discovery
    drive_count = get_drive_list(drives, 64);

    struct timespec last_hotplug_check;
    clock_gettime(CLOCK_MONOTONIC, &last_hotplug_check);

    // Layout configuration
    int height = LINES;
    int width = COLS;
    int title_height = 3;
    int help_height = 1; // Frameless bottom line
    int main_height = height - title_height - help_height;
    int left_width = 32;

    // Create windows
    WINDOW *win_title = newwin(title_height, width, 0, 0);
    WINDOW *win_left  = newwin(main_height, left_width, title_height, 0);
    WINDOW *win_right = newwin(main_height, width - left_width, title_height, left_width);
    WINDOW *win_help  = newwin(help_height, width, height - 1, 0);

    keypad(stdscr, TRUE);
    keypad(win_left, TRUE);
    keypad(win_right, TRUE);

    // Set 250ms non-blocking timeout for wgetch to enable responsive UI without CPU spinning
    wtimeout(win_left, 250);

    int running = 1;
    while (running) {
        // --- Throttled Hot-Plug Check (Runs every 1 second max, only if drives changed) ---
        struct timespec now_hp;
        clock_gettime(CLOCK_MONOTONIC, &now_hp);
        long hp_elapsed_ms = (now_hp.tv_sec - last_hotplug_check.tv_sec) * 1000 + (now_hp.tv_nsec - last_hotplug_check.tv_nsec) / 1000000;

        if (hp_elapsed_ms >= 1000) {
            last_hotplug_check = now_hp;
            if (check_drives_changed(drives, drive_count)) {
                char selected_name[64] = "";
                off_t saved_offset = 0;
                if (drive_count > 0 && selected_drive < drive_count) {
                    size_t name_len = strlen(drives[selected_drive].name);
                    if (name_len >= sizeof(selected_name)) name_len = sizeof(selected_name) - 1;
                    memcpy(selected_name, drives[selected_drive].name, name_len);
                    selected_name[name_len] = '\0';
                    saved_offset = drives[selected_drive].offset;
                }

                close_drives(drives, drive_count);
                drive_count = get_drive_list(drives, 64);

                int found_sel = 0;
                if (strlen(selected_name) > 0) {
                    for (int i = 0; i < drive_count; i++) {
                        if (strcmp(drives[i].name, selected_name) == 0) {
                            selected_drive = i;
                            drives[i].offset = saved_offset; // Restore position across auto-refresh
                            found_sel = 1;
                            break;
                        }
                    }
                }
                if (!found_sel) {
                    selected_drive = 0;
                    scroll_offset = 0;
                }

                // Ensure selected index is within bounds
                if (drive_count > 0 && selected_drive >= drive_count) {
                    selected_drive = drive_count - 1;
                }
                if (selected_drive < 0) {
                    selected_drive = 0;
                }
            }
        }

        // --- Render Top Title Box ---
        wclear(win_title);
        wattron(win_title, COLOR_PAIR(CP_BORDER));
        box(win_title, 0, 0);
        wattroff(win_title, COLOR_PAIR(CP_BORDER));

        wattron(win_title, COLOR_PAIR(CP_TITLE) | A_BOLD);
        mvwprintw(win_title, 1, 2, " shredos_hexviewer v1.1");
        wattroff(win_title, COLOR_PAIR(CP_TITLE) | A_BOLD);
        wrefresh(win_title);

        // --- Render Left Pane (Scrollable Drive List with Model / Serial) ---
        wclear(win_left);
        wattron(win_left, COLOR_PAIR(CP_BORDER));
        box(win_left, 0, 0);
        wattroff(win_left, COLOR_PAIR(CP_BORDER));

        wattron(win_left, COLOR_PAIR(CP_TITLE) | A_BOLD);
        mvwprintw(win_left, 0, 2, " Devices ");
        wattroff(win_left, COLOR_PAIR(CP_TITLE) | A_BOLD);

        // Calculate how many drive items (2 lines each) fit in the left pane
        int visible_slots = (main_height - 2) / 2;
        if (visible_slots < 1) visible_slots = 1;

        // Adjust scroll offset to keep selected drive in view
        if (selected_drive < scroll_offset) {
            scroll_offset = selected_drive;
        } else if (selected_drive >= scroll_offset + visible_slots) {
            scroll_offset = selected_drive - visible_slots + 1;
        }

        // Top "More" indicator if items exist above view
        if (scroll_offset > 0) {
            wattron(win_left, COLOR_PAIR(CP_TITLE));
            mvwprintw(win_left, 0, 11, "(More ^)");
            wattroff(win_left, COLOR_PAIR(CP_TITLE));
        }

        if (drive_count == 0) {
            mvwprintw(win_left, 2, 2, "No drives found.");
        } else {
            int current_row = 1;
            for (int i = scroll_offset; i < drive_count && (current_row + 1 < main_height - 1); i++) {
                if (i == selected_drive) {
                    wattron(win_left, A_REVERSE);
                }

                // Line 1: Device Name (e.g. sda)
                mvwprintw(win_left, current_row, 1, " %-29s", drives[i].name);
                current_row++;

                // Line 2: Model / Serial
                if (current_row < main_height - 1) {
                    mvwprintw(win_left, current_row, 1, " %-29s", drives[i].model_serial);
                    current_row++;
                }

                if (i == selected_drive) {
                    wattroff(win_left, A_REVERSE);
                }
            }

            // Bottom "More" indicator if items exist below view
            if (scroll_offset + visible_slots < drive_count) {
                wattron(win_left, COLOR_PAIR(CP_TITLE));
                mvwprintw(win_left, main_height - 1, left_width - 10, "(More v)");
                wattroff(win_left, COLOR_PAIR(CP_TITLE));
            }
        }
        wrefresh(win_left);

        // --- Render Right Pane (Help View or Dynamic Hex & ASCII View) ---
        if (show_help) {
            render_help_view(win_right, help_scroll_offset);
        } else if (drive_count > 0) {
            render_hex_view(win_right, &drives[selected_drive]);
        } else {
            wclear(win_right);
            wattron(win_right, COLOR_PAIR(CP_BORDER));
            box(win_right, 0, 0);
            wattroff(win_right, COLOR_PAIR(CP_BORDER));
            mvwprintw(win_right, 1, 2, "No drive selected.");
            wrefresh(win_right);
        }

        // --- Render Frameless Help Bar on Bottom Line ---
        wclear(win_help);
        if (show_help) {
            mvwprintw(win_help, 0, 0, "   Up/Down/PgUp/PgDn=Scroll Help | h or Esc=Return to Hex View");
        } else {
            mvwprintw(win_help, 0, 0, "   r=Reload | g=goto | s=search | e=entropy | h=Help | PgUp/PgDn=Scroll | q=Quit");
        }
        wrefresh(win_help);

        // --- Handle Input (Timed 250ms Poll) ---
        ch = wgetch(win_left);

        // If no key was pressed within 250ms, wgetch returns ERR -> loop continues for auto-refresh
        if (ch == ERR) {
            continue;
        }

        if (ch == KEY_RESIZE) {
            // Update terminal geometry
            resize_term(0, 0);

            height = LINES;
            width = COLS;
            main_height = height - title_height - help_height;

            // Recreate windows with new dimensions
            delwin(win_title);
            delwin(win_left);
            delwin(win_right);
            delwin(win_help);

            win_title = newwin(title_height, width, 0, 0);
            win_left  = newwin(main_height, left_width, title_height, 0);
            win_right = newwin(main_height, width - left_width, title_height, left_width);
            win_help  = newwin(help_height, width, height - 1, 0);

            keypad(win_left, TRUE);
            keypad(win_right, TRUE);
            wtimeout(win_left, 250);
            continue;
        }

        int right_rows;
        int right_cols;
        getmaxyx(win_right, right_rows, right_cols);
        (void)right_cols; // intentionally unused
        int rows_visible = right_rows - 2;
        if (rows_visible < 1) rows_visible = 1;
        int dynamic_bpr = get_dynamic_bytes_per_row(win_right);
        off_t page_bytes = rows_visible * dynamic_bpr;

        // --- Handle Help Navigation Input ---
        if (show_help) {
            switch (ch) {
                case KEY_UP:
                    if (help_scroll_offset > 0) help_scroll_offset--;
                    break;
                case KEY_DOWN:
                    help_scroll_offset++;
                    break;
                case KEY_PPAGE:
                    help_scroll_offset -= rows_visible;
                    if (help_scroll_offset < 0) help_scroll_offset = 0;
                    break;
                case KEY_NPAGE:
                    help_scroll_offset += rows_visible;
                    break;
                case 'h':
                case 'H':
                case 27: // ESC
                    show_help = 0;
                    break;
            }
            continue;
        }

        switch (ch) {
            case KEY_DOWN:
                if (drive_count > 0 && selected_drive < drive_count - 1) {
                    selected_drive++;
                }
                break;

            case KEY_UP:
                if (drive_count > 0 && selected_drive > 0) {
                    selected_drive--;
                }
                break;

            case KEY_NPAGE: // Page Down
                if (drive_count > 0 && drives[selected_drive].offset + page_bytes < drives[selected_drive].size) {
                    drives[selected_drive].offset += page_bytes;
                }
                break;

            case KEY_PPAGE: // Page Up
                if (drive_count > 0 && drives[selected_drive].offset >= page_bytes) {
                    drives[selected_drive].offset -= page_bytes;
                } else if (drive_count > 0) {
                    drives[selected_drive].offset = 0;
                }
                break;

            case '<': // Start of drive
                if (drive_count > 0) {
                    drives[selected_drive].offset = 0;
                }
                break;

            case '>': // End of drive
                if (drive_count > 0) {
                    if (drives[selected_drive].size > page_bytes) {
                        drives[selected_drive].offset = drives[selected_drive].size - page_bytes;
                    } else {
                        drives[selected_drive].offset = 0;
                    }
                }
                break;

            case 'g':
            case 'G':
                if (drive_count > 0) {
                    drives[selected_drive].offset = handle_goto_popup(&drives[selected_drive], dynamic_bpr);
                }
                break;

            case 's':
            case 'S':
                if (drive_count > 0) {
                    drives[selected_drive].offset = handle_random_search(&drives[selected_drive], dynamic_bpr);
                }
                break;

            case 'e':
            case 'E':
                if (drive_count > 0) {
                    handle_entropy_menu(&drives[selected_drive], dynamic_bpr);
                }
                break;

            case 'h':
            case 'H':
                show_help = 1;
                help_scroll_offset = 0;
                break;

            case 'r':
            case 'R':
                // Manual reload forced
            {
                char selected_name[64] = "";
                off_t saved_offset = 0;
                if (drive_count > 0 && selected_drive < drive_count) {
                    size_t name_len = strlen(drives[selected_drive].name);
                    if (name_len >= sizeof(selected_name)) name_len = sizeof(selected_name) - 1;
                    memcpy(selected_name, drives[selected_drive].name, name_len);
                    selected_name[name_len] = '\0';
                    saved_offset = drives[selected_drive].offset;
                }

                close_drives(drives, drive_count);
                drive_count = get_drive_list(drives, 64);

                int found_sel = 0;
                if (strlen(selected_name) > 0) {
                    for (int i = 0; i < drive_count; i++) {
                        if (strcmp(drives[i].name, selected_name) == 0) {
                            selected_drive = i;
                            drives[i].offset = saved_offset;
                            found_sel = 1;
                            break;
                        }
                    }
                }
                if (!found_sel) {
                    selected_drive = 0;
                    scroll_offset = 0;
                }
                if (drive_count > 0 && selected_drive >= drive_count) {
                    selected_drive = drive_count - 1;
                }
            }
            break;

            case 'q':
            case 'Q':
                running = 0;
                break;
        }
    }

    // Cleanup
    close_drives(drives, drive_count);
    delwin(win_title);
    delwin(win_left);
    delwin(win_right);
    delwin(win_help);
    endwin();

    return 0;
}

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

void get_model_and_serial(const char *dev_name, char *dest, size_t max_len) {
    char model[64] = "UnknownModel";
    char serial[64] = "UnknownSerial";
    char path[MAX_PATH];
    FILE *f;

    // Retrieve Model from sysfs
    snprintf(path, sizeof(path), "/sys/block/%s/device/model", dev_name);
    f = fopen(path, "r");
    if (f) {
        if (fgets(model, sizeof(model), f)) {
            model[strcspn(model, "\r\n")] = 0;
        }
        fclose(f);
    } else {
        snprintf(path, sizeof(path), "/sys/block/%s/device/../model", dev_name);
        f = fopen(path, "r");
        if (f) {
            if (fgets(model, sizeof(model), f)) {
                model[strcspn(model, "\r\n")] = 0;
            }
            fclose(f);
        }
    }

    // Retrieve Serial from sysfs
    snprintf(path, sizeof(path), "/sys/block/%s/device/serial", dev_name);
    f = fopen(path, "r");
    if (f) {
        if (fgets(serial, sizeof(serial), f)) {
            serial[strcspn(serial, "\r\n")] = 0;
        }
        fclose(f);
    } else {
        snprintf(path, sizeof(path), "/sys/block/%s/serial", dev_name);
        f = fopen(path, "r");
        if (f) {
            if (fgets(serial, sizeof(serial), f)) {
                serial[strcspn(serial, "\r\n")] = 0;
            }
            fclose(f);
        }
    }

    // Trim trailing whitespace
    for (int i = strlen(model) - 1; i >= 0 && model[i] == ' '; i--) model[i] = '\0';
    for (int i = strlen(serial) - 1; i >= 0 && serial[i] == ' '; i--) serial[i] = '\0';

    snprintf(dest, max_len, "%s / %s", model, serial);
}

void detect_drive_signature(drive_info_t *drive) {
    if (drive->fd < 0) {
        snprintf(drive->signature, sizeof(drive->signature), "Unknown");
        return;
    }

    unsigned char sector_buf[4096];
    memset(sector_buf, 0, sizeof(sector_buf));

    lseek(drive->fd, 0, SEEK_SET);
    ssize_t n = read(drive->fd, sector_buf, sizeof(sector_buf));
    if (n < 512) {
        snprintf(drive->signature, sizeof(drive->signature), "Raw");
        return;
    }

    // 1. Check GPT signature ("EFI PART" at LBA 1 / offset 512)
    if (n >= 1024 && memcmp(sector_buf + 512, "EFI PART", 8) == 0) {
        snprintf(drive->signature, sizeof(drive->signature), "GPT");
        return;
    }

    // 2. Check MBR boot signature (0x55AA at offset 510-511)
    int has_mbr = (sector_buf[510] == 0x55 && sector_buf[511] == 0xAA);

    // 3. Check Filesystem signatures
    if (memcmp(sector_buf + 3, "NTFS    ", 8) == 0) {
        snprintf(drive->signature, sizeof(drive->signature), has_mbr ? "MBR + NTFS" : "NTFS");
        return;
    }
    if (n >= 1082 && sector_buf[1080] == 0x53 && sector_buf[1081] == 0xEF) {
        snprintf(drive->signature, sizeof(drive->signature), has_mbr ? "MBR + ext2/3/4" : "ext2/3/4");
        return;
    }
    if (memcmp(sector_buf + 0x52, "FAT32   ", 8) == 0 || memcmp(sector_buf + 0x26, "FAT", 3) == 0) {
        snprintf(drive->signature, sizeof(drive->signature), has_mbr ? "MBR + FAT" : "FAT");
        return;
    }

    // 4. Check ISO9660 (CD001 signature at offset 32768) using a targeted read
    unsigned char iso_buf[5];
    if (lseek(drive->fd, 32768, SEEK_SET) == 32768 && read(drive->fd, iso_buf, 5) == 5) {
        if (memcmp(iso_buf, "CD001", 5) == 0) {
            snprintf(drive->signature, sizeof(drive->signature), "ISO9660");
            return;
        }
    }

    if (has_mbr) {
        snprintf(drive->signature, sizeof(drive->signature), "MBR");
        return;
    }

    snprintf(drive->signature, sizeof(drive->signature), "Raw / Unknown");
}

int check_drives_changed(drive_info_t drives[], int drive_count) {
    DIR *dir = opendir("/sys/block");
    if (!dir) return 0;

    char sys_names[64][64];
    int sys_count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && sys_count < 64) {
        if (entry->d_name[0] == '.') continue;
        if (strncmp(entry->d_name, "loop", 4) == 0) continue;
        if (strncmp(entry->d_name, "ram", 3) == 0) continue;
        if (strncmp(entry->d_name, "sr", 2) == 0) continue;

        char path[MAX_PATH];
        snprintf(path, sizeof(path), "/dev/%s", entry->d_name);
        struct stat st;
        if (stat(path, &st) < 0) continue;

        size_t len = strlen(entry->d_name);
        if (len >= sizeof(sys_names[sys_count])) len = sizeof(sys_names[sys_count]) - 1;
        memcpy(sys_names[sys_count], entry->d_name, len);
        sys_names[sys_count][len] = '\0';
        sys_count++;
    }
    closedir(dir);

    if (sys_count != drive_count) return 1;

    for (int i = 0; i < drive_count; i++) {
        int found = 0;
        for (int j = 0; j < sys_count; j++) {
            if (strcmp(drives[i].name, sys_names[j]) == 0) {
                found = 1;
                break;
            }
        }
        if (!found) return 1;
    }
    return 0;
}

int get_drive_list(drive_info_t drives[], int max_drives) {
    DIR *dir = opendir("/sys/block");
    if (!dir) return 0;

    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && count < max_drives) {
        if (entry->d_name[0] == '.') continue;

        // Filter out loops, ram disks, and optical drives
        if (strncmp(entry->d_name, "loop", 4) == 0) continue;
        if (strncmp(entry->d_name, "ram", 3) == 0) continue;
        if (strncmp(entry->d_name, "sr", 2) == 0) continue;

        char path[MAX_PATH];
        snprintf(path, sizeof(path), "/dev/%s", entry->d_name);

        int fd = open(path, O_RDONLY);
        if (fd < 0) continue; // Skip if currently unreadable/locked

        // Safe name copy without format-truncation warnings
        size_t name_len = strlen(entry->d_name);
        if (name_len >= sizeof(drives[count].name)) {
            name_len = sizeof(drives[count].name) - 1;
        }
        memcpy(drives[count].name, entry->d_name, name_len);
        drives[count].name[name_len] = '\0';

        // Safe path copy
        size_t path_len = strlen(path);
        if (path_len >= sizeof(drives[count].path)) {
            path_len = sizeof(drives[count].path) - 1;
        }
        memcpy(drives[count].path, path, path_len);
        drives[count].path[path_len] = '\0';

        drives[count].offset = 0;
        drives[count].fd = fd;
        drives[count].last_search_valid = 0;

        // Fetch Model / Serial string
        get_model_and_serial(entry->d_name, drives[count].model_serial, sizeof(drives[count].model_serial));

        // Auto-detect Partition Table / Filesystem signature
        detect_drive_signature(&drives[count]);

        if (ioctl(fd, BLKGETSIZE64, &drives[count].size) < 0) {
            drives[count].size = lseek(fd, 0, SEEK_END);
        }
        count++;
    }
    closedir(dir);
    return count;
}

int get_dynamic_bytes_per_row(WINDOW *win) {
    int rows;
    int cols;
    getmaxyx(win, rows, cols);
    (void)rows; // intentionally unused
    int inner_width = cols - 4; // account for borders/margins
    if (inner_width < 32) inner_width = 32;

    int bpr = (inner_width - 16) / 4;
    if (bpr < 4) bpr = 4;
    if (bpr > 128) bpr = 128;
    return bpr;
}

void show_message_popup(const char *title, const char *msg) {
    int h = 9;
    int w = 66;
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

off_t handle_goto_popup(drive_info_t *drive, int bytes_per_row) {
    (void)bytes_per_row;
    int p_height = 10;
    int p_width = 56;
    int starty = (LINES - p_height) / 2;
    int startx = (COLS - p_width) / 2;

    WINDOW *pop = newwin(p_height, p_width, starty, startx);
    keypad(pop, TRUE);

    box(pop, 0, 0);
    wattron(pop, COLOR_PAIR(CP_TITLE) | A_BOLD);
    mvwprintw(pop, 0, 2, " Go To ");
    wattroff(pop, COLOR_PAIR(CP_TITLE) | A_BOLD);

    mvwprintw(pop, 2, 3, "Select Goto Method:");
    mvwprintw(pop, 4, 5, "[P] Percentage of disk (0-100%%)");
    mvwprintw(pop, 5, 5, "[B] Offset bytes (Decimal / Hex)");
    mvwprintw(pop, 6, 5, "[L] Block number (LBA / 512-byte)");
    mvwprintw(pop, 7, 5, "[Esc] Cancel");
    wrefresh(pop);

    int ch = wgetch(pop);
    off_t new_offset = drive->offset;

    if (ch == 'p' || ch == 'P') {
        wclear(pop);
        box(pop, 0, 0);
        wattron(pop, COLOR_PAIR(CP_TITLE) | A_BOLD);
        mvwprintw(pop, 0, 2, " Go To: Percentage ");
        wattroff(pop, COLOR_PAIR(CP_TITLE) | A_BOLD);

        char input_buf[32] = {0};
        mvwprintw(pop, 3, 3, "Enter percentage (0-100): ");
        wrefresh(pop);

        echo();
        curs_set(1);
        wgetnstr(pop, input_buf, 31);
        noecho();
        curs_set(0);

        double percent = atof(input_buf);
        if (percent < 0.0) percent = 0.0;
        if (percent > 100.0) percent = 100.0;
        new_offset = (off_t)(((double)drive->size * percent) / 100.0);
    }
    else if (ch == 'b' || ch == 'B') {
        wclear(pop);
        box(pop, 0, 0);
        wattron(pop, COLOR_PAIR(CP_TITLE) | A_BOLD);
        mvwprintw(pop, 0, 2, " Go To: Byte Offset ");
        wattroff(pop, COLOR_PAIR(CP_TITLE) | A_BOLD);

        char input_buf[64] = {0};
        mvwprintw(pop, 3, 3, "Enter offset (e.g. 0x1000 or 4096): ");
        wrefresh(pop);

        echo();
        curs_set(1);
        wgetnstr(pop, input_buf, 63);
        noecho();
        curs_set(0);

        unsigned long long val = 0;
        if (strncmp(input_buf, "0x", 2) == 0 || strncmp(input_buf, "0X", 2) == 0) {
            sscanf(input_buf, "%llx", &val);
        } else {
            sscanf(input_buf, "%llu", &val);
        }
        new_offset = (off_t)val;
    }
    else if (ch == 'l' || ch == 'L') {
        wclear(pop);
        box(pop, 0, 0);
        wattron(pop, COLOR_PAIR(CP_TITLE) | A_BOLD);
        mvwprintw(pop, 0, 2, " Go To: Block Number (LBA) ");
        wattroff(pop, COLOR_PAIR(CP_TITLE) | A_BOLD);

        char input_buf[64] = {0};
        mvwprintw(pop, 3, 3, "Enter 512-byte block / LBA number: ");
        wrefresh(pop);

        echo();
        curs_set(1);
        wgetnstr(pop, input_buf, 63);
        noecho();
        curs_set(0);

        unsigned long long block_num = 0;
        if (strncmp(input_buf, "0x", 2) == 0 || strncmp(input_buf, "0X", 2) == 0) {
            sscanf(input_buf, "%llx", &block_num);
        } else {
            sscanf(input_buf, "%llu", &block_num);
        }
        new_offset = (off_t)(block_num * 512);
    }

    delwin(pop);
    touchwin(stdscr);

    if (new_offset > drive->size) {
        new_offset = drive->size;
    }
    if (new_offset < 0) {
        new_offset = 0;
    }

    return new_offset;
}

int parse_hex_pattern(const char *str, search_pattern_element_t *pattern, int max_len) {
    int len = 0;
    const char *p = str;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
        p += 2;
    }
    while (*p && len < max_len) {
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (!*p) break;

        if (p[0] == '?' && p[1] == '?') {
            pattern[len].value = 0x00;
            pattern[len].is_wildcard = 1;
            len++;
            p += 2;
            continue;
        }

        char hex_byte[3] = {0};
        if (p[0] && p[1] && isxdigit(p[0]) && isxdigit(p[1])) {
            hex_byte[0] = p[0];
            hex_byte[1] = p[1];
            pattern[len].value = (unsigned char)strtol(hex_byte, NULL, 16);
            pattern[len].is_wildcard = 0;
            len++;
            p += 2;
        } else {
            break;
        }
    }
    return len;
}

int parse_ascii_pattern(const char *str, unsigned char *buf, int max_len, int case_insensitive) {
    size_t len = strlen(str);
    if ((int)len > max_len) len = max_len;
    for (size_t i = 0; i < len; i++) {
        buf[i] = case_insensitive ? (unsigned char)tolower((unsigned char)str[i]) : (unsigned char)str[i];
    }
    return (int)len;
}

off_t handle_random_search(drive_info_t *drive, int bytes_per_row) {
    int m_height = 13;
    int m_width = 56;
    int starty = (LINES - m_height) / 2;
    int startx = (COLS - m_width) / 2;

    WINDOW *menu_win = newwin(m_height, m_width, starty, startx);
    keypad(menu_win, TRUE);

    box(menu_win, 0, 0);
    wattron(menu_win, COLOR_PAIR(CP_TITLE) | A_BOLD);
    mvwprintw(menu_win, 0, 2, " Search Category ");
    wattroff(menu_win, COLOR_PAIR(CP_TITLE) | A_BOLD);

    mvwprintw(menu_win, 2, 3, "Select Search Type:");
    mvwprintw(menu_win, 4, 5, "[1] Non-Zero Search (!= 0x00)");
    mvwprintw(menu_win, 5, 5, "[2] Non-0xFF Search (!= 0xFF)");
    mvwprintw(menu_win, 6, 5, "[3] Hex Pattern Search (Supports ?\\?\\?)");
    mvwprintw(menu_win, 7, 5, "[4] ASCII Pattern Search (case sensitive)");
    mvwprintw(menu_win, 8, 5, "[5] ASCII Pattern Search (case insensitive)");
    mvwprintw(menu_win, 10, 3, "Press 1-5, or Esc to cancel");
    wrefresh(menu_win);

    int c_ch = wgetch(menu_win);
    delwin(menu_win);
    touchwin(stdscr);

    if (c_ch < '1' || c_ch > '5') {
        return drive->offset;
    }

    search_type_t search_type = (search_type_t)(c_ch - '0');
    search_pattern_element_t hex_pattern[64] = { {0, 0} };
    unsigned char ascii_pattern[64] = {0};
    int pat_len = 0;

    if (search_type == SEARCH_HEX_PATTERN || search_type == SEARCH_ASCII_PATTERN || search_type == SEARCH_ASCII_PATTERN_CI) {
        int p_height = 8;
        int p_width = 62;
        starty = (LINES - p_height) / 2;
        startx = (COLS - p_width) / 2;

        WINDOW *pat_win = newwin(p_height, p_width, starty, startx);
        box(pat_win, 0, 0);
        wattron(pat_win, COLOR_PAIR(CP_TITLE) | A_BOLD);
        if (search_type == SEARCH_HEX_PATTERN) {
            mvwprintw(pat_win, 0, 2, " Hex Pattern Search (?? Wildcard) ");
            mvwprintw(pat_win, 2, 3, "Enter hex bytes/wildcards (e.g. 4D 5A ?? 00):");
        } else if (search_type == SEARCH_ASCII_PATTERN_CI) {
            mvwprintw(pat_win, 0, 2, " ASCII Search (Case Insensitive) ");
            mvwprintw(pat_win, 2, 3, "Enter ASCII search string:");
        } else {
            mvwprintw(pat_win, 0, 2, " ASCII Pattern Search (Case Sensitive) ");
            mvwprintw(pat_win, 2, 3, "Enter ASCII search string:");
        }
        wattroff(pat_win, COLOR_PAIR(CP_TITLE) | A_BOLD);

        mvwprintw(pat_win, 5, 3, "Pattern: ");
        wrefresh(pat_win);

        char pat_buf[64] = {0};
        echo();
        curs_set(1);
        wgetnstr(pat_win, pat_buf, 63);
        noecho();
        curs_set(0);
        delwin(pat_win);
        touchwin(stdscr);

        if (search_type == SEARCH_HEX_PATTERN) {
            pat_len = parse_hex_pattern(pat_buf, hex_pattern, 64);
        } else if (search_type == SEARCH_ASCII_PATTERN_CI) {
            pat_len = parse_ascii_pattern(pat_buf, ascii_pattern, sizeof(ascii_pattern), 1);
        } else {
            pat_len = parse_ascii_pattern(pat_buf, ascii_pattern, sizeof(ascii_pattern), 0);
        }

        if (pat_len <= 0) {
            show_message_popup("Error", "Invalid or empty search pattern entered.");
            return drive->offset;
        }
    }

    int p_height = 10;
    int p_width = 66;
    starty = (LINES - p_height) / 2;
    startx = (COLS - p_width) / 2;

    WINDOW *pop = newwin(p_height, p_width, starty, startx);
    keypad(pop, TRUE);

    box(pop, 0, 0);
    wattron(pop, COLOR_PAIR(CP_TITLE) | A_BOLD);
    mvwprintw(pop, 0, 2, " Search Scope ");
    wattroff(pop, COLOR_PAIR(CP_TITLE) | A_BOLD);

    mvwprintw(pop, 2, 3, "Enter scope (e.g. 1%%, 95%%, 8GB, 1TB, 100%%):");
    mvwprintw(pop, 3, 3, "(100%% = Full Sequential Scan; <100%% = 1000-Chunk Scatter Scan)");
    mvwprintw(pop, 6, 3, "Scope: ");
    wrefresh(pop);

    char input_buf[64] = {0};
    echo();
    curs_set(1);
    wgetnstr(pop, input_buf, 63);
    noecho();
    curs_set(0);
    delwin(pop);
    touchwin(stdscr);

    size_t len = strlen(input_buf);
    if (len == 0) return drive->offset;

    while (len > 0 && (input_buf[len - 1] == ' ' || input_buf[len - 1] == '\r' || input_buf[len - 1] == '\n')) {
        input_buf[--len] = '\0';
    }
    if (len == 0) return drive->offset;

    char unit = '\0';
    char last_char = input_buf[len - 1];

    if (last_char == '%') {
        unit = '%';
        input_buf[--len] = '\0';
    } else if (last_char == 'B' || last_char == 'b') {
        input_buf[--len] = '\0';
        if (len > 0) {
            char prev_char = input_buf[len - 1];
            if (prev_char == 'T' || prev_char == 't') { unit = 'T'; input_buf[--len] = '\0'; }
            else if (prev_char == 'G' || prev_char == 'g') { unit = 'G'; input_buf[--len] = '\0'; }
            else if (prev_char == 'M' || prev_char == 'm') { unit = 'M'; input_buf[--len] = '\0'; }
            else if (prev_char == 'K' || prev_char == 'k') { unit = 'K'; input_buf[--len] = '\0'; }
        }
    } else if (last_char == 'T' || last_char == 't') {
        unit = 'T'; input_buf[--len] = '\0';
    } else if (last_char == 'G' || last_char == 'g') {
        unit = 'G'; input_buf[--len] = '\0';
    } else if (last_char == 'M' || last_char == 'm') {
        unit = 'M'; input_buf[--len] = '\0';
    } else if (last_char == 'K' || last_char == 'k') {
        unit = 'K'; input_buf[--len] = '\0';
    }

    double numeric_val = atof(input_buf);
    unsigned long long target_bytes = 0;

    if (unit == '\0' && numeric_val <= 100.0) {
        unit = '%';
    }

    if (unit == '%') {
        if (numeric_val < 0) numeric_val = 0;
        if (numeric_val > 100) numeric_val = 100;
        target_bytes = (unsigned long long)((double)drive->size * (numeric_val / 100.0));
    } else if (unit == 'T') {
        target_bytes = (unsigned long long)(numeric_val * 1024.0 * 1024.0 * 1024.0 * 1024.0);
    } else if (unit == 'G') {
        target_bytes = (unsigned long long)(numeric_val * 1024.0 * 1024.0 * 1024.0);
    } else if (unit == 'M') {
        target_bytes = (unsigned long long)(numeric_val * 1024.0 * 1024.0);
    } else if (unit == 'K') {
        target_bytes = (unsigned long long)(numeric_val * 1024.0);
    } else {
        target_bytes = (unsigned long long)numeric_val;
    }

    if (target_bytes == 0) target_bytes = (unsigned long long)bytes_per_row * 100;
    if (target_bytes > (unsigned long long)drive->size) {
        target_bytes = (unsigned long long)drive->size;
    }

    int is_full_scan = (unit == '%' && numeric_val >= 99.99) || (target_bytes >= (unsigned long long)drive->size);

    return execute_search(drive, bytes_per_row, search_type, hex_pattern, ascii_pattern, pat_len, target_bytes, is_full_scan, 0, 0);
}

off_t execute_search(drive_info_t *drive, int bytes_per_row, search_type_t search_type, search_pattern_element_t *hex_pattern, unsigned char *ascii_pattern, int pat_len, unsigned long long target_bytes, int is_full_scan, off_t start_offset, int is_find_next) {
    double target_mb = (double)target_bytes / (1024.0 * 1024.0);
    if (target_mb < 0.1) target_mb = 0.1;

    size_t sector_size = 512;
    int found = 0;
    int cancelled = 0;
    off_t found_offset = start_offset;
    unsigned char *sector_buf = malloc(sector_size);
    if (!sector_buf) {
        show_message_popup("Error", "Memory allocation failed for search buffer.");
        return drive->offset;
    }

    int prog_h = 8;
    int prog_w = 64;
    int prog_y = (LINES - prog_h) / 2;
    int prog_x = (COLS - prog_w) / 2;
    WINDOW *prog_win = newwin(prog_h, prog_w, prog_y, prog_x);
    keypad(prog_win, TRUE);
    wtimeout(prog_win, 0);

    box(prog_win, 0, 0);
    wattron(prog_win, COLOR_PAIR(CP_TITLE) | A_BOLD);
    mvwprintw(prog_win, 0, 2, is_full_scan ? " Full Sequential Search " : " 1000-Chunk Scatter Search ");
    wattroff(prog_win, COLOR_PAIR(CP_TITLE) | A_BOLD);
    mvwprintw(prog_win, 2, 3, "Progress:   0%% complete");
    mvwprintw(prog_win, 3, 3, "Checked:    0.0MB / %.1fMB", target_mb);
    mvwprintw(prog_win, 5, 3, "Press ESC to cancel search");
    wrefresh(prog_win);

    struct timespec last_check;
    clock_gettime(CLOCK_MONOTONIC, &last_check);

    if (is_full_scan) {
        unsigned long long total_sectors = (target_bytes + sector_size - 1) / sector_size;
        if (total_sectors == 0) total_sectors = 1;

        unsigned char history[64] = {0};
        int history_len = 0;
        unsigned long long total_bytes_read = 0;

        for (unsigned long long i = 0; i < total_sectors && !found && !cancelled; i++) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            long elapsed_ms = (now.tv_sec - last_check.tv_sec) * 1000 + (now.tv_nsec - last_check.tv_nsec) / 1000000;
            if (elapsed_ms >= 250) {
                last_check = now;
                int ch = wgetch(prog_win);
                if (ch == 27 || ch == 'q' || ch == 'Q') {
                    cancelled = 1;
                    break;
                }
            }

            off_t check_offset = (off_t)(i * sector_size);
            if (check_offset >= drive->size) break;

            size_t to_read = sector_size;
            if (check_offset + (off_t)to_read > drive->size) {
                to_read = (size_t)(drive->size - check_offset);
            }
            if (to_read == 0) break;

            lseek(drive->fd, check_offset, SEEK_SET);
            ssize_t n = read(drive->fd, sector_buf, to_read);
            if (n <= 0) break;

            unsigned char search_buf[512 + 64];
            memcpy(search_buf, history, history_len);
            memcpy(search_buf + history_len, sector_buf, n);
            int total_len = history_len + (int)n;

            for (int idx = 0; idx < total_len; idx++) {
                off_t current_byte_offset = check_offset - history_len + idx;
                if (current_byte_offset < 0) continue;
                if (current_byte_offset >= drive->size) break;

                if (is_find_next && current_byte_offset <= drive->last_found_offset) {
                    continue;
                }

                unsigned char b = search_buf[idx];
                int matched = 0;

                if (search_type == SEARCH_NON_ZERO) {
                    if (b != 0x00) { matched = 1; found_offset = current_byte_offset; }
                } else if (search_type == SEARCH_NON_FF) {
                    if (b != 0xFF) { matched = 1; found_offset = current_byte_offset; }
                } else if (search_type == SEARCH_HEX_PATTERN) {
                    if (idx + pat_len <= total_len) {
                        int p_match = 1;
                        for (int p = 0; p < pat_len; p++) {
                            if (!hex_pattern[p].is_wildcard) {
                                if (search_buf[idx + p] != hex_pattern[p].value) {
                                    p_match = 0;
                                    break;
                                }
                            }
                        }
                        if (p_match) {
                            matched = 1;
                            found_offset = current_byte_offset;
                        }
                    }
                } else if (search_type == SEARCH_ASCII_PATTERN || search_type == SEARCH_ASCII_PATTERN_CI) {
                    if (idx + pat_len <= total_len) {
                        int p_match = 1;
                        for (int p = 0; p < pat_len; p++) {
                            unsigned char disk_b = search_buf[idx + p];
                            if (search_type == SEARCH_ASCII_PATTERN_CI) {
                                disk_b = (unsigned char)tolower(disk_b);
                            }
                            if (disk_b != ascii_pattern[p]) {
                                p_match = 0;
                                break;
                            }
                        }
                        if (p_match) {
                            matched = 1;
                            found_offset = current_byte_offset;
                        }
                    }
                }

                if (matched) {
                    found = 1;
                    break;
                }
            }

            if (search_type == SEARCH_HEX_PATTERN || search_type == SEARCH_ASCII_PATTERN || search_type == SEARCH_ASCII_PATTERN_CI) {
                int keep = pat_len - 1;
                if (keep > n) keep = n;
                if (keep > 0) {
                    memcpy(history, sector_buf + n - keep, keep);
                    history_len = keep;
                } else {
                    history_len = 0;
                }
            }

            total_bytes_read += n;
            int percent = (int)((total_bytes_read * 100) / target_bytes);
            if (percent > 100) percent = 100;
            double checked_mb = (double)total_bytes_read / (1024.0 * 1024.0);

            mvwprintw(prog_win, 2, 13, "%3d%% complete", percent);
            mvwprintw(prog_win, 3, 13, "%.1fMB / %.1fMB", checked_mb, target_mb);
            wrefresh(prog_win);
        }
    } else {
        int num_chunks = 1000;
        unsigned long long total_sectors = (target_bytes + sector_size - 1) / sector_size;
        if (total_sectors < (unsigned long long)num_chunks) {
            num_chunks = (int)total_sectors;
            if (num_chunks < 1) num_chunks = 1;
        }

        typedef struct {
            unsigned long long start_sec;
            unsigned long long end_sec;
        } chunk_range_t;

        chunk_range_t *chunks = malloc(num_chunks * sizeof(chunk_range_t));
        if (!chunks) {
            free(sector_buf);
            delwin(prog_win);
            touchwin(stdscr);
            show_message_popup("Error", "Memory allocation failed for chunks.");
            return drive->offset;
        }

        for (int i = 0; i < num_chunks; i++) {
            chunks[i].start_sec = ((unsigned long long)i * total_sectors) / num_chunks;
            chunks[i].end_sec = ((unsigned long long)(i + 1) * total_sectors) / num_chunks;
        }

        int *indices = malloc(num_chunks * sizeof(int));
        if (!indices) {
            free(chunks);
            free(sector_buf);
            delwin(prog_win);
            touchwin(stdscr);
            show_message_popup("Error", "Memory allocation failed for chunk indices.");
            return drive->offset;
        }
        for (int i = 0; i < num_chunks; i++) {
            indices[i] = i;
        }

        for (int i = num_chunks - 1; i > 0; i--) {
            unsigned long long r1 = ((unsigned long long)rand() << 30) ^ rand();
            int j = (int)(r1 % (i + 1));
            int temp = indices[i];
            indices[i] = indices[j];
            indices[j] = temp;
        }

        unsigned long long total_bytes_read = 0;

        for (int idx = 0; idx < num_chunks && !found && !cancelled; idx++) {
            int c_idx = indices[idx];
            unsigned long long start_sec = chunks[c_idx].start_sec;
            unsigned long long end_sec = chunks[c_idx].end_sec;

            unsigned char history[64] = {0};
            int history_len = 0;

            for (unsigned long long sec = start_sec; sec < end_sec && !found && !cancelled; sec++) {
                if (total_bytes_read >= target_bytes) break;

                struct timespec now;
                clock_gettime(CLOCK_MONOTONIC, &now);
                long elapsed_ms = (now.tv_sec - last_check.tv_sec) * 1000 + (now.tv_nsec - last_check.tv_nsec) / 1000000;
                if (elapsed_ms >= 250) {
                    last_check = now;
                    int ch = wgetch(prog_win);
                    if (ch == 27 || ch == 'q' || ch == 'Q') {
                        cancelled = 1;
                        break;
                    }
                }

                off_t check_offset = (off_t)(sec * sector_size);
                if (check_offset >= drive->size) break;

                size_t to_read = sector_size;
                if (check_offset + (off_t)to_read > drive->size) {
                    to_read = (size_t)(drive->size - check_offset);
                }
                if (to_read == 0) break;

                lseek(drive->fd, check_offset, SEEK_SET);
                ssize_t n = read(drive->fd, sector_buf, to_read);
                if (n <= 0) break;

                unsigned char search_buf[512 + 64];
                memcpy(search_buf, history, history_len);
                memcpy(search_buf + history_len, sector_buf, n);
                int total_len = history_len + (int)n;

                for (int s_idx = 0; s_idx < total_len; s_idx++) {
                    off_t current_byte_offset = check_offset - history_len + s_idx;
                    if (current_byte_offset < 0) continue;
                    if (current_byte_offset >= drive->size) break;

                    if (is_find_next && current_byte_offset <= drive->last_found_offset) {
                        continue;
                    }

                    unsigned char b = search_buf[s_idx];
                    int matched = 0;

                    if (search_type == SEARCH_NON_ZERO) {
                        if (b != 0x00) { matched = 1; found_offset = current_byte_offset; }
                    } else if (search_type == SEARCH_NON_FF) {
                        if (b != 0xFF) { matched = 1; found_offset = current_byte_offset; }
                    } else if (search_type == SEARCH_HEX_PATTERN) {
                        if (s_idx + pat_len <= total_len) {
                            int p_match = 1;
                            for (int p = 0; p < pat_len; p++) {
                                if (!hex_pattern[p].is_wildcard) {
                                    if (search_buf[s_idx + p] != hex_pattern[p].value) {
                                        p_match = 0;
                                        break;
                                    }
                                }
                            }
                            if (p_match) {
                                matched = 1;
                                found_offset = current_byte_offset;
                            }
                        }
                    } else if (search_type == SEARCH_ASCII_PATTERN || search_type == SEARCH_ASCII_PATTERN_CI) {
                        if (s_idx + pat_len <= total_len) {
                            int p_match = 1;
                            for (int p = 0; p < pat_len; p++) {
                                unsigned char disk_b = search_buf[s_idx + p];
                                if (search_type == SEARCH_ASCII_PATTERN_CI) {
                                    disk_b = (unsigned char)tolower(disk_b);
                                }
                                if (disk_b != ascii_pattern[p]) {
                                    p_match = 0;
                                    break;
                                }
                            }
                            if (p_match) {
                                matched = 1;
                                found_offset = current_byte_offset;
                            }
                        }
                    }

                    if (matched) {
                        found = 1;
                        break;
                    }
                }

                if (search_type == SEARCH_HEX_PATTERN || search_type == SEARCH_ASCII_PATTERN || search_type == SEARCH_ASCII_PATTERN_CI) {
                    int keep = pat_len - 1;
                    if (keep > n) keep = n;
                    if (keep > 0) {
                        memcpy(history, sector_buf + n - keep, keep);
                        history_len = keep;
                    } else {
                        history_len = 0;
                    }
                }

                total_bytes_read += n;
                int percent = (int)((total_bytes_read * 100) / target_bytes);
                if (percent > 100) percent = 100;
                double checked_mb = (double)total_bytes_read / (1024.0 * 1024.0);

                mvwprintw(prog_win, 2, 13, "%3d%% complete", percent);
                mvwprintw(prog_win, 3, 13, "%.1fMB / %.1fMB", checked_mb, target_mb);
                wrefresh(prog_win);
            }

            if (total_bytes_read >= target_bytes) break;
        }

        free(indices);
        free(chunks);
    }

    free(sector_buf);
    delwin(prog_win);
    touchwin(stdscr);

    if (cancelled) {
        show_message_popup("Cancelled", "Search cancelled by user.");
        return drive->offset;
    }

    if (found) {
        drive->last_search_valid = 1;
        drive->last_search_type = search_type;
        drive->last_pat_len = pat_len;
        drive->last_found_offset = found_offset;
        drive->last_target_bytes = target_bytes;
        drive->last_is_full_scan = is_full_scan;
        for (int i = 0; i < pat_len; i++) {
            drive->last_hex_pat[i] = hex_pattern[i];
            drive->last_ascii_pat[i] = ascii_pattern[i];
        }

        int h = 10;
        int w = 70;
        int y = (LINES - h) / 2;
        int x = (COLS - w) / 2;
        WINDOW *succ_win = newwin(h, w, y, x);
        keypad(succ_win, TRUE);
        box(succ_win, 0, 0);
        wattron(succ_win, COLOR_PAIR(CP_TITLE) | A_BOLD);
        mvwprintw(succ_win, 0, 2, " Search Success ");
        wattroff(succ_win, COLOR_PAIR(CP_TITLE) | A_BOLD);

        mvwprintw(succ_win, 2, 3, "Pattern found at offset 0x%012lX!", (unsigned long)found_offset);
        mvwprintw(succ_win, 4, 3, "Press 'n' for next (n = next) to continue search.");
        mvwprintw(succ_win, 5, 3, "Press any other key to view offset.");
        wrefresh(succ_win);

        int key = wgetch(succ_win);
        delwin(succ_win);
        touchwin(stdscr);

        if (key == 'n' || key == 'N') {
            return execute_search(drive, bytes_per_row, search_type, hex_pattern, ascii_pattern, pat_len, target_bytes, is_full_scan, found_offset + 1, 1);
        }

        return found_offset;
    } else {
        show_message_popup("Search Result", "No matching pattern found in searched scope.");
        return drive->offset;
    }
}

void handle_entropy_menu(drive_info_t *drive, int bytes_per_row) {
    (void)bytes_per_row;
    int m_height = 12;
    int m_width = 62;
    int starty = (LINES - m_height) / 2;
    int startx = (COLS - m_width) / 2;

    WINDOW *menu_win = newwin(m_height, m_width, starty, startx);
    keypad(menu_win, TRUE);

    box(menu_win, 0, 0);
    wattron(menu_win, COLOR_PAIR(CP_TITLE) | A_BOLD);
    mvwprintw(menu_win, 0, 2, " Analysis & Heuristic Menu ");
    wattroff(menu_win, COLOR_PAIR(CP_TITLE) | A_BOLD);

    mvwprintw(menu_win, 2, 3, "Select Analysis Mode:");
    mvwprintw(menu_win, 4, 5, "[1] Global Shannon Entropy Check (Whole Scope)");
    mvwprintw(menu_win, 5, 5, "[2] Shannon Entropy Outlier Search (Block-by-Block)");
    mvwprintw(menu_win, 6, 5, "[3] Multi-Word Phrase Search (Natural Language Engine)");
    mvwprintw(menu_win, 7, 5, "[4] Single Block Shannon Entropy Check");
    mvwprintw(menu_win, 9, 3, "Press 1-4, or Esc to cancel");
    wrefresh(menu_win);

    int c_ch = wgetch(menu_win);
    delwin(menu_win);
    touchwin(stdscr);

    if (c_ch == '1') {
        handle_entropy_check(drive, bytes_per_row);
    } else if (c_ch == '2') {
        // --- Step 1: Threshold Input Popup ---
        int p_height = 8;
        int p_width = 60;
        starty = (LINES - p_height) / 2;
        startx = (COLS - p_width) / 2;

        WINDOW *t_win = newwin(p_height, p_width, starty, startx);
        box(t_win, 0, 0);
        wattron(t_win, COLOR_PAIR(CP_TITLE) | A_BOLD);
        mvwprintw(t_win, 0, 2, " Entropy Outlier Threshold ");
        wattroff(t_win, COLOR_PAIR(CP_TITLE) | A_BOLD);

        mvwprintw(t_win, 2, 3, "Enter max entropy threshold (0.0 - 8.0):");
        mvwprintw(t_win, 3, 3, "(Default is 5.0 for structured/file data)");
        mvwprintw(t_win, 5, 3, "Threshold [5.0]: ");
        wrefresh(t_win);

        char t_buf[32] = {0};
        echo();
        curs_set(1);
        wgetnstr(t_win, t_buf, 31);
        noecho();
        curs_set(0);
        delwin(t_win);
        touchwin(stdscr);

        double threshold = 5.0;
        if (strlen(t_buf) > 0) {
            double parsed = atof(t_buf);
            if (parsed >= 0.0 && parsed <= 8.0) {
                threshold = parsed;
            }
        }

        // --- Step 2: Scope Input Popup ---
        p_height = 10;
        p_width = 66;
        starty = (LINES - p_height) / 2;
        startx = (COLS - p_width) / 2;

        WINDOW *pop = newwin(p_height, p_width, starty, startx);
        keypad(pop, TRUE);

        box(pop, 0, 0);
        wattron(pop, COLOR_PAIR(CP_TITLE) | A_BOLD);
        mvwprintw(pop, 0, 2, " Search Scope ");
        wattroff(pop, COLOR_PAIR(CP_TITLE) | A_BOLD);

        mvwprintw(pop, 2, 3, "Enter scope (e.g. 1%%, 95%%, 8GB, 1TB, 100%%):");
        mvwprintw(pop, 3, 3, "(100%% = Full Sequential Scan; <100%% = 1000-Chunk Scatter Scan)");
        mvwprintw(pop, 6, 3, "Scope: ");
        wrefresh(pop);

        char input_buf[64] = {0};
        echo();
        curs_set(1);
        wgetnstr(pop, input_buf, 63);
        noecho();
        curs_set(0);
        delwin(pop);
        touchwin(stdscr);

        size_t len = strlen(input_buf);
        if (len == 0) return;

        while (len > 0 && (input_buf[len - 1] == ' ' || input_buf[len - 1] == '\r' || input_buf[len - 1] == '\n')) {
            input_buf[--len] = '\0';
        }
        if (len == 0) return;

        char unit = '\0';
        char last_char = input_buf[len - 1];

        if (last_char == '%') {
            unit = '%';
            input_buf[--len] = '\0';
        } else if (last_char == 'B' || last_char == 'b') {
            input_buf[--len] = '\0';
            if (len > 0) {
                char prev_char = input_buf[len - 1];
                if (prev_char == 'T' || prev_char == 't') { unit = 'T'; input_buf[--len] = '\0'; }
                else if (prev_char == 'G' || prev_char == 'g') { unit = 'G'; input_buf[--len] = '\0'; }
                else if (prev_char == 'M' || prev_char == 'm') { unit = 'M'; input_buf[--len] = '\0'; }
                else if (prev_char == 'K' || prev_char == 'k') { unit = 'K'; input_buf[--len] = '\0'; }
            }
        } else if (last_char == 'T' || last_char == 't') {
            unit = 'T'; input_buf[--len] = '\0';
        } else if (last_char == 'G' || last_char == 'g') {
            unit = 'G'; input_buf[--len] = '\0';
        } else if (last_char == 'M' || last_char == 'm') {
            unit = 'M'; input_buf[--len] = '\0';
        } else if (last_char == 'K' || last_char == 'k') {
            unit = 'K'; input_buf[--len] = '\0';
        }

        double numeric_val = atof(input_buf);
        unsigned long long target_bytes = 0;

        if (unit == '\0' && numeric_val <= 100.0) {
            unit = '%';
        }

        if (unit == '%') {
            if (numeric_val < 0) numeric_val = 0;
            if (numeric_val > 100) numeric_val = 100;
            target_bytes = (unsigned long long)((double)drive->size * (numeric_val / 100.0));
        } else if (unit == 'T') {
            target_bytes = (unsigned long long)(numeric_val * 1024.0 * 1024.0 * 1024.0 * 1024.0);
        } else if (unit == 'G') {
            target_bytes = (unsigned long long)(numeric_val * 1024.0 * 1024.0 * 1024.0);
        } else if (unit == 'M') {
            target_bytes = (unsigned long long)(numeric_val * 1024.0 * 1024.0);
        } else if (unit == 'K') {
            target_bytes = (unsigned long long)(numeric_val * 1024.0);
        } else {
            target_bytes = (unsigned long long)numeric_val;
        }

        if (target_bytes == 0) target_bytes = 4096 * 100;
        if (target_bytes > (unsigned long long)drive->size) {
            target_bytes = (unsigned long long)drive->size;
        }

        int is_full_scan = (unit == '%' && numeric_val >= 99.99) || (target_bytes >= (unsigned long long)drive->size);

        drive->offset = execute_entropy_outlier_search(drive, bytes_per_row, threshold, target_bytes, is_full_scan, 0, 0);
    }
    else if (c_ch == '3') {
        // --- Step 1: Minimum Word Size Input Popup ---
        int p_height = 8;
        int p_width = 62;
        starty = (LINES - p_height) / 2;
        startx = (COLS - p_width) / 2;

        WINDOW *t_win = newwin(p_height, p_width, starty, startx);
        box(t_win, 0, 0);
        wattron(t_win, COLOR_PAIR(CP_TITLE) | A_BOLD);
        mvwprintw(t_win, 0, 2, " Multi-Word Phrase Minimum Word Size ");
        wattroff(t_win, COLOR_PAIR(CP_TITLE) | A_BOLD);

        mvwprintw(t_win, 2, 3, "Enter minimum average word size for phrase:");
        mvwprintw(t_win, 3, 3, "(Allows 1-2 letter words like 'A' if average size met)");
        mvwprintw(t_win, 5, 3, "Min Word Size [3]: ");
        wrefresh(t_win);

        char t_buf[32] = {0};
        echo();
        curs_set(1);
        wgetnstr(t_win, t_buf, 31);
        noecho();
        curs_set(0);
        delwin(t_win);
        touchwin(stdscr);

        int min_word_size = 3;
        if (strlen(t_buf) > 0) {
            int parsed = atoi(t_buf);
            if (parsed >= 1 && parsed <= 32) {
                min_word_size = parsed;
            }
        }

        // --- Step 2: Scope Input Popup ---
        p_height = 10;
        p_width = 66;
        starty = (LINES - p_height) / 2;
        startx = (COLS - p_width) / 2;

        WINDOW *pop = newwin(p_height, p_width, starty, startx);
        keypad(pop, TRUE);

        box(pop, 0, 0);
        wattron(pop, COLOR_PAIR(CP_TITLE) | A_BOLD);
        mvwprintw(pop, 0, 2, " Search Scope ");
        wattroff(pop, COLOR_PAIR(CP_TITLE) | A_BOLD);

        mvwprintw(pop, 2, 3, "Enter scope (e.g. 1%%, 95%%, 8GB, 1TB, 100%%):");
        mvwprintw(pop, 3, 3, "(100%% = Full Sequential Scan; <100%% = 1000-Chunk Scatter Scan)");
        mvwprintw(pop, 6, 3, "Scope: ");
        wrefresh(pop);

        char input_buf[64] = {0};
        echo();
        curs_set(1);
        wgetnstr(pop, input_buf, 63);
        noecho();
        curs_set(0);
        delwin(pop);
        touchwin(stdscr);

        size_t len = strlen(input_buf);
        if (len == 0) return;

        while (len > 0 && (input_buf[len - 1] == ' ' || input_buf[len - 1] == '\r' || input_buf[len - 1] == '\n')) {
            input_buf[--len] = '\0';
        }
        if (len == 0) return;

        char unit = '\0';
        char last_char = input_buf[len - 1];

        if (last_char == '%') {
            unit = '%';
            input_buf[--len] = '\0';
        } else if (last_char == 'B' || last_char == 'b') {
            input_buf[--len] = '\0';
            if (len > 0) {
                char prev_char = input_buf[len - 1];
                if (prev_char == 'T' || prev_char == 't') { unit = 'T'; input_buf[--len] = '\0'; }
                else if (prev_char == 'G' || prev_char == 'g') { unit = 'G'; input_buf[--len] = '\0'; }
                else if (prev_char == 'M' || prev_char == 'm') { unit = 'M'; input_buf[--len] = '\0'; }
                else if (prev_char == 'K' || prev_char == 'k') { unit = 'K'; input_buf[--len] = '\0'; }
            }
        } else if (last_char == 'T' || last_char == 't') {
            unit = 'T'; input_buf[--len] = '\0';
        } else if (last_char == 'G' || last_char == 'g') {
            unit = 'G'; input_buf[--len] = '\0';
        } else if (last_char == 'M' || last_char == 'm') {
            unit = 'M'; input_buf[--len] = '\0';
        } else if (last_char == 'K' || last_char == 'k') {
            unit = 'K'; input_buf[--len] = '\0';
        }

        double numeric_val = atof(input_buf);
        unsigned long long target_bytes = 0;

        if (unit == '\0' && numeric_val <= 100.0) {
            unit = '%';
        }

        if (unit == '%') {
            if (numeric_val < 0) numeric_val = 0;
            if (numeric_val > 100) numeric_val = 100;
            target_bytes = (unsigned long long)((double)drive->size * (numeric_val / 100.0));
        } else if (unit == 'T') {
            target_bytes = (unsigned long long)(numeric_val * 1024.0 * 1024.0 * 1024.0 * 1024.0);
        } else if (unit == 'G') {
            target_bytes = (unsigned long long)(numeric_val * 1024.0 * 1024.0 * 1024.0);
        } else if (unit == 'M') {
            target_bytes = (unsigned long long)(numeric_val * 1024.0 * 1024.0);
        } else if (unit == 'K') {
            target_bytes = (unsigned long long)(numeric_val * 1024.0);
        } else {
            target_bytes = (unsigned long long)numeric_val;
        }

        if (target_bytes == 0) target_bytes = 4096 * 100;
        if (target_bytes > (unsigned long long)drive->size) {
            target_bytes = (unsigned long long)drive->size;
        }

        int is_full_scan = (unit == '%' && numeric_val >= 99.99) || (target_bytes >= (unsigned long long)drive->size);

        drive->offset = execute_phrase_text_search(drive, bytes_per_row, min_word_size, target_bytes, is_full_scan, 0, 0);
    }
    else if (c_ch == '4') {
        handle_single_block_entropy(drive, bytes_per_row);
    }
}

off_t execute_entropy_outlier_search(drive_info_t *drive, int bytes_per_row, double threshold, unsigned long long target_bytes, int is_full_scan, off_t start_offset, int is_find_next) {
    double target_mb = (double)target_bytes / (1024.0 * 1024.0);
    if (target_mb < 0.1) target_mb = 0.1;

    size_t block_size = 4096; // 4KB blocks for block entropy calculation
    int found = 0;
    int cancelled = 0;
    off_t found_offset = start_offset;
    double found_entropy = 0.0;

    unsigned char *block_buf = malloc(block_size);
    if (!block_buf) {
        show_message_popup("Error", "Memory allocation failed for outlier block buffer.");
        return drive->offset;
    }

    int prog_h = 8;
    int prog_w = 64;
    int prog_y = (LINES - prog_h) / 2;
    int prog_x = (COLS - prog_w) / 2;
    WINDOW *prog_win = newwin(prog_h, prog_w, prog_y, prog_x);
    keypad(prog_win, TRUE);
    wtimeout(prog_win, 0);

    box(prog_win, 0, 0);
    wattron(prog_win, COLOR_PAIR(CP_TITLE) | A_BOLD);
    mvwprintw(prog_win, 0, 2, is_full_scan ? " Full Entropy Outlier Scan " : " 1000-Chunk Scatter Outlier Scan ");
    wattroff(prog_win, COLOR_PAIR(CP_TITLE) | A_BOLD);
    mvwprintw(prog_win, 2, 3, "Progress:   0%% complete");
    mvwprintw(prog_win, 3, 3, "Checked:    0.0MB / %.1fMB", target_mb);
    mvwprintw(prog_win, 5, 3, "Press ESC to cancel outlier search");
    wrefresh(prog_win);

    struct timespec last_check;
    clock_gettime(CLOCK_MONOTONIC, &last_check);

    if (is_full_scan) {
        unsigned long long total_blocks = (target_bytes + block_size - 1) / block_size;
        if (total_blocks == 0) total_blocks = 1;
        unsigned long long total_bytes_read = 0;

        for (unsigned long long i = 0; i < total_blocks && !found && !cancelled; i++) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            long elapsed_ms = (now.tv_sec - last_check.tv_sec) * 1000 + (now.tv_nsec - last_check.tv_nsec) / 1000000;
            if (elapsed_ms >= 250) {
                last_check = now;
                int ch = wgetch(prog_win);
                if (ch == 27 || ch == 'q' || ch == 'Q') {
                    cancelled = 1;
                    break;
                }
            }

            off_t check_offset = (off_t)(i * block_size);
            if (check_offset >= drive->size) break;

            if (is_find_next && check_offset <= start_offset) {
                total_bytes_read += block_size;
                continue;
            }

            size_t to_read = block_size;
            if (check_offset + (off_t)to_read > drive->size) {
                to_read = (size_t)(drive->size - check_offset);
            }
            if (to_read == 0) break;

            lseek(drive->fd, check_offset, SEEK_SET);
            ssize_t n = read(drive->fd, block_buf, to_read);
            if (n <= 0) break;

            // Calculate Shannon Entropy for this block
            unsigned long long byte_counts[256] = {0};
            for (ssize_t b = 0; b < n; b++) {
                byte_counts[block_buf[b]]++;
            }

            double block_entropy = 0.0;
            for (int b = 0; b < 256; b++) {
                if (byte_counts[b] > 0) {
                    double p = (double)byte_counts[b] / (double)n;
                    block_entropy -= p * (log(p) / 0.693147180559945319);
                }
            }

            if (block_entropy < threshold) {
                found = 1;
                found_offset = check_offset;
                found_entropy = block_entropy;
                break;
            }

            total_bytes_read += n;
            int percent = (int)((total_bytes_read * 100) / target_bytes);
            if (percent > 100) percent = 100;
            double checked_mb = (double)total_bytes_read / (1024.0 * 1024.0);

            mvwprintw(prog_win, 2, 13, "%3d%% complete", percent);
            mvwprintw(prog_win, 3, 13, "%.1fMB / %.1fMB", checked_mb, target_mb);
            wrefresh(prog_win);
        }
    } else {
        int num_chunks = 1000;
        unsigned long long total_blocks = (target_bytes + block_size - 1) / block_size;
        if (total_blocks < (unsigned long long)num_chunks) {
            num_chunks = (int)total_blocks;
            if (num_chunks < 1) num_chunks = 1;
        }

        typedef struct {
            unsigned long long start_blk;
            unsigned long long end_blk;
        } chunk_range_t;

        chunk_range_t *chunks = malloc(num_chunks * sizeof(chunk_range_t));
        if (!chunks) {
            free(block_buf);
            delwin(prog_win);
            touchwin(stdscr);
            show_message_popup("Error", "Memory allocation failed for chunks.");
            return drive->offset;
        }

        for (int i = 0; i < num_chunks; i++) {
            chunks[i].start_blk = ((unsigned long long)i * total_blocks) / num_chunks;
            chunks[i].end_blk = ((unsigned long long)(i + 1) * total_blocks) / num_chunks;
        }

        int *indices = malloc(num_chunks * sizeof(int));
        if (!indices) {
            free(chunks);
            free(block_buf);
            delwin(prog_win);
            touchwin(stdscr);
            show_message_popup("Error", "Memory allocation failed for chunk indices.");
            return drive->offset;
        }
        for (int i = 0; i < num_chunks; i++) {
            indices[i] = i;
        }

        for (int i = num_chunks - 1; i > 0; i--) {
            unsigned long long r1 = ((unsigned long long)rand() << 30) ^ rand();
            int j = (int)(r1 % (i + 1));
            int temp = indices[i];
            indices[i] = indices[j];
            indices[j] = temp;
        }

        unsigned long long total_bytes_read = 0;

        for (int idx = 0; idx < num_chunks && !found && !cancelled; idx++) {
            int c_idx = indices[idx];
            unsigned long long start_blk = chunks[c_idx].start_blk;
            unsigned long long end_blk = chunks[c_idx].end_blk;

            for (unsigned long long blk = start_blk; blk < end_blk && !found && !cancelled; blk++) {
                if (total_bytes_read >= target_bytes) break;

                struct timespec now;
                clock_gettime(CLOCK_MONOTONIC, &now);
                long elapsed_ms = (now.tv_sec - last_check.tv_sec) * 1000 + (now.tv_nsec - last_check.tv_nsec) / 1000000;
                if (elapsed_ms >= 250) {
                    last_check = now;
                    int ch = wgetch(prog_win);
                    if (ch == 27 || ch == 'q' || ch == 'Q') {
                        cancelled = 1;
                        break;
                    }
                }

                off_t check_offset = (off_t)(blk * block_size);
                if (check_offset >= drive->size) break;

                if (is_find_next && check_offset <= start_offset) {
                    total_bytes_read += block_size;
                    continue;
                }

                size_t to_read = block_size;
                if (check_offset + (off_t)to_read > drive->size) {
                    to_read = (size_t)(drive->size - check_offset);
                }
                if (to_read == 0) break;

                lseek(drive->fd, check_offset, SEEK_SET);
                ssize_t n = read(drive->fd, block_buf, to_read);
                if (n <= 0) break;

                unsigned long long byte_counts[256] = {0};
                for (ssize_t b = 0; b < n; b++) {
                    byte_counts[block_buf[b]]++;
                }

                double block_entropy = 0.0;
                for (int b = 0; b < 256; b++) {
                    if (byte_counts[b] > 0) {
                        double p = (double)byte_counts[b] / (double)n;
                        block_entropy -= p * (log(p) / 0.693147180559945319);
                    }
                }

                if (block_entropy < threshold) {
                    found = 1;
                    found_offset = check_offset;
                    found_entropy = block_entropy;
                    break;
                }

                total_bytes_read += n;
                int percent = (int)((total_bytes_read * 100) / target_bytes);
                if (percent > 100) percent = 100;
                double checked_mb = (double)total_bytes_read / (1024.0 * 1024.0);

                mvwprintw(prog_win, 2, 13, "%3d%% complete", percent);
                mvwprintw(prog_win, 3, 13, "%.1fMB / %.1fMB", checked_mb, target_mb);
                wrefresh(prog_win);
            }

            if (total_bytes_read >= target_bytes) break;
        }

        free(indices);
        free(chunks);
    }

    free(block_buf);
    delwin(prog_win);
    touchwin(stdscr);

    if (cancelled) {
        show_message_popup("Cancelled", "Outlier search cancelled by user.");
        return drive->offset;
    }

    if (found) {
        int h = 10;
        int w = 70;
        int y = (LINES - h) / 2;
        int x = (COLS - w) / 2;
        WINDOW *succ_win = newwin(h, w, y, x);
        keypad(succ_win, TRUE);
        box(succ_win, 0, 0);
        wattron(succ_win, COLOR_PAIR(CP_TITLE) | A_BOLD);
        mvwprintw(succ_win, 0, 2, " Outlier Found ");
        wattroff(succ_win, COLOR_PAIR(CP_TITLE) | A_BOLD);

        mvwprintw(succ_win, 2, 3, "Outlier block at offset 0x%012lX!", (unsigned long)found_offset);
        mvwprintw(succ_win, 3, 3, "Block Entropy: %.4f (Threshold: < %.1f)", found_entropy, threshold);
        mvwprintw(succ_win, 5, 3, "Press 'n' for next (n = next) to continue search.");
        mvwprintw(succ_win, 6, 3, "Press any other key to view offset.");
        wrefresh(succ_win);

        int key = wgetch(succ_win);
        delwin(succ_win);
        touchwin(stdscr);

        if (key == 'n' || key == 'N') {
            return execute_entropy_outlier_search(drive, bytes_per_row, threshold, target_bytes, is_full_scan, found_offset, 1);
        }

        return found_offset;
    } else {
        show_message_popup("Search Result", "No blocks found below entropy threshold.");
        return drive->offset;
    }
}

int is_valid_word(const unsigned char *word_buf, int len) {
    if (len <= 0) return 0;
    int vowels = 0;
    int letters = 0;
    int max_consec_consonants = 0;
    int current_consec_consonants = 0;

    for (int i = 0; i < len; i++) {
        unsigned char c = word_buf[i];
        if (isalpha(c)) {
            letters++;
            char lower = tolower(c);
            if (lower == 'a' || lower == 'e' || lower == 'i' || lower == 'o' || lower == 'u') {
                vowels++;
                if (current_consec_consonants > max_consec_consonants) {
                    max_consec_consonants = current_consec_consonants;
                }
                current_consec_consonants = 0;
            } else {
                current_consec_consonants++;
            }
        }
    }
    if (current_consec_consonants > max_consec_consonants) {
        max_consec_consonants = current_consec_consonants;
    }

    // Max 3 consecutive consonants per word
    if (max_consec_consonants > 3) {
        return 0;
    }

    // For words of length 3+, require at least 1 true vowel
    if (len >= 3 && vowels == 0) {
        return 0;
    }

    return 1;
}

off_t execute_phrase_text_search(drive_info_t *drive, int bytes_per_row, int min_word_size, unsigned long long target_bytes, int is_full_scan, off_t start_offset, int is_find_next) {
    double target_mb = (double)target_bytes / (1024.0 * 1024.0);
    if (target_mb < 0.1) target_mb = 0.1;

    size_t sector_size = 512;
    int found = 0;
    int cancelled = 0;
    off_t found_offset = start_offset;

    unsigned char *sector_buf = malloc(sector_size);
    if (!sector_buf) {
        show_message_popup("Error", "Memory allocation failed for phrase search buffer.");
        return drive->offset;
    }

    int prog_h = 8;
    int prog_w = 64;
    int prog_y = (LINES - prog_h) / 2;
    int prog_x = (COLS - prog_w) / 2;
    WINDOW *prog_win = newwin(prog_h, prog_w, prog_y, prog_x);
    keypad(prog_win, TRUE);
    wtimeout(prog_win, 0);

    box(prog_win, 0, 0);
    wattron(prog_win, COLOR_PAIR(CP_TITLE) | A_BOLD);
    mvwprintw(prog_win, 0, 2, is_full_scan ? " Full Multi-Word Phrase Scan " : " 1000-Chunk Scatter Phrase Scan ");
    wattroff(prog_win, COLOR_PAIR(CP_TITLE) | A_BOLD);

    unsigned long long initial_bytes_scanned = is_find_next ? start_offset : 0;
    int initial_percent = 0;
    if (target_bytes > 0) {
        initial_percent = (int)((initial_bytes_scanned * 100) / target_bytes);
        if (initial_percent > 100) initial_percent = 100;
    }

    mvwprintw(prog_win, 2, 3, "Progress:   %3d%% complete", initial_percent);
    mvwprintw(prog_win, 3, 3, "Checked:    %.1fMB / %.1fMB", (double)initial_bytes_scanned / (1024.0 * 1024.0), target_mb);
    mvwprintw(prog_win, 5, 3, "Press ESC to cancel search");
    wrefresh(prog_win);

    struct timespec last_check;
    clock_gettime(CLOCK_MONOTONIC, &last_check);

    if (is_full_scan) {
        unsigned long long start_sec = (unsigned long long)(start_offset / sector_size);
        unsigned long long total_sectors = (target_bytes + sector_size - 1) / sector_size;
        if (total_sectors == 0) total_sectors = 1;

        unsigned char history[256] = {0};
        int history_len = 0;
        unsigned long long session_bytes_read = 0;

        for (unsigned long long i = start_sec; i < total_sectors && !found && !cancelled; i++) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            long elapsed_ms = (now.tv_sec - last_check.tv_sec) * 1000 + (now.tv_nsec - last_check.tv_nsec) / 1000000;
            if (elapsed_ms >= 250) {
                last_check = now;
                int ch = wgetch(prog_win);
                if (ch == 27 || ch == 'q' || ch == 'Q') {
                    cancelled = 1;
                    break;
                }
            }

            off_t check_offset = (off_t)(i * sector_size);
            if (check_offset >= drive->size) break;

            size_t to_read = sector_size;
            if (check_offset + (off_t)to_read > drive->size) {
                to_read = (size_t)(drive->size - check_offset);
            }
            if (to_read == 0) break;

            lseek(drive->fd, check_offset, SEEK_SET);
            ssize_t n = read(drive->fd, sector_buf, to_read);
            if (n <= 0) break;

            unsigned char search_buf[512 + 256];
            memcpy(search_buf, history, history_len);
            memcpy(search_buf + history_len, sector_buf, n);
            int total_len = history_len + (int)n;

            int phrase_start_idx = -1;
            int word_count = 0;
            int total_word_chars = 0;
            int current_word_len = 0;
            unsigned char current_word[64];

            for (int idx = 0; idx <= total_len; idx++) {
                off_t current_byte_offset = check_offset - history_len + idx;
                if (current_byte_offset < 0) continue;
                if (current_byte_offset >= drive->size) break;

                if (is_find_next && current_byte_offset < start_offset) {
                    continue;
                }

                int is_end = (idx == total_len);
                unsigned char c = 0;
                if (!is_end) {
                    c = search_buf[idx];
                }

                if (!is_end && (c == '\'' || c == '"')) {
                    continue;
                }

                int is_letter = (!is_end && isalpha(c));

                if (is_letter) {
                    if (phrase_start_idx == -1) {
                        phrase_start_idx = idx;
                    }
                    if (current_word_len < 63) {
                        current_word[current_word_len++] = c;
                    }
                } else {
                    if (current_word_len > 0) {
                        current_word[current_word_len] = '\0';
                        if (is_valid_word(current_word, current_word_len)) {
                            word_count++;
                            total_word_chars += current_word_len;
                        } else {
                            phrase_start_idx = -1;
                            word_count = 0;
                            total_word_chars = 0;
                        }
                        current_word_len = 0;

                        if (word_count >= 3) {
                            int avg_size = total_word_chars / word_count;
                            if (avg_size >= min_word_size) {
                                found_offset = check_offset - history_len + phrase_start_idx;
                                found = 1;
                                break;
                            }
                        }
                    }

                    if (!is_end) {
                        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
                            if (word_count < 3) {
                                phrase_start_idx = -1;
                                word_count = 0;
                                total_word_chars = 0;
                            }
                        }
                    }
                }
            }

            int keep = 128;
            if (keep > n) keep = n;
            if (keep > 0) {
                memcpy(history, sector_buf + n - keep, keep);
                history_len = keep;
            } else {
                history_len = 0;
            }

            if (found) break;

            session_bytes_read += n;
            unsigned long long total_bytes_scanned = initial_bytes_scanned + session_bytes_read;
            if (total_bytes_scanned > target_bytes) total_bytes_scanned = target_bytes;

            int percent = (int)((total_bytes_scanned * 100) / target_bytes);
            if (percent > 100) percent = 100;
            double checked_mb = (double)total_bytes_scanned / (1024.0 * 1024.0);

            mvwprintw(prog_win, 2, 13, "%3d%% complete", percent);
            mvwprintw(prog_win, 3, 13, "%.1fMB / %.1fMB", checked_mb, target_mb);
            wrefresh(prog_win);
        }
    } else {
        int num_chunks = 1000;
        unsigned long long total_sectors = (target_bytes + sector_size - 1) / sector_size;
        if (total_sectors < (unsigned long long)num_chunks) {
            num_chunks = (int)total_sectors;
            if (num_chunks < 1) num_chunks = 1;
        }

        typedef struct {
            unsigned long long start_sec;
            unsigned long long end_sec;
        } chunk_range_t;

        chunk_range_t *chunks = malloc(num_chunks * sizeof(chunk_range_t));
        if (!chunks) {
            free(sector_buf);
            delwin(prog_win);
            touchwin(stdscr);
            show_message_popup("Error", "Memory allocation failed for chunks.");
            return drive->offset;
        }

        for (int i = 0; i < num_chunks; i++) {
            chunks[i].start_sec = ((unsigned long long)i * total_sectors) / num_chunks;
            chunks[i].end_sec = ((unsigned long long)(i + 1) * total_sectors) / num_chunks;
        }

        int *indices = malloc(num_chunks * sizeof(int));
        if (!indices) {
            free(chunks);
            free(sector_buf);
            delwin(prog_win);
            touchwin(stdscr);
            show_message_popup("Error", "Memory allocation failed for chunk indices.");
            return drive->offset;
        }
        for (int i = 0; i < num_chunks; i++) {
            indices[i] = i;
        }

        for (int i = num_chunks - 1; i > 0; i--) {
            unsigned long long r1 = ((unsigned long long)rand() << 30) ^ rand();
            int j = (int)(r1 % (i + 1));
            int temp = indices[i];
            indices[i] = indices[j];
            indices[j] = temp;
        }

        unsigned long long session_bytes_read = 0;

        for (int idx = 0; idx < num_chunks && !found && !cancelled; idx++) {
            int c_idx = indices[idx];
            unsigned long long start_sec = chunks[c_idx].start_sec;
            unsigned long long end_sec = chunks[c_idx].end_sec;

            unsigned char history[256] = {0};
            int history_len = 0;

            for (unsigned long long sec = start_sec; sec < end_sec && !found && !cancelled; sec++) {
                struct timespec now;
                clock_gettime(CLOCK_MONOTONIC, &now);
                long elapsed_ms = (now.tv_sec - last_check.tv_sec) * 1000 + (now.tv_nsec - last_check.tv_nsec) / 1000000;
                if (elapsed_ms >= 250) {
                    last_check = now;
                    int ch = wgetch(prog_win);
                    if (ch == 27 || ch == 'q' || ch == 'Q') {
                        cancelled = 1;
                        break;
                    }
                }

                off_t check_offset = (off_t)(sec * sector_size);
                if (check_offset >= drive->size) break;

                size_t to_read = sector_size;
                if (check_offset + (off_t)to_read > drive->size) {
                    to_read = (size_t)(drive->size - check_offset);
                }
                if (to_read == 0) break;

                lseek(drive->fd, check_offset, SEEK_SET);
                ssize_t n = read(drive->fd, sector_buf, to_read);
                if (n <= 0) break;

                unsigned char search_buf[512 + 256];
                memcpy(search_buf, history, history_len);
                memcpy(search_buf + history_len, sector_buf, n);
                int total_len = history_len + (int)n;

                int phrase_start_idx = -1;
                int word_count = 0;
                int total_word_chars = 0;
                int current_word_len = 0;
                unsigned char current_word[64];

                for (int s_idx = 0; s_idx <= total_len; s_idx++) {
                    off_t current_byte_offset = check_offset - history_len + s_idx;
                    if (current_byte_offset < 0) continue;
                    if (current_byte_offset >= drive->size) break;

                    if (is_find_next && current_byte_offset < start_offset) {
                        continue;
                    }

                    int is_end = (s_idx == total_len);
                    unsigned char c = 0;
                    if (!is_end) {
                        c = search_buf[s_idx];
                    }

                    if (!is_end && (c == '\'' || c == '"')) {
                        continue;
                    }

                    int is_letter = (!is_end && isalpha(c));

                    if (is_letter) {
                        if (phrase_start_idx == -1) {
                            phrase_start_idx = s_idx;
                        }
                        if (current_word_len < 63) {
                            current_word[current_word_len++] = c;
                        }
                    } else {
                        if (current_word_len > 0) {
                            current_word[current_word_len] = '\0';
                            if (is_valid_word(current_word, current_word_len)) {
                                word_count++;
                                total_word_chars += current_word_len;
                            } else {
                                phrase_start_idx = -1;
                                word_count = 0;
                                total_word_chars = 0;
                            }
                            current_word_len = 0;

                            if (word_count >= 3) {
                                int avg_size = total_word_chars / word_count;
                                if (avg_size >= min_word_size) {
                                    found_offset = check_offset - history_len + phrase_start_idx;
                                    found = 1;
                                    break;
                                }
                            }
                        }

                        if (!is_end) {
                            if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
                                if (word_count < 3) {
                                    phrase_start_idx = -1;
                                    word_count = 0;
                                    total_word_chars = 0;
                                }
                            }
                        }
                    }
                }

                int keep = 128;
                if (keep > n) keep = n;
                if (keep > 0) {
                    memcpy(history, sector_buf + n - keep, keep);
                    history_len = keep;
                } else {
                    history_len = 0;
                }

                if (found) break;

                session_bytes_read += n;
                unsigned long long total_bytes_scanned = initial_bytes_scanned + session_bytes_read;
                if (total_bytes_scanned > target_bytes) total_bytes_scanned = target_bytes;

                int percent = (int)((total_bytes_scanned * 100) / target_bytes);
                if (percent > 100) percent = 100;
                double checked_mb = (double)total_bytes_scanned / (1024.0 * 1024.0);

                mvwprintw(prog_win, 2, 13, "%3d%% complete", percent);
                mvwprintw(prog_win, 3, 13, "%.1fMB / %.1fMB", checked_mb, target_mb);
                wrefresh(prog_win);
            }

            if (found) break;
        }

        free(indices);
        free(chunks);
    }

    free(sector_buf);
    delwin(prog_win);
    touchwin(stdscr);

    if (cancelled) {
        show_message_popup("Cancelled", "Phrase search cancelled by user.");
        return drive->offset;
    }

    if (found) {
        char text_preview[44] = {0};
        lseek(drive->fd, found_offset, SEEK_SET);
        unsigned char preview_buf[40];
        ssize_t p_n = read(drive->fd, preview_buf, sizeof(preview_buf));
        if (p_n > 0) {
            int p_len = 0;
            for (ssize_t i = 0; i < p_n && p_len < 40; i++) {
                unsigned char c = preview_buf[i];
                if (c >= 32 && c <= 126) {
                    text_preview[p_len++] = c;
                } else {
                    text_preview[p_len++] = '.';
                }
            }
            text_preview[p_len] = '\0';
        }

        int h = 12;
        int w = 76;
        int y = (LINES - h) / 2;
        int x = (COLS - w) / 2;
        WINDOW *succ_win = newwin(h, w, y, x);
        keypad(succ_win, TRUE);
        box(succ_win, 0, 0);
        wattron(succ_win, COLOR_PAIR(CP_TITLE) | A_BOLD);
        mvwprintw(succ_win, 0, 2, " Multi-Word Phrase Found ");
        wattroff(succ_win, COLOR_PAIR(CP_TITLE) | A_BOLD);

        mvwprintw(succ_win, 2, 3, "Offset:       0x%012lX", (unsigned long)found_offset);
        mvwprintw(succ_win, 3, 3, "Min Word Sz:  %d (Average Verified)", min_word_size);
        mvwprintw(succ_win, 5, 3, "Preview:      \"%s\"", text_preview);
        mvwprintw(succ_win, 8, 3, "Press 'n' for next, or any key to view offset.");
        wrefresh(succ_win);

        int key = wgetch(succ_win);
        delwin(succ_win);
        touchwin(stdscr);

        if (key == 'n' || key == 'N') {
            return execute_phrase_text_search(drive, bytes_per_row, min_word_size, target_bytes, is_full_scan, found_offset + 1, 1);
        }

        return found_offset;
    } else {
        show_message_popup("Search Result", "No multi-word phrases found meeting criteria.");
        return drive->offset;
    }
}

void handle_single_block_entropy(drive_info_t *drive, int bytes_per_row) {
    (void)bytes_per_row;
    int p_height = 9;
    int p_width = 64;
    int starty = (LINES - p_height) / 2;
    int startx = (COLS - p_width) / 2;

    WINDOW *pop = newwin(p_height, p_width, starty, startx);
    keypad(pop, TRUE);

    box(pop, 0, 0);
    wattron(pop, COLOR_PAIR(CP_TITLE) | A_BOLD);
    mvwprintw(pop, 0, 2, " Single Block Entropy Check ");
    wattroff(pop, COLOR_PAIR(CP_TITLE) | A_BOLD);

    mvwprintw(pop, 2, 3, "Enter 512-byte Block Number (e.g. 2048)");
    mvwprintw(pop, 3, 3, "or Hex/Dec Byte Offset (e.g. 0x100000):");
    mvwprintw(pop, 6, 3, "Target: ");
    wrefresh(pop);

    char input_buf[64] = {0};
    echo();
    curs_set(1);
    wgetnstr(pop, input_buf, 63);
    noecho();
    curs_set(0);
    delwin(pop);
    touchwin(stdscr);

    size_t len = strlen(input_buf);
    if (len == 0) return;

    while (len > 0 && (input_buf[len - 1] == ' ' || input_buf[len - 1] == '\r' || input_buf[len - 1] == '\n')) {
        input_buf[--len] = '\0';
    }
    if (len == 0) return;

    unsigned long long parsed_val = 0;
    int is_hex = (strncmp(input_buf, "0x", 2) == 0 || strncmp(input_buf, "0X", 2) == 0);

    if (is_hex) {
        sscanf(input_buf, "%llx", &parsed_val);
    } else {
        int has_alpha_hex = 0;
        for (size_t i = 0; i < len; i++) {
            char c = tolower(input_buf[i]);
            if ((c >= 'a' && c <= 'f')) {
                has_alpha_hex = 1;
                break;
            }
        }
        if (has_alpha_hex) {
            sscanf(input_buf, "%llx", &parsed_val);
            is_hex = 1;
        } else {
            sscanf(input_buf, "%llu", &parsed_val);
        }
    }

    off_t target_offset = 0;
    unsigned long long block_num = 0;
    size_t block_size = 512;

    unsigned long long total_sectors = (unsigned long long)(drive->size / (off_t)block_size);
    if (is_hex || parsed_val >= total_sectors) {
        target_offset = (off_t)parsed_val;
        if (target_offset >= drive->size) {
            target_offset = (drive->size > (off_t)block_size) ? (drive->size - (off_t)block_size) : 0;
        }
        block_num = (unsigned long long)(target_offset / (off_t)block_size);
    } else {
        block_num = parsed_val;
        target_offset = (off_t)(block_num * block_size);
        if (target_offset >= drive->size) {
            target_offset = (drive->size > (off_t)block_size) ? (drive->size - (off_t)block_size) : 0;
            block_num = (unsigned long long)(target_offset / (off_t)block_size);
        }
    }

    unsigned char *block_buf = malloc(block_size);
    if (!block_buf) {
        show_message_popup("Error", "Memory allocation failed for single block buffer.");
        return;
    }

    lseek(drive->fd, target_offset, SEEK_SET);
    ssize_t n = read(drive->fd, block_buf, block_size);
    if (n <= 0) {
        free(block_buf);
        show_message_popup("Error", "Failed to read sector from drive.");
        return;
    }

    unsigned long long byte_counts[256] = {0};
    for (ssize_t b = 0; b < n; b++) {
        byte_counts[block_buf[b]]++;
    }

    double entropy = 0.0;
    for (int i = 0; i < 256; i++) {
        if (byte_counts[i] > 0) {
            double p = (double)byte_counts[i] / (double)n;
            entropy -= p * (log(p) / 0.693147180559945319);
        }
    }

    free(block_buf);

    const char *status_desc = "Unknown";
    if (entropy < 0.1) {
        status_desc = "Plain Zeros (0x00) / Empty Space";
    } else if (entropy < 1.0) {
        status_desc = "Low Entropy / Repeating Pattern";
    } else if (entropy < 7.0) {
        status_desc = "Structured/Compressed Data / Standard Sector";
    } else if (entropy >= 7.9) {
        status_desc = "Max Random Noise (NIST Wipe Verified!)";
    } else {
        status_desc = "High Randomness / Encrypted Sector";
    }

    char title_buf[64];
    char msg_buf[384];
    snprintf(title_buf, sizeof(title_buf), " Single Block Entropy Result ");
    snprintf(msg_buf, sizeof(msg_buf), "Block Index:     %llu\nByte Offset:     0x%012lX\nShannon Entropy: %.4f / 8.0\nStatus:          %s",
             block_num, (unsigned long)target_offset, entropy, status_desc);

    show_message_popup(title_buf, msg_buf);
}

void handle_entropy_check(drive_info_t *drive, int bytes_per_row) {
    (void)bytes_per_row;
    int p_height = 10;
    int p_width = 66;
    int starty = (LINES - p_height) / 2;
    int startx = (COLS - p_width) / 2;

    WINDOW *pop = newwin(p_height, p_width, starty, startx);
    keypad(pop, TRUE);

    box(pop, 0, 0);
    wattron(pop, COLOR_PAIR(CP_TITLE) | A_BOLD);
    mvwprintw(pop, 0, 2, " Block Entropy Calculation (Shannon) ");
    wattroff(pop, COLOR_PAIR(CP_TITLE) | A_BOLD);

    mvwprintw(pop, 2, 3, "Enter scope (e.g. 1%%, 95%%, 8GB, 1TB, 100%%):");
    mvwprintw(pop, 3, 3, "(100%% = Full Sequential Scan; <100%% = 1000-Chunk Scatter Scan)");
    mvwprintw(pop, 6, 3, "Scope: ");
    wrefresh(pop);

    char input_buf[64] = {0};
    echo();
    curs_set(1);
    wgetnstr(pop, input_buf, 63);
    noecho();
    curs_set(0);
    delwin(pop);
    touchwin(stdscr);

    size_t len = strlen(input_buf);
    if (len == 0) return;

    while (len > 0 && (input_buf[len - 1] == ' ' || input_buf[len - 1] == '\r' || input_buf[len - 1] == '\n')) {
        input_buf[--len] = '\0';
    }
    if (len == 0) return;

    char unit = '\0';
    char last_char = input_buf[len - 1];

    if (last_char == '%') {
        unit = '%';
        input_buf[--len] = '\0';
    } else if (last_char == 'B' || last_char == 'b') {
        input_buf[--len] = '\0';
        if (len > 0) {
            char prev_char = input_buf[len - 1];
            if (prev_char == 'T' || prev_char == 't') { unit = 'T'; input_buf[--len] = '\0'; }
            else if (prev_char == 'G' || prev_char == 'g') { unit = 'G'; input_buf[--len] = '\0'; }
            else if (prev_char == 'M' || prev_char == 'm') { unit = 'M'; input_buf[--len] = '\0'; }
            else if (prev_char == 'K' || prev_char == 'k') { unit = 'K'; input_buf[--len] = '\0'; }
        }
    } else if (last_char == 'T' || last_char == 't') {
        unit = 'T'; input_buf[--len] = '\0';
    } else if (last_char == 'G' || last_char == 'g') {
        unit = 'G'; input_buf[--len] = '\0';
    } else if (last_char == 'M' || last_char == 'm') {
        unit = 'M'; input_buf[--len] = '\0';
    } else if (last_char == 'K' || last_char == 'k') {
        unit = 'K'; input_buf[--len] = '\0';
    }

    double numeric_val = atof(input_buf);
    unsigned long long target_bytes = 0;

    if (unit == '\0' && numeric_val <= 100.0) {
        unit = '%';
    }

    if (unit == '%') {
        if (numeric_val < 0) numeric_val = 0;
        if (numeric_val > 100) numeric_val = 100;
        target_bytes = (unsigned long long)((double)drive->size * (numeric_val / 100.0));
    } else if (unit == 'T') {
        target_bytes = (unsigned long long)(numeric_val * 1024.0 * 1024.0 * 1024.0 * 1024.0);
    } else if (unit == 'G') {
        target_bytes = (unsigned long long)(numeric_val * 1024.0 * 1024.0 * 1024.0);
    } else if (unit == 'M') {
        target_bytes = (unsigned long long)(numeric_val * 1024.0 * 1024.0);
    } else if (unit == 'K') {
        target_bytes = (unsigned long long)(numeric_val * 1024.0);
    } else {
        target_bytes = (unsigned long long)numeric_val;
    }

    if (target_bytes == 0) target_bytes = 4096 * 100;
    if (target_bytes > (unsigned long long)drive->size) {
        target_bytes = (unsigned long long)drive->size;
    }

    double target_mb = (double)target_bytes / (1024.0 * 1024.0);
    if (target_mb < 0.1) target_mb = 0.1;

    int is_full_scan = (unit == '%' && numeric_val >= 99.99) || (target_bytes >= (unsigned long long)drive->size);

    size_t sector_size = 4096;
    int cancelled = 0;
    unsigned char *sector_buf = malloc(sector_size);
    if (!sector_buf) {
        show_message_popup("Error", "Memory allocation failed for entropy buffer.");
        return;
    }

    int prog_h = 8;
    int prog_w = 64;
    int prog_y = (LINES - prog_h) / 2;
    int prog_x = (COLS - prog_w) / 2;
    WINDOW *prog_win = newwin(prog_h, prog_w, prog_y, prog_x);
    keypad(prog_win, TRUE);
    wtimeout(prog_win, 0);

    box(prog_win, 0, 0);
    wattron(prog_win, COLOR_PAIR(CP_TITLE) | A_BOLD);
    mvwprintw(prog_win, 0, 2, is_full_scan ? " Full Entropy Scan " : " 1000-Chunk Scatter Entropy Scan ");
    wattroff(prog_win, COLOR_PAIR(CP_TITLE) | A_BOLD);
    mvwprintw(prog_win, 2, 3, "Progress:   0%% complete");
    mvwprintw(prog_win, 3, 3, "Checked:    0.0MB / %.1fMB", target_mb);
    mvwprintw(prog_win, 5, 3, "Press ESC to cancel entropy check");
    wrefresh(prog_win);

    struct timespec last_check;
    clock_gettime(CLOCK_MONOTONIC, &last_check);

    unsigned long long byte_counts[256] = {0};
    unsigned long long total_sampled_bytes = 0;

    if (is_full_scan) {
        unsigned long long total_sectors = (target_bytes + sector_size - 1) / sector_size;
        if (total_sectors == 0) total_sectors = 1;

        for (unsigned long long i = 0; i < total_sectors && !cancelled; i++) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            long elapsed_ms = (now.tv_sec - last_check.tv_sec) * 1000 + (now.tv_nsec - last_check.tv_nsec) / 1000000;
            if (elapsed_ms >= 250) {
                last_check = now;
                int ch = wgetch(prog_win);
                if (ch == 27 || ch == 'q' || ch == 'Q') {
                    cancelled = 1;
                    break;
                }
            }

            off_t check_offset = (off_t)(i * sector_size);
            if (check_offset >= drive->size) break;

            size_t to_read = sector_size;
            if (check_offset + (off_t)to_read > drive->size) {
                to_read = (size_t)(drive->size - check_offset);
            }
            if (to_read == 0) break;

            lseek(drive->fd, check_offset, SEEK_SET);
            ssize_t n = read(drive->fd, sector_buf, to_read);
            if (n <= 0) break;

            for (ssize_t b = 0; b < n; b++) {
                byte_counts[sector_buf[b]]++;
            }
            total_sampled_bytes += n;

            int percent = (int)((total_sampled_bytes * 100) / target_bytes);
            if (percent > 100) percent = 100;
            double checked_mb = (double)total_sampled_bytes / (1024.0 * 1024.0);

            mvwprintw(prog_win, 2, 13, "%3d%% complete", percent);
            mvwprintw(prog_win, 3, 13, "%.1fMB / %.1fMB", checked_mb, target_mb);
            wrefresh(prog_win);
        }
    } else {
        int num_chunks = 1000;
        unsigned long long total_sectors = (drive->size + sector_size - 1) / sector_size;
        if (total_sectors < (unsigned long long)num_chunks) {
            num_chunks = (int)total_sectors;
            if (num_chunks < 1) num_chunks = 1;
        }

        typedef struct {
            unsigned long long start_sec;
            unsigned long long end_sec;
        } chunk_range_t;

        chunk_range_t *chunks = malloc(num_chunks * sizeof(chunk_range_t));
        if (!chunks) {
            free(sector_buf);
            delwin(prog_win);
            touchwin(stdscr);
            show_message_popup("Error", "Memory allocation failed for chunks.");
            return;
        }

        for (int i = 0; i < num_chunks; i++) {
            chunks[i].start_sec = ((unsigned long long)i * total_sectors) / num_chunks;
            chunks[i].end_sec = ((unsigned long long)(i + 1) * total_sectors) / num_chunks;
        }

        int *indices = malloc(num_chunks * sizeof(int));
        if (!indices) {
            free(chunks);
            free(sector_buf);
            delwin(prog_win);
            touchwin(stdscr);
            show_message_popup("Error", "Memory allocation failed for chunk indices.");
            return;
        }
        for (int i = 0; i < num_chunks; i++) {
            indices[i] = i;
        }

        for (int i = num_chunks - 1; i > 0; i--) {
            unsigned long long r1 = ((unsigned long long)rand() << 30) ^ rand();
            int j = (int)(r1 % (i + 1));
            int temp = indices[i];
            indices[i] = indices[j];
            indices[j] = temp;
        }

        for (int idx = 0; idx < num_chunks && !cancelled; idx++) {
            int c_idx = indices[idx];
            unsigned long long start_sec = chunks[c_idx].start_sec;
            unsigned long long end_sec = chunks[c_idx].end_sec;

            for (unsigned long long sec = start_sec; sec < end_sec && !cancelled; sec++) {
                if (total_sampled_bytes >= target_bytes) break;

                struct timespec now;
                clock_gettime(CLOCK_MONOTONIC, &now);
                long elapsed_ms = (now.tv_sec - last_check.tv_sec) * 1000 + (now.tv_nsec - last_check.tv_nsec) / 1000000;
                if (elapsed_ms >= 250) {
                    last_check = now;
                    int ch = wgetch(prog_win);
                    if (ch == 27 || ch == 'q' || ch == 'Q') {
                        cancelled = 1;
                        break;
                    }
                }

                off_t check_offset = (off_t)(sec * sector_size);
                if (check_offset >= drive->size) break;

                size_t to_read = sector_size;
                if (check_offset + (off_t)to_read > drive->size) {
                    to_read = (size_t)(drive->size - check_offset);
                }
                if (to_read == 0) break;

                lseek(drive->fd, check_offset, SEEK_SET);
                ssize_t n = read(drive->fd, sector_buf, to_read);
                if (n <= 0) break;

                for (ssize_t b = 0; b < n; b++) {
                    byte_counts[sector_buf[b]]++;
                }
                total_sampled_bytes += n;

                int percent = (int)((total_sampled_bytes * 100) / target_bytes);
                if (percent > 100) percent = 100;
                double checked_mb = (double)total_sampled_bytes / (1024.0 * 1024.0);

                mvwprintw(prog_win, 2, 13, "%3d%% complete", percent);
                mvwprintw(prog_win, 3, 13, "%.1fMB / %.1fMB", checked_mb, target_mb);
                wrefresh(prog_win);
            }

            if (total_sampled_bytes >= target_bytes) break;
        }

        free(indices);
        free(chunks);
    }

    free(sector_buf);
    delwin(prog_win);
    touchwin(stdscr);

    if (cancelled) {
        show_message_popup("Cancelled", "Entropy check cancelled by user.");
        return;
    }

    double entropy = 0.0;
    if (total_sampled_bytes > 0) {
        for (int i = 0; i < 256; i++) {
            if (byte_counts[i] > 0) {
                double p = (double)byte_counts[i] / (double)total_sampled_bytes;
                entropy -= p * (log(p) / 0.693147180559945319);
            }
        }
    }

    char title_buf[64];
    char msg_buf[256];
    snprintf(title_buf, sizeof(title_buf), " Entropy Analysis Result ");

    const char *status_desc = "Unknown";
    if (entropy < 0.1) {
        status_desc = "Plain Zeros (0x00) / Empty Space";
    } else if (entropy < 1.0) {
        status_desc = "Low Entropy / Repeating Pattern";
    } else if (entropy < 7.0) {
        status_desc = "Structured/Compressed Data / Standard Files";
    } else if (entropy >= 7.9) {
        status_desc = "Max Random Noise (NIST Wipe Verified!)";
    } else {
        status_desc = "High Randomness / Encrypted Data";
    }

    snprintf(msg_buf, sizeof(msg_buf), "Shannon Entropy: %.4f / 8.0\nSampled Size:    %.1f MB\nStatus:          %s",
             entropy, (double)total_sampled_bytes / (1024.0 * 1024.0), status_desc);

    show_message_popup(title_buf, msg_buf);
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
        lseek(drive->fd, drive->offset, SEEK_SET);
        ssize_t bytes_read = read(drive->fd, buffer, display_rows * bytes_per_row);
        (void)bytes_read;
    }

    wclear(win);
    wattron(win, COLOR_PAIR(CP_BORDER));
    box(win, 0, 0);
    wattroff(win, COLOR_PAIR(CP_BORDER));

    // Top Border Line: Device Name & Signature
    wattron(win, COLOR_PAIR(CP_TITLE) | A_BOLD);
    mvwprintw(win, 0, 2, " Hex View: %s [%s] ", drive->name, drive->signature);
    wattroff(win, COLOR_PAIR(CP_TITLE) | A_BOLD);

    // Bottom Border Line: Dynamic Block Number, Range, and Offset (adapts to width)
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

    int max_len = cols - 4; // Max width inside borders
    if (len_with_range > max_len) {
        snprintf(bottom_str, sizeof(bottom_str),
                 " Block: %llu | Offset: 0x%012lX ",
                 block_no,
                 (unsigned long)drive->offset);
    }

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

    for (int i = 0; i < display_rows; i++) {
        off_t row_addr = drive->offset + (i * bytes_per_row);
        if (row_addr >= drive->size) break;

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

        mvwprintw(win, i + 1, 2, "%012lX:  %-*s   %s",
                  row_addr,
                  bytes_per_row * 3,
                  hex_buf,
                  ascii_buf);
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
                {"Byte Units", "500K, 100M, 8GB, 1TB"}
            },
            2
        }
    };

    typedef struct {
        int type; // 0=blank, 1=header, 2=wide item, 3=narrow key, 4=narrow desc chunk
        char text1[64];
        char text2[128];
    } render_line_t;

    render_line_t rlines[128];
    int rline_count = 0;

    for (int s = 0; s < 4; s++) {
        if (s > 0 && rline_count < 128) {
            rlines[rline_count].type = 0; // blank
            rline_count++;
        }
        if (rline_count < 128) {
            rlines[rline_count].type = 1; // header
            snprintf(rlines[rline_count].text1, sizeof(rlines[rline_count].text1), "[ %s ]", secs[s].title);
            rline_count++;
        }

        for (int i = 0; i < secs[s].count && rline_count < 128; i++) {
            const char *k = secs[s].items[i].key;
            const char *d = secs[s].items[i].desc;

            // Breakpoint at 87 inner width (corresponds to 123 terminal columns)
            if (inner_width >= 87) {
                // Wide mode: single line
                rlines[rline_count].type = 2;
                snprintf(rlines[rline_count].text1, sizeof(rlines[rline_count].text1), "%s", k);
                snprintf(rlines[rline_count].text2, sizeof(rlines[rline_count].text2), "%s", d);
                rline_count++;
            } else {
                // Narrow mode (< 123 columns): Key on line 1, wrapped description on line(s) 2+
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

void close_drives(drive_info_t drives[], int count) {
    for (int i = 0; i < count; i++) {
        if (drives[i].fd >= 0) {
            close(drives[i].fd);
        }
    }
}

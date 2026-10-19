#include "shredos_hexviewer.h"

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
    int redraw_all = 1;   // Force initial full draw on startup
    int redraw_left = 0;
    int redraw_right = 0;

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
                redraw_all = 1; // Full redraw required due to drive hot-plug change
            }
        }

        // --- Selective UI Rendering ---
        if (redraw_all) {
            // Render Top Title Box
            wclear(win_title);
            wattron(win_title, COLOR_PAIR(CP_BORDER));
            box(win_title, 0, 0);
            wattroff(win_title, COLOR_PAIR(CP_BORDER));

            wattron(win_title, COLOR_PAIR(CP_TITLE) | A_BOLD);
            mvwprintw(win_title, 1, 2, " shredos_hexviewer v1.2");
            wattroff(win_title, COLOR_PAIR(CP_TITLE) | A_BOLD);
            wrefresh(win_title);

            // Render Frameless Help Bar on Bottom Line
            wclear(win_help);
            if (show_help) {
                mvwprintw(win_help, 0, 0, "   Up/Down/PgUp/PgDn=Scroll Help | h or Esc=Return to Hex View");
            } else {
                mvwprintw(win_help, 0, 0, "   r=Reload | g=goto | s=search | e=entropy | h=Help | PgUp/PgDn=Scroll | q=Quit");
            }
            wrefresh(win_help);

            redraw_left = 1;
            redraw_right = 1;
            redraw_all = 0;
        }

        if (redraw_left) {
            // Render Left Pane (Scrollable Drive List with Model / Serial)
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

                    // Line 2: Model / Serial (Safely truncated to prevent border overlap/wrapping)
                    if (current_row < main_height - 1) {
                        int max_len = left_width - 3; // Max interior columns
                        char display_buf[128];
                        snprintf(display_buf, sizeof(display_buf), "%s", drives[i].model_serial);

                        if ((int)strlen(display_buf) > max_len) {
                            display_buf[max_len - 3] = '\0';
                            strcat(display_buf, "...");
                        }

                        mvwprintw(win_left, current_row, 1, " %-*s", max_len, display_buf);
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
            redraw_left = 0;
        }

        if (redraw_right) {
            // Render Right Pane (Help View or Dynamic Hex & ASCII View)
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
            redraw_right = 0;
        }

        // --- Handle Input (Timed 250ms Poll) ---
        ch = wgetch(win_left);

        // If no key was pressed within 250ms, wgetch returns ERR -> loop skips and continues polling
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

            redraw_all = 1;
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
                    redraw_right = 1;
                break;
                case KEY_DOWN:
                    help_scroll_offset++;
                    redraw_right = 1;
                    break;
                case KEY_PPAGE:
                    help_scroll_offset -= rows_visible;
                    if (help_scroll_offset < 0) help_scroll_offset = 0;
                    redraw_right = 1;
                break;
                case KEY_NPAGE:
                    help_scroll_offset += rows_visible;
                    redraw_right = 1;
                    break;
                case 'h':
                case 'H':
                case 27: // ESC
                    show_help = 0;
                    redraw_all = 1; // Redraw all to restore help bar and main hex view
                    break;
            }
            continue;
        }

        switch (ch) {
            case KEY_DOWN:
                if (drive_count > 0 && selected_drive < drive_count - 1) {
                    selected_drive++;
                    redraw_left = 1;
                    redraw_right = 1;
                }
                break;

            case KEY_UP:
                if (drive_count > 0 && selected_drive > 0) {
                    selected_drive--;
                    redraw_left = 1;
                    redraw_right = 1;
                }
                break;

            case KEY_NPAGE: // Page Down
                if (drive_count > 0 && drives[selected_drive].offset + page_bytes < drives[selected_drive].size) {
                    drives[selected_drive].offset += page_bytes;
                    redraw_right = 1;
                }
                break;

            case KEY_PPAGE: // Page Up
                if (drive_count > 0 && drives[selected_drive].offset >= page_bytes) {
                    drives[selected_drive].offset -= page_bytes;
                    redraw_right = 1;
                } else if (drive_count > 0 && drives[selected_drive].offset > 0) {
                    drives[selected_drive].offset = 0;
                    redraw_right = 1;
                }
                break;

            case '<': // Start of drive
                if (drive_count > 0 && drives[selected_drive].offset > 0) {
                    drives[selected_drive].offset = 0;
                    redraw_right = 1;
                }
                break;

            case '>': // End of drive
                if (drive_count > 0) {
                    off_t new_offset = 0;
                    if (drives[selected_drive].size > page_bytes) {
                        new_offset = drives[selected_drive].size - page_bytes;
                    }
                    if (drives[selected_drive].offset != new_offset) {
                        drives[selected_drive].offset = new_offset;
                        redraw_right = 1;
                    }
                }
                break;

            case 'g':
            case 'G':
                if (drive_count > 0) {
                    drives[selected_drive].offset = handle_goto_popup(&drives[selected_drive], dynamic_bpr);
                    redraw_right = 1;
                }
                break;

            case 's':
            case 'S':
                if (drive_count > 0) {
                    drives[selected_drive].offset = handle_random_search(&drives[selected_drive], dynamic_bpr);
                    redraw_right = 1;
                }
                break;

            case 'e':
            case 'E':
                if (drive_count > 0) {
                    handle_entropy_menu(&drives[selected_drive], dynamic_bpr);
                    redraw_right = 1;
                }
                break;

            case 'h':
            case 'H':
                show_help = 1;
                help_scroll_offset = 0;
                redraw_all = 1; // Redraw all to update help bar and show help view
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
                redraw_all = 1;
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

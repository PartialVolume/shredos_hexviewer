#include "shredos_hexviewer.h"

#define SEARCH_BUF_SIZE (16 * 1024 * 1024) // 16MB high-throughput buffer matching nwipe

static void format_speed(double bytes_per_sec, char *buf, size_t size) {
    if (bytes_per_sec >= 1000.0 * 1000.0 * 1000.0) {
        snprintf(buf, size, "%.1f GB/s", bytes_per_sec / (1000.0 * 1000.0 * 1000.0));
    } else if (bytes_per_sec >= 1000.0 * 1000.0) {
        snprintf(buf, size, "%.1f MB/s", bytes_per_sec / (1000.0 * 1000.0));
    } else if (bytes_per_sec >= 1000.0) {
        snprintf(buf, size, "%.1f KB/s", bytes_per_sec / 1000.0);
    } else {
        snprintf(buf, size, "%.1f B/s", bytes_per_sec);
    }
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

        if (new_offset > drive->size) {
            char human_sz[32];
            char msg[256];
            format_human_size(drive->size, human_sz, sizeof(human_sz));
            snprintf(msg, sizeof(msg), "Entered offset (0x%012lX) exceeds drive size!\nMaximum drive size is 0x%012lX (%llu bytes / %s).",
                     (unsigned long)new_offset, (unsigned long)drive->size, (unsigned long long)drive->size, human_sz);
            show_message_popup("Invalid Offset", msg);
            delwin(pop);
            touchwin(stdscr);
            return drive->offset;
        }
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

        if (new_offset > drive->size) {
            char human_sz[32];
            char msg[256];
            format_human_size(drive->size, human_sz, sizeof(human_sz));
            unsigned long long max_blocks = (unsigned long long)(drive->size / 512);
            snprintf(msg, sizeof(msg), "Entered block number (%llu) exceeds drive capacity!\nMaximum block number is %llu (%s).",
                     block_num, max_blocks, human_sz);
            show_message_popup("Invalid Block Number", msg);
            delwin(pop);
            touchwin(stdscr);
            return drive->offset;
        }
    }

    delwin(pop);
    touchwin(stdscr);

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
        target_bytes = (unsigned long long)(numeric_val * 1000.0 * 1000.0 * 1000.0 * 1000.0);
    } else if (unit == 'G') {
        target_bytes = (unsigned long long)(numeric_val * 1000.0 * 1000.0 * 1000.0);
    } else if (unit == 'M') {
        target_bytes = (unsigned long long)(numeric_val * 1000.0 * 1000.0);
    } else if (unit == 'K') {
        target_bytes = (unsigned long long)(numeric_val * 1000.0);
    } else {
        target_bytes = (unsigned long long)numeric_val;
    }

    if (target_bytes == 0) target_bytes = (unsigned long long)bytes_per_row * 100;

    if (target_bytes > (unsigned long long)drive->size) {
        char human_sz[32];
        char msg[256];
        format_human_size(drive->size, human_sz, sizeof(human_sz));
        snprintf(msg, sizeof(msg), "Entered search scope exceeds drive size!\nMaximum drive size is 0x%012lX (%llu bytes / %s).\nPlease specify a valid scope.",
                 (unsigned long)drive->size, (unsigned long long)drive->size, human_sz);
        show_message_popup("Invalid Scope", msg);
        return drive->offset;
    }

    int is_full_scan = (unit == '%' && numeric_val >= 99.99) || (target_bytes >= (unsigned long long)drive->size);

    return execute_search(drive, bytes_per_row, search_type, hex_pattern, ascii_pattern, pat_len, target_bytes, is_full_scan, 0, 0);
}

off_t execute_search(drive_info_t *drive, int bytes_per_row, search_type_t search_type, search_pattern_element_t *hex_pattern, unsigned char *ascii_pattern, int pat_len, unsigned long long target_bytes, int is_full_scan, off_t start_offset, int is_find_next) {
    double target_mb = (double)target_bytes / (1000.0 * 1000.0);
    if (target_mb < 0.1) target_mb = 0.1;

    size_t chunk_buf_size = SEARCH_BUF_SIZE; // 16MB
    int found = 0;
    int cancelled = 0;
    off_t found_offset = start_offset;

    unsigned char *io_buf = malloc(chunk_buf_size);
    if (!io_buf) {
        show_message_popup("Error", "Memory allocation failed for high-throughput search buffer.");
        return drive->offset;
    }

    unsigned char *search_buf = malloc(chunk_buf_size + 128);
    if (!search_buf) {
        free(io_buf);
        show_message_popup("Error", "Memory allocation failed for search boundary buffer.");
        return drive->offset;
    }

    int prog_h = 9;
    int prog_w = 64;
    int prog_y = (LINES - prog_h) / 2;
    int prog_x = (COLS - prog_w) / 2;
    WINDOW *prog_win = newwin(prog_h, prog_w, prog_y, prog_x);
    keypad(prog_win, TRUE);
    wtimeout(prog_win, 0);

    box(prog_win, 0, 0);
    wattron(prog_win, COLOR_PAIR(CP_TITLE) | A_BOLD);
    mvwprintw(prog_win, 0, 2, is_full_scan ? " Full Sequential Search (Buffered) " : " 1000-Chunk Scatter Search ");
    wattroff(prog_win, COLOR_PAIR(CP_TITLE) | A_BOLD);
    mvwprintw(prog_win, 2, 3, "Progress:   0%% complete");
    mvwprintw(prog_win, 3, 3, "Checked:    0.0MB / %.1fMB", target_mb);
    mvwprintw(prog_win, 4, 3, "Speed:      0.0 B/s");
    mvwprintw(prog_win, 6, 3, "Press ESC to cancel search");
    wrefresh(prog_win);

    struct timespec last_speed_update;
    clock_gettime(CLOCK_MONOTONIC, &last_speed_update);

    unsigned long long bytes_since_last_update = 0;
    char speed_buf[32] = "0.0 B/s";

    if (is_full_scan && !is_find_next) {
        // --- HIGH-SPEED OPTIMIZED 16MB FAST PATH WITH HORSPOOL & MEMCMP ---
        unsigned long long bytes_remaining = target_bytes;
        off_t current_offset = start_offset;
        unsigned long long total_bytes_read = 0;

        unsigned char history[64] = {0};
        int history_len = 0;

        while (bytes_remaining > 0 && !found && !cancelled) {
            size_t to_read = (bytes_remaining > chunk_buf_size) ? chunk_buf_size : (size_t)bytes_remaining;
            if (current_offset + (off_t)to_read > drive->size) {
                to_read = (size_t)(drive->size - current_offset);
            }
            if (to_read == 0) break;

            ssize_t n = safe_read(drive->fd, io_buf, to_read, drive->name, current_offset);
            if (n <= 0) {
                cancelled = 1;
                break;
            }

            int effective_history_len = history_len;
            if (effective_history_len > (int)n) effective_history_len = (int)n;

            memcpy(search_buf, history, effective_history_len);
            memcpy(search_buf + effective_history_len, io_buf, n);
            int total_len = effective_history_len + (int)n;

            if (search_type == SEARCH_NON_ZERO) {
                for (int idx = 0; idx < total_len; idx++) {
                    off_t byte_off = current_offset - effective_history_len + idx;
                    if (byte_off >= 0 && search_buf[idx] != 0x00) {
                        found = 1;
                        found_offset = byte_off;
                        break;
                    }
                }
            } else if (search_type == SEARCH_NON_FF) {
                for (int idx = 0; idx < total_len; idx++) {
                    off_t byte_off = current_offset - effective_history_len + idx;
                    if (byte_off >= 0 && search_buf[idx] != 0xFF) {
                        found = 1;
                        found_offset = byte_off;
                        break;
                    }
                }
            } else if (search_type == SEARCH_ASCII_PATTERN || search_type == SEARCH_ASCII_PATTERN_CI) {
                int case_insensitive = (search_type == SEARCH_ASCII_PATTERN_CI);

                // Precompute Boyer-Moore-Horspool bad-character shift table
                int bad_char_skip[256];
                for (int i = 0; i < 256; i++) {
                    bad_char_skip[i] = pat_len;
                }
                for (int i = 0; i < pat_len - 1; i++) {
                    unsigned char c = ascii_pattern[i];
                    bad_char_skip[c] = pat_len - 1 - i;
                    if (case_insensitive) {
                        bad_char_skip[tolower(c)] = pat_len - 1 - i;
                        bad_char_skip[toupper(c)] = pat_len - 1 - i;
                    }
                }

                int idx = 0;
                while (idx <= total_len - pat_len) {
                    off_t byte_off = current_offset - effective_history_len + idx;

                    // Match check from right to left (Horspool optimization)
                    int match = 1;
                    for (int p = pat_len - 1; p >= 0; p--) {
                        unsigned char disk_b = search_buf[idx + p];
                        unsigned char pat_b = ascii_pattern[p];
                        if (case_insensitive) {
                            if (tolower(disk_b) != pat_b) {
                                match = 0;
                                break;
                            }
                        } else {
                            if (disk_b != pat_b) {
                                match = 0;
                                break;
                            }
                        }
                    }

                    if (match) {
                        if (byte_off >= 0) {
                            found = 1;
                            found_offset = byte_off;
                            break;
                        }
                    }

                    // Shift window forward using bad-character table (skips non-matching blocks instantly)
                    unsigned char last_char = search_buf[idx + pat_len - 1];
                    idx += bad_char_skip[case_insensitive ? tolower(last_char) : last_char];
                }
            } else if (search_type == SEARCH_HEX_PATTERN) {
                int has_first_val = !hex_pattern[0].is_wildcard;
                unsigned char first_val = has_first_val ? hex_pattern[0].value : 0;
                int max_idx = total_len - pat_len + 1;
                for (int idx = 0; idx < max_idx; idx++) {
                    off_t byte_off = current_offset - effective_history_len + idx;
                    if (byte_off < 0) continue;

                    if (!has_first_val || search_buf[idx] == first_val) {
                        int p_match = 1;
                        int start_p = has_first_val ? 1 : 0;
                        for (int p = start_p; p < pat_len; p++) {
                            if (!hex_pattern[p].is_wildcard) {
                                if (search_buf[idx + p] != hex_pattern[p].value) {
                                    p_match = 0;
                                    break;
                                }
                            }
                        }
                        if (p_match) {
                            found = 1;
                            found_offset = byte_off;
                            break;
                        }
                    }
                }
            }

            if (search_type == SEARCH_HEX_PATTERN || search_type == SEARCH_ASCII_PATTERN || search_type == SEARCH_ASCII_PATTERN_CI) {
                int keep = pat_len - 1;
                if (keep > n) keep = n;
                if (keep > 0) {
                    memcpy(history, io_buf + n - keep, keep);
                    history_len = keep;
                } else {
                    history_len = 0;
                }
            }

            total_bytes_read += n;
            bytes_since_last_update += n;
            current_offset += n;
            bytes_remaining -= n;

            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            double interval_sec = (now.tv_sec - last_speed_update.tv_sec) + (now.tv_nsec - last_speed_update.tv_nsec) / 1e9;

            if (interval_sec >= 0.25) {
                int ch = wgetch(prog_win);
                if (ch == 27 || ch == 'q' || ch == 'Q') {
                    cancelled = 1;
                    break;
                }

                if (interval_sec > 0.0) {
                    double current_speed = (double)bytes_since_last_update / interval_sec;
                    format_speed(current_speed, speed_buf, sizeof(speed_buf));
                }
                last_speed_update = now;
                bytes_since_last_update = 0;

                int percent = (int)((total_bytes_read * 100) / target_bytes);
                if (percent > 100) percent = 100;
                double checked_mb = (double)total_bytes_read / (1000.0 * 1000.0);

                mvwprintw(prog_win, 2, 13, "%3d%% complete", percent);
                mvwprintw(prog_win, 3, 13, "%.1fMB / %.1fMB", checked_mb, target_mb);
                mvwprintw(prog_win, 4, 13, "%-15s", speed_buf);
                wrefresh(prog_win);
            }
        }
    } else {
        // --- STANDARD GUARDED PATH FOR SCATTER & FIND-NEXT SCANS ---
        unsigned long long bytes_remaining = target_bytes;
        off_t current_offset = start_offset;
        unsigned long long total_bytes_read = 0;

        unsigned char history[64] = {0};
        int history_len = 0;

        while (bytes_remaining > 0 && !found && !cancelled) {
            if (current_offset >= drive->size) break;

            size_t to_read = (bytes_remaining > chunk_buf_size) ? chunk_buf_size : (size_t)bytes_remaining;
            if (current_offset + (off_t)to_read > drive->size) {
                to_read = (size_t)(drive->size - current_offset);
            }
            if (to_read == 0) break;

            ssize_t n = safe_read(drive->fd, io_buf, to_read, drive->name, current_offset);
            if (n <= 0) {
                cancelled = 1;
                break;
            }

            int effective_history_len = history_len;
            if (effective_history_len > (int)n) effective_history_len = (int)n;

            memcpy(search_buf, history, effective_history_len);
            memcpy(search_buf + effective_history_len, io_buf, n);
            int total_len = effective_history_len + (int)n;

            for (int idx = 0; idx < total_len; idx++) {
                off_t current_byte_offset = current_offset - effective_history_len + idx;
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
                    memcpy(history, io_buf + n - keep, keep);
                    history_len = keep;
                } else {
                    history_len = 0;
                }
            }

            total_bytes_read += n;
            bytes_since_last_update += n;
            current_offset += n;
            bytes_remaining -= n;

            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            double interval_sec = (now.tv_sec - last_speed_update.tv_sec) + (now.tv_nsec - last_speed_update.tv_nsec) / 1e9;

            if (interval_sec >= 0.25) {
                int ch = wgetch(prog_win);
                if (ch == 27 || ch == 'q' || ch == 'Q') {
                    cancelled = 1;
                    break;
                }

                if (interval_sec > 0.0) {
                    double current_speed = (double)bytes_since_last_update / interval_sec;
                    format_speed(current_speed, speed_buf, sizeof(speed_buf));
                }
                last_speed_update = now;
                bytes_since_last_update = 0;

                int percent = (int)((total_bytes_read * 100) / target_bytes);
                if (percent > 100) percent = 100;
                double checked_mb = (double)total_bytes_read / (1000.0 * 1000.0);

                mvwprintw(prog_win, 2, 13, "%3d%% complete", percent);
                mvwprintw(prog_win, 3, 13, "%.1fMB / %.1fMB", checked_mb, target_mb);
                mvwprintw(prog_win, 4, 13, "%-15s", speed_buf);
                wrefresh(prog_win);
            }
        }
    }

    free(search_buf);
    free(io_buf);
    delwin(prog_win);
    touchwin(stdscr);

    if (cancelled) {
        show_message_popup("Cancelled", "Search cancelled by user or aborted due to I/O error.");
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

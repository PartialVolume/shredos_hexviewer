#include "shredos_hexviewer.h"

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

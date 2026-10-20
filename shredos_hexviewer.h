/* This MUST come before system includes and guard */
#define _FILE_OFFSET_BITS 64
#ifndef SHREDOS_HEXVIEWER_H
#define SHREDOS_HEXVIEWER_H

#define _GNU_SOURCE
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
#include <errno.h>

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
void format_human_size(off_t bytes, char *buf, size_t buf_size);
void format_compact_size(off_t bytes, char *buf, size_t buf_size);
int parse_hex_pattern(const char *str, search_pattern_element_t *pattern, int max_len);
int parse_ascii_pattern(const char *str, unsigned char *buf, int max_len, int case_insensitive);
int is_valid_word(const unsigned char *word_buf, int len);
ssize_t safe_read(int fd, void *buf, size_t count, const char *dev_name, off_t offset);

#endif

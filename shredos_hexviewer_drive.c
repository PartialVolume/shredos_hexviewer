#include "shredos_hexviewer.h"

void get_model_and_serial(const char *dev_name, char *dest, size_t max_len, int quiet) {
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

    if (quiet) {
        for (int i = 0; serial[i] != '\0'; i++) {
            serial[i] = 'x';
        }
    }

    snprintf(dest, max_len, "%s / %s", model, serial);
}

// Helper structures for partition parsing
typedef struct {
    uint64_t start_lba;
    uint64_t end_lba;
} gpt_entry_info_t;

typedef struct {
    uint32_t start_lba;
    uint32_t sector_count;
    unsigned char type;
} mbr_entry_info_t;

static int parse_gpt_entries(int fd, gpt_entry_info_t *entries, int max_entries) {
    unsigned char header_buf[512];
    if (lseek(fd, 512, SEEK_SET) != 512) return 0;
    if (read(fd, header_buf, 512) != 512) return 0;

    if (memcmp(header_buf, "EFI PART", 8) != 0) return 0;

    uint32_t partition_entries_lba = *(uint32_t *)&header_buf[72];
    uint32_t num_entries           = *(uint32_t *)&header_buf[80];
    uint32_t entry_size            = *(uint32_t *)&header_buf[84];

    if (entry_size < 128) entry_size = 128;

    off_t table_offset = (off_t)partition_entries_lba * 512;
    int count = 0;

    for (uint32_t i = 0; i < num_entries && count < max_entries; i++) {
        unsigned char entry_buf[128];
        off_t e_off = table_offset + (i * entry_size);

        if (lseek(fd, e_off, SEEK_SET) != e_off) break;
        if (read(fd, entry_buf, 128) != 128) break;

        uint64_t g1 = *(uint64_t *)&entry_buf[0];
        uint64_t g2 = *(uint64_t *)&entry_buf[8];
        if (g1 == 0 && g2 == 0) continue;

        uint64_t start_lba = *(uint64_t *)&entry_buf[32];
        uint64_t end_lba   = *(uint64_t *)&entry_buf[40];

        if (start_lba > 0) {
            entries[count].start_lba = start_lba;
            entries[count].end_lba   = end_lba;
            count++;
        }
    }
    return count;
}

static int parse_mbr_entries(int fd, mbr_entry_info_t *entries, int max_entries) {
    unsigned char sector[512];
    if (lseek(fd, 0, SEEK_SET) != 0) return 0;
    if (read(fd, sector, 512) != 512) return 0;

    if (sector[510] != 0x55 || sector[511] != 0xAA) return 0;

    int count = 0;
    for (int i = 0; i < 4 && count < max_entries; i++) {
        int offset = 446 + (i * 16);
        unsigned char type = sector[offset + 4];
        uint32_t start_lba = *(uint32_t *)&sector[offset + 8];
        uint32_t sectors   = *(uint32_t *)&sector[offset + 12];

        if (sectors == 0) continue;

        entries[count].start_lba = start_lba;
        entries[count].sector_count = sectors;
        entries[count].type = type;
        count++;
    }
    return count;
}

static const char* probe_filesystem_at_offset(int fd, off_t base_offset) {
    unsigned char sector[1024];

    if (lseek(fd, base_offset, SEEK_SET) == base_offset) {
        if (read(fd, sector, 512) == 512) {
            if (memcmp(sector + 3, "NTFS    ", 8) == 0) return "NTFS";
            if (memcmp(sector + 0x52, "FAT32   ", 8) == 0) return "FAT32";
            if (memcmp(sector + 0x26, "FAT", 3) == 0) return "FAT";
            if (memcmp(sector + 1, "CD001", 5) == 0) return "ISO9660";
        }
    }

    if (lseek(fd, base_offset + 1024, SEEK_SET) == (base_offset + 1024)) {
        if (read(fd, sector, 512) == 512) {
            if (sector[56] == 0x53 && sector[57] == 0xEF) {
                return "ext4";
            }
        }
    }

    return NULL;
}

void detect_drive_signature(drive_info_t *drive) {
    drive->partition_count = 0;

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

    // 1. Validate GPT signature
    int has_protective_mbr = (sector_buf[450] == 0xEE);
    int is_gpt = (n >= 1024 && memcmp(sector_buf + 512, "EFI PART", 8) == 0 && has_protective_mbr);

    if (is_gpt) {
        gpt_entry_info_t gpt_parts[16];
        int part_count = parse_gpt_entries(drive->fd, gpt_parts, 16);

        char found_fs[4][16];
        int fs_count = 0;

        for (int p = 0; p < part_count && drive->partition_count < 16; p++) {
            off_t part_offset = (off_t)gpt_parts[p].start_lba * 512;
            uint64_t part_size = (gpt_parts[p].end_lba >= gpt_parts[p].start_lba) ?
            (gpt_parts[p].end_lba - gpt_parts[p].start_lba + 1) * 512 : 0;

            const char *fs = probe_filesystem_at_offset(drive->fd, part_offset);

            char dev_base[64];
            snprintf(dev_base, sizeof(dev_base), "%.63s", drive->name);

            snprintf(drive->partitions[drive->partition_count].name,
                     sizeof(drive->partitions[drive->partition_count].name),
                     "%.48s%d", dev_base, drive->partition_count + 1);
            drive->partitions[drive->partition_count].size = part_size;
            snprintf(drive->partitions[drive->partition_count].fs_type,
                     sizeof(drive->partitions[drive->partition_count].fs_type),
                     "%s", fs ? fs : "Unknown");

            drive->partition_count++;

            if (fs) {
                int exists = 0;
                for (int k = 0; k < fs_count; k++) {
                    if (strcmp(found_fs[k], fs) == 0) { exists = 1; break; }
                }
                if (!exists && fs_count < 4) {
                    snprintf(found_fs[fs_count], sizeof(found_fs[fs_count]), "%s", fs);
                    fs_count++;
                }
            }
        }

        if (fs_count == 1) {
            snprintf(drive->signature, sizeof(drive->signature), "GPT + %s", found_fs[0]);
        } else if (fs_count > 1) {
            char combined_fs[64] = "";
            for (int k = 0; k < fs_count; k++) {
                if (k > 0) strncat(combined_fs, ", ", sizeof(combined_fs) - strlen(combined_fs) - 1);
                strncat(combined_fs, found_fs[k], sizeof(combined_fs) - strlen(combined_fs) - 1);
            }
            snprintf(drive->signature, sizeof(drive->signature), "GPT: %s", combined_fs);
        } else {
            snprintf(drive->signature, sizeof(drive->signature), "GPT");
        }
        return;
    }

    // 2. Check MBR partition table
    int has_mbr = (sector_buf[510] == 0x55 && sector_buf[511] == 0xAA);
    if (has_mbr) {
        mbr_entry_info_t mbr_parts[4];
        int part_count = parse_mbr_entries(drive->fd, mbr_parts, 4);

        char found_fs[4][16];
        int fs_count = 0;

        for (int p = 0; p < part_count && drive->partition_count < 16; p++) {
            off_t part_offset = (off_t)mbr_parts[p].start_lba * 512;
            uint64_t part_size = (uint64_t)mbr_parts[p].sector_count * 512;

            const char *fs = probe_filesystem_at_offset(drive->fd, part_offset);

            char dev_base[64];
            snprintf(dev_base, sizeof(dev_base), "%.63s", drive->name);

            snprintf(drive->partitions[drive->partition_count].name,
                     sizeof(drive->partitions[drive->partition_count].name),
                     "%.48s%d", dev_base, drive->partition_count + 1);
            drive->partitions[drive->partition_count].size = part_size;

            const char *label = fs;
            if (!label) {
                if (mbr_parts[p].type == 0xef) label = "EFI";
                else if (mbr_parts[p].type == 0x00) label = "ISO9660";
                else label = "Unknown";
            }

            snprintf(drive->partitions[drive->partition_count].fs_type,
                     sizeof(drive->partitions[drive->partition_count].fs_type),
                     "%s", label);

            drive->partition_count++;

            if (fs) {
                int exists = 0;
                for (int k = 0; k < fs_count; k++) {
                    if (strcmp(found_fs[k], fs) == 0) { exists = 1; break; }
                }
                if (!exists && fs_count < 4) {
                    snprintf(found_fs[fs_count], sizeof(found_fs[fs_count]), "%s", fs);
                    fs_count++;
                }
            }
        }

        const char *direct_fs = probe_filesystem_at_offset(drive->fd, 0);
        if (direct_fs) {
            snprintf(drive->signature, sizeof(drive->signature), "MBR + %s", direct_fs);
        } else if (fs_count > 0) {
            snprintf(drive->signature, sizeof(drive->signature), "MBR (%d partitions)", drive->partition_count);
        } else {
            snprintf(drive->signature, sizeof(drive->signature), "MBR");
        }
        return;
    }

    // 3. Check direct whole-disk filesystems or ISO9660
    const char *direct_fs = probe_filesystem_at_offset(drive->fd, 0);
    if (direct_fs) {
        snprintf(drive->signature, sizeof(drive->signature), "%s", direct_fs);
        return;
    }

    unsigned char iso_buf[5];
    if (lseek(drive->fd, 32768, SEEK_SET) == 32768 && read(drive->fd, iso_buf, 5) == 5) {
        if (memcmp(iso_buf, "CD001", 5) == 0) {
            snprintf(drive->signature, sizeof(drive->signature), "ISO9660");
            return;
        }
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

int get_drive_list(drive_info_t drives[], int max_drives, int quiet) {
    DIR *dir = opendir("/sys/block");
    if (!dir) return 0;

    int count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && count < max_drives) {
        if (entry->d_name[0] == '.') continue;

        if (strncmp(entry->d_name, "loop", 4) == 0) continue;
        if (strncmp(entry->d_name, "ram", 3) == 0) continue;
        if (strncmp(entry->d_name, "sr", 2) == 0) continue;

        char path[MAX_PATH];
        snprintf(path, sizeof(path), "/dev/%s", entry->d_name);

        int fd = open(path, O_RDONLY);
        if (fd < 0) continue;

        size_t name_len = strlen(entry->d_name);
        if (name_len >= sizeof(drives[count].name)) {
            name_len = sizeof(drives[count].name) - 1;
        }
        memcpy(drives[count].name, entry->d_name, name_len);
        drives[count].name[name_len] = '\0';

        size_t path_len = strlen(path);
        if (path_len >= sizeof(drives[count].path)) {
            path_len = sizeof(drives[count].path) - 1;
        }
        memcpy(drives[count].path, path, path_len);
        drives[count].path[path_len] = '\0';

        drives[count].offset = 0;
        drives[count].fd = fd;
        drives[count].last_search_valid = 0;

        get_model_and_serial(entry->d_name, drives[count].model_serial, sizeof(drives[count].model_serial), quiet);
        detect_drive_signature(&drives[count]);

        if (ioctl(fd, BLKGETSIZE64, &drives[count].size) < 0) {
            drives[count].size = lseek(fd, 0, SEEK_END);
        }
        count++;
    }
    closedir(dir);
    return count;
}

void close_drives(drive_info_t drives[], int count) {
    for (int i = 0; i < count; i++) {
        if (drives[i].fd >= 0) {
            close(drives[i].fd);
        }
    }
}

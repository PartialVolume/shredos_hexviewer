#include "shredos_hexviewer.h"

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

// Helper structure to track parsed GPT partitions
typedef struct {
    uint64_t start_lba;
    uint64_t end_lba;
} gpt_entry_info_t;

static int parse_gpt_entries(int fd, gpt_entry_info_t *entries, int max_entries) {
    unsigned char header_buf[512];
    if (lseek(fd, 512, SEEK_SET) != 512) return 0;
    if (read(fd, header_buf, 512) != 512) return 0;

    if (memcmp(header_buf, "EFI PART", 8) != 0) return 0; // Not GPT

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

        // Check type GUID (bytes 0-15). All zeros means unused slot.
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

static const char* probe_filesystem_at_offset(int fd, off_t base_offset) {
    unsigned char sector[1024];

    // Check Sector 0 of partition / image
    if (lseek(fd, base_offset, SEEK_SET) == base_offset) {
        if (read(fd, sector, 512) == 512) {
            if (memcmp(sector + 3, "NTFS    ", 8) == 0) return "NTFS";
            if (memcmp(sector + 0x52, "FAT32   ", 8) == 0) return "FAT32";
            if (memcmp(sector + 0x26, "FAT", 3) == 0) return "FAT";
        }
    }

    // Check ext2/3/4 superblock (1024 bytes offset from partition start)
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
    int is_gpt = (n >= 1024 && memcmp(sector_buf + 512, "EFI PART", 8) == 0);
    if (is_gpt) {
        gpt_entry_info_t gpt_parts[8];
        int part_count = parse_gpt_entries(drive->fd, gpt_parts, 8);

        char found_fs[4][16];
        int fs_count = 0;

        for (int p = 0; p < part_count && fs_count < 4; p++) {
            off_t part_offset = (off_t)gpt_parts[p].start_lba * 512;
            const char *fs = probe_filesystem_at_offset(drive->fd, part_offset);
            if (fs) {
                int exists = 0;
                for (int k = 0; k < fs_count; k++) {
                    if (strcmp(found_fs[k], fs) == 0) { exists = 1; break; }
                }
                if (!exists) {
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

    // 2. Check MBR boot signature (0x55AA at offset 510-511)
    int has_mbr = (sector_buf[510] == 0x55 && sector_buf[511] == 0xAA);

    // 3. Probe direct whole-disk filesystems (unpartitioned or MBR offset 0)
    const char *direct_fs = probe_filesystem_at_offset(drive->fd, 0);
    if (direct_fs) {
        if (has_mbr) {
            snprintf(drive->signature, sizeof(drive->signature), "MBR + %s", direct_fs);
        } else {
            snprintf(drive->signature, sizeof(drive->signature), "%s", direct_fs);
        }
        return;
    }

    // 4. Check ISO9660 (CD001 signature at offset 32768)
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

void close_drives(drive_info_t drives[], int count) {
    for (int i = 0; i < count; i++) {
        if (drives[i].fd >= 0) {
            close(drives[i].fd);
        }
    }
}

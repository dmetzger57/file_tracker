#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <locale.h>
#include <signal.h>

#define MAX_PATH 4096
#define MAX_DESC 1024
#define MAX_CONTAINER 256

// ==== Terminal Wrapping Control ====
void enable_line_wrap(void) {
    printf("\033[?7h");
    fflush(stdout);
}

void disable_line_wrap(void) {
    printf("\033[?7l");
    fflush(stdout);
}

void signal_handler(int signum) {
    enable_line_wrap();
    signal(signum, SIG_DFL);
    raise(signum);
}

void print_usage(const char *prog_name) {
    fprintf(stderr, "Usage: %s <command> [options]\n\n", prog_name);
    fprintf(stderr, "Commands:\n");
    fprintf(stderr, "  add <drive_name> [-d description] [-c container]\n");
    fprintf(stderr, "      Add a new drive to tracking. If mounted, auto-detects capacity.\n");
    fprintf(stderr, "      -d    Description of drive purpose/contents\n");
    fprintf(stderr, "      -c    Storage container location\n\n");
    fprintf(stderr, "  show <drive_name>\n");
    fprintf(stderr, "      Show detailed information for a specific drive\n\n");
    fprintf(stderr, "  search <keyword>\n");
    fprintf(stderr, "      Search for drives by keyword in description\n\n");
    fprintf(stderr, "  list\n");
    fprintf(stderr, "      List all tracked drives\n\n");
    fprintf(stderr, "  update <drive_name> [-d description] [-c container]\n");
    fprintf(stderr, "      Update drive information (refreshes capacity if mounted)\n\n");
    fprintf(stderr, "  verify <drive_name>\n");
    fprintf(stderr, "      Mark drive as verified (updates last_verified timestamp)\n\n");
    fprintf(stderr, "  delete <drive_name>\n");
    fprintf(stderr, "      Delete a drive from tracking (requires confirmation)\n\n");
    fprintf(stderr, "Examples:\n");
    fprintf(stderr, "  %s add 'BackupDrive2024' -d 'Time Machine backups' -c 'Drawer A'\n", prog_name);
    fprintf(stderr, "  %s show BackupDrive2024\n", prog_name);
    fprintf(stderr, "  %s search backup\n", prog_name);
    fprintf(stderr, "  %s list\n", prog_name);
}

// Get current timestamp in ISO format
void get_timestamp(char *buffer, size_t size) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    strftime(buffer, size, "%Y-%m-%d %H:%M:%S", t);
}

// Format bytes to human-readable format
void format_bytes(long long bytes, char *buffer, size_t size) {
    const char *units[] = {"B", "KB", "MB", "GB", "TB", "PB"};
    int unit_index = 0;
    double size_val = (double)bytes;

    while (size_val >= 1024.0 && unit_index < 5) {
        size_val /= 1024.0;
        unit_index++;
    }

    if (unit_index == 0) {
        snprintf(buffer, size, "%lld %s", bytes, units[unit_index]);
    } else {
        snprintf(buffer, size, "%.2f %s", size_val, units[unit_index]);
    }
}

// Get drive statistics if mounted
int get_drive_stats(const char *mount_path, long long *capacity, long long *available, long long *used) {
    struct statfs fs_stats;

    if (statfs(mount_path, &fs_stats) != 0) {
        return 0; // Drive not mounted or error
    }

    *capacity = (long long)fs_stats.f_blocks * fs_stats.f_bsize;
    *available = (long long)fs_stats.f_bavail * fs_stats.f_bsize;
    *used = *capacity - ((long long)fs_stats.f_bfree * fs_stats.f_bsize);

    return 1;
}

// Find mount point for a drive name
int find_mount_point(const char *drive_name, char *mount_path, size_t size) {
    // Common mount point locations on macOS
    snprintf(mount_path, size, "/Volumes/%s", drive_name);

    struct stat st;
    if (stat(mount_path, &st) == 0) {
        return 1;
    }

    return 0;
}

// Initialize database with drives table
int init_database(sqlite3 *db) {
    char *err_msg = NULL;
    const char *sql =
        "CREATE TABLE IF NOT EXISTS drives ("
        "  drive_id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  drive_name TEXT UNIQUE NOT NULL,"
        "  capacity INTEGER,"
        "  space_available INTEGER,"
        "  space_used INTEGER,"
        "  description TEXT,"
        "  last_updated TEXT,"
        "  last_verified TEXT,"
        "  storage_container TEXT"
        ");";

    int rc = sqlite3_exec(db, sql, 0, 0, &err_msg);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQL error: %s\n", err_msg);
        sqlite3_free(err_msg);
        return 0;
    }

    return 1;
}

// Add a new drive
int cmd_add_drive(sqlite3 *db, const char *drive_name, const char *description, const char *container) {
    char timestamp[64];
    get_timestamp(timestamp, sizeof(timestamp));

    // Try to get drive stats if mounted
    char mount_path[MAX_PATH];
    long long capacity = 0, available = 0, used = 0;
    int mounted = 0;

    if (find_mount_point(drive_name, mount_path, sizeof(mount_path))) {
        mounted = get_drive_stats(mount_path, &capacity, &available, &used);
    }

    const char *sql = "INSERT INTO drives (drive_name, capacity, space_available, space_used, description, last_updated, storage_container) "
                      "VALUES (?, ?, ?, ?, ?, ?, ?);";

    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, 0);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Failed to prepare statement: %s\n", sqlite3_errmsg(db));
        return 0;
    }

    sqlite3_bind_text(stmt, 1, drive_name, -1, SQLITE_STATIC);
    if (mounted) {
        sqlite3_bind_int64(stmt, 2, capacity);
        sqlite3_bind_int64(stmt, 3, available);
        sqlite3_bind_int64(stmt, 4, used);
    } else {
        sqlite3_bind_null(stmt, 2);
        sqlite3_bind_null(stmt, 3);
        sqlite3_bind_null(stmt, 4);
    }
    sqlite3_bind_text(stmt, 5, description ? description : "", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 6, timestamp, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 7, container ? container : "", -1, SQLITE_STATIC);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        fprintf(stderr, "Failed to add drive: %s\n", sqlite3_errmsg(db));
        return 0;
    }

    printf("Drive '%s' added successfully.\n", drive_name);
    if (mounted) {
        char cap_str[64], avail_str[64], used_str[64];
        format_bytes(capacity, cap_str, sizeof(cap_str));
        format_bytes(available, avail_str, sizeof(avail_str));
        format_bytes(used, used_str, sizeof(used_str));
        printf("  Capacity: %s\n", cap_str);
        printf("  Available: %s\n", avail_str);
        printf("  Used: %s\n", used_str);
    } else {
        printf("  (Drive not currently mounted - capacity info not available)\n");
    }

    return 1;
}

// Query file_tracker database for last checksum date
void get_last_checksum_date(const char *drive_name, char *result, size_t size) {
    const char *home = getenv("HOME");
    if (!home) {
        result[0] = '\0';
        return;
    }

    // Try to find matching file_tracker database
    char db_path[MAX_PATH];
    snprintf(db_path, sizeof(db_path), "%s/db/FileTracker/%s.db", home, drive_name);

    sqlite3 *ft_db = NULL;
    if (sqlite3_open(db_path, &ft_db) != SQLITE_OK) {
        result[0] = '\0';
        if (ft_db) sqlite3_close(ft_db);
        return;
    }

    // Query most recent checksum verification date
    const char *sql = "SELECT last_checksum_verify_date FROM meta WHERE last_checksum_verify_date IS NOT NULL AND last_checksum_verify_date != '' ORDER BY id DESC LIMIT 1;";

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(ft_db, sql, -1, &stmt, 0) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *date = (const char *)sqlite3_column_text(stmt, 0);
            if (date) {
                strncpy(result, date, size - 1);
                result[size - 1] = '\0';
            } else {
                result[0] = '\0';
            }
        } else {
            result[0] = '\0';
        }
        sqlite3_finalize(stmt);
    } else {
        result[0] = '\0';
    }

    sqlite3_close(ft_db);
}

// Show single drive details
int cmd_show_drive(sqlite3 *db, const char *drive_name) {
    const char *sql = "SELECT * FROM drives WHERE drive_name = ?;";

    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, 0);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Failed to prepare statement: %s\n", sqlite3_errmsg(db));
        return 0;
    }

    sqlite3_bind_text(stmt, 1, drive_name, -1, SQLITE_STATIC);

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        printf("Drive '%s' not found.\n", drive_name);
        sqlite3_finalize(stmt);
        return 0;
    }

    // Extract data
    const char *name = (const char *)sqlite3_column_text(stmt, 1);
    long long capacity = sqlite3_column_int64(stmt, 2);
    long long available = sqlite3_column_int64(stmt, 3);
    long long used = sqlite3_column_int64(stmt, 4);
    const char *description = (const char *)sqlite3_column_text(stmt, 5);
    const char *last_updated = (const char *)sqlite3_column_text(stmt, 6);
    const char *last_verified = (const char *)sqlite3_column_text(stmt, 7);
    const char *container = (const char *)sqlite3_column_text(stmt, 8);

    // Print formatted output
    printf("\n========================================\n");
    printf("Drive Information\n");
    printf("========================================\n");
    printf("Drive Name:         %s\n", name);

    if (capacity > 0) {
        char cap_str[64], avail_str[64], used_str[64];
        format_bytes(capacity, cap_str, sizeof(cap_str));
        format_bytes(available, avail_str, sizeof(avail_str));
        format_bytes(used, used_str, sizeof(used_str));
        double pct_used = (double)used / capacity * 100.0;

        printf("Capacity:           %s (%'lld bytes)\n", cap_str, capacity);
        printf("Used:               %s (%.1f%%)\n", used_str, pct_used);
        printf("Available:          %s\n", avail_str);
    } else {
        printf("Capacity:           Not available\n");
    }

    printf("Description:        %s\n", description ? description : "(none)");
    printf("Storage Container:  %s\n", container ? container : "(none)");
    printf("Last Updated:       %s\n", last_updated ? last_updated : "(never)");
    printf("Last Verified:      %s\n", last_verified ? last_verified : "(never)");

    // Query file_tracker database for actual checksum verification
    char ft_checksum_date[256] = "";
    get_last_checksum_date(drive_name, ft_checksum_date, sizeof(ft_checksum_date));

    if (ft_checksum_date[0] != '\0') {
        printf("Last Checksum Run:  %s (from file_tracker)\n", ft_checksum_date);
    } else {
        printf("Last Checksum Run:  (no file_tracker database found)\n");
    }

    printf("========================================\n\n");

    sqlite3_finalize(stmt);
    return 1;
}

// Search drives by description
int cmd_search_drives(sqlite3 *db, const char *keyword) {
    const char *sql = "SELECT drive_name, description, storage_container FROM drives "
                      "WHERE description LIKE ? OR storage_container LIKE ?;";

    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, 0);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Failed to prepare statement: %s\n", sqlite3_errmsg(db));
        return 0;
    }

    char search_pattern[MAX_DESC];
    snprintf(search_pattern, sizeof(search_pattern), "%%%s%%", keyword);

    sqlite3_bind_text(stmt, 1, search_pattern, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, search_pattern, -1, SQLITE_STATIC);

    printf("\nSearch results for '%s':\n", keyword);
    printf("========================================\n");

    int count = 0;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        const char *name = (const char *)sqlite3_column_text(stmt, 0);
        const char *desc = (const char *)sqlite3_column_text(stmt, 1);
        const char *container = (const char *)sqlite3_column_text(stmt, 2);

        printf("\nDrive: %s\n", name);
        printf("  Description: %s\n", desc ? desc : "(none)");
        printf("  Container: %s\n", container ? container : "(none)");
        count++;
    }

    if (count == 0) {
        printf("No drives found matching '%s'\n", keyword);
    } else {
        printf("\n========================================\n");
        printf("Found %d drive(s)\n\n", count);
    }

    sqlite3_finalize(stmt);
    return 1;
}

// List all drives
int cmd_list_drives(sqlite3 *db) {
    const char *sql = "SELECT drive_name, capacity, space_used, description, last_updated FROM drives ORDER BY drive_name;";

    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, 0);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Failed to prepare statement: %s\n", sqlite3_errmsg(db));
        return 0;
    }

    printf("\nTracked Drives:\n");
    printf("========================================================================================================\n");
    printf("%-20s | %-15s | %-15s | %-25s | %s\n",
           "Drive Name", "Capacity", "Used", "Description", "Last Updated");
    printf("========================================================================================================\n");

    int count = 0;
    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        const char *name = (const char *)sqlite3_column_text(stmt, 0);
        long long capacity = sqlite3_column_int64(stmt, 1);
        long long used = sqlite3_column_int64(stmt, 2);
        const char *desc = (const char *)sqlite3_column_text(stmt, 3);
        const char *updated = (const char *)sqlite3_column_text(stmt, 4);

        char cap_str[64] = "N/A";
        char used_str[64] = "N/A";

        if (capacity > 0) {
            format_bytes(capacity, cap_str, sizeof(cap_str));
            format_bytes(used, used_str, sizeof(used_str));
        }

        // Truncate description if too long
        char short_desc[30];
        if (desc && strlen(desc) > 25) {
            strncpy(short_desc, desc, 22);
            short_desc[22] = '\0';
            strcat(short_desc, "...");
        } else {
            strncpy(short_desc, desc ? desc : "", sizeof(short_desc) - 1);
            short_desc[sizeof(short_desc) - 1] = '\0';
        }

        printf("%-20s | %-15s | %-15s | %-25s | %s\n",
               name, cap_str, used_str, short_desc, updated ? updated : "never");
        count++;
    }

    printf("========================================================================================================\n");
    printf("Total drives: %d\n\n", count);

    sqlite3_finalize(stmt);
    return 1;
}

// Update drive information
int cmd_update_drive(sqlite3 *db, const char *drive_name, const char *description, const char *container) {
    char timestamp[64];
    get_timestamp(timestamp, sizeof(timestamp));

    // Try to get drive stats if mounted
    char mount_path[MAX_PATH];
    long long capacity = 0, available = 0, used = 0;
    int mounted = 0;

    if (find_mount_point(drive_name, mount_path, sizeof(mount_path))) {
        mounted = get_drive_stats(mount_path, &capacity, &available, &used);
    }

    // Build dynamic SQL based on what needs updating
    char sql[1024] = "UPDATE drives SET last_updated = ?";
    int param_idx = 2;

    if (mounted) {
        strcat(sql, ", capacity = ?, space_available = ?, space_used = ?");
    }
    if (description) {
        strcat(sql, ", description = ?");
    }
    if (container) {
        strcat(sql, ", storage_container = ?");
    }
    strcat(sql, " WHERE drive_name = ?;");

    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, 0);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Failed to prepare statement: %s\n", sqlite3_errmsg(db));
        return 0;
    }

    sqlite3_bind_text(stmt, 1, timestamp, -1, SQLITE_STATIC);

    if (mounted) {
        sqlite3_bind_int64(stmt, param_idx++, capacity);
        sqlite3_bind_int64(stmt, param_idx++, available);
        sqlite3_bind_int64(stmt, param_idx++, used);
    }
    if (description) {
        sqlite3_bind_text(stmt, param_idx++, description, -1, SQLITE_STATIC);
    }
    if (container) {
        sqlite3_bind_text(stmt, param_idx++, container, -1, SQLITE_STATIC);
    }
    sqlite3_bind_text(stmt, param_idx, drive_name, -1, SQLITE_STATIC);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        fprintf(stderr, "Failed to update drive: %s\n", sqlite3_errmsg(db));
        return 0;
    }

    if (sqlite3_changes(db) == 0) {
        printf("Drive '%s' not found.\n", drive_name);
        return 0;
    }

    printf("Drive '%s' updated successfully.\n", drive_name);
    if (mounted) {
        char cap_str[64], avail_str[64], used_str[64];
        format_bytes(capacity, cap_str, sizeof(cap_str));
        format_bytes(available, avail_str, sizeof(avail_str));
        format_bytes(used, used_str, sizeof(used_str));
        printf("  Capacity: %s\n", cap_str);
        printf("  Available: %s\n", avail_str);
        printf("  Used: %s\n", used_str);
    }

    return 1;
}

// Mark drive as verified
int cmd_verify_drive(sqlite3 *db, const char *drive_name) {
    char timestamp[64];
    get_timestamp(timestamp, sizeof(timestamp));

    const char *sql = "UPDATE drives SET last_verified = ? WHERE drive_name = ?;";

    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, 0);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Failed to prepare statement: %s\n", sqlite3_errmsg(db));
        return 0;
    }

    sqlite3_bind_text(stmt, 1, timestamp, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, drive_name, -1, SQLITE_STATIC);

    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        fprintf(stderr, "Failed to verify drive: %s\n", sqlite3_errmsg(db));
        return 0;
    }

    if (sqlite3_changes(db) == 0) {
        printf("Drive '%s' not found.\n", drive_name);
        return 0;
    }

    printf("Drive '%s' marked as verified at %s\n", drive_name, timestamp);
    return 1;
}

// Delete a drive with confirmation
int cmd_delete_drive(sqlite3 *db, const char *drive_name) {
    // First check if drive exists
    const char *check_sql = "SELECT drive_name FROM drives WHERE drive_name = ?;";
    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db, check_sql, -1, &stmt, 0);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Failed to prepare statement: %s\n", sqlite3_errmsg(db));
        return 0;
    }

    sqlite3_bind_text(stmt, 1, drive_name, -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_ROW) {
        printf("Drive '%s' not found.\n", drive_name);
        return 0;
    }

    // Prompt for confirmation
    printf("\nWARNING: You are about to delete drive '%s' from tracking.\n", drive_name);
    printf("This will remove all stored information about this drive.\n");
    printf("Type the drive name to confirm deletion: ");
    fflush(stdout);

    char confirmation[256];
    if (fgets(confirmation, sizeof(confirmation), stdin) == NULL) {
        printf("\nDeletion cancelled.\n");
        return 0;
    }

    // Remove newline
    confirmation[strcspn(confirmation, "\r\n")] = 0;

    if (strcmp(confirmation, drive_name) != 0) {
        printf("Confirmation does not match. Deletion cancelled.\n");
        return 0;
    }

    // Delete the drive
    const char *delete_sql = "DELETE FROM drives WHERE drive_name = ?;";
    rc = sqlite3_prepare_v2(db, delete_sql, -1, &stmt, 0);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Failed to prepare statement: %s\n", sqlite3_errmsg(db));
        return 0;
    }

    sqlite3_bind_text(stmt, 1, drive_name, -1, SQLITE_STATIC);
    rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    if (rc != SQLITE_DONE) {
        fprintf(stderr, "Failed to delete drive: %s\n", sqlite3_errmsg(db));
        return 0;
    }

    printf("Drive '%s' has been deleted from tracking.\n", drive_name);
    return 1;
}

int main(int argc, char *argv[]) {
    // Set up signal handler for clean exit
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    // Enable locale for number formatting
    setlocale(LC_NUMERIC, "");

    if (argc < 2) {
        print_usage(argv[0]);
        return 1;
    }

    // Open database
    const char *home = getenv("HOME");
    if (!home) {
        fprintf(stderr, "Error: HOME environment variable not set\n");
        return 1;
    }

    char db_dir[MAX_PATH];
    snprintf(db_dir, sizeof(db_dir), "%s/db/FileTracker", home);

    // Create directory if it doesn't exist
    char mkdir_cmd[MAX_PATH + 20];
    snprintf(mkdir_cmd, sizeof(mkdir_cmd), "mkdir -p %s", db_dir);
    system(mkdir_cmd);

    char db_path[MAX_PATH];
    snprintf(db_path, sizeof(db_path), "%s/drives.db", db_dir);

    sqlite3 *db;
    int rc = sqlite3_open(db_path, &db);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Cannot open database: %s\n", sqlite3_errmsg(db));
        return 1;
    }

    // Initialize database schema
    if (!init_database(db)) {
        sqlite3_close(db);
        return 1;
    }

    // Parse command
    const char *cmd = argv[1];
    int result = 1;

    if (strcmp(cmd, "add") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Error: drive name required\n");
            print_usage(argv[0]);
            sqlite3_close(db);
            return 1;
        }

        const char *drive_name = argv[2];
        const char *description = NULL;
        const char *container = NULL;

        // Parse optional flags
        for (int i = 3; i < argc; i++) {
            if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
                description = argv[++i];
            } else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
                container = argv[++i];
            }
        }

        result = cmd_add_drive(db, drive_name, description, container) ? 0 : 1;

    } else if (strcmp(cmd, "show") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Error: drive name required\n");
            print_usage(argv[0]);
            sqlite3_close(db);
            return 1;
        }

        result = cmd_show_drive(db, argv[2]) ? 0 : 1;

    } else if (strcmp(cmd, "search") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Error: search keyword required\n");
            print_usage(argv[0]);
            sqlite3_close(db);
            return 1;
        }

        result = cmd_search_drives(db, argv[2]) ? 0 : 1;

    } else if (strcmp(cmd, "list") == 0) {
        result = cmd_list_drives(db) ? 0 : 1;

    } else if (strcmp(cmd, "update") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Error: drive name required\n");
            print_usage(argv[0]);
            sqlite3_close(db);
            return 1;
        }

        const char *drive_name = argv[2];
        const char *description = NULL;
        const char *container = NULL;

        // Parse optional flags
        for (int i = 3; i < argc; i++) {
            if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
                description = argv[++i];
            } else if (strcmp(argv[i], "-c") == 0 && i + 1 < argc) {
                container = argv[++i];
            }
        }

        result = cmd_update_drive(db, drive_name, description, container) ? 0 : 1;

    } else if (strcmp(cmd, "verify") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Error: drive name required\n");
            print_usage(argv[0]);
            sqlite3_close(db);
            return 1;
        }

        result = cmd_verify_drive(db, argv[2]) ? 0 : 1;

    } else if (strcmp(cmd, "delete") == 0) {
        if (argc < 3) {
            fprintf(stderr, "Error: drive name required\n");
            print_usage(argv[0]);
            sqlite3_close(db);
            return 1;
        }

        result = cmd_delete_drive(db, argv[2]) ? 0 : 1;

    } else {
        fprintf(stderr, "Error: unknown command '%s'\n", cmd);
        print_usage(argv[0]);
        result = 1;
    }

    sqlite3_close(db);
    return result;
}

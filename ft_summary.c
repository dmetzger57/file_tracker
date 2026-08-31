#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <locale.h>
#include <dirent.h>
#include <signal.h>
#include <sys/stat.h>

#define MAX_PATH 4096

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
    fprintf(stderr, "Usage: %s [-d <database_name>] [-a] [-N] [-m] [-c] [-n]\n", prog_name);
    fprintf(stderr, "  -d <name>   Database name (without .db extension) - if omitted, shows all databases\n");
    fprintf(stderr, "  -a          Show all runs (default: last run only)\n");
    fprintf(stderr, "  -N          Show run notes (Run #, Date, Note)\n");
    fprintf(stderr, "  -m          List files found missing in the last run\n");
    fprintf(stderr, "  -c          List files found changed in the last run\n");
    fprintf(stderr, "  -n          List files found new in the last run\n");
    fprintf(stderr, "\nDatabases are located in $HOME/db/FileTracker/\n");
    fprintf(stderr, "\nExample:\n");
    fprintf(stderr, "  %s                   # Show last run summary for all databases\n", prog_name);
    fprintf(stderr, "  %s -d MyFiles        # Show last run summary for MyFiles\n", prog_name);
    fprintf(stderr, "  %s -d MyFiles -a     # Show all runs summary for MyFiles\n", prog_name);
    fprintf(stderr, "  %s -d MyFiles -N     # Show notes from all runs for MyFiles\n", prog_name);
    fprintf(stderr, "  %s -d MyFiles -m     # Show last run with missing file list\n", prog_name);
    fprintf(stderr, "  %s -d MyFiles -c     # Show last run with changed file list\n", prog_name);
    fprintf(stderr, "  %s -d MyFiles -n     # Show last run with new file list\n", prog_name);
}

void print_separator(int width) {
    for (int i = 0; i < width; i++) {
        printf("-");
    }
    printf("\n");
}

void print_compact_header() {
    printf("\n");
    print_separator(115);
    printf("%-6s | %-19s | %-6s | %-8s | %10s | %10s | %10s | %10s | %8s\n",
           "Run #", "Run Date", "Update", "Checksum", "Unchanged", "Changed", "New", "Missing", "Errors");
    print_separator(115);
}

void print_compact_row(sqlite3_stmt *stmt) {
    int id = sqlite3_column_int(stmt, 0);
    const char *checksum_date = (const char *)sqlite3_column_text(stmt, 1);
    const char *verify_date = (const char *)sqlite3_column_text(stmt, 2);
    int unchanged = sqlite3_column_int(stmt, 4);
    int changed = sqlite3_column_int(stmt, 5);
    int new_files = sqlite3_column_int(stmt, 6);
    int missing = sqlite3_column_int(stmt, 7);
    int errors = sqlite3_column_int(stmt, 8);
    const char *update_mode = (const char *)sqlite3_column_text(stmt, 9);

    // Determine run date and checksum status
    const char *run_date;
    const char *checksum_status;
    if (checksum_date && strlen(checksum_date) > 0) {
        run_date = checksum_date;
        checksum_status = "On";
    } else {
        run_date = verify_date && strlen(verify_date) > 0 ? verify_date : "unknown";
        checksum_status = "Off";
    }

    // Determine update status
    const char *update_status = "Off";
    if (update_mode && strcmp(update_mode, "ON") == 0) {
        update_status = "On";
    }

    printf("%-6d | %-19s | %-6s | %-8s | %'10d | %'10d | %'10d | %'10d | %'8d\n",
           id, run_date, update_status, checksum_status,
           unchanged, changed, new_files, missing, errors);
}

void print_notes_header() {
    printf("\n");
    print_separator(60);
    printf("%-6s | %-19s | %s\n", "Run #", "Run Date", "Note");
    print_separator(60);
}

void print_notes_row(sqlite3_stmt *stmt) {
    int id = sqlite3_column_int(stmt, 0);
    const char *checksum_date = (const char *)sqlite3_column_text(stmt, 1);
    const char *verify_date = (const char *)sqlite3_column_text(stmt, 2);
    const char *note = (const char *)sqlite3_column_text(stmt, 10);

    // Determine run date
    const char *run_date;
    if (checksum_date && strlen(checksum_date) > 0) {
        run_date = checksum_date;
    } else {
        run_date = verify_date && strlen(verify_date) > 0 ? verify_date : "unknown";
    }

    // Display note or "None"
    const char *note_display = (note && strlen(note) > 0) ? note : "None";

    printf("%-6d | %-19s | %s\n", id, run_date, note_display);
}

void print_log_entries(const char *log_dir, const char *db_name, int run_id,
                       const char *run_date, const char *match_prefix,
                       const char *label) {
    DIR *dir = opendir(log_dir);
    if (!dir) {
        printf("\n%s (Run #%d): log directory not found\n", label, run_id);
        return;
    }

    char file_prefix[256];
    snprintf(file_prefix, sizeof(file_prefix), "%s-", db_name);
    size_t file_prefix_len = strlen(file_prefix);
    size_t match_len = strlen(match_prefix);

    char best_log[MAX_PATH] = "";
    char best_ts[20] = "";

    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (strncmp(entry->d_name, file_prefix, file_prefix_len) != 0)
            continue;
        const char *dot = strrchr(entry->d_name, '.');
        if (!dot || strcmp(dot, ".log") != 0)
            continue;

        const char *ts_part = entry->d_name + file_prefix_len;
        if (strlen(ts_part) < 23)
            continue;

        char ts_comparable[20];
        snprintf(ts_comparable, sizeof(ts_comparable),
                 "%.10s %.2s:%.2s:%.2s",
                 ts_part, ts_part + 11, ts_part + 14, ts_part + 17);

        if (strcmp(ts_comparable, run_date) <= 0) {
            if (best_ts[0] == '\0' || strcmp(ts_comparable, best_ts) > 0) {
                strncpy(best_ts, ts_comparable, sizeof(best_ts) - 1);
                best_ts[sizeof(best_ts) - 1] = '\0';
                snprintf(best_log, sizeof(best_log), "%s/%s", log_dir, entry->d_name);
            }
        }
    }
    closedir(dir);

    if (best_log[0] == '\0') {
        printf("\n%s (Run #%d): no matching log file found\n", label, run_id);
        return;
    }

    FILE *fp = fopen(best_log, "r");
    if (!fp) {
        printf("\n%s (Run #%d): could not open log file\n", label, run_id);
        return;
    }

    printf("\n%s (Run #%d):\n", label, run_id);

    char line[MAX_PATH + 64];
    int count = 0;
    while (fgets(line, sizeof(line), fp)) {
        if (strncmp(line, match_prefix, match_len) == 0) {
            char *path_start = strchr(line, ']');
            if (path_start) {
                path_start++;
                while (*path_start == ' ') path_start++;
                size_t len = strlen(path_start);
                if (len > 0 && path_start[len - 1] == '\n')
                    path_start[len - 1] = '\0';
                printf("  %s\n", path_start);
                count++;
            }
        }
    }
    fclose(fp);

    if (count == 0) {
        printf("  (none)\n");
    }
}

// Process a single database
int process_database(const char *db_name, const char *home, int show_all, int show_notes,
                     int show_missing, int show_changed, int show_new, int is_multi_db) {
    char db_path[MAX_PATH];
    snprintf(db_path, sizeof(db_path), "%s/db/FileTracker/%s.db", home, db_name);

    // Check if database exists
    if (access(db_path, F_OK) != 0) {
        fprintf(stderr, "Error: Database not found: %s\n", db_path);
        fprintf(stderr, "Make sure the database name is correct (without .db extension)\n");
        return 1;
    }

    // Open database (need write access to potentially add column)
    sqlite3 *db;
    if (sqlite3_open(db_path, &db) != SQLITE_OK) {
        fprintf(stderr, "Error: Failed to open database: %s\n", sqlite3_errmsg(db));
        return 1;
    }

    // Migrate: Add update_mode column if it doesn't exist (for backward compatibility)
    // This will fail silently if column already exists, which is fine
    char *err_msg = NULL;
    sqlite3_exec(db, "ALTER TABLE meta ADD COLUMN update_mode TEXT", NULL, NULL, &err_msg);
    if (err_msg) {
        // Ignore "duplicate column name" error - it just means column already exists
        sqlite3_free(err_msg);
    }

    // Query meta table
    const char *query;
    if (show_all) {
        query = "SELECT id, last_checksum_verify_date, last_date_verify, verify_machine, "
                "num_unchanged, num_changed, num_new, num_missing, num_errors, update_mode, note "
                "FROM meta ORDER BY id ASC";
    } else {
        query = "SELECT id, last_checksum_verify_date, last_date_verify, verify_machine, "
                "num_unchanged, num_changed, num_new, num_missing, num_errors, update_mode, note "
                "FROM meta ORDER BY id DESC LIMIT 1";
    }

    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db, query, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        if (is_multi_db) {
            // When processing multiple databases, skip databases with missing tables
            fprintf(stderr, "Warning: Skipping database '%s' - %s\n", db_name, sqlite3_errmsg(db));
            sqlite3_close(db);
            return 0;  // Return success to continue processing other databases
        } else {
            fprintf(stderr, "Error: Failed to prepare query: %s\n", sqlite3_errmsg(db));
            sqlite3_close(db);
            return 1;
        }
    }

    // Display results
    int row_count = 0;
    int last_run_id = 0;
    char last_run_date[20] = "";

    // Print database name header if processing multiple databases
    if (is_multi_db) {
        printf("\n");
        printf("================================================================================\n");
        printf("DATABASE: %s\n", db_name);
        printf("================================================================================\n");
    }

    if (show_notes) {
        print_notes_header();
    } else {
        print_compact_header();
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        row_count++;

        if (show_missing || show_changed || show_new) {
            last_run_id = sqlite3_column_int(stmt, 0);
            const char *cd = (const char *)sqlite3_column_text(stmt, 1);
            const char *vd = (const char *)sqlite3_column_text(stmt, 2);
            const char *date = (cd && strlen(cd) > 0) ? cd : vd;
            if (date) {
                strncpy(last_run_date, date, sizeof(last_run_date) - 1);
                last_run_date[sizeof(last_run_date) - 1] = '\0';
            }
        }

        if (show_notes) {
            print_notes_row(stmt);
        } else {
            print_compact_row(stmt);
        }
    }

    int separator_width = show_notes ? 60 : 115;
    if (row_count > 0) {
        print_separator(separator_width);
        printf("Total runs: %d\n", row_count);
    }

    if (row_count == 0) {
        printf("\nNo verification runs found in database: %s\n", db_path);
        printf("The database exists but contains no meta records.\n");
        printf("Run file_tracker to perform a verification scan.\n");
    }

    if ((show_missing || show_changed || show_new) && last_run_id > 0 && last_run_date[0] != '\0') {
        char log_dir[MAX_PATH];
        snprintf(log_dir, sizeof(log_dir), "%s/logs/FileTracker", home);
        if (show_missing)
            print_log_entries(log_dir, db_name, last_run_id, last_run_date,
                              "[MISSING", "Missing Files");
        if (show_changed)
            print_log_entries(log_dir, db_name, last_run_id, last_run_date,
                              "[CHANGED", "Changed Files");
        if (show_new)
            print_log_entries(log_dir, db_name, last_run_id, last_run_date,
                              "[NEW", "New Files");
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);

    return 0;
}

int main(int argc, char *argv[]) {
    char *db_name = NULL;
    int show_all = 0;
    int show_notes = 0;
    int show_missing = 0;
    int show_changed = 0;
    int show_new = 0;

    // Enable locale for thousand separators
    setlocale(LC_NUMERIC, "");

    // Setup terminal wrapping control
    atexit(enable_line_wrap);
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGHUP, signal_handler);
    disable_line_wrap();

    // Parse command line arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            db_name = argv[++i];
        } else if (strcmp(argv[i], "-a") == 0) {
            show_all = 1;
        } else if (strcmp(argv[i], "-N") == 0) {
            show_notes = 1;
            show_all = 1;  // Notes mode shows all runs
        } else if (strcmp(argv[i], "-m") == 0) {
            show_missing = 1;
        } else if (strcmp(argv[i], "-c") == 0) {
            show_changed = 1;
        } else if (strcmp(argv[i], "-n") == 0) {
            show_new = 1;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Error: Unknown option '%s'\n\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    // Get HOME directory
    const char *home = getenv("HOME");
    if (!home) {
        fprintf(stderr, "Error: HOME environment variable not set\n");
        return 1;
    }

    // If database name is provided, process single database
    if (db_name) {
        return process_database(db_name, home, show_all, show_notes,
                                show_missing, show_changed, show_new, 0);
    }

    // Process all databases in the FileTracker directory
    char db_dir[MAX_PATH];
    snprintf(db_dir, sizeof(db_dir), "%s/db/FileTracker", home);

    DIR *dir = opendir(db_dir);
    if (!dir) {
        fprintf(stderr, "Error: Cannot open database directory: %s\n", db_dir);
        return 1;
    }

    // Collect all database names
    char **db_names = NULL;
    int db_count = 0;
    int db_capacity = 10;
    db_names = malloc(db_capacity * sizeof(char *));
    if (!db_names) {
        fprintf(stderr, "Error: Memory allocation failed\n");
        closedir(dir);
        return 1;
    }

    struct dirent *entry;
    while ((entry = readdir(dir))) {
        // Skip . and ..
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        // Check if it's a .db file
        size_t name_len = strlen(entry->d_name);
        if (name_len > 3 && strcmp(entry->d_name + name_len - 3, ".db") == 0) {
            // Check if it's a regular file
            char full_path[MAX_PATH];
            snprintf(full_path, sizeof(full_path), "%s/%s", db_dir, entry->d_name);
            struct stat st;
            if (stat(full_path, &st) == 0 && S_ISREG(st.st_mode)) {
                // Extract database name (without .db extension)
                char *name = strdup(entry->d_name);
                if (name) {
                    name[name_len - 3] = '\0';  // Remove .db extension

                    // Expand array if needed
                    if (db_count >= db_capacity) {
                        db_capacity *= 2;
                        char **new_names = realloc(db_names, db_capacity * sizeof(char *));
                        if (!new_names) {
                            fprintf(stderr, "Error: Memory allocation failed\n");
                            free(name);
                            for (int i = 0; i < db_count; i++) free(db_names[i]);
                            free(db_names);
                            closedir(dir);
                            return 1;
                        }
                        db_names = new_names;
                    }

                    db_names[db_count++] = name;
                }
            }
        }
    }
    closedir(dir);

    if (db_count == 0) {
        printf("No databases found in %s\n", db_dir);
        free(db_names);
        return 0;
    }

    // Sort database names alphabetically for consistent output
    for (int i = 0; i < db_count - 1; i++) {
        for (int j = i + 1; j < db_count; j++) {
            if (strcmp(db_names[i], db_names[j]) > 0) {
                char *temp = db_names[i];
                db_names[i] = db_names[j];
                db_names[j] = temp;
            }
        }
    }

    // Process each database
    int result = 0;
    for (int i = 0; i < db_count; i++) {
        int rc = process_database(db_names[i], home, show_all, show_notes,
                                  show_missing, show_changed, show_new, 1);
        if (rc != 0) {
            result = rc;
        }
        free(db_names[i]);
    }
    free(db_names);

    if (db_count > 1) {
        printf("\n");
    }

    return result;
}

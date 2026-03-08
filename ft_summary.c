#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <locale.h>

#define MAX_PATH 4096

void print_usage(const char *prog_name) {
    fprintf(stderr, "Usage: %s -d <database_name> [-a]\n", prog_name);
    fprintf(stderr, "  -d <name>   Database name (without .db extension)\n");
    fprintf(stderr, "  -a          Show all runs (default: last run only)\n");
    fprintf(stderr, "\nDatabases are located in $HOME/db/FileTracker/\n");
    fprintf(stderr, "\nExample:\n");
    fprintf(stderr, "  %s -d MyFiles        # Show last run for MyFiles.db\n", prog_name);
    fprintf(stderr, "  %s -d MyFiles -a     # Show all runs for MyFiles.db\n", prog_name);
}

void print_separator(int width) {
    for (int i = 0; i < width; i++) {
        printf("-");
    }
    printf("\n");
}

void print_single_run(sqlite3_stmt *stmt) {
    int id = sqlite3_column_int(stmt, 0);
    const char *checksum_date = (const char *)sqlite3_column_text(stmt, 1);
    const char *verify_date = (const char *)sqlite3_column_text(stmt, 2);
    const char *machine = (const char *)sqlite3_column_text(stmt, 3);
    int unchanged = sqlite3_column_int(stmt, 4);
    int changed = sqlite3_column_int(stmt, 5);
    int new_files = sqlite3_column_int(stmt, 6);
    int missing = sqlite3_column_int(stmt, 7);
    int errors = sqlite3_column_int(stmt, 8);
    const char *update_mode = (const char *)sqlite3_column_text(stmt, 9);

    printf("\n==================== RUN #%d ====================\n", id);

    if (checksum_date && strlen(checksum_date) > 0) {
        printf("Checksum Verify Date: %s\n", checksum_date);
    }
    if (verify_date && strlen(verify_date) > 0) {
        printf("Date Verify:          %s\n", verify_date);
    }
    printf("Machine:              %s\n", machine ? machine : "unknown");
    printf("Update Mode:          %s\n", update_mode ? update_mode : "UNK");
    printf("Unchanged:            %'d\n", unchanged);
    printf("Changed:              %'d\n", changed);
    printf("New:                  %'d\n", new_files);
    printf("Missing:              %'d\n", missing);
    printf("Errors:               %'d\n", errors);
    printf("================================================\n");
}

void print_table_header() {
    printf("\n");
    print_separator(132);
    printf("%-4s | %-19s | %-19s | %-15s | %-6s | %10s | %10s | %10s | %10s | %8s\n",
           "ID", "Checksum Date", "Verify Date", "Machine", "Update", "Unchanged", "Changed", "New", "Missing", "Errors");
    print_separator(132);
}

void print_table_row(sqlite3_stmt *stmt) {
    int id = sqlite3_column_int(stmt, 0);
    const char *checksum_date = (const char *)sqlite3_column_text(stmt, 1);
    const char *verify_date = (const char *)sqlite3_column_text(stmt, 2);
    const char *machine = (const char *)sqlite3_column_text(stmt, 3);
    int unchanged = sqlite3_column_int(stmt, 4);
    int changed = sqlite3_column_int(stmt, 5);
    int new_files = sqlite3_column_int(stmt, 6);
    int missing = sqlite3_column_int(stmt, 7);
    int errors = sqlite3_column_int(stmt, 8);
    const char *update_mode = (const char *)sqlite3_column_text(stmt, 9);

    // Truncate machine name if too long
    char machine_short[16];
    if (machine) {
        snprintf(machine_short, sizeof(machine_short), "%s", machine);
    } else {
        strcpy(machine_short, "unknown");
    }

    printf("%-4d | %-19s | %-19s | %-15s | %-6s | %'10d | %'10d | %'10d | %'10d | %'8d\n",
           id,
           checksum_date && strlen(checksum_date) > 0 ? checksum_date : "",
           verify_date && strlen(verify_date) > 0 ? verify_date : "",
           machine_short,
           update_mode ? update_mode : "UNK",
           unchanged, changed, new_files, missing, errors);
}

int main(int argc, char *argv[]) {
    char *db_name = NULL;
    int show_all = 0;

    // Enable locale for thousand separators
    setlocale(LC_NUMERIC, "");

    // Parse command line arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            db_name = argv[++i];
        } else if (strcmp(argv[i], "-a") == 0) {
            show_all = 1;
        } else if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            print_usage(argv[0]);
            return 0;
        } else {
            fprintf(stderr, "Error: Unknown option '%s'\n\n", argv[i]);
            print_usage(argv[0]);
            return 1;
        }
    }

    if (!db_name) {
        fprintf(stderr, "Error: -d option is required\n\n");
        print_usage(argv[0]);
        return 1;
    }

    // Construct database path
    const char *home = getenv("HOME");
    if (!home) {
        fprintf(stderr, "Error: HOME environment variable not set\n");
        return 1;
    }

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
                "num_unchanged, num_changed, num_new, num_missing, num_errors, update_mode "
                "FROM meta ORDER BY id ASC";
    } else {
        query = "SELECT id, last_checksum_verify_date, last_date_verify, verify_machine, "
                "num_unchanged, num_changed, num_new, num_missing, num_errors, update_mode "
                "FROM meta ORDER BY id DESC LIMIT 1";
    }

    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db, query, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Error: Failed to prepare query: %s\n", sqlite3_errmsg(db));
        sqlite3_close(db);
        return 1;
    }

    // Display results
    int row_count = 0;

    if (show_all) {
        print_table_header();
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        row_count++;

        if (show_all) {
            print_table_row(stmt);
        } else {
            print_single_run(stmt);
        }
    }

    if (show_all && row_count > 0) {
        print_separator(132);
        printf("Total runs: %d\n\n", row_count);
    }

    if (row_count == 0) {
        printf("\nNo verification runs found in database: %s\n", db_path);
        printf("The database exists but contains no meta records.\n");
        printf("Run file_tracker to perform a verification scan.\n\n");
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);

    return 0;
}

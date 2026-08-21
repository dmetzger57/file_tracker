#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sqlite3.h>
#include <unistd.h>

#define MAX_PATH 4096

void print_usage(const char *prog_name) {
    fprintf(stderr, "Usage: %s -n database_name [-l | -r run_identifier [-N] [-C] [-M]]\n", prog_name);
    fprintf(stderr, "  -n <name>   Database name (without .db extension) - REQUIRED\n");
    fprintf(stderr, "  -l          List all runs in the database\n");
    fprintf(stderr, "  -r <run_id> Run identifier (DB_Name-YYYY-MM-DD-HH-MM-SS) - use -l to see available runs\n");
    fprintf(stderr, "  -N          Show only NEW file messages\n");
    fprintf(stderr, "  -C          Show only CHANGED file messages\n");
    fprintf(stderr, "  -M          Show only MISSING file messages\n");
    fprintf(stderr, "\nNote: If no filter options are specified, all log messages are displayed.\n");
    fprintf(stderr, "      Multiple filter options can be combined (e.g., -N -C shows NEW and CHANGED).\n");
}

int main(int argc, char *argv[]) {
    char *db_name = NULL;
    char *run_identifier = NULL;
    int show_new = 0;
    int show_changed = 0;
    int show_missing = 0;
    int show_all = 1;
    int list_runs = 0;

    // Parse command line arguments
    int opt;
    while ((opt = getopt(argc, argv, "n:r:NCMlh")) != -1) {
        switch (opt) {
            case 'n':
                db_name = optarg;
                break;
            case 'r':
                run_identifier = optarg;
                break;
            case 'N':
                show_new = 1;
                show_all = 0;
                break;
            case 'C':
                show_changed = 1;
                show_all = 0;
                break;
            case 'M':
                show_missing = 1;
                show_all = 0;
                break;
            case 'l':
                list_runs = 1;
                break;
            case 'h':
                print_usage(argv[0]);
                exit(0);
            default:
                print_usage(argv[0]);
                exit(1);
        }
    }

    // Validate required arguments
    if (!db_name) {
        fprintf(stderr, "Error: -n option is required\n\n");
        print_usage(argv[0]);
        exit(1);
    }

    if (!list_runs && !run_identifier) {
        fprintf(stderr, "Error: -r option is required (unless using -l)\n\n");
        print_usage(argv[0]);
        exit(1);
    }

    // Construct database path
    const char *home = getenv("HOME");
    if (!home) {
        fprintf(stderr, "Error: HOME environment variable not set\n");
        exit(1);
    }

    char db_path[MAX_PATH];
    snprintf(db_path, sizeof(db_path), "%s/db/FileTracker/%s.db", home, db_name);

    // Check if database exists
    if (access(db_path, F_OK) != 0) {
        fprintf(stderr, "Error: Database not found: %s\n", db_path);
        exit(1);
    }

    // Open database
    sqlite3 *db;
    if (sqlite3_open(db_path, &db) != SQLITE_OK) {
        fprintf(stderr, "Error: Failed to open database: %s\n", sqlite3_errmsg(db));
        sqlite3_close(db);
        exit(1);
    }

    // If -l option is used, list all runs and exit
    if (list_runs) {
        sqlite3_stmt *stmt;
        const char *list_query =
            "SELECT id, "
            "COALESCE(last_checksum_verify_date, last_date_verify) as run_date, "
            "verify_machine, num_unchanged, num_changed, num_new, num_missing, "
            "update_mode, note "
            "FROM meta ORDER BY id DESC";

        if (sqlite3_prepare_v2(db, list_query, -1, &stmt, NULL) != SQLITE_OK) {
            fprintf(stderr, "Error: Failed to query database: %s\n", sqlite3_errmsg(db));
            sqlite3_close(db);
            exit(1);
        }

        printf("================ ALL RUNS IN DATABASE ================\n");
        printf("%-42s %-15s %s\n", "Run Identifier", "Machine", "U/C/N/M");
        printf("======================================================\n");

        int count = 0;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            sqlite3_int64 id = sqlite3_column_int64(stmt, 0);
            const char *run_date = (const char *)sqlite3_column_text(stmt, 1);
            const char *machine = (const char *)sqlite3_column_text(stmt, 2);
            int unchanged = sqlite3_column_int(stmt, 3);
            int changed = sqlite3_column_int(stmt, 4);
            int new = sqlite3_column_int(stmt, 5);
            int missing = sqlite3_column_int(stmt, 6);
            const char *update_mode = (const char *)sqlite3_column_text(stmt, 7);
            const char *note = (const char *)sqlite3_column_text(stmt, 8);

            // Format run identifier: dbname-YYYY-MM-DD-HH-MM-SS
            char run_id[128];
            if (run_date) {
                char formatted_date[64];
                strncpy(formatted_date, run_date, sizeof(formatted_date) - 1);
                formatted_date[sizeof(formatted_date) - 1] = '\0';

                // Replace spaces and colons with hyphens
                for (char *p = formatted_date; *p; p++) {
                    if (*p == ' ' || *p == ':') *p = '-';
                }
                snprintf(run_id, sizeof(run_id), "%s-%s", db_name, formatted_date);
            } else {
                snprintf(run_id, sizeof(run_id), "%s-unknown-%lld", db_name, id);
            }

            // Format: Run ID, Machine, Stats
            printf("%-42s %-15s %d/%d/%d/%d",
                   run_id,
                   machine ? machine : "N/A",
                   unchanged, changed, new, missing);

            // Add update mode indicator
            if (update_mode && strcmp(update_mode, "OFF") == 0) {
                printf(" [RO]");
            }

            // Show note if present (truncated)
            if (note && strlen(note) > 0) {
                printf(" \"%s\"", note);
            }

            printf("\n");
            count++;
        }

        printf("======================================================\n");
        printf("Total runs: %d\n", count);
        printf("\nLegend: U/C/N/M = Unchanged/Changed/New/Missing\n");
        printf("        [RO] = Read-only mode (update mode OFF)\n");
        printf("\nUse the Run Identifier with -r option to view logs:\n");
        printf("  Example: %s -n %s -r <run_identifier>\n", argv[0], db_name);

        sqlite3_finalize(stmt);
        sqlite3_close(db);
        exit(0);
    }

    // Parse run identifier: dbname-YYYY-MM-DD-HH-MM-SS
    // Extract date and time from run identifier
    char datetime_pattern[64];

    // Find the database name prefix and skip it
    const char *date_start = strchr(run_identifier, '-');
    if (!date_start) {
        fprintf(stderr, "Error: Invalid run identifier format. Expected: dbname-YYYY-MM-DD-HH-MM-SS\n");
        fprintf(stderr, "Use -l option to see available run identifiers.\n");
        sqlite3_close(db);
        exit(1);
    }
    date_start++; // Skip the first hyphen

    // Parse YYYY-MM-DD-HH-MM-SS
    int year, month, day, hour, min, sec;
    if (sscanf(date_start, "%d-%d-%d-%d-%d-%d", &year, &month, &day, &hour, &min, &sec) != 6) {
        fprintf(stderr, "Error: Invalid run identifier format. Expected: dbname-YYYY-MM-DD-HH-MM-SS\n");
        fprintf(stderr, "Use -l option to see available run identifiers.\n");
        sqlite3_close(db);
        exit(1);
    }

    // Format as "YYYY-MM-DD HH:MM:SS" for database query
    snprintf(datetime_pattern, sizeof(datetime_pattern), "%04d-%02d-%02d %02d:%02d:%02d",
             year, month, day, hour, min, sec);

    // Find the run_id matching the datetime
    sqlite3_stmt *stmt;
    const char *find_run_query =
        "SELECT id, verify_machine, num_unchanged, num_changed, num_new, num_missing, "
        "COALESCE(last_checksum_verify_date, last_date_verify) as run_date "
        "FROM meta WHERE run_date = ? LIMIT 1";

    if (sqlite3_prepare_v2(db, find_run_query, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "Error: Failed to prepare query: %s\n", sqlite3_errmsg(db));
        sqlite3_close(db);
        exit(1);
    }

    sqlite3_bind_text(stmt, 1, datetime_pattern, -1, SQLITE_STATIC);

    sqlite3_int64 run_id = 0;
    if (sqlite3_step(stmt) == SQLITE_ROW) {
        run_id = sqlite3_column_int64(stmt, 0);
        const char *machine = (const char *)sqlite3_column_text(stmt, 1);
        int unchanged = sqlite3_column_int(stmt, 2);
        int changed = sqlite3_column_int(stmt, 3);
        int new = sqlite3_column_int(stmt, 4);
        int missing = sqlite3_column_int(stmt, 5);
        const char *run_date = (const char *)sqlite3_column_text(stmt, 6);

        printf("================ RUN INFORMATION ==================\n");
        printf("Run ID         : %lld\n", run_id);
        printf("Date/Time      : %s\n", run_date);
        printf("Machine        : %s\n", machine);
        printf("Unchanged      : %d\n", unchanged);
        printf("Changed        : %d\n", changed);
        printf("New            : %d\n", new);
        printf("Missing        : %d\n", missing);
        printf("==================================================\n\n");
    } else {
        fprintf(stderr, "Error: No run found matching: %s\n", run_identifier);
        fprintf(stderr, "Available runs in database:\n");

        sqlite3_finalize(stmt);

        const char *list_runs =
            "SELECT id, COALESCE(last_checksum_verify_date, last_date_verify) as run_date, "
            "verify_machine FROM meta ORDER BY id DESC LIMIT 10";
        if (sqlite3_prepare_v2(db, list_runs, -1, &stmt, NULL) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                const char *run_date = (const char *)sqlite3_column_text(stmt, 1);

                // Format run identifier
                char run_id[128];
                if (run_date) {
                    char formatted_date[64];
                    strncpy(formatted_date, run_date, sizeof(formatted_date) - 1);
                    formatted_date[sizeof(formatted_date) - 1] = '\0';

                    // Replace spaces and colons with hyphens
                    for (char *p = formatted_date; *p; p++) {
                        if (*p == ' ' || *p == ':') *p = '-';
                    }
                    snprintf(run_id, sizeof(run_id), "%s-%s", db_name, formatted_date);
                } else {
                    snprintf(run_id, sizeof(run_id), "%s-unknown-%lld", db_name,
                            sqlite3_column_int64(stmt, 0));
                }

                printf("  %s\n", run_id);
            }
        }
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        exit(1);
    }
    sqlite3_finalize(stmt);

    // Build query for log messages
    char log_query[1024];
    if (show_all) {
        snprintf(log_query, sizeof(log_query),
                 "SELECT status, full_path FROM run_logs WHERE run_id = ? ORDER BY id");
    } else {
        // Build WHERE clause for status filters
        char status_filter[256] = "";
        int first = 1;

        if (show_new) {
            strcat(status_filter, first ? "status = 'NEW'" : " OR status = 'NEW'");
            first = 0;
        }
        if (show_changed) {
            strcat(status_filter, first ? "status LIKE 'CHANGED%'" : " OR status LIKE 'CHANGED%'");
            first = 0;
        }
        if (show_missing) {
            strcat(status_filter, first ? "status = 'MISSING'" : " OR status = 'MISSING'");
            first = 0;
        }

        snprintf(log_query, sizeof(log_query),
                 "SELECT status, full_path FROM run_logs WHERE run_id = ? AND (%s) ORDER BY id",
                 status_filter);
    }

    if (sqlite3_prepare_v2(db, log_query, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "Error: Failed to prepare log query: %s\n", sqlite3_errmsg(db));
        sqlite3_close(db);
        exit(1);
    }

    sqlite3_bind_int64(stmt, 1, run_id);

    // Display log messages
    int count = 0;
    printf("=================== LOG MESSAGES ==================\n");
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *status = (const char *)sqlite3_column_text(stmt, 0);
        const char *path = (const char *)sqlite3_column_text(stmt, 1);
        printf("[%-18s] %s\n", status, path);
        count++;
    }

    if (count == 0) {
        printf("No log messages found");
        if (!show_all) {
            printf(" matching the specified filters");
        }
        printf(".\n");
    }
    printf("==================================================\n");
    printf("Total messages : %d\n", count);

    sqlite3_finalize(stmt);
    sqlite3_close(db);

    return 0;
}

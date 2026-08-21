#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sqlite3.h>
#include <unistd.h>

#define MAX_PATH 4096

void print_usage(const char *prog_name) {
    fprintf(stderr, "Usage: %s -n database_name -d YYYY-MM-DD -t HH-MM-SS [-N] [-C] [-M]\n", prog_name);
    fprintf(stderr, "  -n <name>   Database name (without .db extension) - REQUIRED\n");
    fprintf(stderr, "  -d <date>   Date of run in YYYY-MM-DD format - REQUIRED\n");
    fprintf(stderr, "  -t <time>   Time of run in HH-MM-SS format - REQUIRED\n");
    fprintf(stderr, "  -N          Show only NEW file messages\n");
    fprintf(stderr, "  -C          Show only CHANGED file messages\n");
    fprintf(stderr, "  -M          Show only MISSING file messages\n");
    fprintf(stderr, "\nNote: If no filter options are specified, all log messages are displayed.\n");
    fprintf(stderr, "      Multiple filter options can be combined (e.g., -N -C shows NEW and CHANGED).\n");
}

int main(int argc, char *argv[]) {
    char *db_name = NULL;
    char *date_str = NULL;
    char *time_str = NULL;
    int show_new = 0;
    int show_changed = 0;
    int show_missing = 0;
    int show_all = 1;

    // Parse command line arguments
    int opt;
    while ((opt = getopt(argc, argv, "n:d:t:NCMh")) != -1) {
        switch (opt) {
            case 'n':
                db_name = optarg;
                break;
            case 'd':
                date_str = optarg;
                break;
            case 't':
                time_str = optarg;
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
            case 'h':
                print_usage(argv[0]);
                exit(0);
            default:
                print_usage(argv[0]);
                exit(1);
        }
    }

    // Validate required arguments
    if (!db_name || !date_str || !time_str) {
        fprintf(stderr, "Error: -n, -d, and -t options are required\n\n");
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

    // Construct datetime string for query (convert HH-MM-SS to HH:MM:SS)
    char datetime_pattern[64];
    char time_formatted[16];

    // Replace hyphens with colons in time
    int h, m, s;
    if (sscanf(time_str, "%d-%d-%d", &h, &m, &s) != 3) {
        fprintf(stderr, "Error: Time format must be HH-MM-SS (e.g., 14-30-45)\n");
        sqlite3_close(db);
        exit(1);
    }
    snprintf(time_formatted, sizeof(time_formatted), "%02d:%02d:%02d", h, m, s);
    snprintf(datetime_pattern, sizeof(datetime_pattern), "%s %s", date_str, time_formatted);

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
        fprintf(stderr, "Error: No run found for date %s time %s\n", date_str, time_str);
        fprintf(stderr, "Available runs in database:\n");

        sqlite3_finalize(stmt);

        const char *list_runs =
            "SELECT id, COALESCE(last_checksum_verify_date, last_date_verify) as run_date, "
            "verify_machine FROM meta ORDER BY id DESC LIMIT 10";
        if (sqlite3_prepare_v2(db, list_runs, -1, &stmt, NULL) == SQLITE_OK) {
            while (sqlite3_step(stmt) == SQLITE_ROW) {
                printf("  ID: %lld - %s - %s\n",
                       sqlite3_column_int64(stmt, 0),
                       sqlite3_column_text(stmt, 1),
                       sqlite3_column_text(stmt, 2));
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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sqlite3.h>
#include <unistd.h>

#define MAX_PATH 4096

void print_usage(const char *prog_name) {
    fprintf(stderr, "Usage: %s -d database_name [-v]\n", prog_name);
    fprintf(stderr, "  -d <name>   Database name (without .db extension) - REQUIRED\n");
    fprintf(stderr, "  -v          Verbose output (show checksums)\n");
    fprintf(stderr, "  -h          Show this help message\n");
}

int main(int argc, char *argv[]) {
    char *db_name = NULL;
    int verbose = 0;

    // Parse command line arguments
    int opt;
    while ((opt = getopt(argc, argv, "d:vh")) != -1) {
        switch (opt) {
            case 'd':
                db_name = optarg;
                break;
            case 'v':
                verbose = 1;
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
        fprintf(stderr, "Error: -d option is required\n\n");
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

    // Find checksums that appear more than once
    sqlite3_stmt *dup_stmt;
    const char *dup_query =
        "SELECT checksum, COUNT(*) as count "
        "FROM files "
        "WHERE checksum IS NOT NULL AND checksum != '' "
        "GROUP BY checksum "
        "HAVING COUNT(*) > 1 "
        "ORDER BY count DESC, checksum";

    if (sqlite3_prepare_v2(db, dup_query, -1, &dup_stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "Error: Failed to query duplicates: %s\n", sqlite3_errmsg(db));
        sqlite3_close(db);
        exit(1);
    }

    int duplicate_groups = 0;
    int total_duplicate_files = 0;

    // For each duplicate checksum, find all files with that checksum
    while (sqlite3_step(dup_stmt) == SQLITE_ROW) {
        const char *checksum = (const char *)sqlite3_column_text(dup_stmt, 0);
        int count = sqlite3_column_int(dup_stmt, 1);

        duplicate_groups++;
        total_duplicate_files += count;

        if (verbose) {
            printf("\n==================== DUPLICATE GROUP ====================\n");
            printf("Checksum: %s\n", checksum);
            printf("Count: %d files\n", count);
            printf("=========================================================\n");
        } else {
            printf("\n");
        }

        // Query for all files with this checksum
        sqlite3_stmt *files_stmt;
        const char *files_query = "SELECT full_path FROM files WHERE checksum = ? ORDER BY full_path";

        if (sqlite3_prepare_v2(db, files_query, -1, &files_stmt, NULL) != SQLITE_OK) {
            fprintf(stderr, "Error: Failed to query files: %s\n", sqlite3_errmsg(db));
            continue;
        }

        sqlite3_bind_text(files_stmt, 1, checksum, -1, SQLITE_STATIC);

        while (sqlite3_step(files_stmt) == SQLITE_ROW) {
            const char *path = (const char *)sqlite3_column_text(files_stmt, 0);
            printf("  %s\n", path);
        }

        sqlite3_finalize(files_stmt);
    }

    sqlite3_finalize(dup_stmt);

    // Print summary
    printf("\n");
    printf("========================================================\n");
    printf("Summary:\n");
    printf("  Duplicate groups : %d\n", duplicate_groups);
    printf("  Total duplicates : %d files\n", total_duplicate_files);
    printf("========================================================\n");

    sqlite3_close(db);
    return 0;
}

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sqlite3.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>


// To build: gcc -o file_locator file_locator.c -l sqlite3

#define MAX_PATH 4096

int verbose = 0;
int found_count = 0;

char Checksum[128];

// Forward declarations
void search_database(const char *dbname, const char *db_path, const char *filename, int partial);
void list_databases_and_search(const char *dir_path, const char *filename, int partial);

int main(int argc, char *argv[]) {
    char *filename = NULL;
    char *dbname = NULL;
    int partial = 0;

    // Parse arguments
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-f") == 0 && i + 1 < argc) {
            filename = argv[++i];
        } else if (strcmp(argv[i], "-p") == 0) {
            partial = 1;
        } else if (strcmp(argv[i], "-d") == 0 && i + 1 < argc) {
            dbname = argv[++i];
	} else if (strcmp(argv[i], "-v") == 0 && i + 1 < argc) {
            verbose = 1;
	} else if (strcmp(argv[i], "-h") == 0 && i + 1 < argc) {
            fprintf(stderr, "Usage: %s -v -f FileName [-p] [-d DbName]\n", argv[0]);
            return 1;
        } else {
            fprintf(stderr, "Usage: %s -v -f FileName [-p] [-d DbName]\n", argv[0]);
            return 1;
        }
    }

    if (!filename) {
        fprintf(stderr, "Error: -f FileName is required\n");
        return 1;
    }

    memset(Checksum, 0, sizeof(Checksum));

    const char *home = getenv("HOME");
    if (!home) {
        fprintf(stderr, "Error: HOME environment variable not set\n");
        return 1;
    }

    char db_dir[MAX_PATH];
    snprintf(db_dir, sizeof(db_dir), "%s/db/FileTracker", home);

    if (dbname) {
        // Search a specific database
        char db_path[MAX_PATH];
        snprintf(db_path, sizeof(db_path), "%s/%s", db_dir, dbname);
        search_database(dbname, db_path, filename, partial);
    } else {
        // Search all databases in the directory
        list_databases_and_search(db_dir, filename, partial);
    }

    return found_count;
}

void search_database(const char *dbname, const char *db_path, const char *filename, int partial) {
    sqlite3 *db;
    sqlite3_stmt *stmt;
    int rc;

    struct stat st;
    if (stat(db_path, &st) != 0) {
        return; // skip missing or inaccessible files
    }

    rc = sqlite3_open(db_path, &db);
    if (rc) {
        fprintf(stderr, "Cannot open database %s: %s\n", db_path, sqlite3_errmsg(db));
        sqlite3_close(db);
        return;
    }

    char sql[512];
    if (partial) {
        snprintf(sql, sizeof(sql),
                 "SELECT id, file_name, full_path, size, created, last_modified, owner, checksum "
                 "FROM files WHERE file_name LIKE ?;");
    } else {
        snprintf(sql, sizeof(sql),
                 "SELECT id, file_name, full_path, size, created, last_modified, owner, checksum "
                 "FROM files WHERE file_name = ?;");
    }

    rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "Failed to prepare statement for %s: %s\n", db_path, sqlite3_errmsg(db));
        sqlite3_close(db);
        return;
    }

    char pattern[MAX_PATH];
    if (partial) {
        snprintf(pattern, sizeof(pattern), "%%%s%%", filename);
        sqlite3_bind_text(stmt, 1, pattern, -1, SQLITE_STATIC);
    } else {
        sqlite3_bind_text(stmt, 1, filename, -1, SQLITE_STATIC);
    }

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
	++found_count;

        if( Checksum[0] == '\0' ) {
            strcpy( Checksum, (char *)sqlite3_column_text(stmt, 7));
        }

	if ( verbose == 1 ) {
            printf("Database: %s\n", dbname);
            printf("    ID: %d\n", sqlite3_column_int(stmt, 0));
            printf("    Full Path: %s\n", sqlite3_column_text(stmt, 2));
            printf("    Size: %lld bytes\n", sqlite3_column_int64(stmt, 3));
            printf("    Created: %lld\n", sqlite3_column_int64(stmt, 4));
            printf("    Last Modified: %lld\n", sqlite3_column_int64(stmt, 5));
            printf("    Owner: %s\n", sqlite3_column_text(stmt, 6));
            printf("    Checksum: %s\n\n", sqlite3_column_text(stmt, 7));
	}
	else {
            if( strcmp( Checksum, (char *)sqlite3_column_text(stmt, 7) ) != 0 ) {
                printf("%24.24s, %s, Checksum Mismatch\n", dbname, sqlite3_column_text(stmt, 2));
            }
            else {
                printf("%24.24s, %s\n", dbname, sqlite3_column_text(stmt, 2));
	    }
        }
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
}

void list_databases_and_search(const char *dir_path, const char *filename, int partial) {
    DIR *dir = opendir(dir_path);
    if (!dir) {
        fprintf(stderr, "Cannot open directory: %s\n", dir_path);
        return;
    }

    struct dirent *entry;
    int any_found = 0;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_type == DT_REG) {
            char db_path[MAX_PATH];
            snprintf(db_path, sizeof(db_path), "%s/%s", dir_path, entry->d_name);
	    if( strcmp(entry->d_name + (strlen( entry->d_name ) - 3), ".db") == 0 ) {
                search_database(entry->d_name, db_path, filename, partial);
                any_found = 1;
            }
        }
    }
    closedir(dir);

    if (!any_found) {
        printf("No databases found in %s\n", dir_path);
    }
}

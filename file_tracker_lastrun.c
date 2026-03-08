#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char *argv[]) {
    if (argc != 2) {
        printf("Usage: %s <file_tracker.db>\n", argv[0]);
        return 1;
    }

    const char *dbpath = argv[1];
    sqlite3 *db;

    if (sqlite3_open(dbpath, &db) != SQLITE_OK) {
        fprintf(stderr, "Cannot open database: %s\n", sqlite3_errmsg(db));
        return 1;
    }

    const char *sql =
        "SELECT value FROM metadata WHERE key='last_run';";

    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, NULL);

    if (rc != SQLITE_OK) {
        fprintf(stderr, "Error preparing SQL: %s\n", sqlite3_errmsg(db));
        sqlite3_close(db);
        return 1;
    }

    rc = sqlite3_step(stmt);

    if (rc == SQLITE_ROW) {
        const unsigned char *val = sqlite3_column_text(stmt, 0);
        printf("Last Run: %s\n", val);
    } else {
        printf("No last run information stored.\n");
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return 0;
}

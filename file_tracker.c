#include <dirent.h>
#include <openssl/evp.h>
#include <pthread.h>
#include <pwd.h>
#include <sqlite3.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <libgen.h>
#include <locale.h>
#include <errno.h>
#include <signal.h>

#define HASH_SIZE 65
#define MAX_PATH 4096
#define MAX_IGNORES 1024

// ==== Globals ====
int verbose = 0;
int update = 0;
int verifyChecksum = 0;
int showLiveProgress = 0;
int showLiveProgressPath = 0;
int showSummary = 0;
char *note_text = NULL;

// Aggregate counters (Protected by global_count_mutex)
int total_unchanged = 0, total_changed = 0, total_new = 0, total_missing = 0,
    total_ignored = 0, total_error = 0;

char *ignore_list[MAX_IGNORES];
int ignore_count = 0;

pthread_mutex_t global_count_mutex = PTHREAD_MUTEX_INITIALIZER;

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

typedef struct {
    char status[32];
    char path[MAX_PATH];
} LogEntry;

typedef struct {
    char source_path[MAX_PATH];
    char db_path[MAX_PATH];
    char log_path[MAX_PATH];
    FILE *log_fp;
    sqlite3 *db;
    sqlite3_int64 run_id;
    int unchanged, changed, new, missing, ignored, error;
    LogEntry *log_buffer;
    int log_count;
    int log_capacity;
} ThreadContext;

// ==== Ignore List Helpers ====
void load_ignore_list() {
    const char *home = getenv("HOME");
    char ignore_path[MAX_PATH];
    snprintf(ignore_path, sizeof(ignore_path), "%s/.rsync-ignore", home);

    FILE *f = fopen(ignore_path, "r");
    if (!f) return;

    char line[256];
    while (fgets(line, sizeof(line), f) && ignore_count < MAX_IGNORES) {
        line[strcspn(line, "\r\n")] = 0;
        if (strlen(line) > 0) {
            ignore_list[ignore_count++] = strdup(line);
        }
    }
    fclose(f);
}

int is_ignored(const char *name) {
    for (int i = 0; i < ignore_count; i++) {
        if (strcmp(name, ignore_list[i]) == 0) return 1;
    }
    return 0;
}

// ==== Logging Helper ====
void log_message(ThreadContext *ctx, const char *status, const char *path) {
    if (ctx->log_fp) {
        fprintf(ctx->log_fp, "[%-18s] %s\n", status, path);
        if (verbose) {
            fprintf(stdout, "[%s][%-18s] %s\n", basename(ctx->source_path), status, path);
        }
    }

    // Buffer log message for database insertion later
    if (ctx->log_count >= ctx->log_capacity) {
        ctx->log_capacity = ctx->log_capacity == 0 ? 1024 : ctx->log_capacity * 2;
        ctx->log_buffer = realloc(ctx->log_buffer, ctx->log_capacity * sizeof(LogEntry));
    }

    strncpy(ctx->log_buffer[ctx->log_count].status, status, sizeof(ctx->log_buffer[ctx->log_count].status) - 1);
    ctx->log_buffer[ctx->log_count].status[sizeof(ctx->log_buffer[ctx->log_count].status) - 1] = '\0';
    strncpy(ctx->log_buffer[ctx->log_count].path, path, sizeof(ctx->log_buffer[ctx->log_count].path) - 1);
    ctx->log_buffer[ctx->log_count].path[sizeof(ctx->log_buffer[ctx->log_count].path) - 1] = '\0';
    ctx->log_count++;
}

// ==== Utility Functions ====
void compute_sha256(const char *path, char *outputBuffer) {
    FILE *file = fopen(path, "rb");
    if (!file) {
        strcpy(outputBuffer, "");
        return;
    }
    EVP_MD_CTX *mdctx = EVP_MD_CTX_new();
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len;
    EVP_DigestInit_ex(mdctx, EVP_sha256(), NULL);
    const int bufSize = 32768;
    unsigned char *buffer = malloc(bufSize);
    int bytesRead;
    while ((bytesRead = fread(buffer, 1, bufSize, file))) {
        EVP_DigestUpdate(mdctx, buffer, bytesRead);
    }
    EVP_DigestFinal_ex(mdctx, hash, &hash_len);
    for (unsigned int i = 0; i < hash_len; i++) {
        sprintf(outputBuffer + (i * 2), "%02x", hash[i]);
    }
    outputBuffer[hash_len * 2] = '\0';
    EVP_MD_CTX_free(mdctx);
    fclose(file);
    free(buffer);
}

void get_owner(uid_t uid, char *owner, size_t size) {
    struct passwd *pw = getpwuid(uid);
    if (pw) snprintf(owner, size, "%s", pw->pw_name);
    else snprintf(owner, size, "%d", uid);
}

// ==== Utility: Create directory with parents ====
int mkdir_p(const char *path) {
    char tmp[MAX_PATH];
    char *p = NULL;
    size_t len;

    snprintf(tmp, sizeof(tmp), "%s", path);
    len = strlen(tmp);
    if (tmp[len - 1] == '/') tmp[len - 1] = 0;

    for (p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = 0;
            if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
                return -1;
            }
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) != 0 && errno != EEXIST) {
        return -1;
    }
    return 0;
}

// ==== Core Logic ====
void process_file(ThreadContext *ctx, const char *path, const char *name, sqlite3 *db) {
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return;

    if (showLiveProgress) {
        if (showLiveProgressPath)
            printf("\033[2K\r%s", path);
        else
            printf("\033[2K\r%s", name);
        fflush(stdout);
    }

    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db, "SELECT last_modified, checksum FROM files WHERE full_path = ? LIMIT 1", -1, &stmt, NULL);
    if (rc != SQLITE_OK) {
        fprintf(stderr, "SQLite prepare error: %s\n", sqlite3_errmsg(db));
        ctx->error++;
        return;
    }
    sqlite3_bind_text(stmt, 1, path, -1, SQLITE_STATIC);

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        time_t db_mtime = sqlite3_column_int64(stmt, 0);
        const char *db_checksum = (const char *)sqlite3_column_text(stmt, 1);
        int mtime_match = (db_mtime == st.st_mtime);

        if (!verifyChecksum && mtime_match) {
            log_message(ctx, "UNCHANGED", path);
            ctx->unchanged++;
        } else {
            char checksum[HASH_SIZE];
            compute_sha256(path, checksum);
            int checksum_match = (db_checksum && strcmp(checksum, db_checksum) == 0);

            if (verifyChecksum && checksum_match) {
                log_message(ctx, "UNCHANGED", path);
                ctx->unchanged++;
            } else {
                log_message(ctx, (!mtime_match) ? "CHANGED (Metadata)" : "CHANGED (Checksum)", path);
                if (update) {
                    sqlite3_stmt *up_stmt;
                    sqlite3_prepare_v2(db, "UPDATE files SET checksum = ?, last_modified = ? WHERE full_path = ?", -1, &up_stmt, NULL);
                    sqlite3_bind_text(up_stmt, 1, checksum, -1, SQLITE_STATIC);
                    sqlite3_bind_int64(up_stmt, 2, st.st_mtime);
                    sqlite3_bind_text(up_stmt, 3, path, -1, SQLITE_STATIC);
                    sqlite3_step(up_stmt);
                    sqlite3_finalize(up_stmt);
                }
                ctx->changed++;
            }
        }
    } else {
        log_message(ctx, "NEW", path);
        if (update) {
            char checksum[HASH_SIZE], owner[256];
            compute_sha256(path, checksum);
            get_owner(st.st_uid, owner, sizeof(owner));
            sqlite3_stmt *ins_stmt;
            sqlite3_prepare_v2(db, "INSERT INTO files (file_name, full_path, size, created, last_modified, owner, checksum) VALUES (?, ?, ?, ?, ?, ?, ?)", -1, &ins_stmt, NULL);
            sqlite3_bind_text(ins_stmt, 1, name, -1, SQLITE_STATIC);
            sqlite3_bind_text(ins_stmt, 2, path, -1, SQLITE_STATIC);
            sqlite3_bind_int64(ins_stmt, 3, st.st_size);
            sqlite3_bind_int64(ins_stmt, 4, st.st_ctime);
            sqlite3_bind_int64(ins_stmt, 5, st.st_mtime);
            sqlite3_bind_text(ins_stmt, 6, owner, -1, SQLITE_STATIC);
            sqlite3_bind_text(ins_stmt, 7, checksum, -1, SQLITE_STATIC);
            sqlite3_step(ins_stmt);
            sqlite3_finalize(ins_stmt);
        }
        ctx->new++;
    }
    sqlite3_finalize(stmt);
}

void traverse_directory(ThreadContext *ctx, const char *dir_path, sqlite3 *db) {
    DIR *dir = opendir(dir_path);
    if (!dir) return;
    struct dirent *entry;
    while ((entry = readdir(dir))) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        if (is_ignored(entry->d_name)) {
            ctx->ignored++;
            continue;
        }
        char full_path[MAX_PATH];
        struct stat st;
        int path_len = snprintf(full_path, sizeof(full_path), "%s/%s", dir_path, entry->d_name);
        if (path_len >= (int)sizeof(full_path)) {
            fprintf(stderr, "Warning: Path too long, skipping: %s/%s\n", dir_path, entry->d_name);
            ctx->error++;
            continue;
        }
        if (stat(full_path, &st) == 0) {
            if (S_ISDIR(st.st_mode)) {
                traverse_directory(ctx, full_path, db);
            } else if (strcmp(entry->d_name, "LastSyncDate") == 0) {
                ctx->ignored++;
            } else {
                process_file(ctx, full_path, entry->d_name, db);
            }
        }
    }
    closedir(dir);
}

void *path_worker(void *arg) {
    ThreadContext *ctx = (ThreadContext *)arg;

    if (sqlite3_open(ctx->db_path, &ctx->db) != SQLITE_OK) {
        fprintf(stderr, "Error: Failed to open database %s\n", ctx->db_path);
        if (ctx->log_fp) {
            fprintf(ctx->log_fp, "FATAL ERROR: Could not open database\n");
        }
        return NULL;
    }

    // Enable WAL mode for better concurrency
    sqlite3_exec(ctx->db, "PRAGMA journal_mode=WAL;", 0, 0, 0);
    sqlite3_exec(ctx->db, "PRAGMA synchronous=NORMAL;", 0, 0, 0);
    sqlite3_busy_timeout(ctx->db, 30000);  // Increased timeout for concurrent access

    sqlite3_exec(ctx->db, "CREATE TABLE IF NOT EXISTS files (id INTEGER PRIMARY KEY, file_name TEXT, full_path TEXT UNIQUE, size INTEGER, created INTEGER, last_modified INTEGER, owner TEXT, checksum TEXT, keywords TEXT);", 0, 0, 0);
    sqlite3_exec(ctx->db, "CREATE TABLE IF NOT EXISTS meta (id INTEGER PRIMARY KEY AUTOINCREMENT, last_checksum_verify_date TEXT, last_date_verify TEXT, verify_machine TEXT, num_unchanged INTEGER, num_changed INTEGER, num_new INTEGER, num_missing INTEGER, num_errors INTEGER, update_mode TEXT, note TEXT);", 0, 0, 0);
    sqlite3_exec(ctx->db, "CREATE TABLE IF NOT EXISTS run_logs (id INTEGER PRIMARY KEY AUTOINCREMENT, run_id INTEGER, status TEXT, full_path TEXT, FOREIGN KEY(run_id) REFERENCES meta(id));", 0, 0, 0);

    // Begin transaction for better performance and reduced lock contention
    sqlite3_exec(ctx->db, "BEGIN TRANSACTION;", 0, 0, 0);

    if( verbose ) printf("Beginning traversal of %s\n",ctx->source_path);
    traverse_directory(ctx, ctx->source_path, ctx->db);
    if( verbose ) printf("Traversal of %s complete\n",ctx->source_path);

    char **missing_paths = NULL;
    int missing_count = 0, missing_capacity = 0;

    if( verbose ) printf("Beginning Database Update\n");
    sqlite3_stmt *mStmt;
    sqlite3_prepare_v2(ctx->db, "SELECT full_path FROM files", -1, &mStmt, NULL);
    while (sqlite3_step(mStmt) == SQLITE_ROW) {
        const char *dp = (const char *)sqlite3_column_text(mStmt, 0);
        if (access(dp, F_OK) != 0) {
            // Expand array if needed
            if (missing_count >= missing_capacity) {
                missing_capacity = missing_capacity == 0 ? 32 : missing_capacity * 2;
                missing_paths = realloc(missing_paths, missing_capacity * sizeof(char*));
            }
            missing_paths[missing_count++] = strdup(dp);
        }
    }
    sqlite3_finalize(mStmt);
    if( verbose ) printf("Database Update Complete\n");

    // Set missing count before inserting meta record
    ctx->missing = missing_count;

    char hname[256];
    gethostname(hname, 256);
    char *sql;
    asprintf(&sql, "INSERT INTO meta (%s, verify_machine, num_unchanged, num_changed, num_new, num_missing, num_errors, update_mode, note) VALUES (datetime('now','localtime'), ?, ?, ?, ?, ?, ?, ?, ?)",
             verifyChecksum ? "last_checksum_verify_date" : "last_date_verify");
    sqlite3_stmt *insMeta;
    sqlite3_prepare_v2(ctx->db, sql, -1, &insMeta, NULL);
    sqlite3_bind_text(insMeta, 1, hname, -1, SQLITE_STATIC);
    sqlite3_bind_int(insMeta, 2, ctx->unchanged);
    sqlite3_bind_int(insMeta, 3, ctx->changed);
    sqlite3_bind_int(insMeta, 4, ctx->new);
    sqlite3_bind_int(insMeta, 5, ctx->missing);
    sqlite3_bind_int(insMeta, 6, ctx->error);
    sqlite3_bind_text(insMeta, 7, update ? "ON" : "OFF", -1, SQLITE_STATIC);
    sqlite3_bind_text(insMeta, 8, note_text, -1, SQLITE_STATIC);
    sqlite3_step(insMeta);
    sqlite3_finalize(insMeta);
    free(sql);

    // Get the run_id we just inserted
    ctx->run_id = sqlite3_last_insert_rowid(ctx->db);

    // Log and delete the collected missing paths (before inserting buffered logs)
    for (int i = 0; i < missing_count; i++) {
        if( verbose ) printf("Deleting missing files from the database\n");
        log_message(ctx, "MISSING", missing_paths[i]);
        if (update) {
            sqlite3_stmt *dStmt;
            sqlite3_prepare_v2(ctx->db, "DELETE FROM files WHERE full_path = ?", -1, &dStmt, NULL);
            sqlite3_bind_text(dStmt, 1, missing_paths[i], -1, SQLITE_STATIC);
            sqlite3_step(dStmt);
            sqlite3_finalize(dStmt);
        }
        free(missing_paths[i]);
        if( verbose ) printf("Completed deleting missing files from the database\n");
    }
    free(missing_paths);

    // Insert buffered log messages into database (includes MISSING from above)
    if (ctx->log_count > 0) {
        sqlite3_stmt *log_stmt;
        const char *log_sql = "INSERT INTO run_logs (run_id, status, full_path) VALUES (?, ?, ?)";
        sqlite3_prepare_v2(ctx->db, log_sql, -1, &log_stmt, NULL);

        for (int i = 0; i < ctx->log_count; i++) {
            sqlite3_bind_int64(log_stmt, 1, ctx->run_id);
            sqlite3_bind_text(log_stmt, 2, ctx->log_buffer[i].status, -1, SQLITE_STATIC);
            sqlite3_bind_text(log_stmt, 3, ctx->log_buffer[i].path, -1, SQLITE_STATIC);
            sqlite3_step(log_stmt);
            sqlite3_reset(log_stmt);
        }
        sqlite3_finalize(log_stmt);

        free(ctx->log_buffer);
        ctx->log_buffer = NULL;
        ctx->log_count = 0;
        ctx->log_capacity = 0;
    }

    // Commit transaction
    if( verbose ) printf("Committing Database Transaction\n");
    sqlite3_exec(ctx->db, "COMMIT;", 0, 0, 0);
    if( verbose ) printf("Database Transaction Commit Complete\n");

    sqlite3_close(ctx->db);
    // Note: log_fp is now closed in main() to allow appending the summary

    pthread_mutex_lock(&global_count_mutex);
    total_unchanged += ctx->unchanged;
    total_changed += ctx->changed;
    total_new += ctx->new;
    total_missing += ctx->missing;
    total_ignored += ctx->ignored;
    total_error += ctx->error;
    pthread_mutex_unlock(&global_count_mutex);

    return NULL;
}

int main(int argc, char *argv[]) {

    char *path_arg = NULL;
    char *db_name_arg = NULL;
    char *note_file_arg = NULL;

    int help_requested = 0;

    setlocale(LC_NUMERIC, "");

    setvbuf(stdout, NULL, _IONBF, 0);

    // Setup terminal wrapping control
    atexit(enable_line_wrap);
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGHUP, signal_handler);
    disable_line_wrap();

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-p") == 0) path_arg = argv[++i];
        else if (strcmp(argv[i], "-n") == 0) db_name_arg = argv[++i];
        else if (strcmp(argv[i], "-t") == 0) note_text = argv[++i];
        else if (strcmp(argv[i], "-N") == 0) note_file_arg = argv[++i];
        else if (strcmp(argv[i], "-c") == 0) verifyChecksum = 1;
        else if (strcmp(argv[i], "-u") == 0) update = 1;
        else if (strcmp(argv[i], "-v") == 0) verbose = 1;
        else if (strcmp(argv[i], "-l") == 0) showLiveProgress = 1;
        else if (strcmp(argv[i], "-L") == 0) showLiveProgressPath = 1;
        else if (strcmp(argv[i], "-h") == 0) help_requested = 1;
        else if (strcmp(argv[i], "-s") == 0) showSummary = 1;
    }

    if (showLiveProgressPath) showLiveProgress = 1;

    if (help_requested == 1) {
        fprintf(stderr, "Usage: %s -p /path1,/path2 [-n db_name] [-c] [-u] [-v] [-l] [-L] [-s] [-t note] [-N notefile]\n", argv[0]);
        fprintf(stderr, "  -p <paths>  Paths to scan (required, comma-separated)\n");
        fprintf(stderr, "  -n <name>   Database file name (without .db extension)\n");
        fprintf(stderr, "  -c          Verify checksums even if mtime unchanged\n");
        fprintf(stderr, "  -u          Update database with changes\n");
        fprintf(stderr, "  -v          Verbose output\n");
        fprintf(stderr, "  -l          Live view (show current filename being processed)\n");
        fprintf(stderr, "  -L          Live view with full path instead of filename. Implies -l\n");
        fprintf(stderr, "  -s          Show summary\n");
        fprintf(stderr, "  -t <note>   Add note text to this run (requires -u)\n");
        fprintf(stderr, "  -N <file>   Read note text from file for this run (requires -u)\n");
        exit(0);
    }

    if ( ! path_arg) {
        fprintf(stderr, "Error: -p option is required\n");
        fprintf(stderr, "Usage: %s -p /path1,/path2 [-n db_name] [-c] [-u] [-v] [-l] [-L] [-s] [-t note] [-N notefile]\n", argv[0]);
        exit(1);
    }

    // Validate note options
    if ((note_text || note_file_arg) && !update) {
        fprintf(stderr, "Error: Note options (-t or -N) require -u (update mode)\n");
        exit(1);
    }

    // Read note from file if specified
    if (note_file_arg) {
        FILE *note_fp = fopen(note_file_arg, "r");
        if (!note_fp) {
            fprintf(stderr, "Error: Could not open note file %s: %s\n", note_file_arg, strerror(errno));
            exit(1);
        }

        // Read entire file into note_text
        fseek(note_fp, 0, SEEK_END);
        long file_size = ftell(note_fp);
        fseek(note_fp, 0, SEEK_SET);

        note_text = malloc(file_size + 1);
        if (!note_text) {
            fprintf(stderr, "Error: Could not allocate memory for note text\n");
            fclose(note_fp);
            exit(1);
        }

        size_t bytes_read = fread(note_text, 1, file_size, note_fp);
        note_text[bytes_read] = '\0';
        fclose(note_fp);

        // Remove trailing newline if present
        if (bytes_read > 0 && note_text[bytes_read - 1] == '\n') {
            note_text[bytes_read - 1] = '\0';
        }
    }

    load_ignore_list();
    const char *home = getenv("HOME");

    char log_dir[MAX_PATH], db_dir[MAX_PATH];
    snprintf(log_dir, sizeof(log_dir), "%s/logs/FileTracker", home);
    snprintf(db_dir, sizeof(db_dir), "%s/db/FileTracker", home);

    if (mkdir_p(log_dir) != 0) {
        fprintf(stderr, "Warning: Could not create log directory %s: %s\n", log_dir, strerror(errno));
    }
    if (mkdir_p(db_dir) != 0) {
        fprintf(stderr, "Warning: Could not create db directory %s: %s\n", db_dir, strerror(errno));
    }

    // File counting now happens during traversal to avoid double-pass
    char *token = strtok(path_arg, ",");
    ThreadContext contexts[64];
    pthread_t threads[64];
    int thread_count = 0;

    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    char timestamp[64];
    strftime(timestamp, sizeof(timestamp), "%Y-%m-%d-%H-%M-%S", t);

    while (token && thread_count < 64) {
        strncpy(contexts[thread_count].source_path, token, MAX_PATH - 1);
        contexts[thread_count].source_path[MAX_PATH - 1] = '\0';
        char *path_copy = strdup(token);
        char *base = basename(path_copy);

        if (db_name_arg)
            snprintf(contexts[thread_count].db_path, MAX_PATH, "%s/db/FileTracker/%s.db", home, db_name_arg);
        else
            snprintf(contexts[thread_count].db_path, MAX_PATH, "%s/db/FileTracker/%s.db", home, base);

        snprintf(contexts[thread_count].log_path, MAX_PATH, "%s/logs/FileTracker/%s-%s.log", home, base, timestamp);

        contexts[thread_count].log_fp = fopen(contexts[thread_count].log_path, "w");
        contexts[thread_count].unchanged = contexts[thread_count].changed = 0;
        contexts[thread_count].new = contexts[thread_count].missing = 0;
        contexts[thread_count].ignored = contexts[thread_count].error = 0;
        contexts[thread_count].db = NULL;
        contexts[thread_count].run_id = 0;
        contexts[thread_count].log_buffer = NULL;
        contexts[thread_count].log_count = 0;
        contexts[thread_count].log_capacity = 0;

        if (pthread_create(&threads[thread_count], NULL, path_worker, &contexts[thread_count]) != 0) {
            fprintf(stderr, "Error: Failed to create thread for path %s: %s\n",
                    contexts[thread_count].source_path, strerror(errno));
            if (contexts[thread_count].log_fp) {
                fclose(contexts[thread_count].log_fp);
            }
            free(path_copy);
            continue;  // Skip this path but continue with others
        }

        free(path_copy);
        thread_count++;
        token = strtok(NULL, ",");
    }

    if (token != NULL) {
        fprintf(stderr, "Warning: Maximum of 64 paths supported. Additional paths ignored.\n");
    }

    for (int i = 0; i < thread_count; i++) {
        pthread_join(threads[i], NULL);
    }

    // Clear live progress line if it was displayed
    if (showLiveProgress) printf("\n");

    // Output and Log Summary
    const char *summary_header = "\n================ AGGREGATE SUMMARY ================\n";
    const char *summary_footer = "==================================================\n";

    if( showSummary ) printf("%s", summary_header);
    if( showSummary ) printf("Unchanged      : %'d\n", total_unchanged);
    if( showSummary ) printf("Changed        : %'d\n", total_changed);
    if( showSummary ) printf("New            : %'d\n", total_new);
    if( showSummary ) printf("Missing        : %'d\n", total_missing);
    if( showSummary ) printf("Ignored        : %'d\n", total_ignored);
    if( showSummary ) printf("Errors         : %'d\n", total_error);
    if( showSummary ) printf("%s", summary_footer);

    for (int i = 0; i < thread_count; i++) {
        if (contexts[i].log_fp) {
            fprintf(contexts[i].log_fp, "%s", summary_header);
            fprintf(contexts[i].log_fp, "Unchanged      : %d\n", total_unchanged);
            fprintf(contexts[i].log_fp, "Changed        : %d\n", total_changed);
            fprintf(contexts[i].log_fp, "New            : %d\n", total_new);
            fprintf(contexts[i].log_fp, "Missing        : %d\n", total_missing);
            fprintf(contexts[i].log_fp, "Ignored        : %d\n", total_ignored);
            fprintf(contexts[i].log_fp, "Errors         : %d\n", total_error);
            fprintf(contexts[i].log_fp, "%s", summary_footer);
            fclose(contexts[i].log_fp);
        }
    }

    // Display previous run's note if not in update mode
    if (!update && showSummary && thread_count > 0) {
        sqlite3 *db;
        if (sqlite3_open(contexts[0].db_path, &db) == SQLITE_OK) {
            sqlite3_stmt *stmt;
            const char *query = "SELECT note FROM meta WHERE note IS NOT NULL ORDER BY id DESC LIMIT 1";
            if (sqlite3_prepare_v2(db, query, -1, &stmt, NULL) == SQLITE_OK) {
                if (sqlite3_step(stmt) == SQLITE_ROW) {
                    const char *prev_note = (const char *)sqlite3_column_text(stmt, 0);
                    if (prev_note && strlen(prev_note) > 0) {
                        printf("\n============= PREVIOUS RUN'S NOTE ================\n");
                        printf("%s\n", prev_note);
                        printf("==================================================\n");
                    }
                }
                sqlite3_finalize(stmt);
            }
            sqlite3_close(db);
        }
    }

    for (int i = 0; i < ignore_count; i++) free(ignore_list[i]);
    if (note_file_arg && note_text) free(note_text);
    return 0;
}

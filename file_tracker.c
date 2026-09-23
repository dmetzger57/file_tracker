// file_tracker - command line scanner sharing the database with file_tracker_unified
//
// Usage: file_tracker -s source_path [-v] [-u] [-n note]

#include <sqlite3.h>
#include <openssl/evp.h>
#include <errno.h>
#include <dirent.h>
#include <fnmatch.h>
#include <limits.h>
#include <pwd.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define HASH_SIZE 65
#define MAX_PATH 4096
#define MAX_IGNORES 1024

typedef struct {
    char scan_path[MAX_PATH];
    char db_path[MAX_PATH];
    int verify_checksum;
    int update_mode;
    char note[1024];
    sqlite3 *db;
    sqlite3_stmt *find_stmt;
    sqlite3_stmt *log_stmt;
    sqlite3_int64 run_id;
    long total, processed;
    long unchanged, changed, new_files, missing, ignored, errors;
} Ctx;

static char *ignore_list[MAX_IGNORES];
static int ignore_dir_only[MAX_IGNORES];  // pattern had a trailing '/': matches directories only
static int ignore_anchored[MAX_IGNORES];  // pattern contains a '/': matched against the path below the scan root
static int ignore_count = 0;
static volatile sig_atomic_t should_stop = 0;

// ============================================================================
// Utilities (same behavior as file_tracker_unified)
// ============================================================================

static void get_timestamp(char *buffer, size_t size) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    strftime(buffer, size, "%Y-%m-%d %H:%M:%S", t);
}

static void load_ignore_list(void) {
    const char *home = getenv("HOME");
    if (!home) return;

    char ignore_path[MAX_PATH];
    snprintf(ignore_path, sizeof(ignore_path), "%s/.rsync-ignore", home);

    FILE *f = fopen(ignore_path, "r");
    if (!f) return;

    // rsync-style: blank lines and '#' comments are skipped, wildcards are allowed, a
    // trailing '/' limits the pattern to directories and a pattern containing '/' (e.g. a
    // leading one) is anchored to the scan root
    char line[256];
    while (fgets(line, sizeof(line), f) && ignore_count < MAX_IGNORES) {
        size_t len = strcspn(line, "\r\n");
        while (len > 0 && (line[len - 1] == ' ' || line[len - 1] == '\t')) len--;
        line[len] = 0;
        if (len == 0 || line[0] == '#') continue;

        int dir_only = 0;
        if (line[len - 1] == '/') {
            line[--len] = 0;
            dir_only = 1;
            if (len == 0) continue;
        }
        const char *pattern = line;
        int anchored = (strchr(pattern, '/') != NULL);
        while (*pattern == '/') pattern++;
        if (*pattern == 0) continue;

        ignore_dir_only[ignore_count] = dir_only;
        ignore_anchored[ignore_count] = anchored;
        ignore_list[ignore_count++] = strdup(pattern);
    }
    fclose(f);
}

// Path of path below root, without a leading '/'
static const char *path_below_root(const char *root, const char *path) {
    size_t root_len = strlen(root);
    const char *rel = strncmp(path, root, root_len) == 0 ? path + root_len : path;
    while (*rel == '/') rel++;
    return rel;
}

// rel_path is the entry's path below the scan root
static int is_ignored(const char *rel_path, int is_dir) {
    const char *slash = strrchr(rel_path, '/');
    const char *name = slash ? slash + 1 : rel_path;
    for (int i = 0; i < ignore_count; i++) {
        if (ignore_dir_only[i] && !is_dir) continue;
        if (ignore_anchored[i] ? fnmatch(ignore_list[i], rel_path, FNM_PATHNAME) == 0
                               : fnmatch(ignore_list[i], name, 0) == 0) return 1;
    }
    return 0;
}

// True if path, or any directory it is inside (below root), is ignored
static int path_is_ignored(const char *root, const char *path) {
    char rel[MAX_PATH];
    snprintf(rel, sizeof(rel), "%s", path_below_root(root, path));
    for (char *p = rel; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        int ignored = is_ignored(rel, 1);
        *p = '/';
        if (ignored) return 1;
    }
    return is_ignored(rel, 0);
}

static int compute_sha256(const char *path, char *output_buffer) {
    FILE *file = fopen(path, "rb");
    if (!file) return 0;

    EVP_MD_CTX *mdctx = EVP_MD_CTX_new();
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len;

    EVP_DigestInit_ex(mdctx, EVP_sha256(), NULL);

    unsigned char buffer[65536];
    size_t bytes;
    while ((bytes = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        EVP_DigestUpdate(mdctx, buffer, bytes);
    }
    int failed = ferror(file);
    fclose(file);

    if (failed) {
        EVP_MD_CTX_free(mdctx);
        return 0;
    }

    EVP_DigestFinal_ex(mdctx, hash, &hash_len);
    EVP_MD_CTX_free(mdctx);

    for (unsigned int i = 0; i < hash_len; i++) {
        sprintf(output_buffer + (i * 2), "%02x", hash[i]);
    }
    output_buffer[hash_len * 2] = '\0';
    return 1;
}

// ============================================================================
// Drive tracking (same drives.db as the Drives tab)
// ============================================================================

static void auto_add_or_update_drive(const char *drive_name, const char *source_path) {
    const char *home = getenv("HOME");
    if (!home) return;

    char drives_db_path[MAX_PATH];
    snprintf(drives_db_path, sizeof(drives_db_path), "%s/db/FileTracker/drives.db", home);

    sqlite3 *drives_db = NULL;
    if (sqlite3_open(drives_db_path, &drives_db) != SQLITE_OK) {
        if (drives_db) sqlite3_close(drives_db);
        return;
    }
    sqlite3_busy_timeout(drives_db, 30000);

    sqlite3_exec(drives_db,
        "CREATE TABLE IF NOT EXISTS drives ("
        "drive_id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "drive_name TEXT NOT NULL UNIQUE,"
        "capacity INTEGER,"
        "space_available INTEGER,"
        "space_used INTEGER,"
        "description TEXT,"
        "last_updated TEXT,"
        "last_verified TEXT,"
        "storage_container TEXT"
        ");", 0, 0, 0);

    long long capacity = 0, available = 0, used = 0;
    struct statfs fs;
    int has_stats = (statfs(source_path, &fs) == 0);
    if (has_stats) {
        capacity = (long long)fs.f_blocks * fs.f_bsize;
        available = (long long)fs.f_bavail * fs.f_bsize;
        used = capacity - (available + (long long)(fs.f_bfree - fs.f_bavail) * fs.f_bsize);
    }

    char timestamp[64];
    get_timestamp(timestamp, sizeof(timestamp));

    sqlite3_stmt *stmt;
    int exists = 0;
    if (sqlite3_prepare_v2(drives_db, "SELECT COUNT(*) FROM drives WHERE drive_name = ?;", -1, &stmt, 0) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, drive_name, -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW) exists = sqlite3_column_int(stmt, 0) > 0;
        sqlite3_finalize(stmt);
    }

    if (exists) {
        if (has_stats &&
            sqlite3_prepare_v2(drives_db,
                "UPDATE drives SET capacity = ?, space_available = ?, space_used = ?, last_updated = ? WHERE drive_name = ?;",
                -1, &stmt, 0) == SQLITE_OK) {
            sqlite3_bind_int64(stmt, 1, capacity);
            sqlite3_bind_int64(stmt, 2, available);
            sqlite3_bind_int64(stmt, 3, used);
            sqlite3_bind_text(stmt, 4, timestamp, -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 5, drive_name, -1, SQLITE_STATIC);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    } else if (has_stats) {
        if (sqlite3_prepare_v2(drives_db,
                "INSERT INTO drives (drive_name, capacity, space_available, space_used, description, last_updated) "
                "VALUES (?, ?, ?, ?, ?, ?);", -1, &stmt, 0) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, drive_name, -1, SQLITE_STATIC);
            sqlite3_bind_int64(stmt, 2, capacity);
            sqlite3_bind_int64(stmt, 3, available);
            sqlite3_bind_int64(stmt, 4, used);
            sqlite3_bind_text(stmt, 5, "Auto-added by file_tracker", -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 6, timestamp, -1, SQLITE_STATIC);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    } else {
        if (sqlite3_prepare_v2(drives_db,
                "INSERT INTO drives (drive_name, description, last_updated) VALUES (?, ?, ?);",
                -1, &stmt, 0) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, drive_name, -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 2, "Auto-added by file_tracker", -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 3, timestamp, -1, SQLITE_STATIC);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
    }

    sqlite3_close(drives_db);
}

// ============================================================================
// Scanning
// ============================================================================

static void log_entry(Ctx *c, const char *status, const char *path, const char *checksum,
                      long long size, long long mtime) {
    if (!c->log_stmt) return;
    sqlite3_bind_int64(c->log_stmt, 1, c->run_id);
    sqlite3_bind_text(c->log_stmt, 2, status, -1, SQLITE_STATIC);
    sqlite3_bind_text(c->log_stmt, 3, path, -1, SQLITE_STATIC);
    sqlite3_bind_text(c->log_stmt, 4, checksum ? checksum : "", -1, SQLITE_STATIC);
    sqlite3_bind_int64(c->log_stmt, 5, size);
    sqlite3_bind_int64(c->log_stmt, 6, mtime);
    sqlite3_step(c->log_stmt);
    sqlite3_reset(c->log_stmt);
}

static void insert_file(Ctx *c, const char *filepath, const char *filename,
                        const struct stat *sb, const char *checksum) {
    sqlite3_stmt *ins;
    if (sqlite3_prepare_v2(c->db,
            "INSERT INTO files (file_name, full_path, size, created, last_modified, owner, checksum) "
            "VALUES (?, ?, ?, ?, ?, ?, ?)", -1, &ins, NULL) != SQLITE_OK) return;
    struct passwd *pw = getpwuid(sb->st_uid);
    sqlite3_bind_text(ins, 1, filename, -1, SQLITE_STATIC);
    sqlite3_bind_text(ins, 2, filepath, -1, SQLITE_STATIC);
    sqlite3_bind_int64(ins, 3, sb->st_size);
    sqlite3_bind_int64(ins, 4, sb->st_ctime);
    sqlite3_bind_int64(ins, 5, sb->st_mtime);
    sqlite3_bind_text(ins, 6, pw ? pw->pw_name : "unknown", -1, SQLITE_STATIC);
    sqlite3_bind_text(ins, 7, checksum, -1, SQLITE_STATIC);
    sqlite3_step(ins);
    sqlite3_finalize(ins);
}

static int file_in_db(Ctx *c, const char *filepath) {
    if (!c->find_stmt) return 0;
    sqlite3_bind_text(c->find_stmt, 1, filepath, -1, SQLITE_STATIC);
    int found = (sqlite3_step(c->find_stmt) == SQLITE_ROW);
    sqlite3_reset(c->find_stmt);
    return found;
}

static void process_ignored(Ctx *c, const char *filepath, const char *filename) {
    struct stat sb;
    log_entry(c, "IGNORED", filepath, "", 0, 0);
    c->ignored++;

    // Like the GUI, record ignored regular files in the files table (no checksum)
    if (c->update_mode && stat(filepath, &sb) == 0 && S_ISREG(sb.st_mode) && !file_in_db(c, filepath)) {
        insert_file(c, filepath, filename, &sb, "");
    }
}

static void process_error(Ctx *c, const char *filepath) {
    c->errors++;
    log_entry(c, "ERROR", filepath, "", 0, 0);
}

static void process_file(Ctx *c, const char *filepath, const char *filename, const struct stat *sb) {
    char checksum[HASH_SIZE] = "";
    long long db_size = 0, db_mtime = 0;
    char db_checksum[HASH_SIZE] = "";
    int in_db = 0, reappeared = 0;

    if (c->find_stmt) {
        sqlite3_bind_text(c->find_stmt, 1, filepath, -1, SQLITE_STATIC);
        if (sqlite3_step(c->find_stmt) == SQLITE_ROW) {
            in_db = 1;
            db_size = sqlite3_column_int64(c->find_stmt, 0);
            db_mtime = sqlite3_column_int64(c->find_stmt, 1);
            const char *t = (const char *)sqlite3_column_text(c->find_stmt, 2);
            if (t) snprintf(db_checksum, sizeof(db_checksum), "%s", t);
            // A file previously marked MISSING that is back on disk is treated as new
            const char *st = (const char *)sqlite3_column_text(c->find_stmt, 3);
            if (st && strcmp(st, "MISSING") == 0) {
                in_db = 0;
                reappeared = 1;
            }
        }
        sqlite3_reset(c->find_stmt);
    }

    int have_checksum = 0;
    if (c->verify_checksum) {
        have_checksum = compute_sha256(filepath, checksum);
        if (!have_checksum) {
            process_error(c, filepath);
            return;
        }
    }

    if (!in_db) {
        c->new_files++;
        log_entry(c, "NEW", filepath, checksum, sb->st_size, sb->st_mtime);
        if (c->update_mode && reappeared) {
            sqlite3_stmt *up;
            if (sqlite3_prepare_v2(c->db, "UPDATE files SET size=?, last_modified=?, checksum=?, status=NULL WHERE full_path=?",
                                   -1, &up, NULL) == SQLITE_OK) {
                sqlite3_bind_int64(up, 1, sb->st_size);
                sqlite3_bind_int64(up, 2, sb->st_mtime);
                sqlite3_bind_text(up, 3, checksum, -1, SQLITE_STATIC);
                sqlite3_bind_text(up, 4, filepath, -1, SQLITE_STATIC);
                sqlite3_step(up);
                sqlite3_finalize(up);
            }
        } else if (c->update_mode) {
            insert_file(c, filepath, filename, sb, checksum);
        }
        return;
    }

    int sum_diff = have_checksum && db_checksum[0] && strcmp(db_checksum, checksum) != 0;
    int size_diff = db_size != sb->st_size;
    int time_diff = db_mtime != sb->st_mtime;

    if (!sum_diff && !size_diff && !time_diff) {
        c->unchanged++;
        log_entry(c, "UNCHANGED", filepath, db_checksum, sb->st_size, sb->st_mtime);
        return;
    }

    c->changed++;
    char reason[128] = "CHANGED:";
    const char *sep = " ";
    if (sum_diff)  { strcat(reason, sep); strcat(reason, "CheckSum");  sep = ", "; }
    if (time_diff) { strcat(reason, sep); strcat(reason, "Date-Time"); sep = ", "; }
    if (size_diff) { strcat(reason, sep); strcat(reason, "File-Size"); }
    log_entry(c, reason, filepath, checksum, sb->st_size, sb->st_mtime);

    if (c->update_mode) {
        // Store a checksum matching the new content so a later -v scan has a valid baseline
        if (!have_checksum) compute_sha256(filepath, checksum);
        sqlite3_stmt *up;
        if (sqlite3_prepare_v2(c->db, "UPDATE files SET size=?, last_modified=?, checksum=? WHERE full_path=?",
                               -1, &up, NULL) == SQLITE_OK) {
            sqlite3_bind_int64(up, 1, sb->st_size);
            sqlite3_bind_int64(up, 2, sb->st_mtime);
            sqlite3_bind_text(up, 3, checksum, -1, SQLITE_STATIC);
            sqlite3_bind_text(up, 4, filepath, -1, SQLITE_STATIC);
            sqlite3_step(up);
            sqlite3_finalize(up);
        }
    }
}

static int join_path(char *out, size_t size, const char *dir, const char *name) {
    int n = snprintf(out, size, "%s/%s", strcmp(dir, "/") == 0 ? "" : dir, name);
    return n > 0 && (size_t)n < size;
}

static long count_files(const char *root, const char *dirpath) {
    long count = 0;
    DIR *dir = opendir(dirpath);
    if (!dir) return 0;

    struct dirent *entry;
    while (!should_stop && (entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;

        char filepath[MAX_PATH];
        struct stat sb;
        if (!join_path(filepath, sizeof(filepath), dirpath, entry->d_name)) continue;
        if (stat(filepath, &sb) != 0) continue;
        if (is_ignored(path_below_root(root, filepath), S_ISDIR(sb.st_mode))) continue;
        if (S_ISDIR(sb.st_mode)) count += count_files(root, filepath);
        else if (S_ISREG(sb.st_mode)) count++;
    }
    closedir(dir);
    return count;
}

static void scan_directory(Ctx *c, const char *dirpath) {
    DIR *dir = opendir(dirpath);
    if (!dir) {
        process_error(c, dirpath);
        return;
    }

    struct dirent *entry;
    while (!should_stop && (entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;

        char filepath[MAX_PATH];
        if (!join_path(filepath, sizeof(filepath), dirpath, entry->d_name)) {
            process_error(c, dirpath);  // path too long
            continue;
        }

        struct stat sb;
        int stat_ok = (stat(filepath, &sb) == 0);
        int stat_errno = errno;

        if (is_ignored(path_below_root(c->scan_path, filepath), stat_ok && S_ISDIR(sb.st_mode))) {
            process_ignored(c, filepath, entry->d_name);
            continue;
        }

        if (!stat_ok) {
            // Dangling symlinks are skipped; anything else (e.g. permissions) is an error
            if (stat_errno != ENOENT) process_error(c, filepath);
            continue;
        }

        if (S_ISDIR(sb.st_mode)) {
            scan_directory(c, filepath);
        } else if (S_ISREG(sb.st_mode)) {
            process_file(c, filepath, entry->d_name, &sb);
            c->processed++;
        }
    }
    closedir(dir);
}

// Files recorded under the scan path that are gone from disk. In update mode they are marked
// MISSING in the files table; rows already marked MISSING are skipped so they are reported once.
static void find_missing(Ctx *c) {
    if (!c->db) return;

    char prefix[MAX_PATH];
    snprintf(prefix, sizeof(prefix), "%s%s", c->scan_path, strcmp(c->scan_path, "/") == 0 ? "" : "/");

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(c->db,
            "SELECT id, full_path FROM files WHERE substr(full_path, 1, length(?1)) = ?1 "
            "AND (status IS NULL OR status != 'MISSING')", -1, &stmt, NULL) != SQLITE_OK) return;
    sqlite3_bind_text(stmt, 1, prefix, -1, SQLITE_STATIC);

    // Collect ids first; the files table is modified after the query has finished
    sqlite3_int64 *ids = NULL;
    size_t id_count = 0, id_capacity = 0;
    while (!should_stop && sqlite3_step(stmt) == SQLITE_ROW) {
        const char *path = (const char *)sqlite3_column_text(stmt, 1);
        struct stat sb;
        if (!path || stat(path, &sb) == 0 || errno != ENOENT) continue;
        // Entries recorded before an ignore pattern matched them are not reported as missing
        if (path_is_ignored(c->scan_path, path)) continue;

        c->missing++;
        log_entry(c, "MISSING", path, "", 0, 0);
        if (id_count >= id_capacity) {
            id_capacity = id_capacity ? id_capacity * 2 : 64;
            ids = realloc(ids, id_capacity * sizeof(sqlite3_int64));
        }
        ids[id_count++] = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);

    if (c->update_mode && id_count > 0) {
        sqlite3_stmt *up;
        if (sqlite3_prepare_v2(c->db, "UPDATE files SET status = 'MISSING' WHERE id = ?", -1, &up, NULL) == SQLITE_OK) {
            for (size_t i = 0; i < id_count; i++) {
                sqlite3_bind_int64(up, 1, ids[i]);
                sqlite3_step(up);
                sqlite3_reset(up);
            }
            sqlite3_finalize(up);
        }
    }
    free(ids);
}

// ============================================================================
// Setup
// ============================================================================

static void usage(const char *prog) {
    fprintf(stderr,
            "Usage: %s -s source_path [-v] [-u] [-n note]\n"
            "  -s  Source path to scan (required)\n"
            "  -v  Verify via SHA-256 checksum (default: compare size and modification time)\n"
            "  -u  Update file details in the database (default: the scan is recorded but file details are not updated)\n"
            "  -n  Note to store with the scan run\n",
            prog);
}

static void on_signal(int sig) {
    (void)sig;
    should_stop = 1;
}

static int make_db_dir(const char *db_dir) {
    char parent[MAX_PATH];
    snprintf(parent, sizeof(parent), "%s", db_dir);
    char *slash = strrchr(parent, '/');
    if (slash) {
        *slash = '\0';
        mkdir(parent, 0755);
    }
    return (mkdir(db_dir, 0755) == 0 || errno == EEXIST) ? 0 : -1;
}

static int open_database(Ctx *c) {
    if (sqlite3_open(c->db_path, &c->db) != SQLITE_OK) {
        fprintf(stderr, "Error: cannot open database %s: %s\n", c->db_path, sqlite3_errmsg(c->db));
        return -1;
    }
    sqlite3_busy_timeout(c->db, 30000);
    sqlite3_exec(c->db, "CREATE TABLE IF NOT EXISTS files (id INTEGER PRIMARY KEY, file_name TEXT, full_path TEXT UNIQUE, size INTEGER, created INTEGER, last_modified INTEGER, owner TEXT, checksum TEXT, keywords TEXT, status TEXT);", 0, 0, 0);
    sqlite3_exec(c->db, "CREATE TABLE IF NOT EXISTS meta (id INTEGER PRIMARY KEY AUTOINCREMENT, last_checksum_verify_date TEXT, last_date_verify TEXT, verify_machine TEXT, num_unchanged INTEGER, num_changed INTEGER, num_new INTEGER, num_missing INTEGER, num_ignored INTEGER, num_errors INTEGER, update_mode TEXT, note TEXT);", 0, 0, 0);
    sqlite3_exec(c->db, "CREATE TABLE IF NOT EXISTS run_logs (id INTEGER PRIMARY KEY AUTOINCREMENT, run_id INTEGER, status TEXT, full_path TEXT, checksum TEXT, size INTEGER, mtime INTEGER, FOREIGN KEY(run_id) REFERENCES meta(id));", 0, 0, 0);

    // Older databases have no files.status column; add it so files can be marked MISSING
    sqlite3_stmt *stmt;
    int has_status = 0;
    if (sqlite3_prepare_v2(c->db, "PRAGMA table_info(files)", -1, &stmt, NULL) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *col = (const char *)sqlite3_column_text(stmt, 1);
            if (col && strcmp(col, "status") == 0) has_status = 1;
        }
        sqlite3_finalize(stmt);
    }
    if (!has_status) sqlite3_exec(c->db, "ALTER TABLE files ADD COLUMN status TEXT;", 0, 0, 0);

    if (sqlite3_prepare_v2(c->db, "SELECT size, last_modified, checksum, status FROM files WHERE full_path = ?",
                           -1, &c->find_stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "Error: %s is not a File Tracker database: %s\n", c->db_path, sqlite3_errmsg(c->db));
        return -1;
    }
    return 0;
}

int main(int argc, char *argv[]) {
    static Ctx ctx;
    const char *source = NULL;
    int opt;

    while ((opt = getopt(argc, argv, "s:vun:h")) != -1) {
        switch (opt) {
            case 's': source = optarg; break;
            case 'v': ctx.verify_checksum = 1; break;
            case 'u': ctx.update_mode = 1; break;
            case 'n': snprintf(ctx.note, sizeof(ctx.note), "%s", optarg); break;
            default: usage(argv[0]); return opt == 'h' ? 0 : 1;
        }
    }
    if (!source || optind != argc) {
        usage(argv[0]);
        return 1;
    }

    const char *home = getenv("HOME");
    if (!home) {
        fprintf(stderr, "Error: HOME not set\n");
        return 1;
    }

    // Relative paths are made absolute so stored paths match those from the GUI
    if (source[0] == '/') {
        snprintf(ctx.scan_path, sizeof(ctx.scan_path), "%s", source);
    } else if (!realpath(source, ctx.scan_path)) {
        fprintf(stderr, "Error: %s: %s\n", source, strerror(errno));
        return 1;
    }
    size_t len = strlen(ctx.scan_path);
    while (len > 1 && ctx.scan_path[len - 1] == '/') ctx.scan_path[--len] = '\0';

    struct stat sb;
    if (stat(ctx.scan_path, &sb) != 0 || !S_ISDIR(sb.st_mode)) {
        fprintf(stderr, "Error: %s is not a directory\n", ctx.scan_path);
        return 1;
    }

    // Database name is the last path component, as in the Scanner tab
    const char *db_name = strrchr(ctx.scan_path, '/') + 1;
    if (db_name[0] == '\0') {
        fprintf(stderr, "Error: cannot derive a database name from %s\n", ctx.scan_path);
        return 1;
    }

    char db_dir[MAX_PATH];
    snprintf(db_dir, sizeof(db_dir), "%s/db/FileTracker", home);
    if (make_db_dir(db_dir) != 0) {
        fprintf(stderr, "Error: cannot create %s: %s\n", db_dir, strerror(errno));
        return 1;
    }
    snprintf(ctx.db_path, sizeof(ctx.db_path), "%s/%s.db", db_dir, db_name);

    if (open_database(&ctx) != 0) return 1;

    load_ignore_list();
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    {
        sqlite3_exec(ctx.db, "BEGIN TRANSACTION", 0, 0, 0);

        // Create the run record first so per-file log rows can reference it; counts are filled in at the end
        char timestamp[64], hostname[256];
        get_timestamp(timestamp, sizeof(timestamp));
        gethostname(hostname, sizeof(hostname));

        sqlite3_stmt *stmt;
        sqlite3_prepare_v2(ctx.db, "INSERT INTO meta (last_checksum_verify_date, last_date_verify, verify_machine, update_mode, note) VALUES (?, ?, ?, ?, ?)", -1, &stmt, 0);
        sqlite3_bind_text(stmt, 1, ctx.verify_checksum ? timestamp : NULL, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, timestamp, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 3, hostname, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 4, ctx.update_mode ? "ON" : "OFF", -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 5, ctx.note, -1, SQLITE_STATIC);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        ctx.run_id = sqlite3_last_insert_rowid(ctx.db);

        sqlite3_prepare_v2(ctx.db, "INSERT INTO run_logs (run_id, status, full_path, checksum, size, mtime) VALUES (?, ?, ?, ?, ?, ?)", -1, &ctx.log_stmt, 0);
    }

    ctx.total = count_files(ctx.scan_path, ctx.scan_path);
    scan_directory(&ctx, ctx.scan_path);
    find_missing(&ctx);

    {
        sqlite3_finalize(ctx.log_stmt);

        sqlite3_stmt *stmt;
        sqlite3_prepare_v2(ctx.db, "UPDATE meta SET num_unchanged=?, num_changed=?, num_new=?, num_missing=?, num_ignored=?, num_errors=? WHERE id=?", -1, &stmt, 0);
        sqlite3_bind_int64(stmt, 1, ctx.unchanged);
        sqlite3_bind_int64(stmt, 2, ctx.changed);
        sqlite3_bind_int64(stmt, 3, ctx.new_files);
        sqlite3_bind_int64(stmt, 4, ctx.missing);
        sqlite3_bind_int64(stmt, 5, ctx.ignored);
        sqlite3_bind_int64(stmt, 6, ctx.errors);
        sqlite3_bind_int64(stmt, 7, ctx.run_id);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        sqlite3_exec(ctx.db, "COMMIT", 0, 0, 0);
    }
    sqlite3_finalize(ctx.find_stmt);
    sqlite3_close(ctx.db);

    if (ctx.update_mode) auto_add_or_update_drive(db_name, ctx.scan_path);

    printf("Total: %ld - Processed: %ld - New: %ld - Changed: %ld - Unchanged: %ld - Missing: %ld - Errors: %ld\n",
           ctx.total, ctx.processed, ctx.new_files, ctx.changed, ctx.unchanged, ctx.missing, ctx.errors);

    for (int i = 0; i < ignore_count; i++) free(ignore_list[i]);
    return should_stop ? 130 : 0;
}

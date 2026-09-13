#include <gtk/gtk.h>
#include <sqlite3.h>
#include <openssl/evp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <dirent.h>
#include <unistd.h>
#include <pwd.h>

#define HASH_SIZE 65
#define MAX_PATH 4096

// Log entry for run_logs table
typedef struct {
    char status[32];
    char path[MAX_PATH];
} LogEntry;

// Global UI elements
GtkWidget *window;
GtkWidget *volumes_list;
GtkWidget *path_entry;
GtkWidget *db_name_entry;
GtkWidget *checksum_check;
GtkWidget *update_check;
GtkWidget *note_text;
GtkWidget *start_button;
GtkWidget *stop_button;
GtkProgressBar *progress_bar;
GtkWidget *status_label;
GtkWidget *current_file_label;
GtkWidget *results_text;

// Scan state
typedef struct {
    char scan_path[MAX_PATH];
    char db_path[MAX_PATH];
    char db_name[256];
    int enable_checksum;
    int update_mode;
    char note[1024];
    sqlite3 *db;
    sqlite3_int64 run_id;
    int unchanged, changed, new_files, missing, errors;
    int total_files;
    int should_stop;
    LogEntry *log_buffer;
    int log_count;
    int log_capacity;
    GThread *scan_thread;
} ScanContext;

ScanContext *current_scan = NULL;

// ==== Utility Functions ====

void get_timestamp(char *buffer, size_t size) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    strftime(buffer, size, "%Y-%m-%d %H:%M:%S", t);
}

char *get_file_owner(struct stat *sb) {
    struct passwd *pw = getpwuid(sb->st_uid);
    return pw ? pw->pw_name : "unknown";
}

void log_message(ScanContext *ctx, const char *status, const char *path) {
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

// Log error message with descriptive text
void log_error(ScanContext *ctx, const char *error_msg) {
    log_message(ctx, "ERROR", error_msg);
    ctx->errors++;
}

int compute_sha256(const char *path, char *output_buffer) {
    FILE *file = fopen(path, "rb");
    if (!file) return 0;

    EVP_MD_CTX *mdctx = EVP_MD_CTX_new();
    const EVP_MD *md = EVP_sha256();
    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_len;

    EVP_DigestInit_ex(mdctx, md, NULL);

    unsigned char buffer[8192];
    size_t bytes;
    while ((bytes = fread(buffer, 1, sizeof(buffer), file)) > 0) {
        EVP_DigestUpdate(mdctx, buffer, bytes);
    }

    EVP_DigestFinal_ex(mdctx, hash, &hash_len);
    EVP_MD_CTX_free(mdctx);
    fclose(file);

    for (unsigned int i = 0; i < hash_len; i++) {
        sprintf(output_buffer + (i * 2), "%02x", hash[i]);
    }
    output_buffer[hash_len * 2] = '\0';

    return 1;
}

// ==== Database Functions ====

int init_database(sqlite3 *db) {
    const char *sql_files = "CREATE TABLE IF NOT EXISTS files ("
        "id INTEGER PRIMARY KEY, "
        "file_name TEXT, "
        "full_path TEXT UNIQUE, "
        "size INTEGER, "
        "created INTEGER, "
        "last_modified INTEGER, "
        "owner TEXT, "
        "checksum TEXT, "
        "keywords TEXT);";

    const char *sql_meta = "CREATE TABLE IF NOT EXISTS meta ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, "
        "last_checksum_verify_date TEXT, "
        "last_date_verify TEXT, "
        "verify_machine TEXT, "
        "num_unchanged INTEGER, "
        "num_changed INTEGER, "
        "num_new INTEGER, "
        "num_missing INTEGER, "
        "num_errors INTEGER, "
        "update_mode TEXT, "
        "note TEXT);";

    const char *sql_logs = "CREATE TABLE IF NOT EXISTS run_logs ("
        "id INTEGER PRIMARY KEY AUTOINCREMENT, "
        "run_id INTEGER, "
        "status TEXT, "
        "full_path TEXT, "
        "FOREIGN KEY(run_id) REFERENCES meta(id));";

    char *err = NULL;
    if (sqlite3_exec(db, sql_files, 0, 0, &err) != SQLITE_OK ||
        sqlite3_exec(db, sql_meta, 0, 0, &err) != SQLITE_OK ||
        sqlite3_exec(db, sql_logs, 0, 0, &err) != SQLITE_OK) {
        if (err) {
            fprintf(stderr, "SQL error: %s\n", err);
            sqlite3_free(err);
        }
        return 0;
    }

    return 1;
}

// ==== GUI Update Functions (must run in main thread) ====

gboolean update_progress(gpointer data) {
    ScanContext *ctx = (ScanContext *)data;

    int processed = ctx->unchanged + ctx->changed + ctx->new_files + ctx->missing + ctx->errors;
    int remaining = ctx->total_files > processed ? ctx->total_files - processed : 0;

    if (ctx->total_files > 0) {
        double fraction = (double)processed / ctx->total_files;
        gtk_progress_bar_set_fraction(progress_bar, fraction);
    }

    char status[512];
    snprintf(status, sizeof(status),
             "Total: %d | Processed: %d | Remaining: %d | Unchanged: %d | Changed: %d | New: %d | Missing: %d | Errors: %d",
             ctx->total_files, processed, remaining,
             ctx->unchanged, ctx->changed, ctx->new_files, ctx->missing, ctx->errors);
    gtk_label_set_text(GTK_LABEL(status_label), status);

    return G_SOURCE_REMOVE;
}

gboolean update_current_file(gpointer data) {
    char *filename = (char *)data;
    gtk_label_set_text(GTK_LABEL(current_file_label), filename);
    g_free(filename);
    return G_SOURCE_REMOVE;
}

gboolean scan_completed(gpointer data) {
    ScanContext *ctx = (ScanContext *)data;

    gtk_widget_set_sensitive(start_button, TRUE);
    gtk_widget_set_sensitive(stop_button, FALSE);
    gtk_widget_set_sensitive(path_entry, TRUE);
    gtk_widget_set_sensitive(db_name_entry, TRUE);

    gtk_progress_bar_set_fraction(progress_bar, 1.0);

    // Update results
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(results_text));
    char results[2048];
    snprintf(results, sizeof(results),
             "Scan Complete!\n\n"
             "Path: %s\n"
             "Database: %s\n"
             "Mode: %s\n"
             "Checksum: %s\n\n"
             "Results:\n"
             "  Unchanged: %d files\n"
             "  Changed:   %d files\n"
             "  New:       %d files\n"
             "  Missing:   %d files\n"
             "  Errors:    %d files\n\n"
             "Total processed: %d files",
             ctx->scan_path,
             ctx->db_name,
             ctx->update_mode ? "Update" : "Read-only",
             ctx->enable_checksum ? "Enabled" : "Disabled",
             ctx->unchanged,
             ctx->changed,
             ctx->new_files,
             ctx->missing,
             ctx->errors,
             ctx->unchanged + ctx->changed + ctx->new_files + ctx->missing + ctx->errors);

    gtk_text_buffer_set_text(buffer, results, -1);

    if (ctx->db) {
        sqlite3_close(ctx->db);
        ctx->db = NULL;
    }

    return G_SOURCE_REMOVE;
}

// ==== Scanning Logic ====

void process_file(ScanContext *ctx, const char *filepath, const char *filename) {
    if (ctx->should_stop) return;

    g_idle_add(update_current_file, g_strdup(filename));

    struct stat sb;
    if (stat(filepath, &sb) != 0) {
        char err_msg[MAX_PATH + 64];
        snprintf(err_msg, sizeof(err_msg), "stat() failed for: %s", filepath);
        log_error(ctx, err_msg);
        g_idle_add(update_progress, ctx);
        return;
    }

    if (!S_ISREG(sb.st_mode)) return;

    // Query existing file
    const char *sql = "SELECT size, last_modified, checksum FROM files WHERE full_path = ?;";
    sqlite3_stmt *stmt;

    if (sqlite3_prepare_v2(ctx->db, sql, -1, &stmt, 0) != SQLITE_OK) {
        char err_msg[512];
        snprintf(err_msg, sizeof(err_msg), "SQLite prepare error: %s", sqlite3_errmsg(ctx->db));
        log_error(ctx, err_msg);
        g_idle_add(update_progress, ctx);
        return;
    }

    sqlite3_bind_text(stmt, 1, filepath, -1, SQLITE_STATIC);

    int rc = sqlite3_step(stmt);

    if (rc == SQLITE_ROW) {
        // File exists in database
        long long db_size = sqlite3_column_int64(stmt, 0);
        long long db_mtime = sqlite3_column_int64(stmt, 1);
        const char *db_checksum = (const char *)sqlite3_column_text(stmt, 2);

        if (db_size != sb.st_size || db_mtime != sb.st_mtime) {
            // File changed
            ctx->changed++;
            log_message(ctx, "CHANGED", filepath);

            if (ctx->update_mode) {
                char checksum[HASH_SIZE] = "";
                if (ctx->enable_checksum) {
                    compute_sha256(filepath, checksum);
                }

                const char *update_sql = "UPDATE files SET size=?, last_modified=?, checksum=? WHERE full_path=?;";
                sqlite3_stmt *update_stmt;
                if (sqlite3_prepare_v2(ctx->db, update_sql, -1, &update_stmt, 0) == SQLITE_OK) {
                    sqlite3_bind_int64(update_stmt, 1, sb.st_size);
                    sqlite3_bind_int64(update_stmt, 2, sb.st_mtime);
                    sqlite3_bind_text(update_stmt, 3, checksum, -1, SQLITE_STATIC);
                    sqlite3_bind_text(update_stmt, 4, filepath, -1, SQLITE_STATIC);
                    sqlite3_step(update_stmt);
                    sqlite3_finalize(update_stmt);
                }
            }
        } else if (ctx->enable_checksum) {
            // Check checksum
            char checksum[HASH_SIZE];
            if (compute_sha256(filepath, checksum)) {
                if (db_checksum && strcmp(checksum, db_checksum) == 0) {
                    ctx->unchanged++;
                    log_message(ctx, "UNCHANGED", filepath);
                } else {
                    ctx->changed++;
                    log_message(ctx, "CHANGED", filepath);

                    if (ctx->update_mode) {
                        const char *update_sql = "UPDATE files SET checksum=? WHERE full_path=?;";
                        sqlite3_stmt *update_stmt;
                        if (sqlite3_prepare_v2(ctx->db, update_sql, -1, &update_stmt, 0) == SQLITE_OK) {
                            sqlite3_bind_text(update_stmt, 1, checksum, -1, SQLITE_STATIC);
                            sqlite3_bind_text(update_stmt, 2, filepath, -1, SQLITE_STATIC);
                            sqlite3_step(update_stmt);
                            sqlite3_finalize(update_stmt);
                        }
                    }
                }
            }
        } else {
            ctx->unchanged++;
            log_message(ctx, "UNCHANGED", filepath);
        }
    } else {
        // New file
        ctx->new_files++;
        log_message(ctx, "NEW", filepath);

        if (ctx->update_mode) {
            char checksum[HASH_SIZE] = "";
            if (ctx->enable_checksum) {
                compute_sha256(filepath, checksum);
            }

            const char *insert_sql = "INSERT INTO files (file_name, full_path, size, created, last_modified, owner, checksum) "
                                    "VALUES (?, ?, ?, ?, ?, ?, ?);";
            sqlite3_stmt *insert_stmt;
            if (sqlite3_prepare_v2(ctx->db, insert_sql, -1, &insert_stmt, 0) == SQLITE_OK) {
                sqlite3_bind_text(insert_stmt, 1, filename, -1, SQLITE_STATIC);
                sqlite3_bind_text(insert_stmt, 2, filepath, -1, SQLITE_STATIC);
                sqlite3_bind_int64(insert_stmt, 3, sb.st_size);
                sqlite3_bind_int64(insert_stmt, 4, sb.st_ctime);
                sqlite3_bind_int64(insert_stmt, 5, sb.st_mtime);
                sqlite3_bind_text(insert_stmt, 6, get_file_owner(&sb), -1, SQLITE_STATIC);
                sqlite3_bind_text(insert_stmt, 7, checksum, -1, SQLITE_STATIC);
                sqlite3_step(insert_stmt);
                sqlite3_finalize(insert_stmt);
            }
        }
    }

    sqlite3_finalize(stmt);
    g_idle_add(update_progress, ctx);
}

void scan_directory(ScanContext *ctx, const char *dirpath) {
    if (ctx->should_stop) return;

    DIR *dir = opendir(dirpath);
    if (!dir) {
        char err_msg[MAX_PATH + 64];
        snprintf(err_msg, sizeof(err_msg), "Failed to open directory: %s", dirpath);
        log_error(ctx, err_msg);
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (ctx->should_stop) break;

        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        char filepath[MAX_PATH];
        snprintf(filepath, sizeof(filepath), "%s/%s", dirpath, entry->d_name);

        struct stat sb;
        if (stat(filepath, &sb) == 0) {
            if (S_ISDIR(sb.st_mode)) {
                scan_directory(ctx, filepath);
            } else if (S_ISREG(sb.st_mode)) {
                process_file(ctx, filepath, entry->d_name);
            }
        }
    }

    closedir(dir);
}

int count_files_recursive(const char *dirpath) {
    int count = 0;
    DIR *dir = opendir(dirpath);
    if (!dir) return 0;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        char filepath[MAX_PATH];
        snprintf(filepath, sizeof(filepath), "%s/%s", dirpath, entry->d_name);

        struct stat sb;
        if (stat(filepath, &sb) == 0) {
            if (S_ISDIR(sb.st_mode)) {
                count += count_files_recursive(filepath);
            } else if (S_ISREG(sb.st_mode)) {
                count++;
            }
        }
    }

    closedir(dir);
    return count;
}

// ==== Drive Tracking Integration ====

int get_drive_stats(const char *path, long long *capacity, long long *available, long long *used) {
    struct statfs fs_stats;
    if (statfs(path, &fs_stats) != 0) {
        return 0;
    }

    *capacity = (long long)fs_stats.f_blocks * fs_stats.f_bsize;
    *available = (long long)fs_stats.f_bavail * fs_stats.f_bsize;
    *used = *capacity - (*available + (long long)(fs_stats.f_bfree - fs_stats.f_bavail) * fs_stats.f_bsize);

    return 1;
}

int drive_exists_in_tracker(const char *drive_name) {
    const char *home = getenv("HOME");
    if (!home) return 0;

    char drives_db_path[MAX_PATH];
    snprintf(drives_db_path, sizeof(drives_db_path), "%s/db/FileTracker/drives.db", home);

    sqlite3 *drives_db = NULL;
    if (sqlite3_open(drives_db_path, &drives_db) != SQLITE_OK) {
        if (drives_db) sqlite3_close(drives_db);
        return 0;
    }

    const char *sql = "SELECT COUNT(*) FROM drives WHERE drive_name = ?;";
    sqlite3_stmt *stmt;
    int exists = 0;

    if (sqlite3_prepare_v2(drives_db, sql, -1, &stmt, 0) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, drive_name, -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            exists = sqlite3_column_int(stmt, 0) > 0;
        }
        sqlite3_finalize(stmt);
    }

    sqlite3_close(drives_db);
    return exists;
}

void add_drive_to_tracker(const char *drive_name, const char *source_path) {
    const char *home = getenv("HOME");
    if (!home) return;

    char drives_db_path[MAX_PATH];
    snprintf(drives_db_path, sizeof(drives_db_path), "%s/db/FileTracker/drives.db", home);

    sqlite3 *drives_db = NULL;
    if (sqlite3_open(drives_db_path, &drives_db) != SQLITE_OK) {
        if (drives_db) sqlite3_close(drives_db);
        return;
    }

    // Initialize drives schema if needed
    const char *create_table =
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
        ");";

    sqlite3_exec(drives_db, create_table, 0, 0, 0);

    // Try to get drive stats
    long long capacity = 0, available = 0, used = 0;
    int has_stats = get_drive_stats(source_path, &capacity, &available, &used);

    char timestamp[64];
    get_timestamp(timestamp, sizeof(timestamp));

    const char *insert_sql = has_stats ?
        "INSERT INTO drives (drive_name, capacity, space_available, space_used, description, last_updated) "
        "VALUES (?, ?, ?, ?, ?, ?);" :
        "INSERT INTO drives (drive_name, description, last_updated) VALUES (?, ?, ?);";

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(drives_db, insert_sql, -1, &stmt, 0) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, drive_name, -1, SQLITE_STATIC);

        if (has_stats) {
            sqlite3_bind_int64(stmt, 2, capacity);
            sqlite3_bind_int64(stmt, 3, available);
            sqlite3_bind_int64(stmt, 4, used);
            sqlite3_bind_text(stmt, 5, "Auto-added by file_tracker_gui", -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 6, timestamp, -1, SQLITE_STATIC);
        } else {
            sqlite3_bind_text(stmt, 2, "Auto-added by file_tracker_gui", -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 3, timestamp, -1, SQLITE_STATIC);
        }

        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    sqlite3_close(drives_db);
}

void auto_add_drive_to_tracker(ScanContext *ctx) {
    // Extract drive name from db_name
    if (strlen(ctx->db_name) == 0) return;

    // Check if drive already exists
    if (!drive_exists_in_tracker(ctx->db_name)) {
        add_drive_to_tracker(ctx->db_name, ctx->scan_path);
    }
}

gpointer scan_thread_func(gpointer data) {
    ScanContext *ctx = (ScanContext *)data;

    // Open database
    if (sqlite3_open(ctx->db_path, &ctx->db) != SQLITE_OK) {
        g_idle_add(scan_completed, ctx);
        return NULL;
    }

    init_database(ctx->db);

    // Count total files for progress
    g_idle_add(update_current_file, g_strdup("Counting files..."));
    ctx->total_files = count_files_recursive(ctx->scan_path);

    // Scan directory
    scan_directory(ctx, ctx->scan_path);

    // Save metadata
    if (ctx->update_mode) {
        char timestamp[64];
        get_timestamp(timestamp, sizeof(timestamp));

        char hostname[256];
        gethostname(hostname, sizeof(hostname));

        const char *sql = "INSERT INTO meta (last_checksum_verify_date, last_date_verify, verify_machine, "
                         "num_unchanged, num_changed, num_new, num_missing, num_errors, update_mode, note) "
                         "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?);";

        sqlite3_stmt *stmt;
        if (sqlite3_prepare_v2(ctx->db, sql, -1, &stmt, 0) == SQLITE_OK) {
            if (ctx->enable_checksum) {
                sqlite3_bind_text(stmt, 1, timestamp, -1, SQLITE_STATIC);
            } else {
                sqlite3_bind_null(stmt, 1);
            }
            sqlite3_bind_text(stmt, 2, timestamp, -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 3, hostname, -1, SQLITE_STATIC);
            sqlite3_bind_int(stmt, 4, ctx->unchanged);
            sqlite3_bind_int(stmt, 5, ctx->changed);
            sqlite3_bind_int(stmt, 6, ctx->new_files);
            sqlite3_bind_int(stmt, 7, ctx->missing);
            sqlite3_bind_int(stmt, 8, ctx->errors);
            sqlite3_bind_text(stmt, 9, "ON", -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 10, ctx->note, -1, SQLITE_STATIC);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }

        // Get the run_id we just inserted
        ctx->run_id = sqlite3_last_insert_rowid(ctx->db);

        // Insert buffered log messages into run_logs
        if (ctx->log_count > 0) {
            sqlite3_stmt *log_stmt;
            const char *log_sql = "INSERT INTO run_logs (run_id, status, full_path) VALUES (?, ?, ?)";
            if (sqlite3_prepare_v2(ctx->db, log_sql, -1, &log_stmt, 0) == SQLITE_OK) {
                for (int i = 0; i < ctx->log_count; i++) {
                    sqlite3_bind_int64(log_stmt, 1, ctx->run_id);
                    sqlite3_bind_text(log_stmt, 2, ctx->log_buffer[i].status, -1, SQLITE_STATIC);
                    sqlite3_bind_text(log_stmt, 3, ctx->log_buffer[i].path, -1, SQLITE_STATIC);
                    sqlite3_step(log_stmt);
                    sqlite3_reset(log_stmt);
                }
                sqlite3_finalize(log_stmt);
            }

            free(ctx->log_buffer);
            ctx->log_buffer = NULL;
            ctx->log_count = 0;
            ctx->log_capacity = 0;
        }

        // Auto-add drive to drive tracker
        auto_add_drive_to_tracker(ctx);
    }

    g_idle_add(scan_completed, ctx);
    return NULL;
}

// ==== Volumes List Functions ====

void refresh_volumes_list(void) {
    GtkListBox *list = GTK_LIST_BOX(volumes_list);

    // Clear existing entries
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(GTK_WIDGET(list))) != NULL) {
        gtk_list_box_remove(list, child);
    }

    // Scan /Volumes directory
    DIR *dir = opendir("/Volumes");
    if (!dir) return;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue; // Skip hidden

        char full_path[MAX_PATH];
        snprintf(full_path, sizeof(full_path), "/Volumes/%s", entry->d_name);

        struct stat sb;
        if (stat(full_path, &sb) == 0 && S_ISDIR(sb.st_mode)) {
            // Get volume info
            struct statfs fs_stats;
            long long capacity = 0;
            long long available = 0;

            if (statfs(full_path, &fs_stats) == 0) {
                capacity = (long long)fs_stats.f_blocks * fs_stats.f_bsize;
                available = (long long)fs_stats.f_bavail * fs_stats.f_bsize;
            }

            // Create list item
            GtkWidget *row = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
            gtk_widget_set_margin_start(row, 8);
            gtk_widget_set_margin_end(row, 8);
            gtk_widget_set_margin_top(row, 6);
            gtk_widget_set_margin_bottom(row, 6);

            // Volume name
            GtkWidget *name_label = gtk_label_new(entry->d_name);
            gtk_label_set_xalign(GTK_LABEL(name_label), 0.0);
            gtk_widget_add_css_class(name_label, "heading");
            gtk_box_append(GTK_BOX(row), name_label);

            // Capacity info
            if (capacity > 0) {
                char info[128];
                double cap_gb = capacity / (1024.0 * 1024.0 * 1024.0);
                double avail_gb = available / (1024.0 * 1024.0 * 1024.0);
                double used_pct = ((double)(capacity - available) / capacity) * 100.0;

                snprintf(info, sizeof(info), "%.1f GB total, %.1f GB free (%.0f%% used)",
                        cap_gb, avail_gb, used_pct);

                GtkWidget *info_label = gtk_label_new(info);
                gtk_label_set_xalign(GTK_LABEL(info_label), 0.0);
                gtk_widget_add_css_class(info_label, "dim-label");
                gtk_box_append(GTK_BOX(row), info_label);
            }

            // Store full path as data
            g_object_set_data_full(G_OBJECT(row), "volume_path", g_strdup(full_path), g_free);

            gtk_list_box_append(list, row);
        }
    }

    closedir(dir);
}

void on_volume_selected(GtkListBox *box, GtkListBoxRow *row, gpointer user_data) {
    (void)box;
    (void)user_data;

    if (!row) return;

    GtkWidget *row_widget = gtk_list_box_row_get_child(GTK_LIST_BOX_ROW(row));
    const char *volume_path = g_object_get_data(G_OBJECT(row_widget), "volume_path");

    if (volume_path) {
        gtk_editable_set_text(GTK_EDITABLE(path_entry), volume_path);

        // Auto-set database name from volume name
        char *vol_name = strrchr(volume_path, '/');
        if (vol_name) {
            gtk_editable_set_text(GTK_EDITABLE(db_name_entry), vol_name + 1);
        }
    }
}

void on_refresh_volumes_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;
    refresh_volumes_list();
}

// ==== GUI Callbacks ====

void on_folder_selected(GObject *source, GAsyncResult *result, gpointer data) {
    (void)data;
    GtkFileDialog *dialog = GTK_FILE_DIALOG(source);
    GFile *file = gtk_file_dialog_select_folder_finish(dialog, result, NULL);

    if (file) {
        char *path = g_file_get_path(file);
        gtk_editable_set_text(GTK_EDITABLE(path_entry), path);
        g_free(path);
        g_object_unref(file);
    }
}

void on_browse_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;

    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Select Directory to Scan");
    gtk_file_dialog_set_modal(dialog, TRUE);

    gtk_file_dialog_select_folder(dialog, GTK_WINDOW(window), NULL,
                                  on_folder_selected, NULL);
}

void on_start_scan(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;

    const char *path = gtk_editable_get_text(GTK_EDITABLE(path_entry));
    const char *db_name = gtk_editable_get_text(GTK_EDITABLE(db_name_entry));

    if (strlen(path) == 0) {
        GtkAlertDialog *alert = gtk_alert_dialog_new("Please select a directory to scan");
        gtk_alert_dialog_show(alert, GTK_WINDOW(window));
        g_object_unref(alert);
        return;
    }

    // Create scan context
    current_scan = g_new0(ScanContext, 1);
    strncpy(current_scan->scan_path, path, sizeof(current_scan->scan_path) - 1);

    // Determine database path
    const char *home = getenv("HOME");
    char db_dir[MAX_PATH];
    snprintf(db_dir, sizeof(db_dir), "%s/db/FileTracker", home);

    char mkdir_cmd[MAX_PATH + 20];
    snprintf(mkdir_cmd, sizeof(mkdir_cmd), "mkdir -p %s", db_dir);
    system(mkdir_cmd);

    if (strlen(db_name) > 0) {
        strncpy(current_scan->db_name, db_name, sizeof(current_scan->db_name) - 1);
    } else {
        // Use basename of path
        char *base = strrchr(path, '/');
        strncpy(current_scan->db_name, base ? base + 1 : path, sizeof(current_scan->db_name) - 1);
    }

    snprintf(current_scan->db_path, sizeof(current_scan->db_path),
             "%s/%s.db", db_dir, current_scan->db_name);

    current_scan->enable_checksum = gtk_check_button_get_active(GTK_CHECK_BUTTON(checksum_check));
    current_scan->update_mode = gtk_check_button_get_active(GTK_CHECK_BUTTON(update_check));

    GtkTextBuffer *note_buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(note_text));
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(note_buffer, &start, &end);
    char *note = gtk_text_buffer_get_text(note_buffer, &start, &end, FALSE);
    strncpy(current_scan->note, note, sizeof(current_scan->note) - 1);
    g_free(note);

    current_scan->should_stop = 0;

    // Update UI
    gtk_widget_set_sensitive(start_button, FALSE);
    gtk_widget_set_sensitive(stop_button, TRUE);
    gtk_widget_set_sensitive(path_entry, FALSE);
    gtk_widget_set_sensitive(db_name_entry, FALSE);

    gtk_progress_bar_set_fraction(progress_bar, 0.0);
    gtk_label_set_text(GTK_LABEL(status_label), "Initializing scan...");
    gtk_label_set_text(GTK_LABEL(current_file_label), "");

    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(results_text));
    gtk_text_buffer_set_text(buffer, "Scan in progress...", -1);

    // Start scan thread
    current_scan->scan_thread = g_thread_new("scanner", scan_thread_func, current_scan);
}

void on_stop_scan(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;

    if (current_scan) {
        current_scan->should_stop = 1;
        gtk_label_set_text(GTK_LABEL(status_label), "Stopping scan...");
    }
}

void activate(GtkApplication *app, gpointer user_data) {
    (void)user_data;

    window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window), "File Tracker");
    gtk_window_set_default_size(GTK_WINDOW(window), 1100, 750);

    GtkWidget *main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(main_box, 8);
    gtk_widget_set_margin_end(main_box, 8);
    gtk_widget_set_margin_top(main_box, 8);
    gtk_widget_set_margin_bottom(main_box, 8);

    // Horizontal paned: volumes list on left, config on right
    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_vexpand(paned, TRUE);

    // Left side: Volumes list
    GtkWidget *left_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(left_box, 8);
    gtk_widget_set_margin_end(left_box, 8);
    gtk_widget_set_margin_top(left_box, 8);
    gtk_widget_set_margin_bottom(left_box, 8);
    gtk_widget_set_size_request(left_box, 320, -1);

    GtkWidget *volumes_header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *volumes_label = gtk_label_new("Mounted Volumes");
    gtk_widget_add_css_class(volumes_label, "heading");
    gtk_label_set_xalign(GTK_LABEL(volumes_label), 0.0);
    gtk_widget_set_hexpand(volumes_label, TRUE);

    GtkWidget *refresh_volumes_button = gtk_button_new_with_label("Refresh");
    g_signal_connect(refresh_volumes_button, "clicked", G_CALLBACK(on_refresh_volumes_clicked), NULL);

    gtk_box_append(GTK_BOX(volumes_header), volumes_label);
    gtk_box_append(GTK_BOX(volumes_header), refresh_volumes_button);
    gtk_box_append(GTK_BOX(left_box), volumes_header);

    // Volumes list
    GtkWidget *volumes_scroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(volumes_scroll, TRUE);
    volumes_list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(volumes_list), GTK_SELECTION_SINGLE);
    g_signal_connect(volumes_list, "row-activated", G_CALLBACK(on_volume_selected), NULL);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(volumes_scroll), volumes_list);
    gtk_box_append(GTK_BOX(left_box), volumes_scroll);

    gtk_paned_set_start_child(GTK_PANED(paned), left_box);

    // Right side: Configuration
    GtkWidget *right_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start(right_box, 16);
    gtk_widget_set_margin_end(right_box, 16);
    gtk_widget_set_margin_top(right_box, 16);
    gtk_widget_set_margin_bottom(right_box, 16);

    // Path selection (manual entry)
    GtkWidget *path_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *path_label = gtk_label_new("Scan Path:");
    gtk_widget_set_size_request(path_label, 100, -1);
    gtk_label_set_xalign(GTK_LABEL(path_label), 0.0);
    path_entry = gtk_entry_new();
    gtk_widget_set_hexpand(path_entry, TRUE);
    GtkWidget *browse_button = gtk_button_new_with_label("Browse...");
    g_signal_connect(browse_button, "clicked", G_CALLBACK(on_browse_clicked), NULL);
    gtk_box_append(GTK_BOX(path_box), path_label);
    gtk_box_append(GTK_BOX(path_box), path_entry);
    gtk_box_append(GTK_BOX(path_box), browse_button);
    gtk_box_append(GTK_BOX(right_box), path_box);

    // Database name
    GtkWidget *db_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *db_label = gtk_label_new("Database Name:");
    gtk_widget_set_size_request(db_label, 100, -1);
    gtk_label_set_xalign(GTK_LABEL(db_label), 0.0);
    db_name_entry = gtk_entry_new();
    gtk_widget_set_hexpand(db_name_entry, TRUE);
    gtk_entry_set_placeholder_text(GTK_ENTRY(db_name_entry), "Optional - defaults to directory name");
    gtk_box_append(GTK_BOX(db_box), db_label);
    gtk_box_append(GTK_BOX(db_box), db_name_entry);
    gtk_box_append(GTK_BOX(right_box), db_box);

    // Options
    GtkWidget *options_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
    checksum_check = gtk_check_button_new_with_label("Enable Checksum Verification");
    update_check = gtk_check_button_new_with_label("Update Database");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(update_check), TRUE);
    gtk_box_append(GTK_BOX(options_box), checksum_check);
    gtk_box_append(GTK_BOX(options_box), update_check);
    gtk_box_append(GTK_BOX(right_box), options_box);

    // Note
    GtkWidget *note_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *note_label = gtk_label_new("Note (optional):");
    gtk_label_set_xalign(GTK_LABEL(note_label), 0.0);
    gtk_widget_add_css_class(note_label, "dim-label");
    note_text = gtk_text_view_new();
    gtk_widget_set_size_request(note_text, -1, 60);
    GtkWidget *note_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(note_scroll), note_text);
    gtk_box_append(GTK_BOX(note_box), note_label);
    gtk_box_append(GTK_BOX(note_box), note_scroll);
    gtk_box_append(GTK_BOX(right_box), note_box);

    // Control buttons
    GtkWidget *control_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(control_box, GTK_ALIGN_CENTER);
    start_button = gtk_button_new_with_label("Start Scan");
    gtk_widget_add_css_class(start_button, "suggested-action");
    gtk_widget_set_size_request(start_button, 120, -1);
    g_signal_connect(start_button, "clicked", G_CALLBACK(on_start_scan), NULL);
    stop_button = gtk_button_new_with_label("Stop");
    gtk_widget_add_css_class(stop_button, "destructive-action");
    gtk_widget_set_size_request(stop_button, 120, -1);
    gtk_widget_set_sensitive(stop_button, FALSE);
    g_signal_connect(stop_button, "clicked", G_CALLBACK(on_stop_scan), NULL);
    gtk_box_append(GTK_BOX(control_box), start_button);
    gtk_box_append(GTK_BOX(control_box), stop_button);
    gtk_box_append(GTK_BOX(right_box), control_box);

    // Separator
    gtk_box_append(GTK_BOX(right_box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    // Progress
    progress_bar = GTK_PROGRESS_BAR(gtk_progress_bar_new());
    gtk_widget_set_size_request(GTK_WIDGET(progress_bar), -1, 24);
    gtk_box_append(GTK_BOX(right_box), GTK_WIDGET(progress_bar));

    // Status
    status_label = gtk_label_new("Ready to scan");
    gtk_label_set_xalign(GTK_LABEL(status_label), 0.0);
    gtk_box_append(GTK_BOX(right_box), status_label);

    // Current file
    current_file_label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(current_file_label), 0.0);
    gtk_widget_add_css_class(current_file_label, "dim-label");
    gtk_label_set_ellipsize(GTK_LABEL(current_file_label), PANGO_ELLIPSIZE_START);
    gtk_box_append(GTK_BOX(right_box), current_file_label);

    // Results
    GtkWidget *results_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_vexpand(results_box, TRUE);
    GtkWidget *results_label = gtk_label_new("Results:");
    gtk_label_set_xalign(GTK_LABEL(results_label), 0.0);
    gtk_widget_add_css_class(results_label, "heading");
    results_text = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(results_text), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(results_text), TRUE);
    GtkWidget *results_scroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(results_scroll, TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(results_scroll), results_text);
    gtk_box_append(GTK_BOX(results_box), results_label);
    gtk_box_append(GTK_BOX(results_box), results_scroll);
    gtk_box_append(GTK_BOX(right_box), results_box);

    // Add right side to paned
    gtk_paned_set_end_child(GTK_PANED(paned), right_box);
    gtk_paned_set_position(GTK_PANED(paned), 350);

    // Add paned to main box
    gtk_box_append(GTK_BOX(main_box), paned);

    gtk_window_set_child(GTK_WINDOW(window), main_box);
    gtk_window_present(GTK_WINDOW(window));

    // Load initial volumes list
    refresh_volumes_list();
}

int main(int argc, char *argv[]) {
    GtkApplication *app = gtk_application_new("com.filetracker.gui", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);

    int status = g_application_run(G_APPLICATION(app), argc, argv);

    g_object_unref(app);
    return status;
}

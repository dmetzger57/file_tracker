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
#include <errno.h>
#include <fnmatch.h>
#include <unistd.h>
#include <pwd.h>
#include <locale.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>

// macos_dock_menu.m
void macos_install_dock_menu(const char *title, void (*callback)(void));
#endif

#define HASH_SIZE 65
#define MAX_PATH 4096
#define MAX_IGNORES 1024

// ============================================================================
// GLOBAL UI AND STATE
// ============================================================================

GtkWidget *window;
GtkWidget *main_notebook;
char db_dir_path[MAX_PATH];

// Ignore list for scanner
char *ignore_list[MAX_IGNORES];
int ignore_dir_only[MAX_IGNORES];  // pattern had a trailing '/': matches directories only
int ignore_anchored[MAX_IGNORES];  // pattern contains a '/': matched against the path below the scan root
int ignore_count = 0;

// ============================================================================
// UTILITY FUNCTIONS
// ============================================================================

void get_timestamp(char *buffer, size_t size) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    strftime(buffer, size, "%Y-%m-%d %H:%M:%S", t);
}

// Formats n with ',' thousands separators (e.g. 1,234,567) regardless of the process locale
static const char *format_count(long long n, char *buffer, size_t buf_size) {
    char digits[32];
    int len = snprintf(digits, sizeof(digits), "%lld", n < 0 ? -n : n);
    size_t pos = 0;
    if (n < 0 && pos + 1 < buf_size) buffer[pos++] = '-';
    for (int i = 0; i < len && pos + 1 < buf_size; i++) {
        if (i > 0 && (len - i) % 3 == 0 && pos + 1 < buf_size) buffer[pos++] = ',';
        if (pos + 1 < buf_size) buffer[pos++] = digits[i];
    }
    buffer[pos] = '\0';
    return buffer;
}

void format_size(long long size, char *buffer, size_t buf_size) {
    if (size < 1024) {
        snprintf(buffer, buf_size, "%lld B", size);
    } else if (size < 1024 * 1024) {
        snprintf(buffer, buf_size, "%.1f KB", size / 1024.0);
    } else if (size < 1024 * 1024 * 1024) {
        snprintf(buffer, buf_size, "%.1f MB", size / (1024.0 * 1024.0));
    } else {
        snprintf(buffer, buf_size, "%.1f GB", size / (1024.0 * 1024.0 * 1024.0));
    }
}

void load_ignore_list() {
    const char *home = getenv("HOME");
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
int is_ignored(const char *rel_path, int is_dir) {
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

// Opens drives.db. It is shared by every running copy of the app and by the CLI, so wait up to
// 30 seconds (as the CLI does) when another process is writing to it instead of failing
static int open_drives_db(sqlite3 **db, char *err, size_t err_size) {
    const char *home = getenv("HOME");
    char drives_db_path[MAX_PATH];
    snprintf(drives_db_path, sizeof(drives_db_path), "%s/db/FileTracker/drives.db", home ? home : "");

    *db = NULL;
    if (!home || sqlite3_open(drives_db_path, db) != SQLITE_OK) {
        snprintf(err, err_size, "cannot open %s: %s", drives_db_path, *db ? sqlite3_errmsg(*db) : "HOME not set");
        if (*db) sqlite3_close(*db);
        *db = NULL;
        return 0;
    }
    sqlite3_busy_timeout(*db, 30000);

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
    sqlite3_exec(*db, create_table, 0, 0, 0);
    return 1;
}

// 1 if the drive is in drives.db, 0 if not, -1 on error (message in err)
int drive_exists_in_tracker(const char *drive_name, char *err, size_t err_size) {
    sqlite3 *drives_db;
    if (!open_drives_db(&drives_db, err, err_size)) return -1;

    sqlite3_stmt *stmt;
    int exists = -1;
    if (sqlite3_prepare_v2(drives_db, "SELECT COUNT(*) FROM drives WHERE drive_name = ?;", -1, &stmt, 0) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, drive_name, -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW) exists = sqlite3_column_int(stmt, 0) > 0;
        sqlite3_finalize(stmt);
    }
    if (exists < 0) snprintf(err, err_size, "cannot read drives.db: %s", sqlite3_errmsg(drives_db));

    sqlite3_close(drives_db);
    return exists;
}

// Returns 1 on success, 0 on failure (message in err)
int add_drive_to_tracker(const char *drive_name, const char *source_path, char *err, size_t err_size) {
    sqlite3 *drives_db;
    if (!open_drives_db(&drives_db, err, err_size)) return 0;

    // Try to get drive stats
    long long capacity = 0, available = 0, used = 0;
    int has_stats = get_drive_stats(source_path, &capacity, &available, &used);

    char timestamp[64];
    get_timestamp(timestamp, sizeof(timestamp));

    const char *insert_sql = has_stats ?
        "INSERT INTO drives (drive_name, capacity, space_available, space_used, description, last_updated) "
        "VALUES (?, ?, ?, ?, ?, ?);" :
        "INSERT INTO drives (drive_name, description, last_updated) VALUES (?, ?, ?);";

    int ok = 0;
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(drives_db, insert_sql, -1, &stmt, 0) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, drive_name, -1, SQLITE_STATIC);

        if (has_stats) {
            sqlite3_bind_int64(stmt, 2, capacity);
            sqlite3_bind_int64(stmt, 3, available);
            sqlite3_bind_int64(stmt, 4, used);
            sqlite3_bind_text(stmt, 5, "Auto-added by file_tracker", -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 6, timestamp, -1, SQLITE_STATIC);
        } else {
            sqlite3_bind_text(stmt, 2, "Auto-added by file_tracker", -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 3, timestamp, -1, SQLITE_STATIC);
        }

        ok = (sqlite3_step(stmt) == SQLITE_DONE);
        sqlite3_finalize(stmt);
    }
    if (!ok) snprintf(err, err_size, "cannot add drive '%s' to drives.db: %s", drive_name, sqlite3_errmsg(drives_db));

    sqlite3_close(drives_db);
    return ok;
}

// Returns 1 on success (including when the drive's space cannot be read, so there is nothing
// to update), 0 on failure (message in err)
int update_drive_stats(const char *drive_name, const char *source_path, char *err, size_t err_size) {
    // Get drive stats
    long long capacity = 0, available = 0, used = 0;
    if (!get_drive_stats(source_path, &capacity, &available, &used)) return 1;

    sqlite3 *drives_db;
    if (!open_drives_db(&drives_db, err, err_size)) return 0;

    char timestamp[64];
    get_timestamp(timestamp, sizeof(timestamp));

    const char *update_sql =
        "UPDATE drives SET capacity = ?, space_available = ?, space_used = ?, last_updated = ? "
        "WHERE drive_name = ?;";

    int ok = 0;
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(drives_db, update_sql, -1, &stmt, 0) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, capacity);
        sqlite3_bind_int64(stmt, 2, available);
        sqlite3_bind_int64(stmt, 3, used);
        sqlite3_bind_text(stmt, 4, timestamp, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 5, drive_name, -1, SQLITE_STATIC);
        ok = (sqlite3_step(stmt) == SQLITE_DONE);
        sqlite3_finalize(stmt);
    }
    if (!ok) snprintf(err, err_size, "cannot update drive '%s' in drives.db: %s", drive_name, sqlite3_errmsg(drives_db));

    sqlite3_close(drives_db);
    return ok;
}

// Registers the scanned drive, or refreshes its space figures. Returns 1 on success,
// 0 on failure (message in err)
int auto_add_or_update_drive(const char *db_path, const char *source_path, char *err, size_t err_size) {
    // Extract drive name from db_path
    // db_path format: ~/db/FileTracker/DriveName.db
    char *last_slash = strrchr(db_path, '/');
    if (!last_slash) return 1;

    char drive_name[256];
    strncpy(drive_name, last_slash + 1, sizeof(drive_name) - 1);
    drive_name[sizeof(drive_name) - 1] = '\0';

    // Remove .db extension
    char *dot = strrchr(drive_name, '.');
    if (dot && strcmp(dot, ".db") == 0) {
        *dot = '\0';
    }

    // Check if drive already exists - if so, update; if not, add
    int exists = drive_exists_in_tracker(drive_name, err, err_size);
    if (exists < 0) return 0;
    return exists ? update_drive_stats(drive_name, source_path, err, err_size)
                  : add_drive_to_tracker(drive_name, source_path, err, err_size);
}

// Volumes hidden from all volume lists
static int is_excluded_volume(const char *name) {
    return fnmatch("com.apple.TimeMachine*", name, 0) == 0 ||
           fnmatch("mbp_backup", name, 0) == 0 ||
           fnmatch("Macintosh HD", name, 0) == 0;
}

// Refreshes space figures for every tracked drive that is mounted. Returns the number updated;
// *failed counts drives whose update failed, and err holds the last error message
int update_all_mounted_drives(int *failed, char *err, size_t err_size) {
    *failed = 0;
    DIR *dir = opendir("/Volumes");
    if (!dir) return 0;

    int updated_count = 0;
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        if (is_excluded_volume(entry->d_name)) continue;

        char full_path[MAX_PATH];
        snprintf(full_path, sizeof(full_path), "/Volumes/%s", entry->d_name);

        struct stat sb;
        if (stat(full_path, &sb) == 0 && S_ISDIR(sb.st_mode)) {
            // Check if this drive is in the database
            int exists = drive_exists_in_tracker(entry->d_name, err, err_size);
            if (exists < 0 ||
                (exists && !update_drive_stats(entry->d_name, full_path, err, err_size))) {
                (*failed)++;
            } else if (exists) {
                updated_count++;
            }
        }
    }
    closedir(dir);
    return updated_count;
}

void refresh_all_database_combos();
void drives_refresh_list();
void logs_refresh_databases();
void locator_refresh_databases();
void dupe_refresh_databases();
void summary_refresh_databases();
void compare_refresh_databases();

// ============================================================================
// TAB 1: FILE LOCATOR (Simplified - most commonly used)
// ============================================================================

GtkWidget *locator_search_entry;
GtkWidget *locator_db_combo;
GtkWidget *locator_partial_check;
GtkWidget *locator_mode_combo;   // 0 = File Name, 1 = Checksum
GtkWidget *locator_results_tree;
GtkWidget *locator_status_label;

void locator_search_database(const char *dbname, const char *db_path, const char *filename,
                              int by_checksum, int partial, GtkListStore *store, char *first_checksum, int *count) {
    sqlite3 *db;
    sqlite3_stmt *stmt;

    struct stat st;
    if (stat(db_path, &st) != 0) return;
    if (sqlite3_open(db_path, &db) != SQLITE_OK) return;

    char sql[512];
    if (by_checksum) {
        // Checksums are hex; compare case-insensitively
        snprintf(sql, sizeof(sql),
                 "SELECT full_path, size, last_modified, owner, checksum FROM files "
                 "WHERE lower(checksum) %s lower(?);",
                 partial ? "LIKE" : "=");
    } else {
        snprintf(sql, sizeof(sql),
                 "SELECT full_path, size, last_modified, owner, checksum FROM files WHERE file_name %s ?;",
                 partial ? "LIKE" : "=");
    }

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
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

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        (*count)++;
        const char *checksum = (const char *)sqlite3_column_text(stmt, 4);

        if (first_checksum[0] == '\0' && checksum) {
            strncpy(first_checksum, checksum, HASH_SIZE - 1);
        }

        int match = (checksum && strcmp(first_checksum, checksum) == 0);
        char size_str[64];
        format_size(sqlite3_column_int64(stmt, 1), size_str, sizeof(size_str));

        time_t mtime = sqlite3_column_int64(stmt, 2);
        struct tm *tm_info = localtime(&mtime);
        char time_str[64];
        strftime(time_str, sizeof(time_str), "%Y-%m-%d %H:%M:%S", tm_info);

        GtkTreeIter iter;
        gtk_list_store_append(store, &iter);
        gtk_list_store_set(store, &iter,
                          0, dbname,
                          1, sqlite3_column_text(stmt, 0),
                          2, size_str,
                          3, time_str,
                          4, sqlite3_column_text(stmt, 3),
                          5, match ? "✓" : "⚠",
                          6, checksum ? checksum : "",
                          -1);
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
}

void on_locator_search_clicked(GtkButton *button, gpointer user_data) {
    (void)button; (void)user_data;

    int by_checksum = gtk_combo_box_get_active(GTK_COMBO_BOX(locator_mode_combo)) == 1;
    char search_text[MAX_PATH];
    snprintf(search_text, sizeof(search_text), "%s",
             gtk_editable_get_text(GTK_EDITABLE(locator_search_entry)));
    if (by_checksum) g_strstrip(search_text);
    if (strlen(search_text) == 0) {
        gtk_label_set_text(GTK_LABEL(locator_status_label),
                           by_checksum ? "Enter a checksum to search" : "Enter a filename to search");
        return;
    }

    GtkListStore *store = GTK_LIST_STORE(gtk_tree_view_get_model(GTK_TREE_VIEW(locator_results_tree)));
    gtk_list_store_clear(store);

    char first_checksum[HASH_SIZE] = "";
    int count = 0;
    int partial = gtk_check_button_get_active(GTK_CHECK_BUTTON(locator_partial_check));
    char *selected_db = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(locator_db_combo));

    gtk_label_set_text(GTK_LABEL(locator_status_label), "Searching...");

    if (selected_db && strcmp(selected_db, "All Databases") == 0) {
        DIR *dir = opendir(db_dir_path);
        if (dir) {
            struct dirent *entry;
            while ((entry = readdir(dir)) != NULL) {
                size_t len = strlen(entry->d_name);
                if (len > 3 && strcmp(entry->d_name + len - 3, ".db") == 0) {
                    char db_path[MAX_PATH];
                    snprintf(db_path, sizeof(db_path), "%s/%s", db_dir_path, entry->d_name);
                    locator_search_database(entry->d_name, db_path, search_text, by_checksum, partial, store, first_checksum, &count);
                }
            }
            closedir(dir);
        }
    } else if (selected_db) {
        char db_path[MAX_PATH];
        snprintf(db_path, sizeof(db_path), "%s/%s", db_dir_path, selected_db);
        locator_search_database(selected_db, db_path, search_text, by_checksum, partial, store, first_checksum, &count);
    }

    if (selected_db) g_free(selected_db);

    char status[256];
    snprintf(status, sizeof(status), "Found %d file%s", count, count == 1 ? "" : "s");
    gtk_label_set_text(GTK_LABEL(locator_status_label), status);
}

static void on_locator_mode_changed(GtkComboBox *combo, gpointer user_data) {
    (void)user_data;
    int by_checksum = gtk_combo_box_get_active(combo) == 1;
    gtk_entry_set_placeholder_text(GTK_ENTRY(locator_search_entry),
                                   by_checksum ? "Enter SHA-256 checksum..." : "Enter filename...");
}

GtkWidget *create_locator_tab() {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start(box, 16);
    gtk_widget_set_margin_end(box, 16);
    gtk_widget_set_margin_top(box, 16);
    gtk_widget_set_margin_bottom(box, 16);

    // Search controls
    GtkWidget *search_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

    GtkWidget *label = gtk_label_new("Search by:");
    gtk_widget_set_size_request(label, 70, -1);

    locator_mode_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(locator_mode_combo), "File Name");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(locator_mode_combo), "Checksum");
    gtk_combo_box_set_active(GTK_COMBO_BOX(locator_mode_combo), 0);
    g_signal_connect(locator_mode_combo, "changed", G_CALLBACK(on_locator_mode_changed), NULL);

    locator_search_entry = gtk_entry_new();
    gtk_widget_set_hexpand(locator_search_entry, TRUE);
    gtk_entry_set_placeholder_text(GTK_ENTRY(locator_search_entry), "Enter filename...");

    locator_partial_check = gtk_check_button_new_with_label("Partial");

    locator_db_combo = gtk_combo_box_text_new();
    gtk_widget_set_size_request(locator_db_combo, 200, -1);

    GtkWidget *search_btn = gtk_button_new_with_label("Search");
    gtk_widget_add_css_class(search_btn, "suggested-action");
    g_signal_connect(search_btn, "clicked", G_CALLBACK(on_locator_search_clicked), NULL);

    gtk_box_append(GTK_BOX(search_box), label);
    gtk_box_append(GTK_BOX(search_box), locator_mode_combo);
    gtk_box_append(GTK_BOX(search_box), locator_search_entry);
    gtk_box_append(GTK_BOX(search_box), locator_partial_check);
    GtkWidget *refresh_btn = gtk_button_new_with_label("Refresh");
    gtk_widget_set_tooltip_text(refresh_btn, "Reload the database list");
    g_signal_connect(refresh_btn, "clicked", G_CALLBACK((GCallback)locator_refresh_databases), NULL);

    gtk_box_append(GTK_BOX(search_box), locator_db_combo);
    gtk_box_append(GTK_BOX(search_box), search_btn);
    gtk_box_append(GTK_BOX(search_box), refresh_btn);
    gtk_box_append(GTK_BOX(box), search_box);

    // Results
    GtkListStore *store = gtk_list_store_new(7, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                                             G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                                             G_TYPE_STRING);
    locator_results_tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
    g_object_unref(store);

    const char *titles[] = {"Database", "Path", "Size", "Modified", "Owner", "✓", "Checksum"};
    for (int i = 0; i < 7; i++) {
        GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
        GtkTreeViewColumn *column = gtk_tree_view_column_new_with_attributes(titles[i], renderer, "text", i, NULL);
        gtk_tree_view_column_set_resizable(column, TRUE);
        if (i == 1) gtk_tree_view_column_set_expand(column, TRUE);
        gtk_tree_view_append_column(GTK_TREE_VIEW(locator_results_tree), column);
    }

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), locator_results_tree);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_append(GTK_BOX(box), scroll);

    locator_status_label = gtk_label_new("Ready");
    gtk_label_set_xalign(GTK_LABEL(locator_status_label), 0.0);
    gtk_widget_add_css_class(locator_status_label, "dim-label");
    gtk_box_append(GTK_BOX(box), locator_status_label);

    return box;
}

// Writes s as a quoted CSV field, doubling any embedded quotes
static void csv_field(FILE *fp, const char *s) {
    fputc('"', fp);
    for (; s && *s; s++) {
        if (*s == '"') fputc('"', fp);
        fputc(*s, fp);
    }
    fputc('"', fp);
}

// ============================================================================
// TAB 2: DRIVES MANAGER
// ============================================================================

GtkWidget *drives_tree;
GtkWidget *drives_name_entry;
GtkWidget *drives_location_entry;
GtkWidget *drives_desc_text;
sqlite3_int64 selected_drive_id = -1;

void on_drives_update_mounted_clicked(GtkButton *button, gpointer user_data) {
    (void)button; (void)user_data;

    int failed;
    char err[512] = "";
    int count = update_all_mounted_drives(&failed, err, sizeof(err));
    drives_refresh_list();

    char message[1024];
    if (failed > 0) {
        snprintf(message, sizeof(message),
                "Updated %d mounted drive%s; %d could not be updated.\n\n%s",
                count, count == 1 ? "" : "s", failed, err);
    } else if (count > 0) {
        snprintf(message, sizeof(message),
                "Updated capacity and available space for %d mounted drive%s",
                count, count == 1 ? "" : "s");
    } else {
        snprintf(message, sizeof(message),
                "No tracked drives are currently mounted");
    }

    GtkAlertDialog *alert = gtk_alert_dialog_new("%s", message);
    gtk_alert_dialog_show(alert, GTK_WINDOW(window));
    g_object_unref(alert);
}

// Most recent scan with checksum verification enabled, and the number of files in the
// most recent run (-1 if unknown), from the drive's own DB
static void drives_db_stats(const char *drive_name, char *out, size_t out_size, long long *file_count) {
    snprintf(out, out_size, "Never");
    *file_count = -1;

    char path[MAX_PATH];
    snprintf(path, sizeof(path), "%s/%s.db", db_dir_path, drive_name);
    if (access(path, F_OK) != 0) {
        snprintf(out, out_size, "No database");
        return;
    }

    sqlite3 *db = NULL;
    if (sqlite3_open_v2(path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return;
    }
    sqlite3_busy_timeout(db, 1000);

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, "SELECT MAX(last_checksum_verify_date) FROM meta "
                               "WHERE last_checksum_verify_date IS NOT NULL;", -1, &stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *val = (const char *)sqlite3_column_text(stmt, 0);
            if (val && *val) snprintf(out, out_size, "%s", val);
        }
        sqlite3_finalize(stmt);
    }
    // Same total as the Summary tab: ignored files are not counted
    if (sqlite3_prepare_v2(db, "SELECT COALESCE(num_unchanged,0) + COALESCE(num_changed,0) + COALESCE(num_new,0) + "
                               "COALESCE(num_missing,0) + COALESCE(num_errors,0) FROM meta ORDER BY id DESC LIMIT 1;",
                           -1, &stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) *file_count = sqlite3_column_int64(stmt, 0);
        sqlite3_finalize(stmt);
    }
    sqlite3_close(db);
}

static void drives_files_cell_data(GtkTreeViewColumn *column, GtkCellRenderer *renderer,
                                   GtkTreeModel *model, GtkTreeIter *iter, gpointer data) {
    (void)column; (void)data;
    gint64 count;
    gtk_tree_model_get(model, iter, 6, &count, -1);
    char text[32] = "";
    if (count >= 0) snprintf(text, sizeof(text), "%'lld", (long long)count);
    g_object_set(renderer, "text", text, NULL);
}

void drives_refresh_list() {
    char drives_db[MAX_PATH];
    snprintf(drives_db, sizeof(drives_db), "%s/drives.db", db_dir_path);

    GtkListStore *store = GTK_LIST_STORE(gtk_tree_view_get_model(GTK_TREE_VIEW(drives_tree)));
    gtk_list_store_clear(store);

    sqlite3 *db;
    if (sqlite3_open(drives_db, &db) != SQLITE_OK) return;

    const char *sql = "SELECT drive_id, drive_name, storage_container, space_available, description "
                     "FROM drives ORDER BY drive_name;";
    sqlite3_stmt *stmt;

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            long long available = sqlite3_column_int64(stmt, 3);
            char avail_str[64];
            format_size(available, avail_str, sizeof(avail_str));

            const char *location = (const char *)sqlite3_column_text(stmt, 2);

            const char *drive_name = (const char *)sqlite3_column_text(stmt, 1);
            char checksum_scan[64];
            long long file_count;
            drives_db_stats(drive_name ? drive_name : "", checksum_scan, sizeof(checksum_scan), &file_count);

            GtkTreeIter iter;
            gtk_list_store_append(store, &iter);
            gtk_list_store_set(store, &iter,
                              0, sqlite3_column_int64(stmt, 0),
                              1, sqlite3_column_text(stmt, 1),
                              2, location ? location : "",
                              3, avail_str,
                              4, sqlite3_column_text(stmt, 4),
                              5, checksum_scan,
                              6, (gint64)file_count,
                              7, (gint64)available,
                              -1);
        }
        sqlite3_finalize(stmt);
    }
    sqlite3_close(db);
}

void on_drives_add_clicked(GtkButton *button, gpointer user_data) {
    (void)button; (void)user_data;

    const char *name = gtk_editable_get_text(GTK_EDITABLE(drives_name_entry));
    if (strlen(name) == 0) {
        GtkAlertDialog *alert = gtk_alert_dialog_new("Please enter a drive name");
        gtk_alert_dialog_show(alert, GTK_WINDOW(window));
        g_object_unref(alert);
        return;
    }

    const char *location = gtk_editable_get_text(GTK_EDITABLE(drives_location_entry));

    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(drives_desc_text));
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(buffer, &start, &end);
    char *desc = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);

    char drives_db[MAX_PATH];
    snprintf(drives_db, sizeof(drives_db), "%s/drives.db", db_dir_path);

    sqlite3 *db;
    if (sqlite3_open(drives_db, &db) == SQLITE_OK) {
        const char *create_sql = "CREATE TABLE IF NOT EXISTS drives ("
            "drive_id INTEGER PRIMARY KEY AUTOINCREMENT, "
            "drive_name TEXT NOT NULL UNIQUE, "
            "capacity INTEGER, "
            "space_available INTEGER, "
            "space_used INTEGER, "
            "description TEXT, "
            "last_updated TEXT, "
            "last_verified TEXT, "
            "storage_container TEXT);";
        sqlite3_exec(db, create_sql, 0, 0, 0);

        char timestamp[64];
        get_timestamp(timestamp, sizeof(timestamp));

        const char *insert_sql = "INSERT INTO drives (drive_name, storage_container, description, last_updated) VALUES (?, ?, ?, ?);";
        sqlite3_stmt *stmt;
        if (sqlite3_prepare_v2(db, insert_sql, -1, &stmt, 0) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 2, location, -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 3, desc, -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 4, timestamp, -1, SQLITE_STATIC);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
        sqlite3_close(db);
    }

    g_free(desc);
    gtk_editable_set_text(GTK_EDITABLE(drives_name_entry), "");
    gtk_editable_set_text(GTK_EDITABLE(drives_location_entry), "");
    gtk_text_buffer_set_text(buffer, "", -1);
    gtk_editable_set_editable(GTK_EDITABLE(drives_name_entry), TRUE);
    selected_drive_id = -1;
    drives_refresh_list();
}

static void drives_show_message(const char *msg) {
    GtkAlertDialog *alert = gtk_alert_dialog_new("%s", msg);
    gtk_alert_dialog_show(alert, GTK_WINDOW(window));
    g_object_unref(alert);
}

typedef struct {
    sqlite3_int64 drive_id;
    char name[256];
} DriveDeleteRequest;

static void drives_delete_finish(GObject *source, GAsyncResult *result, gpointer user_data) {
    DriveDeleteRequest *req = user_data;
    GError *error = NULL;
    // Buttons: 0 = Cancel, 1 = Remove Drive Only, 2 = Delete Drive and Database
    int choice = gtk_alert_dialog_choose_finish(GTK_ALERT_DIALOG(source), result, &error);
    if (error) {
        g_error_free(error);
        choice = 0;
    }

    if (choice == 1 || choice == 2) {
        char drives_db[MAX_PATH];
        snprintf(drives_db, sizeof(drives_db), "%s/drives.db", db_dir_path);

        sqlite3 *db;
        if (sqlite3_open(drives_db, &db) == SQLITE_OK) {
            sqlite3_busy_timeout(db, 2000);
            sqlite3_stmt *stmt;
            if (sqlite3_prepare_v2(db, "DELETE FROM drives WHERE drive_id = ?;", -1, &stmt, 0) == SQLITE_OK) {
                sqlite3_bind_int64(stmt, 1, req->drive_id);
                sqlite3_step(stmt);
                sqlite3_finalize(stmt);
            }
            sqlite3_close(db);
        }

        int failed = 0;
        if (choice == 2 && req->name[0] != '\0') {
            static const char *suffixes[] = {"", "-wal", "-shm", "-journal"};
            for (int i = 0; i < 4; i++) {
                char path[MAX_PATH];
                snprintf(path, sizeof(path), "%s/%s.db%s", db_dir_path, req->name, suffixes[i]);
                if (unlink(path) != 0 && errno != ENOENT) failed = 1;
            }
        }

        selected_drive_id = -1;
        drives_refresh_list();
        refresh_all_database_combos();
        logs_refresh_databases();

        if (failed) drives_show_message("The drive was removed, but the database file could not be deleted");
    }
    g_free(req);
}

void on_drives_delete_clicked(GtkButton *button, gpointer user_data) {
    (void)button; (void)user_data;

    if (selected_drive_id < 0) {
        drives_show_message("Please select a drive to delete");
        return;
    }

    DriveDeleteRequest *req = g_new0(DriveDeleteRequest, 1);
    req->drive_id = selected_drive_id;
    snprintf(req->name, sizeof(req->name), "%s", gtk_editable_get_text(GTK_EDITABLE(drives_name_entry)));

    char msg[400];
    snprintf(msg, sizeof(msg), "Delete drive '%s'?", req->name);
    char detail[600];
    snprintf(detail, sizeof(detail),
             "\"Remove Drive Only\" removes it from the Drives list and keeps its database (%s.db) and scan history.\n\n"
             "\"Delete Drive and Database\" also permanently deletes the database file. This cannot be undone.",
             req->name);

    GtkAlertDialog *alert = gtk_alert_dialog_new("%s", msg);
    gtk_alert_dialog_set_detail(alert, detail);
    const char *buttons[] = {"Cancel", "Remove Drive Only", "Delete Drive and Database", NULL};
    gtk_alert_dialog_set_buttons(alert, buttons);
    gtk_alert_dialog_set_cancel_button(alert, 0);
    gtk_alert_dialog_set_default_button(alert, 0);
    gtk_alert_dialog_choose(alert, GTK_WINDOW(window), NULL, drives_delete_finish, req);
    g_object_unref(alert);
}

void on_drives_update_clicked(GtkButton *button, gpointer user_data) {
    (void)button; (void)user_data;

    if (selected_drive_id < 0) {
        GtkAlertDialog *alert = gtk_alert_dialog_new("Please select a drive to update");
        gtk_alert_dialog_show(alert, GTK_WINDOW(window));
        g_object_unref(alert);
        return;
    }

    const char *location = gtk_editable_get_text(GTK_EDITABLE(drives_location_entry));

    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(drives_desc_text));
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(buffer, &start, &end);
    char *desc = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);

    char drives_db[MAX_PATH];
    snprintf(drives_db, sizeof(drives_db), "%s/drives.db", db_dir_path);

    sqlite3 *db;
    if (sqlite3_open(drives_db, &db) == SQLITE_OK) {
        char timestamp[64];
        get_timestamp(timestamp, sizeof(timestamp));

        const char *update_sql = "UPDATE drives SET storage_container = ?, description = ?, last_updated = ? WHERE drive_id = ?;";
        sqlite3_stmt *stmt;
        if (sqlite3_prepare_v2(db, update_sql, -1, &stmt, 0) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, location, -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 2, desc, -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 3, timestamp, -1, SQLITE_STATIC);
            sqlite3_bind_int64(stmt, 4, selected_drive_id);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
        sqlite3_close(db);
    }

    g_free(desc);

    GtkAlertDialog *alert = gtk_alert_dialog_new("Drive information updated");
    gtk_alert_dialog_show(alert, GTK_WINDOW(window));
    g_object_unref(alert);

    drives_refresh_list();
}

void on_drives_selection_changed(GtkTreeSelection *selection, gpointer user_data) {
    (void)user_data;

    GtkTreeModel *model;
    GtkTreeIter iter;

    if (gtk_tree_selection_get_selected(selection, &model, &iter)) {
        char *name, *location, *description;

        gtk_tree_model_get(model, &iter,
                          0, &selected_drive_id,
                          1, &name,
                          2, &location,
                          4, &description,
                          -1);

        // Populate fields with selected drive's data
        gtk_editable_set_text(GTK_EDITABLE(drives_name_entry), name ? name : "");
        gtk_editable_set_text(GTK_EDITABLE(drives_location_entry), location ? location : "");

        GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(drives_desc_text));
        gtk_text_buffer_set_text(buffer, description ? description : "", -1);

        // Make name field read-only when editing existing drive
        gtk_editable_set_editable(GTK_EDITABLE(drives_name_entry), FALSE);

        g_free(name);
        g_free(location);
        g_free(description);
    } else {
        selected_drive_id = -1;
        // Clear fields and make name editable again for adding new drives
        gtk_editable_set_text(GTK_EDITABLE(drives_name_entry), "");
        gtk_editable_set_text(GTK_EDITABLE(drives_location_entry), "");
        GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(drives_desc_text));
        gtk_text_buffer_set_text(buffer, "", -1);
        gtk_editable_set_editable(GTK_EDITABLE(drives_name_entry), TRUE);
    }
}

// ---- Rename drive: renames <old>.db to <new>.db and re-keys the drives.db entry ----

// Renames a file plus any SQLite sidecar files. Returns 0 on success; on failure nothing is left renamed.
static int drives_rename_db_files(const char *old_base, const char *new_base) {
    static const char *suffixes[] = {"", "-wal", "-shm", "-journal"};
    for (int i = 0; i < 4; i++) {
        char from[MAX_PATH], to[MAX_PATH];
        snprintf(from, sizeof(from), "%s%s", old_base, suffixes[i]);
        snprintf(to, sizeof(to), "%s%s", new_base, suffixes[i]);
        if (access(from, F_OK) != 0) continue;
        if (rename(from, to) != 0) {
            // Undo the ones already moved
            for (int j = 0; j < i; j++) {
                char f2[MAX_PATH], t2[MAX_PATH];
                snprintf(f2, sizeof(f2), "%s%s", old_base, suffixes[j]);
                snprintf(t2, sizeof(t2), "%s%s", new_base, suffixes[j]);
                if (access(t2, F_OK) == 0) rename(t2, f2);
            }
            return -1;
        }
    }
    return 0;
}

// Returns NULL on success or an error message
static const char *drives_do_rename(sqlite3_int64 drive_id, const char *old_name, const char *new_name) {
    if (new_name[0] == '\0') return "Enter a new drive name";
    if (strcmp(old_name, new_name) == 0) return "The new name is the same as the current name";
    if (strchr(new_name, '/') || new_name[0] == '.' || strlen(new_name) > 200)
        return "Invalid name: it cannot contain '/', start with '.', or exceed 200 characters";
    if (strcmp(new_name, "drives") == 0) return "'drives' is reserved";

    char old_base[MAX_PATH], new_base[MAX_PATH], new_db[MAX_PATH], drives_path[MAX_PATH];
    snprintf(old_base, sizeof(old_base), "%s/%s.db", db_dir_path, old_name);
    snprintf(new_base, sizeof(new_base), "%s/%s.db", db_dir_path, new_name);
    snprintf(new_db, sizeof(new_db), "%s", new_base);
    snprintf(drives_path, sizeof(drives_path), "%s/drives.db", db_dir_path);

    if (access(new_db, F_OK) == 0) return "A database with that name already exists";

    int had_db = access(old_base, F_OK) == 0;
    if (had_db && drives_rename_db_files(old_base, new_base) != 0)
        return "Could not rename the database file";

    sqlite3 *db = NULL;
    const char *err = NULL;
    if (sqlite3_open(drives_path, &db) != SQLITE_OK) {
        err = "Could not open the drives database";
    } else {
        sqlite3_busy_timeout(db, 2000);
        char timestamp[64];
        get_timestamp(timestamp, sizeof(timestamp));
        sqlite3_stmt *stmt;
        if (sqlite3_prepare_v2(db, "UPDATE drives SET drive_name = ?, last_updated = ? WHERE drive_id = ?;",
                               -1, &stmt, NULL) != SQLITE_OK) {
            err = "Could not update the drives database";
        } else {
            sqlite3_bind_text(stmt, 1, new_name, -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 2, timestamp, -1, SQLITE_STATIC);
            sqlite3_bind_int64(stmt, 3, drive_id);
            if (sqlite3_step(stmt) != SQLITE_DONE) err = "That drive name is already in use";
            sqlite3_finalize(stmt);
        }
    }
    if (db) sqlite3_close(db);

    if (err && had_db) drives_rename_db_files(new_base, old_base);  // roll back
    return err;
}

typedef struct {
    GtkWidget *window;
    GtkWidget *entry;
    sqlite3_int64 drive_id;
    char old_name[256];
} RenameDialog;

static void on_rename_cancel(GtkButton *button, gpointer user_data) {
    (void)button;
    RenameDialog *rd = user_data;
    gtk_window_destroy(GTK_WINDOW(rd->window));
}

static void on_rename_destroy(GtkWidget *widget, gpointer user_data) {
    (void)widget;
    g_free(user_data);
}

static void on_rename_ok(GtkButton *button, gpointer user_data) {
    (void)button;
    RenameDialog *rd = user_data;
    char new_name[256];
    snprintf(new_name, sizeof(new_name), "%s", gtk_editable_get_text(GTK_EDITABLE(rd->entry)));
    g_strstrip(new_name);

    const char *err = drives_do_rename(rd->drive_id, rd->old_name, new_name);
    if (err) {
        drives_show_message(err);
        return;
    }

    char msg[600];
    snprintf(msg, sizeof(msg), "Renamed '%s' to '%s'. All scan history was kept.\n\n"
             "To keep scanning this drive into the same database, set the Scanner's database name to '%s'.",
             rd->old_name, new_name, new_name);
    gtk_window_destroy(GTK_WINDOW(rd->window));

    selected_drive_id = -1;
    drives_refresh_list();
    refresh_all_database_combos();
    logs_refresh_databases();
    drives_show_message(msg);
}

void on_drives_rename_clicked(GtkButton *button, gpointer user_data) {
    (void)button; (void)user_data;

    if (selected_drive_id < 0) {
        drives_show_message("Please select a drive to rename");
        return;
    }

    RenameDialog *rd = g_new0(RenameDialog, 1);
    rd->drive_id = selected_drive_id;
    snprintf(rd->old_name, sizeof(rd->old_name), "%s", gtk_editable_get_text(GTK_EDITABLE(drives_name_entry)));

    rd->window = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(rd->window), "Rename Drive");
    gtk_window_set_transient_for(GTK_WINDOW(rd->window), GTK_WINDOW(window));
    gtk_window_set_modal(GTK_WINDOW(rd->window), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(rd->window), 400, -1);
    g_signal_connect(rd->window, "destroy", G_CALLBACK(on_rename_destroy), rd);

    GtkWidget *vbox = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start(vbox, 16);
    gtk_widget_set_margin_end(vbox, 16);
    gtk_widget_set_margin_top(vbox, 16);
    gtk_widget_set_margin_bottom(vbox, 16);

    char prompt[400];
    snprintf(prompt, sizeof(prompt), "New name for '%s'.\nThe database is renamed and all its history is kept.", rd->old_name);
    GtkWidget *label = gtk_label_new(prompt);
    gtk_label_set_xalign(GTK_LABEL(label), 0.0);

    rd->entry = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(rd->entry), rd->old_name);
    gtk_entry_set_activates_default(GTK_ENTRY(rd->entry), TRUE);

    GtkWidget *btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(btn_box, GTK_ALIGN_END);
    GtkWidget *cancel_btn = gtk_button_new_with_label("Cancel");
    GtkWidget *ok_btn = gtk_button_new_with_label("Rename");
    gtk_widget_add_css_class(ok_btn, "suggested-action");
    g_signal_connect(cancel_btn, "clicked", G_CALLBACK(on_rename_cancel), rd);
    g_signal_connect(ok_btn, "clicked", G_CALLBACK(on_rename_ok), rd);
    gtk_box_append(GTK_BOX(btn_box), cancel_btn);
    gtk_box_append(GTK_BOX(btn_box), ok_btn);

    gtk_box_append(GTK_BOX(vbox), label);
    gtk_box_append(GTK_BOX(vbox), rd->entry);
    gtk_box_append(GTK_BOX(vbox), btn_box);
    gtk_window_set_child(GTK_WINDOW(rd->window), vbox);
    gtk_window_set_default_widget(GTK_WINDOW(rd->window), ok_btn);
    gtk_window_present(GTK_WINDOW(rd->window));
    gtk_editable_select_region(GTK_EDITABLE(rd->entry), 0, -1);
}

// ---- Export: saves the drives table, in its current sort order, as CSV ----

static void on_drives_export_response(GObject *source, GAsyncResult *result, gpointer user_data) {
    (void)user_data;

    GFile *file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(source), result, NULL);
    if (!file) return;

    char *path = g_file_get_path(file);
    g_object_unref(file);
    if (!path) return;

    FILE *fp = fopen(path, "w");
    if (!fp) {
        char msg[MAX_PATH + 64];
        snprintf(msg, sizeof(msg), "Could not create %s: %s", path, strerror(errno));
        drives_show_message(msg);
        g_free(path);
        return;
    }

    fprintf(fp, "ID,Name,Location,Available,Available (bytes),Description,Last Checksum Scan,Files (Last Run)\n");

    GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(drives_tree));
    GtkTreeIter iter;
    long count = 0;
    for (gboolean valid = gtk_tree_model_get_iter_first(model, &iter); valid;
         valid = gtk_tree_model_iter_next(model, &iter)) {
        gint64 id, file_count, available;
        char *name, *location, *avail_str, *desc, *checksum_scan;
        gtk_tree_model_get(model, &iter,
                           0, &id, 1, &name, 2, &location, 3, &avail_str,
                           4, &desc, 5, &checksum_scan, 6, &file_count, 7, &available, -1);

        fprintf(fp, "%lld,", (long long)id);
        csv_field(fp, name);
        fputc(',', fp);
        csv_field(fp, location);
        fputc(',', fp);
        csv_field(fp, avail_str);
        fprintf(fp, ",%lld,", (long long)available);
        csv_field(fp, desc);
        fputc(',', fp);
        csv_field(fp, checksum_scan);
        fputc(',', fp);
        if (file_count >= 0) fprintf(fp, "%lld", (long long)file_count);
        fputc('\n', fp);
        count++;

        g_free(name);
        g_free(location);
        g_free(avail_str);
        g_free(desc);
        g_free(checksum_scan);
    }

    int write_failed = ferror(fp);
    if (fclose(fp) != 0) write_failed = 1;

    char msg[MAX_PATH + 64];
    if (write_failed) snprintf(msg, sizeof(msg), "Error writing %s", path);
    else snprintf(msg, sizeof(msg), "Exported %ld drive%s to %s", count, count == 1 ? "" : "s", path);
    drives_show_message(msg);
    g_free(path);
}

void on_drives_export_clicked(GtkButton *button, gpointer user_data) {
    (void)button; (void)user_data;

    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Export Drives to CSV");
    gtk_file_dialog_set_initial_name(dialog, "drives.csv");
    gtk_file_dialog_save(dialog, GTK_WINDOW(window), NULL, on_drives_export_response, NULL);
    g_object_unref(dialog);
}

GtkWidget *create_drives_tab() {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start(box, 16);
    gtk_widget_set_margin_end(box, 16);
    gtk_widget_set_margin_top(box, 16);
    gtk_widget_set_margin_bottom(box, 16);

    // Add drive controls
    GtkWidget *add_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

    GtkWidget *name_label = gtk_label_new("Drive:");
    gtk_widget_set_size_request(name_label, 50, -1);

    drives_name_entry = gtk_entry_new();
    gtk_widget_set_hexpand(drives_name_entry, TRUE);

    GtkWidget *location_label = gtk_label_new("Location:");
    drives_location_entry = gtk_entry_new();
    gtk_widget_set_hexpand(drives_location_entry, TRUE);

    GtkWidget *add_btn = gtk_button_new_with_label("Add");
    gtk_widget_add_css_class(add_btn, "suggested-action");
    g_signal_connect(add_btn, "clicked", G_CALLBACK(on_drives_add_clicked), NULL);

    GtkWidget *update_btn = gtk_button_new_with_label("Update");
    g_signal_connect(update_btn, "clicked", G_CALLBACK(on_drives_update_clicked), NULL);

    GtkWidget *rename_btn = gtk_button_new_with_label("Rename");
    g_signal_connect(rename_btn, "clicked", G_CALLBACK(on_drives_rename_clicked), NULL);

    GtkWidget *del_btn = gtk_button_new_with_label("Delete");
    gtk_widget_add_css_class(del_btn, "destructive-action");
    g_signal_connect(del_btn, "clicked", G_CALLBACK(on_drives_delete_clicked), NULL);

    GtkWidget *refresh_btn = gtk_button_new_with_label("Refresh");
    g_signal_connect(refresh_btn, "clicked", G_CALLBACK((GCallback)drives_refresh_list), NULL);

    GtkWidget *update_mounted_btn = gtk_button_new_with_label("Update Mounted Drives");
    g_signal_connect(update_mounted_btn, "clicked", G_CALLBACK(on_drives_update_mounted_clicked), NULL);

    GtkWidget *export_btn = gtk_button_new_with_label("Export to CSV");
    g_signal_connect(export_btn, "clicked", G_CALLBACK(on_drives_export_clicked), NULL);

    gtk_box_append(GTK_BOX(add_box), name_label);
    gtk_box_append(GTK_BOX(add_box), drives_name_entry);
    gtk_box_append(GTK_BOX(add_box), location_label);
    gtk_box_append(GTK_BOX(add_box), drives_location_entry);
    gtk_box_append(GTK_BOX(add_box), add_btn);
    gtk_box_append(GTK_BOX(add_box), update_btn);
    gtk_box_append(GTK_BOX(add_box), rename_btn);
    gtk_box_append(GTK_BOX(add_box), del_btn);
    gtk_box_append(GTK_BOX(add_box), refresh_btn);
    gtk_box_append(GTK_BOX(add_box), update_mounted_btn);
    gtk_box_append(GTK_BOX(add_box), export_btn);
    gtk_box_append(GTK_BOX(box), add_box);

    // Description
    GtkWidget *desc_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *desc_label = gtk_label_new("Description:");
    gtk_label_set_xalign(GTK_LABEL(desc_label), 0.0);
    drives_desc_text = gtk_text_view_new();
    gtk_widget_set_size_request(drives_desc_text, -1, 60);
    GtkWidget *desc_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(desc_scroll), drives_desc_text);
    gtk_box_append(GTK_BOX(desc_box), desc_label);
    gtk_box_append(GTK_BOX(desc_box), desc_scroll);
    gtk_box_append(GTK_BOX(box), desc_box);

    // Drives list
    // Column 7 (not displayed) holds the available bytes so the Available column sorts numerically
    GtkListStore *store = gtk_list_store_new(8, G_TYPE_INT64, G_TYPE_STRING, G_TYPE_STRING,
                                             G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_INT64,
                                             G_TYPE_INT64);
    drives_tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
    g_object_unref(store);

    const char *titles[] = {"ID", "Name", "Location", "Available", "Description", "Last Checksum Scan"};
    for (int i = 0; i < 6; i++) {
        GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
        GtkTreeViewColumn *column = gtk_tree_view_column_new_with_attributes(titles[i], renderer, "text", i, NULL);
        gtk_tree_view_column_set_resizable(column, TRUE);
        if (i == 4) gtk_tree_view_column_set_expand(column, TRUE);
        gtk_tree_view_column_set_sort_column_id(column, i == 3 ? 7 : i);
        gtk_tree_view_append_column(GTK_TREE_VIEW(drives_tree), column);
    }

    // Files in the last run; stored as a number so it sorts numerically
    GtkCellRenderer *files_renderer = gtk_cell_renderer_text_new();
    g_object_set(files_renderer, "xalign", 1.0, NULL);
    GtkTreeViewColumn *files_column = gtk_tree_view_column_new();
    gtk_tree_view_column_set_title(files_column, "Files (Last Run)");
    gtk_tree_view_column_pack_start(files_column, files_renderer, TRUE);
    gtk_tree_view_column_set_cell_data_func(files_column, files_renderer, drives_files_cell_data, NULL, NULL);
    gtk_tree_view_column_set_resizable(files_column, TRUE);
    gtk_tree_view_column_set_sort_column_id(files_column, 6);
    gtk_tree_view_append_column(GTK_TREE_VIEW(drives_tree), files_column);

    GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(drives_tree));
    g_signal_connect(selection, "changed", G_CALLBACK(on_drives_selection_changed), NULL);

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), drives_tree);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_append(GTK_BOX(box), scroll);

    return box;
}

// ============================================================================
// TAB 3: SUMMARY VIEWER (Simplified)
// ============================================================================

GtkWidget *summary_db_combo;
GtkWidget *summary_tree;
GtkWidget *summary_details_text;

void summary_load_runs() {
    char *db_name = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(summary_db_combo));
    if (!db_name || strlen(db_name) == 0) return;

    char db_path[MAX_PATH];
    snprintf(db_path, sizeof(db_path), "%s/%s", db_dir_path, db_name);

    GtkListStore *store = GTK_LIST_STORE(gtk_tree_view_get_model(GTK_TREE_VIEW(summary_tree)));
    gtk_list_store_clear(store);

    sqlite3 *db;
    if (sqlite3_open(db_path, &db) != SQLITE_OK) {
        g_free(db_name);
        return;
    }

    const char *sql = "SELECT id, COALESCE(last_checksum_verify_date, last_date_verify) as date, "
                     "num_unchanged, num_changed, num_new, num_missing, num_ignored, num_errors "
                     "FROM meta ORDER BY id DESC LIMIT 10;";
    sqlite3_stmt *stmt;

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            GtkTreeIter iter;
            gtk_list_store_append(store, &iter);
            gtk_list_store_set(store, &iter,
                              0, sqlite3_column_int(stmt, 0),
                              1, sqlite3_column_text(stmt, 1),
                              2, sqlite3_column_int(stmt, 2),
                              3, sqlite3_column_int(stmt, 3),
                              4, sqlite3_column_int(stmt, 4),
                              5, sqlite3_column_int(stmt, 5),
                              6, sqlite3_column_int(stmt, 6),
                              7, sqlite3_column_int(stmt, 7),
                              -1);
        }
        sqlite3_finalize(stmt);
    }
    sqlite3_close(db);
    g_free(db_name);
}

void on_summary_run_selected(GtkTreeSelection *selection, gpointer user_data) {
    (void)user_data;

    GtkTreeModel *model;
    GtkTreeIter iter;

    if (!gtk_tree_selection_get_selected(selection, &model, &iter)) return;

    int run_id, unch, chg, new, miss, ign, err;
    char *date;
    gtk_tree_model_get(model, &iter, 0, &run_id, 1, &date, 2, &unch, 3, &chg,
                      4, &new, 5, &miss, 6, &ign, 7, &err, -1);

    char details[1024];
    snprintf(details, sizeof(details),
             "Run #%d - %s\n\n"
             "Unchanged: %'d\n"
             "Changed:   %'d\n"
             "New:       %'d\n"
             "Missing:   %'d\n"
             "Ignored:   %'d\n"
             "Errors:    %'d\n\n"
             "Total:     %'d",
             run_id, date, unch, chg, new, miss, ign, err,
             unch + chg + new + miss + err);

    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(summary_details_text));
    gtk_text_buffer_set_text(buffer, details, -1);
    g_free(date);
}

GtkWidget *create_summary_tab() {
    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);

    // Left: Run list
    GtkWidget *left_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_size_request(left_box, 600, -1);
    gtk_widget_set_margin_start(left_box, 8);
    gtk_widget_set_margin_end(left_box, 8);
    gtk_widget_set_margin_top(left_box, 8);
    gtk_widget_set_margin_bottom(left_box, 8);

    GtkWidget *db_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *db_label = gtk_label_new("Database:");
    summary_db_combo = gtk_combo_box_text_new();
    gtk_widget_set_hexpand(summary_db_combo, TRUE);
    g_signal_connect(summary_db_combo, "changed", G_CALLBACK((GCallback)summary_load_runs), NULL);
    GtkWidget *refresh_btn = gtk_button_new_with_label("Refresh");
    gtk_widget_set_tooltip_text(refresh_btn, "Reload the database list and scan runs");
    g_signal_connect(refresh_btn, "clicked", G_CALLBACK((GCallback)summary_refresh_databases), NULL);
    gtk_box_append(GTK_BOX(db_box), db_label);
    gtk_box_append(GTK_BOX(db_box), summary_db_combo);
    gtk_box_append(GTK_BOX(db_box), refresh_btn);
    gtk_box_append(GTK_BOX(left_box), db_box);

    GtkListStore *store = gtk_list_store_new(8, G_TYPE_INT, G_TYPE_STRING, G_TYPE_INT,
                                             G_TYPE_INT, G_TYPE_INT, G_TYPE_INT, G_TYPE_INT, G_TYPE_INT);
    summary_tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
    g_object_unref(store);

    const char *titles[] = {"ID", "Date", "Unch", "Chg", "New", "Miss", "Ign", "Err"};
    for (int i = 0; i < 8; i++) {
        GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
        GtkTreeViewColumn *column = gtk_tree_view_column_new_with_attributes(titles[i], renderer, "text", i, NULL);
        gtk_tree_view_column_set_resizable(column, TRUE);
        gtk_tree_view_append_column(GTK_TREE_VIEW(summary_tree), column);
    }

    GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(summary_tree));
    g_signal_connect(selection, "changed", G_CALLBACK(on_summary_run_selected), NULL);

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), summary_tree);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_append(GTK_BOX(left_box), scroll);

    gtk_paned_set_start_child(GTK_PANED(paned), left_box);

    // Right: Details
    GtkWidget *right_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(right_box, 8);
    gtk_widget_set_margin_end(right_box, 8);
    gtk_widget_set_margin_top(right_box, 8);
    gtk_widget_set_margin_bottom(right_box, 8);

    GtkWidget *details_label = gtk_label_new("Run Details");
    gtk_widget_add_css_class(details_label, "heading");
    gtk_label_set_xalign(GTK_LABEL(details_label), 0.0);
    gtk_box_append(GTK_BOX(right_box), details_label);

    summary_details_text = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(summary_details_text), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(summary_details_text), TRUE);

    GtkWidget *details_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(details_scroll), summary_details_text);
    gtk_widget_set_vexpand(details_scroll, TRUE);
    gtk_box_append(GTK_BOX(right_box), details_scroll);

    gtk_paned_set_end_child(GTK_PANED(paned), right_box);

    return paned;
}

// ============================================================================
// TAB 4: LOGS VIEWER
// ============================================================================

GtkWidget *logs_db_combo;
GtkWidget *logs_runs_list;
GtkWidget *logs_run_info_text;
GtkWidget *logs_note_text;
GtkWidget *logs_text;
GtkWidget *logs_filter_new_check;
GtkWidget *logs_filter_changed_check;
GtkWidget *logs_filter_missing_check;
GtkWidget *logs_filter_unchanged_check;
GtkWidget *logs_filter_ignored_check;
GtkWidget *logs_filter_errors_check;
GtkWidget *logs_filter_all_check;

char logs_current_db_path[MAX_PATH] = "";
sqlite3_int64 logs_selected_run_id = 0;

void logs_format_run_identifier(char *buffer, size_t size, const char *db_name, const char *run_date, sqlite3_int64 id) {
    if (run_date) {
        char formatted_date[64];
        strncpy(formatted_date, run_date, sizeof(formatted_date) - 1);
        formatted_date[sizeof(formatted_date) - 1] = '\0';
        for (char *p = formatted_date; *p; p++) {
            if (*p == ' ' || *p == ':') *p = '-';
        }
        snprintf(buffer, size, "%s-%s", db_name, formatted_date);
    } else {
        snprintf(buffer, size, "%s-unknown-%lld", db_name, id);
    }
}

static gint compare_db_file_names(gconstpointer a, gconstpointer b) {
    return g_ascii_strcasecmp(*(const char **)a, *(const char **)b);
}

// Returns the drive database file names (e.g. "MyDrive.db") in db_dir_path,
// excluding drives.db, sorted alphabetically (case-insensitive).
// Caller frees with g_ptr_array_unref(). Returns NULL if the directory can't be opened.
GPtrArray *list_database_files() {
    DIR *dir = opendir(db_dir_path);
    if (!dir) return NULL;

    GPtrArray *files = g_ptr_array_new_with_free_func(g_free);
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        size_t len = strlen(entry->d_name);
        if (len > 3 && strcmp(entry->d_name + len - 3, ".db") == 0 &&
            strcmp(entry->d_name, "drives.db") != 0) {
            g_ptr_array_add(files, g_strdup(entry->d_name));
        }
    }
    closedir(dir);

    g_ptr_array_sort(files, compare_db_file_names);
    return files;
}

// Refills a database combo from files (".db" names, or plain names when strip_ext is set),
// after an optional first_item such as "All Databases". The previous selection is kept when
// it still exists, otherwise the first entry is selected. Selecting emits "changed", so tabs
// that load runs on that signal reload them.
static void refill_database_combo(GtkWidget *combo, GPtrArray *files, const char *first_item, gboolean strip_ext) {
    char *previous = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(combo));
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(combo));

    int index = 0, active = 0;
    if (first_item) {
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), first_item);
        index++;
    }
    for (guint i = 0; i < files->len; i++, index++) {
        const char *file_name = g_ptr_array_index(files, i);
        char name[256];
        snprintf(name, sizeof(name), "%.*s", (int)(strlen(file_name) - (strip_ext ? 3 : 0)), file_name);
        gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(combo), name);
        if (previous && strcmp(previous, name) == 0) active = index;
    }
    if (index > 0) gtk_combo_box_set_active(GTK_COMBO_BOX(combo), active);
    g_free(previous);
}

void logs_refresh_databases() {
    GPtrArray *files = list_database_files();
    if (!files) return;
    refill_database_combo(logs_db_combo, files, NULL, TRUE);
    g_ptr_array_unref(files);
}

void logs_load_runs(const char *db_name) {
    if (!db_name || strlen(db_name) == 0) return;

    snprintf(logs_current_db_path, sizeof(logs_current_db_path), "%s/%s.db", db_dir_path, db_name);

    GtkListBox *list = GTK_LIST_BOX(logs_runs_list);
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(GTK_WIDGET(list))) != NULL) {
        gtk_list_box_remove(list, child);
    }

    GtkTextBuffer *info_buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(logs_run_info_text));
    gtk_text_buffer_set_text(info_buffer, "", -1);
    GtkTextBuffer *note_buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(logs_note_text));
    gtk_text_buffer_set_text(note_buffer, "", -1);
    GtkTextBuffer *logs_buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(logs_text));
    gtk_text_buffer_set_text(logs_buffer, "", -1);

    if (access(logs_current_db_path, F_OK) != 0) return;

    sqlite3 *db;
    if (sqlite3_open(logs_current_db_path, &db) != SQLITE_OK) return;

    const char *query =
        "SELECT id, COALESCE(last_checksum_verify_date, last_date_verify) as run_date, "
        "verify_machine, num_unchanged, num_changed, num_new, num_missing, num_ignored, "
        "update_mode, note FROM meta ORDER BY id DESC";

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, query, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return;
    }

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        sqlite3_int64 id = sqlite3_column_int64(stmt, 0);
        const char *run_date = (const char *)sqlite3_column_text(stmt, 1);
        const char *machine = (const char *)sqlite3_column_text(stmt, 2);
        int unchanged = sqlite3_column_int(stmt, 3);
        int changed = sqlite3_column_int(stmt, 4);
        int new = sqlite3_column_int(stmt, 5);
        int missing = sqlite3_column_int(stmt, 6);
        int ignored = sqlite3_column_int(stmt, 7);
        const char *update_mode = (const char *)sqlite3_column_text(stmt, 8);
        const char *note = (const char *)sqlite3_column_text(stmt, 9);

        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        gtk_widget_set_margin_start(row, 8);
        gtk_widget_set_margin_end(row, 8);
        gtk_widget_set_margin_top(row, 6);
        gtk_widget_set_margin_bottom(row, 6);

        GtkWidget *date_label = gtk_label_new(run_date ? run_date : "Unknown");
        gtk_label_set_xalign(GTK_LABEL(date_label), 0.0);
        gtk_widget_add_css_class(date_label, "heading");
        gtk_box_append(GTK_BOX(row), date_label);

        char stats[256];
        snprintf(stats, sizeof(stats), "U:%d C:%d N:%d M:%d I:%d on %s%s%s",
                unchanged, changed, new, missing, ignored,
                machine ? machine : "N/A",
                update_mode && strcmp(update_mode, "OFF") == 0 ? " [RO]" : "",
                note && strlen(note) > 0 ? " 📝" : "");

        GtkWidget *stats_label = gtk_label_new(stats);
        gtk_label_set_xalign(GTK_LABEL(stats_label), 0.0);
        gtk_widget_add_css_class(stats_label, "dim-label");
        gtk_box_append(GTK_BOX(row), stats_label);

        g_object_set_data(G_OBJECT(row), "run_id", GINT_TO_POINTER((gint)id));
        gtk_list_box_append(list, row);
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
}

void logs_load_details() {
    if (logs_selected_run_id == 0) return;

    sqlite3 *db;
    if (sqlite3_open(logs_current_db_path, &db) != SQLITE_OK) return;

    const char *query =
        "SELECT COALESCE(last_checksum_verify_date, last_date_verify) as run_date, "
        "verify_machine, num_unchanged, num_changed, num_new, num_missing, num_ignored, num_errors, "
        "update_mode, note, last_checksum_verify_date FROM meta WHERE id = ?";

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, query, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return;
    }

    sqlite3_bind_int64(stmt, 1, logs_selected_run_id);

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *run_date = (const char *)sqlite3_column_text(stmt, 0);
        const char *machine = (const char *)sqlite3_column_text(stmt, 1);
        int unchanged = sqlite3_column_int(stmt, 2);
        int changed = sqlite3_column_int(stmt, 3);
        int new = sqlite3_column_int(stmt, 4);
        int missing = sqlite3_column_int(stmt, 5);
        int ignored = sqlite3_column_int(stmt, 6);
        int errors = sqlite3_column_int(stmt, 7);
        const char *update_mode = (const char *)sqlite3_column_text(stmt, 8);
        const char *note = (const char *)sqlite3_column_text(stmt, 9);
        const char *checksum_date = (const char *)sqlite3_column_text(stmt, 10);

        char info[512];
        snprintf(info, sizeof(info),
                "Run Date: %s\n"
                "Machine: %s\n"
                "Mode: %s\n"
                "Checksum: %s\n"
                "Legend: U/C/N/M/I/E\n\n"
                "Unchanged: %'d\n"
                "Changed:   %'d\n"
                "New:       %'d\n"
                "Missing:   %'d\n"
                "Ignored:   %'d\n"
                "Errors:    %'d\n",
                run_date ? run_date : "Unknown",
                machine ? machine : "N/A",
                update_mode ? update_mode : "N/A",
                checksum_date ? "Verified" : "Not Verified",
                unchanged, changed, new, missing, ignored, errors);

        GtkTextBuffer *info_buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(logs_run_info_text));
        gtk_text_buffer_set_text(info_buffer, info, -1);

        GtkTextBuffer *note_buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(logs_note_text));
        gtk_text_buffer_set_text(note_buffer, note ? note : "", -1);
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
}

// Builds SQL WHERE fragment from the Logs filter checkboxes. Returns 0 if none selected.
static int logs_build_status_filter(char *status_filter) {
    int any_filter = 0;


    if (gtk_check_button_get_active(GTK_CHECK_BUTTON(logs_filter_all_check))) {
        strcpy(status_filter, "1=1");
        any_filter = 1;
    } else {
        int first = 1;
        if (gtk_check_button_get_active(GTK_CHECK_BUTTON(logs_filter_new_check))) {
            strcat(status_filter, "status = 'NEW'");
            first = 0;
            any_filter = 1;
        }
        if (gtk_check_button_get_active(GTK_CHECK_BUTTON(logs_filter_changed_check))) {
            if (!first) strcat(status_filter, " OR ");
            strcat(status_filter, "status LIKE 'CHANGED%'");
            first = 0;
            any_filter = 1;
        }
        if (gtk_check_button_get_active(GTK_CHECK_BUTTON(logs_filter_missing_check))) {
            if (!first) strcat(status_filter, " OR ");
            strcat(status_filter, "status = 'MISSING'");
            first = 0;
            any_filter = 1;
        }
        if (gtk_check_button_get_active(GTK_CHECK_BUTTON(logs_filter_unchanged_check))) {
            if (!first) strcat(status_filter, " OR ");
            strcat(status_filter, "status = 'UNCHANGED'");
            first = 0;
            any_filter = 1;
        }
        if (gtk_check_button_get_active(GTK_CHECK_BUTTON(logs_filter_ignored_check))) {
            if (!first) strcat(status_filter, " OR ");
            strcat(status_filter, "status = 'IGNORED'");
            first = 0;
            any_filter = 1;
        }
        if (gtk_check_button_get_active(GTK_CHECK_BUTTON(logs_filter_errors_check))) {
            if (!first) strcat(status_filter, " OR ");
            strcat(status_filter, "status = 'ERROR'");
            any_filter = 1;
        }
    }

    return any_filter;
}

void logs_load_logs() {
    if (logs_selected_run_id == 0) return;

    sqlite3 *db;
    if (sqlite3_open(logs_current_db_path, &db) != SQLITE_OK) return;

    char status_filter[512] = "";
    int any_filter = logs_build_status_filter(status_filter);

    if (!any_filter) {
        GtkTextBuffer *logs_buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(logs_text));
        gtk_text_buffer_set_text(logs_buffer, "No filters selected. Select at least one filter or 'All'.", -1);
        sqlite3_close(db);
        return;
    }

    char query[1024];
    snprintf(query, sizeof(query),
             "SELECT status, full_path FROM run_logs WHERE run_id = ? AND (%s) ORDER BY id",
             status_filter);

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, query, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return;
    }

    sqlite3_bind_int64(stmt, 1, logs_selected_run_id);

    GString *text = g_string_new("");
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *status = (const char *)sqlite3_column_text(stmt, 0);
        const char *path = (const char *)sqlite3_column_text(stmt, 1);
        g_string_append_printf(text, "[%-18s] %s\n", status, path);
    }

    GtkTextBuffer *logs_buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(logs_text));
    gtk_text_buffer_set_text(logs_buffer, text->str, -1);
    g_string_free(text, TRUE);

    sqlite3_finalize(stmt);
    sqlite3_close(db);
}

static void logs_alert(const char *msg) {
    GtkAlertDialog *alert = gtk_alert_dialog_new("%s", msg);
    gtk_alert_dialog_show(alert, GTK_WINDOW(window));
    g_object_unref(alert);
}

static void on_logs_export_response(GObject *source, GAsyncResult *result, gpointer user_data) {
    (void)user_data;

    GFile *file = gtk_file_dialog_save_finish(GTK_FILE_DIALOG(source), result, NULL);
    if (!file) return;

    char *path = g_file_get_path(file);
    g_object_unref(file);
    if (!path) return;

    char status_filter[512] = "";
    if (!logs_build_status_filter(status_filter)) {
        logs_alert("No filters selected. Select at least one filter or 'All'.");
        g_free(path);
        return;
    }

    sqlite3 *db;
    if (sqlite3_open(logs_current_db_path, &db) != SQLITE_OK) {
        logs_alert("Failed to open database");
        g_free(path);
        return;
    }

    char query[1024];
    snprintf(query, sizeof(query),
             "SELECT status, full_path FROM run_logs WHERE run_id = ? AND (%s) ORDER BY id",
             status_filter);

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, query, -1, &stmt, NULL) != SQLITE_OK) {
        logs_alert("Failed to query logs");
        sqlite3_close(db);
        g_free(path);
        return;
    }
    sqlite3_bind_int64(stmt, 1, logs_selected_run_id);

    FILE *fp = fopen(path, "w");
    if (!fp) {
        logs_alert("Failed to create export file");
        sqlite3_finalize(stmt);
        sqlite3_close(db);
        g_free(path);
        return;
    }

    fprintf(fp, "Status,Full Path\n");
    long count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        csv_field(fp, (const char *)sqlite3_column_text(stmt, 0));
        fputc(',', fp);
        csv_field(fp, (const char *)sqlite3_column_text(stmt, 1));
        fputc('\n', fp);
        count++;
    }
    fclose(fp);
    sqlite3_finalize(stmt);
    sqlite3_close(db);

    char msg[MAX_PATH + 64];
    snprintf(msg, sizeof(msg), "Exported %ld log records to %s", count, path);
    logs_alert(msg);
    g_free(path);
}

void on_logs_export_clicked(GtkButton *button, gpointer user_data) {
    (void)button; (void)user_data;

    if (logs_selected_run_id == 0) {
        logs_alert("Please select a run to export");
        return;
    }

    char name[64];
    snprintf(name, sizeof(name), "run_%lld_logs.csv", (long long)logs_selected_run_id);

    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Export Logs to CSV");
    gtk_file_dialog_set_initial_name(dialog, name);
    gtk_file_dialog_save(dialog, GTK_WINDOW(window), NULL, on_logs_export_response, NULL);
    g_object_unref(dialog);
}

void on_logs_db_changed(GtkComboBox *combo, gpointer user_data) {
    (void)user_data;
    char *db_name = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(combo));
    if (db_name) {
        logs_load_runs(db_name);
        g_free(db_name);
    }
}

void on_logs_run_selected(GtkListBox *box, GtkListBoxRow *row, gpointer user_data) {
    (void)box; (void)user_data;
    if (!row) return;

    GtkWidget *row_widget = gtk_list_box_row_get_child(GTK_LIST_BOX_ROW(row));
    logs_selected_run_id = (sqlite3_int64)GPOINTER_TO_INT(g_object_get_data(G_OBJECT(row_widget), "run_id"));

    logs_load_details();
    logs_load_logs();
}

void on_logs_filter_toggled(GtkCheckButton *button, gpointer user_data) {
    (void)user_data;

    if (button == GTK_CHECK_BUTTON(logs_filter_all_check) &&
        gtk_check_button_get_active(GTK_CHECK_BUTTON(logs_filter_all_check))) {
        gtk_check_button_set_active(GTK_CHECK_BUTTON(logs_filter_new_check), FALSE);
        gtk_check_button_set_active(GTK_CHECK_BUTTON(logs_filter_changed_check), FALSE);
        gtk_check_button_set_active(GTK_CHECK_BUTTON(logs_filter_missing_check), FALSE);
        gtk_check_button_set_active(GTK_CHECK_BUTTON(logs_filter_unchanged_check), FALSE);
        gtk_check_button_set_active(GTK_CHECK_BUTTON(logs_filter_ignored_check), FALSE);
        gtk_check_button_set_active(GTK_CHECK_BUTTON(logs_filter_errors_check), FALSE);
    } else if (button != GTK_CHECK_BUTTON(logs_filter_all_check) &&
               gtk_check_button_get_active(button)) {
        gtk_check_button_set_active(GTK_CHECK_BUTTON(logs_filter_all_check), FALSE);
    }

    logs_load_logs();
}

void on_logs_save_note_clicked(GtkButton *button, gpointer user_data) {
    (void)button; (void)user_data;

    if (logs_selected_run_id == 0) {
        GtkAlertDialog *alert = gtk_alert_dialog_new("Please select a run to update");
        gtk_alert_dialog_show(alert, GTK_WINDOW(window));
        g_object_unref(alert);
        return;
    }

    // Get note text
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(logs_note_text));
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(buffer, &start, &end);
    char *note = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);

    // Open database
    sqlite3 *db;
    if (sqlite3_open(logs_current_db_path, &db) != SQLITE_OK) {
        GtkAlertDialog *alert = gtk_alert_dialog_new("Failed to open database");
        gtk_alert_dialog_show(alert, GTK_WINDOW(window));
        g_object_unref(alert);
        g_free(note);
        return;
    }

    // Update note in database
    const char *update_sql = "UPDATE meta SET note = ? WHERE id = ?";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, update_sql, -1, &stmt, 0) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, note, -1, SQLITE_STATIC);
        sqlite3_bind_int64(stmt, 2, logs_selected_run_id);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    sqlite3_close(db);
    g_free(note);

    GtkAlertDialog *alert = gtk_alert_dialog_new("Note saved successfully");
    gtk_alert_dialog_show(alert, GTK_WINDOW(window));
    g_object_unref(alert);
}

void on_logs_delete_confirmed(GObject *source, GAsyncResult *result, gpointer user_data);

void on_logs_delete_run_clicked(GtkButton *button, gpointer user_data) {
    (void)button; (void)user_data;

    if (logs_selected_run_id == 0) {
        GtkAlertDialog *alert = gtk_alert_dialog_new("Please select a run to delete");
        gtk_alert_dialog_show(alert, GTK_WINDOW(window));
        g_object_unref(alert);
        return;
    }

    // Open database
    sqlite3 *db;
    if (sqlite3_open(logs_current_db_path, &db) != SQLITE_OK) {
        GtkAlertDialog *alert = gtk_alert_dialog_new("Failed to open database");
        gtk_alert_dialog_show(alert, GTK_WINDOW(window));
        g_object_unref(alert);
        return;
    }

    // Get run info for confirmation message
    char run_info[256] = "this run";
    const char *query = "SELECT last_date_verify FROM meta WHERE id = ?";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, query, -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, logs_selected_run_id);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *date = (const char *)sqlite3_column_text(stmt, 0);
            if (date) {
                snprintf(run_info, sizeof(run_info), "Run #%lld from %s", logs_selected_run_id, date);
            }
        }
        sqlite3_finalize(stmt);
    }

    sqlite3_close(db);

    // Confirm deletion
    char message[512];
    snprintf(message, sizeof(message),
            "Delete %s?\n\nThis will remove:\n"
            "• Run metadata from the database\n"
            "• All log entries for this run\n\n"
            "This action cannot be undone.",
            run_info);

    GtkAlertDialog *confirm = gtk_alert_dialog_new("%s", message);
    gtk_alert_dialog_set_buttons(confirm, (const char *[]){"Cancel", "Delete", NULL});
    gtk_alert_dialog_set_cancel_button(confirm, 0);
    gtk_alert_dialog_set_default_button(confirm, 0);

    // Use async API to get response
    g_object_set_data(G_OBJECT(confirm), "run_id", GINT_TO_POINTER((int)logs_selected_run_id));
    gtk_alert_dialog_choose(confirm, GTK_WINDOW(window), NULL,
                           (GAsyncReadyCallback)on_logs_delete_confirmed, NULL);
}

void on_logs_delete_confirmed(GObject *source, GAsyncResult *result, gpointer user_data) {
    (void)user_data;

    GtkAlertDialog *dialog = GTK_ALERT_DIALOG(source);
    int response = gtk_alert_dialog_choose_finish(dialog, result, NULL);

    if (response != 1) {  // Not "Delete" button
        g_object_unref(dialog);
        return;
    }

    sqlite3_int64 run_id = (sqlite3_int64)GPOINTER_TO_INT(g_object_get_data(source, "run_id"));
    g_object_unref(dialog);

    // Open database
    sqlite3 *db;
    if (sqlite3_open(logs_current_db_path, &db) != SQLITE_OK) {
        GtkAlertDialog *alert = gtk_alert_dialog_new("Failed to open database");
        gtk_alert_dialog_show(alert, GTK_WINDOW(window));
        g_object_unref(alert);
        return;
    }

    // Delete run_logs entries first (foreign key constraint)
    const char *delete_logs_sql = "DELETE FROM run_logs WHERE run_id = ?";
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, delete_logs_sql, -1, &stmt, 0) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, run_id);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    // Delete meta entry
    const char *delete_meta_sql = "DELETE FROM meta WHERE id = ?";
    if (sqlite3_prepare_v2(db, delete_meta_sql, -1, &stmt, 0) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, run_id);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    sqlite3_close(db);

    // Clear selection and refresh
    logs_selected_run_id = 0;

    // Clear text buffers
    GtkTextBuffer *info_buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(logs_run_info_text));
    gtk_text_buffer_set_text(info_buffer, "", -1);
    GtkTextBuffer *note_buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(logs_note_text));
    gtk_text_buffer_set_text(note_buffer, "", -1);
    GtkTextBuffer *logs_buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(logs_text));
    gtk_text_buffer_set_text(logs_buffer, "", -1);

    // Reload runs list
    const char *db_name = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(logs_db_combo));
    if (db_name) {
        logs_load_runs(db_name);
        g_free((void *)db_name);
    }

    GtkAlertDialog *alert = gtk_alert_dialog_new("Run deleted successfully");
    gtk_alert_dialog_show(alert, GTK_WINDOW(window));
    g_object_unref(alert);
}

GtkWidget *create_logs_tab() {
    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);

    // Left: Database and runs list
    GtkWidget *left_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_size_request(left_box, 350, -1);
    gtk_widget_set_margin_start(left_box, 8);
    gtk_widget_set_margin_end(left_box, 8);
    gtk_widget_set_margin_top(left_box, 8);
    gtk_widget_set_margin_bottom(left_box, 8);

    GtkWidget *db_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *db_label = gtk_label_new("Database:");
    logs_db_combo = gtk_combo_box_text_new();
    g_signal_connect(logs_db_combo, "changed", G_CALLBACK(on_logs_db_changed), NULL);
    gtk_widget_set_hexpand(logs_db_combo, TRUE);
    GtkWidget *refresh_btn = gtk_button_new_with_label("Refresh");
    gtk_widget_set_tooltip_text(refresh_btn, "Reload the database list and scan runs");
    g_signal_connect(refresh_btn, "clicked", G_CALLBACK((GCallback)logs_refresh_databases), NULL);
    gtk_box_append(GTK_BOX(db_box), db_label);
    gtk_box_append(GTK_BOX(db_box), logs_db_combo);
    gtk_box_append(GTK_BOX(db_box), refresh_btn);
    gtk_box_append(GTK_BOX(left_box), db_box);

    GtkWidget *runs_label = gtk_label_new("Scan Runs:");
    gtk_label_set_xalign(GTK_LABEL(runs_label), 0.0);
    gtk_box_append(GTK_BOX(left_box), runs_label);

    logs_runs_list = gtk_list_box_new();
    g_signal_connect(logs_runs_list, "row-activated", G_CALLBACK(on_logs_run_selected), NULL);
    GtkWidget *runs_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(runs_scroll), logs_runs_list);
    gtk_widget_set_vexpand(runs_scroll, TRUE);
    gtk_box_append(GTK_BOX(left_box), runs_scroll);

    GtkWidget *delete_btn = gtk_button_new_with_label("Delete Selected Run");
    gtk_widget_add_css_class(delete_btn, "destructive-action");
    g_signal_connect(delete_btn, "clicked", G_CALLBACK(on_logs_delete_run_clicked), NULL);
    gtk_box_append(GTK_BOX(left_box), delete_btn);

    gtk_paned_set_start_child(GTK_PANED(paned), left_box);

    // Right: Run details and logs
    GtkWidget *right_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(right_box, 8);
    gtk_widget_set_margin_end(right_box, 8);
    gtk_widget_set_margin_top(right_box, 8);
    gtk_widget_set_margin_bottom(right_box, 8);

    // Run info
    GtkWidget *info_label = gtk_label_new("Run Information:");
    gtk_label_set_xalign(GTK_LABEL(info_label), 0.0);
    logs_run_info_text = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(logs_run_info_text), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(logs_run_info_text), TRUE);
    GtkWidget *info_scroll = gtk_scrolled_window_new();
    gtk_widget_set_size_request(info_scroll, -1, 220);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(info_scroll), logs_run_info_text);
    gtk_box_append(GTK_BOX(right_box), info_label);
    gtk_box_append(GTK_BOX(right_box), info_scroll);

    // Note
    GtkWidget *note_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *note_label = gtk_label_new("Note:");
    gtk_label_set_xalign(GTK_LABEL(note_label), 0.0);
    gtk_widget_set_hexpand(note_label, TRUE);

    GtkWidget *save_note_btn = gtk_button_new_with_label("Save Note");
    gtk_widget_add_css_class(save_note_btn, "suggested-action");
    g_signal_connect(save_note_btn, "clicked", G_CALLBACK(on_logs_save_note_clicked), NULL);

    gtk_box_append(GTK_BOX(note_box), note_label);
    gtk_box_append(GTK_BOX(note_box), save_note_btn);

    logs_note_text = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(logs_note_text), TRUE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(logs_note_text), GTK_WRAP_WORD);
    gtk_widget_set_size_request(logs_note_text, -1, 60);
    GtkWidget *note_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(note_scroll), logs_note_text);
    gtk_box_append(GTK_BOX(right_box), note_box);
    gtk_box_append(GTK_BOX(right_box), note_scroll);

    // Filters
    GtkWidget *filter_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *filter_label = gtk_label_new("Filters:");

    logs_filter_all_check = gtk_check_button_new_with_label("All");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(logs_filter_all_check), TRUE);
    g_signal_connect(logs_filter_all_check, "toggled", G_CALLBACK(on_logs_filter_toggled), NULL);

    logs_filter_new_check = gtk_check_button_new_with_label("New");
    g_signal_connect(logs_filter_new_check, "toggled", G_CALLBACK(on_logs_filter_toggled), NULL);

    logs_filter_changed_check = gtk_check_button_new_with_label("Changed");
    g_signal_connect(logs_filter_changed_check, "toggled", G_CALLBACK(on_logs_filter_toggled), NULL);

    logs_filter_missing_check = gtk_check_button_new_with_label("Missing");
    g_signal_connect(logs_filter_missing_check, "toggled", G_CALLBACK(on_logs_filter_toggled), NULL);

    logs_filter_unchanged_check = gtk_check_button_new_with_label("Unchanged");
    g_signal_connect(logs_filter_unchanged_check, "toggled", G_CALLBACK(on_logs_filter_toggled), NULL);

    logs_filter_ignored_check = gtk_check_button_new_with_label("Ignored");
    g_signal_connect(logs_filter_ignored_check, "toggled", G_CALLBACK(on_logs_filter_toggled), NULL);

    logs_filter_errors_check = gtk_check_button_new_with_label("Errors");
    g_signal_connect(logs_filter_errors_check, "toggled", G_CALLBACK(on_logs_filter_toggled), NULL);

    gtk_box_append(GTK_BOX(filter_box), filter_label);
    gtk_box_append(GTK_BOX(filter_box), logs_filter_all_check);
    gtk_box_append(GTK_BOX(filter_box), logs_filter_new_check);
    gtk_box_append(GTK_BOX(filter_box), logs_filter_changed_check);
    gtk_box_append(GTK_BOX(filter_box), logs_filter_missing_check);
    gtk_box_append(GTK_BOX(filter_box), logs_filter_unchanged_check);
    gtk_box_append(GTK_BOX(filter_box), logs_filter_ignored_check);
    gtk_box_append(GTK_BOX(filter_box), logs_filter_errors_check);

    GtkWidget *export_btn = gtk_button_new_with_label("Export");
    gtk_widget_set_hexpand(export_btn, TRUE);
    gtk_widget_set_halign(export_btn, GTK_ALIGN_END);
    g_signal_connect(export_btn, "clicked", G_CALLBACK(on_logs_export_clicked), NULL);
    gtk_box_append(GTK_BOX(filter_box), export_btn);
    gtk_box_append(GTK_BOX(right_box), filter_box);

    // Logs
    GtkWidget *logs_label = gtk_label_new("Logs:");
    gtk_label_set_xalign(GTK_LABEL(logs_label), 0.0);
    logs_text = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(logs_text), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(logs_text), TRUE);
    gtk_widget_set_vexpand(logs_text, TRUE);
    GtkWidget *logs_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(logs_scroll), logs_text);
    gtk_widget_set_vexpand(logs_scroll, TRUE);
    gtk_box_append(GTK_BOX(right_box), logs_label);
    gtk_box_append(GTK_BOX(right_box), logs_scroll);

    gtk_paned_set_end_child(GTK_PANED(paned), right_box);
    gtk_paned_set_position(GTK_PANED(paned), 370);

    return paned;
}

// ============================================================================
// TAB 5: FULL FILE SCANNER
// ============================================================================

typedef struct {
    char status[32];
    char path[MAX_PATH];
    char checksum[HASH_SIZE];
    long long size;
    long long mtime;
} ScanLogEntry;

typedef struct {
    char scan_path[MAX_PATH];
    char db_path[MAX_PATH];
    char db_name[256];
    int enable_checksum;
    int update_mode;
    char note[1024];
    sqlite3 *db;
    sqlite3_int64 run_id;
    int unchanged, changed, new_files, missing, ignored, errors;
    int total_files;
    int should_stop;
    ScanLogEntry *log_buffer;
    int log_count;
    int log_capacity;
    GThread *scan_thread;
    int num_workers;
    GThreadPool *pool;
    GMutex lock;  // guards db, counters and log_buffer while workers are running
    char drive_error[512];  // set when the drive's drives.db entry could not be added or updated
    char open_error[512];   // set when the scan path itself could not be opened
} ScannerContext;

// Most files queued for the workers before the directory walk pauses
#define SCANNER_MAX_QUEUED 1000

GtkWidget *scanner_volumes_list;
GtkWidget *scanner_path_entry;
GtkWidget *scanner_db_entry;
GtkWidget *scanner_checksum_check;
GtkWidget *scanner_update_check;
GtkWidget *scanner_workers_spin;
GtkWidget *scanner_note_text;
GtkWidget *scanner_start_button;
GtkWidget *scanner_stop_button;
GtkProgressBar *scanner_progress_bar;
GtkWidget *scanner_status_label;
GtkWidget *scanner_current_file_label;
GtkWidget *scanner_results_text;

ScannerContext *current_scanner_scan = NULL;

void scanner_log_message(ScannerContext *ctx, const char *status, const char *path, const char *checksum, long long size, long long mtime) {
    if (ctx->log_count >= ctx->log_capacity) {
        ctx->log_capacity = ctx->log_capacity == 0 ? 1024 : ctx->log_capacity * 2;
        ctx->log_buffer = realloc(ctx->log_buffer, ctx->log_capacity * sizeof(ScanLogEntry));
    }
    strncpy(ctx->log_buffer[ctx->log_count].status, status, 31);
    ctx->log_buffer[ctx->log_count].status[31] = '\0';
    strncpy(ctx->log_buffer[ctx->log_count].path, path, MAX_PATH - 1);
    ctx->log_buffer[ctx->log_count].path[MAX_PATH - 1] = '\0';
    strncpy(ctx->log_buffer[ctx->log_count].checksum, checksum ? checksum : "", HASH_SIZE - 1);
    ctx->log_buffer[ctx->log_count].checksum[HASH_SIZE - 1] = '\0';
    ctx->log_buffer[ctx->log_count].size = size;
    ctx->log_buffer[ctx->log_count].mtime = mtime;
    ctx->log_count++;
}

gboolean scanner_update_progress(gpointer data) {
    ScannerContext *ctx = (ScannerContext *)data;
    int processed = ctx->unchanged + ctx->changed + ctx->new_files + ctx->missing + ctx->errors;

    if (ctx->total_files > 0) {
        gtk_progress_bar_set_fraction(scanner_progress_bar, (double)processed / ctx->total_files);
    }

    char status[512], n[8][32];
    snprintf(status, sizeof(status),
             "Processed: %s/%s | Unch: %s | Chg: %s | New: %s | Miss: %s | Ign: %s | Err: %s",
             format_count(processed, n[0], sizeof(n[0])), format_count(ctx->total_files, n[1], sizeof(n[1])),
             format_count(ctx->unchanged, n[2], sizeof(n[2])), format_count(ctx->changed, n[3], sizeof(n[3])),
             format_count(ctx->new_files, n[4], sizeof(n[4])), format_count(ctx->missing, n[5], sizeof(n[5])),
             format_count(ctx->ignored, n[6], sizeof(n[6])), format_count(ctx->errors, n[7], sizeof(n[7])));
    gtk_label_set_text(GTK_LABEL(scanner_status_label), status);
    return G_SOURCE_REMOVE;
}

gboolean scanner_update_current_file(gpointer data) {
    char *filename = (char *)data;
    gtk_label_set_text(GTK_LABEL(scanner_current_file_label), filename);
    g_free(filename);
    return G_SOURCE_REMOVE;
}

gboolean scanner_scan_completed(gpointer data);

// Older databases have no files.status column; add it so files can be marked MISSING
static void scanner_ensure_status_column(sqlite3 *db) {
    sqlite3_stmt *stmt;
    int has_status = 0;
    if (sqlite3_prepare_v2(db, "PRAGMA table_info(files)", -1, &stmt, NULL) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *col = (const char *)sqlite3_column_text(stmt, 1);
            if (col && strcmp(col, "status") == 0) has_status = 1;
        }
        sqlite3_finalize(stmt);
    }
    if (!has_status) sqlite3_exec(db, "ALTER TABLE files ADD COLUMN status TEXT;", 0, 0, 0);
}

// Files recorded under the scan path that are gone from disk. In update mode they are marked
// MISSING in the files table; rows already marked MISSING are skipped so they are reported once.
static void scanner_find_missing(ScannerContext *ctx) {
    char prefix[MAX_PATH];
    size_t len = strlen(ctx->scan_path);
    snprintf(prefix, sizeof(prefix), "%s%s", ctx->scan_path, (len > 0 && ctx->scan_path[len - 1] == '/') ? "" : "/");

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(ctx->db,
            "SELECT id, full_path FROM files WHERE substr(full_path, 1, length(?1)) = ?1 "
            "AND (status IS NULL OR status != 'MISSING')", -1, &stmt, NULL) != SQLITE_OK) return;
    sqlite3_bind_text(stmt, 1, prefix, -1, SQLITE_STATIC);

    // Collect ids first; the files table is modified after the query has finished
    sqlite3_int64 *ids = NULL;
    int id_count = 0, id_capacity = 0;
    while (!ctx->should_stop && sqlite3_step(stmt) == SQLITE_ROW) {
        const char *path = (const char *)sqlite3_column_text(stmt, 1);
        struct stat sb;
        if (!path || stat(path, &sb) == 0 || errno != ENOENT) continue;
        // Entries recorded before an ignore pattern matched them are not reported as missing
        if (path_is_ignored(ctx->scan_path, path)) continue;

        ctx->missing++;
        scanner_log_message(ctx, "MISSING", path, "", 0, 0);
        if (id_count >= id_capacity) {
            id_capacity = id_capacity == 0 ? 64 : id_capacity * 2;
            ids = realloc(ids, id_capacity * sizeof(sqlite3_int64));
        }
        ids[id_count++] = sqlite3_column_int64(stmt, 0);
    }
    sqlite3_finalize(stmt);

    if (ctx->update_mode && id_count > 0) {
        sqlite3_stmt *up;
        if (sqlite3_prepare_v2(ctx->db, "UPDATE files SET status = 'MISSING' WHERE id = ?", -1, &up, NULL) == SQLITE_OK) {
            for (int i = 0; i < id_count; i++) {
                sqlite3_bind_int64(up, 1, ids[i]);
                sqlite3_step(up);
                sqlite3_reset(up);
            }
            sqlite3_finalize(up);
        }
    }
    free(ids);
    g_idle_add(scanner_update_progress, ctx);
}

void scanner_process_ignored_file(ScannerContext *ctx, const char *filepath, const char *filename) {
    struct stat sb;

    // Always log ignored files, even if they can't be stat'd or aren't regular files
    scanner_log_message(ctx, "IGNORED", filepath, "", 0, 0);

    if (stat(filepath, &sb) != 0 || !S_ISREG(sb.st_mode)) {
        ctx->ignored++;
        g_idle_add(scanner_update_progress, ctx);
        return;
    }

    if (ctx->update_mode) {
        // Check if file already exists in database
        sqlite3_stmt *check_stmt;
        if (sqlite3_prepare_v2(ctx->db, "SELECT id FROM files WHERE full_path = ? LIMIT 1", -1, &check_stmt, 0) == SQLITE_OK) {
            sqlite3_bind_text(check_stmt, 1, filepath, -1, SQLITE_STATIC);
            int exists = (sqlite3_step(check_stmt) == SQLITE_ROW);
            sqlite3_finalize(check_stmt);

            if (!exists) {
                // Add to files table without checksum
                sqlite3_stmt *ins;
                if (sqlite3_prepare_v2(ctx->db, "INSERT INTO files (file_name, full_path, size, created, last_modified, owner, checksum) VALUES (?, ?, ?, ?, ?, ?, ?)", -1, &ins, NULL) == SQLITE_OK) {
                    struct passwd *pw = getpwuid(sb.st_uid);
                    sqlite3_bind_text(ins, 1, filename, -1, SQLITE_STATIC);
                    sqlite3_bind_text(ins, 2, filepath, -1, SQLITE_STATIC);
                    sqlite3_bind_int64(ins, 3, sb.st_size);
                    sqlite3_bind_int64(ins, 4, sb.st_ctime);
                    sqlite3_bind_int64(ins, 5, sb.st_mtime);
                    sqlite3_bind_text(ins, 6, pw ? pw->pw_name : "unknown", -1, SQLITE_STATIC);
                    sqlite3_bind_text(ins, 7, "", -1, SQLITE_STATIC);  // Empty checksum
                    sqlite3_step(ins);
                    sqlite3_finalize(ins);
                }
            }
        }
    }

    ctx->ignored++;
    g_idle_add(scanner_update_progress, ctx);
}

static void scanner_update_file_row(ScannerContext *ctx, const char *filepath, const struct stat *sb, const char *checksum) {
    sqlite3_stmt *up;
    sqlite3_prepare_v2(ctx->db, "UPDATE files SET size=?, last_modified=?, checksum=? WHERE full_path=?", -1, &up, NULL);
    sqlite3_bind_int64(up, 1, sb->st_size);
    sqlite3_bind_int64(up, 2, sb->st_mtime);
    sqlite3_bind_text(up, 3, checksum, -1, SQLITE_STATIC);
    sqlite3_bind_text(up, 4, filepath, -1, SQLITE_STATIC);
    sqlite3_step(up);
    sqlite3_finalize(up);
}

// Runs on a worker thread. The database, counters and run log are shared, so they are only
// touched while holding ctx->lock; checksums are computed outside it so workers hash in parallel.
void scanner_process_file(ScannerContext *ctx, const char *filepath, const char *filename) {
    if (ctx->should_stop) return;

    g_idle_add(scanner_update_current_file, g_strdup(filename));

    char log_mesg[256];

    struct stat sb;
    if (stat(filepath, &sb) != 0 || !S_ISREG(sb.st_mode)) return;

    long long db_size = 0, db_mtime = 0;
    char db_checksum[HASH_SIZE] = "";
    int found, reappeared = 0;

    g_mutex_lock(&ctx->lock);
    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(ctx->db, "SELECT size, last_modified, checksum, status FROM files WHERE full_path = ?", -1, &stmt, NULL);
    sqlite3_bind_text(stmt, 1, filepath, -1, SQLITE_STATIC);
    found = (sqlite3_step(stmt) == SQLITE_ROW);
    if (found) {
        const char *db_status = (const char *)sqlite3_column_text(stmt, 3);
        // A file previously marked MISSING that is back on disk is treated as new
        reappeared = db_status && strcmp(db_status, "MISSING") == 0;
        db_size = sqlite3_column_int64(stmt, 0);
        db_mtime = sqlite3_column_int64(stmt, 1);
        const char *c = (const char *)sqlite3_column_text(stmt, 2);
        if (c) g_strlcpy(db_checksum, c, sizeof(db_checksum));
    }
    sqlite3_finalize(stmt);
    g_mutex_unlock(&ctx->lock);

    if (found && !reappeared) {
        char checksum[HASH_SIZE] = "";
        int has_checksum = 0;
        if (ctx->enable_checksum) {
            has_checksum = compute_sha256(filepath, checksum);
        }

        if (ctx->enable_checksum && has_checksum && strlen(db_checksum) > 0 && strcmp(db_checksum, checksum) != 0) {
            if ((db_size != sb.st_size) && (db_mtime != sb.st_mtime)) {
	        snprintf(log_mesg, sizeof(log_mesg), "CHANGED: CheckSum, Date-Time, File-Size");
            }
            else if((db_size != sb.st_size) && (db_mtime == sb.st_mtime)) {
	        snprintf(log_mesg, sizeof(log_mesg), "CHANGED: CheckSum, File-Size");
            }
            else if((db_size == sb.st_size) && (db_mtime != sb.st_mtime)) {
	        snprintf(log_mesg, sizeof(log_mesg), "CHANGED: CheckSum, Date-Time");
            }
            else {
	        snprintf(log_mesg, sizeof(log_mesg), "CHANGED: CheckSum");
            }

            g_mutex_lock(&ctx->lock);
            ctx->changed++;
            scanner_log_message(ctx, log_mesg, filepath, checksum, sb.st_size, sb.st_mtime);
            if (ctx->update_mode) scanner_update_file_row(ctx, filepath, &sb, checksum);
            g_mutex_unlock(&ctx->lock);

        } else if (db_size != sb.st_size || db_mtime != sb.st_mtime) {
            if ((db_size != sb.st_size) && (db_mtime != sb.st_mtime)) {
	        snprintf(log_mesg, sizeof(log_mesg), "CHANGED: Date-Time, File-Size");
            }
            else if((db_size != sb.st_size) && (db_mtime == sb.st_mtime)) {
	        snprintf(log_mesg, sizeof(log_mesg), "CHANGED: File-Size");
            }
            else if((db_size == sb.st_size) && (db_mtime != sb.st_mtime)) {
	        snprintf(log_mesg, sizeof(log_mesg), "CHANGED: Date-Time");
            }
            else {
	        snprintf(log_mesg, sizeof(log_mesg), "CHANGED: CheckSum");
            }

            // Always store a checksum that matches the new content
            char new_checksum[HASH_SIZE];
            g_strlcpy(new_checksum, checksum, sizeof(new_checksum));
            if (ctx->update_mode && !has_checksum) {
                compute_sha256(filepath, new_checksum);
            }

            g_mutex_lock(&ctx->lock);
            ctx->changed++;
            scanner_log_message(ctx, log_mesg, filepath, checksum, sb.st_size, sb.st_mtime);
            if (ctx->update_mode) scanner_update_file_row(ctx, filepath, &sb, new_checksum);
            g_mutex_unlock(&ctx->lock);
        } else {
            g_mutex_lock(&ctx->lock);
            ctx->unchanged++;
            scanner_log_message(ctx, "UNCHANGED", filepath, db_checksum, sb.st_size, sb.st_mtime);
            g_mutex_unlock(&ctx->lock);
        }
    } else {
        char checksum[HASH_SIZE] = "";
        if (ctx->enable_checksum) compute_sha256(filepath, checksum);

        g_mutex_lock(&ctx->lock);
        ctx->new_files++;
        scanner_log_message(ctx, "NEW", filepath, checksum, sb.st_size, sb.st_mtime);
        if (ctx->update_mode && reappeared) {
            sqlite3_stmt *up;
            sqlite3_prepare_v2(ctx->db, "UPDATE files SET size=?, last_modified=?, checksum=?, status=NULL WHERE full_path=?", -1, &up, NULL);
            sqlite3_bind_int64(up, 1, sb.st_size);
            sqlite3_bind_int64(up, 2, sb.st_mtime);
            sqlite3_bind_text(up, 3, checksum, -1, SQLITE_STATIC);
            sqlite3_bind_text(up, 4, filepath, -1, SQLITE_STATIC);
            sqlite3_step(up);
            sqlite3_finalize(up);
        } else if (ctx->update_mode) {
            sqlite3_stmt *ins;
            sqlite3_prepare_v2(ctx->db, "INSERT INTO files (file_name, full_path, size, created, last_modified, owner, checksum) VALUES (?, ?, ?, ?, ?, ?, ?)", -1, &ins, NULL);
            sqlite3_bind_text(ins, 1, filename, -1, SQLITE_STATIC);
            sqlite3_bind_text(ins, 2, filepath, -1, SQLITE_STATIC);
            sqlite3_bind_int64(ins, 3, sb.st_size);
            sqlite3_bind_int64(ins, 4, sb.st_ctime);
            sqlite3_bind_int64(ins, 5, sb.st_mtime);
            struct passwd *pw = getpwuid(sb.st_uid);
            sqlite3_bind_text(ins, 6, pw ? pw->pw_name : "unknown", -1, SQLITE_STATIC);
            sqlite3_bind_text(ins, 7, checksum, -1, SQLITE_STATIC);
            sqlite3_step(ins);
            sqlite3_finalize(ins);
        }
        g_mutex_unlock(&ctx->lock);
    }

    g_idle_add(scanner_update_progress, ctx);
}

static void scanner_worker_func(gpointer data, gpointer user_data) {
    char *filepath = (char *)data;
    const char *slash = strrchr(filepath, '/');
    scanner_process_file((ScannerContext *)user_data, filepath, slash ? slash + 1 : filepath);
    g_free(filepath);
}

void scanner_scan_directory(ScannerContext *ctx, const char *dirpath) {
    if (ctx->should_stop) return;

    DIR *dir = opendir(dirpath);
    if (!dir) {
        // Record unreadable directories (e.g. macOS denied access to the volume) as errors
        // rather than silently scanning nothing
        int err = errno;
        g_mutex_lock(&ctx->lock);
        ctx->errors++;
        scanner_log_message(ctx, "ERROR", dirpath, "", 0, 0);
        if (strcmp(dirpath, ctx->scan_path) == 0) {
            snprintf(ctx->open_error, sizeof(ctx->open_error), "%s", strerror(err));
        }
        g_mutex_unlock(&ctx->lock);
        g_idle_add(scanner_update_progress, ctx);
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (ctx->should_stop) break;
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;

        char filepath[MAX_PATH];
        snprintf(filepath, sizeof(filepath), "%s/%s", dirpath, entry->d_name);

        struct stat sb;
        int stat_ok = (stat(filepath, &sb) == 0);

        if (is_ignored(path_below_root(ctx->scan_path, filepath), stat_ok && S_ISDIR(sb.st_mode))) {
            // Process ignored files to record them in database
            g_mutex_lock(&ctx->lock);
            if (stat_ok && S_ISREG(sb.st_mode)) {
                scanner_process_ignored_file(ctx, filepath, entry->d_name);
            } else {
                // Log non-regular ignored files (directories, symlinks, etc.)
                scanner_log_message(ctx, "IGNORED", filepath, "", 0, 0);
                ctx->ignored++;
            }
            g_mutex_unlock(&ctx->lock);
            continue;
        }

        if (stat_ok) {
            if (S_ISDIR(sb.st_mode)) {
                scanner_scan_directory(ctx, filepath);
            } else if (S_ISREG(sb.st_mode)) {
                // Keep the queue bounded so the walk doesn't buffer the whole drive in memory
                while (g_thread_pool_unprocessed(ctx->pool) > SCANNER_MAX_QUEUED && !ctx->should_stop) {
                    g_usleep(1000);
                }
                g_thread_pool_push(ctx->pool, g_strdup(filepath), NULL);
            }
        }
    }
    closedir(dir);
}

int scanner_count_files(const char *root, const char *dirpath) {
    int count = 0;
    DIR *dir = opendir(dirpath);
    if (!dir) return 0;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        char filepath[MAX_PATH];
        snprintf(filepath, sizeof(filepath), "%s/%s", dirpath, entry->d_name);
        struct stat sb;
        if (stat(filepath, &sb) == 0) {
            if (is_ignored(path_below_root(root, filepath), S_ISDIR(sb.st_mode))) continue;
            if (S_ISDIR(sb.st_mode)) count += scanner_count_files(root, filepath);
            else if (S_ISREG(sb.st_mode)) count++;
        }
    }
    closedir(dir);
    return count;
}

gpointer scanner_thread_func(gpointer data) {
    ScannerContext *ctx = (ScannerContext *)data;

    if (sqlite3_open(ctx->db_path, &ctx->db) != SQLITE_OK) {
        g_idle_add(scanner_scan_completed, ctx);
        return NULL;
    }

    sqlite3_exec(ctx->db, "CREATE TABLE IF NOT EXISTS files (id INTEGER PRIMARY KEY, file_name TEXT, full_path TEXT UNIQUE, size INTEGER, created INTEGER, last_modified INTEGER, owner TEXT, checksum TEXT, keywords TEXT, status TEXT);", 0, 0, 0);
    scanner_ensure_status_column(ctx->db);
    sqlite3_exec(ctx->db, "CREATE TABLE IF NOT EXISTS meta (id INTEGER PRIMARY KEY AUTOINCREMENT, last_checksum_verify_date TEXT, last_date_verify TEXT, verify_machine TEXT, num_unchanged INTEGER, num_changed INTEGER, num_new INTEGER, num_missing INTEGER, num_ignored INTEGER, num_errors INTEGER, update_mode TEXT, note TEXT);", 0, 0, 0);
    sqlite3_exec(ctx->db, "CREATE TABLE IF NOT EXISTS run_logs (id INTEGER PRIMARY KEY AUTOINCREMENT, run_id INTEGER, status TEXT, full_path TEXT, checksum TEXT, size INTEGER, mtime INTEGER, FOREIGN KEY(run_id) REFERENCES meta(id));", 0, 0, 0);

    g_idle_add(scanner_update_current_file, g_strdup("Counting files..."));
    ctx->total_files = scanner_count_files(ctx->scan_path, ctx->scan_path);

    // This thread walks the tree; the pool's workers stat, hash and record each file
    ctx->pool = g_thread_pool_new(scanner_worker_func, ctx, ctx->num_workers, TRUE, NULL);
    scanner_scan_directory(ctx, ctx->scan_path);
    g_thread_pool_free(ctx->pool, FALSE, TRUE);  // waits for queued files to finish
    ctx->pool = NULL;

    scanner_find_missing(ctx);

    // Always create meta record for all scans (both update and read-only)
    char timestamp[64], hostname[256];
    get_timestamp(timestamp, sizeof(timestamp));
    gethostname(hostname, sizeof(hostname));

    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(ctx->db, "INSERT INTO meta (last_checksum_verify_date, last_date_verify, verify_machine, num_unchanged, num_changed, num_new, num_missing, num_ignored, num_errors, update_mode, note) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)", -1, &stmt, 0);
    sqlite3_bind_text(stmt, 1, ctx->enable_checksum ? timestamp : NULL, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 2, timestamp, -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 3, hostname, -1, SQLITE_STATIC);
    sqlite3_bind_int(stmt, 4, ctx->unchanged);
    sqlite3_bind_int(stmt, 5, ctx->changed);
    sqlite3_bind_int(stmt, 6, ctx->new_files);
    sqlite3_bind_int(stmt, 7, ctx->missing);
    sqlite3_bind_int(stmt, 8, ctx->ignored);
    sqlite3_bind_int(stmt, 9, ctx->errors);
    sqlite3_bind_text(stmt, 10, ctx->update_mode ? "ON" : "OFF", -1, SQLITE_STATIC);
    sqlite3_bind_text(stmt, 11, ctx->note, -1, SQLITE_STATIC);
    sqlite3_step(stmt);
    sqlite3_finalize(stmt);

    ctx->run_id = sqlite3_last_insert_rowid(ctx->db);

    if (ctx->log_count > 0) {
        sqlite3_stmt *log_stmt;
        sqlite3_prepare_v2(ctx->db, "INSERT INTO run_logs (run_id, status, full_path, checksum, size, mtime) VALUES (?, ?, ?, ?, ?, ?)", -1, &log_stmt, 0);
        for (int i = 0; i < ctx->log_count; i++) {
            sqlite3_bind_int64(log_stmt, 1, ctx->run_id);
            sqlite3_bind_text(log_stmt, 2, ctx->log_buffer[i].status, -1, SQLITE_STATIC);
            sqlite3_bind_text(log_stmt, 3, ctx->log_buffer[i].path, -1, SQLITE_STATIC);
            sqlite3_bind_text(log_stmt, 4, ctx->log_buffer[i].checksum, -1, SQLITE_STATIC);
            sqlite3_bind_int64(log_stmt, 5, ctx->log_buffer[i].size);
            sqlite3_bind_int64(log_stmt, 6, ctx->log_buffer[i].mtime);
            sqlite3_step(log_stmt);
            sqlite3_reset(log_stmt);
        }
        sqlite3_finalize(log_stmt);
        free(ctx->log_buffer);
        ctx->log_buffer = NULL;
    }

    // Auto-add or update drive in drive tracker (only in update mode)
    if (ctx->update_mode) {
        if (!auto_add_or_update_drive(ctx->db_path, ctx->scan_path, ctx->drive_error, sizeof(ctx->drive_error))) {
            g_printerr("file_tracker_unified: %s\n", ctx->drive_error);
        }
    }

    g_idle_add(scanner_scan_completed, ctx);
    return NULL;
}

gboolean scanner_scan_completed(gpointer data) {
    ScannerContext *ctx = (ScannerContext *)data;

    gtk_widget_set_sensitive(scanner_start_button, TRUE);
    gtk_widget_set_sensitive(scanner_stop_button, FALSE);
    gtk_widget_set_sensitive(scanner_path_entry, TRUE);
    gtk_widget_set_sensitive(scanner_db_entry, TRUE);
    gtk_widget_set_sensitive(scanner_workers_spin, TRUE);
    gtk_progress_bar_set_fraction(scanner_progress_bar, 1.0);

    char results[3072], n[7][32];
    int len = snprintf(results, sizeof(results),
             "Scan Complete!\n\nPath: %s\nDatabase: %s\nMode: %s\nChecksum: %s\nWorkers: %d\n\n"
             "Unchanged: %s\nChanged: %s\nNew: %s\nMissing: %s\nIgnored: %s\nErrors: %s\n\n"
             "Total: %s files",
             ctx->scan_path, ctx->db_name,
             ctx->update_mode ? "Update" : "Read-only",
             ctx->enable_checksum ? "Enabled" : "Disabled",
             ctx->num_workers,
             format_count(ctx->unchanged, n[0], sizeof(n[0])), format_count(ctx->changed, n[1], sizeof(n[1])),
             format_count(ctx->new_files, n[2], sizeof(n[2])), format_count(ctx->missing, n[3], sizeof(n[3])),
             format_count(ctx->ignored, n[4], sizeof(n[4])), format_count(ctx->errors, n[5], sizeof(n[5])),
             format_count((long long)ctx->unchanged + ctx->changed + ctx->new_files + ctx->missing + ctx->errors,
                          n[6], sizeof(n[6])));
    if (ctx->open_error[0] && len >= 0 && (size_t)len < sizeof(results)) {
        len += snprintf(results + len, sizeof(results) - len,
                 "\n\nError: could not read %s (%s). No files were scanned.%s",
                 ctx->scan_path, ctx->open_error,
                 strncmp(ctx->scan_path, "/Volumes/", 9) == 0
                     ? "\nmacOS may have denied access: allow File Tracker Unified under System Settings > "
                       "Privacy & Security > Files and Folders > Removable Volumes, then scan again."
                     : "");
    }
    if (ctx->drive_error[0] && len >= 0 && (size_t)len < sizeof(results)) {
        snprintf(results + len, sizeof(results) - len,
                 "\n\nWarning: the scan results were saved, but the drive's entry in the Drives tab "
                 "was not updated (%s). Click \"Update Mounted Drives\" on the Drives tab to retry.",
                 ctx->drive_error);
    }

    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(scanner_results_text));
    gtk_text_buffer_set_text(buffer, results, -1);

    if (ctx->db) sqlite3_close(ctx->db);
    return G_SOURCE_REMOVE;
}

void scanner_refresh_volumes() {
    GtkListBox *list = GTK_LIST_BOX(scanner_volumes_list);
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(GTK_WIDGET(list))) != NULL) {
        gtk_list_box_remove(list, child);
    }

    DIR *dir = opendir("/Volumes");
    if (!dir) return;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        if (is_excluded_volume(entry->d_name)) continue;

        char full_path[MAX_PATH];
        snprintf(full_path, sizeof(full_path), "/Volumes/%s", entry->d_name);

        struct stat sb;
        if (stat(full_path, &sb) == 0 && S_ISDIR(sb.st_mode)) {
            GtkWidget *row = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
            gtk_widget_set_margin_start(row, 8);
            gtk_widget_set_margin_end(row, 8);
            gtk_widget_set_margin_top(row, 6);
            gtk_widget_set_margin_bottom(row, 6);

            GtkWidget *name_label = gtk_label_new(entry->d_name);
            gtk_label_set_xalign(GTK_LABEL(name_label), 0.0);
            gtk_widget_add_css_class(name_label, "heading");
            gtk_box_append(GTK_BOX(row), name_label);

            struct statfs fs_stats;
            if (statfs(full_path, &fs_stats) == 0) {
                long long cap = (long long)fs_stats.f_blocks * fs_stats.f_bsize;
                long long avail = (long long)fs_stats.f_bavail * fs_stats.f_bsize;
                char info[128];
                snprintf(info, sizeof(info), "%.1f GB total, %.1f GB free",
                        cap / (1024.0 * 1024.0 * 1024.0), avail / (1024.0 * 1024.0 * 1024.0));
                GtkWidget *info_label = gtk_label_new(info);
                gtk_label_set_xalign(GTK_LABEL(info_label), 0.0);
                gtk_widget_add_css_class(info_label, "dim-label");
                gtk_box_append(GTK_BOX(row), info_label);
            }

            g_object_set_data_full(G_OBJECT(row), "volume_path", g_strdup(full_path), g_free);
            gtk_list_box_append(list, row);
        }
    }
    closedir(dir);
}

void on_scanner_volume_selected(GtkListBox *box, GtkListBoxRow *row, gpointer user_data) {
    (void)box; (void)user_data;
    if (!row) return;

    GtkWidget *row_widget = gtk_list_box_row_get_child(GTK_LIST_BOX_ROW(row));
    const char *volume_path = g_object_get_data(G_OBJECT(row_widget), "volume_path");

    if (volume_path) {
        gtk_editable_set_text(GTK_EDITABLE(scanner_path_entry), volume_path);
        char *vol_name = strrchr(volume_path, '/');
        if (vol_name) gtk_editable_set_text(GTK_EDITABLE(scanner_db_entry), vol_name + 1);
    }
}

void on_scanner_start_clicked(GtkButton *button, gpointer user_data) {
    (void)button; (void)user_data;

    const char *path = gtk_editable_get_text(GTK_EDITABLE(scanner_path_entry));
    const char *db_name = gtk_editable_get_text(GTK_EDITABLE(scanner_db_entry));

    if (strlen(path) == 0) {
        GtkAlertDialog *alert = gtk_alert_dialog_new("Please select a directory to scan");
        gtk_alert_dialog_show(alert, GTK_WINDOW(window));
        g_object_unref(alert);
        return;
    }

    current_scanner_scan = g_new0(ScannerContext, 1);
    strncpy(current_scanner_scan->scan_path, path, sizeof(current_scanner_scan->scan_path) - 1);

    if (strlen(db_name) > 0) {
        strncpy(current_scanner_scan->db_name, db_name, sizeof(current_scanner_scan->db_name) - 1);
    } else {
        char *base = strrchr(path, '/');
        strncpy(current_scanner_scan->db_name, base ? base + 1 : path, sizeof(current_scanner_scan->db_name) - 1);
    }

    snprintf(current_scanner_scan->db_path, sizeof(current_scanner_scan->db_path),
             "%s/%s.db", db_dir_path, current_scanner_scan->db_name);

    current_scanner_scan->enable_checksum = gtk_check_button_get_active(GTK_CHECK_BUTTON(scanner_checksum_check));
    current_scanner_scan->update_mode = gtk_check_button_get_active(GTK_CHECK_BUTTON(scanner_update_check));
    current_scanner_scan->num_workers = gtk_spin_button_get_value_as_int(GTK_SPIN_BUTTON(scanner_workers_spin));
    g_mutex_init(&current_scanner_scan->lock);

    GtkTextBuffer *note_buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(scanner_note_text));
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(note_buffer, &start, &end);
    char *note = gtk_text_buffer_get_text(note_buffer, &start, &end, FALSE);
    strncpy(current_scanner_scan->note, note, sizeof(current_scanner_scan->note) - 1);
    g_free(note);

    gtk_widget_set_sensitive(scanner_start_button, FALSE);
    gtk_widget_set_sensitive(scanner_stop_button, TRUE);
    gtk_widget_set_sensitive(scanner_path_entry, FALSE);
    gtk_widget_set_sensitive(scanner_db_entry, FALSE);
    gtk_widget_set_sensitive(scanner_workers_spin, FALSE);
    gtk_progress_bar_set_fraction(scanner_progress_bar, 0.0);
    gtk_label_set_text(GTK_LABEL(scanner_status_label), "Initializing...");

    // Clear results from any previous run
    gtk_label_set_text(GTK_LABEL(scanner_current_file_label), "");
    gtk_text_buffer_set_text(gtk_text_view_get_buffer(GTK_TEXT_VIEW(scanner_results_text)), "", -1);

    current_scanner_scan->scan_thread = g_thread_new("scanner", scanner_thread_func, current_scanner_scan);
}

void on_scanner_stop_clicked(GtkButton *button, gpointer user_data) {
    (void)button; (void)user_data;
    if (current_scanner_scan) {
        current_scanner_scan->should_stop = 1;
        gtk_label_set_text(GTK_LABEL(scanner_status_label), "Stopping...");
    }
}

GtkWidget *create_scanner_tab() {
    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);

    // Left: Volumes
    GtkWidget *left_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_size_request(left_box, 300, -1);
    gtk_widget_set_margin_start(left_box, 8);
    gtk_widget_set_margin_end(left_box, 8);
    gtk_widget_set_margin_top(left_box, 8);
    gtk_widget_set_margin_bottom(left_box, 8);

    GtkWidget *vol_header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *vol_label = gtk_label_new("Volumes");
    gtk_widget_add_css_class(vol_label, "heading");
    gtk_label_set_xalign(GTK_LABEL(vol_label), 0.0);
    gtk_widget_set_hexpand(vol_label, TRUE);
    GtkWidget *refresh_btn = gtk_button_new_with_label("Refresh");
    g_signal_connect(refresh_btn, "clicked", G_CALLBACK((GCallback)scanner_refresh_volumes), NULL);
    gtk_box_append(GTK_BOX(vol_header), vol_label);
    gtk_box_append(GTK_BOX(vol_header), refresh_btn);
    gtk_box_append(GTK_BOX(left_box), vol_header);

    scanner_volumes_list = gtk_list_box_new();
    g_signal_connect(scanner_volumes_list, "row-activated", G_CALLBACK(on_scanner_volume_selected), NULL);
    GtkWidget *vol_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(vol_scroll), scanner_volumes_list);
    gtk_widget_set_vexpand(vol_scroll, TRUE);
    gtk_box_append(GTK_BOX(left_box), vol_scroll);

    gtk_paned_set_start_child(GTK_PANED(paned), left_box);

    // Right: Controls
    GtkWidget *right_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start(right_box, 16);
    gtk_widget_set_margin_end(right_box, 16);
    gtk_widget_set_margin_top(right_box, 16);
    gtk_widget_set_margin_bottom(right_box, 16);

    // Path
    GtkWidget *path_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *path_label = gtk_label_new("Path:");
    gtk_widget_set_size_request(path_label, 80, -1);
    scanner_path_entry = gtk_entry_new();
    gtk_widget_set_hexpand(scanner_path_entry, TRUE);
    gtk_box_append(GTK_BOX(path_box), path_label);
    gtk_box_append(GTK_BOX(path_box), scanner_path_entry);
    gtk_box_append(GTK_BOX(right_box), path_box);

    // Database
    GtkWidget *db_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *db_label = gtk_label_new("Database:");
    gtk_widget_set_size_request(db_label, 80, -1);
    scanner_db_entry = gtk_entry_new();
    gtk_widget_set_hexpand(scanner_db_entry, TRUE);
    gtk_box_append(GTK_BOX(db_box), db_label);
    gtk_box_append(GTK_BOX(db_box), scanner_db_entry);
    gtk_box_append(GTK_BOX(right_box), db_box);

    // Options
    GtkWidget *opts_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 16);
    scanner_checksum_check = gtk_check_button_new_with_label("Verify Checksums");
    scanner_update_check = gtk_check_button_new_with_label("Update Database");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(scanner_update_check), TRUE);
    gtk_box_append(GTK_BOX(opts_box), scanner_checksum_check);
    gtk_box_append(GTK_BOX(opts_box), scanner_update_check);
    GtkWidget *workers_label = gtk_label_new("Workers:");
    scanner_workers_spin = gtk_spin_button_new_with_range(1, MAX(2 * (int)g_get_num_processors(), 2), 1);
    gtk_spin_button_set_value(GTK_SPIN_BUTTON(scanner_workers_spin), 1);
    gtk_widget_set_tooltip_text(scanner_workers_spin,
        "Number of files processed in parallel. Keep at 1 for spinning hard drives; "
        "higher values speed up checksum scans on SSD/NVMe drives.");
    GtkWidget *workers_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 6);
    gtk_box_append(GTK_BOX(workers_box), workers_label);
    gtk_box_append(GTK_BOX(workers_box), scanner_workers_spin);
    gtk_box_append(GTK_BOX(opts_box), workers_box);
    gtk_box_append(GTK_BOX(right_box), opts_box);

    // Note
    GtkWidget *note_label = gtk_label_new("Note:");
    gtk_label_set_xalign(GTK_LABEL(note_label), 0.0);
    scanner_note_text = gtk_text_view_new();
    gtk_widget_set_size_request(scanner_note_text, -1, 50);
    GtkWidget *note_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(note_scroll), scanner_note_text);
    gtk_box_append(GTK_BOX(right_box), note_label);
    gtk_box_append(GTK_BOX(right_box), note_scroll);

    // Buttons
    GtkWidget *btn_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(btn_box, GTK_ALIGN_CENTER);
    scanner_start_button = gtk_button_new_with_label("Start Scan");
    gtk_widget_add_css_class(scanner_start_button, "suggested-action");
    gtk_widget_set_size_request(scanner_start_button, 120, -1);
    g_signal_connect(scanner_start_button, "clicked", G_CALLBACK(on_scanner_start_clicked), NULL);
    scanner_stop_button = gtk_button_new_with_label("Stop");
    gtk_widget_add_css_class(scanner_stop_button, "destructive-action");
    gtk_widget_set_size_request(scanner_stop_button, 120, -1);
    gtk_widget_set_sensitive(scanner_stop_button, FALSE);
    g_signal_connect(scanner_stop_button, "clicked", G_CALLBACK(on_scanner_stop_clicked), NULL);
    gtk_box_append(GTK_BOX(btn_box), scanner_start_button);
    gtk_box_append(GTK_BOX(btn_box), scanner_stop_button);
    gtk_box_append(GTK_BOX(right_box), btn_box);

    gtk_box_append(GTK_BOX(right_box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    // Progress
    scanner_progress_bar = GTK_PROGRESS_BAR(gtk_progress_bar_new());
    gtk_widget_set_size_request(GTK_WIDGET(scanner_progress_bar), -1, 24);
    gtk_box_append(GTK_BOX(right_box), GTK_WIDGET(scanner_progress_bar));

    // Status
    scanner_status_label = gtk_label_new("Ready");
    gtk_label_set_xalign(GTK_LABEL(scanner_status_label), 0.0);
    gtk_box_append(GTK_BOX(right_box), scanner_status_label);

    // Current file
    scanner_current_file_label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(scanner_current_file_label), 0.0);
    gtk_widget_add_css_class(scanner_current_file_label, "dim-label");
    gtk_label_set_ellipsize(GTK_LABEL(scanner_current_file_label), PANGO_ELLIPSIZE_START);
    gtk_box_append(GTK_BOX(right_box), scanner_current_file_label);

    // Results
    GtkWidget *results_label = gtk_label_new("Results:");
    gtk_label_set_xalign(GTK_LABEL(results_label), 0.0);
    scanner_results_text = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(scanner_results_text), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(scanner_results_text), TRUE);
    GtkWidget *results_scroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(results_scroll, TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(results_scroll), scanner_results_text);
    gtk_box_append(GTK_BOX(right_box), results_label);
    gtk_box_append(GTK_BOX(right_box), results_scroll);

    gtk_paned_set_end_child(GTK_PANED(paned), right_box);
    gtk_paned_set_position(GTK_PANED(paned), 320);

    return paned;
}

// ============================================================================
// TAB 6: COMPARE RUNS
// ============================================================================

GtkWidget *compare_run1_drive_combo;
GtkWidget *compare_run1_run_combo;
GtkWidget *compare_run2_drive_combo;
GtkWidget *compare_run2_run_combo;
GtkWidget *compare_results_tree;
GtkWidget *compare_filter_only_run1;
GtkWidget *compare_filter_only_run2;
GtkWidget *compare_filter_different;
GtkWidget *compare_filter_missing_run1;
GtkWidget *compare_filter_missing_run2;
GtkWidget *compare_filter_all_run1;
GtkWidget *compare_filter_all_run2;
GtkWidget *compare_filter_all_diffs;
GtkWidget *compare_filter_mtime_diff;
GtkWidget *compare_filter_size_diff;

char compare_run1_db_path[MAX_PATH] = "";
char compare_run2_db_path[MAX_PATH] = "";
sqlite3_int64 compare_run1_id = 0;
sqlite3_int64 compare_run2_id = 0;

void compare_load_runs_for_drive(GtkComboBoxText *combo, const char *db_name) {
    gtk_combo_box_text_remove_all(combo);

    if (!db_name || strlen(db_name) == 0) return;

    char db_path[MAX_PATH];
    snprintf(db_path, sizeof(db_path), "%s/%s.db", db_dir_path, db_name);

    sqlite3 *db;
    if (sqlite3_open(db_path, &db) != SQLITE_OK) return;

    const char *sql = "SELECT id, last_date_verify FROM meta ORDER BY id DESC;";
    sqlite3_stmt *stmt;

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            int id = sqlite3_column_int(stmt, 0);
            const char *date = (const char *)sqlite3_column_text(stmt, 1);
            char label[256];
            snprintf(label, sizeof(label), "Run #%d - %s", id, date ? date : "Unknown");

            char id_str[32];
            snprintf(id_str, sizeof(id_str), "%d", id);
            gtk_combo_box_text_append(combo, id_str, label);
        }
        sqlite3_finalize(stmt);
    }
    sqlite3_close(db);
}

void on_compare_run1_drive_changed(GtkComboBox *combo, gpointer user_data) {
    (void)user_data;
    const char *db_name = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(combo));
    if (db_name) {
        compare_load_runs_for_drive(GTK_COMBO_BOX_TEXT(compare_run1_run_combo), db_name);
        snprintf(compare_run1_db_path, sizeof(compare_run1_db_path), "%s/%s.db", db_dir_path, db_name);
        g_free((void *)db_name);
    }
}

void on_compare_run2_drive_changed(GtkComboBox *combo, gpointer user_data) {
    (void)user_data;
    const char *db_name = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(combo));
    if (db_name) {
        compare_load_runs_for_drive(GTK_COMBO_BOX_TEXT(compare_run2_run_combo), db_name);
        snprintf(compare_run2_db_path, sizeof(compare_run2_db_path), "%s/%s.db", db_dir_path, db_name);
        g_free((void *)db_name);
    }
}

void on_compare_run1_run_changed(GtkComboBox *combo, gpointer user_data) {
    (void)user_data;
    const char *id_str = gtk_combo_box_get_active_id(GTK_COMBO_BOX(combo));
    compare_run1_id = id_str ? atoll(id_str) : 0;
}

void on_compare_run2_run_changed(GtkComboBox *combo, gpointer user_data) {
    (void)user_data;
    const char *id_str = gtk_combo_box_get_active_id(GTK_COMBO_BOX(combo));
    compare_run2_id = id_str ? atoll(id_str) : 0;
}

typedef struct {
    char path_run1[MAX_PATH];
    char path_run2[MAX_PATH];
    char status_run1[32];
    char status_run2[32];
    char checksum_run1[HASH_SIZE];
    char checksum_run2[HASH_SIZE];
    long long size_run1;
    long long size_run2;
    long long mtime_run1;
    long long mtime_run2;
} CompareResult;

// Strip mount point (first two path components) for comparison
// e.g., "/Volumes/MediaArch-C2/folder/file" -> "/folder/file"
const char* get_relative_path(const char *full_path) {
    if (!full_path || full_path[0] != '/') return full_path;

    const char *p = full_path + 1; // Skip first '/'
    int slashes = 0;

    while (*p && slashes < 2) {
        if (*p == '/') slashes++;
        p++;
    }

    // If we found 2 slashes, return pointer after them
    // Otherwise return original path
    if (slashes == 2 && *p) {
        return p - 1; // Return including the slash
    }
    return full_path;
}

void compare_perform_comparison() {
    if (compare_run1_id == 0 || compare_run2_id == 0) {
        GtkAlertDialog *alert = gtk_alert_dialog_new("Please select both runs to compare");
        gtk_alert_dialog_show(alert, GTK_WINDOW(window));
        g_object_unref(alert);
        return;
    }

    // Clear existing results
    GtkListStore *store = GTK_LIST_STORE(gtk_tree_view_get_model(GTK_TREE_VIEW(compare_results_tree)));
    gtk_list_store_clear(store);

    // Open both databases
    sqlite3 *db1, *db2;
    if (sqlite3_open(compare_run1_db_path, &db1) != SQLITE_OK) return;
    if (sqlite3_open(compare_run2_db_path, &db2) != SQLITE_OK) {
        sqlite3_close(db1);
        return;
    }

    // Get files from run 1
    GHashTable *files_run1 = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);
    GHashTable *files_run2 = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, g_free);

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db1, "SELECT full_path, checksum, status, size, mtime FROM run_logs "
                                "WHERE run_id = ?",
                          -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, compare_run1_id);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *path = (const char *)sqlite3_column_text(stmt, 0);
            const char *checksum = (const char *)sqlite3_column_text(stmt, 1);
            const char *status = (const char *)sqlite3_column_text(stmt, 2);
            long long size = sqlite3_column_int64(stmt, 3);
            long long mtime = sqlite3_column_int64(stmt, 4);

            // Use relative path (without mount point) as comparison key
            const char *rel_path = get_relative_path(path);

            CompareResult *result = g_new0(CompareResult, 1);
            strncpy(result->path_run1, path, MAX_PATH - 1);  // Store full path for display
            result->path_run1[MAX_PATH - 1] = '\0';
            strcpy(result->path_run2, "");  // Not in run 2 yet
            strncpy(result->status_run1, status ? status : "UNKNOWN", 31);
            strncpy(result->checksum_run1, checksum ? checksum : "", HASH_SIZE - 1);
            strcpy(result->status_run2, "NOT_IN_RUN");
            strcpy(result->checksum_run2, "");
            result->size_run1 = size;
            result->mtime_run1 = mtime;
            result->size_run2 = 0;
            result->mtime_run2 = 0;

            g_hash_table_insert(files_run1, g_strdup(rel_path), result);
        }
        sqlite3_finalize(stmt);
    }

    // Get files from run 2 and update comparison
    if (sqlite3_prepare_v2(db2, "SELECT full_path, checksum, status, size, mtime FROM run_logs "
                                "WHERE run_id = ?",
                          -1, &stmt, NULL) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, compare_run2_id);
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *path = (const char *)sqlite3_column_text(stmt, 0);
            const char *checksum = (const char *)sqlite3_column_text(stmt, 1);
            const char *status = (const char *)sqlite3_column_text(stmt, 2);
            long long size = sqlite3_column_int64(stmt, 3);
            long long mtime = sqlite3_column_int64(stmt, 4);

            // Use relative path (without mount point) as comparison key
            const char *rel_path = get_relative_path(path);

            CompareResult *result = g_hash_table_lookup(files_run1, rel_path);
            if (result) {
                // File exists in both runs - update run 2 info
                strncpy(result->path_run2, path, MAX_PATH - 1);  // Store full path from run 2
                result->path_run2[MAX_PATH - 1] = '\0';
                strncpy(result->status_run2, status ? status : "UNKNOWN", 31);
                strncpy(result->checksum_run2, checksum ? checksum : "", HASH_SIZE - 1);
                result->size_run2 = size;
                result->mtime_run2 = mtime;
            } else {
                // File only in run 2
                result = g_new0(CompareResult, 1);
                strcpy(result->path_run1, "");  // Not in run 1
                strncpy(result->path_run2, path, MAX_PATH - 1);  // Store full path for display
                result->path_run2[MAX_PATH - 1] = '\0';
                strcpy(result->status_run1, "NOT_IN_RUN");
                strcpy(result->checksum_run1, "");
                strncpy(result->status_run2, status ? status : "UNKNOWN", 31);
                strncpy(result->checksum_run2, checksum ? checksum : "", HASH_SIZE - 1);
                result->size_run1 = 0;
                result->mtime_run1 = 0;
                result->size_run2 = size;
                result->mtime_run2 = mtime;
                g_hash_table_insert(files_run2, g_strdup(rel_path), result);
            }
        }
        sqlite3_finalize(stmt);
    }

    sqlite3_close(db1);
    sqlite3_close(db2);

    // Apply filters and populate tree view
    GHashTableIter iter;
    gpointer key, value;

    g_hash_table_iter_init(&iter, files_run1);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        CompareResult *result = (CompareResult *)value;

        int show = 0;
        if (gtk_check_button_get_active(GTK_CHECK_BUTTON(compare_filter_only_run1)) &&
            strcmp(result->status_run2, "NOT_IN_RUN") == 0) show = 1;
        if (gtk_check_button_get_active(GTK_CHECK_BUTTON(compare_filter_only_run2)) &&
            strcmp(result->status_run1, "NOT_IN_RUN") == 0) show = 1;
        if (gtk_check_button_get_active(GTK_CHECK_BUTTON(compare_filter_different)) &&
            strcmp(result->status_run1, "NOT_IN_RUN") != 0 &&
            strcmp(result->status_run2, "NOT_IN_RUN") != 0 &&
            strcmp(result->checksum_run1, result->checksum_run2) != 0) show = 1;
        if (gtk_check_button_get_active(GTK_CHECK_BUTTON(compare_filter_all_diffs)) &&
            (strcmp(result->status_run2, "NOT_IN_RUN") == 0 ||
             strcmp(result->status_run1, "NOT_IN_RUN") == 0 ||
             (strcmp(result->status_run1, "NOT_IN_RUN") != 0 &&
              strcmp(result->status_run2, "NOT_IN_RUN") != 0 &&
              (strcmp(result->checksum_run1, result->checksum_run2) != 0 ||
               result->mtime_run1 != result->mtime_run2 ||
               result->size_run1 != result->size_run2)))) show = 1;
        if (gtk_check_button_get_active(GTK_CHECK_BUTTON(compare_filter_missing_run2)) &&
            strcmp(result->status_run2, "NOT_IN_RUN") == 0) show = 1;
        if (gtk_check_button_get_active(GTK_CHECK_BUTTON(compare_filter_missing_run1)) &&
            strcmp(result->status_run1, "NOT_IN_RUN") == 0) show = 1;
        if (gtk_check_button_get_active(GTK_CHECK_BUTTON(compare_filter_all_run1)) &&
            strcmp(result->status_run1, "NOT_IN_RUN") != 0) show = 1;
        if (gtk_check_button_get_active(GTK_CHECK_BUTTON(compare_filter_all_run2)) &&
            strcmp(result->status_run2, "NOT_IN_RUN") != 0) show = 1;
        if (gtk_check_button_get_active(GTK_CHECK_BUTTON(compare_filter_mtime_diff)) &&
            strcmp(result->status_run1, "NOT_IN_RUN") != 0 &&
            strcmp(result->status_run2, "NOT_IN_RUN") != 0 &&
            result->mtime_run1 != result->mtime_run2) show = 1;
        if (gtk_check_button_get_active(GTK_CHECK_BUTTON(compare_filter_size_diff)) &&
            strcmp(result->status_run1, "NOT_IN_RUN") != 0 &&
            strcmp(result->status_run2, "NOT_IN_RUN") != 0 &&
            result->size_run1 != result->size_run2) show = 1;

        if (show) {
            // Use relative path for display (common portion)
            const char *display_path = get_relative_path(
                strlen(result->path_run1) > 0 ? result->path_run1 : result->path_run2
            );

            GtkTreeIter tree_iter;
            gtk_list_store_append(store, &tree_iter);
            gtk_list_store_set(store, &tree_iter,
                              0, display_path,
                              1, result->status_run1,
                              2, result->status_run2,
                              3, result->checksum_run1,
                              4, result->checksum_run2,
                              -1);
        }
    }

    g_hash_table_iter_init(&iter, files_run2);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        CompareResult *result = (CompareResult *)value;

        int show = 0;
        if (gtk_check_button_get_active(GTK_CHECK_BUTTON(compare_filter_only_run2)) &&
            strcmp(result->status_run1, "NOT_IN_RUN") == 0) show = 1;
        if (gtk_check_button_get_active(GTK_CHECK_BUTTON(compare_filter_all_diffs)) &&
            strcmp(result->status_run1, "NOT_IN_RUN") == 0) show = 1;
        if (gtk_check_button_get_active(GTK_CHECK_BUTTON(compare_filter_missing_run1)) &&
            strcmp(result->status_run1, "NOT_IN_RUN") == 0) show = 1;
        if (gtk_check_button_get_active(GTK_CHECK_BUTTON(compare_filter_all_run2)) &&
            strcmp(result->status_run2, "NOT_IN_RUN") != 0) show = 1;

        if (show) {
            // Use relative path for display (common portion)
            const char *display_path = get_relative_path(
                strlen(result->path_run1) > 0 ? result->path_run1 : result->path_run2
            );

            GtkTreeIter tree_iter;
            gtk_list_store_append(store, &tree_iter);
            gtk_list_store_set(store, &tree_iter,
                              0, display_path,
                              1, result->status_run1,
                              2, result->status_run2,
                              3, result->checksum_run1,
                              4, result->checksum_run2,
                              -1);
        }
    }

    g_hash_table_destroy(files_run1);
    g_hash_table_destroy(files_run2);
}

void on_compare_filter_toggled(GtkCheckButton *button, gpointer user_data) {
    (void)button; (void)user_data;
    compare_perform_comparison();
}

void on_compare_export_csv_response(GObject *source, GAsyncResult *result, gpointer user_data);

void on_compare_export_csv_clicked(GtkButton *button, gpointer user_data) {
    (void)button; (void)user_data;

    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Export Comparison to CSV");
    gtk_file_dialog_set_initial_name(dialog, "comparison.csv");

    gtk_file_dialog_save(dialog, GTK_WINDOW(window), NULL,
                        (GAsyncReadyCallback)on_compare_export_csv_response, NULL);
}

void on_compare_export_csv_response(GObject *source, GAsyncResult *result, gpointer user_data) {
    (void)user_data;

    GtkFileDialog *dialog = GTK_FILE_DIALOG(source);
    GFile *file = gtk_file_dialog_save_finish(dialog, result, NULL);

    if (!file) return;

    char *path = g_file_get_path(file);
    g_object_unref(file);

    FILE *fp = fopen(path, "w");
    if (!fp) {
        GtkAlertDialog *alert = gtk_alert_dialog_new("Failed to create CSV file");
        gtk_alert_dialog_show(alert, GTK_WINDOW(window));
        g_object_unref(alert);
        g_free(path);
        return;
    }

    // Write CSV header
    fprintf(fp, "File Path,Status Run 1,Status Run 2,Checksum Run 1,Checksum Run 2\n");

    // Write data
    GtkTreeModel *model = gtk_tree_view_get_model(GTK_TREE_VIEW(compare_results_tree));
    GtkTreeIter iter;
    gboolean valid = gtk_tree_model_get_iter_first(model, &iter);

    while (valid) {
        char *path_str, *status1, *status2, *check1, *check2;
        gtk_tree_model_get(model, &iter,
                          0, &path_str,
                          1, &status1,
                          2, &status2,
                          3, &check1,
                          4, &check2,
                          -1);

        fprintf(fp, "\"%s\",\"%s\",\"%s\",\"%s\",\"%s\"\n",
                path_str, status1, status2, check1, check2);

        g_free(path_str);
        g_free(status1);
        g_free(status2);
        g_free(check1);
        g_free(check2);

        valid = gtk_tree_model_iter_next(model, &iter);
    }

    fclose(fp);
    g_free(path);

    GtkAlertDialog *alert = gtk_alert_dialog_new("Comparison exported successfully");
    gtk_alert_dialog_show(alert, GTK_WINDOW(window));
    g_object_unref(alert);
}

GtkWidget *create_compare_tab() {
    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);

    // Left panel: Selection and filters
    GtkWidget *left_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_size_request(left_box, 350, -1);
    gtk_widget_set_margin_start(left_box, 8);
    gtk_widget_set_margin_end(left_box, 8);
    gtk_widget_set_margin_top(left_box, 8);
    gtk_widget_set_margin_bottom(left_box, 8);

    // Run 1 selection
    GtkWidget *run1_header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *run1_label = gtk_label_new("Run 1");
    gtk_widget_add_css_class(run1_label, "heading");
    gtk_label_set_xalign(GTK_LABEL(run1_label), 0.0);
    gtk_widget_set_hexpand(run1_label, TRUE);
    GtkWidget *refresh_btn = gtk_button_new_with_label("Refresh");
    gtk_widget_set_tooltip_text(refresh_btn, "Reload the drive and run lists");
    g_signal_connect(refresh_btn, "clicked", G_CALLBACK((GCallback)compare_refresh_databases), NULL);
    gtk_box_append(GTK_BOX(run1_header), run1_label);
    gtk_box_append(GTK_BOX(run1_header), refresh_btn);
    gtk_box_append(GTK_BOX(left_box), run1_header);

    GtkWidget *run1_drive_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *run1_drive_label = gtk_label_new("Drive:");
    gtk_widget_set_size_request(run1_drive_label, 60, -1);
    compare_run1_drive_combo = gtk_combo_box_text_new();
    gtk_widget_set_hexpand(compare_run1_drive_combo, TRUE);
    g_signal_connect(compare_run1_drive_combo, "changed", G_CALLBACK(on_compare_run1_drive_changed), NULL);
    gtk_box_append(GTK_BOX(run1_drive_box), run1_drive_label);
    gtk_box_append(GTK_BOX(run1_drive_box), compare_run1_drive_combo);
    gtk_box_append(GTK_BOX(left_box), run1_drive_box);

    GtkWidget *run1_run_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *run1_run_label = gtk_label_new("Run:");
    gtk_widget_set_size_request(run1_run_label, 60, -1);
    compare_run1_run_combo = gtk_combo_box_text_new();
    gtk_widget_set_hexpand(compare_run1_run_combo, TRUE);
    g_signal_connect(compare_run1_run_combo, "changed", G_CALLBACK(on_compare_run1_run_changed), NULL);
    gtk_box_append(GTK_BOX(run1_run_box), run1_run_label);
    gtk_box_append(GTK_BOX(run1_run_box), compare_run1_run_combo);
    gtk_box_append(GTK_BOX(left_box), run1_run_box);

    // Run 2 selection
    GtkWidget *run2_label = gtk_label_new("Run 2");
    gtk_widget_add_css_class(run2_label, "heading");
    gtk_label_set_xalign(GTK_LABEL(run2_label), 0.0);
    gtk_box_append(GTK_BOX(left_box), run2_label);

    GtkWidget *run2_drive_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *run2_drive_label = gtk_label_new("Drive:");
    gtk_widget_set_size_request(run2_drive_label, 60, -1);
    compare_run2_drive_combo = gtk_combo_box_text_new();
    gtk_widget_set_hexpand(compare_run2_drive_combo, TRUE);
    g_signal_connect(compare_run2_drive_combo, "changed", G_CALLBACK(on_compare_run2_drive_changed), NULL);
    gtk_box_append(GTK_BOX(run2_drive_box), run2_drive_label);
    gtk_box_append(GTK_BOX(run2_drive_box), compare_run2_drive_combo);
    gtk_box_append(GTK_BOX(left_box), run2_drive_box);

    GtkWidget *run2_run_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *run2_run_label = gtk_label_new("Run:");
    gtk_widget_set_size_request(run2_run_label, 60, -1);
    compare_run2_run_combo = gtk_combo_box_text_new();
    gtk_widget_set_hexpand(compare_run2_run_combo, TRUE);
    g_signal_connect(compare_run2_run_combo, "changed", G_CALLBACK(on_compare_run2_run_changed), NULL);
    gtk_box_append(GTK_BOX(run2_run_box), run2_run_label);
    gtk_box_append(GTK_BOX(run2_run_box), compare_run2_run_combo);
    gtk_box_append(GTK_BOX(left_box), run2_run_box);

    // Compare button
    GtkWidget *compare_btn = gtk_button_new_with_label("Compare Runs");
    gtk_widget_add_css_class(compare_btn, "suggested-action");
    g_signal_connect(compare_btn, "clicked", G_CALLBACK((GCallback)compare_perform_comparison), NULL);
    gtk_box_append(GTK_BOX(left_box), compare_btn);

    // Filters
    GtkWidget *filter_label = gtk_label_new("View By:");
    gtk_widget_add_css_class(filter_label, "heading");
    gtk_label_set_xalign(GTK_LABEL(filter_label), 0.0);
    gtk_box_append(GTK_BOX(left_box), filter_label);

    compare_filter_only_run1 = gtk_check_button_new_with_label("Only in Run 1");
    g_signal_connect(compare_filter_only_run1, "toggled", G_CALLBACK(on_compare_filter_toggled), NULL);
    gtk_box_append(GTK_BOX(left_box), compare_filter_only_run1);

    compare_filter_only_run2 = gtk_check_button_new_with_label("Only in Run 2");
    g_signal_connect(compare_filter_only_run2, "toggled", G_CALLBACK(on_compare_filter_toggled), NULL);
    gtk_box_append(GTK_BOX(left_box), compare_filter_only_run2);

    compare_filter_different = gtk_check_button_new_with_label("Different Checksum");
    g_signal_connect(compare_filter_different, "toggled", G_CALLBACK(on_compare_filter_toggled), NULL);
    gtk_box_append(GTK_BOX(left_box), compare_filter_different);

    compare_filter_all_diffs = gtk_check_button_new_with_label("All Diffs");
    g_signal_connect(compare_filter_all_diffs, "toggled", G_CALLBACK(on_compare_filter_toggled), NULL);
    gtk_box_append(GTK_BOX(left_box), compare_filter_all_diffs);

    compare_filter_missing_run2 = gtk_check_button_new_with_label("Missing in Run 2");
    g_signal_connect(compare_filter_missing_run2, "toggled", G_CALLBACK(on_compare_filter_toggled), NULL);
    gtk_box_append(GTK_BOX(left_box), compare_filter_missing_run2);

    compare_filter_missing_run1 = gtk_check_button_new_with_label("Missing in Run 1");
    g_signal_connect(compare_filter_missing_run1, "toggled", G_CALLBACK(on_compare_filter_toggled), NULL);
    gtk_box_append(GTK_BOX(left_box), compare_filter_missing_run1);

    compare_filter_all_run1 = gtk_check_button_new_with_label("All Files in Run 1");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(compare_filter_all_run1), TRUE);
    g_signal_connect(compare_filter_all_run1, "toggled", G_CALLBACK(on_compare_filter_toggled), NULL);
    gtk_box_append(GTK_BOX(left_box), compare_filter_all_run1);

    compare_filter_all_run2 = gtk_check_button_new_with_label("All Files in Run 2");
    g_signal_connect(compare_filter_all_run2, "toggled", G_CALLBACK(on_compare_filter_toggled), NULL);
    gtk_box_append(GTK_BOX(left_box), compare_filter_all_run2);

    compare_filter_mtime_diff = gtk_check_button_new_with_label("Date/Time Difference");
    g_signal_connect(compare_filter_mtime_diff, "toggled", G_CALLBACK(on_compare_filter_toggled), NULL);
    gtk_box_append(GTK_BOX(left_box), compare_filter_mtime_diff);

    compare_filter_size_diff = gtk_check_button_new_with_label("Size Difference");
    g_signal_connect(compare_filter_size_diff, "toggled", G_CALLBACK(on_compare_filter_toggled), NULL);
    gtk_box_append(GTK_BOX(left_box), compare_filter_size_diff);

    // Export button
    GtkWidget *export_btn = gtk_button_new_with_label("Export to CSV");
    g_signal_connect(export_btn, "clicked", G_CALLBACK(on_compare_export_csv_clicked), NULL);
    gtk_box_append(GTK_BOX(left_box), export_btn);

    gtk_paned_set_start_child(GTK_PANED(paned), left_box);

    // Right panel: Results table
    GtkWidget *right_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(right_box, 8);
    gtk_widget_set_margin_end(right_box, 8);
    gtk_widget_set_margin_top(right_box, 8);
    gtk_widget_set_margin_bottom(right_box, 8);

    GtkWidget *results_label = gtk_label_new("Comparison Results");
    gtk_widget_add_css_class(results_label, "heading");
    gtk_label_set_xalign(GTK_LABEL(results_label), 0.0);
    gtk_box_append(GTK_BOX(right_box), results_label);

    GtkListStore *store = gtk_list_store_new(5, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                                             G_TYPE_STRING, G_TYPE_STRING);
    compare_results_tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
    g_object_unref(store);

    const char *titles[] = {"File Path", "Status Run 1", "Status Run 2", "Checksum Run 1", "Checksum Run 2"};
    for (int i = 0; i < 5; i++) {
        GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
        GtkTreeViewColumn *column = gtk_tree_view_column_new_with_attributes(titles[i], renderer, "text", i, NULL);
        gtk_tree_view_column_set_resizable(column, TRUE);
        if (i == 0) gtk_tree_view_column_set_expand(column, TRUE);
        gtk_tree_view_append_column(GTK_TREE_VIEW(compare_results_tree), column);
    }

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), compare_results_tree);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_append(GTK_BOX(right_box), scroll);

    gtk_paned_set_end_child(GTK_PANED(paned), right_box);
    gtk_paned_set_position(GTK_PANED(paned), 370);

    return paned;
}

// ============================================================================
// TAB: DUPE FINDER
// ============================================================================

enum {
    DUPE_MODE_SEARCH = 0,          // Search by a given file name or checksum
    DUPE_MODE_SAME_NAME,           // Every file whose name appears more than once
    DUPE_MODE_SAME_NAME_CHECKSUM   // Every file whose name and checksum both appear more than once
};

GtkWidget *dupe_mode_combo;       // DUPE_MODE_*
GtkWidget *dupe_search_by_label;
GtkWidget *dupe_search_by_combo;  // 0 = File Name, 1 = Checksum
GtkWidget *dupe_search_entry;
GtkWidget *dupe_search_filler;    // Takes the entry's space when the entry is hidden
GtkWidget *dupe_db_combo;         // Single database or All (search mode)
GtkWidget *dupe_db_multi_button;  // Popover of database checkboxes (duplicate-group modes)
GtkWidget *dupe_db_all_check;
GtkWidget *dupe_db_check_box;     // Holds one check button per database
GtkWidget *dupe_results_tree;
GtkWidget *dupe_status_label;

// Builds a GLOB pattern from a user file name: '*' stays a wildcard, while GLOB's other
// metacharacters (? and [) are bracketed so they match literally.
static void dupe_name_to_glob(const char *name, char *out, size_t out_size) {
    size_t n = 0;
    for (const char *c = name; *c && n + 4 < out_size; c++) {
        if (*c == '?' || *c == '[') {
            out[n++] = '[';
            out[n++] = *c;
            out[n++] = ']';
        } else {
            out[n++] = *c;
        }
    }
    out[n] = '\0';
}

// Drive name is the database name without ".db"
static void dupe_drive_name(const char *db_file, char *out, size_t out_size) {
    snprintf(out, out_size, "%s", db_file);
    size_t len = strlen(out);
    if (len > 3 && strcmp(out + len - 3, ".db") == 0) out[len - 3] = '\0';
}

// File checksums are not stamped individually; report the drive's latest scan run
// with checksum verification enabled. schema is "main" or an attached database name.
static void dupe_latest_checksum_date(sqlite3 *db, const char *schema, char *out, size_t out_size) {
    snprintf(out, out_size, "Never");
    char sql[256];
    snprintf(sql, sizeof(sql), "SELECT MAX(last_checksum_verify_date) FROM %s.meta "
                               "WHERE last_checksum_verify_date IS NOT NULL;", schema);
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *val = (const char *)sqlite3_column_text(stmt, 0);
            if (val && *val) snprintf(out, out_size, "%s", val);
        }
        sqlite3_finalize(stmt);
    }
}

// Adds every present file in one database that matches the name/checksum. Returns rows added.
static int dupe_search_database(const char *db_file, const char *db_path, const char *value,
                                int by_checksum, GtkListStore *store) {
    sqlite3 *db = NULL;
    if (sqlite3_open_v2(db_path, &db, SQLITE_OPEN_READONLY, NULL) != SQLITE_OK) {
        if (db) sqlite3_close(db);
        return 0;
    }
    sqlite3_busy_timeout(db, 1000);

    char drive_name[256];
    dupe_drive_name(db_file, drive_name, sizeof(drive_name));
    char scan_date[64];
    dupe_latest_checksum_date(db, "main", scan_date, sizeof(scan_date));

    sqlite3_stmt *stmt;
    const char *match = by_checksum ? "lower(checksum) = lower(?1)" : "file_name GLOB ?1";
    char sql[512];
    // Skip files no longer on disk; older databases may lack the status column
    snprintf(sql, sizeof(sql),
             "SELECT file_name, checksum, full_path FROM files WHERE %s "
             "AND (status IS NULL OR status != 'MISSING') ORDER BY full_path;", match);
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
        snprintf(sql, sizeof(sql),
                 "SELECT file_name, checksum, full_path FROM files WHERE %s ORDER BY full_path;", match);
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) != SQLITE_OK) {
            sqlite3_close(db);
            return 0;
        }
    }
    char glob[MAX_PATH * 3];
    if (by_checksum) {
        snprintf(glob, sizeof(glob), "%s", value);
    } else {
        dupe_name_to_glob(value, glob, sizeof(glob));
    }
    sqlite3_bind_text(stmt, 1, glob, -1, SQLITE_STATIC);

    int count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *checksum = (const char *)sqlite3_column_text(stmt, 1);
        GtkTreeIter iter;
        gtk_list_store_append(store, &iter);
        gtk_list_store_set(store, &iter,
                          0, sqlite3_column_text(stmt, 0),
                          1, drive_name,
                          2, checksum ? checksum : "",
                          3, sqlite3_column_text(stmt, 2),
                          4, (checksum && *checksum) ? scan_date : "Never",
                          -1);
        count++;
    }
    sqlite3_finalize(stmt);
    sqlite3_close(db);
    return count;
}

// Copies the present files of one database into the in-memory all_files table,
// so duplicates can be grouped across drives. Returns 1 on success.
static int dupe_collect_database(sqlite3 *mem, const char *db_file, const char *db_path) {
    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(mem, "ATTACH DATABASE ?1 AS src;", -1, &stmt, NULL) != SQLITE_OK) return 0;
    sqlite3_bind_text(stmt, 1, db_path, -1, SQLITE_STATIC);
    int rc = sqlite3_step(stmt);
    sqlite3_finalize(stmt);
    if (rc != SQLITE_DONE) return 0;

    char drive_name[256];
    dupe_drive_name(db_file, drive_name, sizeof(drive_name));
    char scan_date[64];
    dupe_latest_checksum_date(mem, "src", scan_date, sizeof(scan_date));

    const char *insert =
        "INSERT INTO all_files (file_name, drive, checksum, full_path, scan_date) "
        "SELECT file_name, ?1, COALESCE(checksum, ''), full_path, "
        "CASE WHEN checksum IS NULL OR checksum = '' THEN 'Never' ELSE ?2 END FROM src.files";
    char sql[512];
    // Skip files no longer on disk; older databases may lack the status column
    snprintf(sql, sizeof(sql), "%s WHERE status IS NULL OR status != 'MISSING';", insert);
    int ok = 0;
    if (sqlite3_prepare_v2(mem, sql, -1, &stmt, NULL) != SQLITE_OK) {
        snprintf(sql, sizeof(sql), "%s;", insert);
        if (sqlite3_prepare_v2(mem, sql, -1, &stmt, NULL) != SQLITE_OK) stmt = NULL;
    }
    if (stmt) {
        sqlite3_bind_text(stmt, 1, drive_name, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, scan_date, -1, SQLITE_STATIC);
        ok = sqlite3_step(stmt) == SQLITE_DONE;
        sqlite3_finalize(stmt);
    }
    sqlite3_exec(mem, "DETACH DATABASE src;", NULL, NULL, NULL);
    return ok;
}

// Lists every file whose name (and, if with_checksum, checksum) is shared with at least one
// other file across the given databases. Sets *groups and *drives; returns rows added.
static int dupe_find_groups(char **db_files, int with_checksum, GtkListStore *store,
                            int *groups, int *drives) {
    *groups = 0;
    *drives = 0;
    sqlite3 *mem = NULL;
    if (sqlite3_open(":memory:", &mem) != SQLITE_OK) {
        if (mem) sqlite3_close(mem);
        return 0;
    }
    sqlite3_busy_timeout(mem, 1000);
    sqlite3_exec(mem, "CREATE TABLE all_files (file_name TEXT, drive TEXT, checksum TEXT, "
                      "full_path TEXT, scan_date TEXT);", NULL, NULL, NULL);

    sqlite3_exec(mem, "BEGIN TRANSACTION;", NULL, NULL, NULL);
    for (int i = 0; db_files[i]; i++) {
        char db_path[MAX_PATH];
        snprintf(db_path, sizeof(db_path), "%s/%s", db_dir_path, db_files[i]);
        dupe_collect_database(mem, db_files[i], db_path);
    }
    sqlite3_exec(mem, "COMMIT;", NULL, NULL, NULL);

    const char *sql;
    if (with_checksum) {
        // Files without a checksum can't be confirmed identical, so they are left out
        sqlite3_exec(mem, "CREATE INDEX idx_all_name_ck ON all_files(file_name, lower(checksum));",
                     NULL, NULL, NULL);
        sql = "SELECT a.file_name, a.drive, a.checksum, a.full_path, a.scan_date, "
              "a.file_name || '/' || lower(a.checksum) FROM all_files a "
              "JOIN (SELECT file_name, lower(checksum) AS ck FROM all_files WHERE checksum != '' "
              "      GROUP BY file_name, lower(checksum) HAVING COUNT(*) > 1) g "
              "ON a.file_name = g.file_name AND lower(a.checksum) = g.ck "
              "ORDER BY a.file_name, lower(a.checksum), a.drive, a.full_path;";
    } else {
        sqlite3_exec(mem, "CREATE INDEX idx_all_name ON all_files(file_name);", NULL, NULL, NULL);
        sql = "SELECT a.file_name, a.drive, a.checksum, a.full_path, a.scan_date, a.file_name "
              "FROM all_files a "
              "JOIN (SELECT file_name FROM all_files GROUP BY file_name HAVING COUNT(*) > 1) g "
              "ON a.file_name = g.file_name "
              "ORDER BY a.file_name, a.drive, a.full_path;";
    }

    sqlite3_stmt *stmt;
    int count = 0;
    if (sqlite3_prepare_v2(mem, sql, -1, &stmt, NULL) == SQLITE_OK) {
        GHashTable *seen_drives = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
        char *last_key = NULL;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *drive = (const char *)sqlite3_column_text(stmt, 1);
            const char *key = (const char *)sqlite3_column_text(stmt, 5);
            if (!last_key || strcmp(last_key, key) != 0) {
                g_free(last_key);
                last_key = g_strdup(key);
                (*groups)++;
            }
            if (!g_hash_table_contains(seen_drives, drive)) {
                g_hash_table_add(seen_drives, g_strdup(drive));
            }
            GtkTreeIter iter;
            gtk_list_store_append(store, &iter);
            gtk_list_store_set(store, &iter,
                              0, sqlite3_column_text(stmt, 0),
                              1, drive,
                              2, sqlite3_column_text(stmt, 2),
                              3, sqlite3_column_text(stmt, 3),
                              4, sqlite3_column_text(stmt, 4),
                              -1);
            count++;
        }
        *drives = g_hash_table_size(seen_drives);
        g_free(last_key);
        g_hash_table_destroy(seen_drives);
        sqlite3_finalize(stmt);
    }
    sqlite3_close(mem);
    return count;
}

// Returns a NULL-terminated list of the database file names the "In:" combo selects.
// In the duplicate-group modes the checkboxes pick the databases instead of the combo.
static char **dupe_selected_databases(int mode) {
    GPtrArray *files = g_ptr_array_new();
    char *selected_db;
    if (mode == DUPE_MODE_SEARCH) {
        selected_db = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(dupe_db_combo));
    } else if (gtk_check_button_get_active(GTK_CHECK_BUTTON(dupe_db_all_check))) {
        selected_db = g_strdup("All Databases");
    } else {
        selected_db = NULL;
        for (GtkWidget *c = gtk_widget_get_first_child(dupe_db_check_box); c; c = gtk_widget_get_next_sibling(c)) {
            if (gtk_check_button_get_active(GTK_CHECK_BUTTON(c))) {
                g_ptr_array_add(files, g_strdup(gtk_check_button_get_label(GTK_CHECK_BUTTON(c))));
            }
        }
    }
    if (selected_db && strcmp(selected_db, "All Databases") == 0) {
        DIR *dir = opendir(db_dir_path);
        if (dir) {
            struct dirent *entry;
            while ((entry = readdir(dir)) != NULL) {
                size_t len = strlen(entry->d_name);
                if (len > 3 && strcmp(entry->d_name + len - 3, ".db") == 0 &&
                    strcmp(entry->d_name, "drives.db") != 0) {
                    g_ptr_array_add(files, g_strdup(entry->d_name));
                }
            }
            closedir(dir);
        }
    } else if (selected_db) {
        g_ptr_array_add(files, g_strdup(selected_db));
    }
    g_free(selected_db);
    g_ptr_array_add(files, NULL);
    return (char **)g_ptr_array_free(files, FALSE);
}

void on_dupe_search_clicked(GtkButton *button, gpointer user_data) {
    (void)button; (void)user_data;

    int mode = gtk_combo_box_get_active(GTK_COMBO_BOX(dupe_mode_combo));
    int by_checksum = gtk_combo_box_get_active(GTK_COMBO_BOX(dupe_search_by_combo)) == 1;
    char value[MAX_PATH];
    if (mode == DUPE_MODE_SEARCH) {
        snprintf(value, sizeof(value), "%s", gtk_editable_get_text(GTK_EDITABLE(dupe_search_entry)));
        if (by_checksum) g_strstrip(value);
        if (value[0] == '\0') {
            gtk_label_set_text(GTK_LABEL(dupe_status_label),
                               by_checksum ? "Enter a checksum to search" : "Enter a file name to search");
            return;
        }
    }

    // Detach the model while filling it; a large result set fills much faster unsorted and unbound
    GtkListStore *store = GTK_LIST_STORE(gtk_tree_view_get_model(GTK_TREE_VIEW(dupe_results_tree)));
    g_object_ref(store);
    gtk_tree_view_set_model(GTK_TREE_VIEW(dupe_results_tree), NULL);
    gtk_list_store_clear(store);
    gtk_tree_sortable_set_sort_column_id(GTK_TREE_SORTABLE(store),
                                         GTK_TREE_SORTABLE_UNSORTED_SORT_COLUMN_ID, GTK_SORT_ASCENDING);

    char **db_files = dupe_selected_databases(mode);
    if (!db_files[0]) {
        g_strfreev(db_files);
        gtk_tree_view_set_model(GTK_TREE_VIEW(dupe_results_tree), GTK_TREE_MODEL(store));
        g_object_unref(store);
        gtk_label_set_text(GTK_LABEL(dupe_status_label), "Select at least one database");
        return;
    }
    int count = 0, drives = 0, groups = 0;
    char status[256];

    if (mode == DUPE_MODE_SEARCH) {
        for (int i = 0; db_files[i]; i++) {
            char db_path[MAX_PATH];
            snprintf(db_path, sizeof(db_path), "%s/%s", db_dir_path, db_files[i]);
            int n = dupe_search_database(db_files[i], db_path, value, by_checksum, store);
            if (n > 0) drives++;
            count += n;
        }
        if (count == 0) {
            snprintf(status, sizeof(status), "No matching files found");
        } else if (count == 1) {
            snprintf(status, sizeof(status), "Found 1 file - no duplicates");
        } else {
            snprintf(status, sizeof(status), "Found %d copies on %d drive%s", count, drives, drives == 1 ? "" : "s");
        }
    } else {
        int with_checksum = mode == DUPE_MODE_SAME_NAME_CHECKSUM;
        count = dupe_find_groups(db_files, with_checksum, store, &groups, &drives);
        if (count == 0) {
            snprintf(status, sizeof(status), "No duplicate files found");
        } else {
            snprintf(status, sizeof(status), "Found %d files in %d duplicate group%s on %d drive%s",
                     count, groups, groups == 1 ? "" : "s", drives, drives == 1 ? "" : "s");
        }
    }
    g_strfreev(db_files);

    gtk_tree_view_set_model(GTK_TREE_VIEW(dupe_results_tree), GTK_TREE_MODEL(store));
    g_object_unref(store);
    gtk_label_set_text(GTK_LABEL(dupe_status_label), status);
}

static void on_dupe_search_by_changed(GtkComboBox *combo, gpointer user_data) {
    (void)user_data;
    gtk_entry_set_placeholder_text(GTK_ENTRY(dupe_search_entry),
                                   gtk_combo_box_get_active(combo) == 1 ? "Enter SHA-256 checksum..." : "File name (* matches any characters)...");
}

// The search fields only apply to the search mode; the duplicate-group modes need no input
static void on_dupe_mode_changed(GtkComboBox *combo, gpointer user_data) {
    (void)user_data;
    gboolean searching = gtk_combo_box_get_active(combo) == DUPE_MODE_SEARCH;
    gtk_widget_set_visible(dupe_search_by_label, searching);
    gtk_widget_set_visible(dupe_search_by_combo, searching);
    gtk_widget_set_visible(dupe_search_entry, searching);
    gtk_widget_set_visible(dupe_search_filler, !searching);
    gtk_widget_set_visible(dupe_db_combo, searching);
    gtk_widget_set_visible(dupe_db_multi_button, !searching);
}

// Shows the database selection on the button: "All Databases", one name, or a count
static void dupe_update_db_multi_label(void) {
    const char *label = "All Databases";
    char buf[300];
    if (!gtk_check_button_get_active(GTK_CHECK_BUTTON(dupe_db_all_check))) {
        int n = 0;
        const char *only = NULL;
        for (GtkWidget *c = gtk_widget_get_first_child(dupe_db_check_box); c; c = gtk_widget_get_next_sibling(c)) {
            if (gtk_check_button_get_active(GTK_CHECK_BUTTON(c))) {
                n++;
                only = gtk_check_button_get_label(GTK_CHECK_BUTTON(c));
            }
        }
        if (n == 0) {
            label = "Select Databases...";
        } else if (n == 1) {
            snprintf(buf, sizeof(buf), "%s", only);
            label = buf;
        } else {
            snprintf(buf, sizeof(buf), "%d Databases", n);
            label = buf;
        }
    }
    gtk_menu_button_set_label(GTK_MENU_BUTTON(dupe_db_multi_button), label);
}

// "All Databases" overrides the individual checkboxes, so they are disabled while it is on
static void on_dupe_db_all_toggled(GtkCheckButton *check, gpointer user_data) {
    (void)user_data;
    gtk_widget_set_sensitive(dupe_db_check_box, !gtk_check_button_get_active(check));
    dupe_update_db_multi_label();
}

static void on_dupe_db_check_toggled(GtkCheckButton *check, gpointer user_data) {
    (void)check; (void)user_data;
    dupe_update_db_multi_label();
}

// Rebuilds the database checkboxes from the given ".db" file names, keeping earlier selections
static void dupe_refresh_db_checks(GPtrArray *files) {
    GHashTable *checked = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
    GtkWidget *c;
    while ((c = gtk_widget_get_first_child(dupe_db_check_box)) != NULL) {
        if (gtk_check_button_get_active(GTK_CHECK_BUTTON(c))) {
            g_hash_table_add(checked, g_strdup(gtk_check_button_get_label(GTK_CHECK_BUTTON(c))));
        }
        gtk_box_remove(GTK_BOX(dupe_db_check_box), c);
    }
    for (guint i = 0; i < files->len; i++) {
        const char *file_name = g_ptr_array_index(files, i);
        GtkWidget *check = gtk_check_button_new_with_label(file_name);
        gtk_check_button_set_active(GTK_CHECK_BUTTON(check), g_hash_table_contains(checked, file_name));
        g_signal_connect(check, "toggled", G_CALLBACK(on_dupe_db_check_toggled), NULL);
        gtk_box_append(GTK_BOX(dupe_db_check_box), check);
    }
    g_hash_table_destroy(checked);
    dupe_update_db_multi_label();
}

GtkWidget *create_dupefinder_tab() {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start(box, 16);
    gtk_widget_set_margin_end(box, 16);
    gtk_widget_set_margin_top(box, 16);
    gtk_widget_set_margin_bottom(box, 16);

    GtkWidget *search_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

    dupe_mode_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(dupe_mode_combo), "Search By File Name / Checksum");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(dupe_mode_combo), "All Files With Matching Names");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(dupe_mode_combo), "All Files With Matching Name + Checksum");
    gtk_combo_box_set_active(GTK_COMBO_BOX(dupe_mode_combo), DUPE_MODE_SEARCH);
    g_signal_connect(dupe_mode_combo, "changed", G_CALLBACK(on_dupe_mode_changed), NULL);

    dupe_search_by_label = gtk_label_new("Search by:");

    dupe_search_by_combo = gtk_combo_box_text_new();
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(dupe_search_by_combo), "File Name");
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(dupe_search_by_combo), "File Checksum");
    gtk_combo_box_set_active(GTK_COMBO_BOX(dupe_search_by_combo), 0);
    g_signal_connect(dupe_search_by_combo, "changed", G_CALLBACK(on_dupe_search_by_changed), NULL);

    dupe_search_entry = gtk_entry_new();
    gtk_widget_set_hexpand(dupe_search_entry, TRUE);
    gtk_entry_set_placeholder_text(GTK_ENTRY(dupe_search_entry), "File name (* matches any characters)...");
    g_signal_connect(dupe_search_entry, "activate", G_CALLBACK(on_dupe_search_clicked), NULL);

    dupe_search_filler = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    gtk_widget_set_hexpand(dupe_search_filler, TRUE);
    gtk_widget_set_visible(dupe_search_filler, FALSE);

    dupe_db_combo = gtk_combo_box_text_new();
    gtk_widget_set_size_request(dupe_db_combo, 200, -1);

    // Duplicate-group modes: pick one or more databases, or All, from a checkbox popover
    GtkWidget *popover_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 6);
    gtk_widget_set_margin_start(popover_box, 6);
    gtk_widget_set_margin_end(popover_box, 6);
    gtk_widget_set_margin_top(popover_box, 6);
    gtk_widget_set_margin_bottom(popover_box, 6);
    dupe_db_all_check = gtk_check_button_new_with_label("All Databases");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(dupe_db_all_check), TRUE);
    g_signal_connect(dupe_db_all_check, "toggled", G_CALLBACK(on_dupe_db_all_toggled), NULL);
    gtk_box_append(GTK_BOX(popover_box), dupe_db_all_check);
    gtk_box_append(GTK_BOX(popover_box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));
    dupe_db_check_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
    gtk_widget_set_sensitive(dupe_db_check_box, FALSE);
    GtkWidget *check_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_policy(GTK_SCROLLED_WINDOW(check_scroll), GTK_POLICY_NEVER, GTK_POLICY_AUTOMATIC);
    gtk_scrolled_window_set_propagate_natural_height(GTK_SCROLLED_WINDOW(check_scroll), TRUE);
    gtk_scrolled_window_set_max_content_height(GTK_SCROLLED_WINDOW(check_scroll), 400);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(check_scroll), dupe_db_check_box);
    gtk_box_append(GTK_BOX(popover_box), check_scroll);
    GtkWidget *popover = gtk_popover_new();
    gtk_popover_set_child(GTK_POPOVER(popover), popover_box);

    dupe_db_multi_button = gtk_menu_button_new();
    gtk_menu_button_set_popover(GTK_MENU_BUTTON(dupe_db_multi_button), popover);
    gtk_menu_button_set_label(GTK_MENU_BUTTON(dupe_db_multi_button), "All Databases");
    gtk_widget_set_size_request(dupe_db_multi_button, 200, -1);
    gtk_widget_set_visible(dupe_db_multi_button, FALSE);

    GtkWidget *search_btn = gtk_button_new_with_label("Find Duplicates");
    gtk_widget_add_css_class(search_btn, "suggested-action");
    g_signal_connect(search_btn, "clicked", G_CALLBACK(on_dupe_search_clicked), NULL);

    gtk_box_append(GTK_BOX(search_box), gtk_label_new("Mode:"));
    gtk_box_append(GTK_BOX(search_box), dupe_mode_combo);
    gtk_box_append(GTK_BOX(search_box), dupe_search_by_label);
    gtk_box_append(GTK_BOX(search_box), dupe_search_by_combo);
    gtk_box_append(GTK_BOX(search_box), dupe_search_entry);
    gtk_box_append(GTK_BOX(search_box), dupe_search_filler);
    gtk_box_append(GTK_BOX(search_box), gtk_label_new("In:"));
    gtk_box_append(GTK_BOX(search_box), dupe_db_combo);
    GtkWidget *refresh_btn = gtk_button_new_with_label("Refresh");
    gtk_widget_set_tooltip_text(refresh_btn, "Reload the database list");
    g_signal_connect(refresh_btn, "clicked", G_CALLBACK((GCallback)dupe_refresh_databases), NULL);

    gtk_box_append(GTK_BOX(search_box), dupe_db_multi_button);
    gtk_box_append(GTK_BOX(search_box), search_btn);
    gtk_box_append(GTK_BOX(search_box), refresh_btn);
    gtk_box_append(GTK_BOX(box), search_box);

    GtkListStore *store = gtk_list_store_new(5, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                                             G_TYPE_STRING, G_TYPE_STRING);
    dupe_results_tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
    g_object_unref(store);

    const char *titles[] = {"File Name", "Drive", "Checksum", "Full Path", "Last Checksum Calculation"};
    for (int i = 0; i < 5; i++) {
        GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
        GtkTreeViewColumn *column = gtk_tree_view_column_new_with_attributes(titles[i], renderer, "text", i, NULL);
        gtk_tree_view_column_set_resizable(column, TRUE);
        gtk_tree_view_column_set_sort_column_id(column, i);
        if (i == 3) gtk_tree_view_column_set_expand(column, TRUE);
        gtk_tree_view_append_column(GTK_TREE_VIEW(dupe_results_tree), column);
    }

    GtkWidget *scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scroll), dupe_results_tree);
    gtk_widget_set_vexpand(scroll, TRUE);
    gtk_box_append(GTK_BOX(box), scroll);

    dupe_status_label = gtk_label_new("Ready");
    gtk_label_set_xalign(GTK_LABEL(dupe_status_label), 0.0);
    gtk_widget_add_css_class(dupe_status_label, "dim-label");
    gtk_box_append(GTK_BOX(box), dupe_status_label);

    return box;
}

// ============================================================================
// TAB 7: ABOUT
// ============================================================================

GtkWidget *create_about_tab() {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
    gtk_widget_set_margin_start(box, 32);
    gtk_widget_set_margin_end(box, 32);
    gtk_widget_set_margin_top(box, 32);
    gtk_widget_set_margin_bottom(box, 32);
    gtk_widget_set_halign(box, GTK_ALIGN_CENTER);
    gtk_widget_set_valign(box, GTK_ALIGN_CENTER);

    GtkWidget *title = gtk_label_new("File Tracker Unified");
    gtk_widget_add_css_class(title, "title-1");
    gtk_box_append(GTK_BOX(box), title);

    GtkWidget *version = gtk_label_new("Version 1.0");
    gtk_widget_add_css_class(version, "dim-label");
    gtk_box_append(GTK_BOX(box), version);

    gtk_box_append(GTK_BOX(box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    GtkWidget *desc = gtk_label_new(
        "Unified application combining:\n\n"
        "• File Locator - Search files across databases\n"
        "• DupeFinder - Find duplicate files by name, checksum, or both\n"
        "• Drives Manager - Track and manage storage drives\n"
        "• Summary Viewer - View scan run statistics\n"
        "• Logs Viewer - View detailed run logs with filters\n"
        "• File Scanner - Full file scanning with progress tracking\n\n"
        "Complete File Tracker functionality\n"
        "in one convenient application.");
    gtk_label_set_justify(GTK_LABEL(desc), GTK_JUSTIFY_CENTER);
    gtk_box_append(GTK_BOX(box), desc);

    return box;
}

// ============================================================================
// MAIN WINDOW SETUP
// ============================================================================

void locator_refresh_databases() {
    GPtrArray *files = list_database_files();
    if (!files) return;
    refill_database_combo(locator_db_combo, files, "All Databases", FALSE);
    g_ptr_array_unref(files);
}

void dupe_refresh_databases() {
    GPtrArray *files = list_database_files();
    if (!files) return;
    refill_database_combo(dupe_db_combo, files, "All Databases", FALSE);
    dupe_refresh_db_checks(files);
    g_ptr_array_unref(files);
}

void summary_refresh_databases() {
    GPtrArray *files = list_database_files();
    if (!files) return;
    refill_database_combo(summary_db_combo, files, NULL, FALSE);
    g_ptr_array_unref(files);
}

// Refilling a drive combo reloads its run list, so the selected run is restored afterwards
static void compare_refill_side(GtkWidget *drive_combo, GtkWidget *run_combo, GPtrArray *files) {
    char *run_id = g_strdup(gtk_combo_box_get_active_id(GTK_COMBO_BOX(run_combo)));
    refill_database_combo(drive_combo, files, NULL, TRUE);
    if (run_id) gtk_combo_box_set_active_id(GTK_COMBO_BOX(run_combo), run_id);
    g_free(run_id);
}

void compare_refresh_databases() {
    GPtrArray *files = list_database_files();
    if (!files) return;
    compare_refill_side(compare_run1_drive_combo, compare_run1_run_combo, files);
    compare_refill_side(compare_run2_drive_combo, compare_run2_run_combo, files);
    g_ptr_array_unref(files);
}

void refresh_all_database_combos() {
    locator_refresh_databases();
    dupe_refresh_databases();
    summary_refresh_databases();
    compare_refresh_databases();
}

// ============================================================================
// NEW WINDOW
// ============================================================================

// Each window is its own instance of the application (separate process), so
// windows have fully independent tab state and can run scans side by side
static void launch_new_instance(void) {
    char exe[MAX_PATH] = "";
#ifdef __APPLE__
    char raw[MAX_PATH];
    uint32_t size = sizeof(raw);
    if (_NSGetExecutablePath(raw, &size) != 0 || !realpath(raw, exe)) exe[0] = '\0';
#else
    ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    exe[n > 0 ? n : 0] = '\0';
#endif

    GError *error = NULL;
    gboolean ok = FALSE;
    if (exe[0]) {
        // Inside an app bundle, ask Launch Services for a new instance of the
        // bundle so it is activated and brought to the front
        char *bundle_end = strstr(exe, ".app/Contents/MacOS/");
        if (bundle_end) {
            bundle_end[4] = '\0';
            char *argv[] = { "/usr/bin/open", "-n", exe, NULL };
            ok = g_spawn_async(NULL, argv, NULL, G_SPAWN_DEFAULT, NULL, NULL, NULL, &error);
        } else {
            char *argv[] = { exe, NULL };
            ok = g_spawn_async(NULL, argv, NULL, G_SPAWN_DEFAULT, NULL, NULL, NULL, &error);
        }
    }

    if (!ok) {
        GtkAlertDialog *alert = gtk_alert_dialog_new("Could not open a new window");
        gtk_alert_dialog_set_detail(alert, error ? error->message : "Could not find the application executable");
        gtk_alert_dialog_show(alert, GTK_WINDOW(window));
        g_object_unref(alert);
    }
    g_clear_error(&error);
}

static void on_new_window_action(GSimpleAction *action, GVariant *parameter, gpointer user_data) {
    (void)action; (void)parameter; (void)user_data;
    launch_new_instance();
}

static void on_quit_action(GSimpleAction *action, GVariant *parameter, gpointer user_data) {
    (void)action; (void)parameter;
    g_application_quit(G_APPLICATION(user_data));
}

void startup(GtkApplication *app, gpointer user_data) {
    (void)user_data;

    static const GActionEntry app_actions[] = {
        { .name = "new-window", .activate = on_new_window_action },
        { .name = "quit",       .activate = on_quit_action },
    };
    g_action_map_add_action_entries(G_ACTION_MAP(app), app_actions, G_N_ELEMENTS(app_actions), app);
    gtk_application_set_accels_for_action(app, "app.new-window", (const char *[]){ "<Primary>n", NULL });
    gtk_application_set_accels_for_action(app, "app.quit", (const char *[]){ "<Primary>q", NULL });

    // File menu (in the macOS menu bar; inside the window elsewhere)
    GMenu *file_menu = g_menu_new();
    g_menu_append(file_menu, "New Window", "app.new-window");
    GMenu *menubar = g_menu_new();
    g_menu_append_submenu(menubar, "File", G_MENU_MODEL(file_menu));
    gtk_application_set_menubar(app, G_MENU_MODEL(menubar));
    g_object_unref(file_menu);
    g_object_unref(menubar);

#ifdef __APPLE__
    // Right-click the Dock icon → New Window
    macos_install_dock_menu("New Window", launch_new_instance);
#endif
}

void activate(GtkApplication *app, gpointer user_data) {
    (void)user_data;

    window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window), "File Tracker Unified");
    gtk_window_set_default_size(GTK_WINDOW(window), 1200, 750);

    // Create notebook with tabs
    main_notebook = gtk_notebook_new();
    gtk_notebook_set_tab_pos(GTK_NOTEBOOK(main_notebook), GTK_POS_TOP);

    // Add tabs
    gtk_notebook_append_page(GTK_NOTEBOOK(main_notebook), create_locator_tab(),
                            gtk_label_new("File Locator"));
    gtk_notebook_append_page(GTK_NOTEBOOK(main_notebook), create_dupefinder_tab(),
                            gtk_label_new("DupeFinder"));
    gtk_notebook_append_page(GTK_NOTEBOOK(main_notebook), create_drives_tab(),
                            gtk_label_new("Drives"));
    gtk_notebook_append_page(GTK_NOTEBOOK(main_notebook), create_summary_tab(),
                            gtk_label_new("Summary"));
    gtk_notebook_append_page(GTK_NOTEBOOK(main_notebook), create_logs_tab(),
                            gtk_label_new("Logs"));
    gtk_notebook_append_page(GTK_NOTEBOOK(main_notebook), create_compare_tab(),
                            gtk_label_new("Compare"));
    gtk_notebook_append_page(GTK_NOTEBOOK(main_notebook), create_scanner_tab(),
                            gtk_label_new("File Scanner"));
    gtk_notebook_append_page(GTK_NOTEBOOK(main_notebook), create_about_tab(),
                            gtk_label_new("About"));

    gtk_window_set_child(GTK_WINDOW(window), main_notebook);

    // Initialize scanner volumes list and logs databases
    scanner_refresh_volumes();
    logs_refresh_databases();
    gtk_window_present(GTK_WINDOW(window));

    // Initialize
    refresh_all_database_combos();
    drives_refresh_list();
}

int main(int argc, char *argv[]) {
    setlocale(LC_NUMERIC, "");

    const char *home = getenv("HOME");
    if (!home) {
        fprintf(stderr, "Error: HOME not set\n");
        return 1;
    }

    snprintf(db_dir_path, sizeof(db_dir_path), "%s/db/FileTracker", home);

    // Create directory if it doesn't exist
    char mkdir_cmd[MAX_PATH + 20];
    snprintf(mkdir_cmd, sizeof(mkdir_cmd), "mkdir -p %s", db_dir_path);
    system(mkdir_cmd);

    load_ignore_list();

    // NON_UNIQUE: every launch runs its own window rather than handing off
    // to an already-running instance
    GtkApplication *app = gtk_application_new("com.filetracker.unified", G_APPLICATION_NON_UNIQUE);
    g_signal_connect(app, "startup", G_CALLBACK(startup), NULL);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);

    int status = g_application_run(G_APPLICATION(app), argc, argv);

    // Cleanup
    for (int i = 0; i < ignore_count; i++) {
        free(ignore_list[i]);
    }

    g_object_unref(app);
    return status;
}

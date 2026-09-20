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
int ignore_count = 0;

// ============================================================================
// UTILITY FUNCTIONS
// ============================================================================

void get_timestamp(char *buffer, size_t size) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    strftime(buffer, size, "%Y-%m-%d %H:%M:%S", t);
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
            sqlite3_bind_text(stmt, 5, "Auto-added by file_tracker", -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 6, timestamp, -1, SQLITE_STATIC);
        } else {
            sqlite3_bind_text(stmt, 2, "Auto-added by file_tracker", -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 3, timestamp, -1, SQLITE_STATIC);
        }

        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    sqlite3_close(drives_db);
}

void update_drive_stats(const char *drive_name, const char *source_path) {
    const char *home = getenv("HOME");
    if (!home) return;

    char drives_db_path[MAX_PATH];
    snprintf(drives_db_path, sizeof(drives_db_path), "%s/db/FileTracker/drives.db", home);

    sqlite3 *drives_db = NULL;
    if (sqlite3_open(drives_db_path, &drives_db) != SQLITE_OK) {
        if (drives_db) sqlite3_close(drives_db);
        return;
    }

    // Get drive stats
    long long capacity = 0, available = 0, used = 0;
    int has_stats = get_drive_stats(source_path, &capacity, &available, &used);

    if (!has_stats) {
        sqlite3_close(drives_db);
        return;
    }

    char timestamp[64];
    get_timestamp(timestamp, sizeof(timestamp));

    const char *update_sql =
        "UPDATE drives SET capacity = ?, space_available = ?, space_used = ?, last_updated = ? "
        "WHERE drive_name = ?;";

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(drives_db, update_sql, -1, &stmt, 0) == SQLITE_OK) {
        sqlite3_bind_int64(stmt, 1, capacity);
        sqlite3_bind_int64(stmt, 2, available);
        sqlite3_bind_int64(stmt, 3, used);
        sqlite3_bind_text(stmt, 4, timestamp, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 5, drive_name, -1, SQLITE_STATIC);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }

    sqlite3_close(drives_db);
}

void auto_add_or_update_drive(const char *db_path, const char *source_path) {
    // Extract drive name from db_path
    // db_path format: ~/db/FileTracker/DriveName.db
    char *last_slash = strrchr(db_path, '/');
    if (!last_slash) return;

    char drive_name[256];
    strncpy(drive_name, last_slash + 1, sizeof(drive_name) - 1);
    drive_name[sizeof(drive_name) - 1] = '\0';

    // Remove .db extension
    char *dot = strrchr(drive_name, '.');
    if (dot && strcmp(dot, ".db") == 0) {
        *dot = '\0';
    }

    // Check if drive already exists - if so, update; if not, add
    if (drive_exists_in_tracker(drive_name)) {
        update_drive_stats(drive_name, source_path);
    } else {
        add_drive_to_tracker(drive_name, source_path);
    }
}

// Volumes hidden from all volume lists
static int is_excluded_volume(const char *name) {
    return fnmatch("com.apple.TimeMachine*", name, 0) == 0 ||
           fnmatch("mbp_backup", name, 0) == 0 ||
           fnmatch("Macintosh HD", name, 0) == 0;
}

int update_all_mounted_drives() {
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
            if (drive_exists_in_tracker(entry->d_name)) {
                update_drive_stats(entry->d_name, full_path);
                updated_count++;
            }
        }
    }
    closedir(dir);
    return updated_count;
}

void refresh_all_database_combos();
void drives_refresh_list();

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
    gtk_box_append(GTK_BOX(search_box), locator_db_combo);
    gtk_box_append(GTK_BOX(search_box), search_btn);
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

    int count = update_all_mounted_drives();
    drives_refresh_list();

    char message[256];
    if (count > 0) {
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

void drives_refresh_list() {
    char drives_db[MAX_PATH];
    snprintf(drives_db, sizeof(drives_db), "%s/drives.db", db_dir_path);

    GtkListStore *store = GTK_LIST_STORE(gtk_tree_view_get_model(GTK_TREE_VIEW(drives_tree)));
    gtk_list_store_clear(store);

    sqlite3 *db;
    if (sqlite3_open(drives_db, &db) != SQLITE_OK) return;

    const char *sql = "SELECT drive_id, drive_name, storage_container, capacity, space_available, description, last_verified "
                     "FROM drives ORDER BY drive_name;";
    sqlite3_stmt *stmt;

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            long long capacity = sqlite3_column_int64(stmt, 3);
            long long available = sqlite3_column_int64(stmt, 4);
            long long used = capacity - available;

            char cap_str[64], used_str[64], avail_str[64];
            format_size(capacity, cap_str, sizeof(cap_str));
            format_size(used, used_str, sizeof(used_str));
            format_size(available, avail_str, sizeof(avail_str));

            const char *location = (const char *)sqlite3_column_text(stmt, 2);

            GtkTreeIter iter;
            gtk_list_store_append(store, &iter);
            gtk_list_store_set(store, &iter,
                              0, sqlite3_column_int64(stmt, 0),
                              1, sqlite3_column_text(stmt, 1),
                              2, location ? location : "",
                              3, cap_str,
                              4, used_str,
                              5, avail_str,
                              6, sqlite3_column_text(stmt, 5),
                              7, sqlite3_column_text(stmt, 6),
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

void on_drives_delete_clicked(GtkButton *button, gpointer user_data) {
    (void)button; (void)user_data;

    if (selected_drive_id < 0) {
        GtkAlertDialog *alert = gtk_alert_dialog_new("Please select a drive to delete");
        gtk_alert_dialog_show(alert, GTK_WINDOW(window));
        g_object_unref(alert);
        return;
    }

    char drives_db[MAX_PATH];
    snprintf(drives_db, sizeof(drives_db), "%s/drives.db", db_dir_path);

    sqlite3 *db;
    if (sqlite3_open(drives_db, &db) == SQLITE_OK) {
        sqlite3_stmt *stmt;
        sqlite3_prepare_v2(db, "DELETE FROM drives WHERE drive_id = ?;", -1, &stmt, 0);
        sqlite3_bind_int64(stmt, 1, selected_drive_id);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
        sqlite3_close(db);
    }

    selected_drive_id = -1;
    drives_refresh_list();
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
                          6, &description,
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

    GtkWidget *del_btn = gtk_button_new_with_label("Delete");
    gtk_widget_add_css_class(del_btn, "destructive-action");
    g_signal_connect(del_btn, "clicked", G_CALLBACK(on_drives_delete_clicked), NULL);

    GtkWidget *refresh_btn = gtk_button_new_with_label("Refresh");
    g_signal_connect(refresh_btn, "clicked", G_CALLBACK((GCallback)drives_refresh_list), NULL);

    GtkWidget *update_mounted_btn = gtk_button_new_with_label("Update Mounted Drives");
    g_signal_connect(update_mounted_btn, "clicked", G_CALLBACK(on_drives_update_mounted_clicked), NULL);

    gtk_box_append(GTK_BOX(add_box), name_label);
    gtk_box_append(GTK_BOX(add_box), drives_name_entry);
    gtk_box_append(GTK_BOX(add_box), location_label);
    gtk_box_append(GTK_BOX(add_box), drives_location_entry);
    gtk_box_append(GTK_BOX(add_box), add_btn);
    gtk_box_append(GTK_BOX(add_box), update_btn);
    gtk_box_append(GTK_BOX(add_box), del_btn);
    gtk_box_append(GTK_BOX(add_box), refresh_btn);
    gtk_box_append(GTK_BOX(add_box), update_mounted_btn);
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
    GtkListStore *store = gtk_list_store_new(8, G_TYPE_INT64, G_TYPE_STRING, G_TYPE_STRING,
                                             G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    drives_tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
    g_object_unref(store);

    const char *titles[] = {"ID", "Name", "Location", "Capacity", "Used", "Available", "Description", "Last Verified"};
    for (int i = 0; i < 8; i++) {
        GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
        GtkTreeViewColumn *column = gtk_tree_view_column_new_with_attributes(titles[i], renderer, "text", i, NULL);
        gtk_tree_view_column_set_resizable(column, TRUE);
        if (i == 6) gtk_tree_view_column_set_expand(column, TRUE);
        // Enable sorting on Name and Location columns
        if (i == 1 || i == 2) {
            gtk_tree_view_column_set_sort_column_id(column, i);
        }
        gtk_tree_view_append_column(GTK_TREE_VIEW(drives_tree), column);
    }

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
    gtk_box_append(GTK_BOX(db_box), db_label);
    gtk_box_append(GTK_BOX(db_box), summary_db_combo);
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

void logs_refresh_databases() {
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(logs_db_combo));

    DIR *dir = opendir(db_dir_path);
    if (!dir) return;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        size_t len = strlen(entry->d_name);
        if (len > 3 && strcmp(entry->d_name + len - 3, ".db") == 0 &&
            strcmp(entry->d_name, "drives.db") != 0) {
            char db_name[256];
            strncpy(db_name, entry->d_name, len - 3);
            db_name[len - 3] = '\0';
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(logs_db_combo), db_name);
        }
    }
    closedir(dir);

    gtk_combo_box_set_active(GTK_COMBO_BOX(logs_db_combo), 0);
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

static void logs_csv_field(FILE *fp, const char *s) {
    fputc('"', fp);
    for (; s && *s; s++) {
        if (*s == '"') fputc('"', fp);
        fputc(*s, fp);
    }
    fputc('"', fp);
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
        logs_csv_field(fp, (const char *)sqlite3_column_text(stmt, 0));
        fputc(',', fp);
        logs_csv_field(fp, (const char *)sqlite3_column_text(stmt, 1));
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
    gtk_box_append(GTK_BOX(db_box), db_label);
    gtk_box_append(GTK_BOX(db_box), logs_db_combo);
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
} ScannerContext;

GtkWidget *scanner_volumes_list;
GtkWidget *scanner_path_entry;
GtkWidget *scanner_db_entry;
GtkWidget *scanner_checksum_check;
GtkWidget *scanner_update_check;
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

    char status[512];
    snprintf(status, sizeof(status),
             "Processed: %d/%d | Unch: %d | Chg: %d | New: %d | Miss: %d | Ign: %d | Err: %d",
             processed, ctx->total_files, ctx->unchanged, ctx->changed,
             ctx->new_files, ctx->missing, ctx->ignored, ctx->errors);
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

void scanner_process_file(ScannerContext *ctx, const char *filepath, const char *filename) {
    if (ctx->should_stop) return;

    g_idle_add(scanner_update_current_file, g_strdup(filename));

    char log_mesg[256];

    struct stat sb;
    if (stat(filepath, &sb) != 0 || !S_ISREG(sb.st_mode)) return;

    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(ctx->db, "SELECT size, last_modified, checksum, status FROM files WHERE full_path = ?", -1, &stmt, NULL);
    sqlite3_bind_text(stmt, 1, filepath, -1, SQLITE_STATIC);

    int found = (sqlite3_step(stmt) == SQLITE_ROW);
    const char *db_status = found ? (const char *)sqlite3_column_text(stmt, 3) : NULL;
    // A file previously marked MISSING that is back on disk is treated as new
    int reappeared = db_status && strcmp(db_status, "MISSING") == 0;

    if (found && !reappeared) {
        long long db_size = sqlite3_column_int64(stmt, 0);
        long long db_mtime = sqlite3_column_int64(stmt, 1);
        const char *db_checksum = (const char *)sqlite3_column_text(stmt, 2);

        char checksum[HASH_SIZE] = "";
        int has_checksum = 0;
        if (ctx->enable_checksum) {
            has_checksum = compute_sha256(filepath, checksum);
        }

        if (ctx->enable_checksum && has_checksum && db_checksum && strlen(db_checksum) > 0 && strcmp(db_checksum, checksum) != 0) {
            ctx->changed++;

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

            scanner_log_message(ctx, log_mesg, filepath, checksum, sb.st_size, sb.st_mtime);

            if (ctx->update_mode) {
                sqlite3_stmt *up;
                sqlite3_prepare_v2(ctx->db, "UPDATE files SET size=?, last_modified=?, checksum=? WHERE full_path=?", -1, &up, NULL);
                sqlite3_bind_int64(up, 1, sb.st_size);
                sqlite3_bind_int64(up, 2, sb.st_mtime);
                sqlite3_bind_text(up, 3, checksum, -1, SQLITE_STATIC);
                sqlite3_bind_text(up, 4, filepath, -1, SQLITE_STATIC);
                sqlite3_step(up);
                sqlite3_finalize(up);
            }

        } else if (db_size != sb.st_size || db_mtime != sb.st_mtime) {
            ctx->changed++;

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

            scanner_log_message(ctx, log_mesg, filepath, checksum, sb.st_size, sb.st_mtime);

            if (ctx->update_mode) {
                // Always store a checksum that matches the new content
                if (!has_checksum) {
                    compute_sha256(filepath, checksum);
                }
                sqlite3_stmt *up;
                sqlite3_prepare_v2(ctx->db, "UPDATE files SET size=?, last_modified=?, checksum=? WHERE full_path=?", -1, &up, NULL);
                sqlite3_bind_int64(up, 1, sb.st_size);
                sqlite3_bind_int64(up, 2, sb.st_mtime);
                sqlite3_bind_text(up, 3, checksum, -1, SQLITE_STATIC);
                sqlite3_bind_text(up, 4, filepath, -1, SQLITE_STATIC);
                sqlite3_step(up);
                sqlite3_finalize(up);
            }
        } else {
            ctx->unchanged++;
            scanner_log_message(ctx, "UNCHANGED", filepath, db_checksum ? db_checksum : "", sb.st_size, sb.st_mtime);
        }
    } else {
        ctx->new_files++;
        char checksum[HASH_SIZE] = "";
        if (ctx->enable_checksum) compute_sha256(filepath, checksum);
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
    }

    sqlite3_finalize(stmt);
    g_idle_add(scanner_update_progress, ctx);
}

void scanner_scan_directory(ScannerContext *ctx, const char *dirpath) {
    if (ctx->should_stop) return;

    DIR *dir = opendir(dirpath);
    if (!dir) return;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (ctx->should_stop) break;
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;

        char filepath[MAX_PATH];
        snprintf(filepath, sizeof(filepath), "%s/%s", dirpath, entry->d_name);

        if (is_ignored(entry->d_name)) {
            // Process ignored files to record them in database
            struct stat sb;
            if (stat(filepath, &sb) == 0 && S_ISREG(sb.st_mode)) {
                scanner_process_ignored_file(ctx, filepath, entry->d_name);
            } else {
                // Log non-regular ignored files (directories, symlinks, etc.)
                scanner_log_message(ctx, "IGNORED", filepath, "", 0, 0);
                ctx->ignored++;
            }
            continue;
        }

        struct stat sb;
        if (stat(filepath, &sb) == 0) {
            if (S_ISDIR(sb.st_mode)) {
                scanner_scan_directory(ctx, filepath);
            } else if (S_ISREG(sb.st_mode)) {
                scanner_process_file(ctx, filepath, entry->d_name);
            }
        }
    }
    closedir(dir);
}

int scanner_count_files(const char *dirpath) {
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
            if (S_ISDIR(sb.st_mode)) count += scanner_count_files(filepath);
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
    ctx->total_files = scanner_count_files(ctx->scan_path);

    scanner_scan_directory(ctx, ctx->scan_path);
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
        auto_add_or_update_drive(ctx->db_path, ctx->scan_path);
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
    gtk_progress_bar_set_fraction(scanner_progress_bar, 1.0);

    char results[2048];
    snprintf(results, sizeof(results),
             "Scan Complete!\n\nPath: %s\nDatabase: %s\nMode: %s\nChecksum: %s\n\n"
             "Unchanged: %'d\nChanged: %'d\nNew: %'d\nMissing: %'d\nIgnored: %'d\nErrors: %'d\n\n"
             "Total: %'d files",
             ctx->scan_path, ctx->db_name,
             ctx->update_mode ? "Update" : "Read-only",
             ctx->enable_checksum ? "Enabled" : "Disabled",
             ctx->unchanged, ctx->changed, ctx->new_files, ctx->missing, ctx->ignored, ctx->errors,
             ctx->unchanged + ctx->changed + ctx->new_files + ctx->missing + ctx->errors);

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
    gtk_progress_bar_set_fraction(scanner_progress_bar, 0.0);
    gtk_label_set_text(GTK_LABEL(scanner_status_label), "Initializing...");

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
    GtkWidget *run1_label = gtk_label_new("Run 1");
    gtk_widget_add_css_class(run1_label, "heading");
    gtk_label_set_xalign(GTK_LABEL(run1_label), 0.0);
    gtk_box_append(GTK_BOX(left_box), run1_label);

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

void refresh_all_database_combos() {
    DIR *dir = opendir(db_dir_path);
    if (!dir) return;

    // Refresh locator combo
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(locator_db_combo));
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(locator_db_combo), "All Databases");

    // Refresh summary combo
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(summary_db_combo));

    // Refresh compare combos
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(compare_run1_drive_combo));
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(compare_run2_drive_combo));

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        size_t len = strlen(entry->d_name);
        if (len > 3 && strcmp(entry->d_name + len - 3, ".db") == 0 &&
            strcmp(entry->d_name, "drives.db") != 0) {
            char db_name[256];
            strncpy(db_name, entry->d_name, len - 3);
            db_name[len - 3] = '\0';

            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(locator_db_combo), entry->d_name);
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(summary_db_combo), entry->d_name);
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(compare_run1_drive_combo), db_name);
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(compare_run2_drive_combo), db_name);
        }
    }
    closedir(dir);

    gtk_combo_box_set_active(GTK_COMBO_BOX(locator_db_combo), 0);
    if (gtk_combo_box_get_active(GTK_COMBO_BOX(summary_db_combo)) < 0) {
        gtk_combo_box_set_active(GTK_COMBO_BOX(summary_db_combo), 0);
    }
    if (gtk_combo_box_get_active(GTK_COMBO_BOX(compare_run1_drive_combo)) < 0) {
        gtk_combo_box_set_active(GTK_COMBO_BOX(compare_run1_drive_combo), 0);
    }
    if (gtk_combo_box_get_active(GTK_COMBO_BOX(compare_run2_drive_combo)) < 0) {
        gtk_combo_box_set_active(GTK_COMBO_BOX(compare_run2_drive_combo), 0);
    }
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

    GtkApplication *app = gtk_application_new("com.filetracker.unified", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);

    int status = g_application_run(G_APPLICATION(app), argc, argv);

    // Cleanup
    for (int i = 0; i < ignore_count; i++) {
        free(ignore_list[i]);
    }

    g_object_unref(app);
    return status;
}

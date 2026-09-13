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

void refresh_all_database_combos();

// ============================================================================
// TAB 1: FILE LOCATOR (Simplified - most commonly used)
// ============================================================================

GtkWidget *locator_search_entry;
GtkWidget *locator_db_combo;
GtkWidget *locator_partial_check;
GtkWidget *locator_results_tree;
GtkWidget *locator_status_label;

void locator_search_database(const char *dbname, const char *db_path, const char *filename,
                              int partial, GtkListStore *store, char *first_checksum, int *count) {
    sqlite3 *db;
    sqlite3_stmt *stmt;

    struct stat st;
    if (stat(db_path, &st) != 0) return;
    if (sqlite3_open(db_path, &db) != SQLITE_OK) return;

    char sql[512];
    snprintf(sql, sizeof(sql),
             "SELECT full_path, size, last_modified, owner, checksum FROM files WHERE file_name %s ?;",
             partial ? "LIKE" : "=");

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
                          -1);
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
}

void on_locator_search_clicked(GtkButton *button, gpointer user_data) {
    (void)button; (void)user_data;

    const char *search_text = gtk_editable_get_text(GTK_EDITABLE(locator_search_entry));
    if (strlen(search_text) == 0) {
        gtk_label_set_text(GTK_LABEL(locator_status_label), "Enter a filename to search");
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
                    locator_search_database(entry->d_name, db_path, search_text, partial, store, first_checksum, &count);
                }
            }
            closedir(dir);
        }
    } else if (selected_db) {
        char db_path[MAX_PATH];
        snprintf(db_path, sizeof(db_path), "%s/%s", db_dir_path, selected_db);
        locator_search_database(selected_db, db_path, search_text, partial, store, first_checksum, &count);
    }

    if (selected_db) g_free(selected_db);

    char status[256];
    snprintf(status, sizeof(status), "Found %d file%s", count, count == 1 ? "" : "s");
    gtk_label_set_text(GTK_LABEL(locator_status_label), status);
}

GtkWidget *create_locator_tab() {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start(box, 16);
    gtk_widget_set_margin_end(box, 16);
    gtk_widget_set_margin_top(box, 16);
    gtk_widget_set_margin_bottom(box, 16);

    // Search controls
    GtkWidget *search_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

    GtkWidget *label = gtk_label_new("Search:");
    gtk_widget_set_size_request(label, 70, -1);

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
    gtk_box_append(GTK_BOX(search_box), locator_search_entry);
    gtk_box_append(GTK_BOX(search_box), locator_partial_check);
    gtk_box_append(GTK_BOX(search_box), locator_db_combo);
    gtk_box_append(GTK_BOX(search_box), search_btn);
    gtk_box_append(GTK_BOX(box), search_box);

    // Results
    GtkListStore *store = gtk_list_store_new(6, G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING,
                                             G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    locator_results_tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
    g_object_unref(store);

    const char *titles[] = {"Database", "Path", "Size", "Modified", "Owner", "✓"};
    for (int i = 0; i < 6; i++) {
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
GtkWidget *drives_desc_text;
sqlite3_int64 selected_drive_id = -1;

void drives_refresh_list() {
    char drives_db[MAX_PATH];
    snprintf(drives_db, sizeof(drives_db), "%s/drives.db", db_dir_path);

    GtkListStore *store = GTK_LIST_STORE(gtk_tree_view_get_model(GTK_TREE_VIEW(drives_tree)));
    gtk_list_store_clear(store);

    sqlite3 *db;
    if (sqlite3_open(drives_db, &db) != SQLITE_OK) return;

    const char *sql = "SELECT drive_id, drive_name, capacity, space_available, description, last_verified "
                     "FROM drives ORDER BY drive_name;";
    sqlite3_stmt *stmt;

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, NULL) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            char cap_str[64], avail_str[64];
            format_size(sqlite3_column_int64(stmt, 2), cap_str, sizeof(cap_str));
            format_size(sqlite3_column_int64(stmt, 3), avail_str, sizeof(avail_str));

            GtkTreeIter iter;
            gtk_list_store_append(store, &iter);
            gtk_list_store_set(store, &iter,
                              0, sqlite3_column_int64(stmt, 0),
                              1, sqlite3_column_text(stmt, 1),
                              2, cap_str,
                              3, avail_str,
                              4, sqlite3_column_text(stmt, 4),
                              5, sqlite3_column_text(stmt, 5),
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

        const char *insert_sql = "INSERT INTO drives (drive_name, description, last_updated) VALUES (?, ?, ?);";
        sqlite3_stmt *stmt;
        if (sqlite3_prepare_v2(db, insert_sql, -1, &stmt, 0) == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, name, -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 2, desc, -1, SQLITE_STATIC);
            sqlite3_bind_text(stmt, 3, timestamp, -1, SQLITE_STATIC);
            sqlite3_step(stmt);
            sqlite3_finalize(stmt);
        }
        sqlite3_close(db);
    }

    g_free(desc);
    gtk_editable_set_text(GTK_EDITABLE(drives_name_entry), "");
    gtk_text_buffer_set_text(buffer, "", -1);
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

void on_drives_selection_changed(GtkTreeSelection *selection, gpointer user_data) {
    (void)user_data;

    GtkTreeModel *model;
    GtkTreeIter iter;

    if (gtk_tree_selection_get_selected(selection, &model, &iter)) {
        gtk_tree_model_get(model, &iter, 0, &selected_drive_id, -1);
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

    GtkWidget *add_btn = gtk_button_new_with_label("Add");
    gtk_widget_add_css_class(add_btn, "suggested-action");
    g_signal_connect(add_btn, "clicked", G_CALLBACK(on_drives_add_clicked), NULL);

    GtkWidget *del_btn = gtk_button_new_with_label("Delete");
    gtk_widget_add_css_class(del_btn, "destructive-action");
    g_signal_connect(del_btn, "clicked", G_CALLBACK(on_drives_delete_clicked), NULL);

    GtkWidget *refresh_btn = gtk_button_new_with_label("Refresh");
    g_signal_connect(refresh_btn, "clicked", G_CALLBACK((GCallback)drives_refresh_list), NULL);

    gtk_box_append(GTK_BOX(add_box), name_label);
    gtk_box_append(GTK_BOX(add_box), drives_name_entry);
    gtk_box_append(GTK_BOX(add_box), add_btn);
    gtk_box_append(GTK_BOX(add_box), del_btn);
    gtk_box_append(GTK_BOX(add_box), refresh_btn);
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
    GtkListStore *store = gtk_list_store_new(6, G_TYPE_INT64, G_TYPE_STRING, G_TYPE_STRING,
                                             G_TYPE_STRING, G_TYPE_STRING, G_TYPE_STRING);
    drives_tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
    g_object_unref(store);

    const char *titles[] = {"ID", "Name", "Capacity", "Available", "Description", "Last Verified"};
    for (int i = 0; i < 6; i++) {
        GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
        GtkTreeViewColumn *column = gtk_tree_view_column_new_with_attributes(titles[i], renderer, "text", i, NULL);
        gtk_tree_view_column_set_resizable(column, TRUE);
        if (i == 4) gtk_tree_view_column_set_expand(column, TRUE);
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
// TAB 4: FULL FILE SCANNER
// ============================================================================

typedef struct {
    char status[32];
    char path[MAX_PATH];
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

void scanner_log_message(ScannerContext *ctx, const char *status, const char *path) {
    if (ctx->log_count >= ctx->log_capacity) {
        ctx->log_capacity = ctx->log_capacity == 0 ? 1024 : ctx->log_capacity * 2;
        ctx->log_buffer = realloc(ctx->log_buffer, ctx->log_capacity * sizeof(ScanLogEntry));
    }
    strncpy(ctx->log_buffer[ctx->log_count].status, status, 31);
    ctx->log_buffer[ctx->log_count].status[31] = '\0';
    strncpy(ctx->log_buffer[ctx->log_count].path, path, MAX_PATH - 1);
    ctx->log_buffer[ctx->log_count].path[MAX_PATH - 1] = '\0';
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

void scanner_process_file(ScannerContext *ctx, const char *filepath, const char *filename) {
    if (ctx->should_stop) return;

    g_idle_add(scanner_update_current_file, g_strdup(filename));

    struct stat sb;
    if (stat(filepath, &sb) != 0 || !S_ISREG(sb.st_mode)) return;

    sqlite3_stmt *stmt;
    sqlite3_prepare_v2(ctx->db, "SELECT size, last_modified, checksum FROM files WHERE full_path = ?", -1, &stmt, NULL);
    sqlite3_bind_text(stmt, 1, filepath, -1, SQLITE_STATIC);

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        long long db_size = sqlite3_column_int64(stmt, 0);
        long long db_mtime = sqlite3_column_int64(stmt, 1);

        if (db_size != sb.st_size || db_mtime != sb.st_mtime) {
            ctx->changed++;
            scanner_log_message(ctx, "CHANGED", filepath);
            if (ctx->update_mode) {
                char checksum[HASH_SIZE] = "";
                if (ctx->enable_checksum) compute_sha256(filepath, checksum);
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
            scanner_log_message(ctx, "UNCHANGED", filepath);
        }
    } else {
        ctx->new_files++;
        scanner_log_message(ctx, "NEW", filepath);
        if (ctx->update_mode) {
            char checksum[HASH_SIZE] = "";
            if (ctx->enable_checksum) compute_sha256(filepath, checksum);
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
        if (is_ignored(entry->d_name)) { ctx->ignored++; continue; }

        char filepath[MAX_PATH];
        snprintf(filepath, sizeof(filepath), "%s/%s", dirpath, entry->d_name);

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

    sqlite3_exec(ctx->db, "CREATE TABLE IF NOT EXISTS files (id INTEGER PRIMARY KEY, file_name TEXT, full_path TEXT UNIQUE, size INTEGER, created INTEGER, last_modified INTEGER, owner TEXT, checksum TEXT, keywords TEXT);", 0, 0, 0);
    sqlite3_exec(ctx->db, "CREATE TABLE IF NOT EXISTS meta (id INTEGER PRIMARY KEY AUTOINCREMENT, last_checksum_verify_date TEXT, last_date_verify TEXT, verify_machine TEXT, num_unchanged INTEGER, num_changed INTEGER, num_new INTEGER, num_missing INTEGER, num_ignored INTEGER, num_errors INTEGER, update_mode TEXT, note TEXT);", 0, 0, 0);
    sqlite3_exec(ctx->db, "CREATE TABLE IF NOT EXISTS run_logs (id INTEGER PRIMARY KEY AUTOINCREMENT, run_id INTEGER, status TEXT, full_path TEXT, FOREIGN KEY(run_id) REFERENCES meta(id));", 0, 0, 0);

    g_idle_add(scanner_update_current_file, g_strdup("Counting files..."));
    ctx->total_files = scanner_count_files(ctx->scan_path);

    scanner_scan_directory(ctx, ctx->scan_path);

    if (ctx->update_mode) {
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
        sqlite3_bind_text(stmt, 10, "ON", -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 11, ctx->note, -1, SQLITE_STATIC);
        sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        ctx->run_id = sqlite3_last_insert_rowid(ctx->db);

        if (ctx->log_count > 0) {
            sqlite3_stmt *log_stmt;
            sqlite3_prepare_v2(ctx->db, "INSERT INTO run_logs (run_id, status, full_path) VALUES (?, ?, ?)", -1, &log_stmt, 0);
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
// TAB 5: ABOUT
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
        "• File Scanner - Full file scanning with progress tracking\n\n"
        "All major File Tracker features\n"
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

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        size_t len = strlen(entry->d_name);
        if (len > 3 && strcmp(entry->d_name + len - 3, ".db") == 0 &&
            strcmp(entry->d_name, "drives.db") != 0) {
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(locator_db_combo), entry->d_name);
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(summary_db_combo), entry->d_name);
        }
    }
    closedir(dir);

    gtk_combo_box_set_active(GTK_COMBO_BOX(locator_db_combo), 0);
    if (gtk_combo_box_get_active(GTK_COMBO_BOX(summary_db_combo)) < 0) {
        gtk_combo_box_set_active(GTK_COMBO_BOX(summary_db_combo), 0);
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
    gtk_notebook_append_page(GTK_NOTEBOOK(main_notebook), create_scanner_tab(),
                            gtk_label_new("File Scanner"));
    gtk_notebook_append_page(GTK_NOTEBOOK(main_notebook), create_about_tab(),
                            gtk_label_new("About"));

    gtk_window_set_child(GTK_WINDOW(window), main_notebook);

    // Initialize scanner volumes list
    scanner_refresh_volumes();
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

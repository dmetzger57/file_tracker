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
// TAB 4: QUICK SCANNER (Simplified version)
// ============================================================================

GtkWidget *scanner_path_entry;
GtkWidget *scanner_db_entry;
GtkWidget *scanner_update_check;
GtkWidget *scanner_status_label;
GtkWidget *scanner_start_button;

void on_scanner_browse_clicked(GtkButton *button, gpointer user_data) {
    (void)button; (void)user_data;

    GtkFileDialog *dialog = gtk_file_dialog_new();
    gtk_file_dialog_set_title(dialog, "Select Directory to Scan");
    gtk_file_dialog_set_modal(dialog, TRUE);

    // We'll handle the async result inline
    // For simplicity in this unified version, we'll just show the dialog
    // In a full implementation, you'd use gtk_file_dialog_select_folder with callback
}

void on_scanner_start_clicked(GtkButton *button, gpointer user_data) {
    (void)button; (void)user_data;

    const char *path = gtk_editable_get_text(GTK_EDITABLE(scanner_path_entry));
    const char *db_name = gtk_editable_get_text(GTK_EDITABLE(scanner_db_entry));

    if (strlen(path) == 0) {
        gtk_label_set_text(GTK_LABEL(scanner_status_label), "Please select a path to scan");
        return;
    }

    gtk_label_set_text(GTK_LABEL(scanner_status_label),
        "Quick scan feature - use the full File Tracker app for complete scanning functionality");
}

GtkWidget *create_scanner_tab() {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start(box, 16);
    gtk_widget_set_margin_end(box, 16);
    gtk_widget_set_margin_top(box, 16);
    gtk_widget_set_margin_bottom(box, 16);

    // Info label
    GtkWidget *info_label = gtk_label_new(
        "Quick Scanner - For full scanning features, use the dedicated File Tracker application");
    gtk_widget_add_css_class(info_label, "dim-label");
    gtk_box_append(GTK_BOX(box), info_label);

    // Path
    GtkWidget *path_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *path_label = gtk_label_new("Path:");
    gtk_widget_set_size_request(path_label, 80, -1);
    scanner_path_entry = gtk_entry_new();
    gtk_widget_set_hexpand(scanner_path_entry, TRUE);
    GtkWidget *browse_btn = gtk_button_new_with_label("Browse...");
    g_signal_connect(browse_btn, "clicked", G_CALLBACK(on_scanner_browse_clicked), NULL);
    gtk_box_append(GTK_BOX(path_box), path_label);
    gtk_box_append(GTK_BOX(path_box), scanner_path_entry);
    gtk_box_append(GTK_BOX(path_box), browse_btn);
    gtk_box_append(GTK_BOX(box), path_box);

    // Database
    GtkWidget *db_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *db_label = gtk_label_new("Database:");
    gtk_widget_set_size_request(db_label, 80, -1);
    scanner_db_entry = gtk_entry_new();
    gtk_widget_set_hexpand(scanner_db_entry, TRUE);
    gtk_box_append(GTK_BOX(db_box), db_label);
    gtk_box_append(GTK_BOX(db_box), scanner_db_entry);
    gtk_box_append(GTK_BOX(box), db_box);

    // Options
    scanner_update_check = gtk_check_button_new_with_label("Update Database");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(scanner_update_check), TRUE);
    gtk_box_append(GTK_BOX(box), scanner_update_check);

    // Start button
    scanner_start_button = gtk_button_new_with_label("Start Scan");
    gtk_widget_add_css_class(scanner_start_button, "suggested-action");
    gtk_widget_set_halign(scanner_start_button, GTK_ALIGN_CENTER);
    gtk_widget_set_size_request(scanner_start_button, 150, -1);
    g_signal_connect(scanner_start_button, "clicked", G_CALLBACK(on_scanner_start_clicked), NULL);
    gtk_box_append(GTK_BOX(box), scanner_start_button);

    // Status
    scanner_status_label = gtk_label_new("Ready - Configure path and database name above");
    gtk_label_set_xalign(GTK_LABEL(scanner_status_label), 0.0);
    gtk_widget_add_css_class(scanner_status_label, "dim-label");
    gtk_box_append(GTK_BOX(box), scanner_status_label);

    // Spacer
    GtkWidget *spacer = gtk_box_new(GTK_ORIENTATION_VERTICAL, 0);
    gtk_widget_set_vexpand(spacer, TRUE);
    gtk_box_append(GTK_BOX(box), spacer);

    return box;
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
        "• Quick Scanner - Basic file scanning\n\n"
        "For advanced scanning features, use the\n"
        "dedicated File Tracker application.");
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
                            gtk_label_new("Quick Scan"));
    gtk_notebook_append_page(GTK_NOTEBOOK(main_notebook), create_about_tab(),
                            gtk_label_new("About"));

    gtk_window_set_child(GTK_WINDOW(window), main_notebook);
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

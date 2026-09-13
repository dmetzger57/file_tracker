#include <gtk/gtk.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>

#define MAX_PATH 4096

// Global UI elements
GtkWidget *window;
GtkWidget *search_entry;
GtkWidget *db_combo;
GtkWidget *partial_check;
GtkWidget *search_button;
GtkWidget *results_tree;
GtkWidget *status_label;
GtkWidget *clear_button;

// Search state
char db_dir[MAX_PATH];
int total_found = 0;
char first_checksum[128] = "";

// ==== Utility Functions ====

void format_timestamp(time_t timestamp, char *buffer, size_t size) {
    struct tm *tm_info = localtime(&timestamp);
    strftime(buffer, size, "%Y-%m-%d %H:%M:%S", tm_info);
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

// ==== Database Search Functions ====

void search_database(const char *dbname, const char *db_path, const char *filename,
                     int partial, GtkListStore *store) {
    sqlite3 *db;
    sqlite3_stmt *stmt;
    int rc;

    struct stat st;
    if (stat(db_path, &st) != 0) {
        return; // skip missing or inaccessible files
    }

    rc = sqlite3_open(db_path, &db);
    if (rc) {
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
        total_found++;

        const char *checksum = (const char *)sqlite3_column_text(stmt, 7);
        if (first_checksum[0] == '\0' && checksum) {
            strncpy(first_checksum, checksum, sizeof(first_checksum) - 1);
        }

        // Check for checksum mismatch
        int checksum_match = (checksum && strcmp(first_checksum, checksum) == 0);
        const char *match_status = checksum_match ? "✓" : "⚠ Mismatch";

        // Format file size
        char size_str[64];
        format_size(sqlite3_column_int64(stmt, 3), size_str, sizeof(size_str));

        // Format timestamps
        char created_str[64], modified_str[64];
        format_timestamp(sqlite3_column_int64(stmt, 4), created_str, sizeof(created_str));
        format_timestamp(sqlite3_column_int64(stmt, 5), modified_str, sizeof(modified_str));

        // Add to results
        GtkTreeIter iter;
        gtk_list_store_append(store, &iter);
        gtk_list_store_set(store, &iter,
                          0, dbname,
                          1, sqlite3_column_text(stmt, 2), // full_path
                          2, size_str,
                          3, modified_str,
                          4, sqlite3_column_text(stmt, 6), // owner
                          5, checksum ? checksum : "",
                          6, match_status,
                          -1);
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);
}

void search_all_databases(const char *filename, int partial, GtkListStore *store) {
    DIR *dir = opendir(db_dir);
    if (!dir) {
        return;
    }

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        size_t len = strlen(entry->d_name);
        if (len > 3 && strcmp(entry->d_name + len - 3, ".db") == 0) {
            char db_path[MAX_PATH];
            snprintf(db_path, sizeof(db_path), "%s/%s", db_dir, entry->d_name);
            search_database(entry->d_name, db_path, filename, partial, store);
        }
    }
    closedir(dir);
}

// ==== GUI Callbacks ====

void on_search_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;

    const char *search_text = gtk_editable_get_text(GTK_EDITABLE(search_entry));
    if (strlen(search_text) == 0) {
        gtk_label_set_text(GTK_LABEL(status_label), "Please enter a filename to search");
        return;
    }

    // Clear previous results
    GtkTreeView *tree = GTK_TREE_VIEW(results_tree);
    GtkListStore *store = GTK_LIST_STORE(gtk_tree_view_get_model(tree));
    gtk_list_store_clear(store);

    total_found = 0;
    first_checksum[0] = '\0';

    gtk_label_set_text(GTK_LABEL(status_label), "Searching...");
    gtk_widget_set_sensitive(search_button, FALSE);

    // Get search options
    int partial = gtk_check_button_get_active(GTK_CHECK_BUTTON(partial_check));
    char *selected_db = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(db_combo));

    // Perform search
    if (selected_db && strcmp(selected_db, "All Databases") == 0) {
        search_all_databases(search_text, partial, store);
    } else if (selected_db) {
        char db_path[MAX_PATH];
        snprintf(db_path, sizeof(db_path), "%s/%s", db_dir, selected_db);
        search_database(selected_db, db_path, search_text, partial, store);
    }

    if (selected_db) {
        g_free(selected_db);
    }

    // Update status
    char status[256];
    if (total_found == 0) {
        snprintf(status, sizeof(status), "No matches found for '%s'", search_text);
    } else if (total_found == 1) {
        snprintf(status, sizeof(status), "Found 1 file matching '%s'", search_text);
    } else {
        snprintf(status, sizeof(status), "Found %d files matching '%s'", total_found, search_text);
    }
    gtk_label_set_text(GTK_LABEL(status_label), status);

    gtk_widget_set_sensitive(search_button, TRUE);
}

void on_clear_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;

    // Clear search field
    gtk_editable_set_text(GTK_EDITABLE(search_entry), "");

    // Clear results
    GtkTreeView *tree = GTK_TREE_VIEW(results_tree);
    GtkListStore *store = GTK_LIST_STORE(gtk_tree_view_get_model(tree));
    gtk_list_store_clear(store);

    // Reset status
    gtk_label_set_text(GTK_LABEL(status_label), "Ready");

    total_found = 0;
    first_checksum[0] = '\0';
}

void on_search_entry_activate(GtkEntry *entry, gpointer user_data) {
    (void)entry;
    (void)user_data;
    on_search_clicked(NULL, NULL);
}

void on_row_activated(GtkTreeView *tree_view, GtkTreePath *path,
                      GtkTreeViewColumn *column, gpointer user_data) {
    (void)column;
    (void)user_data;

    GtkTreeModel *model = gtk_tree_view_get_model(tree_view);
    GtkTreeIter iter;

    if (gtk_tree_model_get_iter(model, &iter, path)) {
        char *db_name, *full_path, *size, *modified, *owner, *checksum, *match_status;

        gtk_tree_model_get(model, &iter,
                          0, &db_name,
                          1, &full_path,
                          2, &size,
                          3, &modified,
                          4, &owner,
                          5, &checksum,
                          6, &match_status,
                          -1);

        // Create detailed info dialog
        char info[2048];
        snprintf(info, sizeof(info),
                 "Database: %s\n\n"
                 "Full Path: %s\n\n"
                 "Size: %s\n"
                 "Last Modified: %s\n"
                 "Owner: %s\n\n"
                 "Checksum: %s\n"
                 "Match Status: %s",
                 db_name, full_path, size, modified, owner,
                 checksum, match_status);

        GtkAlertDialog *dialog = gtk_alert_dialog_new("%s", info);
        gtk_alert_dialog_show(dialog, GTK_WINDOW(window));
        g_object_unref(dialog);

        g_free(db_name);
        g_free(full_path);
        g_free(size);
        g_free(modified);
        g_free(owner);
        g_free(checksum);
        g_free(match_status);
    }
}

void refresh_databases_list(void) {
    const char *home = getenv("HOME");
    if (!home) return;

    snprintf(db_dir, sizeof(db_dir), "%s/db/FileTracker", home);

    // Clear existing entries
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(db_combo));

    // Add "All Databases" option
    gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(db_combo), "All Databases");

    DIR *dir = opendir(db_dir);
    if (!dir) return;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        size_t len = strlen(entry->d_name);
        if (len > 3 && strcmp(entry->d_name + len - 3, ".db") == 0) {
            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(db_combo), entry->d_name);
        }
    }
    closedir(dir);

    // Select "All Databases" by default
    gtk_combo_box_set_active(GTK_COMBO_BOX(db_combo), 0);
}

// ==== Main Window Setup ====

void activate(GtkApplication *app, gpointer user_data) {
    (void)user_data;

    window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window), "File Locator");
    gtk_window_set_default_size(GTK_WINDOW(window), 1100, 700);

    GtkWidget *main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start(main_box, 16);
    gtk_widget_set_margin_end(main_box, 16);
    gtk_widget_set_margin_top(main_box, 16);
    gtk_widget_set_margin_bottom(main_box, 16);

    // Search controls
    GtkWidget *search_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

    GtkWidget *search_label = gtk_label_new("Filename:");
    gtk_widget_set_size_request(search_label, 80, -1);
    gtk_label_set_xalign(GTK_LABEL(search_label), 0.0);

    search_entry = gtk_entry_new();
    gtk_widget_set_hexpand(search_entry, TRUE);
    gtk_entry_set_placeholder_text(GTK_ENTRY(search_entry), "Enter filename to search...");
    g_signal_connect(search_entry, "activate", G_CALLBACK(on_search_entry_activate), NULL);

    partial_check = gtk_check_button_new_with_label("Partial Match");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(partial_check), FALSE);

    gtk_box_append(GTK_BOX(search_box), search_label);
    gtk_box_append(GTK_BOX(search_box), search_entry);
    gtk_box_append(GTK_BOX(search_box), partial_check);

    gtk_box_append(GTK_BOX(main_box), search_box);

    // Database selection and action buttons
    GtkWidget *db_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);

    GtkWidget *db_label = gtk_label_new("Database:");
    gtk_widget_set_size_request(db_label, 80, -1);
    gtk_label_set_xalign(GTK_LABEL(db_label), 0.0);

    db_combo = gtk_combo_box_text_new();
    gtk_widget_set_hexpand(db_combo, TRUE);

    search_button = gtk_button_new_with_label("Search");
    gtk_widget_add_css_class(search_button, "suggested-action");
    gtk_widget_set_size_request(search_button, 100, -1);
    g_signal_connect(search_button, "clicked", G_CALLBACK(on_search_clicked), NULL);

    clear_button = gtk_button_new_with_label("Clear");
    gtk_widget_set_size_request(clear_button, 100, -1);
    g_signal_connect(clear_button, "clicked", G_CALLBACK(on_clear_clicked), NULL);

    gtk_box_append(GTK_BOX(db_box), db_label);
    gtk_box_append(GTK_BOX(db_box), db_combo);
    gtk_box_append(GTK_BOX(db_box), search_button);
    gtk_box_append(GTK_BOX(db_box), clear_button);

    gtk_box_append(GTK_BOX(main_box), db_box);

    // Separator
    gtk_box_append(GTK_BOX(main_box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    // Results tree view
    GtkWidget *results_label = gtk_label_new("Search Results");
    gtk_widget_add_css_class(results_label, "heading");
    gtk_label_set_xalign(GTK_LABEL(results_label), 0.0);
    gtk_box_append(GTK_BOX(main_box), results_label);

    // Create tree view with columns
    GtkListStore *store = gtk_list_store_new(7,
                                             G_TYPE_STRING, // Database
                                             G_TYPE_STRING, // Full Path
                                             G_TYPE_STRING, // Size
                                             G_TYPE_STRING, // Modified
                                             G_TYPE_STRING, // Owner
                                             G_TYPE_STRING, // Checksum
                                             G_TYPE_STRING);// Match Status

    results_tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
    g_object_unref(store);

    // Add columns
    const char *titles[] = {"Database", "Full Path", "Size", "Modified", "Owner", "Checksum", "Status"};
    int widths[] = {150, 400, 80, 150, 100, 180, 80};

    for (int i = 0; i < 7; i++) {
        GtkCellRenderer *renderer = gtk_cell_renderer_text_new();
        GtkTreeViewColumn *column = gtk_tree_view_column_new_with_attributes(
            titles[i], renderer, "text", i, NULL);
        gtk_tree_view_column_set_resizable(column, TRUE);
        gtk_tree_view_column_set_min_width(column, widths[i]);
        if (i == 1) { // Full Path column
            gtk_tree_view_column_set_expand(column, TRUE);
        }
        if (i == 2 || i == 6) { // Size and Status columns - center align
            g_object_set(renderer, "xalign", 0.5, NULL);
        }
        gtk_tree_view_append_column(GTK_TREE_VIEW(results_tree), column);
    }

    g_signal_connect(results_tree, "row-activated", G_CALLBACK(on_row_activated), NULL);

    GtkWidget *results_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(results_scroll), results_tree);
    gtk_widget_set_vexpand(results_scroll, TRUE);
    gtk_box_append(GTK_BOX(main_box), results_scroll);

    // Status bar
    status_label = gtk_label_new("Ready");
    gtk_label_set_xalign(GTK_LABEL(status_label), 0.0);
    gtk_widget_add_css_class(status_label, "dim-label");
    gtk_box_append(GTK_BOX(main_box), status_label);

    gtk_window_set_child(GTK_WINDOW(window), main_box);
    gtk_window_present(GTK_WINDOW(window));

    // Load database list
    refresh_databases_list();
}

int main(int argc, char *argv[]) {
    GtkApplication *app = gtk_application_new("com.filetracker.locator", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);

    int status = g_application_run(G_APPLICATION(app), argc, argv);

    g_object_unref(app);
    return status;
}

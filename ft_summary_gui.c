#include <gtk/gtk.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <locale.h>

#define MAX_PATH 4096

// Global UI elements
GtkWidget *window;
GtkWidget *db_combo;
GtkWidget *refresh_button;
GtkWidget *all_runs_check;
GtkWidget *runs_tree;
GtkWidget *details_text;
GtkWidget *missing_text;
GtkWidget *changed_text;
GtkWidget *new_text;
GtkWidget *notebook;

// Data
char db_dir[MAX_PATH];
char current_db[256] = "";
int current_run_id = -1;

// ==== Utility Functions ====

void get_db_list(GListStore *store) {
    const char *home = getenv("HOME");
    if (!home) return;

    snprintf(db_dir, sizeof(db_dir), "%s/db/FileTracker", home);

    DIR *dir = opendir(db_dir);
    if (!dir) return;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strstr(entry->d_name, ".db") && strcmp(entry->d_name, "drives.db") != 0) {
            // Remove .db extension
            char name[256];
            strncpy(name, entry->d_name, sizeof(name) - 1);
            char *dot = strrchr(name, '.');
            if (dot) *dot = '\0';

            GtkStringObject *obj = gtk_string_object_new(name);
            g_list_store_append(store, obj);
            g_object_unref(obj);
        }
    }

    closedir(dir);
}

// ==== Display Functions ====

void load_run_history(const char *db_name) {
    char db_path[MAX_PATH];
    snprintf(db_path, sizeof(db_path), "%s/%s.db", db_dir, db_name);

    sqlite3 *db;
    if (sqlite3_open(db_path, &db) != SQLITE_OK) {
        GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(details_text));
        gtk_text_buffer_set_text(buffer, "Error: Could not open database", -1);
        return;
    }

    // Clear existing runs
    GtkTreeView *tree = GTK_TREE_VIEW(runs_tree);
    GtkListStore *store = GTK_LIST_STORE(gtk_tree_view_get_model(tree));
    gtk_list_store_clear(store);

    const char *sql;
    int all_runs = gtk_check_button_get_active(GTK_CHECK_BUTTON(all_runs_check));

    if (all_runs) {
        sql = "SELECT id, last_checksum_verify_date, last_date_verify, num_unchanged, "
              "num_changed, num_new, num_missing, num_errors, update_mode "
              "FROM meta ORDER BY id DESC;";
    } else {
        sql = "SELECT id, last_checksum_verify_date, last_date_verify, num_unchanged, "
              "num_changed, num_new, num_missing, num_errors, update_mode "
              "FROM meta ORDER BY id DESC LIMIT 1;";
    }

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, 0) == SQLITE_OK) {
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            int id = sqlite3_column_int(stmt, 0);
            const char *checksum_date = (const char *)sqlite3_column_text(stmt, 1);
            const char *verify_date = (const char *)sqlite3_column_text(stmt, 2);
            int unchanged = sqlite3_column_int(stmt, 3);
            int changed = sqlite3_column_int(stmt, 4);
            int new_files = sqlite3_column_int(stmt, 5);
            int missing = sqlite3_column_int(stmt, 6);
            int errors = sqlite3_column_int(stmt, 7);
            const char *update_mode = (const char *)sqlite3_column_text(stmt, 8);

            const char *run_date;
            const char *checksum_status;
            if (checksum_date && strlen(checksum_date) > 0) {
                run_date = checksum_date;
                checksum_status = "On";
            } else {
                run_date = verify_date && strlen(verify_date) > 0 ? verify_date : "unknown";
                checksum_status = "Off";
            }

            const char *update_status = "Off";
            if (update_mode && strcmp(update_mode, "ON") == 0) {
                update_status = "On";
            }

            GtkTreeIter iter;
            gtk_list_store_append(store, &iter);
            gtk_list_store_set(store, &iter,
                              0, id,
                              1, run_date,
                              2, update_status,
                              3, checksum_status,
                              4, unchanged,
                              5, changed,
                              6, new_files,
                              7, missing,
                              8, errors,
                              -1);
        }
        sqlite3_finalize(stmt);
    }

    sqlite3_close(db);

    // Auto-select first run if available
    GtkTreeIter first_iter;
    if (gtk_tree_model_get_iter_first(GTK_TREE_MODEL(store), &first_iter)) {
        GtkTreeSelection *selection = gtk_tree_view_get_selection(tree);
        gtk_tree_selection_select_iter(selection, &first_iter);
    }
}

void load_file_list(const char *db_name, int run_id, const char *status_filter, GtkTextView *text_view) {
    char db_path[MAX_PATH];
    snprintf(db_path, sizeof(db_path), "%s/%s.db", db_dir, db_name);

    sqlite3 *db;
    if (sqlite3_open(db_path, &db) != SQLITE_OK) {
        GtkTextBuffer *buffer = gtk_text_view_get_buffer(text_view);
        gtk_text_buffer_set_text(buffer, "Error: Could not open database", -1);
        return;
    }

    char sql[512];
    if (status_filter) {
        snprintf(sql, sizeof(sql),
                 "SELECT full_path FROM run_logs WHERE run_id = ? AND status LIKE '%%%s%%' ORDER BY full_path;",
                 status_filter);
    } else {
        snprintf(sql, sizeof(sql),
                 "SELECT full_path, status FROM run_logs WHERE run_id = ? ORDER BY full_path;");
    }

    sqlite3_stmt *stmt;
    GString *result = g_string_new("");

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, 0) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, run_id);

        int count = 0;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *path = (const char *)sqlite3_column_text(stmt, 0);
            if (status_filter) {
                g_string_append_printf(result, "%s\n", path);
            } else {
                const char *status = (const char *)sqlite3_column_text(stmt, 1);
                g_string_append_printf(result, "[%s] %s\n", status, path);
            }
            count++;
        }

        if (count == 0) {
            g_string_append(result, "(No files found)");
        } else {
            char header[128];
            snprintf(header, sizeof(header), "Total: %'d files\n\n", count);
            g_string_prepend(result, header);
        }

        sqlite3_finalize(stmt);
    }

    GtkTextBuffer *buffer = gtk_text_view_get_buffer(text_view);
    gtk_text_buffer_set_text(buffer, result->str, -1);

    g_string_free(result, TRUE);
    sqlite3_close(db);
}

void load_run_details(const char *db_name, int run_id) {
    char db_path[MAX_PATH];
    snprintf(db_path, sizeof(db_path), "%s/%s.db", db_dir, db_name);

    sqlite3 *db;
    if (sqlite3_open(db_path, &db) != SQLITE_OK) {
        GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(details_text));
        gtk_text_buffer_set_text(buffer, "Error: Could not open database", -1);
        return;
    }

    const char *sql = "SELECT * FROM meta WHERE id = ?;";
    sqlite3_stmt *stmt;

    GString *details = g_string_new("");

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, 0) == SQLITE_OK) {
        sqlite3_bind_int(stmt, 1, run_id);

        if (sqlite3_step(stmt) == SQLITE_ROW) {
            int id = sqlite3_column_int(stmt, 0);
            const char *checksum_date = (const char *)sqlite3_column_text(stmt, 1);
            const char *verify_date = (const char *)sqlite3_column_text(stmt, 2);
            const char *machine = (const char *)sqlite3_column_text(stmt, 3);
            int unchanged = sqlite3_column_int(stmt, 4);
            int changed = sqlite3_column_int(stmt, 5);
            int new_files = sqlite3_column_int(stmt, 6);
            int missing = sqlite3_column_int(stmt, 7);
            int errors = sqlite3_column_int(stmt, 8);
            const char *update_mode = (const char *)sqlite3_column_text(stmt, 9);
            const char *note = (const char *)sqlite3_column_text(stmt, 10);

            g_string_append_printf(details, "Run #%d Details\n", id);
            g_string_append(details, "═══════════════════════════════════════\n\n");

            g_string_append_printf(details, "Database:       %s\n", db_name);
            g_string_append_printf(details, "Machine:        %s\n", machine ? machine : "(unknown)");

            if (checksum_date && strlen(checksum_date) > 0) {
                g_string_append_printf(details, "Run Date:       %s\n", checksum_date);
                g_string_append(details, "Checksum:       Enabled\n");
            } else if (verify_date && strlen(verify_date) > 0) {
                g_string_append_printf(details, "Run Date:       %s\n", verify_date);
                g_string_append(details, "Checksum:       Disabled\n");
            }

            g_string_append_printf(details, "Update Mode:    %s\n",
                                  update_mode && strcmp(update_mode, "ON") == 0 ? "On" : "Off");

            g_string_append(details, "\nFile Statistics:\n");
            g_string_append(details, "───────────────────────────────────────\n");
            g_string_append_printf(details, "  Unchanged:    %'10d files\n", unchanged);
            g_string_append_printf(details, "  Changed:      %'10d files\n", changed);
            g_string_append_printf(details, "  New:          %'10d files\n", new_files);
            g_string_append_printf(details, "  Missing:      %'10d files\n", missing);
            g_string_append_printf(details, "  Errors:       %'10d files\n", errors);
            g_string_append(details, "───────────────────────────────────────\n");
            g_string_append_printf(details, "  Total:        %'10d files\n",
                                  unchanged + changed + new_files + missing + errors);

            if (note && strlen(note) > 0) {
                g_string_append(details, "\nRun Note:\n");
                g_string_append(details, "───────────────────────────────────────\n");
                g_string_append_printf(details, "%s\n", note);
            }
        }

        sqlite3_finalize(stmt);
    }

    GtkTextBuffer *buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(details_text));
    gtk_text_buffer_set_text(buffer, details->str, -1);

    g_string_free(details, TRUE);
    sqlite3_close(db);

    // Load file lists
    load_file_list(db_name, run_id, "MISSING", GTK_TEXT_VIEW(missing_text));
    load_file_list(db_name, run_id, "CHANGED", GTK_TEXT_VIEW(changed_text));
    load_file_list(db_name, run_id, "NEW", GTK_TEXT_VIEW(new_text));
}

// ==== Callbacks ====

void on_db_selected(GtkDropDown *dropdown, GParamSpec *pspec, gpointer user_data) {
    (void)pspec;
    (void)user_data;

    guint position = gtk_drop_down_get_selected(dropdown);
    if (position == GTK_INVALID_LIST_POSITION) return;

    GtkStringObject *obj = gtk_drop_down_get_selected_item(dropdown);
    const char *db_name = gtk_string_object_get_string(obj);

    strncpy(current_db, db_name, sizeof(current_db) - 1);
    current_run_id = -1;

    load_run_history(db_name);
}

void on_run_selected(GtkTreeSelection *selection, gpointer user_data) {
    (void)user_data;

    GtkTreeModel *model;
    GtkTreeIter iter;

    if (gtk_tree_selection_get_selected(selection, &model, &iter)) {
        int run_id;
        gtk_tree_model_get(model, &iter, 0, &run_id, -1);

        current_run_id = run_id;

        if (strlen(current_db) > 0) {
            load_run_details(current_db, run_id);
        }
    }
}

void on_refresh_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;

    if (strlen(current_db) > 0) {
        load_run_history(current_db);
    }
}

void on_all_runs_toggled(GtkCheckButton *check, gpointer user_data) {
    (void)check;
    (void)user_data;

    if (strlen(current_db) > 0) {
        load_run_history(current_db);
    }
}

void on_export_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;

    if (strlen(current_db) == 0 || current_run_id < 0) {
        GtkAlertDialog *alert = gtk_alert_dialog_new("Please select a database and run first");
        gtk_alert_dialog_show(alert, GTK_WINDOW(window));
        g_object_unref(alert);
        return;
    }

    // Get current page from notebook
    int page = gtk_notebook_get_current_page(GTK_NOTEBOOK(notebook));
    GtkTextView *text_view;
    const char *filename_suffix;

    switch (page) {
        case 0: // Details
            text_view = GTK_TEXT_VIEW(details_text);
            filename_suffix = "details";
            break;
        case 1: // Missing
            text_view = GTK_TEXT_VIEW(missing_text);
            filename_suffix = "missing";
            break;
        case 2: // Changed
            text_view = GTK_TEXT_VIEW(changed_text);
            filename_suffix = "changed";
            break;
        case 3: // New
            text_view = GTK_TEXT_VIEW(new_text);
            filename_suffix = "new";
            break;
        default:
            return;
    }

    // Get text content
    GtkTextBuffer *buffer = gtk_text_view_get_buffer(text_view);
    GtkTextIter start, end;
    gtk_text_buffer_get_bounds(buffer, &start, &end);
    char *content = gtk_text_buffer_get_text(buffer, &start, &end, FALSE);

    // Generate filename
    char filename[512];
    snprintf(filename, sizeof(filename), "%s-run%d-%s.txt", current_db, current_run_id, filename_suffix);

    // Write to file
    FILE *f = fopen(filename, "w");
    if (f) {
        fprintf(f, "%s", content);
        fclose(f);

        char msg[1024];
        snprintf(msg, sizeof(msg), "Exported to: %s", filename);
        GtkAlertDialog *alert = gtk_alert_dialog_new(msg);
        gtk_alert_dialog_show(alert, GTK_WINDOW(window));
        g_object_unref(alert);
    } else {
        GtkAlertDialog *alert = gtk_alert_dialog_new("Error: Could not write file");
        gtk_alert_dialog_show(alert, GTK_WINDOW(window));
        g_object_unref(alert);
    }

    g_free(content);
}

// ==== Main Window Setup ====

void activate(GtkApplication *app, gpointer user_data) {
    (void)user_data;

    window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window), "File Tracker Summary");
    gtk_window_set_default_size(GTK_WINDOW(window), 1100, 750);

    GtkWidget *main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start(main_box, 16);
    gtk_widget_set_margin_end(main_box, 16);
    gtk_widget_set_margin_top(main_box, 16);
    gtk_widget_set_margin_bottom(main_box, 16);

    // Header with database selection
    GtkWidget *header_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);

    GtkWidget *db_label = gtk_label_new("Database:");
    gtk_widget_set_size_request(db_label, 80, -1);

    // Database dropdown
    GListStore *db_store = g_list_store_new(GTK_TYPE_STRING_OBJECT);
    get_db_list(db_store);

    db_combo = gtk_drop_down_new(G_LIST_MODEL(db_store), NULL);
    gtk_widget_set_hexpand(db_combo, TRUE);
    g_signal_connect(db_combo, "notify::selected", G_CALLBACK(on_db_selected), NULL);

    // All runs checkbox
    all_runs_check = gtk_check_button_new_with_label("Show All Runs");
    g_signal_connect(all_runs_check, "toggled", G_CALLBACK(on_all_runs_toggled), NULL);

    // Refresh button
    refresh_button = gtk_button_new_with_label("Refresh");
    g_signal_connect(refresh_button, "clicked", G_CALLBACK(on_refresh_clicked), NULL);

    // Export button
    GtkWidget *export_button = gtk_button_new_with_label("Export");
    g_signal_connect(export_button, "clicked", G_CALLBACK(on_export_clicked), NULL);

    gtk_box_append(GTK_BOX(header_box), db_label);
    gtk_box_append(GTK_BOX(header_box), db_combo);
    gtk_box_append(GTK_BOX(header_box), all_runs_check);
    gtk_box_append(GTK_BOX(header_box), refresh_button);
    gtk_box_append(GTK_BOX(header_box), export_button);

    gtk_box_append(GTK_BOX(main_box), header_box);

    // Separator
    gtk_box_append(GTK_BOX(main_box), gtk_separator_new(GTK_ORIENTATION_HORIZONTAL));

    // Main paned - runs list on left, details on right
    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_vexpand(paned, TRUE);

    // Left: Run history tree
    GtkWidget *left_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_size_request(left_box, 700, -1);

    GtkWidget *runs_label = gtk_label_new("Run History");
    gtk_widget_add_css_class(runs_label, "heading");
    gtk_label_set_xalign(GTK_LABEL(runs_label), 0.0);
    gtk_box_append(GTK_BOX(left_box), runs_label);

    // Create tree view
    GtkListStore *store = gtk_list_store_new(9,
                                             G_TYPE_INT,    // Run #
                                             G_TYPE_STRING, // Date
                                             G_TYPE_STRING, // Update
                                             G_TYPE_STRING, // Checksum
                                             G_TYPE_INT,    // Unchanged
                                             G_TYPE_INT,    // Changed
                                             G_TYPE_INT,    // New
                                             G_TYPE_INT,    // Missing
                                             G_TYPE_INT);   // Errors

    runs_tree = gtk_tree_view_new_with_model(GTK_TREE_MODEL(store));
    g_object_unref(store);

    // Add columns
    GtkCellRenderer *renderer;
    GtkTreeViewColumn *column;

    const char *titles[] = {"Run #", "Date", "Update", "Checksum", "Unchanged", "Changed", "New", "Missing", "Errors"};
    for (int i = 0; i < 9; i++) {
        renderer = gtk_cell_renderer_text_new();
        column = gtk_tree_view_column_new_with_attributes(titles[i], renderer, "text", i, NULL);
        gtk_tree_view_column_set_resizable(column, TRUE);
        if (i >= 4) { // Numeric columns
            g_object_set(renderer, "xalign", 1.0, NULL);
        }
        gtk_tree_view_append_column(GTK_TREE_VIEW(runs_tree), column);
    }

    GtkTreeSelection *selection = gtk_tree_view_get_selection(GTK_TREE_VIEW(runs_tree));
    gtk_tree_selection_set_mode(selection, GTK_SELECTION_SINGLE);
    g_signal_connect(selection, "changed", G_CALLBACK(on_run_selected), NULL);

    GtkWidget *runs_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(runs_scroll), runs_tree);
    gtk_widget_set_vexpand(runs_scroll, TRUE);
    gtk_box_append(GTK_BOX(left_box), runs_scroll);

    gtk_paned_set_start_child(GTK_PANED(paned), left_box);

    // Right: Details notebook
    notebook = gtk_notebook_new();
    gtk_widget_set_size_request(notebook, 400, -1);

    // Details tab
    GtkWidget *details_scroll = gtk_scrolled_window_new();
    details_text = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(details_text), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(details_text), TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(details_scroll), details_text);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), details_scroll, gtk_label_new("Details"));

    // Missing files tab
    GtkWidget *missing_scroll = gtk_scrolled_window_new();
    missing_text = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(missing_text), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(missing_text), TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(missing_scroll), missing_text);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), missing_scroll, gtk_label_new("Missing Files"));

    // Changed files tab
    GtkWidget *changed_scroll = gtk_scrolled_window_new();
    changed_text = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(changed_text), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(changed_text), TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(changed_scroll), changed_text);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), changed_scroll, gtk_label_new("Changed Files"));

    // New files tab
    GtkWidget *new_scroll = gtk_scrolled_window_new();
    new_text = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(new_text), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(new_text), TRUE);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(new_scroll), new_text);
    gtk_notebook_append_page(GTK_NOTEBOOK(notebook), new_scroll, gtk_label_new("New Files"));

    gtk_paned_set_end_child(GTK_PANED(paned), notebook);
    gtk_paned_set_position(GTK_PANED(paned), 700);

    gtk_box_append(GTK_BOX(main_box), paned);

    gtk_window_set_child(GTK_WINDOW(window), main_box);
    gtk_window_present(GTK_WINDOW(window));

    // Auto-select first database
    if (g_list_model_get_n_items(G_LIST_MODEL(db_store)) > 0) {
        gtk_drop_down_set_selected(GTK_DROP_DOWN(db_combo), 0);
    }
}

int main(int argc, char *argv[]) {
    setlocale(LC_NUMERIC, "");

    GtkApplication *app = gtk_application_new("com.filetracker.summary", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);

    int status = g_application_run(G_APPLICATION(app), argc, argv);

    g_object_unref(app);
    return status;
}

#include <gtk/gtk.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>

#define MAX_PATH 4096

// Global UI elements
GtkWidget *window;
GtkWidget *db_combo;
GtkWidget *runs_list;
GtkWidget *run_info_text;
GtkWidget *note_text;
GtkWidget *logs_text;
GtkWidget *filter_new_check;
GtkWidget *filter_changed_check;
GtkWidget *filter_missing_check;
GtkWidget *filter_unchanged_check;
GtkWidget *filter_all_check;
GtkWidget *show_note_check;
GtkWidget *refresh_button;
GtkWidget *status_label;

// Current state
char current_db_path[MAX_PATH] = "";
sqlite3_int64 selected_run_id = 0;

// ==== Database Functions ====

void refresh_databases_list(void) {
    const char *home = getenv("HOME");
    if (!home) return;

    char db_dir[MAX_PATH];
    snprintf(db_dir, sizeof(db_dir), "%s/db/FileTracker", home);

    // Clear existing entries
    gtk_combo_box_text_remove_all(GTK_COMBO_BOX_TEXT(db_combo));

    DIR *dir = opendir(db_dir);
    if (!dir) return;

    struct dirent *entry;
    int count = 0;
    while ((entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;

        size_t len = strlen(entry->d_name);
        if (len > 3 && strcmp(entry->d_name + len - 3, ".db") == 0) {
            // Remove .db extension for display
            char db_name[256];
            strncpy(db_name, entry->d_name, len - 3);
            db_name[len - 3] = '\0';

            gtk_combo_box_text_append_text(GTK_COMBO_BOX_TEXT(db_combo), db_name);
            count++;
        }
    }
    closedir(dir);

    if (count > 0) {
        gtk_combo_box_set_active(GTK_COMBO_BOX(db_combo), 0);
    }

    char status[256];
    snprintf(status, sizeof(status), "Found %d database(s)", count);
    gtk_label_set_text(GTK_LABEL(status_label), status);
}

void format_run_identifier(char *buffer, size_t size, const char *db_name, const char *run_date, sqlite3_int64 id) {
    if (run_date) {
        char formatted_date[64];
        strncpy(formatted_date, run_date, sizeof(formatted_date) - 1);
        formatted_date[sizeof(formatted_date) - 1] = '\0';

        // Replace spaces and colons with hyphens
        for (char *p = formatted_date; *p; p++) {
            if (*p == ' ' || *p == ':') *p = '-';
        }
        snprintf(buffer, size, "%s-%s", db_name, formatted_date);
    } else {
        snprintf(buffer, size, "%s-unknown-%lld", db_name, id);
    }
}

void load_runs_list(const char *db_name) {
    if (!db_name || strlen(db_name) == 0) return;

    const char *home = getenv("HOME");
    snprintf(current_db_path, sizeof(current_db_path), "%s/db/FileTracker/%s.db", home, db_name);

    // Clear runs list
    GtkListBox *list = GTK_LIST_BOX(runs_list);
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(GTK_WIDGET(list))) != NULL) {
        gtk_list_box_remove(list, child);
    }

    // Clear details
    GtkTextBuffer *info_buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(run_info_text));
    gtk_text_buffer_set_text(info_buffer, "", -1);
    GtkTextBuffer *note_buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(note_text));
    gtk_text_buffer_set_text(note_buffer, "", -1);
    GtkTextBuffer *logs_buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(logs_text));
    gtk_text_buffer_set_text(logs_buffer, "", -1);

    // Check if database exists
    if (access(current_db_path, F_OK) != 0) {
        gtk_label_set_text(GTK_LABEL(status_label), "Database not found");
        return;
    }

    sqlite3 *db;
    if (sqlite3_open(current_db_path, &db) != SQLITE_OK) {
        gtk_label_set_text(GTK_LABEL(status_label), "Failed to open database");
        return;
    }

    const char *query =
        "SELECT id, "
        "COALESCE(last_checksum_verify_date, last_date_verify) as run_date, "
        "verify_machine, num_unchanged, num_changed, num_new, num_missing, "
        "update_mode, note "
        "FROM meta ORDER BY id DESC";

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, query, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        gtk_label_set_text(GTK_LABEL(status_label), "Database query failed");
        return;
    }

    int count = 0;
    while (sqlite3_step(stmt) == SQLITE_ROW) {
        sqlite3_int64 id = sqlite3_column_int64(stmt, 0);
        const char *run_date = (const char *)sqlite3_column_text(stmt, 1);
        const char *machine = (const char *)sqlite3_column_text(stmt, 2);
        int unchanged = sqlite3_column_int(stmt, 3);
        int changed = sqlite3_column_int(stmt, 4);
        int new = sqlite3_column_int(stmt, 5);
        int missing = sqlite3_column_int(stmt, 6);
        const char *update_mode = (const char *)sqlite3_column_text(stmt, 7);
        const char *note = (const char *)sqlite3_column_text(stmt, 8);

        char run_id[128];
        format_run_identifier(run_id, sizeof(run_id), db_name, run_date, id);

        // Create list item
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_VERTICAL, 2);
        gtk_widget_set_margin_start(row, 8);
        gtk_widget_set_margin_end(row, 8);
        gtk_widget_set_margin_top(row, 6);
        gtk_widget_set_margin_bottom(row, 6);

        // Run date
        GtkWidget *date_label = gtk_label_new(run_date ? run_date : "Unknown");
        gtk_label_set_xalign(GTK_LABEL(date_label), 0.0);
        gtk_widget_add_css_class(date_label, "heading");
        gtk_box_append(GTK_BOX(row), date_label);

        // Stats
        char stats[256];
        snprintf(stats, sizeof(stats), "U:%d C:%d N:%d M:%d on %s%s%s",
                unchanged, changed, new, missing,
                machine ? machine : "N/A",
                update_mode && strcmp(update_mode, "OFF") == 0 ? " [RO]" : "",
                note && strlen(note) > 0 ? " 📝" : "");

        GtkWidget *stats_label = gtk_label_new(stats);
        gtk_label_set_xalign(GTK_LABEL(stats_label), 0.0);
        gtk_widget_add_css_class(stats_label, "dim-label");
        gtk_box_append(GTK_BOX(row), stats_label);

        // Store run_id as data
        g_object_set_data(G_OBJECT(row), "run_id", GINT_TO_POINTER((gint)id));

        gtk_list_box_append(list, row);
        count++;
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);

    char status[256];
    snprintf(status, sizeof(status), "Loaded %d run(s) from %s", count, db_name);
    gtk_label_set_text(GTK_LABEL(status_label), status);
}

void load_run_details(sqlite3_int64 run_id) {
    if (strlen(current_db_path) == 0 || run_id == 0) return;

    selected_run_id = run_id;

    sqlite3 *db;
    if (sqlite3_open(current_db_path, &db) != SQLITE_OK) {
        return;
    }

    // Get run information
    const char *query =
        "SELECT id, verify_machine, num_unchanged, num_changed, num_new, num_missing, "
        "COALESCE(last_checksum_verify_date, last_date_verify) as run_date, "
        "update_mode, note "
        "FROM meta WHERE id = ? LIMIT 1";

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, query, -1, &stmt, NULL) != SQLITE_OK) {
        sqlite3_close(db);
        return;
    }

    sqlite3_bind_int64(stmt, 1, run_id);

    if (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *machine = (const char *)sqlite3_column_text(stmt, 1);
        int unchanged = sqlite3_column_int(stmt, 2);
        int changed = sqlite3_column_int(stmt, 3);
        int new = sqlite3_column_int(stmt, 4);
        int missing = sqlite3_column_int(stmt, 5);
        const char *run_date = (const char *)sqlite3_column_text(stmt, 6);
        const char *update_mode = (const char *)sqlite3_column_text(stmt, 7);
        const char *note = (const char *)sqlite3_column_text(stmt, 8);

        // Format run info
        char info[1024];
        snprintf(info, sizeof(info),
                 "Run ID         : %lld\n"
                 "Date/Time      : %s\n"
                 "Machine        : %s\n"
                 "Update Mode    : %s\n"
                 "Unchanged      : %d\n"
                 "Changed        : %d\n"
                 "New            : %d\n"
                 "Missing        : %d\n"
                 "Total          : %d",
                 run_id,
                 run_date ? run_date : "Unknown",
                 machine ? machine : "N/A",
                 update_mode ? update_mode : "N/A",
                 unchanged, changed, new, missing,
                 unchanged + changed + new + missing);

        GtkTextBuffer *info_buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(run_info_text));
        gtk_text_buffer_set_text(info_buffer, info, -1);

        // Set note
        GtkTextBuffer *note_buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(note_text));
        if (note && strlen(note) > 0) {
            gtk_text_buffer_set_text(note_buffer, note, -1);
        } else {
            gtk_text_buffer_set_text(note_buffer, "(No note recorded for this run)", -1);
        }
    }

    sqlite3_finalize(stmt);
    sqlite3_close(db);

    // Trigger log reload
    g_signal_emit_by_name(filter_all_check, "toggled");
}

void load_logs(void) {
    if (strlen(current_db_path) == 0 || selected_run_id == 0) return;

    sqlite3 *db;
    if (sqlite3_open(current_db_path, &db) != SQLITE_OK) {
        return;
    }

    // Build status filter
    char status_filter[512] = "";
    int any_filter = 0;

    if (gtk_check_button_get_active(GTK_CHECK_BUTTON(filter_all_check))) {
        strcpy(status_filter, "1=1");
        any_filter = 1;
    } else {
        int first = 1;
        if (gtk_check_button_get_active(GTK_CHECK_BUTTON(filter_new_check))) {
            strcat(status_filter, "status = 'NEW'");
            first = 0;
            any_filter = 1;
        }
        if (gtk_check_button_get_active(GTK_CHECK_BUTTON(filter_changed_check))) {
            if (!first) strcat(status_filter, " OR ");
            strcat(status_filter, "status LIKE 'CHANGED%'");
            first = 0;
            any_filter = 1;
        }
        if (gtk_check_button_get_active(GTK_CHECK_BUTTON(filter_missing_check))) {
            if (!first) strcat(status_filter, " OR ");
            strcat(status_filter, "status = 'MISSING'");
            first = 0;
            any_filter = 1;
        }
        if (gtk_check_button_get_active(GTK_CHECK_BUTTON(filter_unchanged_check))) {
            if (!first) strcat(status_filter, " OR ");
            strcat(status_filter, "status = 'UNCHANGED'");
            any_filter = 1;
        }
    }

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

    sqlite3_bind_int64(stmt, 1, selected_run_id);

    // Build logs text
    GString *logs = g_string_new("");
    int count = 0;

    while (sqlite3_step(stmt) == SQLITE_ROW) {
        const char *status = (const char *)sqlite3_column_text(stmt, 0);
        const char *path = (const char *)sqlite3_column_text(stmt, 1);
        g_string_append_printf(logs, "[%-18s] %s\n", status, path);
        count++;
    }

    if (count == 0) {
        g_string_assign(logs, "No log messages found matching the selected filters.");
    } else {
        char footer[128];
        snprintf(footer, sizeof(footer), "\n--- Total messages: %d ---", count);
        g_string_append(logs, footer);
    }

    GtkTextBuffer *logs_buffer = gtk_text_view_get_buffer(GTK_TEXT_VIEW(logs_text));
    gtk_text_buffer_set_text(logs_buffer, logs->str, -1);

    g_string_free(logs, TRUE);
    sqlite3_finalize(stmt);
    sqlite3_close(db);

    char status[256];
    snprintf(status, sizeof(status), "Showing %d log message(s)", count);
    gtk_label_set_text(GTK_LABEL(status_label), status);
}

// ==== GUI Callbacks ====

void on_database_changed(GtkComboBox *combo, gpointer user_data) {
    (void)user_data;

    char *db_name = gtk_combo_box_text_get_active_text(GTK_COMBO_BOX_TEXT(combo));
    if (db_name) {
        load_runs_list(db_name);
        g_free(db_name);
    }
}

void on_run_selected(GtkListBox *box, GtkListBoxRow *row, gpointer user_data) {
    (void)box;
    (void)user_data;

    if (!row) return;

    GtkWidget *row_widget = gtk_list_box_row_get_child(GTK_LIST_BOX_ROW(row));
    gpointer run_id_ptr = g_object_get_data(G_OBJECT(row_widget), "run_id");
    sqlite3_int64 run_id = (sqlite3_int64)GPOINTER_TO_INT(run_id_ptr);

    load_run_details(run_id);
}

void on_filter_toggled(GtkCheckButton *button, gpointer user_data) {
    (void)user_data;

    // If "All" was checked, uncheck individual filters
    if (button == GTK_CHECK_BUTTON(filter_all_check) &&
        gtk_check_button_get_active(GTK_CHECK_BUTTON(filter_all_check))) {
        gtk_check_button_set_active(GTK_CHECK_BUTTON(filter_new_check), FALSE);
        gtk_check_button_set_active(GTK_CHECK_BUTTON(filter_changed_check), FALSE);
        gtk_check_button_set_active(GTK_CHECK_BUTTON(filter_missing_check), FALSE);
        gtk_check_button_set_active(GTK_CHECK_BUTTON(filter_unchanged_check), FALSE);
    }
    // If any individual filter was checked, uncheck "All"
    else if (button != GTK_CHECK_BUTTON(filter_all_check) &&
             gtk_check_button_get_active(button)) {
        gtk_check_button_set_active(GTK_CHECK_BUTTON(filter_all_check), FALSE);
    }

    load_logs();
}

void on_refresh_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;

    refresh_databases_list();
}

void activate(GtkApplication *app, gpointer user_data) {
    (void)user_data;

    window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window), "File Tracker Logs Viewer");
    gtk_window_set_default_size(GTK_WINDOW(window), 1200, 800);

    GtkWidget *main_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(main_box, 8);
    gtk_widget_set_margin_end(main_box, 8);
    gtk_widget_set_margin_top(main_box, 8);
    gtk_widget_set_margin_bottom(main_box, 8);

    // Database selector
    GtkWidget *db_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *db_label = gtk_label_new("Database:");
    gtk_widget_set_size_request(db_label, 80, -1);
    db_combo = gtk_combo_box_text_new();
    gtk_widget_set_hexpand(db_combo, TRUE);
    g_signal_connect(db_combo, "changed", G_CALLBACK(on_database_changed), NULL);

    refresh_button = gtk_button_new_with_label("Refresh");
    g_signal_connect(refresh_button, "clicked", G_CALLBACK(on_refresh_clicked), NULL);

    gtk_box_append(GTK_BOX(db_box), db_label);
    gtk_box_append(GTK_BOX(db_box), db_combo);
    gtk_box_append(GTK_BOX(db_box), refresh_button);
    gtk_box_append(GTK_BOX(main_box), db_box);

    // Main paned view
    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_widget_set_vexpand(paned, TRUE);

    // Left panel: Runs list
    GtkWidget *left_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_size_request(left_box, 350, -1);

    GtkWidget *runs_label = gtk_label_new("Runs");
    gtk_widget_add_css_class(runs_label, "heading");
    gtk_label_set_xalign(GTK_LABEL(runs_label), 0.0);
    gtk_box_append(GTK_BOX(left_box), runs_label);

    GtkWidget *runs_scroll = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(runs_scroll, TRUE);
    runs_list = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(runs_list), GTK_SELECTION_SINGLE);
    g_signal_connect(runs_list, "row-activated", G_CALLBACK(on_run_selected), NULL);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(runs_scroll), runs_list);
    gtk_box_append(GTK_BOX(left_box), runs_scroll);

    gtk_paned_set_start_child(GTK_PANED(paned), left_box);

    // Right panel: Details
    GtkWidget *right_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 12);
    gtk_widget_set_margin_start(right_box, 8);
    gtk_widget_set_margin_end(right_box, 8);

    // Run info
    GtkWidget *info_label = gtk_label_new("Run Information");
    gtk_widget_add_css_class(info_label, "heading");
    gtk_label_set_xalign(GTK_LABEL(info_label), 0.0);
    gtk_box_append(GTK_BOX(right_box), info_label);

    run_info_text = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(run_info_text), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(run_info_text), TRUE);
    GtkWidget *info_scroll = gtk_scrolled_window_new();
    gtk_widget_set_size_request(info_scroll, -1, 200);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(info_scroll), run_info_text);
    gtk_box_append(GTK_BOX(right_box), info_scroll);

    // Note section
    GtkWidget *note_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    GtkWidget *note_header = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *note_label = gtk_label_new("Run Note");
    gtk_widget_add_css_class(note_label, "heading");
    gtk_label_set_xalign(GTK_LABEL(note_label), 0.0);
    gtk_widget_set_hexpand(note_label, TRUE);

    show_note_check = gtk_check_button_new_with_label("Show Note");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(show_note_check), TRUE);

    gtk_box_append(GTK_BOX(note_header), note_label);
    gtk_box_append(GTK_BOX(note_header), show_note_check);
    gtk_box_append(GTK_BOX(note_box), note_header);

    note_text = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(note_text), FALSE);
    gtk_text_view_set_wrap_mode(GTK_TEXT_VIEW(note_text), GTK_WRAP_WORD);
    gtk_widget_set_size_request(note_text, -1, 60);
    GtkWidget *note_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(note_scroll), note_text);
    gtk_box_append(GTK_BOX(note_box), note_scroll);

    g_object_bind_property(show_note_check, "active", note_scroll, "visible", G_BINDING_SYNC_CREATE);
    gtk_box_append(GTK_BOX(right_box), note_box);

    // Filters
    GtkWidget *filter_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 12);
    GtkWidget *filter_label = gtk_label_new("Filters:");
    gtk_widget_set_size_request(filter_label, 60, -1);

    filter_all_check = gtk_check_button_new_with_label("All");
    gtk_check_button_set_active(GTK_CHECK_BUTTON(filter_all_check), TRUE);
    g_signal_connect(filter_all_check, "toggled", G_CALLBACK(on_filter_toggled), NULL);

    filter_new_check = gtk_check_button_new_with_label("New");
    g_signal_connect(filter_new_check, "toggled", G_CALLBACK(on_filter_toggled), NULL);

    filter_changed_check = gtk_check_button_new_with_label("Changed");
    g_signal_connect(filter_changed_check, "toggled", G_CALLBACK(on_filter_toggled), NULL);

    filter_missing_check = gtk_check_button_new_with_label("Missing");
    g_signal_connect(filter_missing_check, "toggled", G_CALLBACK(on_filter_toggled), NULL);

    filter_unchanged_check = gtk_check_button_new_with_label("Unchanged");
    g_signal_connect(filter_unchanged_check, "toggled", G_CALLBACK(on_filter_toggled), NULL);

    gtk_box_append(GTK_BOX(filter_box), filter_label);
    gtk_box_append(GTK_BOX(filter_box), filter_all_check);
    gtk_box_append(GTK_BOX(filter_box), filter_new_check);
    gtk_box_append(GTK_BOX(filter_box), filter_changed_check);
    gtk_box_append(GTK_BOX(filter_box), filter_missing_check);
    gtk_box_append(GTK_BOX(filter_box), filter_unchanged_check);
    gtk_box_append(GTK_BOX(right_box), filter_box);

    // Logs viewer
    GtkWidget *logs_label = gtk_label_new("Log Messages");
    gtk_widget_add_css_class(logs_label, "heading");
    gtk_label_set_xalign(GTK_LABEL(logs_label), 0.0);
    gtk_box_append(GTK_BOX(right_box), logs_label);

    logs_text = gtk_text_view_new();
    gtk_text_view_set_editable(GTK_TEXT_VIEW(logs_text), FALSE);
    gtk_text_view_set_monospace(GTK_TEXT_VIEW(logs_text), TRUE);
    gtk_widget_set_vexpand(logs_text, TRUE);
    GtkWidget *logs_scroll = gtk_scrolled_window_new();
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(logs_scroll), logs_text);
    gtk_box_append(GTK_BOX(right_box), logs_scroll);

    gtk_paned_set_end_child(GTK_PANED(paned), right_box);
    gtk_paned_set_position(GTK_PANED(paned), 350);

    gtk_box_append(GTK_BOX(main_box), paned);

    // Status bar
    status_label = gtk_label_new("Ready");
    gtk_label_set_xalign(GTK_LABEL(status_label), 0.0);
    gtk_widget_add_css_class(status_label, "dim-label");
    gtk_box_append(GTK_BOX(main_box), status_label);

    gtk_window_set_child(GTK_WINDOW(window), main_box);
    gtk_window_present(GTK_WINDOW(window));

    // Load initial data
    refresh_databases_list();
}

int main(int argc, char *argv[]) {
    GtkApplication *app = gtk_application_new("com.filetracker.logs", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);

    int status = g_application_run(G_APPLICATION(app), argc, argv);

    g_object_unref(app);
    return status;
}

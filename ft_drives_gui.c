#include <gtk/gtk.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <sys/mount.h>

#define MAX_PATH 4096
#define MAX_DESC 1024
#define MAX_CONTAINER 256

// Global database connection
sqlite3 *db = NULL;

// UI Elements
GtkWidget *window;
GtkWidget *drives_list_box;
GtkWidget *search_entry;
GtkWidget *detail_name_label;
GtkWidget *detail_capacity_label;
GtkWidget *detail_used_label;
GtkWidget *detail_available_label;
GtkWidget *detail_percent_label;
GtkWidget *detail_description_label;
GtkWidget *detail_container_label;
GtkWidget *detail_updated_label;
GtkWidget *detail_verified_label;
GtkWidget *detail_checksum_label;
GtkWidget *detail_box;
GtkProgressBar *usage_progress_bar;

// Current selection
char current_drive_name[256] = "";

// ==== Helper Functions ====

void get_timestamp(char *buffer, size_t size) {
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    strftime(buffer, size, "%Y-%m-%d %H:%M:%S", t);
}

void format_bytes(long long bytes, char *buffer, size_t size) {
    const char *units[] = {"B", "KB", "MB", "GB", "TB", "PB"};
    int unit_index = 0;
    double size_val = (double)bytes;

    while (size_val >= 1024.0 && unit_index < 5) {
        size_val /= 1024.0;
        unit_index++;
    }

    if (unit_index == 0) {
        snprintf(buffer, size, "%lld %s", bytes, units[unit_index]);
    } else {
        snprintf(buffer, size, "%.2f %s", size_val, units[unit_index]);
    }
}

int get_drive_stats(const char *mount_path, long long *capacity, long long *available, long long *used) {
    struct statfs fs_stats;

    if (statfs(mount_path, &fs_stats) != 0) {
        return 0;
    }

    *capacity = (long long)fs_stats.f_blocks * fs_stats.f_bsize;
    *available = (long long)fs_stats.f_bavail * fs_stats.f_bsize;
    *used = *capacity - ((long long)fs_stats.f_bfree * fs_stats.f_bsize);

    return 1;
}

int find_mount_point(const char *drive_name, char *mount_path, size_t size) {
    snprintf(mount_path, size, "/Volumes/%s", drive_name);
    struct stat st;
    if (stat(mount_path, &st) == 0) {
        return 1;
    }
    return 0;
}

int init_database(void) {
    const char *home = getenv("HOME");
    if (!home) {
        return 0;
    }

    char db_dir[MAX_PATH];
    snprintf(db_dir, sizeof(db_dir), "%s/db/FileTracker", home);

    char mkdir_cmd[MAX_PATH + 20];
    snprintf(mkdir_cmd, sizeof(mkdir_cmd), "mkdir -p %s", db_dir);
    system(mkdir_cmd);

    char db_path[MAX_PATH];
    snprintf(db_path, sizeof(db_path), "%s/drives.db", db_dir);

    int rc = sqlite3_open(db_path, &db);
    if (rc != SQLITE_OK) {
        return 0;
    }

    const char *sql =
        "CREATE TABLE IF NOT EXISTS drives ("
        "  drive_id INTEGER PRIMARY KEY AUTOINCREMENT,"
        "  drive_name TEXT UNIQUE NOT NULL,"
        "  capacity INTEGER,"
        "  space_available INTEGER,"
        "  space_used INTEGER,"
        "  description TEXT,"
        "  last_updated TEXT,"
        "  last_verified TEXT,"
        "  storage_container TEXT"
        ");";

    char *err_msg = NULL;
    rc = sqlite3_exec(db, sql, 0, 0, &err_msg);
    if (rc != SQLITE_OK) {
        sqlite3_free(err_msg);
        return 0;
    }

    return 1;
}

// ==== Database Operations ====

// Check if drive exists in drives database
int drive_exists(sqlite3 *db, const char *drive_name) {
    const char *sql = "SELECT COUNT(*) FROM drives WHERE drive_name = ?;";
    sqlite3_stmt *stmt;
    int exists = 0;

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, 0) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, drive_name, -1, SQLITE_STATIC);
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            exists = sqlite3_column_int(stmt, 0) > 0;
        }
        sqlite3_finalize(stmt);
    }

    return exists;
}

// Auto-add drive from file_tracker database
void auto_add_drive(sqlite3 *db, const char *drive_name) {
    char timestamp[64];
    get_timestamp(timestamp, sizeof(timestamp));

    // Try to find mount point and get capacity
    char mount_path[MAX_PATH];
    long long capacity = 0, available = 0, used = 0;
    int mounted = 0;

    if (find_mount_point(drive_name, mount_path, sizeof(mount_path))) {
        mounted = get_drive_stats(mount_path, &capacity, &available, &used);
    }

    const char *sql = "INSERT INTO drives (drive_name, capacity, space_available, space_used, "
                      "description, last_updated, storage_container) "
                      "VALUES (?, ?, ?, ?, ?, ?, ?);";

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(db, sql, -1, &stmt, 0) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, drive_name, -1, SQLITE_STATIC);

        if (mounted) {
            sqlite3_bind_int64(stmt, 2, capacity);
            sqlite3_bind_int64(stmt, 3, available);
            sqlite3_bind_int64(stmt, 4, used);
        } else {
            sqlite3_bind_null(stmt, 2);
            sqlite3_bind_null(stmt, 3);
            sqlite3_bind_null(stmt, 4);
        }

        sqlite3_bind_text(stmt, 5, "Auto-discovered from file_tracker database", -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 6, timestamp, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 7, "", -1, SQLITE_STATIC);

        sqlite3_step(stmt);
        sqlite3_finalize(stmt);
    }
}

// Sync drives from file_tracker databases
int sync_from_databases(sqlite3 *db) {
    const char *home = getenv("HOME");
    if (!home) return 0;

    char db_dir[MAX_PATH];
    snprintf(db_dir, sizeof(db_dir), "%s/db/FileTracker", home);

    DIR *dir = opendir(db_dir);
    if (!dir) return 0;

    int added_count = 0;
    struct dirent *entry;

    while ((entry = readdir(dir)) != NULL) {
        // Skip if not a .db file or if it's drives.db itself
        if (!strstr(entry->d_name, ".db")) continue;
        if (strcmp(entry->d_name, "drives.db") == 0) continue;

        // Extract drive name (remove .db extension)
        char drive_name[256];
        strncpy(drive_name, entry->d_name, sizeof(drive_name) - 1);
        char *dot = strrchr(drive_name, '.');
        if (dot) *dot = '\0';

        // Check if this drive already exists in drives database
        if (!drive_exists(db, drive_name)) {
            auto_add_drive(db, drive_name);
            added_count++;
        }
    }

    closedir(dir);
    return added_count;
}

// Query file_tracker database for last checksum date
void get_last_checksum_date(const char *drive_name, char *result, size_t size) {
    const char *home = getenv("HOME");
    if (!home) {
        result[0] = '\0';
        return;
    }

    // Try to find matching file_tracker database
    char db_path[MAX_PATH];
    snprintf(db_path, sizeof(db_path), "%s/db/FileTracker/%s.db", home, drive_name);

    sqlite3 *ft_db = NULL;
    if (sqlite3_open(db_path, &ft_db) != SQLITE_OK) {
        result[0] = '\0';
        if (ft_db) sqlite3_close(ft_db);
        return;
    }

    // Query most recent checksum verification date
    const char *sql = "SELECT last_checksum_verify_date FROM meta WHERE last_checksum_verify_date IS NOT NULL AND last_checksum_verify_date != '' ORDER BY id DESC LIMIT 1;";

    sqlite3_stmt *stmt;
    if (sqlite3_prepare_v2(ft_db, sql, -1, &stmt, 0) == SQLITE_OK) {
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *date = (const char *)sqlite3_column_text(stmt, 0);
            if (date) {
                strncpy(result, date, size - 1);
                result[size - 1] = '\0';
            } else {
                result[0] = '\0';
            }
        } else {
            result[0] = '\0';
        }
        sqlite3_finalize(stmt);
    } else {
        result[0] = '\0';
    }

    sqlite3_close(ft_db);
}

void refresh_drives_list(const char *search_filter) {
    // Clear existing list
    GtkWidget *child;
    while ((child = gtk_widget_get_first_child(drives_list_box)) != NULL) {
        gtk_list_box_remove(GTK_LIST_BOX(drives_list_box), child);
    }

    // Query drives
    char sql[512];
    if (search_filter && strlen(search_filter) > 0) {
        snprintf(sql, sizeof(sql),
                 "SELECT drive_name, capacity, description FROM drives "
                 "WHERE drive_name LIKE '%%%s%%' OR description LIKE '%%%s%%' OR storage_container LIKE '%%%s%%' "
                 "ORDER BY drive_name;",
                 search_filter, search_filter, search_filter);
    } else {
        snprintf(sql, sizeof(sql),
                 "SELECT drive_name, capacity, description FROM drives ORDER BY drive_name;");
    }

    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, 0);
    if (rc != SQLITE_OK) {
        return;
    }

    while ((rc = sqlite3_step(stmt)) == SQLITE_ROW) {
        const char *name = (const char *)sqlite3_column_text(stmt, 0);
        long long capacity = sqlite3_column_int64(stmt, 1);
        const char *desc = (const char *)sqlite3_column_text(stmt, 2);

        // Create row
        GtkWidget *row = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
        gtk_widget_set_margin_start(row, 8);
        gtk_widget_set_margin_end(row, 8);
        gtk_widget_set_margin_top(row, 8);
        gtk_widget_set_margin_bottom(row, 8);

        // Drive name
        GtkWidget *name_label = gtk_label_new(NULL);
        char markup[512];
        snprintf(markup, sizeof(markup), "<b>%s</b>", name);
        gtk_label_set_markup(GTK_LABEL(name_label), markup);
        gtk_label_set_xalign(GTK_LABEL(name_label), 0.0);
        gtk_box_append(GTK_BOX(row), name_label);

        // Info line
        GtkWidget *info_label = gtk_label_new(NULL);
        char info[512];
        if (capacity > 0) {
            char cap_str[64];
            format_bytes(capacity, cap_str, sizeof(cap_str));
            snprintf(info, sizeof(info), "%s - %s", cap_str, desc ? desc : "");
        } else {
            snprintf(info, sizeof(info), "Not mounted - %s", desc ? desc : "");
        }
        gtk_label_set_text(GTK_LABEL(info_label), info);
        gtk_label_set_xalign(GTK_LABEL(info_label), 0.0);
        gtk_widget_add_css_class(info_label, "dim-label");
        gtk_box_append(GTK_BOX(row), info_label);

        // Store drive name as data
        g_object_set_data_full(G_OBJECT(row), "drive_name", g_strdup(name), g_free);

        gtk_list_box_append(GTK_LIST_BOX(drives_list_box), row);
    }

    sqlite3_finalize(stmt);
}

void show_drive_details(const char *drive_name) {
    const char *sql = "SELECT * FROM drives WHERE drive_name = ?;";

    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, 0);
    if (rc != SQLITE_OK) {
        return;
    }

    sqlite3_bind_text(stmt, 1, drive_name, -1, SQLITE_STATIC);

    rc = sqlite3_step(stmt);
    if (rc != SQLITE_ROW) {
        sqlite3_finalize(stmt);
        gtk_widget_set_visible(detail_box, FALSE);
        return;
    }

    // Extract data
    const char *name = (const char *)sqlite3_column_text(stmt, 1);
    long long capacity = sqlite3_column_int64(stmt, 2);
    long long available = sqlite3_column_int64(stmt, 3);
    long long used = sqlite3_column_int64(stmt, 4);
    const char *description = (const char *)sqlite3_column_text(stmt, 5);
    const char *last_updated = (const char *)sqlite3_column_text(stmt, 6);
    const char *last_verified = (const char *)sqlite3_column_text(stmt, 7);
    const char *container = (const char *)sqlite3_column_text(stmt, 8);

    // Update labels
    gtk_label_set_text(GTK_LABEL(detail_name_label), name);

    if (capacity > 0) {
        char cap_str[64], avail_str[64], used_str[64];
        format_bytes(capacity, cap_str, sizeof(cap_str));
        format_bytes(available, avail_str, sizeof(avail_str));
        format_bytes(used, used_str, sizeof(used_str));
        double pct_used = (double)used / capacity;

        char cap_text[256];
        snprintf(cap_text, sizeof(cap_text), "%s (%'lld bytes)", cap_str, capacity);
        gtk_label_set_text(GTK_LABEL(detail_capacity_label), cap_text);
        gtk_label_set_text(GTK_LABEL(detail_used_label), used_str);
        gtk_label_set_text(GTK_LABEL(detail_available_label), avail_str);

        char pct_text[64];
        snprintf(pct_text, sizeof(pct_text), "%.1f%% used", pct_used * 100.0);
        gtk_label_set_text(GTK_LABEL(detail_percent_label), pct_text);

        gtk_progress_bar_set_fraction(usage_progress_bar, pct_used);
    } else {
        gtk_label_set_text(GTK_LABEL(detail_capacity_label), "Not available");
        gtk_label_set_text(GTK_LABEL(detail_used_label), "N/A");
        gtk_label_set_text(GTK_LABEL(detail_available_label), "N/A");
        gtk_label_set_text(GTK_LABEL(detail_percent_label), "Drive not mounted");
        gtk_progress_bar_set_fraction(usage_progress_bar, 0.0);
    }

    gtk_label_set_text(GTK_LABEL(detail_description_label), description ? description : "(none)");
    gtk_label_set_text(GTK_LABEL(detail_container_label), container ? container : "(none)");
    gtk_label_set_text(GTK_LABEL(detail_updated_label), last_updated ? last_updated : "(never)");
    gtk_label_set_text(GTK_LABEL(detail_verified_label), last_verified ? last_verified : "(never)");

    // Query file_tracker database for actual checksum verification
    char ft_checksum_date[256] = "";
    get_last_checksum_date(drive_name, ft_checksum_date, sizeof(ft_checksum_date));

    if (ft_checksum_date[0] != '\0') {
        char checksum_text[300];
        snprintf(checksum_text, sizeof(checksum_text), "%s (from file_tracker)", ft_checksum_date);
        gtk_label_set_text(GTK_LABEL(detail_checksum_label), checksum_text);
    } else {
        gtk_label_set_text(GTK_LABEL(detail_checksum_label), "(no file_tracker database found)");
    }

    gtk_widget_set_visible(detail_box, TRUE);

    sqlite3_finalize(stmt);
}

// ==== Callbacks ====

void on_search_changed(GtkSearchEntry *entry, gpointer user_data) {
    (void)user_data;
    const char *search_text = gtk_editable_get_text(GTK_EDITABLE(entry));
    refresh_drives_list(search_text);
    current_drive_name[0] = '\0';
    gtk_widget_set_visible(detail_box, FALSE);
}

void on_drives_list_row_activated(GtkListBox *box, GtkListBoxRow *row, gpointer user_data) {
    (void)box;
    (void)user_data;
    if (!row) {
        return;
    }

    GtkWidget *row_widget = gtk_list_box_row_get_child(GTK_LIST_BOX_ROW(row));
    const char *drive_name = g_object_get_data(G_OBJECT(row_widget), "drive_name");

    if (drive_name) {
        strncpy(current_drive_name, drive_name, sizeof(current_drive_name) - 1);
        current_drive_name[sizeof(current_drive_name) - 1] = '\0';
        show_drive_details(drive_name);
    }
}

// Helper for adding drive with collected data
void do_add_drive(const char *drive_name, const char *description, const char *container) {
    if (strlen(drive_name) == 0) {
        GtkAlertDialog *alert = gtk_alert_dialog_new("Drive name cannot be empty");
        gtk_alert_dialog_show(alert, GTK_WINDOW(window));
        g_object_unref(alert);
        return;
    }

    // Add to database
    char timestamp[64];
    get_timestamp(timestamp, sizeof(timestamp));

    char mount_path[MAX_PATH];
    long long capacity = 0, available = 0, used = 0;
    int mounted = 0;

    if (find_mount_point(drive_name, mount_path, sizeof(mount_path))) {
        mounted = get_drive_stats(mount_path, &capacity, &available, &used);
    }

    const char *sql = "INSERT INTO drives (drive_name, capacity, space_available, space_used, description, last_updated, storage_container) "
                      "VALUES (?, ?, ?, ?, ?, ?, ?);";

    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, 0);
    if (rc == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, drive_name, -1, SQLITE_TRANSIENT);
        if (mounted) {
            sqlite3_bind_int64(stmt, 2, capacity);
            sqlite3_bind_int64(stmt, 3, available);
            sqlite3_bind_int64(stmt, 4, used);
        } else {
            sqlite3_bind_null(stmt, 2);
            sqlite3_bind_null(stmt, 3);
            sqlite3_bind_null(stmt, 4);
        }
        sqlite3_bind_text(stmt, 5, description, -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(stmt, 6, timestamp, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 7, container, -1, SQLITE_TRANSIENT);

        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        if (rc == SQLITE_DONE) {
            refresh_drives_list(NULL);
        } else {
            char msg[256];
            snprintf(msg, sizeof(msg), "Failed to add drive: %s", sqlite3_errmsg(db));
            GtkAlertDialog *alert = gtk_alert_dialog_new(msg);
            gtk_alert_dialog_show(alert, GTK_WINDOW(window));
            g_object_unref(alert);
        }
    }
}

// OK button callback for add drive dialog
void on_add_ok_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    GtkWidget **entries = (GtkWidget **)user_data;
    const char *drive_name = gtk_editable_get_text(GTK_EDITABLE(entries[0]));
    const char *description = gtk_editable_get_text(GTK_EDITABLE(entries[1]));
    const char *container = gtk_editable_get_text(GTK_EDITABLE(entries[2]));
    GtkWidget *dialog = entries[3];

    do_add_drive(drive_name, description, container);
    gtk_window_close(GTK_WINDOW(dialog));
}

// Cleanup callback for add drive dialog
void on_add_dialog_destroy(GtkWidget *widget, gpointer user_data) {
    (void)widget;
    g_free(user_data);
}

void on_add_drive_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;

    GtkWidget *dialog = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dialog), "Add Drive");
    gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(window));
    gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 400, 200);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
    gtk_widget_set_margin_start(box, 16);
    gtk_widget_set_margin_end(box, 16);
    gtk_widget_set_margin_top(box, 16);
    gtk_widget_set_margin_bottom(box, 16);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);

    // Drive name
    GtkWidget *name_label = gtk_label_new("Drive Name:");
    gtk_label_set_xalign(GTK_LABEL(name_label), 1.0);
    GtkWidget *name_entry = gtk_entry_new();
    gtk_widget_set_hexpand(name_entry, TRUE);
    gtk_grid_attach(GTK_GRID(grid), name_label, 0, 0, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), name_entry, 1, 0, 1, 1);

    // Description
    GtkWidget *desc_label = gtk_label_new("Description:");
    gtk_label_set_xalign(GTK_LABEL(desc_label), 1.0);
    GtkWidget *desc_entry = gtk_entry_new();
    gtk_widget_set_hexpand(desc_entry, TRUE);
    gtk_grid_attach(GTK_GRID(grid), desc_label, 0, 1, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), desc_entry, 1, 1, 1, 1);

    // Container
    GtkWidget *container_label = gtk_label_new("Container:");
    gtk_label_set_xalign(GTK_LABEL(container_label), 1.0);
    GtkWidget *container_entry = gtk_entry_new();
    gtk_widget_set_hexpand(container_entry, TRUE);
    gtk_grid_attach(GTK_GRID(grid), container_label, 0, 2, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), container_entry, 1, 2, 1, 1);

    gtk_box_append(GTK_BOX(box), grid);

    // Button box
    GtkWidget *button_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(button_box, GTK_ALIGN_END);

    GtkWidget *cancel_button = gtk_button_new_with_label("Cancel");
    GtkWidget *ok_button = gtk_button_new_with_label("Add");
    gtk_widget_add_css_class(ok_button, "suggested-action");

    // Store entry widgets and dialog for callback
    GtkWidget **entries = g_malloc(sizeof(GtkWidget *) * 4);
    entries[0] = name_entry;
    entries[1] = desc_entry;
    entries[2] = container_entry;
    entries[3] = dialog;

    g_signal_connect_swapped(cancel_button, "clicked", G_CALLBACK(gtk_window_close), dialog);
    g_signal_connect(ok_button, "clicked", G_CALLBACK(on_add_ok_clicked), entries);
    g_signal_connect(dialog, "destroy", G_CALLBACK(on_add_dialog_destroy), entries);

    gtk_box_append(GTK_BOX(button_box), cancel_button);
    gtk_box_append(GTK_BOX(button_box), ok_button);

    gtk_box_append(GTK_BOX(box), button_box);

    gtk_window_set_child(GTK_WINDOW(dialog), box);
    gtk_window_present(GTK_WINDOW(dialog));
}

// Structure to hold edit dialog data
typedef struct {
    GtkWidget *desc_entry;
    GtkWidget *container_entry;
    GtkWidget *capacity_entry;
    GtkWidget *used_entry;
    GtkWidget *available_entry;
    GtkWidget *dialog;
    char drive_name[256];
} EditDialogData;

// OK button callback for edit drive dialog
void on_edit_ok_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    EditDialogData *data = (EditDialogData *)user_data;

    const char *description = gtk_editable_get_text(GTK_EDITABLE(data->desc_entry));
    const char *container = gtk_editable_get_text(GTK_EDITABLE(data->container_entry));
    const char *capacity_str = gtk_editable_get_text(GTK_EDITABLE(data->capacity_entry));
    const char *used_str = gtk_editable_get_text(GTK_EDITABLE(data->used_entry));
    const char *available_str = gtk_editable_get_text(GTK_EDITABLE(data->available_entry));

    // Convert GB to bytes
    long long capacity = 0, used = 0, available = 0;
    int update_capacity = 0;

    if (strlen(capacity_str) > 0 || strlen(used_str) > 0 || strlen(available_str) > 0) {
        capacity = (long long)(atof(capacity_str) * 1024.0 * 1024.0 * 1024.0);
        used = (long long)(atof(used_str) * 1024.0 * 1024.0 * 1024.0);
        available = (long long)(atof(available_str) * 1024.0 * 1024.0 * 1024.0);
        update_capacity = 1;
    }

    char timestamp[64];
    get_timestamp(timestamp, sizeof(timestamp));

    // Build dynamic SQL
    char sql[1024] = "UPDATE drives SET last_updated = ?";
    int param_idx = 2;

    if (update_capacity) {
        strcat(sql, ", capacity = ?, space_available = ?, space_used = ?");
    }
    if (strlen(description) > 0) {
        strcat(sql, ", description = ?");
    }
    if (strlen(container) > 0) {
        strcat(sql, ", storage_container = ?");
    }
    strcat(sql, " WHERE drive_name = ?;");

    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, 0);
    if (rc == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, timestamp, -1, SQLITE_STATIC);

        if (update_capacity) {
            sqlite3_bind_int64(stmt, param_idx++, capacity);
            sqlite3_bind_int64(stmt, param_idx++, available);
            sqlite3_bind_int64(stmt, param_idx++, used);
        }
        if (strlen(description) > 0) {
            sqlite3_bind_text(stmt, param_idx++, description, -1, SQLITE_STATIC);
        }
        if (strlen(container) > 0) {
            sqlite3_bind_text(stmt, param_idx++, container, -1, SQLITE_STATIC);
        }
        sqlite3_bind_text(stmt, param_idx, data->drive_name, -1, SQLITE_STATIC);

        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        if (rc == SQLITE_DONE) {
            refresh_drives_list(NULL);
            show_drive_details(data->drive_name);

            char msg[256];
            snprintf(msg, sizeof(msg), "Drive '%s' updated successfully", data->drive_name);

            GtkAlertDialog *alert = gtk_alert_dialog_new(msg);
            gtk_alert_dialog_show(alert, GTK_WINDOW(window));
            g_object_unref(alert);
        }
    }

    gtk_window_close(GTK_WINDOW(data->dialog));
}

// Cleanup callback for edit drive dialog
void on_edit_dialog_destroy(GtkWidget *widget, gpointer user_data) {
    (void)widget;
    g_free(user_data);
}

void on_edit_drive_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;

    if (strlen(current_drive_name) == 0) {
        GtkAlertDialog *alert = gtk_alert_dialog_new("Please select a drive first");
        gtk_alert_dialog_show(alert, GTK_WINDOW(window));
        g_object_unref(alert);
        return;
    }

    // Query current drive data
    const char *sql = "SELECT description, storage_container, capacity, space_used, space_available FROM drives WHERE drive_name = ?;";
    sqlite3_stmt *stmt;

    char current_desc[MAX_DESC] = "";
    char current_container[MAX_CONTAINER] = "";
    double current_capacity_gb = 0.0;
    double current_used_gb = 0.0;
    double current_available_gb = 0.0;

    if (sqlite3_prepare_v2(db, sql, -1, &stmt, 0) == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, current_drive_name, -1, SQLITE_STATIC);

        if (sqlite3_step(stmt) == SQLITE_ROW) {
            const char *desc = (const char *)sqlite3_column_text(stmt, 0);
            const char *cont = (const char *)sqlite3_column_text(stmt, 1);
            long long capacity = sqlite3_column_int64(stmt, 2);
            long long used = sqlite3_column_int64(stmt, 3);
            long long available = sqlite3_column_int64(stmt, 4);

            if (desc) strncpy(current_desc, desc, sizeof(current_desc) - 1);
            if (cont) strncpy(current_container, cont, sizeof(current_container) - 1);

            if (capacity > 0) current_capacity_gb = (double)capacity / (1024.0 * 1024.0 * 1024.0);
            if (used > 0) current_used_gb = (double)used / (1024.0 * 1024.0 * 1024.0);
            if (available > 0) current_available_gb = (double)available / (1024.0 * 1024.0 * 1024.0);
        }
        sqlite3_finalize(stmt);
    }

    // Create edit dialog
    GtkWidget *dialog = gtk_window_new();
    gtk_window_set_title(GTK_WINDOW(dialog), "Edit Drive");
    gtk_window_set_transient_for(GTK_WINDOW(dialog), GTK_WINDOW(window));
    gtk_window_set_modal(GTK_WINDOW(dialog), TRUE);
    gtk_window_set_default_size(GTK_WINDOW(dialog), 450, 300);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
    gtk_widget_set_margin_start(box, 16);
    gtk_widget_set_margin_end(box, 16);
    gtk_widget_set_margin_top(box, 16);
    gtk_widget_set_margin_bottom(box, 16);

    // Drive name (read-only display)
    GtkWidget *name_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    GtkWidget *name_label = gtk_label_new("Drive Name:");
    gtk_widget_add_css_class(name_label, "dim-label");
    GtkWidget *name_value = gtk_label_new(current_drive_name);
    gtk_widget_add_css_class(name_value, "title-3");
    gtk_box_append(GTK_BOX(name_box), name_label);
    gtk_box_append(GTK_BOX(name_box), name_value);
    gtk_box_append(GTK_BOX(box), name_box);

    GtkWidget *grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(grid), 8);

    int row = 0;

    // Description
    GtkWidget *desc_label = gtk_label_new("Description:");
    gtk_label_set_xalign(GTK_LABEL(desc_label), 1.0);
    GtkWidget *desc_entry = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(desc_entry), current_desc);
    gtk_widget_set_hexpand(desc_entry, TRUE);
    gtk_grid_attach(GTK_GRID(grid), desc_label, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), desc_entry, 1, row++, 1, 1);

    // Container
    GtkWidget *container_label = gtk_label_new("Container:");
    gtk_label_set_xalign(GTK_LABEL(container_label), 1.0);
    GtkWidget *container_entry = gtk_entry_new();
    gtk_editable_set_text(GTK_EDITABLE(container_entry), current_container);
    gtk_widget_set_hexpand(container_entry, TRUE);
    gtk_grid_attach(GTK_GRID(grid), container_label, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), container_entry, 1, row++, 1, 1);

    // Capacity (GB)
    GtkWidget *capacity_label = gtk_label_new("Capacity (GB):");
    gtk_label_set_xalign(GTK_LABEL(capacity_label), 1.0);
    GtkWidget *capacity_entry = gtk_entry_new();
    if (current_capacity_gb > 0) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.2f", current_capacity_gb);
        gtk_editable_set_text(GTK_EDITABLE(capacity_entry), buf);
    }
    gtk_widget_set_hexpand(capacity_entry, TRUE);
    gtk_grid_attach(GTK_GRID(grid), capacity_label, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), capacity_entry, 1, row++, 1, 1);

    // Used (GB)
    GtkWidget *used_label = gtk_label_new("Used (GB):");
    gtk_label_set_xalign(GTK_LABEL(used_label), 1.0);
    GtkWidget *used_entry = gtk_entry_new();
    if (current_used_gb > 0) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.2f", current_used_gb);
        gtk_editable_set_text(GTK_EDITABLE(used_entry), buf);
    }
    gtk_widget_set_hexpand(used_entry, TRUE);
    gtk_grid_attach(GTK_GRID(grid), used_label, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), used_entry, 1, row++, 1, 1);

    // Available (GB)
    GtkWidget *available_label = gtk_label_new("Available (GB):");
    gtk_label_set_xalign(GTK_LABEL(available_label), 1.0);
    GtkWidget *available_entry = gtk_entry_new();
    if (current_available_gb > 0) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.2f", current_available_gb);
        gtk_editable_set_text(GTK_EDITABLE(available_entry), buf);
    }
    gtk_widget_set_hexpand(available_entry, TRUE);
    gtk_grid_attach(GTK_GRID(grid), available_label, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(grid), available_entry, 1, row++, 1, 1);

    gtk_box_append(GTK_BOX(box), grid);

    // Button box
    GtkWidget *button_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(button_box, GTK_ALIGN_END);

    GtkWidget *cancel_button = gtk_button_new_with_label("Cancel");
    GtkWidget *ok_button = gtk_button_new_with_label("Save");
    gtk_widget_add_css_class(ok_button, "suggested-action");

    // Store data for callback
    EditDialogData *data = g_malloc(sizeof(EditDialogData));
    data->desc_entry = desc_entry;
    data->container_entry = container_entry;
    data->capacity_entry = capacity_entry;
    data->used_entry = used_entry;
    data->available_entry = available_entry;
    data->dialog = dialog;
    strncpy(data->drive_name, current_drive_name, sizeof(data->drive_name) - 1);

    g_signal_connect_swapped(cancel_button, "clicked", G_CALLBACK(gtk_window_close), dialog);
    g_signal_connect(ok_button, "clicked", G_CALLBACK(on_edit_ok_clicked), data);
    g_signal_connect(dialog, "destroy", G_CALLBACK(on_edit_dialog_destroy), data);

    gtk_box_append(GTK_BOX(button_box), cancel_button);
    gtk_box_append(GTK_BOX(button_box), ok_button);

    gtk_box_append(GTK_BOX(box), button_box);

    gtk_window_set_child(GTK_WINDOW(dialog), box);
    gtk_window_present(GTK_WINDOW(dialog));
}

void on_update_drive_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;

    if (strlen(current_drive_name) == 0) {
        GtkAlertDialog *alert = gtk_alert_dialog_new("Please select a drive first");
        gtk_alert_dialog_show(alert, GTK_WINDOW(window));
        g_object_unref(alert);
        return;
    }

    char timestamp[64];
    get_timestamp(timestamp, sizeof(timestamp));

    char mount_path[MAX_PATH];
    long long capacity = 0, available = 0, used = 0;
    int mounted = 0;

    if (find_mount_point(current_drive_name, mount_path, sizeof(mount_path))) {
        mounted = get_drive_stats(mount_path, &capacity, &available, &used);
    }

    const char *sql = mounted ?
        "UPDATE drives SET last_updated = ?, capacity = ?, space_available = ?, space_used = ? WHERE drive_name = ?;" :
        "UPDATE drives SET last_updated = ? WHERE drive_name = ?;";

    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, 0);
    if (rc == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, timestamp, -1, SQLITE_STATIC);
        if (mounted) {
            sqlite3_bind_int64(stmt, 2, capacity);
            sqlite3_bind_int64(stmt, 3, available);
            sqlite3_bind_int64(stmt, 4, used);
            sqlite3_bind_text(stmt, 5, current_drive_name, -1, SQLITE_STATIC);
        } else {
            sqlite3_bind_text(stmt, 2, current_drive_name, -1, SQLITE_STATIC);
        }

        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        if (rc == SQLITE_DONE) {
            refresh_drives_list(NULL);
            show_drive_details(current_drive_name);

            char msg[256];
            if (mounted) {
                snprintf(msg, sizeof(msg), "Drive '%s' updated with current capacity information", current_drive_name);
            } else {
                snprintf(msg, sizeof(msg), "Drive '%s' timestamp updated (not currently mounted)", current_drive_name);
            }

            GtkAlertDialog *alert = gtk_alert_dialog_new(msg);
            gtk_alert_dialog_show(alert, GTK_WINDOW(window));
            g_object_unref(alert);
        }
    }
}

void on_verify_drive_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;

    if (strlen(current_drive_name) == 0) {
        GtkAlertDialog *alert = gtk_alert_dialog_new("Please select a drive first");
        gtk_alert_dialog_show(alert, GTK_WINDOW(window));
        g_object_unref(alert);
        return;
    }

    char timestamp[64];
    get_timestamp(timestamp, sizeof(timestamp));

    const char *sql = "UPDATE drives SET last_verified = ? WHERE drive_name = ?;";

    sqlite3_stmt *stmt;
    int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, 0);
    if (rc == SQLITE_OK) {
        sqlite3_bind_text(stmt, 1, timestamp, -1, SQLITE_STATIC);
        sqlite3_bind_text(stmt, 2, current_drive_name, -1, SQLITE_STATIC);

        rc = sqlite3_step(stmt);
        sqlite3_finalize(stmt);

        if (rc == SQLITE_DONE) {
            show_drive_details(current_drive_name);

            char msg[256];
            snprintf(msg, sizeof(msg), "Drive '%s' marked as verified", current_drive_name);

            GtkAlertDialog *alert = gtk_alert_dialog_new(msg);
            gtk_alert_dialog_show(alert, GTK_WINDOW(window));
            g_object_unref(alert);
        }
    }
}

// Callback for delete confirmation
void on_delete_confirmed(GObject *source, GAsyncResult *result, gpointer user_data) {
    (void)result;
    GtkAlertDialog *dialog = GTK_ALERT_DIALOG(source);
    int response = gtk_alert_dialog_choose_finish(dialog, result, NULL);

    // Response 0 = Delete, 1 = Cancel
    if (response == 0) {
        char *drive_name = (char *)user_data;

        const char *sql = "DELETE FROM drives WHERE drive_name = ?;";
        sqlite3_stmt *stmt;
        int rc = sqlite3_prepare_v2(db, sql, -1, &stmt, 0);
        if (rc == SQLITE_OK) {
            sqlite3_bind_text(stmt, 1, drive_name, -1, SQLITE_STATIC);
            rc = sqlite3_step(stmt);
            sqlite3_finalize(stmt);

            if (rc == SQLITE_DONE) {
                char msg[256];
                snprintf(msg, sizeof(msg), "Drive '%s' has been deleted from tracking", drive_name);

                GtkAlertDialog *alert = gtk_alert_dialog_new(msg);
                gtk_alert_dialog_show(alert, GTK_WINDOW(window));
                g_object_unref(alert);

                // Clear details and refresh list
                current_drive_name[0] = '\0';
                gtk_widget_set_visible(detail_box, FALSE);
                refresh_drives_list(NULL);
            }
        }
    }

    g_free(user_data);
}

void on_delete_drive_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;

    if (strlen(current_drive_name) == 0) {
        GtkAlertDialog *alert = gtk_alert_dialog_new("Please select a drive first");
        gtk_alert_dialog_show(alert, GTK_WINDOW(window));
        g_object_unref(alert);
        return;
    }

    // Create confirmation dialog
    char msg[512];
    snprintf(msg, sizeof(msg),
             "Are you sure you want to delete drive '%s'?\n\n"
             "This will remove all stored information about this drive from tracking.",
             current_drive_name);

    GtkAlertDialog *dialog = gtk_alert_dialog_new(msg);

    const char *buttons[] = { "Delete", "Cancel", NULL };
    gtk_alert_dialog_set_buttons(dialog, buttons);
    gtk_alert_dialog_set_default_button(dialog, 1); // Default to Cancel
    gtk_alert_dialog_set_cancel_button(dialog, 1);  // Cancel button

    // Store drive name for callback
    char *drive_name_copy = g_strdup(current_drive_name);

    gtk_alert_dialog_choose(dialog, GTK_WINDOW(window), NULL,
                           on_delete_confirmed, drive_name_copy);
}

void on_refresh_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;
    refresh_drives_list(NULL);
    if (strlen(current_drive_name) > 0) {
        show_drive_details(current_drive_name);
    }
}

void on_sync_databases_clicked(GtkButton *button, gpointer user_data) {
    (void)button;
    (void)user_data;

    int added = sync_from_databases(db);

    if (added > 0) {
        char msg[256];
        snprintf(msg, sizeof(msg), "Auto-discovered and added %d drive%s from file_tracker databases",
                 added, added == 1 ? "" : "s");

        GtkAlertDialog *alert = gtk_alert_dialog_new(msg);
        gtk_alert_dialog_show(alert, GTK_WINDOW(window));
        g_object_unref(alert);

        refresh_drives_list(NULL);
    } else {
        GtkAlertDialog *alert = gtk_alert_dialog_new("All file_tracker databases are already tracked");
        gtk_alert_dialog_show(alert, GTK_WINDOW(window));
        g_object_unref(alert);
    }
}

// ==== Main Window Setup ====

void activate(GtkApplication *app, gpointer user_data) {
    (void)user_data;

    // Main window
    window = gtk_application_window_new(app);
    gtk_window_set_title(GTK_WINDOW(window), "Drive Tracker");
    gtk_window_set_default_size(GTK_WINDOW(window), 1000, 700);
    gtk_widget_set_size_request(GTK_WIDGET(window), 800, 400);

    // Main container - horizontal paned
    GtkWidget *paned = gtk_paned_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_paned_set_resize_start_child(GTK_PANED(paned), FALSE);
    gtk_paned_set_resize_end_child(GTK_PANED(paned), TRUE);
    gtk_paned_set_shrink_start_child(GTK_PANED(paned), FALSE);
    gtk_paned_set_shrink_end_child(GTK_PANED(paned), FALSE);
    gtk_window_set_child(GTK_WINDOW(window), paned);

    // Left side - drives list
    GtkWidget *left_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 8);
    gtk_widget_set_margin_start(left_box, 8);
    gtk_widget_set_margin_end(left_box, 8);
    gtk_widget_set_margin_top(left_box, 8);
    gtk_widget_set_margin_bottom(left_box, 8);
    gtk_widget_set_size_request(left_box, 350, -1);

    // Search box
    search_entry = gtk_search_entry_new();
    gtk_search_entry_set_placeholder_text(GTK_SEARCH_ENTRY(search_entry), "Search drives...");
    g_signal_connect(search_entry, "search-changed", G_CALLBACK(on_search_changed), NULL);
    gtk_box_append(GTK_BOX(left_box), search_entry);

    // Drives list
    GtkWidget *scrolled = gtk_scrolled_window_new();
    gtk_widget_set_vexpand(scrolled, TRUE);
    gtk_scrolled_window_set_min_content_width(GTK_SCROLLED_WINDOW(scrolled), 320);

    drives_list_box = gtk_list_box_new();
    gtk_list_box_set_selection_mode(GTK_LIST_BOX(drives_list_box), GTK_SELECTION_SINGLE);
    g_signal_connect(drives_list_box, "row-activated", G_CALLBACK(on_drives_list_row_activated), NULL);
    gtk_scrolled_window_set_child(GTK_SCROLLED_WINDOW(scrolled), drives_list_box);
    gtk_box_append(GTK_BOX(left_box), scrolled);

    // Button bar
    GtkWidget *button_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);
    gtk_widget_set_halign(button_box, GTK_ALIGN_FILL);

    GtkWidget *top_buttons = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 4);
    gtk_widget_set_halign(top_buttons, GTK_ALIGN_CENTER);

    GtkWidget *add_button = gtk_button_new_with_label("Add Drive");
    g_signal_connect(add_button, "clicked", G_CALLBACK(on_add_drive_clicked), NULL);
    gtk_box_append(GTK_BOX(top_buttons), add_button);

    GtkWidget *refresh_button = gtk_button_new_with_label("Refresh");
    g_signal_connect(refresh_button, "clicked", G_CALLBACK(on_refresh_clicked), NULL);
    gtk_box_append(GTK_BOX(top_buttons), refresh_button);

    gtk_box_append(GTK_BOX(button_box), top_buttons);

    GtkWidget *sync_button = gtk_button_new_with_label("Sync from Databases");
    gtk_widget_set_halign(sync_button, GTK_ALIGN_CENTER);
    g_signal_connect(sync_button, "clicked", G_CALLBACK(on_sync_databases_clicked), NULL);
    gtk_box_append(GTK_BOX(button_box), sync_button);

    gtk_box_append(GTK_BOX(left_box), button_box);

    gtk_paned_set_start_child(GTK_PANED(paned), left_box);

    // Right side - details
    detail_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 16);
    gtk_widget_set_margin_start(detail_box, 16);
    gtk_widget_set_margin_end(detail_box, 16);
    gtk_widget_set_margin_top(detail_box, 16);
    gtk_widget_set_margin_bottom(detail_box, 16);
    gtk_widget_set_size_request(detail_box, 450, -1);
    gtk_widget_set_visible(detail_box, FALSE);

    // Drive name
    detail_name_label = gtk_label_new("");
    char *markup = g_markup_printf_escaped("<span size='xx-large' weight='bold'>%s</span>", "");
    gtk_label_set_markup(GTK_LABEL(detail_name_label), markup);
    g_free(markup);
    gtk_label_set_xalign(GTK_LABEL(detail_name_label), 0.0);
    gtk_box_append(GTK_BOX(detail_box), detail_name_label);

    // Separator
    GtkWidget *sep1 = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_box_append(GTK_BOX(detail_box), sep1);

    // Usage info
    GtkWidget *usage_box = gtk_box_new(GTK_ORIENTATION_VERTICAL, 4);

    usage_progress_bar = GTK_PROGRESS_BAR(gtk_progress_bar_new());
    gtk_widget_set_size_request(GTK_WIDGET(usage_progress_bar), -1, 20);
    gtk_box_append(GTK_BOX(usage_box), GTK_WIDGET(usage_progress_bar));

    detail_percent_label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(detail_percent_label), 0.5);
    gtk_box_append(GTK_BOX(usage_box), detail_percent_label);

    gtk_box_append(GTK_BOX(detail_box), usage_box);

    // Details grid
    GtkWidget *details_grid = gtk_grid_new();
    gtk_grid_set_row_spacing(GTK_GRID(details_grid), 8);
    gtk_grid_set_column_spacing(GTK_GRID(details_grid), 16);

    int row = 0;

    // Capacity
    GtkWidget *cap_label = gtk_label_new("Capacity:");
    gtk_label_set_xalign(GTK_LABEL(cap_label), 1.0);
    gtk_widget_add_css_class(cap_label, "dim-label");
    detail_capacity_label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(detail_capacity_label), 0.0);
    gtk_label_set_selectable(GTK_LABEL(detail_capacity_label), TRUE);
    gtk_grid_attach(GTK_GRID(details_grid), cap_label, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(details_grid), detail_capacity_label, 1, row++, 1, 1);

    // Used
    GtkWidget *used_label = gtk_label_new("Used:");
    gtk_label_set_xalign(GTK_LABEL(used_label), 1.0);
    gtk_widget_add_css_class(used_label, "dim-label");
    detail_used_label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(detail_used_label), 0.0);
    gtk_label_set_selectable(GTK_LABEL(detail_used_label), TRUE);
    gtk_grid_attach(GTK_GRID(details_grid), used_label, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(details_grid), detail_used_label, 1, row++, 1, 1);

    // Available
    GtkWidget *avail_label = gtk_label_new("Available:");
    gtk_label_set_xalign(GTK_LABEL(avail_label), 1.0);
    gtk_widget_add_css_class(avail_label, "dim-label");
    detail_available_label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(detail_available_label), 0.0);
    gtk_label_set_selectable(GTK_LABEL(detail_available_label), TRUE);
    gtk_grid_attach(GTK_GRID(details_grid), avail_label, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(details_grid), detail_available_label, 1, row++, 1, 1);

    // Separator
    GtkWidget *sep2 = gtk_separator_new(GTK_ORIENTATION_HORIZONTAL);
    gtk_grid_attach(GTK_GRID(details_grid), sep2, 0, row++, 2, 1);

    // Description
    GtkWidget *desc_label = gtk_label_new("Description:");
    gtk_label_set_xalign(GTK_LABEL(desc_label), 1.0);
    gtk_widget_add_css_class(desc_label, "dim-label");
    detail_description_label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(detail_description_label), 0.0);
    gtk_label_set_wrap(GTK_LABEL(detail_description_label), TRUE);
    gtk_label_set_selectable(GTK_LABEL(detail_description_label), TRUE);
    gtk_grid_attach(GTK_GRID(details_grid), desc_label, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(details_grid), detail_description_label, 1, row++, 1, 1);

    // Container
    GtkWidget *cont_label = gtk_label_new("Container:");
    gtk_label_set_xalign(GTK_LABEL(cont_label), 1.0);
    gtk_widget_add_css_class(cont_label, "dim-label");
    detail_container_label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(detail_container_label), 0.0);
    gtk_label_set_selectable(GTK_LABEL(detail_container_label), TRUE);
    gtk_grid_attach(GTK_GRID(details_grid), cont_label, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(details_grid), detail_container_label, 1, row++, 1, 1);

    // Last Updated
    GtkWidget *updated_label = gtk_label_new("Last Updated:");
    gtk_label_set_xalign(GTK_LABEL(updated_label), 1.0);
    gtk_widget_add_css_class(updated_label, "dim-label");
    detail_updated_label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(detail_updated_label), 0.0);
    gtk_label_set_selectable(GTK_LABEL(detail_updated_label), TRUE);
    gtk_grid_attach(GTK_GRID(details_grid), updated_label, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(details_grid), detail_updated_label, 1, row++, 1, 1);

    // Last Verified
    GtkWidget *verified_label = gtk_label_new("Last Verified:");
    gtk_label_set_xalign(GTK_LABEL(verified_label), 1.0);
    gtk_widget_add_css_class(verified_label, "dim-label");
    detail_verified_label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(detail_verified_label), 0.0);
    gtk_label_set_selectable(GTK_LABEL(detail_verified_label), TRUE);
    gtk_grid_attach(GTK_GRID(details_grid), verified_label, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(details_grid), detail_verified_label, 1, row++, 1, 1);

    // Last Checksum Run (from file_tracker)
    GtkWidget *checksum_label = gtk_label_new("Last Checksum Run:");
    gtk_label_set_xalign(GTK_LABEL(checksum_label), 1.0);
    gtk_widget_add_css_class(checksum_label, "dim-label");
    detail_checksum_label = gtk_label_new("");
    gtk_label_set_xalign(GTK_LABEL(detail_checksum_label), 0.0);
    gtk_label_set_selectable(GTK_LABEL(detail_checksum_label), TRUE);
    gtk_grid_attach(GTK_GRID(details_grid), checksum_label, 0, row, 1, 1);
    gtk_grid_attach(GTK_GRID(details_grid), detail_checksum_label, 1, row++, 1, 1);

    gtk_box_append(GTK_BOX(detail_box), details_grid);

    // Action buttons
    GtkWidget *action_box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 8);
    gtk_widget_set_halign(action_box, GTK_ALIGN_CENTER);
    gtk_widget_set_margin_top(action_box, 16);

    GtkWidget *update_button = gtk_button_new_with_label("Update Drive Info");
    g_signal_connect(update_button, "clicked", G_CALLBACK(on_update_drive_clicked), NULL);
    gtk_box_append(GTK_BOX(action_box), update_button);

    GtkWidget *edit_button = gtk_button_new_with_label("Edit Drive");
    g_signal_connect(edit_button, "clicked", G_CALLBACK(on_edit_drive_clicked), NULL);
    gtk_box_append(GTK_BOX(action_box), edit_button);

    GtkWidget *verify_button = gtk_button_new_with_label("Mark as Verified");
    g_signal_connect(verify_button, "clicked", G_CALLBACK(on_verify_drive_clicked), NULL);
    gtk_box_append(GTK_BOX(action_box), verify_button);

    GtkWidget *delete_button = gtk_button_new_with_label("Delete Drive");
    gtk_widget_add_css_class(delete_button, "destructive-action");
    g_signal_connect(delete_button, "clicked", G_CALLBACK(on_delete_drive_clicked), NULL);
    gtk_box_append(GTK_BOX(action_box), delete_button);

    gtk_box_append(GTK_BOX(detail_box), action_box);

    gtk_paned_set_end_child(GTK_PANED(paned), detail_box);

    // Present window first to establish layout
    gtk_window_present(GTK_WINDOW(window));

    // Set paned position after window is shown
    gtk_paned_set_position(GTK_PANED(paned), 380);

    // Auto-sync drives from file_tracker databases on startup
    sync_from_databases(db);

    // Load initial data
    refresh_drives_list(NULL);
}

int main(int argc, char *argv[]) {
    if (!init_database()) {
        fprintf(stderr, "Failed to initialize database\n");
        return 1;
    }

    GtkApplication *app = gtk_application_new("com.filetracker.drives", G_APPLICATION_DEFAULT_FLAGS);
    g_signal_connect(app, "activate", G_CALLBACK(activate), NULL);

    int status = g_application_run(G_APPLICATION(app), argc, argv);

    g_object_unref(app);
    sqlite3_close(db);

    return status;
}

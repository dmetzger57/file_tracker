# GUI Error Support - Implementation Summary

## Date: 2026-09-12

## Overview
Updated all GUI applications to support viewing and filtering error messages logged during file_tracker runs.

## Modified GUI Applications

### 1. ft_summary_gui
**Purpose:** Browse scan history and view file lists from tracker databases

**Changes:**
- Added "Errors" tab to the notebook interface
- New global widget: `GtkWidget *errors_text;`
- Automatically loads error messages when a run is selected
- Export functionality includes errors tab (exports to `dbname-runX-errors.txt`)

**Features:**
- Errors tab displays all ERROR status messages from selected run
- Monospace font for easy reading of error messages
- Shows count of errors at top of list
- Displays "(No files found)" if no errors occurred

**User Experience:**
```
Tabs:
  - Details (run information and statistics)
  - Missing Files
  - Changed Files
  - New Files
  - Errors  <-- NEW TAB
```

**Implementation Details:**
```c
// New widget declaration
GtkWidget *errors_text;

// Load errors when run is selected
load_file_list(db_name, run_id, "ERROR", GTK_TEXT_VIEW(errors_text));

// Export support (case 4 in export switch)
case 4: // Errors
    text_view = GTK_TEXT_VIEW(errors_text);
    filename_suffix = "errors";
    break;
```

### 2. ft_logs_gui
**Purpose:** View detailed log messages from specific file_tracker runs with filtering

**Changes:**
- Added "Errors" filter checkbox
- New global widget: `GtkWidget *filter_errors_check;`
- Filter checkbox appears alongside existing filters (New, Changed, Missing, Unchanged)
- Integrated into filter toggle logic

**Features:**
- "Errors" checkbox filters logs to show only ERROR status messages
- Can be combined with other filters (e.g., Errors + Missing)
- When "All" is selected, Errors checkbox is cleared along with others
- Clicking Errors unchecks the "All" filter

**User Experience:**
```
Filters:  [All] [New] [Changed] [Missing] [Unchanged] [Errors]  <-- NEW FILTER
```

**Implementation Details:**
```c
// New widget declaration
GtkWidget *filter_errors_check;

// Create checkbox
filter_errors_check = gtk_check_button_new_with_label("Errors");
g_signal_connect(filter_errors_check, "toggled", G_CALLBACK(on_filter_toggled), NULL);

// Filter logic (in query building)
if (gtk_check_button_get_active(GTK_CHECK_BUTTON(filter_errors_check))) {
    if (!first) strcat(status_filter, " OR ");
    strcat(status_filter, "status = 'ERROR'");
    any_filter = 1;
}

// Reset with "All" filter
gtk_check_button_set_active(GTK_CHECK_BUTTON(filter_errors_check), FALSE);
```

### 3. file_tracker_gui
**Purpose:** Visual interface for running file_tracker scans

**Status:** Already supports error display (no changes needed)

**Existing Features:**
- Live error count in status bar during scan
- Error count in final results summary
- Errors included in progress statistics

**Display Format:**
```
Status Line:
Total: 1000 | Processed: 500 | Remaining: 500 | Unchanged: 450 | Changed: 20 | New: 10 | Missing: 5 | Errors: 15

Results Summary:
Scan Complete!

Path: /path/to/scan
Database: MyDatabase
Mode: Update
Checksum: Enabled

Results:
  Unchanged: 450 files
  Changed:   20 files
  New:       10 files
  Missing:   5 files
  Errors:    15 files  <-- Already included

Total processed: 500 files
```

## Database Integration

All GUI tools query the same `run_logs` table used by CLI tools:

```sql
-- Query for errors in ft_summary_gui
SELECT full_path FROM run_logs 
WHERE run_id = ? AND status LIKE '%ERROR%' 
ORDER BY full_path;

-- Query for errors in ft_logs_gui
SELECT status, full_path FROM run_logs 
WHERE run_id = ? AND (status = 'ERROR') 
ORDER BY id;
```

## User Workflows

### Viewing Errors in ft_summary_gui

1. Launch `ft_summary_gui`
2. Select database from dropdown
3. Click on a run in the history table
4. Click the "Errors" tab
5. View list of all errors from that run
6. Optional: Click "Export" to save errors to text file

### Filtering for Errors in ft_logs_gui

1. Launch `ft_logs_gui`
2. Select database from dropdown
3. Click on a run from the runs list
4. Check the "Errors" filter checkbox
5. View only error messages (unchecks other filters)
6. Optional: Combine with other filters for multi-status view

### Monitoring Errors during Scan

1. Launch `file_tracker_gui`
2. Configure scan path and options
3. Click "Start Scan"
4. Watch error count in status bar during scan
5. Review error count in final results summary

## Testing GUI Applications

### Quick Test - ft_summary_gui

```bash
# Launch GUI
./ft_summary_gui

# Actions:
# 1. Select any database
# 2. Click on a run
# 3. Click "Errors" tab
# 4. Verify it shows error messages or "(No files found)"
# 5. Click "Export" button
# 6. Check that dbname-runX-errors.txt was created
```

### Quick Test - ft_logs_gui

```bash
# Launch GUI
./ft_logs_gui

# Actions:
# 1. Select any database
# 2. Click on a run
# 3. Click "All" filter (should show all logs)
# 4. Click "Errors" filter (should show only errors)
# 5. Verify "All" is unchecked when "Errors" is checked
# 6. Click "All" again and verify "Errors" is unchecked
```

### Quick Test - file_tracker_gui

```bash
# Launch GUI
./file_tracker_gui

# Actions:
# 1. Select a path to scan
# 2. Enable update mode and checksum
# 3. Click "Start Scan"
# 4. Observe error count in status bar
# 5. Wait for completion
# 6. Check error count in results summary
```

## Build Information

**Compilation:**
```bash
make clean && make
```

**Binary Sizes:**
- file_tracker_gui: 43K
- ft_summary_gui: 58K
- ft_logs_gui: 40K

**Dependencies:**
- GTK4
- SQLite3
- GLib

**Compiler Warnings:**
- Some GTK deprecation warnings (pre-existing, not related to error support)
- All warnings are non-critical and don't affect functionality

## Code Statistics

**ft_summary_gui.c:**
- Added: 1 widget declaration
- Added: 1 load_file_list call
- Added: 1 export case
- Added: 7 lines for Errors tab UI creation

**ft_logs_gui.c:**
- Added: 1 widget declaration
- Added: 1 filter condition
- Added: 1 checkbox reset in "All" handler
- Added: 6 lines for Errors checkbox UI creation

**file_tracker_gui.c:**
- No changes (already supported errors)

## Compatibility

**Backward Compatibility:**
✅ All changes are additive only
✅ No changes to database schema
✅ Existing functionality unchanged
✅ Works with old databases (shows no errors if none logged)

**Forward Compatibility:**
✅ New error logs immediately visible in all GUIs
✅ No migration required
✅ Consistent behavior with CLI tools

## Screenshots Description

### ft_summary_gui - Errors Tab
```
┌─────────────────────────────────────────────────────────────┐
│ Database: [MyDatabase ▼]  [Refresh]  ☐ Show all runs       │
├─────────────────────────────────────────────────────────────┤
│ Run History (left panel)                                     │
│ ┌─────────────────────────┐  Tabs (right panel):           │
│ │ Run # │ Date    │ Files │  ┌──────────────────────────┐ │
│ │   12  │ 09-12   │ 1000  │  │ Details │ Missing │ ... │ │
│ │   11  │ 09-11   │  950  │  │ Changed │ New │ Errors  │ │
│ └─────────────────────────┘  └──────────────────────────┘ │
│                               ┌──────────────────────────┐ │
│                               │ Total: 2 files           │ │
│                               │                          │ │
│                               │ Path too long: /very/... │ │
│                               │ SQLite error: database...│ │
│                               └──────────────────────────┘ │
│                               [Export]                     │
└─────────────────────────────────────────────────────────────┘
```

### ft_logs_gui - Errors Filter
```
┌─────────────────────────────────────────────────────────────┐
│ Database: [MyDatabase ▼]  [Refresh]                        │
├─────────────────────────────────────────────────────────────┤
│ Runs (left panel)           │ Run Info (right panel)       │
│ ┌─────────────────────────┐ │ Run #: 12                   │
│ │ MyDB-2026-09-12-10-30-45│ │ Date: 2026-09-12 10:30:45   │
│ │ MyDB-2026-09-11-14-20-30│ │ Machine: hostname           │
│ └─────────────────────────┘ │ Unchanged: 1000             │
│                             │ ... (other stats)           │
│                             │                             │
│ Filters: ☐All ☐New ☐Changed ☐Missing ☐Unchanged ☑Errors │
│                             │                             │
│ Log Messages:               │                             │
│ ┌───────────────────────────┐                             │
│ │ [ERROR] Path too long...  │                             │
│ │ [ERROR] SQLite error...   │                             │
│ └───────────────────────────┘                             │
└─────────────────────────────────────────────────────────────┘
```

## Future Enhancements

Possible improvements:
- Add color coding for error severity in ft_logs_gui
- Add error count badge on Errors tab in ft_summary_gui
- Add context menu to copy error messages
- Add search/filter within error messages
- Add error statistics chart
- Add "Jump to first error" button in file_tracker_gui

## Conclusion

All GUI applications now fully support viewing and filtering error messages, providing a complete visual interface for the error logging feature. The implementation follows existing patterns in each GUI application and maintains consistency with CLI tools.

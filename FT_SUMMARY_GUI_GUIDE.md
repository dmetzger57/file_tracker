# File Tracker Summary GUI Guide

## Overview

The File Tracker Summary GUI (`ft_summary_gui`) provides a visual interface for browsing file tracker scan history, viewing detailed statistics, and exploring file lists. It presents all the information from `ft_summary` and `ft_logs` in an easy-to-navigate graphical format.

## Main Window Layout

### Header Section

**Database Selection:**
- **Dropdown Menu:** Shows all file tracker databases found in `~/db/FileTracker/`
- Automatically excludes `drives.db` (drive tracker database)
- Select any database to view its scan history

**Options:**
- **Show All Runs:** When checked, displays complete scan history; when unchecked, shows only the most recent run
- **Refresh Button:** Reload database list and current data
- **Export Button:** Save current tab contents to a text file

### Run History Table (Left Panel)

Displays a table with all runs for the selected database:

| Column | Description |
|--------|-------------|
| **Run #** | Unique run identifier |
| **Date** | When the scan was performed |
| **Update** | Whether database was updated (On/Off) |
| **Checksum** | Whether SHA-256 verification was enabled (On/Off) |
| **Unchanged** | Number of files that didn't change |
| **Changed** | Number of files that were modified |
| **New** | Number of newly discovered files |
| **Missing** | Number of files in database but not found on disk |
| **Errors** | Number of files that couldn't be processed |

**Interaction:**
- Click any row to view details for that run
- Columns are resizable by dragging separators
- Table scrolls if there are many runs

### Details Panel (Right Side)

Tabbed interface showing different views of the selected run:

#### Tab 1: Details
Shows comprehensive information about the selected run:

```
Run #X Details
═══════════════════════════════════════

Database:       example_db
Machine:        hostname
Run Date:       2026-09-11 14:00:00
Checksum:       Enabled/Disabled
Update Mode:    On/Off

File Statistics:
───────────────────────────────────────
  Unchanged:          500 files
  Changed:             10 files
  New:                 25 files
  Missing:              2 files
  Errors:               0 files
───────────────────────────────────────
  Total:              537 files

Run Note:
───────────────────────────────────────
Monthly verification check
```

#### Tab 2: Missing Files
Lists all files that were in the database but not found on disk during this run.

Shows:
- Total count at the top
- Full path of each missing file
- "(No files found)" if none missing

**Why files might be missing:**
- Files deleted
- Files moved to different location
- Files renamed
- Drive not mounted
- Directory no longer accessible

#### Tab 3: Changed Files
Lists all files that were modified since the last scan.

Shows:
- Total count at the top
- Full path of each changed file
- "(No files found)" if nothing changed

**What constitutes a change:**
- File size changed
- Modification time changed
- Checksum differs (when checksum verification enabled)

#### Tab 4: New Files
Lists all files discovered during this run that weren't in the database.

Shows:
- Total count at the top
- Full path of each new file
- "(No files found)" if no new files

**Why files might be new:**
- First scan of directory
- Files added since last scan
- Files moved from another location
- Previously ignored files now included

## Common Workflows

### Viewing Scan History

1. Launch `ft_summary_gui`
2. Select database from dropdown
3. Check "Show All Runs" to see complete history
4. Browse through runs to see how files changed over time
5. Compare statistics between runs

### Investigating Missing Files

1. Select database
2. View most recent run (uncheck "Show All Runs")
3. Click run in table
4. Switch to "Missing Files" tab
5. Review list to identify what's missing
6. Determine if files were deleted or drive unmounted

### Finding What Changed

1. Select database
2. Click a recent run
3. Switch to "Changed Files" tab
4. Review list of modified files
5. Investigate unexpected changes (possible corruption or unauthorized modifications)

### Verifying New Backups

1. Select backup drive database
2. View latest run
3. Check "New Files" tab
4. Verify expected files were backed up
5. Check "Changed Files" to see what was updated

### Exporting Reports

1. Navigate to desired database and run
2. Switch to the tab you want to export (Details, Missing, Changed, or New)
3. Click "Export" button
4. File saved as: `<database>-run<N>-<tabname>.txt` in current directory
5. Open file in text editor or attach to email

Example exported filenames:
- `photos-run23-details.txt`
- `photos-run23-missing.txt`
- `photos-run23-changed.txt`
- `photos-run23-new.txt`

### Monitoring Drive Health

1. Create routine: Run file_tracker monthly with checksums
2. Open `ft_summary_gui` after each scan
3. Check for:
   - **Changed files** with checksums enabled → possible bit rot
   - **Missing files** → drive failures or deletions
   - **Unexpected new files** → verify legitimacy
4. Export details for record keeping

## Understanding the Data

### Unchanged vs Changed

**Unchanged:**
- File exists in same location
- Size matches database
- Modification time matches database
- Checksum matches (if verification enabled)

**Changed:**
- File exists but one or more attributes differ
- Could be legitimate edits or silent corruption
- With checksums: detects content changes even if mtime unchanged

### Update Mode On vs Off

**Update Mode On:**
- Changes saved to database
- New files added to database
- This run becomes the new baseline
- Next scan compares against this run

**Update Mode Off (Read-Only):**
- Changes detected but not saved
- Database unchanged after scan
- Useful for verification without committing changes
- Next scan still compares against previous update

### Checksum On vs Off

**Checksum On:**
- Full SHA-256 hash computed for each file
- Detects any content changes
- Slower but thorough
- Recommended for archival verification

**Checksum Off:**
- Only modification time and size compared
- Much faster
- May miss silent corruption if mtime unchanged
- Good for quick change detection

## Integration with CLI Tools

The GUI and CLI tools share the same databases:

```bash
# Scan with CLI
file_tracker -p /Volumes/MyDrive -u -c

# View with GUI
ft_summary_gui
# Select "MyDrive" from dropdown

# Or view with CLI
ft_summary -d MyDrive -a

# Export specific file list
ft_logs -d MyDrive -r MyDrive-2026-09-11-14-00-00 -M > missing.txt
```

All views show the same underlying data.

## Tips and Best Practices

### Regular Monitoring
- Check summary after each file_tracker run
- Look for trends (increasing changed/missing files)
- Export details for important runs

### Investigating Issues
- High "Changed" count with checksums → investigate for corruption
- "Missing" files → check if drive mounted correctly
- Unexpected "New" files → verify they're legitimate

### Database Selection
- Organize databases by purpose (e.g., `photos`, `documents`, `backups`)
- Use descriptive names when running file_tracker
- One database per logical volume/archive

### Notes Field
- Always add notes when running file_tracker (`-t` option)
- Helps identify runs later ("Before migration", "After cleanup")
- Visible in Details tab

### Performance
- Large file lists may take a moment to load
- Resize columns to see full paths
- Use export for very long lists (easier to search in text editor)

## Troubleshooting

**No databases shown:**
- Ensure you've run `file_tracker` at least once
- Check `~/db/FileTracker/` directory exists
- Verify databases have `.db` extension

**"Error: Could not open database":**
- Database file may be locked
- Check file permissions
- Ensure database isn't corrupted (try opening with `sqlite3`)

**Run list empty:**
- Database may have been created but never scanned
- Check database with `ft_summary -d <name>` via CLI
- Verify database has `meta` table

**File lists show "(No files found)":**
- Scan may not have tracked file-level changes
- Only runs with `-u` flag create log entries
- Check if `run_logs` table exists in database

**Export button does nothing:**
- Ensure a database and run are selected
- Check current directory write permissions
- Look for exported file in current working directory

## Keyboard Shortcuts

- **Tab:** Navigate between UI elements
- **Up/Down Arrows:** Navigate run list
- **Enter:** (on run list) Select run
- **Ctrl+Tab:** Switch between tabs in details panel

## See Also

**CLI Tools:**
- **ft_summary** - Command-line version with additional filtering
- **ft_logs** - View detailed per-file logs with more filtering options
- **file_tracker** - Scan directories and create/update databases
- **file_locator** - Search for specific files across databases

**Other GUI Tools:**
- **file_tracker_gui** - Visual interface for scanning directories
- **ft_drives_gui** - Track external drive information

## Example Use Cases

### Monthly Archive Verification
```
1. Run: file_tracker -p /Volumes/Archive -u -c -t "Monthly check"
2. Open: ft_summary_gui
3. Select: "Archive" database
4. Review: Latest run details
5. Check: Missing and Changed tabs
6. Export: Details for records
```

### After Drive Migration
```
1. Run: file_tracker -p /new/location -n archive -u -c -t "Post-migration"
2. Open: ft_summary_gui
3. Select: "archive" database
4. Compare: Last two runs
5. Verify: No unexpected missing files
6. Check: New files are expected
```

### Investigating Corruption
```
1. Open: ft_summary_gui
2. Select: Affected database
3. Check "Show All Runs"
4. Find: When "Changed" count increased
5. Compare: Changed files between runs
6. Export: Changed files list for analysis
```

The File Tracker Summary GUI provides a powerful visual interface for understanding your file tracking data and maintaining the integrity of your archival storage.

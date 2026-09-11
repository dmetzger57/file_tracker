# File Tracker GUI Guide

## Overview

The File Tracker GUI provides a visual interface for scanning directories and tracking file changes with SHA-256 checksums. It offers the same functionality as the CLI `file_tracker` tool with real-time progress updates and an intuitive interface.

## Main Window

The application window is organized with a two-panel layout:

### Left Panel: Mounted Volumes

Shows all drives and volumes mounted under `/Volumes/`:

**Features:**
- Lists all mounted volumes (external drives, network shares, etc.)
- Displays volume name and capacity information
- Shows space usage (total, free, percentage used)
- Click any volume to auto-populate the scan path
- "Refresh" button to reload the volumes list

**Interaction:**
- Click a volume to automatically set it as the scan path
- Database name is auto-filled with the volume name
- Makes it easy to scan external drives without typing paths

### Right Panel: Configuration Section

**Scan Path:**
- Auto-filled when you click a volume from the list
- Can also be manually typed
- "Browse..." button to select any directory visually
- Supports both mounted volumes and any file path

**Database Name:**
- Optional field for custom database name
- If left empty, uses the directory name as the database name
- Database stored in `~/db/FileTracker/<name>.db`

**Scan Options:**
- **Enable Checksum Verification** - Compute SHA-256 hashes for all files
  - When unchecked: Only compares modification times (faster)
  - When checked: Full checksum verification (slower but thorough)
- **Update Database** - Save changes to the database
  - When checked: Updates database with new/changed/missing files
  - When unchecked: Read-only mode (reports changes without saving)

**Note Field:**
- Multi-line text area for adding context to this scan
- Examples: "Weekly verification", "Post-migration check", "Before disk archive"
- Saved in database metadata for future reference
- Optional but recommended for audit trails

### Control Buttons

**Start Scan:**
- Green button to begin scanning
- Disabled during active scan
- Validates that a path is selected before starting

**Stop:**
- Red button to abort scan in progress
- Only enabled during active scans
- Stops after completing current file

### Progress Section

**Progress Bar:**
- Visual indicator of scan completion
- Updates in real-time as files are processed
- Fills from 0% to 100%

**Status Line:**
- Shows running totals for:
  - **Unchanged:** Files that haven't changed
  - **Changed:** Files modified since last scan
  - **New:** Files not in database
  - **Missing:** Files in database but not found on disk
  - **Errors:** Files that couldn't be processed

**Current File:**
- Shows the filename currently being processed
- Updates continuously during scan
- Ellipsized at start if path is too long

### Results Display

Large text area showing scan completion summary:
- Scan configuration used
- Complete file counts for all categories
- Total files processed
- Database location

Results persist after scan completion for review.

## Workflow Examples

### Initial Scan (Building Database)

**Using Mounted Volumes List:**
1. Look at the left panel for mounted volumes
2. Click on the drive you want to scan (e.g., "BackupDrive2024")
3. Path and database name are auto-filled
4. Check "Enable Checksum Verification"
5. Check "Update Database"
6. Add note: "Initial baseline scan"
7. Click "Start Scan"
8. Wait for completion
9. Review results

**Or Using Manual Path:**
1. Click "Browse..." and select your directory
2. Leave "Database Name" empty to use directory name
3. Check "Enable Checksum Verification"
4. Check "Update Database"
5. Add note: "Initial baseline scan"
6. Click "Start Scan"
7. Wait for completion
8. Review results

This creates the initial database with checksums for all files.

### Verification Scan (Detecting Changes)

1. Browse to previously scanned directory
2. Database name auto-matches from path
3. Check "Enable Checksum Verification"
4. Check "Update Database"
5. Add note: "Monthly verification check"
6. Click "Start Scan"
7. Monitor for Changed/Missing files
8. Review results

Detects any files that have changed or gone missing.

### Read-Only Check (No Database Changes)

1. Select directory
2. Check "Enable Checksum Verification"
3. **Uncheck** "Update Database"
4. Click "Start Scan"
5. Review results without modifying database

Useful for verification without committing changes.

### Fast Scan (Modification Time Only)

1. Select directory
2. **Uncheck** "Enable Checksum Verification"
3. Check "Update Database"
4. Click "Start Scan"

Much faster but only detects changes via modification time, not actual content changes.

## Understanding Results

### Unchanged Files
Files that match the database exactly (size, mtime, and checksum if enabled).

### Changed Files
Files where:
- Size changed, OR
- Modification time changed, OR
- Checksum differs (when checksum verification enabled)

### New Files
Files found on disk but not in the database. Only saved if "Update Database" is checked.

### Missing Files
Files in the database but not found on disk. Could indicate:
- Deleted files
- Moved files
- Renamed files
- Unmounted drive

### Errors
Files that couldn't be processed due to:
- Permission denied
- File locked
- I/O errors
- Path too long

## Automatic Drive Tracking

When "Update Database" is checked, the GUI automatically registers the scanned drive with the drive tracking system (`ft_drives`).

**What Happens:**
- After scan completes, checks if drive exists in drive tracker
- If not found, automatically adds it with:
  - Drive name from database name
  - Auto-detected capacity information
  - Description: "Auto-added by file_tracker_gui"
  - Current timestamp

**Benefits:**
- No manual drive registration needed
- All scanned drives automatically tracked
- Seamless integration with drive management tools
- Can edit drive details later with `ft_drives` or `ft_drives_gui`

**Example Workflow:**
1. Scan external drive "BackupDrive2024" with Update Database checked
2. Drive is automatically added to drive tracker
3. View drive info: `ft_drives show BackupDrive2024`
4. Or manage in `ft_drives_gui`

**Note:** Only happens when "Update Database" is checked. Read-only scans don't add drives.

## Integration with CLI Tools

The GUI uses the **same database schema** as CLI `file_tracker`, so:

- Databases created by GUI can be queried by CLI tools
- Databases created by CLI can be opened in GUI
- Use `ft_summary` to view scan history
- Use `ft_logs` to see detailed per-file logs
- Use `file_locator` to search for specific files

Example workflow:
```bash
# Scan with GUI
file_tracker_gui  # Creates ~/db/FileTracker/MyDrive.db

# View history with CLI
ft_summary -d MyDrive -a

# Search for a file
file_locator -f "important.doc" -d MyDrive.db
```

## Performance Tips

1. **First scan is slow:** Computing checksums for large directories takes time
2. **Subsequent scans are faster:** Only changed files need checksum recomputation
3. **Disable checksum for speed:** Uncheck verification for quick modification time checks
4. **Stop and resume:** Use Stop button if needed; run again later to continue
5. **Monitor current file:** Watch for stuck files (may indicate I/O issues)

## Database Location

All databases stored in: `~/db/FileTracker/<database_name>.db`

Multiple scans of the same directory should use the same database name to track changes over time.

## Troubleshooting

**GUI won't start:**
- Ensure GTK4 is installed: `brew install gtk4`
- Check for error messages in terminal

**Scan seems stuck:**
- Check "Current File" to see what file is being processed
- Large files take longer for checksum computation
- Use Stop button to abort if needed

**No files detected:**
- Verify the scan path is correct
- Check directory permissions
- Ensure directory is mounted (for external drives)

**Changed files showing as unchanged:**
- Enable "Checksum Verification" for content-based comparison
- Without checksums, only modification time is checked

**Results not saving:**
- Ensure "Update Database" is checked
- Verify write permissions to `~/db/FileTracker/`

## Best Practices

1. **Use descriptive notes:** Document why you're scanning
2. **Enable checksums for verification:** Especially for archival storage
3. **Run periodic scans:** Monthly or quarterly for archives
4. **Check for Missing files:** May indicate hardware failures
5. **Review Changed files:** Unexpected changes may indicate corruption
6. **Keep scan notes:** Build audit trail over time

## Keyboard Shortcuts

- **Tab:** Navigate between fields
- **Escape:** (in dialogs) Cancel operation
- **Ctrl+C:** (in terminal) Stop application

## See Also

- **ft_summary** - View scan history and statistics
- **ft_logs** - View detailed per-file change logs
- **file_locator** - Search for files across databases
- **ft_drives** - Track drive information and verification dates

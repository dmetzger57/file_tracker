# File Tracker

A comprehensive file tracking application using SHA-256 checksums to detect bit-rot and silent corruption on archival storage. All functionality is unified in a single GTK4 application with a tabbed interface.

## Quick Start

### Installation

```bash
# Build the application
make

# Create macOS app bundle (optional)
make apps
```

### Launch

```bash
# From command line
./file_tracker_unified

# Or double-click "File Tracker Unified.app" in Finder
```

## Overview

**File Tracker Unified** is an all-in-one application that provides:

- **File Scanner** - Scan directories and compute SHA-256 checksums
- **Summary** - View scan history and statistics  
- **Logs** - Browse detailed per-file change logs
- **Drives** - Track external drive information
- **Locator** - Search for files across databases
- **DupeFinder** - Find duplicate files by name or checksum
- **Compare** - Compare two scan runs to see what changed

All tools share the same SQLite database format. Scan once, query many ways.

## Features

### File Scanner Tab

Recursively scans directory trees, computes SHA-256 hashes, and stores file metadata in SQLite databases. Multi-threaded for performance.

**Features:**
- **Mounted Volumes List:** Shows all drives mounted at `/Volumes/` with capacity info (Time Machine volumes, `mbp_backup` and `Macintosh HD` are hidden; edit `is_excluded_volume()` in `file_tracker_unified.c` to change this)
- **Directory Selection:** Browse button or manual path entry
- **Scan Options:**
  - Enable Checksum Verification (SHA-256) - compare checksums even when mtime unchanged
  - Update Database mode - save changes vs read-only scan
  - Record Ignored Files - track files matching `.rsync-ignore` patterns
- **Note Field:** Add contextual notes to scan runs for audit trail
- **Live Progress:** Real-time file counts and current file being processed
- **Results Display:** Detailed summary upon completion

**Typical Workflow:**
1. Select a mounted drive from the volumes list (or browse to any path)
2. Choose database name (auto-fills from volume name)
3. Enable "Update Database" to save changes
4. Optionally enable "Enable Checksum Verification" for deep scan
5. Add a note (e.g., "Monthly verification - Dec 2024")
6. Click "Start Scan"

**File Status Values:**
- **UNCHANGED** - File matches database record
- **CHANGED** - Metadata or checksum differs from stored value
- **NEW** - File found on disk, not in database
- **MISSING** - In database, not found on disk
- **IGNORED** - Matched `.rsync-ignore` pattern (if "Record Ignored Files" enabled)
- **ERROR** - Processing failed (path too long, permission denied, etc.)

**Drive Integration:** Scanned drives are automatically registered in the drive tracking database.

### Summary Tab

View scan history and statistics for any database. Shows run-level summaries and per-file details.

**Features:**
- **Database Selector:** Choose from tracked drives or browse to any `.db` file
- **Run History:** List of all scans with date, counts, and notes
- **Tabbed File Lists:**
  - Changed Files (with details on what changed)
  - New Files
  - Missing Files
  - Unchanged Files
  - Errors
- **Metadata Display:** Run notes and verification dates
- **Search/Filter:** Find specific files in results

### Logs Tab

Browse detailed per-file change logs with filtering.

**Features:**
- **Run Selection:** View logs from any historical scan
- **Status Filters:** Filter by CHANGED, NEW, MISSING, ERROR, IGNORED, ALL
- **File List:** Full paths of files matching filter
- **Quick Stats:** Counts for each status category
- **Export:** Save the records matching the current filter to a CSV file (`Status,Full Path`) via a file-save dialog

### Drives Tab

Track external drive metadata, verification history, and storage location.

**Features:**
- **Drive List:** All tracked drives with capacity, description, location
- **Add Drive:** Register new drives with auto-detected capacity
- **Verify Drive:** Mark drive as verified on current date
- **Delete Drive:** Asks whether to remove only the drive entry (keeping its database and scan history) or to also permanently delete its database file
- **Rename Drive:** Select a drive and click Rename. The drive's database file is renamed and its entry updated, keeping all scan history. To keep scanning into the same database afterwards, set the Scanner's database name to the new name
- **Edit Metadata:** Update description, physical location notes
- **Last Checksum Scan:** Date of the drive's most recent scan run with checksum verification enabled (sortable; "Never" if none, "No database" if the drive has no database file)
- **Auto-Discovery:** Detects drives when scanning (no manual registration needed); the same excluded volumes as the Scanner list are skipped

### Locator Tab

Search for files across all tracked databases by filename. Compare checksums between databases to find duplicates or verify copies.

**Features:**
- **Filename Search:** Find files by name across all databases
- **Multi-Database Results:** Shows which databases contain matching files
- **Checksum Comparison:** Automatic duplicate detection
- **Path Information:** Full paths and database locations

### DupeFinder Tab

Find every copy of a file, either by name or by checksum, within one drive or across all drives.

**Features:**
- **Search By:** File Name or File Checksum
- **Scope:** A single database (drive) or All Databases
- **File Name Wildcard:** `*` matches any run of characters (e.g. `*File*should*`); other characters, including `?` and `[`, match literally. Matching is case-sensitive
- **Checksum Search:** Exact SHA-256 match, case-insensitive
- **Results:** File Name, Drive, Checksum, Full Path, and Last Checksum Calculation (sortable columns)
- **Notes:** Files marked MISSING are excluded. Checksum dates are not stored per file, so the date shown is the drive's latest scan run with checksum verification enabled

### Compare Tab

Compare two scan runs from the same database to see exactly what changed between dates.

**Features:**
- **Run Selection:** Pick any two runs from database history
- **Change Detection:**
  - Files with different checksums
  - Files with different modification times
  - Files with different sizes
  - Status changes (NEW → UNCHANGED, etc.)
- **Filter Options:**
  - Checksum differences only
  - Date/time differences only  
  - Size differences only
  - All differences combined
- **Detailed View:** Shows old vs new values for each changed attribute

## Command Line Scanner

`file_tracker` scans a directory from the terminal using the same databases as `file_tracker_unified` (`~/db/FileTracker/<last path component>.db`, the same name the Scanner tab derives).

```bash
file_tracker -s /Volumes/Archive            # record the run, file details untouched
file_tracker -s /Volumes/Archive -u -n "Weekly check"    # update file details and record the run
file_tracker -s /Volumes/Archive -u -v      # deep verification (SHA-256)
```

| Option | Meaning |
|--------|---------|
| `-s path` | Directory to scan (required) |
| `-v` | Verify via SHA-256 checksum; without it, compare size and modification time |
| `-u` | Update file details in the database; without it the run is still recorded but the files table is not changed |
| `-n note` | Note stored with the run |

Output is one line: `Total: X - Processed: X - New: X - Changed: X - Unchanged: X - Missing: X - Errors: X`. `make` builds both binaries.

### Missing Files

Both the CLI and the Scanner tab report files that are recorded in the database under the scan path but no longer exist on disk. With updates enabled (`-u` / "Update Database") these rows are marked `status = 'MISSING'` in the `files` table and skipped, not reported again, on later scans. A file that reappears is reported as New and its status is cleared. Older databases gain the `status` column automatically on the next scan. Changed files always have their checksum refreshed in the database when updates are enabled.

## Database Structure

### Storage Location
Databases are stored in `~/db/FileTracker/`

### Primary Tables

**files:** File metadata and checksums
- `id, file_name, full_path (UNIQUE), size, created, last_modified, owner, checksum, keywords, status`

**meta:** Scan run summaries
- `id, last_checksum_verify_date, last_date_verify, verify_machine, num_unchanged, num_changed, num_new, num_missing, num_errors, update_mode, note`

**run_logs:** Detailed per-file status for each run
- `id, run_id (FK to meta.id), status, full_path, checksum, last_modified, size`
- Enables historical comparison and change tracking

**drives:** External drive tracking (in `drives.db`)
- `id, drive_name, description, capacity_gb, physical_location, last_verified_date, last_updated`

### Ignore Patterns
Files matching patterns in `~/.rsync-ignore` can optionally be recorded with status `IGNORED` or skipped entirely.

## Building from Source

### Dependencies

**Required:**
- GCC
- OpenSSL 3 (`libssl`, `libcrypto`)
- SQLite3
- GTK4
- pthreads

**macOS Installation:**
```bash
brew install openssl@3 sqlite gtk4
```

### Build Commands

```bash
# Build application
make

# Build and create .app bundle
make apps

# Clean build artifacts
make clean

# Install to ~/bin
make install
```

### Manual Compilation

```bash
gcc -Wall -Wextra -O2 \
  -I/opt/homebrew/opt/openssl@3/include \
  -L/opt/homebrew/opt/openssl@3/lib \
  `pkg-config --cflags --libs gtk4` \
  -o file_tracker_unified file_tracker_unified.c \
  -lssl -lcrypto -lsqlite3
```

## Use Cases

### Initial Baseline
Create a reference database for a new external drive:
1. Connect drive
2. Select from volumes list in Scanner tab
3. Enable "Update Database"
4. Add note: "Initial baseline"
5. Start scan

### Periodic Verification (Quick)
Fast verification using modification time only:
1. Select drive
2. Enable "Update Database"
3. **Disable** "Enable Checksum Verification" (mtime comparison only)
4. Add note: "Weekly quick check"
5. Start scan

### Deep Verification (Full Checksum)
Monthly full checksum verification:
1. Select drive
2. Enable "Update Database"
3. **Enable** "Enable Checksum Verification" (full SHA-256)
4. Add note: "Monthly deep scan"
5. Start scan

### Investigate Changes
1. Go to Summary tab
2. Select database
3. View Changed Files tab
4. Or use Compare tab to diff two specific runs

### Find Duplicates
1. Go to DupeFinder tab
2. Choose File Name or File Checksum and enter the value (`*` is a wildcard in names)
3. Choose a database or All Databases
4. Results list every copy with its drive, path, and checksum

## Performance

- **Speed:** ~500MB/s on SSD (checksum mode), ~10GB/s (mtime-only mode)
- **Threading:** Multi-threaded scanner (one thread per path when scanning multiple paths)
- **Bottlenecks:** I/O (reading files), SHA-256 computation

## Logs

Scan logs are written to `~/logs/FileTracker/` with timestamps.

## macOS App Bundle

After running `make apps`, you get:
- **File Tracker Unified.app** - Complete application with all tabs, using the icon in `icon/AppIcon.icns`

Double-click to launch without terminal window. Optionally move to `/Applications/` for easy access.

## Architecture Notes

- Written in C
- GTK4 for GUI
- SQLite for storage  
- Multi-threaded scanning (pthreads)
- SHA-256 via OpenSSL

All functionality (scan, query, compare, manage) is in one unified application. No need to switch between multiple tools.

## Support

For issues or questions, refer to the source code or documentation in the repository.

# File Tracker

File Tracker detects bit-rot and silent corruption on archival storage. It records every file's size, modification time and SHA-256 checksum in a SQLite database, then re-scans the drive later and reports what changed, what is new, and what has gone missing.

The repository contains two programs that share the same databases:

| Program | Source | What it is |
|---------|--------|------------|
| `file_tracker_unified` | `file_tracker_unified.c` | GTK4 desktop application with tabs for scanning, browsing results, managing drives and finding duplicates. Packaged as **File Tracker Unified.app** |
| `file_tracker` | `file_tracker.c` | Command-line scanner, for scripts and scheduled scans |

## Quick Start

```bash
brew install openssl@3 sqlite gtk4   # dependencies (macOS)
make                                 # builds file_tracker_unified and file_tracker
make apps                            # optional: creates File Tracker Unified.app
```

Launch the application:

```bash
./file_tracker_unified
# or double-click "File Tracker Unified.app" in Finder
```

The `.app` bundle launches without a terminal window. Double-clicking the raw `file_tracker_unified` executable in Finder opens a terminal as well.

## How It Works

1. **Baseline:** scan a drive with *Update Database* enabled. Every file is recorded in `~/db/FileTracker/<drive name>.db`.
2. **Verify:** scan the same drive again later. Each file is compared with its stored record and given a status. The scan is saved as a *run*, with totals and a per-file log.
3. **Investigate:** use the Summary, Logs and Compare tabs to see what changed and when.

A quick verification compares only size and modification time. A deep verification recomputes every SHA-256 checksum, which also catches corruption that leaves the modification time unchanged.

### File Status Values

| Status | Meaning |
|--------|---------|
| **UNCHANGED** | File matches its database record |
| **CHANGED** | Size, modification time or checksum differs. The log says which (e.g. `CHANGED: CheckSum, Date-Time`) |
| **NEW** | On disk but not in the database, or back on disk after being marked MISSING |
| **MISSING** | In the database but no longer on disk |
| **IGNORED** | Matches a pattern in `~/.rsync-ignore` (see [Ignore Patterns](#ignore-patterns)) |
| **ERROR** | Could not be processed (permission denied, path too long, unreadable) |

With *Update Database* on (`-u` on the command line), changed files have their stored size, time and checksum updated, new files are added, and missing files are marked `status = 'MISSING'` so they are reported only once. Without it, the run and its log are still saved but the `files` table is left alone.

## Desktop Application

The tabs appear in this order: File Locator, DupeFinder, Drives, Summary, Logs, Compare, File Scanner, About.

### File Scanner

Scans one directory tree and saves the results as a run.

- **Volumes:** drives mounted under `/Volumes`. Time Machine volumes, `mbp_backup` and `Macintosh HD` are hidden (see `is_excluded_volume()`). Clicking a volume fills in Path and Database. **Refresh** reloads the list.
- **Path / Database:** the directory to scan and the database name (stored as `~/db/FileTracker/<name>.db`). Both can be typed in.
- **Verify Checksums:** recompute SHA-256 for every file (deep verification). Off: compare size and modification time only.
- **Update Database:** save file changes to the database (see above).
- **Workers:** how many files are processed in parallel (1 to 2× CPU cores, default 1). Keep it at 1 for spinning hard drives, where parallel reads cause head seeking. Raise it for checksum scans on SSD/NVMe.
- **Note:** text saved with the run, e.g. "Monthly deep scan".
- **Start Scan / Stop:** progress, the file being processed, and a summary when the scan finishes.

The scanned drive is registered in the drives database automatically, and its capacity and free space are recorded.

### Summary

Pick a database to see its 10 most recent runs, with counts of unchanged, changed, new, missing, ignored and error files. Select a run to see its totals. **Refresh** reloads the database list and runs.

### Logs

The detailed, per-file log of any run.

- **Database / Scan Runs:** pick a database, then a run. **Refresh** reloads the database list and runs.
- **Filters:** All, New, Changed, Missing, Unchanged, Ignored, Errors.
- **Note / Save Note:** view or edit the run's note.
- **Export:** save the files matching the current filter as CSV (`Status,Full Path`).
- **Delete Selected Run:** remove a run and its log.

### Compare

Compares two runs, from the same database or from two different ones (for example a drive and its backup).

- Pick a drive and run for Run 1 and Run 2, then click **Compare Runs**. **Refresh** reloads the drive and run lists and keeps the selected runs.
- **View By:** Only in Run 1, Only in Run 2, Different Checksum, All Diffs, Missing in Run 1, Missing in Run 2, All Files in Run 1, All Files in Run 2, Date/Time Difference, Size Difference.
- **Export to CSV:** `File Path,Status Run 1,Status Run 2,Checksum Run 1,Checksum Run 2`.

### Drives

Tracks the drives you have databases for, in `~/db/FileTracker/drives.db`.

| Column | Meaning |
|--------|---------|
| Name, Location, Description | The drive's name (also its database name), where it is kept, and notes |
| Available | Free space when last updated |
| Last Checksum Scan | Date of the most recent run with checksum verification ("Never" if none, "No database" if the drive has no database file) |
| Files (Last Run) | Files in the most recent run: unchanged + changed + new + missing + errors, the same total the Summary tab shows (ignored files are not counted) |

Name, Location, Last Checksum Scan and Files (Last Run) are sortable.

- **Add / Update:** register a drive, or save changes to the selected drive's location and description.
- **Rename:** rename the drive and its database file, keeping all scan history. Afterwards, scan with the new database name.
- **Delete:** remove only the drive entry (keeping its database), or also permanently delete its database file.
- **Refresh:** reload the list.
- **Update Mounted Drives:** refresh capacity and free space for every tracked drive that is currently mounted.

### File Locator

Find a file across databases.

- **Search by:** File Name (exact, or tick **Partial** to match part of the name) or Checksum.
- **Database:** one database or All Databases. **Refresh** reloads the list.
- **Results:** database, path, size, modified date, owner and checksum. The ✓ column shows ✓ when a result's checksum matches the first result, and ⚠ when it differs, so copies that have diverged stand out.

### DupeFinder

Find every copy of a file, or list every set of duplicates, within one drive, a chosen set of drives, or all drives.

**Modes:**
- **Search By File Name / Checksum:** find every copy of one file name or checksum. In names, `*` matches any run of characters (e.g. `*File*should*`); other characters, including `?` and `[`, match literally. Matching is case-sensitive. Checksum search is an exact SHA-256 match, case-insensitive.
- **All Files With Matching Names:** every file whose exact file name (not path) appears more than once. Case-sensitive.
- **All Files With Matching Name + Checksum:** every file whose name and checksum both match another file. Files without a checksum are left out.

**Scope:** search mode takes one database or All Databases. The two "All Files" modes take any set of databases from a checkbox list, or All Databases. **Refresh** reloads the database list and keeps your ticks.

**Results:** File Name, Drive, Checksum, Full Path and Last Checksum Calculation (all sortable). In the "All Files" modes, duplicates are grouped together and sorted by file name, and the status line shows the number of files, groups and drives. Files marked MISSING are excluded. Checksum dates are not stored per file, so the date shown is the drive's latest run with checksum verification.

### Picking Up New Data

Each tab loads its database list when the application starts. Use the tab's **Refresh** button to pick up drives, databases or runs created since then, for example by a `file_tracker` scan from the command line. Refresh keeps the currently selected database where it still exists.

## Command-Line Scanner

`file_tracker` scans a directory from the terminal using the same databases as the application. The database is `~/db/FileTracker/<last path component>.db`, the same name the File Scanner tab uses.

```bash
file_tracker -s /Volumes/Archive                        # record the run, file details untouched
file_tracker -s /Volumes/Archive -u -n "Weekly check"   # update file details and record the run
file_tracker -s /Volumes/Archive -u -v                  # deep verification (SHA-256)
```

| Option | Meaning |
|--------|---------|
| `-s path` | Directory to scan (required) |
| `-v` | Verify via SHA-256 checksum; without it, compare size and modification time |
| `-u` | Update file details in the database; without it the run is still recorded but the `files` table is not changed |
| `-n note` | Note stored with the run |

Output is one line: `Total: X - Processed: X - New: X - Changed: X - Unchanged: X - Missing: X - Errors: X`. Status rules, missing-file handling, ignore patterns and drive registration are the same as in the File Scanner tab. The run then shows up in the application's Summary, Logs and Compare tabs after a **Refresh**.

## Ignore Patterns

Both programs read `~/.rsync-ignore` when they start, so the same file can drive your rsync backups. Matching follows rsync's rules:

| Pattern | Matches |
|---------|---------|
| `.DS_Store` | Any file or directory with that exact name, at any depth |
| `._*`, `*.swp` | Names matching the wildcard (`*`, `?`, `[...]`), at any depth |
| `.AppleDouble/` | A trailing `/` limits the pattern to directories |
| `/.fseventsd/` | A leading `/` (or any `/` in the pattern) anchors it to the top of the scan, so this matches `.fseventsd` at the root of the scanned drive but not `docs/.fseventsd` |
| `# comment`, blank lines | Skipped |

Ignored entries are logged as IGNORED and counted in the run's ignored total. Ignored directories are not scanned inside. With *Update Database* on, ignored regular files are added to the `files` table without a checksum. Files recorded before a pattern matched them are not reported as MISSING when they later disappear.

Anchored patterns are relative to the scanned directory. When you scan a whole drive that is the drive's root. When you scan a subfolder, it is that subfolder.

## Database Structure

Each drive has its own database, `~/db/FileTracker/<drive name>.db`, plus one shared `~/db/FileTracker/drives.db`.

**files:** one row per file
- `id, file_name, full_path (UNIQUE), size, created, last_modified, owner, checksum, keywords, status`

**meta:** one row per run
- `id, last_checksum_verify_date, last_date_verify, verify_machine, num_unchanged, num_changed, num_new, num_missing, num_ignored, num_errors, update_mode, note`
- `last_checksum_verify_date` is set only for runs with checksum verification

**run_logs:** the per-file log of each run
- `id, run_id (→ meta.id), status, full_path, checksum, size, mtime`

**drives** (in `drives.db`)
- `drive_id, drive_name (UNIQUE), capacity, space_available, space_used, description, last_updated, last_verified, storage_container`
- `storage_container` is the Location shown in the Drives tab

Missing columns (such as `files.status`) are added automatically to older databases on their next scan. The `migrate_*.sh` scripts upgrade databases created by much older versions (adding `run_logs`, `meta.note` and `run_logs.checksum`). They are safe to run more than once, and with no arguments they migrate every database in `~/db/FileTracker/`.

## Building

### Dependencies

- GCC (or Clang)
- OpenSSL 3 (`libssl`, `libcrypto`)
- SQLite3
- GTK4 (the application only; also provides GLib threads)

On macOS: `brew install openssl@3 sqlite gtk4`. The Makefile adds the Homebrew include and library paths automatically.

### Make Targets

| Target | What it does |
|--------|--------------|
| `make` | Build `file_tracker_unified` and `file_tracker` |
| `make apps` | Build `file_tracker_unified` and create **File Tracker Unified.app** (`create_app_bundles.sh`) |
| `make install` | Copy **File Tracker Unified.app** to `/Applications` (run `make apps` first) |
| `make clean` | Remove the built binaries |

`make install` does not install `file_tracker`. Copy it somewhere on your `PATH` (e.g. `~/bin`) yourself.

After rebuilding, run `make apps` again so the bundle picks up the new binary. If the app reports missing libraries, check the Homebrew packages above are installed.

## Recommended Workflow

- **Initial baseline:** select the drive in File Scanner, enable *Update Database* and *Verify Checksums*, add a note such as "Initial baseline", and start the scan.
- **Weekly quick check:** same, with *Verify Checksums* off.
- **Monthly deep check:** same, with *Verify Checksums* on. Only this catches content changes that leave the size and modification time untouched.
- **Investigate:** Summary for the totals, Logs filtered to Changed / Missing / Errors for the files, Compare to diff two runs or a drive against its backup.

## Performance

- Throughput is roughly 500 MB/s on SSD with checksums, and about 10 GB/s of data covered in modification-time-only mode.
- Reading files and SHA-256 hashing are the bottlenecks. On SSD/NVMe, raising Workers hashes several files at once. On spinning drives, keep it at 1.

## Repository Layout

| Path | Contents |
|------|----------|
| `file_tracker_unified.c` | Desktop application |
| `file_tracker.c` | Command-line scanner |
| `Makefile`, `create_app_bundles.sh`, `icon/` | Build and `.app` packaging |
| `migrate_*.sh`, `migrate_*.sql` | Database migrations for old databases |
| `file_tracker_lastrun.c` | Legacy utility for the old database format (reads a `metadata` table current databases do not have). Not built |
| `launcher.c`, `launcher.m` | Legacy `.app` launchers. Not used: the bundle now runs the real binary directly |
| `CLAUDE.md` | Notes for AI-assisted development |

# File Tracker

A suite of command-line tools for tracking files via SHA-256 checksums stored in SQLite databases. Designed for detecting bit-rot and silent corruption on archival storage drives.

Run `file_tracker` against your storage media periodically to detect unauthorized changes or silent file corruption via checksum verification.

## Tools

### file_tracker

The core engine. Recursively scans directory trees, computes SHA-256 hashes, and stores file metadata in per-path SQLite databases. On subsequent runs, detects new, changed, and missing files by comparing against stored records. Spawns one thread per path for parallel processing.

```
file_tracker -p path1,path2,pathN [-n db_name] [-c] [-u] [-v] [-l] [-L] [-s] [-t note] [-N notefile]
```

| Option | Description |
|--------|-------------|
| `-p`   | Paths to scan (required, comma-separated). One thread per path. |
| `-n`   | Database file name (without `.db` extension). Stored in `~/db/FileTracker/`. When omitted, the database is named after each path's basename. When provided with multiple paths, all paths share the same database file. |
| `-c`   | Compare checksums even when file modification time is unchanged. |
| `-u`   | Update the database with changes. Without this, differences are only reported. |
| `-v`   | Verbose output. |
| `-l`   | Live view — show current filename being processed (updates on every file). |
| `-L`   | Live view with full path instead of filename. Implies `-l`. |
| `-s`   | Print an aggregate summary when processing completes. |
| `-t`   | Add a note to this run's metadata (requires `-u`). Use for documenting the purpose of the scan. |
| `-N`   | Read note text from a file and attach it to this run's metadata (requires `-u`). Useful for multi-line notes. |

**Default behavior (no `-c`):** Files are compared by modification time only. If the mtime matches the stored value, the file is marked unchanged without recomputing its hash. Use `-c` for a full checksum verification pass.

**Notes:** Use `-t` or `-N` to attach contextual information to each run (e.g., "Weekly backup", "Post-migration verification"). Notes are stored in the database's `meta` table and can be queried later for audit purposes. When running without `-u` (read-only mode), the previous run's note is automatically displayed after the summary (if `-s` is enabled) to provide context about the last update.

### file_locator

Search tracker databases for a specific file by name. Scans all `.db` files in `~/db/FileTracker/` by default, or a single database when specified. When the same filename appears across multiple databases (or multiple times within one), `file_locator` compares checksums against the first match and flags any that differ — useful for verifying that copies of a file on different volumes are identical.

```
file_locator -f filename [-p] [-d database] [-v]
```

| Option | Description |
|--------|-------------|
| `-f`   | Filename to search for (required). |
| `-p`   | Partial match. Wraps the filename in SQL `%` wildcards so `LIKE` matches any path containing the string. |
| `-d`   | Search only the named database file (relative to `~/db/FileTracker/`) instead of all databases. |
| `-v`   | Verbose output: prints full metadata for each match (ID, full path, size, created, last modified, owner, checksum). |

**Default (compact) output:** One line per match showing the database name and full path. If a match's checksum differs from the first result, `, Checksum Mismatch` is appended.

**Exit code:** Returns the number of matches found (0 = no matches).

### ft_summary

Report run history and statistics from a tracker database.

```
ft_summary -d database [-a] [-N] [-m] [-c] [-n]
```

| Option | Description |
|--------|-------------|
| `-d`   | Database name, without the `.db` extension (required). |
| `-a`   | Show all recorded runs. Default: last run only. |
| `-N`   | Show run notes (Run #, Run Date, Note). Displays "None" for runs without notes. |
| `-m`   | List files found missing in the last run. |
| `-c`   | List files found changed in the last run. |
| `-n`   | List files found new in the last run. |

**Default output format:** Run #, Run Date, Update (On/Off), Checksum (On/Off), Unchanged, Changed, New, Missing, Errors

**Notes output format (`-N`):** Run #, Run Date, Note

### ft_logs

View detailed log messages from a specific file_tracker run, or list all runs in a database. All file operations (NEW, CHANGED, UNCHANGED, MISSING) are stored in the database and can be queried by run identifier.

```
ft_logs -d database_name [-l | -r run_identifier [-N] [-C] [-M] [-U] [-n]]
```

| Option | Description |
|--------|-------------|
| `-d`   | Database name, without the `.db` extension (required). |
| `-l`   | List all runs in the database with summary statistics and run identifiers. |
| `-r`   | Run identifier in format `dbname-YYYY-MM-DD-HH-MM-SS` (use `-l` to see available identifiers). |
| `-N`   | Filter: show only NEW file messages. |
| `-C`   | Filter: show only CHANGED file messages (includes "CHANGED (Metadata)" and "CHANGED (Checksum)"). |
| `-M`   | Filter: show only MISSING file messages. |
| `-U`   | Filter: show only UNCHANGED file messages. |
| `-n`   | Display the note associated with the run. |

**Listing all runs (`-l` option):** Displays a table of all runs showing run identifier, machine, file counts (Unchanged/Changed/New/Missing), read-only mode indicator `[RO]`, and notes if present. Copy the run identifier from this list to use with the `-r` option.

**Viewing specific run:** Use the run identifier displayed by `-l` with the `-r` option. Multiple filters can be combined (e.g., `-N -C` shows both NEW and CHANGED files). If no filters are specified, all log messages are displayed.

**Run identifier format:** `dbname-YYYY-MM-DD-HH-MM-SS` (e.g., `archive-2024-03-15-14-30-45`)

## Storage Layout

| Path | Contents |
|------|----------|
| `~/db/FileTracker/` | SQLite databases (one per scanned path, named after the path's basename unless overridden with `-n`) |
| `~/logs/FileTracker/` | Timestamped log files from each run |
| `~/.rsync-ignore` | Optional ignore list (one entry per line); matched files/directories are skipped |

## Building

Requires GCC, OpenSSL 3, SQLite3, and pthreads.

```sh
make          # build all three tools
make install  # copy binaries to ~/bin
make clean    # remove build artifacts
```

On macOS the Makefile automatically picks up Homebrew paths for OpenSSL and SQLite. On Linux no extra flags are needed if the libraries are installed system-wide.

### Dependencies

| Library | Purpose |
|---------|---------|
| OpenSSL (`libssl`, `libcrypto`) | SHA-256 hashing |
| SQLite3 | File metadata storage |
| pthreads | Multi-path parallel scanning |

**macOS (Homebrew):**

```sh
brew install openssl@3 sqlite
```

**Debian/Ubuntu:**

```sh
sudo apt install libssl-dev libsqlite3-dev
```

**Fedora/RHEL:**

```sh
sudo dnf install openssl-devel sqlite-devel
```

## Example Workflow

```sh
# Initial scan — hash all files and record them in the database with a note
file_tracker -p /mnt/archive -u -s -t "Initial baseline scan"

# Periodic quick check — compare modification times, update database
file_tracker -p /mnt/archive -u -s -t "Weekly verification"

# Read-only verification — see changes without updating, displays previous run's note
file_tracker -p /mnt/archive -s

# Scan multiple paths into a single named database
file_tracker -p /mnt/photos,/mnt/videos -n media_archive -u -s

# Deep verification — recompute and compare every checksum
file_tracker -p /mnt/archive -c -u -s -t "Full checksum verification"

# Add a detailed multi-line note from a file
echo "Post-migration scan
Moved files from old_storage to new_storage
Contact: admin@example.com" > /tmp/scan_note.txt
file_tracker -p /mnt/archive -u -s -N /tmp/scan_note.txt

# Live view — watch files being processed in real-time
file_tracker -p /mnt/archive -u -l -s

# Live view with full paths
file_tracker -p /mnt/archive -u -L -s

# Check what changed
ft_summary -d archive -m -c

# Find a file across all tracked volumes
file_locator -f important_document.pdf

# Partial match — find any tracked file containing "report" in its name
file_locator -f report -p

# Verbose search in a specific database
file_locator -f backup.tar.gz -d archive.db -v

# Query notes from previous runs
sqlite3 ~/db/FileTracker/archive.db \
  "SELECT last_date_verify, note FROM meta WHERE note IS NOT NULL ORDER BY id DESC LIMIT 5;"

# List all file_tracker runs in a database
ft_logs -d archive -l

# View detailed logs from a specific run
ft_logs -d archive -r archive-2024-03-15-14-30-45

# Show only files that were changed in that run
ft_logs -d archive -r archive-2024-03-15-14-30-45 -C

# Show files that were added or changed
ft_logs -d archive -r archive-2024-03-15-14-30-45 -N -C

# Show only unchanged files from a run
ft_logs -d archive -r archive-2024-03-15-14-30-45 -U

# Display the note associated with a run
ft_logs -d archive -r archive-2024-03-15-14-30-45 -n

# Combine note display with file filters
ft_logs -d archive -r archive-2024-03-15-14-30-45 -n -C
```

## Database Migration

If you have existing databases that need schema updates, run the appropriate migration scripts:

### Add note field (if upgrading from versions before note support)
```sh
# Migrate all databases in ~/db/FileTracker/
./migrate_add_note.sh

# Or migrate a specific database
./migrate_add_note.sh ~/db/FileTracker/archive.db
```

### Add run_logs table (if upgrading from versions before log storage)
```sh
# Migrate all databases in ~/db/FileTracker/
./migrate_add_run_logs.sh

# Or migrate a specific database
./migrate_add_run_logs.sh ~/db/FileTracker/archive.db
```

Both migrations are idempotent and safe to run multiple times.

## Database Schema

### files table
Stores individual file metadata with SHA-256 checksums.

| Column | Type | Description |
|--------|------|-------------|
| `id` | INTEGER | Primary key |
| `file_name` | TEXT | Base filename |
| `full_path` | TEXT | Absolute path (unique) |
| `size` | INTEGER | File size in bytes |
| `created` | INTEGER | Creation timestamp (Unix epoch) |
| `last_modified` | INTEGER | Modification timestamp (Unix epoch) |
| `owner` | TEXT | File owner username |
| `checksum` | TEXT | SHA-256 hash (64 hex chars) |
| `keywords` | TEXT | Reserved for future use |

### meta table
Stores run history and statistics.

| Column | Type | Description |
|--------|------|-------------|
| `id` | INTEGER | Primary key (auto-increment) |
| `last_checksum_verify_date` | TEXT | Timestamp of last checksum verification run |
| `last_date_verify` | TEXT | Timestamp of last mtime-only run |
| `verify_machine` | TEXT | Hostname of machine that performed the scan |
| `num_unchanged` | INTEGER | Count of unchanged files |
| `num_changed` | INTEGER | Count of changed files |
| `num_new` | INTEGER | Count of new files |
| `num_missing` | INTEGER | Count of missing files |
| `num_errors` | INTEGER | Count of errors encountered |
| `update_mode` | TEXT | "ON" if `-u` was used, "OFF" otherwise |
| `note` | TEXT | Optional note attached to this run |

### run_logs table
Stores detailed log messages for each run.

| Column | Type | Description |
|--------|------|-------------|
| `id` | INTEGER | Primary key (auto-increment) |
| `run_id` | INTEGER | Foreign key to `meta.id` |
| `status` | TEXT | File status (NEW, UNCHANGED, CHANGED (Metadata), CHANGED (Checksum), MISSING) |
| `full_path` | TEXT | Absolute path of the file |

**Indexes:** `run_id` and `status` are indexed for faster queries.

## License

This project is provided as-is with no warranty. See source files for details.

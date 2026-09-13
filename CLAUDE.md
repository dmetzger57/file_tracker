# File Tracker - Project Context

## Overview
File tracking suite using SHA-256 checksums to detect bit-rot and silent corruption on archival storage. Written in C, uses SQLite for storage, GTK4 for GUIs. All tools (CLI and GUI) share the same database schema.

**Primary Use Case:** Periodic verification of external drives and archival storage to detect unauthorized changes or corruption.

## Architecture

### Tool Categories
1. **Core Scanner:** `file_tracker` (CLI), `file_tracker_gui` (GUI)
2. **Search:** `file_locator` (CLI), `file_locator_gui` (GUI) 
3. **History/Stats:** `ft_summary` (CLI), `ft_summary_gui` (GUI)
4. **Logs:** `ft_logs` (CLI), `ft_logs_gui` (GUI)
5. **Drive Management:** `ft_drives` (CLI), `ft_drives_gui` (GUI)
6. **Utilities:** `ft_find_dupes` (duplicate finder)
7. **Unified:** `file_tracker_unified` (all-in-one GUI with tabs)

### Data Flow
```
file_tracker → SQLite DB (~/db/FileTracker/*.db) ← all other tools read/query
              ↓
         run_logs table (detailed per-file status)
              ↓
         meta table (run summary stats)
```

## Database Schema Quick Reference

### Primary Tables
- **files:** `id, file_name, full_path (UNIQUE), size, created, last_modified, owner, checksum, keywords, status`
  - status: UNCHANGED, CHANGED, NEW, MISSING, IGNORED, ERROR
- **meta:** `id, last_checksum_verify_date, last_date_verify, verify_machine, num_unchanged, num_changed, num_new, num_missing, num_errors, update_mode, note`
- **run_logs:** `id, run_id (FK to meta.id), status, full_path` (indexed on run_id, status)

### Storage Locations
- Databases: `~/db/FileTracker/*.db`
- Drive tracking: `~/db/FileTracker/drives.db`
- Log files: `~/logs/FileTracker/`
- Ignore list: `~/.rsync-ignore`

## Build System

### Dependencies
- **Required:** GCC, OpenSSL 3 (`libssl`, `libcrypto`), SQLite3, pthreads
- **Optional:** GTK4 (for GUI tools)
- **macOS:** Homebrew paths auto-detected in Makefile
- **Install:** `brew install openssl@3 sqlite gtk4` (macOS)

### Build Targets
```bash
make              # Build all tools
make apps         # Build GUIs and create .app bundles
make clean        # Remove binaries
make install      # Move binaries to ~/bin
```

### Single Tool Compilation Pattern
```bash
# CLI tools: OpenSSL + SQLite + pthreads
gcc -Wall -Wextra -O2 -o tool tool.c -lssl -lcrypto -lsqlite3 -lpthread

# GUI tools: Add GTK4
gcc -Wall -Wextra -O2 `pkg-config --cflags --libs gtk4` -o tool_gui tool_gui.c -lsqlite3
```

## Key Source Files

### Core Scanners
- `file_tracker.c` (31KB): Multi-threaded scanner with SHA-256, mtime comparison
- `file_tracker_gui.c` (40KB): GTK4 GUI with mounted volumes list, live progress
- `file_tracker_unified.c` (72KB): All-in-one tabbed GUI (Scanner, Summary, Logs, Drives, Locator)

### Query/Display Tools
- `file_locator.c` (5KB): Search by filename across all databases, checksum comparison
- `ft_summary.c` (20KB): Run history and statistics viewer
- `ft_logs.c` (17KB): Per-run detailed log viewer with filters
- `ft_drives.c` (26KB): Drive metadata tracking with auto-capacity detection

### GUI Implementations
- `*_gui.c` files: GTK4 interfaces for corresponding CLI tools
- Pattern: Same functionality as CLI, added visual progress/filtering

### Scripts
- `install_apps.sh`: Install .app bundles to /Applications
- `create_app_bundles.sh`: Create macOS app bundles from binaries
- `migrate_add_*.sh`: Database schema migration scripts (idempotent)

## Code Conventions

### Multi-threading
- `file_tracker` spawns one thread per path argument
- Uses pthreads with mutex locks for database writes
- Pattern: `pthread_create()` → worker function → `pthread_join()`

### Database Operations
- Always check `sqlite3_open()` return value
- Use prepared statements for queries: `sqlite3_prepare_v2()`
- Transaction pattern: `BEGIN TRANSACTION` → operations → `COMMIT`
- Close statements with `sqlite3_finalize()`

### Error Handling
- Errors logged to `run_logs` table with status='ERROR'
- GUI tools: Show error dialogs with GTK `gtk_alert_dialog_show()`
- CLI tools: Print to stderr, continue processing

### Checksum Strategy
- Default: Compare mtime only (fast verification)
- `-c` flag: Force SHA-256 recomputation (deep verification)
- Unchanged mtime + no `-c` = skip checksum (optimization)

### Status Values
- **UNCHANGED:** File matches database record
- **CHANGED:** Metadata or checksum differs
- **NEW:** File found on disk, not in database
- **MISSING:** In database, not found on disk
- **IGNORED:** Matched `.rsync-ignore` pattern
- **ERROR:** Processing failed (path too long, permission denied, etc.)

## Common Workflows

### Initial Setup
```bash
# First scan - baseline
file_tracker -p /Volumes/ExternalDrive -u -s -t "Initial baseline"

# Add drive to tracking
ft_drives add ExternalDrive -d "Backup drive" -c "Drawer A"
```

### Periodic Verification
```bash
# Quick check (mtime only)
file_tracker -p /Volumes/ExternalDrive -u -s -t "Weekly check"

# Deep verification (full checksum)
file_tracker -p /Volumes/ExternalDrive -c -u -s -t "Monthly deep scan"
```

### Investigation
```bash
# What changed?
ft_summary -d ExternalDrive -c -m

# View detailed logs
ft_logs -d ExternalDrive -l  # List runs
ft_logs -d ExternalDrive -r <run-id> -C  # Changed files only

# Find duplicates
ft_find_dupes -d ExternalDrive -v
```

### Multi-Path Scanning
```bash
# Scan multiple paths into one database
file_tracker -p /path1,/path2,/path3 -n shared_db -u -s
```

## GUI Application Bundles

Created by `create_app_bundles.sh`, installed via `install_apps.sh`:
- **File Tracker.app:** Scanner GUI
- **File Tracker Unified.app:** All-in-one tabbed interface (recommended)
- **File Tracker Summary.app:** History viewer
- **File Tracker Logs.app:** Log browser
- **File Tracker Drives.app:** Drive manager
- **File Locator.app:** File search

## Development Notes

### When Modifying Scanner Logic
- Update both `file_tracker.c` and `file_tracker_gui.c`
- Test with `-u` (update mode) and without (read-only)
- Test with `-c` (checksum mode) and without (mtime-only)
- Verify thread safety for database writes

### When Adding Database Fields
1. Update schema in source file `CREATE TABLE` statements
2. Create migration script in `migrate_*.sql`
3. Wrap in shell script with safety checks
4. Test idempotency (safe to re-run)
5. Update all tools that query the table

### GUI Development
- Use GTK4 builder pattern for complex UIs
- Progress updates: `g_idle_add()` for thread-safe UI updates
- Use `GtkColumnView` for tables, `GtkTextView` for logs
- File dialogs: `GtkFileDialog` (async API)

### Testing Checklist
- [ ] CLI tool with no arguments (should show usage)
- [ ] Read-only mode (no `-u`)
- [ ] Update mode (`-u`)
- [ ] With and without checksum (`-c`)
- [ ] Multi-threaded (multiple `-p` paths)
- [ ] Missing/changed/new file detection
- [ ] GUI responsiveness during long scans
- [ ] Database schema compatibility

## Performance Characteristics

- **Bottlenecks:** I/O (reading files), SHA-256 computation
- **Optimization:** Multi-threading (one thread per path)
- **Fast mode:** mtime-only comparison (default)
- **Slow mode:** Full checksum verification (`-c`)
- **Typical Speed:** ~500MB/s on SSD (checksum mode), ~10GB/s (mtime mode)

## Documentation Files

- `README.md`: Comprehensive user guide
- `README-GUI-APPS.md`: GUI application overview
- Feature docs: `*_FEATURE.md` (note storage, error logging, log storage)
- GUI guides: `*_GUI_GUIDE.md` (implementation details)
- Change logs: `CHANGELOG_*.md`

## Common Issues

### macOS-Specific
- Homebrew paths must be in CFLAGS/LDFLAGS (Makefile handles this)
- Use `pkg-config` for GTK4 flags
- App bundles need launcher script to set `DYLD_LIBRARY_PATH`

### Database Locking
- SQLite busy errors: Wait and retry with `sqlite3_busy_timeout()`
- Multiple writers: Use transactions
- Long-running queries: Keep statements finalized when not in use

### Path Handling
- Maximum path length: Check `PATH_MAX` or `4096`
- Special characters: Use prepared statements (auto-escaping)
- Symbolic links: Follow them (`stat()` vs `lstat()`)

## Token Optimization Tips

### Instead of reading README.md (25KB), reference this file for:
- Tool purposes and relationships
- Database schema
- Build commands
- Common workflows

### For code changes:
- Only read the specific `*.c` file being modified
- Reference schema here instead of grepping database code
- Use Makefile pattern shown here instead of reading full Makefile

### For debugging:
- Status values and their meanings are documented above
- Error patterns (logged to run_logs with status='ERROR')
- Multi-threading uses standard pthread pattern

## Quick Command Reference

```bash
# Scan
file_tracker -p <path> -u -c -s -t "note"

# Query
ft_summary -d <db>              # Stats
ft_logs -d <db> -l              # List runs  
ft_logs -d <db> -r <run-id> -C  # Changed files
file_locator -f <filename>      # Find file

# Manage
ft_drives add <name> -d "desc"  # Add drive
ft_drives verify <name>         # Mark verified
ft_find_dupes -d <db>           # Find duplicates

# Build
make                            # All tools
make apps                       # + app bundles
```

## Current Development Status

**Latest Changes (from git log):**
- Added Logs Viewer tab to file_tracker_unified
- Record ignored files in database with IGNORED status
- Replaced Quick Scanner with full File Scanner functionality
- Created file_tracker_unified - all-in-one GUI

**Active Development:**
- Focus on unified GUI application
- Integration of all tools into tabbed interface
- Improved error logging and status tracking

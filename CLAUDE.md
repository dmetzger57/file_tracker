# File Tracker - Project Context

## Overview
File tracking suite using SHA-256 checksums to detect bit-rot and silent corruption on archival storage. Written in C, uses SQLite for storage, GTK4 for GUIs. All tools (CLI and GUI) share the same database schema.

**Primary Use Case:** Periodic verification of external drives and archival storage to detect unauthorized changes or corruption.

## Architecture

### Application
**Unified GUI:** `file_tracker_unified` - All-in-one GTK4 application with tabbed interface:
1. **Scanner** - Scan directories and compute checksums
2. **Summary** - View scan history and statistics
3. **Logs** - Browse detailed per-file change logs
4. **Drives** - Track external drive information
5. **Locator** - Search for files across databases
6. **Compare** - Compare two scan runs

### Data Flow
```
file_tracker_unified → SQLite DB (~/db/FileTracker/*.db)
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
- Ignore list: `~/.rsync-ignore`

## Build System

### Dependencies
- **Required:** GCC, OpenSSL 3 (`libssl`, `libcrypto`), SQLite3, GTK4 (GLib threads)
- **macOS:** Homebrew paths auto-detected in Makefile
- **Install:** `brew install openssl@3 sqlite gtk4` (macOS)

### Build Targets
```bash
make              # Build file_tracker_unified
make apps         # Build and create .app bundle
make clean        # Remove binaries
make install      # Copy File Tracker Unified.app to /Applications
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

## Key Source Files

### Application
- `file_tracker_unified.c` (~120KB): All-in-one GTK4 application with tabbed interface
  - Scanner tab: SHA-256 / mtime scanner with an adjustable worker pool (Workers setting)
  - Summary tab: Run history and statistics viewer
  - Logs tab: Per-run detailed log viewer with filters
  - Drives tab: Drive metadata tracking with auto-capacity detection
  - Locator tab: Search by filename across databases, checksum comparison
  - Compare tab: Compare two scan runs to see changes

### Scripts
- `create_app_bundles.sh`: Create macOS app bundle from binary
- `migrate_add_*.sh`: Database schema migration scripts (idempotent)

## Code Conventions

### Multi-threading
- Scanner tab scans a single path. "Start Scan" starts one `GThread` (`scanner_thread_func`) that walks the directory tree
- Regular files are queued to a `GThreadPool` of N workers (`scanner_worker_func` → `scanner_process_file`); N comes from the "Workers" spin button (default 1, max 2× CPU cores)
- `ScannerContext.lock` (`GMutex`) guards the shared SQLite connection, counters and `log_buffer`; SHA-256 hashing runs outside the lock so workers hash in parallel
- The walker handles ignored files itself (under the lock) and pauses when more than `SCANNER_MAX_QUEUED` files are waiting
- `g_thread_pool_free(pool, FALSE, TRUE)` waits for the workers before `scanner_find_missing()` and the run summary/log writes
- Guidance: keep Workers at 1 for spinning HDDs (parallel reads cause head seeking); raise it for checksum scans on SSD/NVMe

### Database Operations
- Always check `sqlite3_open()` return value
- Use prepared statements for queries: `sqlite3_prepare_v2()`
- Transaction pattern: `BEGIN TRANSACTION` → operations → `COMMIT`
- Close statements with `sqlite3_finalize()`

### Error Handling
- Errors logged to `run_logs` table with status='ERROR'
- GUI: Show error dialogs with GTK `gtk_alert_dialog_show()`
- Continue processing after errors (don't abort entire scan)

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

### Initial Setup (Scanner Tab)
1. Launch File Tracker Unified
2. Go to Scanner tab
3. Select drive from Mounted Volumes list
4. Enable "Update Database"
5. Add note: "Initial baseline"
6. Click "Start Scan"

### Periodic Verification (Scanner Tab)
**Quick check (mtime only):**
1. Select drive
2. Enable "Update Database"
3. **Disable** "Enable Checksum Verification"
4. Add note: "Weekly check"
5. Start scan

**Deep verification (full checksum):**
1. Select drive
2. Enable "Update Database"
3. **Enable** "Enable Checksum Verification"
4. Add note: "Monthly deep scan"
5. Start scan

### Investigation (Summary/Logs/Compare Tabs)
**What changed?**
1. Go to Summary tab
2. Select database
3. View Changed Files tab

**View detailed logs:**
1. Go to Logs tab
2. Select database and run
3. Filter by status (CHANGED, NEW, MISSING, etc.)

**Compare two runs:**
1. Go to Compare tab
2. Select database
3. Choose two runs to compare
4. View differences

**Find duplicates:**
1. Go to Locator tab
2. Search for filename
3. View results across all databases

## GUI Application Bundle

Created by `create_app_bundles.sh` (`make apps`), installed to /Applications via `make install`:
- **File Tracker Unified.app:** All-in-one tabbed interface with Scanner, Summary, Logs, Drives, Locator, and Compare tabs

## Development Notes

### When Modifying Scanner Logic
- Update `file_tracker_unified.c` Scanner tab code
- Test with "Update Database" enabled and disabled (read-only)
- Test with "Enable Checksum Verification" on and off (mtime-only)
- Verify thread safety: any access to `ctx->db`, counters or `scanner_log_message()` from worker code must hold `ctx->lock`

### When Adding Database Fields
1. Update schema in source file `CREATE TABLE` statements
2. Create migration script in `migrate_*.sql`
3. Wrap in shell script with safety checks
4. Test idempotency (safe to re-run)
5. Update all tools that query the table

### GUI Development
- Tabbed interface using `GtkNotebook`
- Progress updates: `g_idle_add()` for thread-safe UI updates
- Use `GtkColumnView` for tables, `GtkTextView` for logs
- File dialogs: `GtkFileDialog` (async API)
- Each tab has independent functionality and state

### Testing Checklist
- [ ] Scanner tab: Read-only mode (Update Database disabled)
- [ ] Scanner tab: Update mode (Update Database enabled)
- [ ] Scanner tab: With and without checksum verification
- [ ] Scanner tab: Workers = 1 and Workers > 1 give identical counts
- [ ] Scanner tab: Missing/changed/new file detection
- [ ] Summary tab: Run history and tabbed file lists
- [ ] Logs tab: Filtering by status
- [ ] Drives tab: Add, verify, edit operations
- [ ] Locator tab: Search across databases
- [ ] Compare tab: Run comparison and filtering
- [ ] GUI responsiveness during long scans
- [ ] Database schema compatibility across all tabs

## Performance Characteristics

- **Bottlenecks:** I/O (reading files), SHA-256 computation
- **Optimization:** Worker pool hashes files in parallel (Workers setting; helps SSD/NVMe, not HDDs)
- **Fast mode:** mtime-only comparison (default)
- **Slow mode:** Full checksum verification (`-c`)
- **Typical Speed:** ~500MB/s on SSD (checksum mode), ~10GB/s (mtime mode)

## Documentation Files

- `README.md`: The single user guide for the whole repository (application, CLI, ignore patterns, schema, build). Keep it current when behavior changes

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

### Instead of reading README.md (~14KB), reference this file for:
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
- Multi-threading uses a GLib `GThreadPool` + `GMutex` (see Code Conventions)

## Quick Reference

### Launch Application
```bash
./file_tracker_unified
# Or double-click File Tracker Unified.app
```

### Scanner Tab
1. Select drive/path
2. Enable "Update Database" to save changes
3. Enable "Enable Checksum Verification" for deep scan
4. Add note (optional)
5. Click "Start Scan"

### Query Operations
- **Summary tab:** View run history and file lists
- **Logs tab:** Browse detailed change logs with filters
- **Locator tab:** Search for files across databases
- **Compare tab:** Compare two runs to see differences

### Drive Management
- **Drives tab:** Add, verify, edit drive metadata

### Build
```bash
make                            # Build application
make apps                       # Build + create .app bundle
```

## Current Development Status

**Latest Changes:**
- Consolidated all functionality into file_tracker_unified
- Removed legacy individual CLI and GUI tools
- Updated documentation to focus on unified application
- All features accessible via tabbed interface

**Active Development:**
- Enhancements to unified GUI application
- Additional tab features and improvements
- Database schema optimizations

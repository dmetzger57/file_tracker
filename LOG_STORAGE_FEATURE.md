# Log Storage Feature

## Overview
File_tracker now stores all log messages in the database, enabling detailed analysis of past runs without parsing text log files. The new `ft_logs` tool provides flexible querying of these stored logs.

## Database Changes

### New Table: run_logs
```sql
CREATE TABLE run_logs (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    run_id INTEGER NOT NULL,
    status TEXT NOT NULL,
    full_path TEXT NOT NULL,
    FOREIGN KEY(run_id) REFERENCES meta(id)
);

CREATE INDEX idx_run_logs_run_id ON run_logs(run_id);
CREATE INDEX idx_run_logs_status ON run_logs(status);
```

This table stores one entry per file operation during each run, linked to the `meta` table via `run_id`.

### Status Values
- `NEW` - File not previously in database
- `UNCHANGED` - File unchanged (mtime match or checksum match)
- `CHANGED (Metadata)` - File modification time changed
- `CHANGED (Checksum)` - File content changed (checksum differs)
- `MISSING` - File in database but not on filesystem

## Implementation Details

### Log Message Buffering
Log messages are buffered in memory during file processing:

```c
typedef struct {
    char status[32];
    char path[MAX_PATH];
} LogEntry;
```

The `ThreadContext` structure maintains:
- `log_buffer` - Dynamic array of LogEntry structs
- `log_count` - Current number of buffered messages
- `log_capacity` - Allocated capacity of buffer

### Insertion Sequence
1. **Traversal Phase**: Files are processed and log messages buffered
2. **Meta Insert**: Run metadata inserted into `meta` table
3. **Get run_id**: Last inserted row ID retrieved
4. **Missing Files**: Missing files logged (adds to buffer)
5. **Bulk Insert**: All buffered logs inserted into `run_logs` with run_id
6. **Cleanup**: Buffer freed

This ensures:
- All log messages have a valid run_id
- Missing file count is accurate in meta record
- Single transaction for consistency

## ft_logs Tool

### Usage
```bash
ft_logs -n database_name -d YYYY-MM-DD -t HH-MM-SS [-N] [-C] [-M]
```

### Required Arguments
- `-n database_name` - Name of database (without .db extension)
- `-d YYYY-MM-DD` - Date of run (e.g., 2024-03-15)
- `-t HH-MM-SS` - Time of run (e.g., 14-30-45)

**Note:** Date and time match the log file naming convention:
`basename-YYYY-MM-DD-HH-MM-SS.log`

### Filter Options
- `-N` - Show only NEW files
- `-C` - Show only CHANGED files (both metadata and checksum changes)
- `-M` - Show only MISSING files
- *No filter* - Show all log messages

Multiple filters can be combined: `-N -C` shows both NEW and CHANGED files.

### Output Format
```
================ RUN INFORMATION ==================
Run ID         : 15
Date/Time      : 2024-03-15 14:30:45
Machine        : hostname
Unchanged      : 1523
Changed        : 12
New            : 3
Missing        : 1
==================================================

=================== LOG MESSAGES ==================
[NEW               ] /path/to/new/file1.txt
[NEW               ] /path/to/new/file2.txt
[NEW               ] /path/to/new/file3.txt
[CHANGED (Metadata)] /path/to/changed/file.doc
...
==================================================
Total messages : 2547
```

### Error Handling
If the specified date/time doesn't match any run, ft_logs displays:
- Error message with the requested date/time
- List of the 10 most recent runs in the database
- Suggested format: `ID: 15 - 2024-03-15 14:30:45 - hostname`

## Migration

### For Existing Databases
Run the migration script to add the `run_logs` table:

```bash
# Migrate all databases
./migrate_add_run_logs.sh

# Migrate specific database
./migrate_add_run_logs.sh ~/db/FileTracker/archive.db
```

The script:
- Creates the `run_logs` table if it doesn't exist
- Creates indexes for performance
- Is idempotent (safe to run multiple times)
- Provides success/failure feedback

### Historical Data
Databases migrated from older versions will have the `run_logs` table but no historical log entries. Only runs performed after migration will have detailed logs in the database.

## Performance Considerations

### Memory Usage
- Log buffer grows dynamically (starts at 1024 entries, doubles as needed)
- Typical run with 10,000 files: ~4MB buffer (400 bytes per entry)
- Buffer freed immediately after insertion

### Database Size
- Each log entry: ~100-300 bytes (depends on path length)
- 10,000 files per run: ~1-3 MB of log data
- Indexes add ~10-20% overhead
- 100 runs of 10,000 files each: ~150 MB total

### Query Performance
- Indexes on `run_id` and `status` enable fast filtering
- Typical query (single run, filtered): <10ms
- Full run display (10,000 entries): <50ms

## Use Cases

### Audit and Compliance
```bash
# Find all files deleted in the last month
for run in $(sqlite3 db.db "SELECT id, date FROM meta WHERE date > date('now','-1 month')"); do
    ft_logs -n mydb -d ... -t ... -M
done
```

### Change Tracking
```bash
# Review what changed in a specific run
ft_logs -n archive -d 2024-03-15 -t 14-30-45 -C -M
```

### Verification
```bash
# Verify all new files from initial scan
ft_logs -n backup -d 2024-01-01 -t 00-00-00 -N | wc -l
```

### Debugging
```bash
# When was this file last seen as changed?
sqlite3 ~/db/FileTracker/archive.db \
  "SELECT m.last_date_verify FROM run_logs l 
   JOIN meta m ON l.run_id = m.id 
   WHERE l.full_path = '/path/to/file.txt' 
   AND l.status LIKE 'CHANGED%' 
   ORDER BY m.id DESC LIMIT 1;"
```

## Future Enhancements

Possible additions:
- Date range queries (`ft_logs --from YYYY-MM-DD --to YYYY-MM-DD`)
- Path pattern filtering (`ft_logs --path-like "%.pdf"`)
- Export to CSV/JSON
- Statistics per file (how many times changed, when)
- Anomaly detection (unusual change patterns)

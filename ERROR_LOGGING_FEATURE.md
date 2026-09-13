# Error Logging Feature

## Overview
File_tracker now logs all errors to the database, enabling detailed analysis of what went wrong during each run. The `ft_summary` tool provides the ability to view these errors.

## What Gets Logged

All runtime errors are now stored in the `run_logs` table with status "ERROR". This includes:

- **Path too long errors** - When a file path exceeds MAX_PATH (4096 characters)
- **SQLite errors** - Database preparation failures and other DB errors
- **File access errors** - Permission denied, file not found during processing
- Any other errors that occur during file traversal and processing

## Database Storage

Errors are stored in the existing `run_logs` table with:
- `status` = "ERROR"
- `full_path` = Error message text
- `run_id` = Link to the run in the meta table

This means errors are queryable just like file status changes (NEW, CHANGED, MISSING).

## Error Counts

The `meta` table's `num_errors` field contains the total count of errors for each run. This count is displayed in:
- `ft_summary` output (default view)
- Log file summaries
- Run statistics

## Viewing Errors with ft_summary

### Show Error List
```bash
# View errors from the last run
ft_summary -d database_name -e

# Example output:
================================================================================
DATABASE: MyFiles
================================================================================

------------------------------------------------------------------------------------------------------------------
Run #  | Run Date            | Update | Checksum | Unchanged | Changed    | New        | Missing    | Errors
------------------------------------------------------------------------------------------------------------------
12     | 2026-09-12 10:30:45 | On     | On       |     1,523 |         12 |          3 |          1 |       2
------------------------------------------------------------------------------------------------------------------
Total runs: 1

Errors (Run #12):
  Path too long, skipping: /very/long/path/that/exceeds/max/length...
  SQLite prepare error: database is locked
```

### Combine with Other Filters
```bash
# Show both errors and missing files
ft_summary -d MyFiles -e -m

# Show errors and changed files
ft_summary -d MyFiles -e -c
```

## How It Works

### In file_tracker.c

1. **log_error() Function**
   ```c
   void log_error(ThreadContext *ctx, const char *error_msg) {
       fprintf(stderr, "%s\n", error_msg);
       if (ctx->log_fp) {
           fprintf(ctx->log_fp, "[ERROR            ] %s\n", error_msg);
       }
       log_message(ctx, "ERROR", error_msg);
       ctx->error++;
   }
   ```

2. **Error Handling**
   - Whenever an error occurs, `log_error()` is called
   - Error is written to stderr (console)
   - Error is written to text log file
   - Error is buffered for database insertion
   - Error counter is incremented

3. **Database Insertion**
   - Errors are buffered during traversal along with other status messages
   - All buffered messages (including errors) are bulk-inserted into `run_logs` table
   - This ensures errors have the correct `run_id` and are part of the transaction

### In ft_summary.c

1. **New `-e` Option**
   - Added to command line argument parsing
   - Follows same pattern as `-m`, `-c`, `-n` options
   - Queries log files for lines starting with `[ERROR`

2. **Display Format**
   - Errors are shown after the run summary
   - Each error message is displayed on its own line
   - Count is shown if no errors found

## Use Cases

### Debugging Failed Runs
```bash
# Check why a run had errors
ft_summary -d archive -e
```

### Monitoring for Issues
```bash
# Check all databases for errors
for db in ~/db/FileTracker/*.db; do
    dbname=$(basename "$db" .db)
    errors=$(sqlite3 "$db" "SELECT num_errors FROM meta ORDER BY id DESC LIMIT 1")
    if [ "$errors" -gt 0 ]; then
        echo "$dbname had $errors errors"
        ./ft_summary -d "$dbname" -e
    fi
done
```

### Audit Trail
```bash
# Find all runs with errors
sqlite3 ~/db/FileTracker/myfiles.db \
  "SELECT id, last_date_verify, num_errors 
   FROM meta 
   WHERE num_errors > 0 
   ORDER BY id DESC"
```

### Detailed Error Analysis
```bash
# Query specific error types
sqlite3 ~/db/FileTracker/myfiles.db \
  "SELECT m.id, m.last_date_verify, l.full_path 
   FROM run_logs l 
   JOIN meta m ON l.run_id = m.id 
   WHERE l.status = 'ERROR' 
   AND l.full_path LIKE '%Path too long%'
   ORDER BY m.id DESC"
```

## Error Types

### Path Too Long
**Trigger:** File path exceeds 4096 characters  
**Message:** `Path too long, skipping: /path/to/file`  
**Impact:** File is skipped, not tracked in database  
**Resolution:** Restructure directory hierarchy or increase MAX_PATH

### SQLite Errors
**Trigger:** Database operation failures  
**Message:** `SQLite prepare error: [sqlite error message]`  
**Impact:** Depends on operation; may skip files or abort processing  
**Resolution:** Check database integrity, disk space, permissions

## Backward Compatibility

- Existing databases work without modification
- `run_logs` table already exists (added in log storage feature)
- Old runs have no ERROR entries, only NEW/CHANGED/MISSING/UNCHANGED
- New runs include ERROR entries when errors occur
- Error count in `meta.num_errors` continues to work as before

## Performance Impact

- **Minimal** - Errors are logged using the same buffering mechanism as status messages
- No additional database transactions
- No performance degradation during normal (error-free) operation
- Only when errors occur is there a small overhead to format and buffer the message

## Future Enhancements

Possible additions:
- Error severity levels (WARNING, ERROR, FATAL)
- Error categories (PATH, DATABASE, PERMISSION, IO)
- Structured error data (error code, context, stack trace)
- Error rate monitoring and alerting
- Automatic error recovery suggestions
- Export errors to JSON for external monitoring systems

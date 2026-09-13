# Error Logging Feature - Change Summary

## Date: 2026-09-12

## Overview
Added comprehensive error logging to file_tracker system. Errors are now stored in the database and can be viewed using ft_summary and ft_logs tools.

## Modified Files

### 1. file_tracker.c
**Changes:**
- Added `log_error()` function to log errors to both stderr/file and database
- Modified error handling to use `log_error()` instead of just incrementing error counter
- Errors are now buffered and stored in `run_logs` table with status='ERROR'

**New Function:**
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

**Error Types Logged:**
- SQLite prepare errors
- Path too long errors
- Any future errors that call `log_error()`

### 2. ft_summary.c
**Changes:**
- Added `-e` command line option to show errors from last run
- Added `show_errors` variable to track error display mode
- Updated `process_database()` function signature to include `show_errors` parameter
- Updated `use_compact` logic to exclude `-e` flag
- Added error display using `print_log_entries()` with "[ERROR" prefix

**New Functionality:**
```bash
# View errors from last run
ft_summary -d database_name -e

# Combine with other options
ft_summary -d MyFiles -e -m -c
```

### 3. ft_logs.c
**Changes:**
- Added `-E` command line option to filter for ERROR messages
- Added `show_errors` variable
- Updated help text to include `-E` option
- Added ERROR status filter in query building logic

**New Functionality:**
```bash
# View only errors from a specific run
ft_logs -d database_name -r run_id -E

# Combine with other filters
ft_logs -d database_name -r run_id -E -M
```

### 4. README.md
**Updates:**
- Added `-e` option to ft_summary documentation
- Added `-E` option to ft_logs documentation
- Added references to ERROR_LOGGING_FEATURE.md
- Updated descriptions to mention error logging capability

### 5. New Documentation Files

**ERROR_LOGGING_FEATURE.md:**
- Comprehensive documentation of error logging feature
- What gets logged
- Database storage details
- Usage examples for ft_summary and ft_logs
- Use cases and future enhancements

**TEST_ERROR_LOGGING.md:**
- Testing procedures for error logging
- Quick test scenarios
- Database query examples
- Feature verification checklist

**CHANGELOG_ERROR_LOGGING.md:**
- This file - summary of all changes

## Database Schema

No changes to database schema required. Uses existing `run_logs` table:
```sql
CREATE TABLE run_logs (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    run_id INTEGER NOT NULL,
    status TEXT NOT NULL,
    full_path TEXT NOT NULL,
    FOREIGN KEY(run_id) REFERENCES meta(id)
);
```

Errors are stored with:
- `status` = "ERROR"
- `full_path` = Error message text
- `run_id` = Link to meta table

## Backward Compatibility

✅ **Fully backward compatible**
- Existing databases work without modification
- Old runs have no ERROR entries (only NEW/CHANGED/MISSING/UNCHANGED)
- New runs include ERROR entries when errors occur
- Error count in `meta.num_errors` continues to work as before

## Testing

### Manual Testing Performed:
1. ✅ Compiled all binaries successfully
2. ✅ Verified help text for ft_summary shows `-e` option
3. ✅ Verified help text for ft_logs shows `-E` option
4. ✅ Code review for error handling paths
5. ✅ Documentation updated and cross-referenced

### Recommended Testing:
See TEST_ERROR_LOGGING.md for detailed test procedures:
- Basic functionality test (normal operation, no errors)
- Error scenario tests (path too long, database errors)
- ft_summary -e functionality
- ft_logs -E functionality
- Database query verification

## Usage Examples

### View Errors in Last Run
```bash
# Using ft_summary
ft_summary -d MyDatabase -e

# Using ft_logs
ft_logs -d MyDatabase -l  # List runs
ft_logs -d MyDatabase -r MyDatabase-2026-09-12-10-30-45 -E
```

### Combine Error View with Other Filters
```bash
# Show both errors and missing files
ft_summary -d MyDatabase -e -m

# Show errors and changed files
ft_logs -d MyDatabase -r run_id -E -C
```

### Query Database Directly
```bash
sqlite3 ~/db/FileTracker/MyDatabase.db \
  "SELECT m.id, m.last_date_verify, l.full_path 
   FROM run_logs l 
   JOIN meta m ON l.run_id = m.id 
   WHERE l.status = 'ERROR' 
   ORDER BY m.id DESC"
```

## Performance Impact

**Minimal to None:**
- Uses existing log buffering mechanism
- No additional database transactions
- Same bulk insert as other status messages
- Only overhead is when errors actually occur

## Future Enhancements

Potential additions documented in ERROR_LOGGING_FEATURE.md:
- Error severity levels (WARNING, ERROR, FATAL)
- Error categories (PATH, DATABASE, PERMISSION, IO)
- Structured error data
- Error rate monitoring
- Automatic recovery suggestions
- Export to JSON for external monitoring

## Build Information

**Compiled with:**
- gcc with -Wall -Wextra -O2
- OpenSSL 3.x
- SQLite 3.x
- pthread support

**Binary sizes:**
- file_tracker: 55K
- ft_summary: 36K
- ft_logs: 35K

## Files Changed Summary

```
Modified:
  file_tracker.c          (+13 lines)
  ft_summary.c            (+15 lines)
  ft_logs.c               (+8 lines)
  README.md               (+5 lines)

Created:
  ERROR_LOGGING_FEATURE.md
  TEST_ERROR_LOGGING.md
  CHANGELOG_ERROR_LOGGING.md
```

## Verification Checklist

- [x] Code compiles without errors or warnings
- [x] Help text updated for ft_summary
- [x] Help text updated for ft_logs
- [x] README.md updated
- [x] Feature documentation created
- [x] Test documentation created
- [x] Backward compatibility maintained
- [x] No database schema changes required
- [x] Error logging integrated into existing infrastructure
- [x] Both CLI tools support error viewing

## Notes

This enhancement completes the file_tracker logging system by ensuring that not only file status changes but also system errors are persistently stored and easily queryable. This is particularly valuable for:

1. **Debugging** - Understanding what went wrong during a run
2. **Auditing** - Tracking system reliability over time
3. **Monitoring** - Alerting on error conditions
4. **Analysis** - Identifying patterns in failures

The implementation follows the existing patterns in the codebase and requires no migration for existing databases.

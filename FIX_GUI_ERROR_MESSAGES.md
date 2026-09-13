# Fix: file_tracker_gui Error Message Storage

## Issue Found

**file_tracker_gui was NOT storing descriptive error messages** in the database. It was:

1. **Storing only filepath** for some errors (not descriptive):
   ```c
   // OLD CODE - just stores the path
   ctx->errors++;
   log_message(ctx, "ERROR", filepath);  // Full path, not error description
   ```

2. **Not storing anything** for other errors:
   ```c
   // OLD CODE - only increments counter
   ctx->errors++;  // No database entry at all!
   ```

This was inconsistent with CLI `file_tracker.c` which stores descriptive messages like:
- "Path too long, skipping: /path/..."
- "SQLite prepare error: database is locked"

## Fix Applied

### 1. Added `log_error()` Function

```c
// Log error message with descriptive text
void log_error(ScanContext *ctx, const char *error_msg) {
    log_message(ctx, "ERROR", error_msg);
    ctx->errors++;
}
```

### 2. Updated Error Handlers to Use Descriptive Messages

**stat() failure:**
```c
// NEW CODE - descriptive message
char err_msg[MAX_PATH + 64];
snprintf(err_msg, sizeof(err_msg), "stat() failed for: %s", filepath);
log_error(ctx, err_msg);
```

**SQLite prepare failure:**
```c
// NEW CODE - includes SQLite error message
char err_msg[512];
snprintf(err_msg, sizeof(err_msg), "SQLite prepare error: %s", sqlite3_errmsg(ctx->db));
log_error(ctx, err_msg);
```

**opendir() failure:**
```c
// NEW CODE - now logs the error (previously only incremented counter)
char err_msg[MAX_PATH + 64];
snprintf(err_msg, sizeof(err_msg), "Failed to open directory: %s", dirpath);
log_error(ctx, err_msg);
```

## Result

Now **file_tracker_gui stores ALL error messages** with descriptive text:

### Before (What Was Stored):
```
[ERROR] /path/to/file.txt              (just the path - not helpful)
(nothing stored for opendir failures)
```

### After (What Is Stored Now):
```
[ERROR] stat() failed for: /path/to/file.txt
[ERROR] SQLite prepare error: database is locked
[ERROR] Failed to open directory: /path/to/directory
```

## Error Messages Now Visible In:

1. **ft_summary_gui** - "Errors" tab shows descriptive messages
2. **ft_logs_gui** - "Errors" filter shows descriptive messages
3. **ft_summary -e** - CLI displays descriptive error text
4. **ft_logs -E** - CLI displays descriptive error text
5. **Database queries** - Full error text available

## Testing

### Before the Fix:
```bash
# Errors would show as just file paths
./ft_logs -d mydb -r run_id -E
# Output: [ERROR] /some/file.txt  (not helpful!)
```

### After the Fix:
```bash
# Errors now show descriptive messages
./ft_logs -d mydb -r run_id -E
# Output: [ERROR] stat() failed for: /some/file.txt
# Output: [ERROR] SQLite prepare error: database is locked
```

## Verification

```bash
# Rebuild
make file_tracker_gui

# Run a scan that will trigger errors
./file_tracker_gui
# ... perform scan ...

# View errors in CLI
./ft_summary -d database_name -e
./ft_logs -d database_name -r run_id -E

# View errors in GUI
./ft_summary_gui  # Click Errors tab
./ft_logs_gui     # Check Errors filter
```

## Consistency Achieved

All file tracker tools now store errors consistently:

| Tool | Error Storage | Error Messages |
|------|--------------|----------------|
| file_tracker (CLI) | ✅ Yes | ✅ Descriptive |
| file_tracker_gui | ✅ Yes (FIXED) | ✅ Descriptive (FIXED) |

## Files Modified

- `file_tracker_gui.c` - Added log_error() function and updated 3 error handlers

## Build Status

✅ Compiles successfully  
✅ Binary size: 43K (unchanged)  
✅ No new warnings or errors

## Impact

- **Backward Compatible:** Old database entries unchanged
- **Forward Compatible:** New scans store descriptive errors
- **No Breaking Changes:** All existing functionality intact
- **Better Debugging:** Users can now see exactly what went wrong

## Date: 2026-09-12

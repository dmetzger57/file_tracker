# Quick Start: Error Logging Feature

## What's New?

File tracker now logs **all errors** to the database, making it easy to review what went wrong during any scan run.

## Quick Examples

### 1. View Error Count

```bash
# Show summary of last run (includes error count)
./ft_summary -d MyDatabase

# Output shows:
# Errors:        2
```

### 2. See What Errors Occurred

```bash
# List all errors from last run
./ft_summary -d MyDatabase -e

# Output:
# Errors (Run #12):
#   Path too long, skipping: /very/long/path/...
#   SQLite prepare error: database is locked
```

### 3. Filter Logs for Errors Only

```bash
# First, list available runs
./ft_logs -d MyDatabase -l

# Then view only errors from a specific run
./ft_logs -d MyDatabase -r MyDatabase-2026-09-12-10-30-45 -E

# Output:
# [ERROR            ] Path too long, skipping: /path/...
# [ERROR            ] SQLite prepare error: ...
```

### 4. Using the GUI

**ft_summary_gui:**
```bash
./ft_summary_gui
# 1. Select database
# 2. Click on a run
# 3. Click "Errors" tab
# 4. See list of errors
```

**ft_logs_gui:**
```bash
./ft_logs_gui
# 1. Select database
# 2. Click on a run
# 3. Check "Errors" checkbox
# 4. See filtered error messages
```

## Common Use Cases

### Debugging a Failed Run

```bash
# See what went wrong
./ft_summary -d archive -e

# Get more details from logs
./ft_logs -d archive -r archive-2026-09-12-10-30-45 -E
```

### Monitoring for Problems

```bash
# Check all databases for errors
./ft_summary | grep -A 1 "Errors"

# Or more sophisticated check
for db in ~/db/FileTracker/*.db; do
    dbname=$(basename "$db" .db)
    errors=$(sqlite3 "$db" "SELECT num_errors FROM meta ORDER BY id DESC LIMIT 1" 2>/dev/null)
    if [ "$errors" -gt 0 ]; then
        echo "⚠️  $dbname has $errors errors"
    fi
done
```

### Exporting Error Report

Using GUI:
```bash
./ft_summary_gui
# Select database → Select run → Click "Errors" tab → Click "Export"
# Creates: dbname-runX-errors.txt
```

Using CLI:
```bash
./ft_logs -d MyDatabase -r run_id -E > errors.txt
```

### Querying Error History

```bash
# Find all runs with errors
sqlite3 ~/db/FileTracker/MyDatabase.db \
  "SELECT id, last_date_verify, num_errors 
   FROM meta 
   WHERE num_errors > 0 
   ORDER BY id DESC"

# See actual error messages
sqlite3 ~/db/FileTracker/MyDatabase.db \
  "SELECT m.last_date_verify, l.full_path 
   FROM run_logs l 
   JOIN meta m ON l.run_id = m.id 
   WHERE l.status = 'ERROR' 
   ORDER BY m.id DESC"
```

## Error Types You Might See

### Path Too Long
```
Path too long, skipping: /very/long/directory/path/...
```
**Cause:** File path exceeds 4096 characters  
**Fix:** Restructure directory hierarchy

### SQLite Errors
```
SQLite prepare error: database is locked
```
**Cause:** Database access conflict  
**Fix:** Ensure only one file_tracker instance per database

### Other Errors
As more error types are added, they'll appear here automatically.

## Tips

1. **Always check error count** after a scan - even if it completed
2. **Use `-e` flag** in ft_summary for quick error review
3. **Combine filters** in ft_logs to see errors alongside other issues:
   ```bash
   ft_logs -d db -r run_id -E -M  # errors + missing files
   ```
4. **Export errors** from GUI for sharing or reporting issues
5. **Set up monitoring** to alert when errors occur

## Need More Details?

- Full feature documentation: [ERROR_LOGGING_FEATURE.md](ERROR_LOGGING_FEATURE.md)
- GUI-specific guide: [GUI_ERROR_SUPPORT.md](GUI_ERROR_SUPPORT.md)
- Testing procedures: [TEST_ERROR_LOGGING.md](TEST_ERROR_LOGGING.md)
- Complete changelog: [CHANGELOG_ERROR_LOGGING.md](CHANGELOG_ERROR_LOGGING.md)

## Still Have Questions?

Check the main [README.md](README.md) for complete documentation of all tools and options.

# Testing Error Logging Feature

## Quick Test

### 1. Create a test directory and run file_tracker

```bash
# Create a simple test directory
mkdir -p /tmp/ft_test
echo "test content" > /tmp/ft_test/file1.txt
echo "test content 2" > /tmp/ft_test/file2.txt

# Run file_tracker to create initial database
./file_tracker -p /tmp/ft_test -d test_errors -u -s
```

### 2. View the run summary

```bash
# View summary (will show 0 errors for successful run)
./ft_summary -d test_errors
```

Expected output:
```
------------------------------------------------------------------------------------------------------------------
Run #  | Run Date            | Update | Checksum | Unchanged | Changed    | New        | Missing    | Errors
------------------------------------------------------------------------------------------------------------------
1      | 2026-09-12 10:30:45 | On     | Off      |         0 |          0 |          2 |          0 |       0
------------------------------------------------------------------------------------------------------------------
```

### 3. Try to view errors (should be none)

```bash
./ft_summary -d test_errors -e
```

Expected output:
```
Errors (Run #1):
  (none)
```

### 4. Use ft_logs to view all messages

```bash
# List all runs
./ft_logs -d test_errors -l

# View all logs from the run
./ft_logs -d test_errors -r test_errors-2026-09-12-10-30-45

# View only errors (should be empty)
./ft_logs -d test_errors -r test_errors-2026-09-12-10-30-45 -E
```

## Testing Error Scenarios

To test actual error logging, you would need to trigger errors such as:

### 1. Path Too Long Error
Create a directory structure that exceeds MAX_PATH (4096 characters). This is difficult to create on most filesystems, but the code is in place to handle it.

### 2. Database Lock Error
Run two instances of file_tracker simultaneously on the same database (though WAL mode should prevent most locking issues).

### 3. Permission Error
Try to scan a directory with files you don't have permission to read (this would be handled at a different level, but future enhancements could log these as errors).

## Verifying Error Logging in Database

You can directly query the database to see error logs:

```bash
# Connect to the database
sqlite3 ~/db/FileTracker/test_errors.db

# Query to see all error entries
SELECT m.id as run_id, m.last_date_verify, l.status, l.full_path 
FROM run_logs l 
JOIN meta m ON l.run_id = m.id 
WHERE l.status = 'ERROR'
ORDER BY m.id DESC;

# Count errors per run
SELECT m.id, m.last_date_verify, m.num_errors, 
       COUNT(CASE WHEN l.status = 'ERROR' THEN 1 END) as logged_errors
FROM meta m
LEFT JOIN run_logs l ON m.id = l.run_id
GROUP BY m.id
ORDER BY m.id DESC;
```

## Feature Verification Checklist

- [x] file_tracker.c has `log_error()` function
- [x] Errors are logged to both stderr/file and database
- [x] Error count is stored in meta.num_errors
- [x] ft_summary has `-e` option to show errors
- [x] ft_logs has `-E` option to filter for errors
- [x] Documentation updated (README.md, ERROR_LOGGING_FEATURE.md)
- [x] All binaries compile without errors
- [x] Error messages stored in run_logs table with status='ERROR'

## Future Testing Scenarios

1. **Simulate Database Error**: Use `chmod` to make database read-only while running in update mode
2. **Test Concurrent Access**: Run multiple file_tracker instances on same database
3. **File System Errors**: Create symbolic links to non-existent files
4. **Corrupted Database**: Manually corrupt database file and attempt to run
5. **Disk Full**: Fill up disk space and try to update database

## Cleanup

```bash
# Remove test database
rm -f ~/db/FileTracker/test_errors.db

# Remove test directory
rm -rf /tmp/ft_test
```

# Run Logs Checksum Storage - Fix Summary

## Problem
The Compare tab was showing different checksums for identical files because it was pulling checksums from the `files` table (current state) instead of storing and using historical checksums from when each run was performed.

**Example:** If Run 1 scanned a file at time T1 with checksum X, and later the file was modified (checksum Y), then modified back (checksum X), comparing Run 1 to a new Run 2 would incorrectly show different checksums because it was reading the current checksum from `files` table, not the historical one from Run 1.

## Root Cause
- `run_logs` table only stored `status` and `full_path`
- Compare queries joined to `files` table to get checksums: `LEFT JOIN files f ON rl.full_path = f.full_path`
- This gave the **current** checksum, not the **historical** checksum from when the run was performed

## Solution
Store point-in-time checksums in `run_logs` table so comparisons reflect what each run actually saw.

## Changes Made

### 1. Database Schema
Added `checksum TEXT` column to `run_logs` table:
```sql
CREATE TABLE IF NOT EXISTS run_logs (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    run_id INTEGER,
    status TEXT,
    full_path TEXT,
    checksum TEXT,  -- NEW COLUMN
    FOREIGN KEY(run_id) REFERENCES meta(id)
);
```

### 2. Code Changes

#### file_tracker.c (CLI Scanner)
- Added `checksum` field to `LogEntry` struct
- Updated `log_message()` to accept checksum parameter
- Modified all log calls to pass appropriate checksums:
  - **UNCHANGED (mtime only):** Database checksum
  - **UNCHANGED (verified):** Computed checksum
  - **CHANGED:** Newly computed checksum
  - **NEW:** Newly computed checksum
  - **MISSING:** Empty string (file not on disk)
  - **IGNORED:** Empty string
  - **ERROR:** Empty string
- Updated INSERT statement to include checksum column

#### file_tracker_gui.c (GUI Scanner)
- Same changes as CLI version
- Computes checksums before logging for NEW and CHANGED files
- Stores checksums in log buffer for database insertion

#### file_tracker_unified.c (Compare Tab)
- **Before:**
  ```sql
  SELECT rl.full_path, f.checksum, rl.status FROM run_logs rl
  LEFT JOIN files f ON rl.full_path = f.full_path
  WHERE rl.run_id = ?
  ```
- **After:**
  ```sql
  SELECT rl.full_path, rl.checksum, rl.status FROM run_logs rl
  WHERE rl.run_id = ?
  ```
- Now reads historical checksums directly from `run_logs`

### 3. Migration Script
Created `migrate_add_checksum_to_run_logs.sh`:
- Idempotent (safe to re-run)
- Adds `checksum` column to all existing databases
- Existing run_logs entries will have empty checksums
- Future scans will populate checksums

## Behavior Changes

### Before Fix
- Compare tab showed **current** checksums
- False positives when files were modified after a run
- Comparisons didn't reflect actual run state

### After Fix
- Compare tab shows **historical** checksums from when each run was performed
- Accurate point-in-time comparisons
- Each run preserves what it actually saw

### Important Notes

1. **Existing run_logs entries** (pre-migration) have empty checksums
   - They will still work for status comparison
   - Checksum comparison requires both runs to be post-fix

2. **New scans** will populate checksums in run_logs
   - Even in non-checksum mode (mtime-only), checksums are computed for NEW files
   - In checksum mode (`-c`), all checksums are computed and stored

3. **Checksum computation timing**
   - **NEW files:** Always computed before logging
   - **CHANGED files:** Computed to determine change type
   - **UNCHANGED files:** Use DB checksum (mtime mode) or compute (checksum mode)
   - **MISSING/IGNORED/ERROR:** Empty string

## Testing the Fix

### Verify Schema
```bash
sqlite3 ~/db/FileTracker/YourDB.db "PRAGMA table_info(run_logs);"
```
Should show `checksum TEXT` as column 4.

### Test Run Comparison
1. Run a new scan on MediaArch-C1 and MediaArch-C3
2. Use Compare tab in file_tracker_unified
3. Checksums should now match for identical files
4. Historical comparisons preserve point-in-time state

### Validate Checksum Storage
```bash
sqlite3 ~/db/FileTracker/YourDB.db \
  "SELECT full_path, status, checksum FROM run_logs WHERE run_id = <latest_run_id> LIMIT 10;"
```
Should show checksums populated for NEW/CHANGED/UNCHANGED files.

## Files Modified
- file_tracker.c
- file_tracker_gui.c
- file_tracker_unified.c
- migrate_add_checksum_to_run_logs.sh (new)

## Migration Applied
All databases in ~/db/FileTracker/ have been migrated:
- MediaArch-C1.db ✓
- MediaArch-C2.db ✓
- MediaArch-C3.db ✓
- PhotoArch-*.db ✓

## Next Steps
1. Rebuild .app bundles if needed: `make apps`
2. Install to ~/bin if using CLI: `make install`
3. Run new scans on your drives to populate historical checksums
4. Compare runs will now show accurate point-in-time checksums

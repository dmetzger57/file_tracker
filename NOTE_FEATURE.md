# Note Field Feature

## Overview
The file_tracker now supports adding notes to each run. Notes are stored in the `meta` table and can be used to document why a particular scan was performed, what changes were expected, or any other relevant context.

## Database Schema Changes
A new column `note TEXT` has been added to the `meta` table:
```sql
ALTER TABLE meta ADD COLUMN note TEXT;
```

## Migration
For existing databases, run the migration script to add the note field:

### Migrate All Databases
```bash
./migrate_add_note.sh
```
This will migrate all database files in `~/db/FileTracker/`

### Migrate Specific Database
```bash
./migrate_add_note.sh /path/to/specific/database.db
```

### Manual Migration
You can also run the SQL migration directly:
```bash
sqlite3 ~/db/FileTracker/your_database.db < migrate_add_note.sql
```

## Usage

### Adding Notes via Command Line
Use the `-t` option to add a note directly from the command line:
```bash
file_tracker -p /path/to/scan -u -t "Initial backup scan"
```

### Adding Notes from a File
Use the `-N` option to read a note from a file:
```bash
file_tracker -p /path/to/scan -u -N /path/to/note.txt
```

### Requirements
- Both `-t` and `-N` options **require** the `-u` (update) flag
- If both `-t` and `-N` are specified, `-N` takes precedence (file content overwrites inline note)
- Notes can be of any length, though excessively long notes may impact database performance

## Examples

### Example 1: Weekly Backup Scan
```bash
file_tracker -p /home/user/documents -u -t "Weekly backup - 2024-08-21"
```

### Example 2: Post-Maintenance Scan
```bash
echo "Scan after disk cleanup and reorganization" > /tmp/scan_note.txt
file_tracker -p /var/data -u -N /tmp/scan_note.txt
```

### Example 3: Multi-line Note from File
Create a note file with detailed information:
```bash
cat > /tmp/detailed_note.txt << EOF
Migration scan post database upgrade
- Moved files from old_server to new_server
- Verified all checksums
- Contact: admin@example.com
EOF

file_tracker -p /data/migrated -u -N /tmp/detailed_note.txt
```

## Querying Notes
You can query notes from the database:

### View all runs with notes:
```bash
sqlite3 ~/db/FileTracker/your_database.db \
  "SELECT id, last_date_verify, verify_machine, note FROM meta WHERE note IS NOT NULL;"
```

### View the most recent note:
```bash
sqlite3 ~/db/FileTracker/your_database.db \
  "SELECT note FROM meta ORDER BY id DESC LIMIT 1;"
```

### View notes for a specific date range:
```bash
sqlite3 ~/db/FileTracker/your_database.db \
  "SELECT last_date_verify, note FROM meta 
   WHERE last_date_verify BETWEEN '2024-08-01' AND '2024-08-31';"
```

## Technical Details
- Notes are stored as TEXT in SQLite, allowing unlimited length
- Notes are trimmed of trailing newlines when read from files
- NULL is stored if no note is provided
- The note is associated with the entire run, not individual files

#!/bin/bash

# Migration script to add checksum column to run_logs table
# This is idempotent - safe to run multiple times

DB_DIR="$HOME/db/FileTracker"

if [ ! -d "$DB_DIR" ]; then
    echo "Database directory not found: $DB_DIR"
    exit 1
fi

echo "Migrating databases in $DB_DIR..."
echo

for db in "$DB_DIR"/*.db; do
    if [ -f "$db" ]; then
        echo "Processing: $(basename "$db")"

        # Check if column already exists
        existing_columns=$(sqlite3 "$db" "PRAGMA table_info(run_logs);" 2>/dev/null | grep -c "checksum")

        if [ "$existing_columns" -eq 0 ]; then
            # Add checksum column with default empty string
            sqlite3 "$db" "ALTER TABLE run_logs ADD COLUMN checksum TEXT DEFAULT '';" 2>/dev/null
            if [ $? -eq 0 ]; then
                echo "  ✓ Added checksum column"
            else
                echo "  ✗ Failed to add checksum column (table may not exist yet)"
            fi
        else
            echo "  - Column already exists, skipping"
        fi
    fi
done

echo
echo "Migration complete!"
echo
echo "Note: Existing run_logs entries will have empty checksums."
echo "New scans will populate checksums for future comparisons."

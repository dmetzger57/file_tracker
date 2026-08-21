#!/bin/bash

# Migration script to add 'note' field to existing file_tracker databases
# Usage: ./migrate_add_note.sh [database_path]
#
# If no database path is provided, it will migrate all databases in ~/db/FileTracker/

DB_DIR="${HOME}/db/FileTracker"

if [ $# -eq 1 ]; then
    # Single database provided
    DB_PATH="$1"
    if [ ! -f "$DB_PATH" ]; then
        echo "Error: Database file not found: $DB_PATH"
        exit 1
    fi

    echo "Migrating database: $DB_PATH"
    sqlite3 "$DB_PATH" "ALTER TABLE meta ADD COLUMN note TEXT;" 2>&1 | grep -v "duplicate column name"

    if [ $? -eq 0 ] || [ "${PIPESTATUS[0]}" -eq 0 ]; then
        echo "Migration completed successfully for: $DB_PATH"
    else
        echo "Warning: Migration may have failed for: $DB_PATH"
    fi
else
    # Migrate all databases in the default directory
    if [ ! -d "$DB_DIR" ]; then
        echo "Error: Database directory not found: $DB_DIR"
        exit 1
    fi

    echo "Migrating all databases in: $DB_DIR"

    DB_COUNT=0
    for db in "$DB_DIR"/*.db; do
        if [ -f "$db" ]; then
            echo "Migrating: $db"
            sqlite3 "$db" "ALTER TABLE meta ADD COLUMN note TEXT;" 2>&1 | grep -v "duplicate column name"
            DB_COUNT=$((DB_COUNT + 1))
        fi
    done

    if [ $DB_COUNT -eq 0 ]; then
        echo "No database files found in $DB_DIR"
    else
        echo "Migration completed for $DB_COUNT database(s)"
    fi
fi

echo ""
echo "Note: 'duplicate column name' errors are expected if the column already exists"
echo "and can be safely ignored."

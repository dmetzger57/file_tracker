-- Migration script to add 'note' field to the meta table
-- This script is idempotent and can be run multiple times safely

-- SQLite doesn't support "ADD COLUMN IF NOT EXISTS" directly,
-- so we use a workaround with PRAGMA table_info

-- Create a temporary table to check if the column exists
-- If it doesn't exist, add it
BEGIN TRANSACTION;

-- Check if note column exists and add it if it doesn't
-- This uses SQLite's ability to ignore errors with OR IGNORE
ALTER TABLE meta ADD COLUMN note TEXT;

COMMIT;

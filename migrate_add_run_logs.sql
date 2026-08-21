-- Migration script to add 'run_logs' table for storing per-run log messages
-- This script is idempotent and can be run multiple times safely

BEGIN TRANSACTION;

-- Create run_logs table to store log messages per run
CREATE TABLE IF NOT EXISTS run_logs (
    id INTEGER PRIMARY KEY AUTOINCREMENT,
    run_id INTEGER NOT NULL,
    status TEXT NOT NULL,
    full_path TEXT NOT NULL,
    FOREIGN KEY(run_id) REFERENCES meta(id)
);

-- Create index for faster queries by run_id
CREATE INDEX IF NOT EXISTS idx_run_logs_run_id ON run_logs(run_id);

-- Create index for faster filtering by status
CREATE INDEX IF NOT EXISTS idx_run_logs_status ON run_logs(status);

COMMIT;

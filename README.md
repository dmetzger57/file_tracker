# File Tracker
File Tracker creates a SQL database tracking all files in a specified path, including the file name, checksum, last modified date, and owner.

Databases are stored in the ${USER}/Desktop/db/FileTracker folder.

Primary Use Case

The primary use case is the detection of bit-rot in archival storage drives. Run file_tracker against your storage media to detect silent file corruption or unauthorized changes via checksum verification.

The solution is composed of three primary command-line tools:

file_tracker: The core engine for hashing and database management.

file_locator: A utility to find specific files within the tracker databases.

ft_summary: A reporting tool for database statistics.

## file_tracker
Used to create/store checksum values and verify existing files against stored hashes.

### Syntax

file_tracker -p path-1,path-2,path-N [-c] [-u] [-P] [-s]

#### Options

-p: Specifies the path(s) to be processed. If multiple paths are specified, file_tracker will spawn one thread per path for parallel processing.

-c: Compare mode. When running against existing data, this causes file_tracker to compare the current file against the last run. (Note: This has no effect on new files).

-u: Update mode. Instructs the tool to record changed file checks into the database. Without this, it only reports differences.

-P: Progress mode. Displays "Percent Complete" in 1% increments.

-s: Summary mode. Produces a summary of results at the end of path processing.

## file_locator
Searches specified (or all) databases for a specific file entry.

### Syntax

file_locator -f file_name [-p] [-d database] [-v]

#### Options

-f: Specifies the filename to be located.

-p: Match partial file names (fuzzy search).

-d: Specifies a specific database to be searched.

-v: Verbose output.

## ft_summary
Prints statistics about the most recent or historical runs recorded in the database.

#### Syntax

ft_summary -d database [-a]

Options

-d: Specifies the name of the database to be processed.

-a: Display statistics for all runs recorded in the database, rather than just the most recent.

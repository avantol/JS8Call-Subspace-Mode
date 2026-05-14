#ifndef STORAGE_PATH_MIGRATION_H
#define STORAGE_PATH_MIGRATION_H

namespace StoragePathMigration {

// Moves user data left behind in the historic "Subspace Edition"
// AppLocalDataLocation directory into the canonical "JS8Call" directory
// used by older builds and by upgraded users. Idempotent: once complete,
// either the source directory is removed or a "merged" sentinel file is
// dropped into it so subsequent runs skip the work. All operations are
// silent and non-fatal -- partial migrations are acceptable.
void run();

} // namespace StoragePathMigration

#endif // STORAGE_PATH_MIGRATION_H

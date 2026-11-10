#ifndef ATOMIC_FILE_H
#define ATOMIC_FILE_H

// ---------------------------------------------------------------
// CRASH-SAFE, CONCURRENCY-SAFE FILE WRITES
//
// Every CSV is replaced as a whole, never edited in place:
//   1. the new content is written to a temp file next to it and synced
//      ("<path>.<pid>-<thread>-<n>-<random>.tmp": unique per write, so
//      simultaneous writers never touch each other's temp file)
//   2. the temp file is renamed over <path> (an atomic replace; on Windows
//      MoveFileExW with MOVEFILE_REPLACE_EXISTING, retried briefly if a
//      reader has the file open)
// A crash or error leaves <path> either entirely old or entirely new. On a
// reported failure the temp file is removed. A temp file left by a crash is
// deleted by a later write once it is over TEMP_STALE_SECONDS old.
//
// Concurrent replacements of one file are safe: the last one wins and every
// reader sees a complete file. Appends are read-modify-write, so they are
// serialised (in this process by a mutex, across processes by an operating-
// system lock on "<path>.append.lock": flock / LockFileEx). That lock belongs
// to the open handle, so it disappears when its holder exits or crashes and
// is never taken over by timing: a slow writer keeps it, and an append that
// cannot get the lock in time fails instead of losing rows. The lock file is
// left in place. Callers that read, change and replace a file (cards, card
// history, ...) must still serialise those cycles themselves.
//
// Text mode is used, so newline handling matches the old ofstream code.
// ---------------------------------------------------------------

#include <string>

const int TEMP_STALE_SECONDS = 10 * 60;     // orphaned temp files older than this are removed
const int LOCK_TIMEOUT_MS = 15000;          // how long an append waits for the lock before failing

// Replaces the whole file with `content`. Returns false (file untouched) on failure.
bool writeFileAtomically(const std::string& path, const std::string& content);

// Appends `addition` to the file: the old content plus `addition` is written
// atomically. If the file is missing or empty, `headerIfNew` goes first.
// Returns false (file untouched) on failure, including when the lock cannot be
// obtained or the existing file cannot be read - it is never mistaken for an
// empty one.
bool appendToFileAtomically(const std::string& path,
                            const std::string& addition,
                            const std::string& headerIfNew = "");

// Failure injection for tests only; all defaults mean "no fault".
struct AtomicFileTestHooks
{
    long long failWriteAfterBytes = -1;      // >= 0: the write fails after this many bytes
    bool      failRename = false;            // every rename attempt fails (permanently)
    int       transientRenameFailures = 0;   // the next N rename attempts fail with a retryable error
    long long lockTimeoutMs = -1;            // >= 0 replaces LOCK_TIMEOUT_MS
    void    (*beforeRename)() = nullptr;     // called once the temp file is complete;
                                             // may throw to simulate a crash
};

AtomicFileTestHooks& atomicFileTestHooks();

#endif

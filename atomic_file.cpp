#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <functional>
#include <mutex>
#include <random>
#include <sstream>
#include <system_error>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <io.h>
#include <process.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

#include "atomic_file.h"

using namespace std;
namespace fs = std::filesystem;

AtomicFileTestHooks& atomicFileTestHooks()
{
    static AtomicFileTestHooks hooks;
    return hooks;
}

namespace
{

const int RENAME_ATTEMPTS = 40;             // only for retryable errors
const int RENAME_RETRY_DELAY_MS = 25;
const int LOCK_POLL_MS = 5;

// ---------------------------------------------------------------
// unique temp file names
// ---------------------------------------------------------------

string uniqueTempName(const string& path)
{
    static atomic<unsigned long long> counter(0);

#ifdef _WIN32
    const unsigned long pid = static_cast<unsigned long>(_getpid());
#else
    const unsigned long pid = static_cast<unsigned long>(getpid());
#endif

    random_device rd;

    while (true)
    {
        ostringstream name;
        name << path << '.' << pid << '-'
             << hash<thread::id>()(this_thread::get_id()) % 100000 << '-'
             << ++counter << '-' << hex << rd() << ".tmp";

        error_code ec;
        if (!fs::exists(name.str(), ec)) return name.str();
    }
}

// Removes temp files of this path left behind by a crash. Recent ones may
// belong to a writer that is still running, so only old ones are touched.
void removeStaleTemps(const string& path)
{
    error_code ec;
    fs::path p(path);
    fs::path dir = p.parent_path();
    if (dir.empty()) dir = ".";

    const string prefix = p.filename().string() + ".";
    const auto now = fs::file_time_type::clock::now();

    vector<fs::path> stale;

    for (fs::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec))
    {
        const string name = it->path().filename().string();

        if (name.size() < prefix.size() + 3) continue;
        if (name.compare(0, prefix.size(), prefix) != 0) continue;
        if (name.compare(name.size() - 4, 4, ".tmp") != 0) continue;

        error_code e;
        if (!it->is_regular_file(e) || e) continue;

        auto mtime = fs::last_write_time(it->path(), e);
        if (!e && now - mtime > chrono::seconds(TEMP_STALE_SECONDS))
            stale.push_back(it->path());
    }

    for (const auto& f : stale)
    {
        error_code e;
        fs::remove(f, e);
    }
}

// ---------------------------------------------------------------
// durability
// ---------------------------------------------------------------

// Pushes the file's data to the disk before it is renamed into place.
bool syncFile(FILE* f)
{
#ifdef _WIN32
    return _commit(_fileno(f)) == 0;
#else
    return fsync(fileno(f)) == 0;
#endif
}

// Best effort: makes the rename itself durable (Windows does it with
// MOVEFILE_WRITE_THROUGH).
void syncDirectoryOf(const string& path)
{
#ifndef _WIN32
    fs::path dir = fs::path(path).parent_path();
    if (dir.empty()) dir = ".";

    int fd = open(dir.c_str(), O_RDONLY);
    if (fd >= 0)
    {
        fsync(fd);
        close(fd);
    }
#else
    (void)path;
#endif
}

// ---------------------------------------------------------------
// replacing the destination
// ---------------------------------------------------------------

// Moves `from` over `to`, replacing `to` if it exists. Returns 0 on success;
// otherwise an error number, with `retryable` set if trying again may work
// (Windows: the destination is briefly open in another handle).
int replaceFile(const string& from, const string& to, bool& retryable)
{
    retryable = false;

#ifdef _WIN32
    if (MoveFileExW(fs::path(from).wstring().c_str(),
                    fs::path(to).wstring().c_str(),
                    MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        return 0;

    const DWORD e = GetLastError();

    retryable = (e == ERROR_ACCESS_DENIED ||
                 e == ERROR_SHARING_VIOLATION ||
                 e == ERROR_LOCK_VIOLATION);

    return static_cast<int>(e);
#else
    error_code ec;
    fs::rename(from, to, ec);                      // rename(2) replaces atomically
    return ec ? ec.value() : 0;
#endif
}

bool renameIntoPlace(const string& from, const string& to)
{
    AtomicFileTestHooks& hooks = atomicFileTestHooks();

    for (int attempt = 1; ; attempt++)
    {
        bool retryable = false;
        int rc;

        if (hooks.failRename)
        {
            rc = EIO;
        }
        else if (hooks.transientRenameFailures > 0)
        {
            hooks.transientRenameFailures--;
            rc = EACCES;
            retryable = true;
        }
        else
        {
            rc = replaceFile(from, to, retryable);
        }

        if (rc == 0) return true;
        if (!retryable || attempt >= RENAME_ATTEMPTS) return false;

        this_thread::sleep_for(chrono::milliseconds(RENAME_RETRY_DELAY_MS));
    }
}

// ---------------------------------------------------------------
// append lock
// ---------------------------------------------------------------

mutex& appendMutex()
{
    static mutex m;
    return m;
}

// One lock file per physical file, whatever way its path is spelled.
string lockFileFor(const string& path)
{
    error_code ec;
    fs::path p = fs::weakly_canonical(path, ec);
    return (ec ? path : p.string()) + ".append.lock";
}

// Cross-process lock held on an operating-system file lock (flock on POSIX,
// LockFileEx on Windows). The system ties it to the open handle, so it is
// released the moment the holder closes it or dies, and it can never be held
// by a process that has gone. There is deliberately no time-based takeover:
// a slow writer keeps its lock for as long as it needs, and a waiter that runs
// out of time fails instead of stealing it. The lock file itself is never
// deleted (deleting it would let two processes lock different files).
class AppendLock
{
public:
    AppendLock(const string& path, chrono::steady_clock::time_point deadline)
    {
        const string lockPath = lockFileFor(path);

#ifdef _WIN32
        handle = CreateFileW(fs::path(lockPath).wstring().c_str(),
                             GENERIC_READ | GENERIC_WRITE,
                             FILE_SHARE_READ | FILE_SHARE_WRITE,
                             nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);

        if (handle == INVALID_HANDLE_VALUE) return;

        while (true)
        {
            OVERLAPPED range = OVERLAPPED();           // first byte of the file

            if (LockFileEx(handle, LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                           0, 1, 0, &range))
            {
                held = true;
                return;
            }

            if (GetLastError() != ERROR_LOCK_VIOLATION) break;     // not "someone else has it"
            if (chrono::steady_clock::now() >= deadline) break;

            this_thread::sleep_for(chrono::milliseconds(LOCK_POLL_MS));
        }

        CloseHandle(handle);
        handle = INVALID_HANDLE_VALUE;
#else
        fd = open(lockPath.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);

        if (fd < 0) return;

        while (true)
        {
            if (flock(fd, LOCK_EX | LOCK_NB) == 0)
            {
                held = true;
                return;
            }

            if (errno != EWOULDBLOCK && errno != EINTR) break;     // a real error
            if (chrono::steady_clock::now() >= deadline) break;

            this_thread::sleep_for(chrono::milliseconds(LOCK_POLL_MS));
        }

        close(fd);
        fd = -1;
#endif
    }

    ~AppendLock()
    {
#ifdef _WIN32
        if (handle != INVALID_HANDLE_VALUE)
        {
            if (held)
            {
                OVERLAPPED range = OVERLAPPED();
                UnlockFileEx(handle, 0, 1, 0, &range);
            }
            CloseHandle(handle);
        }
#else
        if (fd >= 0)
        {
            if (held) flock(fd, LOCK_UN);
            close(fd);
        }
#endif
    }

    AppendLock(const AppendLock&) = delete;
    AppendLock& operator=(const AppendLock&) = delete;

    bool ok() const { return held; }

private:
#ifdef _WIN32
    HANDLE handle = INVALID_HANDLE_VALUE;
#else
    int fd = -1;
#endif
    bool held = false;
};

} // namespace


// ==========================================================
// REPLACE A WHOLE FILE
// ==========================================================

bool writeFileAtomically(const string& path, const string& content)
{
    const AtomicFileTestHooks& hooks = atomicFileTestHooks();
    const string tmp = uniqueTempName(path);

    FILE* f = fopen(tmp.c_str(), "w");
    if (!f) return false;                          // nothing created, path untouched

    bool ok = true;
    size_t toWrite = content.size();

    if (hooks.failWriteAfterBytes >= 0 &&
        static_cast<size_t>(hooks.failWriteAfterBytes) < toWrite)
    {
        toWrite = static_cast<size_t>(hooks.failWriteAfterBytes);
        ok = false;                                // simulated error part-way through
    }

    if (toWrite > 0 && fwrite(content.data(), 1, toWrite, f) != toWrite) ok = false;
    if (ok && fflush(f) != 0) ok = false;
    if (ok && !syncFile(f)) ok = false;
    if (fclose(f) != 0) ok = false;

    error_code ec;

    if (!ok)
    {
        fs::remove(tmp, ec);
        return false;
    }

    if (hooks.beforeRename) hooks.beforeRename();  // tests: simulated crash point

    if (!renameIntoPlace(tmp, path))
    {
        fs::remove(tmp, ec);
        return false;
    }

    syncDirectoryOf(path);
    removeStaleTemps(path);
    return true;
}


// ==========================================================
// APPEND (read, add, replace) under a lock
// ==========================================================

bool appendToFileAtomically(const string& path,
                            const string& addition,
                            const string& headerIfNew)
{
    const AtomicFileTestHooks& hooks = atomicFileTestHooks();
    const long long timeoutMs =
        hooks.lockTimeoutMs >= 0 ? hooks.lockTimeoutMs : LOCK_TIMEOUT_MS;
    const auto deadline =
        chrono::steady_clock::now() + chrono::milliseconds(timeoutMs);

    // Threads of this process queue on the mutex, other processes on the file
    // lock. Both are held for the whole read-modify-replace, and an append
    // that cannot get them in time fails: it never goes ahead unlocked.
    unique_lock<mutex> sameProcess(appendMutex(), defer_lock);

    while (!sameProcess.try_lock())                    // polled, so the wait is bounded
    {
        if (chrono::steady_clock::now() >= deadline) return false;
        this_thread::sleep_for(chrono::milliseconds(LOCK_POLL_MS));
    }

    AppendLock otherProcesses(path, deadline);
    if (!otherProcesses.ok()) return false;

    string existing;

    error_code ec;
    if (fs::exists(path, ec))
    {
        if (fs::is_directory(path, ec)) return false;

        ifstream in(path);
        if (!in.is_open()) return false;           // present but unreadable: do not overwrite

        stringstream buffer;
        buffer << in.rdbuf();

        if (in.bad()) return false;

        existing = buffer.str();
    }

    string content;

    if (existing.empty()) content = headerIfNew + addition;
    else                  content = existing + addition;

    return writeFileAtomically(path, content);
}

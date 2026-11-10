// Tests for crash-safe CSV persistence. Build/run: make test_atomic
// Runs inside a temporary directory; project data is never touched.
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <atomic>
#include <chrono>
#include <set>
#include <stdexcept>
#include <thread>
#include <vector>
#ifndef _WIN32
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "atomic_file.h"
#include "card_history.h"
#include "dequemodule.h"
#include "hashmodule.h"
#include "location_history.h"
#include "transaction_history.h"

using namespace std;
namespace fs = std::filesystem;

static int passed = 0, failed = 0;
static ostream* out = nullptr;                  // real stdout (cout is silenced)

static void check(const string& name, bool ok)
{
    *out << (ok ? "[PASS] " : "[FAIL] ") << name << '\n';
    ok ? passed++ : failed++;
}

static string slurp(const string& f)
{
    ifstream in(f, ios::binary);
    stringstream s;
    s << in.rdbuf();
    return s.str();
}

static void put(const string& f, const string& content)
{
    ofstream o(f, ios::binary);
    o << content;
}

static vector<string> temps()                   // leftover "*.tmp" regular files
{
    vector<string> r;
    for (const auto& e : fs::directory_iterator("."))
        if (e.is_regular_file() && e.path().extension() == ".tmp")
            r.push_back(e.path().filename().string());
    return r;
}

struct Faults                                   // resets all hooks on scope exit
{
    ~Faults() { atomicFileTestHooks() = AtomicFileTestHooks(); }
    void failRename()          { atomicFileTestHooks().failRename = true; }
    void failWriteAfter(long long n) { atomicFileTestHooks().failWriteAfterBytes = n; }
    void clear()               { atomicFileTestHooks() = AtomicFileTestHooks(); }
};

static void crash() { throw runtime_error("simulated crash"); }

// Is the append lock of this file free right now?
static bool lockIsFree(const string& path)
{
    long long saved = atomicFileTestHooks().lockTimeoutMs;
    atomicFileTestHooks().lockTimeoutMs = 300;
    bool free = appendToFileAtomically(path, "", "");   // adds nothing
    atomicFileTestHooks().lockTimeoutMs = saved;
    return free;
}

#ifndef _WIN32
// ---- helpers for tests that run appenders in separate processes ----
static int signalFd = -1;                       // child -> parent: "I hold the lock"

static void holdLockBriefly()                   // runs inside an append, lock held
{
    char c = 'x';
    if (write(signalFd, &c, 1) < 0) _exit(2);
    usleep(1500 * 1000);
}

static void holdLockForever()
{
    char c = 'x';
    if (write(signalFd, &c, 1) < 0) _exit(2);
    for (;;) pause();
}

static void waitForByte(int fd)                 // blocks until the other side writes
{
    char c;
    if (read(fd, &c, 1) < 0) _exit(2);
}

static pid_t forkAppender(const string& file, const string& tag, int rows, int startFd, long long timeoutMs)
{
    pid_t pid = fork();
    if (pid != 0) return pid;

    if (startFd >= 0) waitForByte(startFd);     // start together with the others
    if (timeoutMs >= 0) atomicFileTestHooks().lockTimeoutMs = timeoutMs;

    int bad = 0;
    for (int i = 0; i < rows; i++)
        if (!appendToFileAtomically(file, tag + "-" + to_string(i) + "\n", "H\n")) bad++;
    _exit(bad == 0 ? 0 : 1);
}

static int exitCode(pid_t pid)
{
    int st = 0;
    waitpid(pid, &st, 0);
    return WIFEXITED(st) ? WEXITSTATUS(st) : 100 + (WIFSIGNALED(st) ? WTERMSIG(st) : 0);
}

static pid_t forkHolder(const string& file, const string& tag, void (*hook)(), int signalWriteFd)
{
    pid_t pid = fork();
    if (pid != 0) return pid;

    signalFd = signalWriteFd;
    atomicFileTestHooks().beforeRename = hook;  // runs with the lock held
    _exit(appendToFileAtomically(file, tag + "\n", "H\n") ? 0 : 1);
}

static vector<string> rowsOf(const string& file, string& header)
{
    istringstream in(slurp(file));
    string line;
    getline(in, header);
    vector<string> r;
    while (getline(in, line)) r.push_back(line);
    return r;
}
#endif

static Transaction txn(int id, int card)
{
    Transaction t;
    t.id = id; t.cardNo = card; t.amount = 1234.5;
    t.merchant = "Amazon"; t.location = "Delhi";
    t.latitude = 28.6139; t.longitude = 77.209;
    t.timestamp = "2026-11-03 10:00:00"; t.deviceId = "dev1";
    t.prelimScore = 10; t.finalScore = 10;
    t.otpStatus = "NOT_REQUIRED"; t.authStatus = "NOT_REQUIRED";
    t.fraudStatus = "NORMAL"; t.decision = "ALLOWED";
    t.failedAuthAttempts = 0; t.reasons = "No risk factors";
    return t;
}

int main()
{
    fs::path origin = fs::current_path();
    fs::path tmp = fs::temp_directory_path() / ("atomic_test_" + to_string(time(nullptr)) + "_" + to_string(rand()));
    fs::create_directories(tmp);
    fs::current_path(tmp);

    ostringstream sink;
    streambuf* real = cout.rdbuf(sink.rdbuf());
    ostream realOut(real);
    out = &realOut;

    // ================= writeFileAtomically =================
    {
        check("write: creates a new file with exact content, no temp left",
              writeFileAtomically("a.csv", "h\n1,2\n") && slurp("a.csv") == "h\n1,2\n" && temps().empty());
        check("write: replaces existing content exactly",
              writeFileAtomically("a.csv", "new\n") && slurp("a.csv") == "new\n" && temps().empty());
        check("write: empty content gives an empty file",
              writeFileAtomically("e.csv", "") && fs::exists("e.csv") && slurp("e.csv").empty());
    }
    {
        bool ok = writeFileAtomically("no_such_dir/b.csv", "NEW\n");   // temp file cannot be created
        check("write fails (temp cannot be created): false, nothing created",
              !ok && !fs::exists("no_such_dir") && temps().empty());
    }
    {
        Faults f;
        put("c.csv", "ORIGINAL\n");
        f.failWriteAfter(7);
        bool ok = writeFileAtomically("c.csv", "NEW CONTENT THAT IS LONGER\n");
        check("write fails part-way: false, original intact, temp removed",
              !ok && slurp("c.csv") == "ORIGINAL\n" && temps().empty());

        f.clear();
        check("write fails part-way: nothing left behind for a new file",
              (f.failWriteAfter(3), !writeFileAtomically("new1.csv", "abcdefgh")) && !fs::exists("new1.csv") && temps().empty());
    }
    {
        Faults f;
        put("d.csv", "ORIGINAL\n");
        f.failRename();
        bool ok = writeFileAtomically("d.csv", "NEW\n");
        check("rename fails (injected): false, original intact, temp removed",
              !ok && slurp("d.csv") == "ORIGINAL\n" && temps().empty());

        f.clear();
        check("rename fails: retry succeeds with exact content, no temp",
              writeFileAtomically("d.csv", "NEW\n") && slurp("d.csv") == "NEW\n" && temps().empty());
    }
    {
        fs::create_directory("dir.csv");
        put("dir.csv/child.txt", "keep me");
        bool ok = writeFileAtomically("dir.csv", "NEW\n");     // real rename failure
        check("rename fails (real OS error): false, target and temp handled",
              !ok && fs::is_directory("dir.csv") && slurp("dir.csv/child.txt") == "keep me" && temps().empty());
        fs::remove_all("dir.csv");
    }
    {
        Faults f;
        put("g.csv", "ORIGINAL\n");
        atomicFileTestHooks().beforeRename = crash;
        bool crashed = false;
        try { writeFileAtomically("g.csv", "NEW\n"); } catch (const runtime_error&) { crashed = true; }
        atomicFileTestHooks().beforeRename = nullptr;

        check("crash before rename: original still fully intact", crashed && slurp("g.csv") == "ORIGINAL\n");
        check("crash before rename: the temp file holds the complete new content",
              temps().size() == 1 && slurp(temps()[0]) == "NEW\n");

        check("crash recovery: next write succeeds; a fresh leftover temp is not touched (may be a live writer's)",
              writeFileAtomically("g.csv", "NEWER\n") && slurp("g.csv") == "NEWER\n" && temps().size() == 1);

        fs::last_write_time(temps()[0], fs::file_time_type::clock::now() - chrono::hours(2));
        check("crash recovery: a leftover temp over 10 minutes old is removed by the next write",
              writeFileAtomically("g.csv", "NEWEST\n") && slurp("g.csv") == "NEWEST\n" && temps().empty());
    }


    // ================= concurrency =================
    // (multi-process test first: forking is only safe before any threads exist)

#ifndef _WIN32
    {   // contended appends from many processes released at the same instant
        const int procs = 8, per = 25;
        int gate[2];
        if (pipe(gate) != 0) return 2;

        vector<pid_t> kids;
        for (int p = 0; p < procs; p++) kids.push_back(forkAppender("mp.csv", "p" + to_string(p), per, gate[0], -1));
        for (int p = 0; p < procs; p++) { char go = 1; if (write(gate[1], &go, 1) < 0) return 2; }   // release all

        int bad = 0;
        for (pid_t k : kids) if (exitCode(k) != 0) bad++;
        close(gate[0]); close(gate[1]);

        string header;
        vector<string> rows = rowsOf("mp.csv", header);
        set<string> uniq(rows.begin(), rows.end());

        check("concurrent appends from 8 processes: all succeed, 200 rows, none lost or duplicated, header once",
              bad == 0 && header == "H" && (int)rows.size() == procs * per && (int)uniq.size() == procs * per);
        check("concurrent appends from 8 processes: no temp left, lock free", temps().empty() && lockIsFree("mp.csv"));
    }

    {   // a long-running holder keeps its lock; it is never taken over because of elapsed time
        put("long.csv", "H\nexisting\n");
        int sig[2];
        if (pipe(sig) != 0) return 2;

        pid_t holder = forkHolder("long.csv", "holder", holdLockBriefly, sig[1]);
        waitForByte(sig[0]);                                  // the holder now holds the lock

        // Make the lock look as old as possible: a timing-based takeover would steal it now.
        fs::path lockFile = fs::weakly_canonical("long.csv").string() + ".append.lock";
        fs::last_write_time(lockFile, fs::file_time_type::clock::now() - chrono::hours(48));

        auto t0 = chrono::steady_clock::now();
        vector<pid_t> contenders;
        for (int i = 0; i < 3; i++) contenders.push_back(forkAppender("long.csv", "intruder" + to_string(i), 1, -1, 300));
        int refused = 0;
        for (pid_t c : contenders) if (exitCode(c) == 1) refused++;
        long long waitedMs = chrono::duration_cast<chrono::milliseconds>(chrono::steady_clock::now() - t0).count();

        string before = slurp("long.csv");
        check("long-held lock: contenders fail instead of stealing it, even though the lock file is 48 hours old",
              refused == 3 && waitedMs < 1400);
        check("long-held lock: nothing was written while the holder was still working", before == "H\nexisting\n");

        check("long-held lock: the holder finishes its append", exitCode(holder) == 0);
        string header;
        vector<string> rows = rowsOf("long.csv", header);
        check("long-held lock: holder's row landed exactly once, intruders' rows never did",
              rows.size() == 2 && rows[0] == "existing" && rows[1] == "holder");
        check("long-held lock: free again afterwards, and a normal append works",
              lockIsFree("long.csv") && appendToFileAtomically("long.csv", "after\n", "H\n"));
        close(sig[0]); close(sig[1]);
    }

    {   // one file reached through a symlink shares the same lock
        put("real.csv", "H\nexisting\n");
        fs::create_symlink("real.csv", "link.csv");
        int sig[2];
        if (pipe(sig) != 0) return 2;

        pid_t holder = forkHolder("real.csv", "holder", holdLockBriefly, sig[1]);
        waitForByte(sig[0]);
        pid_t viaLink = forkAppender("link.csv", "viaLink", 1, -1, 300);

        check("lock path: the same file reached through a symlink is blocked by the holder's lock", exitCode(viaLink) == 1);
        exitCode(holder);
        string header;
        vector<string> rows = rowsOf("real.csv", header);
        check("lock path: only the holder's row was written", rows.size() == 2 && rows[1] == "holder");
        close(sig[0]); close(sig[1]);
        fs::remove("link.csv");
    }

    {   // holder crashes mid-append while others wait; they all take over at once
        put("crash.csv", "H\nexisting\n");
        int sig[2];
        if (pipe(sig) != 0) return 2;

        pid_t holder = forkHolder("crash.csv", "doomed", holdLockForever, sig[1]);
        waitForByte(sig[0]);                                  // holder has the lock, temp file written

        const int procs = 6, per = 20;
        vector<pid_t> waiters;
        for (int p = 0; p < procs; p++) waiters.push_back(forkAppender("crash.csv", "w" + to_string(p), per, -1, -1));

        usleep(200 * 1000);                                   // let them all queue up on the lock
        auto t0 = chrono::steady_clock::now();
        kill(holder, SIGKILL);                                // crash while holding the lock
        int holderCode = exitCode(holder);

        int bad = 0;
        for (pid_t w : waiters) if (exitCode(w) != 0) bad++;
        long long recoverMs = chrono::duration_cast<chrono::milliseconds>(chrono::steady_clock::now() - t0).count();

        string header;
        vector<string> rows = rowsOf("crash.csv", header);
        set<string> uniq(rows.begin(), rows.end());
        bool hasDoomed = uniq.count("doomed") > 0;

        check("crashed holder: the waiting processes all recover and finish (no waiting for any timeout)",
              holderCode == 100 + SIGKILL && bad == 0 && recoverMs < 5000);
        check("crashed holder: 6 simultaneous takeovers lose nothing: existing + 120 rows, no duplicates",
              header == "H" && (int)rows.size() == 1 + procs * per && (int)uniq.size() == 1 + procs * per && !hasDoomed);
        check("crashed holder: the half-finished append left the data valid; lock is free",
              lockIsFree("crash.csv"));
        close(sig[0]); close(sig[1]);

        // the crashed process left a complete but unused temp file; remove it for later checks
        for (const auto& x : temps()) fs::remove(x);
    }
#endif

    {   // many threads replacing the same file
        const int threads = 8, iters = 40;
        atomic<int> failures(0), corrupt(0);
        atomic<bool> done(false);

        auto content = [](int t, int i) { string m = "T" + to_string(t) + "-I" + to_string(i) + "\n", s; for (int k = 0; k < 300; k++) s += m; return s; };
        auto wellFormed = [](const string& s) {
            if (s.empty()) return false;
            istringstream in(s); string first, line; getline(in, first);
            int n = 1;
            while (getline(in, line)) { if (line != first) return false; n++; }
            return n == 300;
        };

        writeFileAtomically("cw.csv", content(0, 0));

        thread reader([&] {
            while (!done) { string s = slurp("cw.csv"); if (!wellFormed(s)) corrupt++; }
        });

        vector<thread> ts;
        for (int t = 0; t < threads; t++)
            ts.emplace_back([&, t] { for (int i = 0; i < iters; i++) if (!writeFileAtomically("cw.csv", content(t, i))) failures++; });
        for (auto& th : ts) th.join();
        done = true;
        reader.join();

        check("concurrent replacements (8 threads x 40, same file): all succeed", failures == 0);
        check("concurrent replacements: every read saw a complete file, never a mix or partial", corrupt == 0 && wellFormed(slurp("cw.csv")));
        check("concurrent replacements: no temp files left", temps().empty());
    }
    {   // many threads appending
        const int threads = 8, per = 25;
        atomic<int> failures(0);
        vector<thread> ts;

        for (int t = 0; t < threads; t++)
            ts.emplace_back([&, t] { for (int i = 0; i < per; i++)
                if (!appendToFileAtomically("ca.csv", "t" + to_string(t) + "-" + to_string(i) + "\n", "H\n")) failures++; });
        for (auto& th : ts) th.join();

        istringstream in(slurp("ca.csv"));
        string line, header;
        getline(in, header);
        set<string> seen;
        int count = 0;
        while (getline(in, line)) { seen.insert(line); count++; }

        check("concurrent appends (8 threads x 25): all succeed, 200 rows, none lost or duplicated, header once",
              failures == 0 && header == "H" && count == threads * per && (int)seen.size() == threads * per);
        check("concurrent appends: no temp left, lock free", temps().empty() && lockIsFree("ca.csv"));
    }
    {   // the real transaction writer under concurrency
        const int threads = 6, per = 20;
        vector<thread> ts;
        atomic<int> failures(0);

        for (int t = 0; t < threads; t++)
            ts.emplace_back([&, t] { for (int i = 0; i < per; i++)
                if (!appendTransaction(txn(1000 + t * 100 + i, 8000 + t), "ct.csv")) failures++; });
        for (auto& th : ts) th.join();

        vector<Transaction> h;
        loadTransactions(h, "ct.csv");
        set<int> ids;
        for (const auto& x : h) ids.insert(x.id);

        check("concurrent appendTransaction (6 threads x 20): 120 rows, every ID present once",
              failures == 0 && (int)h.size() == threads * per && (int)ids.size() == threads * per && temps().empty());
    }
    {   // failures under concurrency leave nothing behind
        Faults f;
        writeFileAtomically("cf.csv", "ORIGINAL\n");
        f.failRename();
        atomic<int> succeeded(0);
        vector<thread> ts;
        for (int t = 0; t < 8; t++)
            ts.emplace_back([&] { for (int i = 0; i < 10; i++) if (writeFileAtomically("cf.csv", "NEW\n")) succeeded++; });
        for (auto& th : ts) th.join();
        check("concurrent failed writes: all report failure, original intact, no temps",
              succeeded == 0 && slurp("cf.csv") == "ORIGINAL\n" && temps().empty());
    }

    // ================= unique temp files =================
    {
        Faults f;
        put("u.csv", "ORIGINAL\n");
        atomicFileTestHooks().beforeRename = crash;
        for (const char* c : {"FIRST\n", "SECOND\n"})
        {
            try { writeFileAtomically("u.csv", c); } catch (const runtime_error&) {}
        }
        atomicFileTestHooks().beforeRename = nullptr;

        vector<string> t = temps();
        bool bothComplete = t.size() == 2 &&
            ((slurp(t[0]) == "FIRST\n" && slurp(t[1]) == "SECOND\n") || (slurp(t[0]) == "SECOND\n" && slurp(t[1]) == "FIRST\n"));
        check("two writes to one file use different temp files (neither overwrites the other)", bothComplete && slurp("u.csv") == "ORIGINAL\n");

        for (const auto& x : t) fs::last_write_time(x, fs::file_time_type::clock::now() - chrono::hours(2));
        check("old leftover temp files are all swept by the next write",
              writeFileAtomically("u.csv", "DONE\n") && temps().empty());
    }

    // ================= replacing an existing destination =================
    {
        bool ok = true;
        string expect;
        for (int i = 0; i < 10; i++)                    // grow and shrink
        {
            expect = string((i % 2 ? 5 : 5000), 'a' + i) + "\n";
            ok = ok && writeFileAtomically("r.csv", expect) && slurp("r.csv") == expect;
        }
        check("replace: an existing destination is replaced exactly, repeatedly, growing and shrinking", ok && temps().empty());
    }
#ifndef _WIN32
    {
        put("rd.csv", "OLD VERSION\n");
        ifstream reader("rd.csv");                      // a reader has the file open
        bool ok = writeFileAtomically("rd.csv", "NEW VERSION\n");
        string seen;
        getline(reader, seen);
        check("replace: works while a reader has the old file open; the reader still sees a complete old version",
              ok && seen == "OLD VERSION" && slurp("rd.csv") == "NEW VERSION\n");
    }
#endif
    {
        Faults f;
        put("t.csv", "ORIGINAL\n");
        atomicFileTestHooks().transientRenameFailures = 3;
        check("replace: retries a briefly-blocked rename (e.g. Windows sharing violation) and succeeds",
              writeFileAtomically("t.csv", "NEW\n") && slurp("t.csv") == "NEW\n" && temps().empty());

        put("t2.csv", "ORIGINAL\n");
        atomicFileTestHooks().transientRenameFailures = 100000;
        bool ok = writeFileAtomically("t2.csv", "NEW\n");
        check("replace: a rename that stays blocked fails cleanly: original intact, temp removed",
              !ok && slurp("t2.csv") == "ORIGINAL\n" && temps().empty());
    }

    // ================= append lock (in-process) =================
    {
        Faults f;
        put("l.csv", "H\n1\n");
        f.failRename();
        bool ok = appendToFileAtomically("l.csv", "2\n", "H\n");
        bool intact = slurp("l.csv") == "H\n1\n";
        f.clear();
        check("append lock: released after a failed append", !ok && intact && lockIsFree("l.csv"));

        check("append lock: usable again after that failure", appendToFileAtomically("l.csv", "2\n", "H\n") && slurp("l.csv") == "H\n1\n2\n");
    }

    // ================= appendToFileAtomically =================
    {
        check("append: new file gets header then row",
              appendToFileAtomically("p.csv", "1,a\n", "H\n") && slurp("p.csv") == "H\n1,a\n");
        check("append: existing file gets only the row (no second header)",
              appendToFileAtomically("p.csv", "2,b\n", "H\n") && slurp("p.csv") == "H\n1,a\n2,b\n" && temps().empty());
        put("crlf.csv", "H\r\n1,a\r\n");
        appendToFileAtomically("crlf.csv", "2,b\n", "H\n");
        check("append: existing bytes (even CRLF) are preserved verbatim", slurp("crlf.csv") == "H\r\n1,a\r\n2,b\n");
    }
    {
        Faults f;
        put("q.csv", "H\n1,a\n");
        f.failRename();
        bool ok = appendToFileAtomically("q.csv", "2,b\n", "H\n");
        check("append fails: false, original intact, temp removed", !ok && slurp("q.csv") == "H\n1,a\n" && temps().empty());

        f.clear();
        appendToFileAtomically("q.csv", "2,b\n", "H\n");
        check("append retry: exactly one new row", slurp("q.csv") == "H\n1,a\n2,b\n");
    }
    {
        fs::create_directory("qd.csv");
        bool ok = appendToFileAtomically("qd.csv", "2,b\n", "H\n");
        check("append to a directory: false, untouched, no temp", !ok && fs::is_directory("qd.csv") && temps().empty());
        fs::remove("qd.csv");
    }

    // ================= the five CSV writers =================
    // each: failed replace -> false + file byte-identical + no temp; retry -> correct

    {   // transactions.csv
        Faults f;
        appendTransaction(txn(1, 7001));
        string golden = "TransactionID,CardID,Amount,Merchant,Location,Latitude,Longitude,Timestamp,DeviceID,"
                        "PrelimScore,FinalScore,OTPStatus,AuthStatus,FraudStatus,Decision,FailedAuthAttempts,Reasons\n"
                        "1,7001,1234.5,AMAZON,DELHI,28.6139,77.209,2026-11-03 10:00:00,dev1,10,10,"
                        "NOT_REQUIRED,NOT_REQUIRED,NORMAL,ALLOWED,0,No risk factors\n";
        check("transactions: file format unchanged (header + row)", slurp(TRANSACTION_FILE) == golden);

        f.failRename();
        bool ok = appendTransaction(txn(2, 7001));
        check("transactions: failed append leaves the file byte-identical", !ok && slurp(TRANSACTION_FILE) == golden && temps().empty());

        f.clear();
        vector<Transaction> h;
        bool again = appendTransaction(txn(2, 7001));
        loadTransactions(h);
        check("transactions: retry adds the row exactly once", again && h.size() == 2 && h[0].id == 1 && h[1].id == 2);
    }
    {   // cards.csv
        Faults f;
        addCard(7001, "Tester", "Credit", "Active", 5);
        loadCards();
        string before = slurp("cards.csv");
        check("cards: file format unchanged",
              before == "CardID,CardHolder,AccountType,Status,Points\n7001,Tester,Credit,Active,5\n");

        f.failRename();
        Card c;
        bool ok = updatePoints(7001, 99);
        searchCard(7001, c);
        check("cards: failed save -> false, file identical, memory rolled back, no temp",
              !ok && slurp("cards.csv") == before && c.points == 5 && temps().empty());

        f.clear();
        ok = updatePoints(7001, 99);
        loadCards();
        searchCard(7001, c);
        check("cards: retry persists the new points", ok && c.points == 99 && temps().empty());
    }
    {   // location_history.csv
        Faults f;
        saveLocation(7001, "DELHI", 28.6, 77.2, 100);
        string before = slurp("location_history.csv");

        f.failRename();
        bool ok = saveLocation(7001, "MUMBAI", 19.0, 72.8, 200);
        PreviousLocation p;
        getPreviousLocation(7001, p);
        check("locations: failed save -> false, file identical, old location still read",
              !ok && slurp("location_history.csv") == before && p.location == "DELHI" && temps().empty());

        f.clear();
        ok = saveLocation(7001, "MUMBAI", 19.0, 72.8, 200);
        getPreviousLocation(7001, p);
        check("locations: retry saves the new location", ok && p.location == "MUMBAI" && p.timestamp == 200);
    }
    {   // card_history.csv
        Faults f;
        CardHistory c1;
        c1.cardNo = 7001;
        updateCardHistory(c1, txn(1, 7001), "ch.csv");
        string before = slurp("ch.csv");

        f.failRename();
        CardHistory c2 = c1;
        bool ok = updateCardHistory(c2, txn(2, 7001), "ch.csv");
        check("card history: failed save -> false, file identical, no temp", !ok && slurp("ch.csv") == before && temps().empty());

        f.clear();
        CardHistory c3 = c1;                                    // retry from the saved state
        ok = updateCardHistory(c3, txn(2, 7001), "ch.csv");
        vector<CardHistory> all;
        loadAllCardHistory(all, "ch.csv");
        check("card history: retry counts the transaction exactly once",
              ok && all.size() == 1 && all[0].totalTransactions == 2);
    }
    {   // review_queue.csv
        Faults f;
        TransactionDeque q;
        QueueItem a;
        a.id = 1; a.cardNo = 7001; a.amount = 50; a.score = 75; a.status = "CRITICAL"; a.timestamp = "2026-11-03 10:00:00";
        q.insertRear(a);
        q.save("rq.csv");
        string before = slurp("rq.csv");
        check("review queue: file format unchanged",
              before == "TransactionID,CardID,Amount,Score,Status,Timestamp\n1,7001,50,75,CRITICAL,2026-11-03 10:00:00\n");

        a.id = 2;
        q.insertRear(a);
        f.failRename();
        bool ok = q.save("rq.csv");
        check("review queue: failed save -> false, file identical, no temp", !ok && slurp("rq.csv") == before && temps().empty());

        f.clear();
        ok = q.save("rq.csv");
        TransactionDeque loaded;
        loaded.load("rq.csv");
        check("review queue: retry saves both items", ok && loaded.size() == 2);
    }

    // ---------- cleanup ----------
    cout.rdbuf(real);
    fs::current_path(origin);
    fs::remove_all(tmp);

    cout << "\n" << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}

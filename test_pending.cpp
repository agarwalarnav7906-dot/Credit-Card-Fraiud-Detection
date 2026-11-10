// Tests for PendingTransactionManager. Build/run: make test_pending
// Runs inside a temporary copy of the CSV files, so project data is never touched.
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <set>
#include <sstream>
#include <thread>

#include "transaction_service.h"
#include "location_history.h"
#include "transaction_history.h"

using namespace std;
namespace fs = std::filesystem;

static int passed = 0, failed = 0;
static ostream* out = nullptr;          // real stdout (cout itself is silenced)

static void check(const string& name, bool ok)
{
    *out << (ok ? "[PASS] " : "[FAIL] ") << name << '\n';
    ok ? passed++ : failed++;
}

static LocationGraph graph;
static int nextCard = 5000;

static int rows()
{
    ifstream in(TRANSACTION_FILE);
    string line;
    int n = -1;                         // header
    while (getline(in, line)) if (!line.empty() && line != "\r") n++;
    return n;
}

static int pointsOf(int cardNo)
{
    Card c;
    return searchCard(cardNo, c) ? c.points : -1;
}

static PendingResult startTxn(PendingTransactionManager& m, int id, int cardNo,
                              double amount, const string& merchant,
                              const string& loc, const string& dev)
{
    vector<Transaction> h;
    loadTransactions(h);

    Card c;
    searchCard(cardNo, c);

    TransactionRequest r;
    r.id = id; r.cardNo = cardNo; r.amount = amount;
    r.merchant = merchant; r.deviceId = dev;
    getLocation(loc, r.location);

    return m.start(r, c, h, graph);
}

// New card with one saved low-risk transaction in Delhi.
static int newCardWithBaseline(PendingTransactionManager& m, int baselineId)
{
    int card = nextCard++;
    addCard(card, "Tester", "Credit", "Active", 0);

    TransactionDeque q;
    PendingResult b = startTxn(m, baselineId, card, 100, "AMAZON", "DELHI", "devA");
    m.commit(b.pendingId, q);
    return card;
}

// Starts a transaction on that card which needs OTP (Mumbai right after Delhi).
static PendingResult riskyStart(PendingTransactionManager& m, int id, int card)
{
    return startTxn(m, id, card, 1000, "FLIPKART", "MUMBAI", "devB");
}

// Make a CSV unwritable by putting a directory in its place.
static void breakFile(const string& f)
{
    if (fs::exists(f)) fs::rename(f, f + ".bak");
    fs::create_directory(f);
}

static void fixFile(const string& f)
{
    fs::remove_all(f);
    if (fs::exists(f + ".bak")) fs::rename(f + ".bak", f);
}

static int cardTotal(int cardNo)            // totalTransactions in card_history.csv
{
    vector<Transaction> h;
    loadTransactions(h);
    return getCardHistory(cardNo, h).totalTransactions;
}

static bool isHex32(const string& s)
{
    return s.size() == 32 &&
           all_of(s.begin(), s.end(), [](char c) { return isxdigit((unsigned char)c); });
}

int main()
{
    // ---- isolated working directory ----
    fs::path origin = fs::current_path();
    fs::path tmp = fs::temp_directory_path() / ("pending_test_" + to_string(rand()) + to_string(time(nullptr)));
    fs::create_directories(tmp);
    for (const char* f : {"cards.csv", "locations.csv", "transactions.csv",
                          "card_history.csv", "location_history.csv"})
        if (fs::exists(origin / f)) fs::copy_file(origin / f, tmp / f);
    fs::current_path(tmp);

    ostringstream sink;
    streambuf* real = cout.rdbuf(sink.rdbuf());
    ostream realOut(real);
    out = &realOut;

    loadCards();
    graph.buildFromFile();

    using chrono::steady_clock;
    using chrono::seconds;

    // ---------- start / ID / nothing saved yet ----------
    {
        PendingTransactionManager m;
        int card = newCardWithBaseline(m, 9001);
        int before = rows(), pts = pointsOf(card);

        PendingResult r = riskyStart(m, 9002, card);
        check("start: OTP-required transaction is pending", r.status == PendingStatus::Started && r.otpRequired && r.prelim.score >= OTP_THRESHOLD);
        check("start: pending ID is 32 hex chars", isHex32(r.pendingId));
        check("start: nothing saved yet (rows and card points unchanged)", rows() == before && pointsOf(card) == pts);
        check("start: one pending transaction held", m.pendingCount() == 1);
    }

    // ---------- OTP retries ----------
    {
        PendingTransactionManager m;
        TransactionDeque q;
        int card = newCardWithBaseline(m, 9010);
        PendingResult s = riskyStart(m, 9011, card);
        int before = rows();
        int observed = 0;
        auto obs = [&](const Transaction&, const FraudAnalysis&) { observed++; };

        PendingResult w1 = m.submitOtp(s.pendingId, "000000", q, obs);
        PendingResult w2 = m.submitOtp(s.pendingId, "111111", q, obs);
        check("retry: 1st wrong OTP -> 2 attempts left", w1.status == PendingStatus::WrongOtp && w1.failedAttempts == 1 && w1.attemptsRemaining == 2);
        check("retry: 2nd wrong OTP -> 1 attempt left, still unsaved", w2.status == PendingStatus::WrongOtp && w2.attemptsRemaining == 1 && rows() == before && observed == 0);

        PendingResult ok = m.submitOtp(s.pendingId, "123456", q, obs);
        check("retry: correct OTP on 3rd try commits", ok.status == PendingStatus::Committed);
        check("retry: saved with 2 failed attempts, VERIFIED, repeated-failure rule applied (+10)", ok.outcome.txn.failedAuthAttempts == 2 && ok.outcome.txn.otpStatus == "VERIFIED" && ok.outcome.txn.finalScore == s.prelim.score + POINTS_REPEATED_AUTH_FAIL);
        check("retry: exactly one row saved, observer called once", rows() == before + 1 && observed == 1);
    }

    // ---------- attempts exhausted ----------
    {
        PendingTransactionManager m;
        TransactionDeque q;
        int card = newCardWithBaseline(m, 9020);
        PendingResult s = riskyStart(m, 9021, card);
        int before = rows();

        PendingResult a = m.submitOtp(s.pendingId, "1", q);
        PendingResult b = m.submitOtp(s.pendingId, "2", q);
        PendingResult c = m.submitOtp(s.pendingId, "3", q);
        check("exhaust: 3 wrong OTPs -> 2 x WrongOtp then AttemptsExhausted",
              a.status == PendingStatus::WrongOtp && b.status == PendingStatus::WrongOtp && c.status == PendingStatus::AttemptsExhausted);
        check("exhaust: saved once as BLOCKED with OTP FAILED",
              rows() == before + 1 && c.outcome.txn.blocked && c.outcome.txn.otpStatus == "FAILED" && c.outcome.txn.failedAuthAttempts == 3);

        PendingResult d = m.submitOtp(s.pendingId, "123456", q);
        check("exhaust: no 4th attempt (even with the right OTP), no second save", d.status == PendingStatus::NotFound && rows() == before + 1);
        check("exhaust: card is unlocked afterwards", riskyStart(m, 9022, card).status == PendingStatus::Started);
    }

    // ---------- exactly-once commit ----------
    {
        PendingTransactionManager m;
        TransactionDeque q;
        int card = newCardWithBaseline(m, 9030);
        PendingResult s = riskyStart(m, 9031, card);
        int before = rows();

        PendingResult one = m.submitOtp(s.pendingId, "123456", q);
        PendingResult two = m.submitOtp(s.pendingId, "123456", q);
        check("once: replaying the OTP after commit is rejected, one row only",
              one.status == PendingStatus::Committed && two.status == PendingStatus::NotFound && rows() == before + 1);
    }
    {
        PendingTransactionManager m;
        TransactionDeque q;
        int card = newCardWithBaseline(m, 9040);
        PendingResult s = riskyStart(m, 9041, card);
        int before = rows();

        vector<PendingResult> res(8);
        vector<thread> ts;
        for (int i = 0; i < 8; i++)
            ts.emplace_back([&, i] { res[i] = m.submitOtp(s.pendingId, "123456", q); });
        for (auto& t : ts) t.join();

        int committed = 0;
        for (auto& r : res) if (r.status == PendingStatus::Committed) committed++;
        check("once: 8 concurrent submissions -> exactly 1 commit, 1 row", committed == 1 && rows() == before + 1);
    }

    // ---------- expiry ----------
    {
        auto fakeNow = steady_clock::now();
        PendingTransactionManager m([&] { return fakeNow; });
        TransactionDeque q;
        int card = newCardWithBaseline(m, 9050);
        PendingResult s = riskyStart(m, 9051, card);
        int before = rows(), pts = pointsOf(card);

        fakeNow += seconds(PENDING_TTL_SECONDS - 1);
        check("expiry: still usable at 4:59", m.submitOtp(s.pendingId, "000000", q).status == PendingStatus::WrongOtp);

        fakeNow += seconds(1);
        PendingResult e = m.submitOtp(s.pendingId, "123456", q);
        check("expiry: rejected at 5:00 even with the right OTP", e.status == PendingStatus::Expired);
        check("expiry: nothing saved", rows() == before && pointsOf(card) == pts && m.pendingCount() == 0);
        check("expiry: card unlocked afterwards", riskyStart(m, 9052, card).status == PendingStatus::Started);
    }
    {
        auto fakeNow = steady_clock::now();
        PendingTransactionManager m([&] { return fakeNow; });
        int card = newCardWithBaseline(m, 9060);
        riskyStart(m, 9061, card);
        fakeNow += seconds(PENDING_TTL_SECONDS + 1);
        check("expiry: stale pending does not lock the card or reserve its ID", riskyStart(m, 9061, card).status == PendingStatus::Started);
    }

    // ---------- duplicate transaction IDs ----------
    {
        PendingTransactionManager m;
        TransactionDeque q;
        int cardA = newCardWithBaseline(m, 9070);
        int cardB = nextCard++;
        addCard(cardB, "Tester", "Credit", "Active", 0);

        riskyStart(m, 9071, cardA);
        check("dup ID: same ID already pending on another card is rejected",
              startTxn(m, 9071, cardB, 100, "AMAZON", "DELHI", "devA").status == PendingStatus::DuplicateTransactionId);
        check("dup ID: ID already saved in transactions.csv is rejected",
              startTxn(m, 9070, cardB, 100, "AMAZON", "DELHI", "devA").status == PendingStatus::DuplicateTransactionId);
    }

    // ---------- card locking ----------
    {
        PendingTransactionManager m;
        TransactionDeque q;
        int cardA = newCardWithBaseline(m, 9080);
        int cardB = nextCard++;
        addCard(cardB, "Tester", "Credit", "Active", 0);

        PendingResult s = riskyStart(m, 9081, cardA);
        check("lock: 2nd pending transaction on same card is refused", riskyStart(m, 9082, cardA).status == PendingStatus::CardLocked);
        PendingResult other = startTxn(m, 9083, cardB, 100, "AMAZON", "DELHI", "devA");
        check("lock: other cards are not affected", other.status == PendingStatus::Started);

        m.cancel(s.pendingId);
        PendingResult again = riskyStart(m, 9084, cardA);
        check("lock: released by cancel", again.status == PendingStatus::Started);

        m.submitOtp(again.pendingId, "123456", q);
        check("lock: released by commit", riskyStart(m, 9085, cardA).status == PendingStatus::Started);
    }

    // ---------- cancellation ----------
    {
        PendingTransactionManager m;
        TransactionDeque q;
        int card = newCardWithBaseline(m, 9090);
        PendingResult s = riskyStart(m, 9091, card);
        int before = rows(), pts = pointsOf(card);

        check("cancel: pending transaction cancelled", m.cancel(s.pendingId).status == PendingStatus::Cancelled);
        check("cancel: nothing saved", rows() == before && pointsOf(card) == pts && m.pendingCount() == 0);
        check("cancel: OTP after cancel is rejected", m.submitOtp(s.pendingId, "123456", q).status == PendingStatus::NotFound && rows() == before);
        check("cancel: cancelling twice / unknown ID -> NotFound",
              m.cancel(s.pendingId).status == PendingStatus::NotFound && m.cancel("nope").status == PendingStatus::NotFound);
    }

    // ---------- unique IDs ----------
    {
        PendingTransactionManager m;
        int card = newCardWithBaseline(m, 9100);
        set<string> ids;
        bool allHex = true;
        for (int i = 0; i < 300; i++)
        {
            PendingResult s = riskyStart(m, 9101, card);
            ids.insert(s.pendingId);
            allHex = allHex && isHex32(s.pendingId);
            m.cancel(s.pendingId);
        }
        check("ids: 300 pending IDs all unique, all 32 hex chars", ids.size() == 300 && allHex);
    }

    // ---------- transactions that need no OTP ----------
    {
        PendingTransactionManager m;
        TransactionDeque q;
        int card = nextCard++;
        addCard(card, "Tester", "Credit", "Active", 0);
        int before = rows();

        PendingResult s = startTxn(m, 9110, card, 100, "AMAZON", "DELHI", "devA");
        check("no-OTP: start reports otpRequired=false", s.status == PendingStatus::Started && !s.otpRequired);
        check("no-OTP: submitOtp is refused", m.submitOtp(s.pendingId, "123456", q).status == PendingStatus::OtpNotRequired && rows() == before);
        check("no-OTP: commit saves once", m.commit(s.pendingId, q).status == PendingStatus::Committed && rows() == before + 1);
        check("no-OTP: second commit rejected", m.commit(s.pendingId, q).status == PendingStatus::NotFound && rows() == before + 1);
    }
    {
        PendingTransactionManager m;
        TransactionDeque q;
        int card = newCardWithBaseline(m, 9120);
        PendingResult s = riskyStart(m, 9121, card);
        int before = rows();
        check("commit() cannot bypass OTP", m.commit(s.pendingId, q).status == PendingStatus::OtpRequired && rows() == before && m.pendingCount() == 1);
    }

    // ---------- failed CSV writes: low-level honesty ----------
    {
        PendingTransactionManager m;
        int card = nextCard++;
        addCard(card, "Tester", "Credit", "Active", 0);

        breakFile("cards.csv");
        bool ok = updatePoints(card, 77);
        int p = pointsOf(card);
        fixFile("cards.csv");
        check("write failure: updatePoints reports it and rolls memory back", !ok && p == 0);

        breakFile("location_history.csv");
        bool l = saveLocation(card, "DELHI", 28.6, 77.2, 1);
        fixFile("location_history.csv");
        check("write failure: saveLocation reports it", !l);
    }

    // ---------- failed save of transactions.csv, then retry ----------
    {
        PendingTransactionManager m;
        TransactionDeque q;
        int card = newCardWithBaseline(m, 9200);
        PendingResult s = riskyStart(m, 9201, card);
        int before = rows(), pts = pointsOf(card), total = cardTotal(card);

        breakFile(TRANSACTION_FILE);
        PendingResult f = m.submitOtp(s.pendingId, "123456", q);
        fixFile(TRANSACTION_FILE);

        check("txn row fails: SaveFailed, not Committed", f.status == PendingStatus::SaveFailed && !f.outcome.transactionSaved && !f.outcome.fullySaved);
        check("txn row fails: card history and review deque are not touched", !f.outcome.cardHistorySaved && !f.outcome.enqueued && q.size() == 0 && cardTotal(card) == total);
        check("txn row fails: steps that worked are reported", f.outcome.pointsSaved && f.outcome.locationSaved);
        check("txn row fails: entry kept, card stays locked", m.pendingCount() == 1 && riskyStart(m, 9202, card).status == PendingStatus::CardLocked);
        check("txn row fails: OTP is not asked again", m.submitOtp(s.pendingId, "000000", q).status == PendingStatus::SaveFailed && m.submitOtp(s.pendingId, "123456", q).status == PendingStatus::SaveFailed);
        check("txn row fails: cancel refused after steps were saved", m.cancel(s.pendingId).status == PendingStatus::SaveFailed && m.pendingCount() == 1);

        PendingResult r = m.commit(s.pendingId, q);
        check("retry: commit() finishes the save", r.status == PendingStatus::Committed && r.outcome.fullySaved);
        check("retry: one row, points applied once, card history counted once",
              rows() == before + 1 && pointsOf(card) == pts + r.outcome.txn.finalScore && cardTotal(card) == total + 1);
        check("retry: done -> second commit is NotFound, no extra row", m.commit(s.pendingId, q).status == PendingStatus::NotFound && rows() == before + 1 && m.pendingCount() == 0);
    }

    // ---------- failed save of cards.csv (points) ----------
    {
        PendingTransactionManager m;
        TransactionDeque q;
        int card = newCardWithBaseline(m, 9210);
        PendingResult s = riskyStart(m, 9211, card);
        int before = rows(), pts = pointsOf(card);

        breakFile("cards.csv");
        PendingResult f = m.submitOtp(s.pendingId, "123456", q);
        fixFile("cards.csv");

        check("points fail: SaveFailed, points rolled back, other steps still saved",
              f.status == PendingStatus::SaveFailed && !f.outcome.pointsSaved && pointsOf(card) == pts && f.outcome.transactionSaved && f.outcome.cardHistorySaved);

        PendingResult r = m.commit(s.pendingId, q);
        loadCards();                                   // re-read cards.csv: proves it was persisted
        check("points fail: retry applies points once, no duplicate row or history",
              r.status == PendingStatus::Committed && pointsOf(card) == pts + r.outcome.txn.finalScore && rows() == before + 1);
    }

    // ---------- failed save of card_history.csv: no double counting ----------
    {
        PendingTransactionManager m;
        TransactionDeque q;
        int card = newCardWithBaseline(m, 9220);
        PendingResult s = riskyStart(m, 9221, card);
        int before = rows(), total = cardTotal(card);

        breakFile("card_history.csv");
        PendingResult f = m.submitOtp(s.pendingId, "123456", q);
        fixFile("card_history.csv");

        check("history fails: SaveFailed, history step reported failed", f.status == PendingStatus::SaveFailed && !f.outcome.cardHistorySaved && f.outcome.transactionSaved);

        m.commit(s.pendingId, q);                      // first retry
        check("history fails: retry counts the transaction exactly once", cardTotal(card) == total + 1 && rows() == before + 1);
    }

    // ---------- failed save of location_history.csv ----------
    {
        PendingTransactionManager m;
        TransactionDeque q;
        int card = newCardWithBaseline(m, 9230);
        PendingResult s = riskyStart(m, 9231, card);

        breakFile("location_history.csv");
        PendingResult f = m.submitOtp(s.pendingId, "123456", q);
        fixFile("location_history.csv");
        check("location fails: SaveFailed, location step reported failed", f.status == PendingStatus::SaveFailed && !f.outcome.locationSaved && f.outcome.transactionSaved);

        PendingResult r = m.commit(s.pendingId, q);
        PreviousLocation prev;
        check("location fails: retry saves the new last location", r.status == PendingStatus::Committed && getPreviousLocation(card, prev) && prev.location == "MUMBAI");
    }

    // ---------- failed save of review_queue.csv: no double enqueue ----------
    {
        PendingTransactionManager m;
        TransactionDeque q;
        int card = newCardWithBaseline(m, 9240);
        PendingResult s = riskyStart(m, 9241, card);

        breakFile("review_queue.csv");
        m.submitOtp(s.pendingId, "1", q);
        m.submitOtp(s.pendingId, "2", q);
        PendingResult f = m.submitOtp(s.pendingId, "3", q);       // blocked -> queued
        fixFile("review_queue.csv");
        check("queue fails: SaveFailed, queued in memory but file not written", f.status == PendingStatus::SaveFailed && f.outcome.enqueued && !f.outcome.queueSaved && q.size() == 1);

        PendingResult r = m.commit(s.pendingId, q);
        TransactionDeque loaded;
        loaded.load();
        check("queue fails: retry keeps the original outcome and saves the file", r.status == PendingStatus::AttemptsExhausted && r.outcome.fullySaved && r.outcome.txn.blocked);
        check("queue fails: retry does not enqueue twice", q.size() == 1 && loaded.size() == 1);
    }

    // ---------- row landed although the save reported failure ----------
    {
        PendingTransactionManager m;
        TransactionDeque q;
        int card = newCardWithBaseline(m, 9250);
        PendingResult s = riskyStart(m, 9251, card);
        int before = rows();

        breakFile(TRANSACTION_FILE);
        PendingResult f = m.submitOtp(s.pendingId, "123456", q);
        fixFile(TRANSACTION_FILE);
        appendTransaction(f.outcome.txn);              // pretend the first attempt did land

        PendingResult r = m.commit(s.pendingId, q);
        check("duplicate guard: retry does not append the row a second time", r.status == PendingStatus::Committed && rows() == before + 1);
    }

    // ---------- expiry window after a failed save ----------
    {
        auto fakeNow = steady_clock::now();
        PendingTransactionManager m([&] { return fakeNow; });
        TransactionDeque q;
        int card = newCardWithBaseline(m, 9260);
        PendingResult s = riskyStart(m, 9261, card);

        fakeNow += seconds(200);
        breakFile(TRANSACTION_FILE);
        PendingResult f = m.submitOtp(s.pendingId, "123456", q);
        fixFile(TRANSACTION_FILE);

        fakeNow += seconds(250);                        // past the original 5 minutes
        check("failed save: expiry restarts, retry still possible after original TTL",
              f.status == PendingStatus::SaveFailed && m.commit(s.pendingId, q).status == PendingStatus::Committed);
    }
    {
        auto fakeNow = steady_clock::now();
        PendingTransactionManager m([&] { return fakeNow; });
        TransactionDeque q;
        int card = newCardWithBaseline(m, 9270);
        PendingResult s = riskyStart(m, 9271, card);

        breakFile(TRANSACTION_FILE);
        m.submitOtp(s.pendingId, "123456", q);
        fixFile(TRANSACTION_FILE);

        fakeNow += seconds(PENDING_TTL_SECONDS + 1);
        check("failed save: an entry nobody retries still expires and frees the card",
              m.pendingCount() == 0 && riskyStart(m, 9272, card).status == PendingStatus::Started);
    }

    // ---------- cleanup ----------
    cout.rdbuf(real);
    fs::current_path(origin);
    fs::remove_all(tmp);

    cout << "\n" << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}

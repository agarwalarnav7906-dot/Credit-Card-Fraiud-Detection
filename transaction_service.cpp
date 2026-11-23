#include <ctime>
#include <random>

#include "transaction_service.h"
#include "location_history.h"
#include "time_module.h"
#include "transaction_history.h"

using namespace std;

// ==========================================================
// STEP 1: BEGIN (preliminary analysis, nothing is saved)
// ==========================================================

PendingTransaction beginTransaction(const TransactionRequest& request,
                                    const Card& card,
                                    const vector<Transaction>& history,
                                    const LocationGraph& graph)
{
    PendingTransaction pending;

    pending.card = card;
    pending.location = request.location;
    pending.history = history;


    // ==========================================
    // CURRENT TIMESTAMP (epoch derived from the same string we store)
    // ==========================================

    string currentTimestamp = getCurrentTimestamp();

    long long currentTimestampValue = 0;

    if (!parseTimestamp(currentTimestamp, currentTimestampValue))
    {
        currentTimestampValue = static_cast<long long>(time(nullptr));
    }

    pending.timestampValue = currentTimestampValue;


    // ==========================================
    // READ CARD HISTORY FROM card_history.csv
    // (rebuilt from transactions.csv if the card has no row yet)
    // ==========================================

    pending.cardHistory = getCardHistory(request.cardNo, pending.history);

    CardHistory& cardHistory = pending.cardHistory;

    // One-time migration: cards that only exist in the old
    // location_history.csv keep their last known location.
    if (!cardHistory.hasLastLocation)
    {
        PreviousLocation legacy;

        if (getPreviousLocation(request.cardNo, legacy))
        {
            cardHistory.hasLastLocation = true;
            cardHistory.lastLocation = legacy.location;
            cardHistory.lastLatitude = legacy.latitude;
            cardHistory.lastLongitude = legacy.longitude;
            cardHistory.lastTimestamp = legacy.timestamp;
        }
    }

    double distance = 0.0;

    if (cardHistory.hasLastLocation)
    {
        distance = calculateDistance(
            cardHistory.lastLatitude,
            cardHistory.lastLongitude,
            request.location.latitude,
            request.location.longitude
        );

        pending.hasPreviousLocation = true;
        pending.previousLocation = cardHistory.lastLocation;

        pending.route = graph.shortestRoute(
            cardHistory.lastLocation, request.location.name);
    }


    // ==========================================
    // CREATE TRANSACTION
    // ==========================================

    Transaction& txn = pending.txn;

    txn.id = request.id;
    txn.cardNo = request.cardNo;
    txn.amount = request.amount;
    txn.merchant = request.merchant;
    txn.location = request.location.name;
    txn.latitude = request.location.latitude;
    txn.longitude = request.location.longitude;
    txn.timestamp = currentTimestamp;
    txn.deviceId = request.deviceId;
    txn.distanceFromPrevious = distance;


    // ==========================================
    // PRELIMINARY FRAUD ANALYSIS
    // (history -> average -> frequency -> travel -> device -> merchant)
    // ==========================================

    pending.prelim =
        calculateFraudScore(
            txn,
            pending.history,
            cardHistory,
            0,
            false
        );

    txn.prelimScore = pending.prelim.score;


    // ==========================================
    // OTP / STEP-UP AUTHENTICATION NEEDED?
    // ==========================================

    if (otpRequiredFor(pending.prelim.score))
    {
        txn.otpRequired = true;
        txn.authenticationRequired = true;
        txn.authenticationMethod = "OTP";

        pending.otpRequired = true;
    }

    return pending;
}


// ==========================================================
// STEP 2: OTP VERIFICATION
// ==========================================================

bool verifyOtp(const string& submittedCode)
{
    /*
        Demo OTP: 123456
        Can be replaced with real OTP generation later.
    */

    return submittedCode == "123456";
}


// ==========================================================
// STEP 3: COMPLETE (final score, decision, persistence)
// ==========================================================

// A failed attempt may have written the row before reporting failure.
static bool transactionStored(int id)
{
    vector<Transaction> stored;
    loadTransactions(stored);
    return transactionIdExists(stored, id);
}

TransactionOutcome completeTransaction(PendingTransaction& pending,
                                       const OtpOutcome& otp,
                                       TransactionDeque& reviewQueue,
                                       const DecisionObserver& onDecided)
{
    CommitState& c = pending.commit;
    TransactionOutcome& outcome = c.outcome;

    Transaction& txn = pending.txn;
    const int cardID = txn.cardNo;


    // ==========================================
    // DECISION (made once; a retry keeps it)
    // ==========================================

    if (!c.decided)
    {
        FraudAnalysis finalAnalysis = pending.prelim;

        // OTP outcome
        if (pending.otpRequired)
        {
            txn.failedAuthAttempts = otp.failedAttempts;
            txn.otpVerified = otp.verified;

            if (otp.verified)
            {
                txn.otpStatus = "VERIFIED";
                txn.authStatus = "SUCCESSFUL";
            }
            else
            {
                txn.otpStatus = "FAILED";
                txn.authStatus = "FAILED";
            }

            // Recalculate from scratch with the authentication outcome
            finalAnalysis =
                calculateFraudScore(
                    txn,
                    pending.history,
                    pending.cardHistory,
                    otp.failedAttempts,
                    !otp.verified
                );
        }

        txn.finalScore = finalAnalysis.score;
        txn.score = finalAnalysis.score;
        txn.fraudStatus = finalAnalysis.status;

        txn.blocked = shouldBlock(finalAnalysis.score);
        txn.flagged = txn.blocked;
        txn.decision = txn.blocked ? "BLOCKED" : "ALLOWED";

        if (txn.blocked)
        {
            txn.flagTimestamp = txn.timestamp;
        }

        for (size_t i = 0; i < finalAnalysis.reasons.size(); i++)
        {
            if (i > 0) txn.reasons += "; ";
            txn.reasons += finalAnalysis.reasons[i];
        }

        if (txn.reasons.empty())
            txn.reasons = "No risk factors";

        outcome.finalAnalysis = finalAnalysis;
        outcome.oldPoints = pending.card.points;
        outcome.newPoints = outcome.oldPoints + txn.finalScore;

        c.decided = true;

        if (onDecided)
            onDecided(txn, finalAnalysis);
    }

    const bool isRetry = c.attempts > 0;
    c.attempts++;


    // ==========================================
    // UPDATE CARD POINTS (set, not add: safe to repeat)
    // ==========================================

    if (!c.pointsDone)
        c.pointsDone = updatePoints(cardID, outcome.newPoints);


    // ==========================================
    // SAVE CURRENT LOCATION
    // (blocked transactions do not move the card's "last known" location)
    // ==========================================

    if (!txn.blocked && !c.locationDone)
    {
        c.locationDone = saveLocation(
            cardID,
            pending.location.name,
            pending.location.latitude,
            pending.location.longitude,
            pending.timestampValue
        );
    }


    // ==========================================
    // PERSIST TRANSACTION (transactions.csv)
    // ==========================================

    if (!c.transactionDone)
    {
        if (isRetry && transactionStored(txn.id))
            c.transactionDone = true;
        else
            c.transactionDone = appendTransaction(txn);
    }


    // Card history and review deque describe a saved transaction, so they
    // wait for its row.
    if (c.transactionDone)
    {
        // ==========================================
        // UPDATE card_history.csv (aggregated card data)
        // updateCardHistory() changes the aggregate before writing, so work on
        // a copy: a failed write must not leave the transaction counted.
        // ==========================================

        if (!c.historyDone)
        {
            CardHistory updated = pending.cardHistory;

            if (updateCardHistory(updated, txn))
            {
                pending.cardHistory = updated;
                c.historyDone = true;
            }
        }


        // ==========================================
        // FRAUD REVIEW DEQUE
        // (critical -> front, high risk / suspicious -> rear)
        // Enqueued once; only the file write is retried.
        // ==========================================

        if (!c.queueEvaluated)
        {
            outcome.enqueued =
                reviewQueue.enqueueForReview(txn, &outcome.evicted, &outcome.dropped);

            if (outcome.enqueued)
                outcome.queuedAtFront = txn.finalScore >= CRITICAL_THRESHOLD;

            c.queueSaved = !outcome.enqueued;
            c.queueEvaluated = true;
        }

        if (outcome.enqueued && !c.queueSaved)
            c.queueSaved = reviewQueue.save();
    }


    outcome.txn = txn;
    outcome.pointsSaved = c.pointsDone;
    outcome.locationSaved = c.locationDone;
    outcome.transactionSaved = c.transactionDone;
    outcome.cardHistorySaved = c.historyDone;
    outcome.queueSaved = c.queueEvaluated && c.queueSaved;

    outcome.fullySaved = c.pointsDone &&
                         (txn.blocked || c.locationDone) &&
                         c.transactionDone &&
                         c.historyDone &&
                         outcome.queueSaved;

    return outcome;
}


// ==========================================================
// PENDING-TRANSACTION MANAGER
// ==========================================================


PendingTransactionManager::PendingTransactionManager(Clock clock)
    : now(std::move(clock))
{
}

std::chrono::steady_clock::time_point PendingTransactionManager::current() const
{
    return now ? now() : std::chrono::steady_clock::now();
}

// Whole seconds until the entry expires (rounded up, never negative).
int PendingTransactionManager::secondsLeft(const Entry& e) const
{
    auto ms = chrono::duration_cast<chrono::milliseconds>(e.expiresAt - current()).count();
    if (ms <= 0) return 0;
    return static_cast<int>((ms + 999) / 1000);
}

void PendingTransactionManager::purgeExpiredLocked()
{
    const auto t = current();

    for (auto it = entries.begin(); it != entries.end(); )
    {
        if (t >= it->second.expiresAt) it = entries.erase(it);
        else ++it;
    }
}

// 128 random bits from the OS entropy source, as 32 hex digits.
string PendingTransactionManager::newIdLocked() const
{
    static const char* hex = "0123456789abcdef";
    random_device rd;

    while (true)
    {
        string id;
        for (int i = 0; i < 4; i++)
        {
            unsigned int v = rd();
            for (int b = 0; b < 8; b++)
            {
                id += hex[v & 0xF];
                v >>= 4;
            }
        }

        if (entries.find(id) == entries.end()) return id;
    }
}

PendingResult PendingTransactionManager::start(const TransactionRequest& request,
                                               const Card& card,
                                               const vector<Transaction>& history,
                                               const LocationGraph& graph)
{
    lock_guard<mutex> lock(mtx);
    purgeExpiredLocked();

    PendingResult r;

    if (transactionIdExists(history, request.id))
    {
        r.status = PendingStatus::DuplicateTransactionId;
        return r;
    }

    for (const auto& e : entries)
    {
        if (e.second.pending.txn.id == request.id)
        {
            r.status = PendingStatus::DuplicateTransactionId;
            return r;
        }

        if (e.second.pending.txn.cardNo == request.cardNo)
        {
            r.status = PendingStatus::CardLocked;
            return r;
        }
    }

    Entry entry;
    entry.pending = beginTransaction(request, card, history, graph);
    entry.expiresAt = current() + chrono::seconds(PENDING_TTL_SECONDS);

    r.status = PendingStatus::Started;
    r.pendingId = newIdLocked();
    r.otpRequired = entry.pending.otpRequired;
    // FIX: was left at 0, so a client showed "0 attempts left" before the
    // first OTP was even entered.
    r.attemptsRemaining = entry.pending.otpRequired ? MAX_OTP_ATTEMPTS : 0;
    r.expiresInSeconds = secondsLeft(entry);
    r.txn = entry.pending.txn;
    r.prelim = entry.pending.prelim;

    entries.emplace(r.pendingId, std::move(entry));
    return r;
}

// Runs step 3 on a kept entry. Erased only when everything is saved; after a
// failed save it stays (card locked) with a fresh expiry for the retry.
PendingResult PendingTransactionManager::finishLocked(
    map<string, Entry>::iterator it,
    TransactionDeque& reviewQueue,
    const DecisionObserver& onDecided,
    PendingResult r)
{
    Entry& e = it->second;

    r.failedAttempts = e.otp.failedAttempts;
    r.outcome = completeTransaction(e.pending, e.otp, reviewQueue, onDecided);
    r.txn = r.outcome.txn;

    if (r.outcome.fullySaved)
    {
        r.status = e.doneStatus;
        entries.erase(it);
    }
    else
    {
        r.status = PendingStatus::SaveFailed;
        e.expiresAt = current() + chrono::seconds(PENDING_TTL_SECONDS);
        r.expiresInSeconds = secondsLeft(e);
    }

    return r;
}

PendingResult PendingTransactionManager::submitOtp(const string& pendingId,
                                                   const string& code,
                                                   TransactionDeque& reviewQueue,
                                                   const DecisionObserver& onDecided)
{
    lock_guard<mutex> lock(mtx);

    PendingResult r;
    r.pendingId = pendingId;

    auto it = entries.find(pendingId);
    if (it == entries.end()) return r;                 // NotFound

    if (current() >= it->second.expiresAt)
    {
        entries.erase(it);
        r.status = PendingStatus::Expired;
        return r;
    }

    Entry& e = it->second;
    r.otpRequired = e.pending.otpRequired;

    if (e.otpDone)
    {
        // OTP already finished; only a save is outstanding. No new OTP check.
        r.status = PendingStatus::SaveFailed;
        r.failedAttempts = e.otp.failedAttempts;
        r.outcome = e.pending.commit.outcome;
        r.txn = r.outcome.txn;
        r.expiresInSeconds = secondsLeft(e);
        return r;
    }

    if (!r.otpRequired)
    {
        r.status = PendingStatus::OtpNotRequired;
        r.expiresInSeconds = secondsLeft(e);
        return r;
    }

    bool verified = verifyOtp(code);

    if (!verified) e.failedAttempts++;

    r.failedAttempts = e.failedAttempts;

    if (!verified && r.failedAttempts < MAX_OTP_ATTEMPTS)
    {
        r.status = PendingStatus::WrongOtp;
        r.attemptsRemaining = MAX_OTP_ATTEMPTS - r.failedAttempts;
        r.expiresInSeconds = secondsLeft(e);
        return r;
    }

    // OTP finished (verified, or attempts used up).
    e.otpDone = true;
    e.otp.failedAttempts = e.failedAttempts;
    e.otp.verified = verified;
    e.doneStatus = verified ? PendingStatus::Committed
                            : PendingStatus::AttemptsExhausted;

    return finishLocked(it, reviewQueue, onDecided, r);
}

PendingResult PendingTransactionManager::commit(const string& pendingId,
                                                TransactionDeque& reviewQueue,
                                                const DecisionObserver& onDecided)
{
    lock_guard<mutex> lock(mtx);

    PendingResult r;
    r.pendingId = pendingId;

    auto it = entries.find(pendingId);
    if (it == entries.end()) return r;                 // NotFound

    if (current() >= it->second.expiresAt)
    {
        entries.erase(it);
        r.status = PendingStatus::Expired;
        return r;
    }

    Entry& e = it->second;
    r.otpRequired = e.pending.otpRequired;

    if (!e.otpDone)
    {
        if (r.otpRequired)
        {
            r.status = PendingStatus::OtpRequired;
            r.expiresInSeconds = secondsLeft(e);
            return r;
        }

        e.otpDone = true;                              // no OTP needed
        e.otp = OtpOutcome();
        e.doneStatus = PendingStatus::Committed;
    }

    return finishLocked(it, reviewQueue, onDecided, r);
}

PendingResult PendingTransactionManager::cancel(const string& pendingId)
{
    lock_guard<mutex> lock(mtx);

    PendingResult r;
    r.pendingId = pendingId;

    auto it = entries.find(pendingId);
    if (it == entries.end()) return r;                 // NotFound

    if (current() >= it->second.expiresAt)
    {
        entries.erase(it);
        r.status = PendingStatus::Expired;
        return r;
    }

    // Saved steps cannot be rolled back, so a half-saved transaction must be
    // finished with commit(), not cancelled.
    if (it->second.pending.commit.anyStepDone())
    {
        r.status = PendingStatus::SaveFailed;
        r.expiresInSeconds = secondsLeft(it->second);
        return r;
    }

    entries.erase(it);
    r.status = PendingStatus::Cancelled;
    return r;
}

void PendingTransactionManager::purgeExpired()
{
    lock_guard<mutex> lock(mtx);
    purgeExpiredLocked();
}

size_t PendingTransactionManager::pendingCount()
{
    lock_guard<mutex> lock(mtx);
    purgeExpiredLocked();
    return entries.size();
}

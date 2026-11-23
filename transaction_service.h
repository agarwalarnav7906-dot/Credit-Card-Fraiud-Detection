#ifndef TRANSACTION_SERVICE_H
#define TRANSACTION_SERVICE_H

// ---------------------------------------------------------------
// TRANSACTION SERVICE
// Transaction-processing logic with no console input or output, so it
// can be driven by the console menu today and by another front end
// (e.g. HTTP handlers) later.
//
// The flow is split in three independent steps:
//
//   1. beginTransaction()    -> preliminary analysis, nothing is saved
//   2. verifyOtp()           -> OTP check, only when pending.otpRequired
//   3. completeTransaction() -> final score, decision, persistence
//
// A PendingTransaction owns everything it needs (card, history snapshot,
// location, card history), so it can be kept between steps 1 and 3.
// ---------------------------------------------------------------

#include <chrono>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <vector>

#include "transaction.h"
#include "hashmodule.h"
#include "latlong.h"
#include "card_history.h"
#include "fraudmodule.h"
#include "dequemodule.h"
#include "locationgraph.h"

// Already-validated input for one transaction. The caller has checked the
// amount, looked the card up, resolved the location and upper-cased the
// merchant (all of which the console flow does while prompting).
struct TransactionRequest
{
    int         id = 0;
    int         cardNo = 0;
    double      amount = 0.0;
    std::string merchant;
    Location    location;
    std::string deviceId;
};

// Outcome of the OTP step, supplied to completeTransaction().
struct OtpOutcome
{
    int  failedAttempts = 0;
    bool verified = false;
};

// Everything the caller needs to report what happened in step 3.
struct TransactionOutcome
{
    Transaction   txn;                       // final state, as saved
    FraudAnalysis finalAnalysis;

    int  oldPoints = 0;
    int  newPoints = 0;

    bool pointsSaved = false;                // cards.csv updated
    bool locationSaved = false;              // location_history.csv updated (never for BLOCKED)
    bool transactionSaved = false;           // transactions.csv appended
    bool cardHistorySaved = false;           // card_history.csv updated
    bool queueSaved = false;                 // review_queue.csv written (true if nothing to queue)
    bool fullySaved = false;                 // every required step above succeeded

    bool      enqueued = false;              // added to the fraud review deque
    bool      queuedAtFront = false;         // FRONT (critical) or REAR
    bool      evicted = false;               // deque was full
    QueueItem dropped;                       // item dropped from the rear
};

// Progress of completeTransaction(), kept inside the PendingTransaction so a
// call that fails part-way can be repeated: finished steps are never redone
// (no double points, duplicate row or double-counted card history) and the
// decision is not recalculated.
struct CommitState
{
    bool decided = false;
    bool pointsDone = false;
    bool locationDone = false;
    bool transactionDone = false;
    bool historyDone = false;
    bool queueEvaluated = false;
    bool queueSaved = false;
    int  attempts = 0;
    TransactionOutcome outcome;              // cumulative result

    bool anyStepDone() const
    {
        return pointsDone || locationDone || transactionDone ||
               historyDone || queueEvaluated;
    }
};

// Result of step 1. No file has been written when this is returned.
struct PendingTransaction
{
    Transaction              txn;            // preliminary state
    Card                     card;           // card as read before this transaction
    Location                 location;
    CardHistory              cardHistory;    // aggregated card data (card_history.csv)
    std::vector<Transaction> history;        // snapshot of transactions.csv
    long long                timestampValue = 0;   // epoch seconds of txn.timestamp

    // Location analysis (for display by the caller)
    bool        hasPreviousLocation = false;
    std::string previousLocation;
    RouteResult route;                       // route.found == false if none

    FraudAnalysis prelim;                    // preliminary fraud analysis
    bool          otpRequired = false;       // step-up authentication needed

    CommitState   commit;                    // progress of step 3
};

// Called by completeTransaction() once the final score and decision are known
// and before anything is persisted, so a front end can report them in order.
using DecisionObserver =
    std::function<void(const Transaction&, const FraudAnalysis&)>;

// Step 1: builds the transaction and runs the preliminary fraud analysis.
// Saves nothing.
PendingTransaction beginTransaction(const TransactionRequest& request,
                                    const Card& card,
                                    const std::vector<Transaction>& history,
                                    const LocationGraph& graph);

// Step 2: checks one submitted OTP. Attempt counting (MAX_OTP_ATTEMPTS) is
// left to the caller, which knows how attempts arrive.
bool verifyOtp(const std::string& submittedCode);

// Step 3: applies the OTP outcome (ignored unless pending.otpRequired),
// rescores, decides ALLOWED/BLOCKED, then saves in this order: card points,
// last location (not if blocked), transactions.csv, card_history.csv,
// review deque. Card history and the deque are only touched once the
// transaction row is saved.
//
// If a save fails, outcome.fullySaved is false and the flags show which steps
// worked. Call again with the same PendingTransaction to retry: only the
// missing steps run, the decision is kept and onDecided is not called again.
TransactionOutcome completeTransaction(PendingTransaction& pending,
                                       const OtpOutcome& otp,
                                       TransactionDeque& reviewQueue,
                                       const DecisionObserver& onDecided = nullptr);

// ---------------------------------------------------------------
// PENDING-TRANSACTION MANAGER (in memory)
//
// Holds transactions between beginTransaction() and completeTransaction()
// for front ends that cannot block on a prompt (e.g. HTTP).
//  - pending IDs are random 128-bit hex strings
//  - OTP attempts are counted here (MAX_OTP_ATTEMPTS); the last wrong
//    attempt commits the transaction as failed, exactly like the console
//  - a pending transaction expires PENDING_TTL_SECONDS after start()
//  - one pending transaction per card; transaction IDs cannot repeat
//  - each pending transaction is committed at most once
//  - cancelled / expired transactions save nothing
//  - if saving fails the entry is kept (card stays locked, OTP is not asked
//    again, expiry restarts) and commit() retries only the missing steps;
//    cancel() is refused once any step was saved, and an entry nobody retries
//    still expires (steps already saved are not rolled back)
// All methods are thread-safe. The DecisionObserver runs under the lock.
// ---------------------------------------------------------------

const int PENDING_TTL_SECONDS = 5 * 60;

enum class PendingStatus
{
    Started,                 // start(): pending created
    DuplicateTransactionId,  // start(): ID already pending or in history
    CardLocked,              // start(): card already has a pending transaction
    NotFound,                // unknown, cancelled, committed or purged ID
    Expired,                 // timed out (nothing saved)
    OtpNotRequired,          // submitOtp() on a transaction without OTP
    OtpRequired,             // commit() on a transaction that needs OTP
    WrongOtp,                // wrong code, attempts remain
    Committed,               // saved (OTP verified, or none needed)
    AttemptsExhausted,       // last wrong code: saved as failed OTP
    Cancelled,               // cancel(): discarded (nothing saved)
    SaveFailed               // OTP step done but a save failed: entry kept,
                             // call commit() to retry (see outcome flags)
};

struct PendingResult
{
    PendingStatus status = PendingStatus::NotFound;
    std::string   pendingId;
    bool          otpRequired = false;
    int           failedAttempts = 0;
    int           attemptsRemaining = 0;
    // FIX (integration): whole seconds left on the pending entry, or -1 when
    // there is no live entry (Committed, Cancelled, Expired, NotFound ...).
    // The web UI needs it for the OTP / retry countdown.
    int           expiresInSeconds = -1;
    Transaction   txn;                 // preliminary (Started) - see outcome when saved
    FraudAnalysis prelim;              // Started only
    TransactionOutcome outcome;        // Committed / AttemptsExhausted only
};

class PendingTransactionManager
{
public:
    using Clock = std::function<std::chrono::steady_clock::time_point()>;

    // clock is injectable for tests; default is steady_clock::now
    explicit PendingTransactionManager(Clock clock = nullptr);

    PendingResult start(const TransactionRequest& request,
                        const Card& card,
                        const std::vector<Transaction>& history,
                        const LocationGraph& graph);

    PendingResult submitOtp(const std::string& pendingId,
                            const std::string& code,
                            TransactionDeque& reviewQueue,
                            const DecisionObserver& onDecided = nullptr);

    // Saves a transaction that needs no OTP, or retries a save that failed
    // after the OTP step finished.
    PendingResult commit(const std::string& pendingId,
                         TransactionDeque& reviewQueue,
                         const DecisionObserver& onDecided = nullptr);

    PendingResult cancel(const std::string& pendingId);

    void purgeExpired();
    size_t pendingCount();

private:
    struct Entry
    {
        PendingTransaction pending;
        int  failedAttempts = 0;
        bool otpDone = false;                // OTP step finished (or not needed)
        OtpOutcome  otp;
        PendingStatus doneStatus = PendingStatus::Committed;
        std::chrono::steady_clock::time_point expiresAt;
    };

    std::chrono::steady_clock::time_point current() const;
    int secondsLeft(const Entry& e) const;
    void purgeExpiredLocked();
    std::string newIdLocked() const;
    PendingResult finishLocked(std::map<std::string, Entry>::iterator it,
                               TransactionDeque& reviewQueue,
                               const DecisionObserver& onDecided,
                               PendingResult r);

    std::map<std::string, Entry> entries;
    std::mutex mtx;
    Clock now;
};

#endif

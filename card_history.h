#ifndef CARD_HISTORY_H
#define CARD_HISTORY_H

#include <string>
#include <vector>
#include "transaction.h"

// ---------------------------------------------------------------
// card_history.csv : ONE aggregated row per card.
// transactions.csv  : one detailed row per transaction.
// The fraud engine reads both; every processed transaction updates both.
// ---------------------------------------------------------------

const std::string CARD_HISTORY_FILE = "card_history.csv";

struct CardHistory
{
    int cardNo = 0;
    bool found = false;                 // a record exists for this card

    int totalTransactions = 0;          // all attempts (allowed + blocked)
    int completedTransactions = 0;      // allowed only
    int blockedTransactions = 0;

    double totalCompletedAmount = 0.0;  // sum of allowed amounts

    std::vector<std::string> merchants; // merchants used by completed txns
    std::vector<std::string> devices;   // devices used by completed txns

    bool hasLastLocation = false;       // last location of a completed txn
    std::string lastLocation;
    double lastLatitude = 0.0;
    double lastLongitude = 0.0;
    long long lastTimestamp = 0;        // epoch seconds

    int otpRequiredCount = 0;           // times step-up auth was needed
    int failedOtpAttempts = 0;          // total wrong OTPs entered, ever
    int lastFinalScore = 0;
    std::string lastUpdated;
};

// average = totalCompletedAmount / completedTransactions (0 if none)
double averageAmount(const CardHistory& c);

bool containsIgnoreCase(const std::vector<std::string>& list,
                        const std::string& value);

// Reads card_history.csv. If the card has no row yet but old transactions
// for it exist in transactions.csv, the row is rebuilt from them (so
// older data is never lost). Otherwise returns an empty, found=false record.
CardHistory getCardHistory(int cardNo,
                           const std::vector<Transaction>& transactions,
                           const std::string& file = CARD_HISTORY_FILE);

// Adds one finished transaction to the aggregate (pure in-memory update).
void applyTransactionToCard(CardHistory& card, const Transaction& t);

// applyTransactionToCard + rewrite that card's row in card_history.csv.
bool updateCardHistory(CardHistory& card, const Transaction& t,
                       const std::string& file = CARD_HISTORY_FILE);

bool loadAllCardHistory(std::vector<CardHistory>& all,
                        const std::string& file = CARD_HISTORY_FILE);
bool saveAllCardHistory(const std::vector<CardHistory>& all,
                        const std::string& file = CARD_HISTORY_FILE);

void displayCardHistory(const CardHistory& c);

#endif

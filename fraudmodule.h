#ifndef FRAUDMODULE_H
#define FRAUDMODULE_H

#include <string>
#include <vector>

#include "transaction.h"
#include "card_history.h"

using namespace std;

// ---------------------------------------------------------------
// RISK POINTS (single source of truth)
// ---------------------------------------------------------------
const int POINTS_IMPOSSIBLE_TRAVEL   = 30;
const int POINTS_SPENDING_ANOMALY    = 15;
const int POINTS_RAPID_FREQUENCY     = 10;
const int POINTS_NEW_DEVICE          = 10;
const int POINTS_REPEATED_AUTH_FAIL  = 10;
const int POINTS_NEW_MERCHANT        = 5;
const int POINTS_WRONG_OTP           = 50;

// ---------------------------------------------------------------
// THRESHOLDS
// ---------------------------------------------------------------
const int    MAX_SCORE               = 100;
const int    SUSPICIOUS_THRESHOLD    = 30;   // 30-49
const int    OTP_THRESHOLD           = 50;   // 50-69 HIGH RISK -> OTP
const int    CRITICAL_THRESHOLD      = 70;   // 70-100 CRITICAL -> BLOCK

const double SPENDING_MULTIPLIER     = 2.0;  // amount >= 2 x average
const int    FREQUENCY_WINDOW_SECS   = 10 * 60;
const int    FREQUENCY_LIMIT         = 5;    // 5 or more transactions

const double TRAVEL_MIN_DISTANCE_KM  = 100.0; // ignore short hops
const double TRAVEL_MAX_SPEED_KMH    = 900.0; // faster than a flight

const int    REPEATED_FAIL_LIMIT     = 2;    // 2 or more failed OTPs
const int    MAX_OTP_ATTEMPTS        = 3;

// ---------------------------------------------------------------
// ANALYSIS RESULT - every number needed to explain the score
// ---------------------------------------------------------------
struct FraudAnalysis
{
    // Spending anomaly
    bool   hasHistoricalAverage = false;
    int    previousCompletedCount = 0;
    double historicalAverage = 0.0;
    bool   spendingAnomaly = false;

    // Rapid frequency
    int    recentTransactionCount = 0;   // previous txns in last 10 minutes
    bool   rapidFrequency = false;

    // Impossible travel
    bool   hasPreviousLocation = false;
    double distanceKm = 0.0;
    double hoursElapsed = 0.0;
    double speedKmh = 0.0;
    bool   impossibleTravel = false;

    // Device / merchant (only meaningful if the card has completed history)
    bool   hasDeviceHistory = false;
    bool   newDevice = false;
    bool   hasMerchantHistory = false;
    bool   newMerchant = false;

    // Card history source (card_history.csv)
    bool   hasCardHistory = false;
    int    cardTotalTransactions = 0;
    int    cardBlockedTransactions = 0;
    int    cardPastFailedOtps = 0;       // authentication history (display)
    int    cardPastOtpRequests = 0;

    // Authentication
    int    failedAuthAttempts = 0;
    bool   repeatedAuthFailure = false;
    bool   wrongOtp = false;

    // Result
    int    score = 0;                    // capped 0-100
    int    rawScore = 0;                 // before cap
    string status;
    vector<string> reasons;
};

// THE one authoritative place where the fraud score is calculated.
// Data sources (both read from CSV by the caller before this is called):
//   card  = card_history.csv  -> average, merchants, devices, last location
//   history = transactions.csv -> timestamps for the 10-minute frequency rule
// The score is always rebuilt from scratch, so nothing is counted twice.
//   prelim call : failedAuthAttempts = 0, wrongOtp = false
//   final call  : failedAuthAttempts = n, wrongOtp = (OTP never verified)
FraudAnalysis calculateFraudScore(
    const Transaction& current,
    const vector<Transaction>& history,
    const CardHistory& card,
    int failedAuthAttempts,
    bool wrongOtp
);

string classifyRisk(int score);              // NORMAL / SUSPICIOUS / HIGH RISK / CRITICAL FRAUD
int    capScore(int score);                  // clamp to 0..MAX_SCORE
bool   otpRequiredFor(int score);            // score >= 50
bool   shouldBlock(int score);               // score >= 70

bool   sameTextIgnoreCase(const string& a, const string& b);

// Pretty printers for the demo
void printFraudAnalysis(const FraudAnalysis& a, const Transaction& current);
void printOtpPenaltySummary(const FraudAnalysis& prelim, const FraudAnalysis& fin);

#endif

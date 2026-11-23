#include <iostream>
#include <iomanip>
#include <cctype>
#include <algorithm>

#include "fraudmodule.h"
#include "latlong.h"
#include "time_module.h"

using namespace std;


// ==========================================
// SMALL HELPERS
// ==========================================

bool sameTextIgnoreCase(const string& a, const string& b)
{
    if (a.size() != b.size()) return false;

    for (size_t i = 0; i < a.size(); i++)
    {
        if (tolower(static_cast<unsigned char>(a[i])) !=
            tolower(static_cast<unsigned char>(b[i])))
            return false;
    }
    return true;
}

int capScore(int score)
{
    if (score > MAX_SCORE) return MAX_SCORE;
    if (score < 0) return 0;
    return score;
}

string classifyRisk(int score)
{
    score = capScore(score);

    if (score >= CRITICAL_THRESHOLD)   return "CRITICAL FRAUD";
    if (score >= OTP_THRESHOLD)        return "HIGH RISK";
    if (score >= SUSPICIOUS_THRESHOLD) return "SUSPICIOUS";
    return "NORMAL";
}

bool otpRequiredFor(int score) { return score >= OTP_THRESHOLD; }
bool shouldBlock(int score)    { return score >= CRITICAL_THRESHOLD; }


// ==========================================
// THE UNIFIED SCORING FUNCTION
// ==========================================

FraudAnalysis calculateFraudScore(
    const Transaction& current,
    const vector<Transaction>& history,
    const CardHistory& card,
    int failedAuthAttempts,
    bool wrongOtp)
{
    FraudAnalysis a;
    a.failedAuthAttempts = failedAuthAttempts;
    a.wrongOtp = wrongOtp;

    a.hasCardHistory = card.found;
    a.cardTotalTransactions = card.totalTransactions;
    a.cardBlockedTransactions = card.blockedTransactions;
    a.cardPastFailedOtps = card.failedOtpAttempts;
    a.cardPastOtpRequests = card.otpRequiredCount;

    long long nowSeconds = 0;
    bool nowValid = parseTimestamp(current.timestamp, nowSeconds);

    // ------------------------------------------------------------
    // DATA FROM transactions.csv: timestamps for the frequency window.
    // Only this card's earlier transactions; the current one is never
    // counted. Blocked attempts still count as activity.
    // ------------------------------------------------------------
    for (const Transaction& h : history)
    {
        if (h.cardNo != current.cardNo) continue;
        if (h.id == current.id) continue;

        long long t = 0;
        if (nowValid && parseTimestamp(h.timestamp, t))
        {
            long long age = nowSeconds - t;
            if (age >= 0 && age <= FREQUENCY_WINDOW_SECS)
                a.recentTransactionCount++;
        }
    }

    // ------------------------------------------------------------
    // DATA FROM card_history.csv: completed-transaction aggregates.
    // (Blocked transactions are not included, so they never skew the
    //  average or make a device/merchant "trusted".)
    // ------------------------------------------------------------
    a.previousCompletedCount = card.completedTransactions;

    // ------------------------------------------------------------
    // RULE 1 - SPENDING ANOMALY (+15)
    // ------------------------------------------------------------
    if (card.completedTransactions > 0)
    {
        a.hasHistoricalAverage = true;
        a.historicalAverage = averageAmount(card);

        if (a.historicalAverage > 0 &&
            current.amount >= SPENDING_MULTIPLIER * a.historicalAverage)
        {
            a.spendingAnomaly = true;
            a.rawScore += POINTS_SPENDING_ANOMALY;
            a.reasons.push_back(
                "Extreme spending anomaly: transaction is at least 2x historical average");
        }
    }

    // ------------------------------------------------------------
    // RULE 2 - RAPID FREQUENCY (+10)
    // ------------------------------------------------------------
    if (a.recentTransactionCount >= FREQUENCY_LIMIT)
    {
        a.rapidFrequency = true;
        a.rawScore += POINTS_RAPID_FREQUENCY;
        a.reasons.push_back(
            "Rapid transaction frequency: 5 or more transactions within 10 minutes");
    }

    // ------------------------------------------------------------
    // RULE 3 - IMPOSSIBLE TRAVEL (+30)
    // Last location comes from card_history.csv.
    // Needs BOTH a long distance and a speed no flight could manage.
    // ------------------------------------------------------------
    if (card.hasLastLocation && nowValid)
    {
        a.hasPreviousLocation = true;

        a.distanceKm = calculateDistance(
            card.lastLatitude, card.lastLongitude,
            current.latitude, current.longitude);

        double seconds = static_cast<double>(nowSeconds - card.lastTimestamp);
        if (seconds < 1.0) seconds = 1.0;          // avoid divide-by-zero

        a.hoursElapsed = seconds / 3600.0;
        a.speedKmh = a.distanceKm / a.hoursElapsed;

        if (a.distanceKm >= TRAVEL_MIN_DISTANCE_KM &&
            a.speedKmh > TRAVEL_MAX_SPEED_KMH)
        {
            a.impossibleTravel = true;
            a.rawScore += POINTS_IMPOSSIBLE_TRAVEL;
            a.reasons.push_back("Impossible travel detected");
        }
    }

    // ------------------------------------------------------------
    // RULE 4 - NEW DEVICE (+10) / RULE 5 - NEW MERCHANT (+5)
    // Compared against the card's stored device/merchant lists.
    // No completed history = nothing to compare with = no flag.
    // ------------------------------------------------------------
    if (card.completedTransactions > 0)
    {
        a.hasDeviceHistory = !card.devices.empty();
        a.hasMerchantHistory = !card.merchants.empty();

        if (a.hasDeviceHistory &&
            !containsIgnoreCase(card.devices, current.deviceId))
        {
            a.newDevice = true;
            a.rawScore += POINTS_NEW_DEVICE;
            a.reasons.push_back("Unusual device detected");
        }

        if (a.hasMerchantHistory &&
            !containsIgnoreCase(card.merchants, current.merchant))
        {
            a.newMerchant = true;
            a.rawScore += POINTS_NEW_MERCHANT;
            a.reasons.push_back("Unusual/new merchant detected");
        }
    }

    // ------------------------------------------------------------
    // RULE 6 - REPEATED AUTHENTICATION FAILURES (+10)
    // (only known after the OTP step, so 0 during the preliminary run)
    // ------------------------------------------------------------
    if (failedAuthAttempts >= REPEATED_FAIL_LIMIT)
    {
        a.repeatedAuthFailure = true;
        a.rawScore += POINTS_REPEATED_AUTH_FAIL;
        a.reasons.push_back("Repeated authentication failure");
    }

    // ------------------------------------------------------------
    // OTP PENALTY (+50) - separate from rule 6
    // ------------------------------------------------------------
    if (wrongOtp)
    {
        a.rawScore += POINTS_WRONG_OTP;
        a.reasons.push_back("Incorrect OTP");
    }

    // ------------------------------------------------------------
    // CAP + CLASSIFY
    // ------------------------------------------------------------
    a.score = capScore(a.rawScore);
    a.status = classifyRisk(a.score);

    return a;
}


// ==========================================
// DISPLAY HELPERS
// ==========================================

static string yesNoPoints(bool hit, int points)
{
    return hit ? ("+" + to_string(points)) : "+0";
}

void printFraudAnalysis(const FraudAnalysis& a, const Transaction& cur)
{
    cout << fixed << setprecision(2);

    cout << "\n========== FRAUD ANALYSIS ==========\n\n";

    if (a.hasCardHistory)
        cout << "Card history (card_history.csv): " << a.cardTotalTransactions
             << " transactions on record, " << a.cardBlockedTransactions << " blocked\n"
             << "Past OTP requests / failed OTPs : " << a.cardPastOtpRequests
             << " / " << a.cardPastFailedOtps << "\n\n";
    else
        cout << "Card history (card_history.csv): none - first transaction for this card\n\n";

    // Spending
    if (a.hasHistoricalAverage)
        cout << "Historical average : Rs." << a.historicalAverage
             << " (from " << a.previousCompletedCount << " previous transactions)\n";
    else
        cout << "Historical average : N/A (first transaction for this card)\n";
    cout << "Current transaction: Rs." << cur.amount << "\n";
    cout << "Spending anomaly   : " << yesNoPoints(a.spendingAnomaly, POINTS_SPENDING_ANOMALY) << "\n\n";

    // Frequency
    cout << "Transactions in last 10 minutes: " << a.recentTransactionCount << "\n";
    cout << "Rapid frequency    : " << yesNoPoints(a.rapidFrequency, POINTS_RAPID_FREQUENCY) << "\n\n";

    // Travel
    if (a.hasPreviousLocation)
        cout << "Travel             : " << a.distanceKm << " km in "
             << a.hoursElapsed << " h (" << a.speedKmh << " km/h)\n";
    else
        cout << "Travel             : no previous location\n";
    cout << "Impossible travel  : " << (a.impossibleTravel ? "Yes +30" : "No") << "\n\n";

    // Merchant
    cout << "Merchant           : " << cur.merchant << " ("
         << (!a.hasMerchantHistory ? "no history yet" : (a.newMerchant ? "New Merchant" : "Known Merchant")) << ")\n";
    cout << "Unusual merchant   : " << yesNoPoints(a.newMerchant, POINTS_NEW_MERCHANT) << "\n\n";

    // Device
    cout << "Device             : " << cur.deviceId << " ("
         << (!a.hasDeviceHistory ? "first known device" : (a.newDevice ? "New Device" : "Known Device")) << ")\n";
    cout << "Unusual device     : " << yesNoPoints(a.newDevice, POINTS_NEW_DEVICE) << "\n";

    // Authentication (only meaningful in the final run)
    if (a.failedAuthAttempts > 0 || a.wrongOtp)
    {
        cout << "\nFailed OTP attempts: " << a.failedAuthAttempts << "\n";
        cout << "Repeated auth fail : " << yesNoPoints(a.repeatedAuthFailure, POINTS_REPEATED_AUTH_FAIL) << "\n";
        cout << "Wrong OTP penalty  : " << yesNoPoints(a.wrongOtp, POINTS_WRONG_OTP) << "\n";
    }

    cout << "\n------------------------------------\n";
    if (a.rawScore > MAX_SCORE)
        cout << "(raw total " << a.rawScore << " capped at " << MAX_SCORE << ")\n";
}

void printOtpPenaltySummary(const FraudAnalysis& prelim, const FraudAnalysis& fin)
{
    cout << "\nPreliminary score  : " << prelim.score << "\n";
    cout << "Additional risk    : +" << (fin.rawScore - prelim.rawScore) << "\n";
}

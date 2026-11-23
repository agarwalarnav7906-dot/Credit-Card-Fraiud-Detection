#include <iostream>
#include <fstream>
#include <sstream>
#include <iomanip>
#include "transaction_history.h"
#include "atomic_file.h"
#include "latlong.h"

using namespace std;

static const char* CSV_HEADER =
    "TransactionID,CardID,Amount,Merchant,Location,Latitude,Longitude,"
    "Timestamp,DeviceID,PrelimScore,FinalScore,OTPStatus,AuthStatus,"
    "FraudStatus,Decision,FailedAuthAttempts,Reasons";

// Commas/newlines would break the CSV, so replace them before saving.
static string clean(string s)
{
    for (char& c : s) {
        if (c == ',' || c == '\n' || c == '\r') c = ';';
    }
    return s;
}

bool appendTransaction(const Transaction& t, const string& file)
{
    ostringstream row;

    // FIX: default stream precision (6 significant digits) wrote 1234567.89 as
    // "1.23457e+06", silently changing the stored amount. 15 digits keeps
    // every amount up to 999,999,999.99 exact and leaves "20000" unchanged.
    row << setprecision(15);

    row << t.id << ','
        << t.cardNo << ','
        << t.amount << ','
        << toUpperCase(clean(t.merchant)) << ','
        << toUpperCase(clean(t.location)) << ','
        << t.latitude << ','
        << t.longitude << ','
        << t.timestamp << ','
        << clean(t.deviceId) << ','
        << t.prelimScore << ','
        << t.finalScore << ','
        << clean(t.otpStatus) << ','
        << clean(t.authStatus) << ','
        << clean(t.fraudStatus) << ','
        << clean(t.decision) << ','
        << t.failedAuthAttempts << ','
        << clean(t.reasons) << '\n';

    // The old file plus this row is written as a new file and renamed over
    // the old one; the header is added when the file is new or empty.
    if (!appendToFileAtomically(file, row.str(), string(CSV_HEADER) + '\n')) {
        cout << "ERROR: Could not write " << file << '\n';
        return false;
    }

    return true;
}

bool loadTransactions(vector<Transaction>& transactions, const string& file)
{
    transactions.clear();

    ifstream in(file);
    if (!in.is_open()) return true;     // no history yet

    string line;
    getline(in, line);                  // header

    while (getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;

        stringstream ss(line);
        string f[17];

        // First 16 fields are comma separated; Reasons is the remainder.
        for (int i = 0; i < 16; i++) getline(ss, f[i], ',');
        getline(ss, f[16]);

        try {
            Transaction t;
            t.id = stoi(f[0]);
            t.cardNo = stoi(f[1]);
            t.amount = stod(f[2]);
            t.merchant = toUpperCase(f[3]);
            t.location = toUpperCase(f[4]);
            t.latitude = stod(f[5]);
            t.longitude = stod(f[6]);
            t.timestamp = f[7];
            t.deviceId = f[8];
            t.prelimScore = stoi(f[9]);
            t.finalScore = stoi(f[10]);
            t.score = t.finalScore;
            t.otpStatus = f[11];
            t.authStatus = f[12];
            t.fraudStatus = f[13];
            t.decision = f[14];
            t.failedAuthAttempts = stoi(f[15]);
            t.reasons = f[16];

            t.blocked = (t.decision == "BLOCKED");
            t.flagged = t.blocked;
            t.otpRequired = (t.otpStatus != "NOT REQUIRED");
            t.otpVerified = (t.otpStatus == "VERIFIED");

            transactions.push_back(t);
        } catch (...) {
            cout << "Warning: invalid transaction record skipped.\n";
        }
    }

    return true;
}

bool transactionIdExists(const vector<Transaction>& transactions, int id)
{
    for (const Transaction& t : transactions) {
        if (t.id == id) return true;
    }
    return false;
}

void displayTransactions(const vector<Transaction>& t)
{
    cout << "\n========== ALL TRANSACTIONS ==========\n";
    for (const Transaction& x : t) {
        cout << "\nTransaction ID: " << x.id
             << "\nCard ID: " << x.cardNo
             << "\nAmount: " << x.amount
             << "\nMerchant: " << x.merchant
             << "\nLocation: " << x.location
             << "\nCoordinates: " << x.latitude << ", " << x.longitude
             << "\nTimestamp: " << x.timestamp
             << "\nDevice ID: " << x.deviceId
             << "\nDistance from previous: " << x.distanceFromPrevious << " km"
             << "\nPreliminary Score: " << x.prelimScore
             << "\nFinal Score: " << x.finalScore
             << "\nOTP Status: " << x.otpStatus
             << "\nFraud Status: " << x.fraudStatus
             << "\nDecision: " << x.decision
             << "\nReasons: " << x.reasons
             << '\n';
    }
}

void findByCardID(const vector<Transaction>& t, int cardID)
{
    bool found = false;
    cout << "\n========== CARD HISTORY ==========\n";

    for (const Transaction& x : t) {
        if (x.cardNo == cardID) {
            found = true;
            cout << "\nTransaction ID: " << x.id
                 << " | Amount: " << x.amount
                 << " | Merchant: " << x.merchant
                 << " | Location: " << x.location
                 << " | Device: " << x.deviceId
                 << " | Time: " << x.timestamp
                 << " | Score: " << x.finalScore
                 << " | " << x.fraudStatus
                 << " | " << x.decision
                 << '\n';
        }
    }

    if (!found) cout << "Card ID not found.\n";
}

#include <iostream>
#include <fstream>
#include <sstream>
#include <cctype>
#include <iomanip>

#include "card_history.h"
#include "atomic_file.h"
#include "time_module.h"
#include "latlong.h"

using namespace std;

static const char* CARD_HEADER =
    "CardID,TotalTransactions,CompletedTransactions,BlockedTransactions,"
    "TotalCompletedAmount,AverageAmount,Merchants,Devices,LastLocation,"
    "LastLatitude,LastLongitude,LastTimestamp,OTPRequiredCount,"
    "FailedOTPAttempts,LastFinalScore,LastUpdated";


// ---------- helpers ----------

static bool equalNoCase(const string& a, const string& b)
{
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); i++)
        if (tolower(static_cast<unsigned char>(a[i])) !=
            tolower(static_cast<unsigned char>(b[i])))
            return false;
    return true;
}

bool containsIgnoreCase(const vector<string>& list, const string& value)
{
    for (const string& s : list)
        if (equalNoCase(s, value)) return true;
    return false;
}

double averageAmount(const CardHistory& c)
{
    if (c.completedTransactions <= 0) return 0.0;
    return c.totalCompletedAmount / c.completedTransactions;
}

// ',' and '|' are structural characters in this file format
static string clean(string s)
{
    for (char& c : s)
        if (c == ',' || c == '|' || c == '\n' || c == '\r') c = ';';
    return s;
}

static vector<string> upperAll(vector<string> v)
{
    for (string& s : v) s = toUpperCase(s);
    return v;
}

static string joinList(const vector<string>& v)
{
    string out;
    for (size_t i = 0; i < v.size(); i++) {
        if (i > 0) out += '|';
        out += clean(v[i]);
    }
    return out;
}

static vector<string> splitList(const string& text)
{
    vector<string> out;
    string item;
    stringstream ss(text);
    while (getline(ss, item, '|'))
        if (!item.empty()) out.push_back(item);
    return out;
}


// ---------- file I/O ----------

bool loadAllCardHistory(vector<CardHistory>& all, const string& file)
{
    all.clear();

    ifstream in(file);
    if (!in.is_open()) return true;          // no file yet = no history

    string line;
    getline(in, line);                       // header

    while (getline(in, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;

        stringstream ss(line);
        string f[16];
        for (int i = 0; i < 16; i++) getline(ss, f[i], ',');

        try {
            CardHistory c;
            c.found = true;
            c.cardNo = stoi(f[0]);
            c.totalTransactions = stoi(f[1]);
            c.completedTransactions = stoi(f[2]);
            c.blockedTransactions = stoi(f[3]);
            c.totalCompletedAmount = stod(f[4]);
            // f[5] (AverageAmount) is derived on save - recomputed in code
            c.merchants = upperAll(splitList(f[6]));
            c.devices = splitList(f[7]);
            c.lastLocation = f[8];
            c.lastLatitude = stod(f[9]);
            c.lastLongitude = stod(f[10]);
            c.lastTimestamp = stoll(f[11]);
            c.hasLastLocation = (!c.lastLocation.empty() && c.lastTimestamp > 0);
            c.otpRequiredCount = stoi(f[12]);
            c.failedOtpAttempts = stoi(f[13]);
            c.lastFinalScore = stoi(f[14]);
            c.lastUpdated = f[15];
            all.push_back(c);
        } catch (...) {
            cout << "Warning: invalid card_history record skipped.\n";
        }
    }
    return true;
}

bool saveAllCardHistory(const vector<CardHistory>& all, const string& file)
{
    ostringstream out;

    // FIX: see transaction_history.cpp - TotalCompletedAmount / AverageAmount
    // were rounded to 6 significant digits, corrupting the spending average.
    out << setprecision(15);
    out << CARD_HEADER << '\n';

    for (const CardHistory& c : all) {
        out << c.cardNo << ','
            << c.totalTransactions << ','
            << c.completedTransactions << ','
            << c.blockedTransactions << ','
            << c.totalCompletedAmount << ','
            << averageAmount(c) << ','
            << joinList(upperAll(c.merchants)) << ','
            << joinList(c.devices) << ','
            << toUpperCase(clean(c.lastLocation)) << ','
            << c.lastLatitude << ','
            << c.lastLongitude << ','
            << c.lastTimestamp << ','
            << c.otpRequiredCount << ','
            << c.failedOtpAttempts << ','
            << c.lastFinalScore << ','
            << clean(c.lastUpdated) << '\n';
    }

    if (!writeFileAtomically(file, out.str())) {
        cout << "ERROR: Could not write " << file << '\n';
        return false;
    }
    return true;
}


// ---------- aggregation ----------

void applyTransactionToCard(CardHistory& card, const Transaction& t)
{
    card.found = true;
    card.cardNo = t.cardNo;
    card.totalTransactions++;

    if (t.blocked) {
        // Blocked attempts are counted but never become "trusted" behaviour
        card.blockedTransactions++;
    } else {
        card.completedTransactions++;
        card.totalCompletedAmount += t.amount;

        if (!t.merchant.empty() && !containsIgnoreCase(card.merchants, t.merchant))
            card.merchants.push_back(toUpperCase(t.merchant));

        if (!t.deviceId.empty() && !containsIgnoreCase(card.devices, t.deviceId))
            card.devices.push_back(t.deviceId);

        long long epoch = 0;
        if (parseTimestamp(t.timestamp, epoch)) {
            card.hasLastLocation = true;
            card.lastLocation = t.location;
            card.lastLatitude = t.latitude;
            card.lastLongitude = t.longitude;
            card.lastTimestamp = epoch;
        }
    }

    if (t.otpRequired) card.otpRequiredCount++;
    card.failedOtpAttempts += t.failedAuthAttempts;
    card.lastFinalScore = t.finalScore;
    card.lastUpdated = t.timestamp;
}

CardHistory getCardHistory(int cardNo, const vector<Transaction>& transactions,
                           const string& file)
{
    vector<CardHistory> all;
    loadAllCardHistory(all, file);

    for (const CardHistory& c : all)
        if (c.cardNo == cardNo) return c;

    // No aggregate row yet: rebuild from detailed transactions, if any exist
    CardHistory rebuilt;
    rebuilt.cardNo = cardNo;

    for (const Transaction& t : transactions)
        if (t.cardNo == cardNo)
            applyTransactionToCard(rebuilt, t);

    return rebuilt;                          // found=false when nothing exists
}

bool updateCardHistory(CardHistory& card, const Transaction& t, const string& file)
{
    applyTransactionToCard(card, t);

    vector<CardHistory> all;
    loadAllCardHistory(all, file);

    bool replaced = false;
    for (CardHistory& c : all) {
        if (c.cardNo == card.cardNo) {
            c = card;
            replaced = true;
            break;
        }
    }
    if (!replaced) all.push_back(card);

    return saveAllCardHistory(all, file);
}

void displayCardHistory(const CardHistory& c)
{
    cout << "Card " << c.cardNo << ": " << c.totalTransactions << " total, "
         << c.completedTransactions << " completed, "
         << c.blockedTransactions << " blocked | average Rs."
         << averageAmount(c) << " | failed OTPs: " << c.failedOtpAttempts << '\n';
}

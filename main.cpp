#include <iostream>
#include <string>
#include <ctime>

#include "hashmodule.h"
#include "latlong.h"
#include "time_module.h"
#include "fraudmodule.h"
#include "location_history.h"
#include "transaction.h"
#include "transaction_history.h"
#include "card_history.h"
#include <vector>

using namespace std;


// Queue functions
struct QueueTransaction
{
    int id;
    double amount;
    int score;
};

void insertRear(QueueTransaction t);
void displayQueue();


int main()
{
    // ==========================================
    // LOAD CARD DATABASE
    // ==========================================

    loadCards();


    cout << "=========================================\n";
    cout << "        CREDIT CARD FRAUD SYSTEM\n";
    cout << "=========================================\n";


    // ==========================================
    // TRANSACTION ID
    // ==========================================

    int transactionID;

    // Persistent history (transactions.csv)
    vector<Transaction> history;
    loadTransactions(history);

    cout << "\nEnter Transaction ID: ";
    cin >> transactionID;

    if (transactionIdExists(history, transactionID))
    {
        cout << "\nERROR: Transaction ID already exists in transactions.csv.\n";
        return 0;
    }


    // ==========================================
    // CARD
    // ==========================================

    int cardID;

    cout << "Enter Card ID: ";
    cin >> cardID;


    Card card;

    if (!searchCard(cardID, card))
    {
        cout << "\nERROR: Card not found.\n";
        return 0;
    }


    // ==========================================
    // CARD DETAILS
    // ==========================================

    cout << "\n========== CARD DETAILS ==========\n";

    cout << "Card ID      : "
         << card.cardNo << endl;

    cout << "Card Holder  : "
         << card.name << endl;

    cout << "Account Type : "
         << card.accountType << endl;

    cout << "Status       : "
         << card.status << endl;

    cout << "Points       : "
         << card.points << endl;


    // ==========================================
    // AMOUNT
    // ==========================================

    double amount;

    cout << "\nEnter Transaction Amount: ";
    cin >> amount;

    if (cin.fail() || amount <= 0)
    {
        cout << "\nERROR: Amount must be a positive number.\n";
        return 0;
    }


    // ==========================================
    // MERCHANT
    // ==========================================

    string merchant;

    cout << "Enter Merchant: ";
    cin >> merchant;


    // ==========================================
    // LOCATION
    // ==========================================

    string locationName;

    cout << "Enter Transaction Location: ";
    cin >> locationName;


    Location currentLocation;

    if (!getLocation(locationName, currentLocation))
    {
        cout << "\nERROR: Location not found.\n";
        cout << "Add the location to locations.csv first.\n";

        return 0;
    }


    // ==========================================
    // DEVICE (simulated device identifier)
    // ==========================================

    string deviceId;

    cout << "Enter Device ID: ";
    cin >> deviceId;


    // ==========================================
    // CURRENT TIME
    // ==========================================

    string currentTime = getCurrentTime();
    string currentTimestamp = getCurrentTimestamp();

    cout << "\nCurrent Time: "
         << currentTime << endl;


    // ==========================================
    // CURRENT TIMESTAMP (epoch derived from the same string we store)
    // ==========================================

    long long currentTimestampValue = 0;

    if (!parseTimestamp(currentTimestamp, currentTimestampValue))
    {
        currentTimestampValue = static_cast<long long>(time(nullptr));
    }


    // ==========================================
    // READ CARD HISTORY FROM card_history.csv
    // (rebuilt from transactions.csv if the card has no row yet)
    // ==========================================

    CardHistory cardHistory = getCardHistory(cardID, history);

    // One-time migration: cards that only exist in the old
    // location_history.csv keep their last known location.
    if (!cardHistory.hasLastLocation)
    {
        PreviousLocation legacy;

        if (getPreviousLocation(cardID, legacy))
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
            currentLocation.latitude,
            currentLocation.longitude
        );

        cout << "\n========== LOCATION ANALYSIS ==========\n";

        cout << "Previous Location : "
             << cardHistory.lastLocation << endl;

        cout << "Current Location  : "
             << currentLocation.name << endl;

        cout << "Distance          : "
             << distance << " km" << endl;
    }
    else
    {
        cout << "\nNo previous location found.\n";
        cout << "This is the first recorded location for this card.\n";
    }


    // ==========================================
    // CREATE TRANSACTION
    // ==========================================

    Transaction txn;

    txn.id = transactionID;
    txn.cardNo = cardID;
    txn.amount = amount;
    txn.merchant = merchant;
    txn.location = currentLocation.name;
    txn.latitude = currentLocation.latitude;
    txn.longitude = currentLocation.longitude;
    txn.timestamp = currentTimestamp;
    txn.deviceId = deviceId;
    txn.distanceFromPrevious = distance;


    // ==========================================
    // STEP 1: PRELIMINARY FRAUD ANALYSIS
    // (history -> average -> frequency -> travel -> device -> merchant)
    // ==========================================

    FraudAnalysis prelim =
        calculateFraudScore(
            txn,
            history,
            cardHistory,
            0,
            false
        );

    printFraudAnalysis(prelim, txn);

    cout << "Preliminary Fraud Score: " << prelim.score << endl;
    cout << "Status: " << prelim.status << endl;
    cout << "------------------------------------\n";

    txn.prelimScore = prelim.score;

    FraudAnalysis finalAnalysis = prelim;


    // ==========================================
    // STEP 2: OTP / STEP-UP AUTHENTICATION
    // ==========================================

    if (otpRequiredFor(prelim.score))
    {
        txn.otpRequired = true;
        txn.authenticationRequired = true;
        txn.authenticationMethod = "OTP";

        cout << "\n=========================================\n";
        cout << "OTP VERIFICATION REQUIRED\n";
        cout << "=========================================\n";

        /*
            Demo OTP: 123456
            Can be replaced with real OTP generation later.
        */

        int failed = 0;
        bool verified = false;

        while (failed < MAX_OTP_ATTEMPTS && !verified)
        {
            string otp;

            cout << "Enter OTP (attempt " << (failed + 1)
                 << " of " << MAX_OTP_ATTEMPTS << "): ";
            cin >> otp;

            if (otp == "123456")
            {
                verified = true;
            }
            else
            {
                failed++;
                cout << "Incorrect OTP.\n";
            }
        }

        txn.failedAuthAttempts = failed;
        txn.otpVerified = verified;

        if (verified)
        {
            txn.otpStatus = "VERIFIED";
            txn.authStatus = "SUCCESSFUL";

            cout << "OTP VERIFIED SUCCESSFULLY.\n";
            cout << "Authentication successful. No OTP penalty.\n";
        }
        else
        {
            txn.otpStatus = "FAILED";
            txn.authStatus = "FAILED";

            cout << "Additional risk added: +"
                 << POINTS_WRONG_OTP << endl;
        }

        // Recalculate from scratch with the authentication outcome
        finalAnalysis =
            calculateFraudScore(
                txn,
                history,
                cardHistory,
                failed,
                !verified
            );

        if (failed >= REPEATED_FAIL_LIMIT)
        {
            cout << "Repeated authentication failure: +"
                 << POINTS_REPEATED_AUTH_FAIL << endl;
        }

        cout << "\nPreliminary Fraud Score: " << prelim.score << endl;
        cout << "Final Fraud Score      : " << finalAnalysis.score << endl;
        cout << "Status: " << finalAnalysis.status << endl;
    }


    // ==========================================
    // STEP 3: FINAL DECISION
    // ==========================================

    txn.finalScore = finalAnalysis.score;
    txn.score = finalAnalysis.score;
    txn.fraudStatus = finalAnalysis.status;

    txn.blocked = shouldBlock(finalAnalysis.score);
    txn.flagged = txn.blocked;
    txn.decision = txn.blocked ? "BLOCKED" : "ALLOWED";

    if (txn.blocked)
    {
        txn.flagTimestamp = currentTimestamp;
        cout << "\nTransaction blocked.\n";
    }

    for (size_t i = 0; i < finalAnalysis.reasons.size(); i++)
    {
        if (i > 0) txn.reasons += "; ";
        txn.reasons += finalAnalysis.reasons[i];
    }

    if (txn.reasons.empty())
        txn.reasons = "No risk factors";


    // ==========================================
    // UPDATE CARD POINTS
    // ==========================================

    int oldPoints = card.points;

    int newPoints =
        oldPoints + txn.finalScore;


    if (updatePoints(cardID, newPoints))
    {
        cout << "\nFraud points saved permanently.\n";
    }


    // ==========================================
    // SAVE CURRENT LOCATION
    // (blocked transactions do not move the card's "last known" location)
    // ==========================================

    if (!txn.blocked)
    {
        saveLocation(
            cardID,
            currentLocation.name,
            currentLocation.latitude,
            currentLocation.longitude,
            currentTimestampValue
        );
    }


    // ==========================================
    // PERSIST TRANSACTION (transactions.csv)
    // ==========================================

    if (appendTransaction(txn))
    {
        cout << "Transaction saved to " << TRANSACTION_FILE << ".\n";
    }


    // ==========================================
    // UPDATE card_history.csv (aggregated card data)
    // ==========================================

    if (updateCardHistory(cardHistory, txn))
    {
        cout << "Card history updated in " << CARD_HISTORY_FILE << ".\n";
    }


    // ==========================================
    // QUEUE
    // ==========================================

    QueueTransaction queueTransaction;

    queueTransaction.id = transactionID;
    queueTransaction.amount = amount;
    queueTransaction.score = txn.finalScore;

    insertRear(queueTransaction);


    // ==========================================
    // FINAL RESULT
    // ==========================================

    cout << "\n=========================================\n";
    cout << "           TRANSACTION RESULT\n";
    cout << "=========================================\n";

    cout << "Transaction ID      : " << transactionID << endl;
    cout << "Card ID             : " << cardID << endl;
    cout << "Merchant            : " << merchant << endl;
    cout << "Location            : " << locationName << endl;
    cout << "Device ID           : " << deviceId << endl;
    cout << "Distance            : " << distance << " km" << endl;
    cout << "Timestamp           : " << currentTimestamp << endl;

    cout << "Preliminary Score   : " << txn.prelimScore << endl;
    cout << "Final Fraud Score   : " << txn.finalScore << endl;
    cout << "Status              : " << txn.fraudStatus << endl;

    cout << "OTP Required        : " << (txn.otpRequired ? "YES" : "NO") << endl;
    cout << "OTP Status          : " << txn.otpStatus << endl;
    cout << "Authentication      : " << txn.authStatus << endl;
    cout << "Decision            : " << txn.decision << endl;
    cout << "Reasons             : " << txn.reasons << endl;

    cout << "Old Points          : " << oldPoints << endl;
    cout << "New Points          : " << newPoints << endl;

    cout << "=========================================\n";


    // ==========================================
    // QUEUE
    // ==========================================

    displayQueue();


    return 0;
}

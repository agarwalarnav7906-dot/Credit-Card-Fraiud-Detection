#include <iostream>
#include <string>
#include <ctime>
#include <cctype>

#include "hashmodule.h"
#include "latlong.h"
#include "time_module.h"
#include "fraudmodule.h"
#include "location_history.h"
#include "transaction.h"
#include "transaction_history.h"
#include "card_history.h"
#include "sortmodule.h"
#include "dequemodule.h"
#include "locationgraph.h"
#include "transaction_service.h"
#include <vector>

using namespace std;


// ==========================================================
// INPUT HELPERS + ADD NEW CARD
// ==========================================================

static int readInt(const string& prompt);   // defined further below

// Reads one whole line (spaces allowed), skipping any leftover newline from
// an earlier "cin >>". Commas are removed so they can't corrupt cards.csv.
// Returns "" only if input has been closed.
static string readText(const string& prompt)
{
    while (true)
    {
        cout << prompt;

        string s;
        if (!getline(cin >> ws, s)) return "";

        for (char& c : s)
            if (c == ',' || c == '|' || c == '\r') c = ' ';

        size_t b = s.find_first_not_of(' ');
        size_t e = s.find_last_not_of(' ');
        if (b == string::npos) { cout << "Cannot be empty.\n"; continue; }

        return s.substr(b, e - b + 1);
    }
}

// Accepts "y", "yes", "n", "no" in any case.
static bool readYesNo(const string& prompt)
{
    while (true)
    {
        string s = readText(prompt);
        if (s.empty()) return false;

        char c = (char)tolower((unsigned char)s[0]);
        if (c == 'y') return true;
        if (c == 'n') return false;

        cout << "Please answer y or n.\n";
    }
}

// Asks until the user picks optionA or optionB (full word or first letter,
// any case). Returns the canonical spelling, or "" if input closed.
static string readOneOf(const string& prompt, const string& a, const string& b)
{
    while (true)
    {
        string s = readText(prompt);
        if (s.empty()) return "";

        for (char& c : s) c = (char)tolower((unsigned char)c);

        string la = a, lb = b;
        for (char& c : la) c = (char)tolower((unsigned char)c);
        for (char& c : lb) c = (char)tolower((unsigned char)c);

        if (s == la || s == string(1, la[0])) return a;
        if (s == lb || s == string(1, lb[0])) return b;

        cout << "Please enter " << a << " or " << b << ".\n";
    }
}

// Collects the details for a new card and saves it to cards.csv.
static bool addCardInteractive(int cardID)
{
    if (cardID <= 0)
    {
        cout << "\nERROR: Card ID must be a positive number.\n";
        return false;
    }

    Card existing;
    if (searchCard(cardID, existing))
    {
        cout << "\nERROR: Card " << cardID << " already exists.\n";
        return false;
    }

    string holderName = readText("Enter Card Holder Name: ");
    if (holderName.empty()) return false;

    string accountType = readOneOf("Enter Account Type (Credit/Debit): ",
                                   "Credit", "Debit");
    if (accountType.empty()) return false;

    string status = readOneOf("Enter Status (Active/Blocked): ",
                              "Active", "Blocked");
    if (status.empty()) return false;

    if (!addCard(cardID, holderName, accountType, status, 0))
    {
        cout << "\nERROR: Could not add card.\n";
        return false;
    }

    return true;
}


// ==========================================================
// PROCESS ONE NEW TRANSACTION (original fraud-detection flow)
// ==========================================================

static void processNewTransaction(TransactionDeque& reviewQueue,
                                  const LocationGraph& graph)
{
    // ==========================================
    // TRANSACTION ID
    // ==========================================

    // Persistent history (transactions.csv)
    vector<Transaction> history;
    loadTransactions(history);

    // FIX: was "cin >> transactionID" with no check, so typing letters left
    // the ID at 0 (and the stream failed), and 0 / negative IDs were accepted.
    int transactionID = readInt("\nEnter Transaction ID: ");

    if (transactionID <= 0)
    {
        cout << "\nERROR: Transaction ID must be a positive number.\n";
        return;
    }

    if (transactionIdExists(history, transactionID))
    {
        cout << "\nERROR: Transaction ID already exists in transactions.csv.\n";
        return;
    }


    // ==========================================
    // CARD
    // ==========================================

    int cardID = readInt("Enter Card ID: ");


    Card card;

    if (!searchCard(cardID, card))
    {
        cout << "\nCard " << cardID << " not found.\n";

        if (!readYesNo("Do you want to add it as a new card? (y/n): "))
        {
            cout << "\nTransaction cancelled.\n";
            return;
        }

        if (!addCardInteractive(cardID))
        {
            cout << "\nTransaction cancelled.\n";
            return;
        }

        // Load the freshly added card so the rest of the program can use it
        if (!searchCard(cardID, card))
        {
            cout << "\nERROR: Card was added but could not be read back.\n";
            return;
        }
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
        return;
    }


    // ==========================================
    // MERCHANT
    // ==========================================

    string merchant;

    cout << "Enter Merchant: ";
    cin >> merchant;
    merchant = toUpperCase(merchant);


    // ==========================================
    // LOCATION
    // ==========================================

    string locationName;

    cout << "Enter Transaction Location: ";
    cin >> locationName;
    locationName = toUpperCase(locationName);


    Location currentLocation;

    if (!getLocation(locationName, currentLocation))
    {
        cout << "\nERROR: Location not found.\n";
        cout << "Add the location to locations.csv first.\n";

        return;
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

    cout << "\nCurrent Time: "
         << currentTime << endl;


    // ==========================================
    // STEP 1: BEGIN TRANSACTION
    // (card history -> location -> preliminary fraud analysis)
    // ==========================================

    TransactionRequest request;

    request.id = transactionID;
    request.cardNo = cardID;
    request.amount = amount;
    request.merchant = merchant;
    request.location = currentLocation;
    request.deviceId = deviceId;

    PendingTransaction pending =
        beginTransaction(request, card, history, graph);

    const Transaction& txn = pending.txn;
    const FraudAnalysis& prelim = pending.prelim;


    // ==========================================
    // LOCATION ANALYSIS
    // ==========================================

    if (pending.hasPreviousLocation)
    {
        cout << "\n========== LOCATION ANALYSIS ==========\n";

        cout << "Previous Location : "
             << pending.previousLocation << endl;

        cout << "Current Location  : "
             << currentLocation.name << endl;

        cout << "Distance          : "
             << txn.distanceFromPrevious << " km" << endl;

        if (pending.route.found && pending.route.path.size() > 2)
        {
            cout << "Travel network    : ";
            graph.displayRoute(pending.route);
        }
    }
    else
    {
        cout << "\nNo previous location found.\n";
        cout << "This is the first recorded location for this card.\n";
    }


    // ==========================================
    // PRELIMINARY FRAUD ANALYSIS
    // ==========================================

    printFraudAnalysis(prelim, txn);

    cout << "Preliminary Fraud Score: " << prelim.score << endl;
    cout << "Status: " << prelim.status << endl;
    cout << "------------------------------------\n";


    // ==========================================
    // STEP 2: OTP / STEP-UP AUTHENTICATION
    // ==========================================

    OtpOutcome otp;

    if (pending.otpRequired)
    {
        cout << "\n=========================================\n";
        cout << "OTP VERIFICATION REQUIRED\n";
        cout << "=========================================\n";

        while (otp.failedAttempts < MAX_OTP_ATTEMPTS && !otp.verified)
        {
            string code;

            cout << "Enter OTP (attempt " << (otp.failedAttempts + 1)
                 << " of " << MAX_OTP_ATTEMPTS << "): ";
            cin >> code;

            if (verifyOtp(code))
            {
                otp.verified = true;
            }
            else
            {
                otp.failedAttempts++;
                cout << "Incorrect OTP.\n";
            }
        }

        if (otp.verified)
        {
            cout << "OTP VERIFIED SUCCESSFULLY.\n";
            cout << "Authentication successful. No OTP penalty.\n";
        }
        else
        {
            cout << "Additional risk added: +"
                 << POINTS_WRONG_OTP << endl;
        }
    }


    // ==========================================
    // STEP 3: COMPLETE TRANSACTION
    // (final score -> decision -> save everything)
    // ==========================================

    TransactionOutcome result = completeTransaction(
        pending, otp, reviewQueue,
        [&](const Transaction& decided, const FraudAnalysis& finalAnalysis)
        {
            if (decided.otpRequired)
            {
                if (otp.failedAttempts >= REPEATED_FAIL_LIMIT)
                {
                    cout << "Repeated authentication failure: +"
                         << POINTS_REPEATED_AUTH_FAIL << endl;
                }

                cout << "\nPreliminary Fraud Score: " << decided.prelimScore << endl;
                cout << "Final Fraud Score      : " << finalAnalysis.score << endl;
                cout << "Status: " << finalAnalysis.status << endl;
            }

            if (decided.blocked)
            {
                cout << "\nTransaction blocked.\n";
            }
        });


    // ==========================================
    // SAVE CONFIRMATIONS
    // ==========================================

    if (result.pointsSaved)
    {
        cout << "\nFraud points saved permanently.\n";
    }

    if (result.transactionSaved)
    {
        cout << "Transaction saved to " << TRANSACTION_FILE << ".\n";
    }

    if (result.cardHistorySaved)
    {
        cout << "Card history updated in " << CARD_HISTORY_FILE << ".\n";
    }

    if (result.enqueued)
    {
        cout << "\nTransaction added to the fraud review deque ("
             << (result.queuedAtFront ? "FRONT" : "REAR")
             << ").\n";

        if (result.evicted)
        {
            cout << "Deque was full: transaction " << result.dropped.id
                 << " dropped from the rear.\n";
        }
    }

    // ==========================================
    // FINAL RESULT
    // ==========================================

    const Transaction& done = result.txn;

    cout << "\n=========================================\n";
    cout << "           TRANSACTION RESULT\n";
    cout << "=========================================\n";

    cout << "Transaction ID      : " << transactionID << endl;
    cout << "Card ID             : " << cardID << endl;
    cout << "Merchant            : " << merchant << endl;
    cout << "Location            : " << locationName << endl;
    cout << "Device ID           : " << deviceId << endl;
    cout << "Distance            : " << done.distanceFromPrevious << " km" << endl;
    cout << "Timestamp           : " << done.timestamp << endl;

    cout << "Preliminary Score   : " << done.prelimScore << endl;
    cout << "Final Fraud Score   : " << done.finalScore << endl;
    cout << "Status              : " << done.fraudStatus << endl;

    cout << "OTP Required        : " << (done.otpRequired ? "YES" : "NO") << endl;
    cout << "OTP Status          : " << done.otpStatus << endl;
    cout << "Authentication      : " << done.authStatus << endl;
    cout << "Decision            : " << done.decision << endl;
    cout << "Reasons             : " << done.reasons << endl;

    cout << "Old Points          : " << result.oldPoints << endl;
    cout << "New Points          : " << result.newPoints << endl;

    cout << "=========================================\n";


    reviewQueue.display();
}


// ==========================================================
// SMALL INPUT HELPERS
// ==========================================================

static int readInt(const string& prompt)
{
    int value;

    while (true)
    {
        cout << prompt;

        if (cin >> value) return value;

        if (cin.eof()) return 0;            // input closed -> behave like "Exit"

        cin.clear();
        cin.ignore(10000, '\n');
        cout << "Please enter a number.\n";
    }
}


// ==========================================================
// SORTING MENU
// ==========================================================

static void sortMenu(bool topOnly)
{
    vector<Transaction> history;
    loadTransactions(history);

    if (history.empty())
    {
        cout << "\nNo transactions saved yet.\n";
        return;
    }

    if (topOnly)
    {
        int n = readInt("How many of the riskiest transactions to show? ");
        if (n < 1) n = 1;

        displaySortedTable(
            topRiskTransactions(history, static_cast<size_t>(n)),
            "TOP RISKIEST TRANSACTIONS");
        return;
    }

    cout << "\nSort by:\n  1. Time\n  2. Amount\n  3. Risk score\n";
    int f = readInt("Choice: ");

    cout << "Order:\n  1. Ascending\n  2. Descending\n";
    int o = readInt("Choice: ");

    if (f < 1 || f > 3 || o < 1 || o > 2)
    {
        cout << "Invalid choice.\n";
        return;
    }

    const char* names[] = {"", "TIME", "AMOUNT", "RISK SCORE"};
    string title = string("TRANSACTIONS SORTED BY ") + names[f] +
                   (o == 1 ? " (ASCENDING)" : " (DESCENDING)");

    displaySortedTable(
        sortTransactions(history,
                         static_cast<SortField>(f),
                         static_cast<SortOrder>(o)),
        title.c_str());
}


// ==========================================================
// REVIEW DEQUE MENU
// ==========================================================

static void printItem(const char* label, const QueueItem& q)
{
    cout << label << " transaction " << q.id
         << " | card " << q.cardNo
         << " | amount " << q.amount
         << " | score " << q.score
         << " | " << q.status << '\n';
}

static void dequeMenu(TransactionDeque& reviewQueue)
{
    while (true)
    {
        cout << "\n---------- FRAUD REVIEW DEQUE ----------\n"
             << "1. Display deque\n"
             << "2. Review next case (delete FRONT)\n"
             << "3. Remove newest low-priority case (delete REAR)\n"
             << "4. Peek front and rear\n"
             << "0. Back\n";

        int c = readInt("Choice: ");
        QueueItem q;

        if (c == 0) return;

        if (c == 1)
        {
            reviewQueue.display();
        }
        else if (c == 2)
        {
            if (reviewQueue.deleteFront(q))
            {
                printItem("Reviewed:", q);
                reviewQueue.save();
            }
            else cout << "Review queue is empty.\n";
        }
        else if (c == 3)
        {
            if (reviewQueue.deleteRear(q))
            {
                printItem("Removed :", q);
                reviewQueue.save();
            }
            else cout << "Review queue is empty.\n";
        }
        else if (c == 4)
        {
            if (reviewQueue.peekFront(q))
            {
                printItem("Front:", q);
                reviewQueue.peekRear(q);
                printItem("Rear :", q);
            }
            else cout << "Review queue is empty.\n";
        }
        else cout << "Invalid choice.\n";
    }
}


// ==========================================================
// LOCATION GRAPH MENU
// ==========================================================

static void printNames(const vector<string>& v)
{
    for (size_t i = 0; i < v.size(); i++)
    {
        if (i > 0) cout << " -> ";
        cout << v[i];
    }
    cout << '\n';
}

static void graphMenu(const LocationGraph& graph)
{
    while (true)
    {
        cout << "\n---------- LOCATION GRAPH ----------\n"
             << "1. Show graph (adjacency list)\n"
             << "2. Shortest route between two cities\n"
             << "3. Cities near a city (radius)\n"
             << "4. BFS / DFS from a city\n"
             << "5. Travel trail of a card\n"
             << "0. Back\n";

        int c = readInt("Choice: ");

        if (c == 0) return;

        if (c == 1)
        {
            graph.displayAdjacency();
        }
        else if (c == 2)
        {
            string a, b;
            cout << "From city: "; cin >> a;
            cout << "To city  : "; cin >> b;

            graph.displayRoute(graph.shortestRoute(a, b));

            double direct;
            if (graph.directDistance(a, b, direct))
                cout << "Straight-line distance: " << direct << " km\n";
        }
        else if (c == 3)
        {
            string a;
            cout << "City: "; cin >> a;
            int r = readInt("Radius (km): ");

            if (!graph.hasCity(a))
            {
                cout << "Unknown city.\n";
                continue;
            }

            auto list = graph.nearby(a, r);

            if (list.empty()) cout << "No cities within " << r << " km.\n";

            for (const auto& p : list)
                cout << "  " << p.first << " - " << p.second << " km\n";
        }
        else if (c == 4)
        {
            string a;
            cout << "Start city: "; cin >> a;

            if (!graph.hasCity(a))
            {
                cout << "Unknown city.\n";
                continue;
            }

            cout << "BFS: "; printNames(graph.bfs(a));
            cout << "DFS: "; printNames(graph.dfs(a));
        }
        else if (c == 5)
        {
            int card = readInt("Card ID: ");

            vector<Transaction> history;
            loadTransactions(history);

            graph.displayTrail(card, graph.cardTrail(card, history));
        }
        else cout << "Invalid choice.\n";
    }
}


// ==========================================================
// MAIN MENU
// ==========================================================

int main()
{
    loadCards();

    TransactionDeque reviewQueue;
    reviewQueue.load();

    LocationGraph graph;

    if (!graph.buildFromFile())
        cout << "Warning: locations.csv missing - location graph unavailable.\n";

    cout << "=========================================\n";
    cout << "        CREDIT CARD FRAUD SYSTEM\n";
    cout << "=========================================\n";

    while (true)
    {
        cout << "\n============= MAIN MENU =============\n"
             << "1. Process new transaction\n"
             << "2. View all transactions\n"
             << "3. Sort transactions (time / amount / risk)\n"
             << "4. Top riskiest transactions\n"
             << "5. Fraud review deque\n"
             << "6. Location graph\n"
             << "7. Add new card\n"
             << "0. Exit\n";

        int choice = readInt("Choice: ");

        if (choice == 0)
        {
            cout << "Goodbye.\n";
            return 0;
        }

        switch (choice)
        {
            case 1: processNewTransaction(reviewQueue, graph); break;

            case 2:
            {
                vector<Transaction> history;
                loadTransactions(history);
                displayTransactions(history);
                break;
            }

            case 3: sortMenu(false);            break;
            case 4: sortMenu(true);             break;
            case 5: dequeMenu(reviewQueue);     break;
            case 6: graphMenu(graph);           break;

            case 7:
            {
                int newCard = readInt("New Card ID: ");

                if (addCardInteractive(newCard))
                {
                    Card c;
                    if (searchCard(newCard, c))
                    {
                        cout << "\n========== NEW CARD ==========\n"
                             << "Card ID      : " << c.cardNo << '\n'
                             << "Card Holder  : " << c.name << '\n'
                             << "Account Type : " << c.accountType << '\n'
                             << "Status       : " << c.status << '\n'
                             << "Points       : " << c.points << '\n';
                    }
                }
                break;
            }

            default: cout << "Invalid choice.\n";
        }
    }
}

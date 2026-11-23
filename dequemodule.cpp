#include <iostream>
#include <iomanip>
#include <fstream>
#include <sstream>

#include "dequemodule.h"
#include "atomic_file.h"
#include "fraudmodule.h"      // thresholds

using namespace std;

TransactionDeque::TransactionDeque() : frontIdx(0), count(0) {}

void TransactionDeque::clear()
{
    frontIdx = 0;
    count = 0;
}

// ------------------------------------------------------------
// Circular-array arithmetic:
//   position p (0 = front)  ->  array index (frontIdx + p) % CAP
// ------------------------------------------------------------

bool TransactionDeque::insertRear(const QueueItem& item)
{
    if (isFull()) return false;

    items[(frontIdx + count) % DEQUE_CAPACITY] = item;
    count++;
    return true;
}

bool TransactionDeque::insertFront(const QueueItem& item)
{
    if (isFull()) return false;

    // step the front pointer one place backwards (wrap-around)
    frontIdx = (frontIdx - 1 + DEQUE_CAPACITY) % DEQUE_CAPACITY;
    items[frontIdx] = item;
    count++;
    return true;
}

bool TransactionDeque::deleteFront(QueueItem& removed)
{
    if (isEmpty()) return false;

    removed = items[frontIdx];
    frontIdx = (frontIdx + 1) % DEQUE_CAPACITY;
    count--;
    return true;
}

bool TransactionDeque::deleteRear(QueueItem& removed)
{
    if (isEmpty()) return false;

    removed = items[(frontIdx + count - 1) % DEQUE_CAPACITY];
    count--;
    return true;
}

bool TransactionDeque::peekFront(QueueItem& item) const
{
    if (isEmpty()) return false;
    item = items[frontIdx];
    return true;
}

bool TransactionDeque::peekRear(QueueItem& item) const
{
    if (isEmpty()) return false;
    item = items[(frontIdx + count - 1) % DEQUE_CAPACITY];
    return true;
}

bool TransactionDeque::at(int position, QueueItem& item) const
{
    if (position < 0 || position >= count) return false;
    item = items[(frontIdx + position) % DEQUE_CAPACITY];
    return true;
}

// ------------------------------------------------------------
// Fraud routing
// ------------------------------------------------------------

QueueItem makeQueueItem(const Transaction& t)
{
    QueueItem q;
    q.id        = t.id;
    q.cardNo    = t.cardNo;
    q.amount    = t.amount;
    q.score     = t.finalScore;
    q.status    = t.fraudStatus;
    q.timestamp = t.timestamp;
    return q;
}

bool TransactionDeque::enqueueForReview(const Transaction& t,
                                        bool* evictedAny,
                                        QueueItem* evicted)
{
    if (evictedAny != nullptr) *evictedAny = false;

    // Normal transactions need no manual review.
    if (t.finalScore < SUSPICIOUS_THRESHOLD) return false;

    QueueItem item = makeQueueItem(t);

    if (t.finalScore >= CRITICAL_THRESHOLD)
    {
        // Urgent case: jumps to the front. Make room if necessary.
        if (isFull())
        {
            QueueItem dropped;
            deleteRear(dropped);

            if (evictedAny != nullptr) *evictedAny = true;
            if (evicted != nullptr)    *evicted = dropped;
        }
        return insertFront(item);
    }

    // High risk / suspicious: waits its turn at the rear.
    return insertRear(item);
}

// ------------------------------------------------------------
// Display
// ------------------------------------------------------------

void TransactionDeque::display() const
{
    cout << "\n========== FRAUD REVIEW DEQUE ("
         << count << "/" << DEQUE_CAPACITY << ") ==========\n";

    if (isEmpty())
    {
        cout << "Review queue is empty.\n";
        return;
    }

    cout << left
         << setw(6)  << "Pos"
         << setw(8)  << "TxnID"
         << setw(8)  << "Card"
         << setw(12) << "Amount"
         << setw(7)  << "Score"
         << setw(16) << "Status"
         << "Time\n";
    cout << string(78, '-') << '\n';

    for (int p = 0; p < count; p++)
    {
        const QueueItem& q = items[(frontIdx + p) % DEQUE_CAPACITY];

        string tag = (p == 0) ? "FRONT" : (p == count - 1 ? "REAR" : to_string(p));

        cout << left
             << setw(6)  << tag
             << setw(8)  << q.id
             << setw(8)  << q.cardNo
             << setw(12) << fixed << setprecision(2) << q.amount
             << setw(7)  << q.score
             << setw(16) << q.status
             << q.timestamp << '\n';
    }

    cout.unsetf(ios::floatfield);
    cout << setprecision(6);
}

// ------------------------------------------------------------
// Persistence (front -> rear order)
// ------------------------------------------------------------

bool TransactionDeque::save(const string& file) const
{
    ostringstream out;

    out << setprecision(15);   // FIX: amounts were rounded to 6 significant digits
    out << "TransactionID,CardID,Amount,Score,Status,Timestamp\n";

    for (int p = 0; p < count; p++)
    {
        const QueueItem& q = items[(frontIdx + p) % DEQUE_CAPACITY];

        out << q.id << ',' << q.cardNo << ',' << q.amount << ','
            << q.score << ',' << q.status << ',' << q.timestamp << '\n';
    }

    return writeFileAtomically(file, out.str());
}

bool TransactionDeque::load(const string& file)
{
    clear();

    ifstream in(file);
    if (!in.is_open()) return true;        // nothing saved yet

    string line;
    getline(in, line);                     // header

    while (getline(in, line))
    {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;

        stringstream ss(line);
        string f[6];
        for (int i = 0; i < 5; i++) getline(ss, f[i], ',');
        getline(ss, f[5]);                 // timestamp contains a space

        try
        {
            QueueItem q;
            q.id        = stoi(f[0]);
            q.cardNo    = stoi(f[1]);
            q.amount    = stod(f[2]);
            q.score     = stoi(f[3]);
            q.status    = f[4];
            q.timestamp = f[5];

            if (!insertRear(q)) break;     // file bigger than capacity
        }
        catch (...)
        {
            cout << "Warning: invalid review-queue record skipped.\n";
        }
    }
    return true;
}

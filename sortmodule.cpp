#include <iostream>
#include <iomanip>

#include "sortmodule.h"
#include "time_module.h"

using namespace std;

// ------------------------------------------------------------
// Key extraction: everything is compared as a double so one merge
// sort works for time, amount and score.
// ------------------------------------------------------------
static double keyOf(const Transaction& t, SortField field)
{
    switch (field)
    {
        case SORT_BY_TIME:
        {
            long long epoch = 0;
            if (!parseTimestamp(t.timestamp, epoch)) return 0.0;
            return static_cast<double>(epoch);
        }
        case SORT_BY_AMOUNT:
            return t.amount;

        case SORT_BY_RISK:
        default:
            return static_cast<double>(t.finalScore);
    }
}

// true if `a` must come BEFORE `b`.
// Equal keys return false for both directions, so the merge below
// (which takes from the left on ties) keeps the sort stable.
static bool comesBefore(const Transaction& a, const Transaction& b,
                        SortField field, SortOrder order)
{
    double ka = keyOf(a, field);
    double kb = keyOf(b, field);

    return (order == ASCENDING) ? (ka < kb) : (ka > kb);
}

// ------------------------------------------------------------
// MERGE SORT on indices [lo, hi)
// ------------------------------------------------------------
static void mergeParts(vector<Transaction>& v, vector<Transaction>& tmp,
                       size_t lo, size_t mid, size_t hi,
                       SortField field, SortOrder order)
{
    size_t i = lo, j = mid, k = lo;

    while (i < mid && j < hi)
    {
        // Take from the right only if it is strictly better -> stable.
        if (comesBefore(v[j], v[i], field, order))
            tmp[k++] = v[j++];
        else
            tmp[k++] = v[i++];
    }

    while (i < mid) tmp[k++] = v[i++];
    while (j < hi)  tmp[k++] = v[j++];

    for (size_t x = lo; x < hi; x++) v[x] = tmp[x];
}

static void mergeSortRange(vector<Transaction>& v, vector<Transaction>& tmp,
                           size_t lo, size_t hi,
                           SortField field, SortOrder order)
{
    if (hi - lo < 2) return;

    size_t mid = lo + (hi - lo) / 2;

    mergeSortRange(v, tmp, lo, mid, field, order);
    mergeSortRange(v, tmp, mid, hi, field, order);
    mergeParts(v, tmp, lo, mid, hi, field, order);
}

vector<Transaction> sortTransactions(const vector<Transaction>& list,
                                     SortField field, SortOrder order)
{
    vector<Transaction> result = list;       // work on a copy
    vector<Transaction> tmp(result.size());

    mergeSortRange(result, tmp, 0, result.size(), field, order);

    return result;
}

vector<Transaction> topRiskTransactions(const vector<Transaction>& list,
                                        size_t n)
{
    vector<Transaction> sorted = sortTransactions(list, SORT_BY_RISK, DESCENDING);

    if (n < sorted.size()) sorted.resize(n);

    return sorted;
}

void displaySortedTable(const vector<Transaction>& list, const char* title)
{
    cout << "\n========== " << title << " ==========\n";

    if (list.empty())
    {
        cout << "No transactions to show.\n";
        return;
    }

    cout << left
         << setw(8)  << "TxnID"
         << setw(8)  << "Card"
         << setw(12) << "Amount"
         << setw(21) << "Time"
         << setw(7)  << "Score"
         << setw(16) << "Status"
         << "Decision\n";
    cout << string(80, '-') << '\n';

    for (const Transaction& t : list)
    {
        cout << left
             << setw(8)  << t.id
             << setw(8)  << t.cardNo
             << setw(12) << fixed << setprecision(2) << t.amount
             << setw(21) << t.timestamp
             << setw(7)  << t.finalScore
             << setw(16) << t.fraudStatus
             << t.decision << '\n';
    }

    cout.unsetf(ios::floatfield);
    cout << setprecision(6);
}

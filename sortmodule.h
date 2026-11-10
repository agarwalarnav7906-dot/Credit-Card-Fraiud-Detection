#ifndef SORTMODULE_H
#define SORTMODULE_H

#include <vector>
#include "transaction.h"

// ---------------------------------------------------------------
// SORTING MODULE
// Sorts a list of transactions by time, amount or risk score.
// Uses a hand-written MERGE SORT (stable, O(n log n)) so that
// transactions with equal keys keep their original order.
// The original vector is never modified; a sorted copy is returned.
// ---------------------------------------------------------------

enum SortField
{
    SORT_BY_TIME   = 1,   // Transaction::timestamp (parsed to epoch seconds)
    SORT_BY_AMOUNT = 2,   // Transaction::amount
    SORT_BY_RISK   = 3    // Transaction::finalScore
};

enum SortOrder
{
    ASCENDING  = 1,
    DESCENDING = 2
};

// Returns a sorted copy of `list`.
std::vector<Transaction> sortTransactions(const std::vector<Transaction>& list,
                                          SortField field,
                                          SortOrder order = ASCENDING);

// Returns the N riskiest transactions (highest finalScore first).
// If n > list size, everything is returned.
std::vector<Transaction> topRiskTransactions(const std::vector<Transaction>& list,
                                             size_t n);

// Prints a compact table of transactions.
void displaySortedTable(const std::vector<Transaction>& list,
                        const char* title);

#endif

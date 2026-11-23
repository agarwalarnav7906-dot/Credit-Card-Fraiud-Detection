#ifndef TRANSACTION_HISTORY_H
#define TRANSACTION_HISTORY_H

#include <string>
#include <vector>
#include "transaction.h"

// Persistent transaction database (transactions.csv)
const std::string TRANSACTION_FILE = "transactions.csv";

// Loads every saved transaction. Missing file = empty history (returns true).
bool loadTransactions(std::vector<Transaction>& transactions,
                      const std::string& file = TRANSACTION_FILE);

// Appends one transaction to the CSV (creates the file + header if needed).
bool appendTransaction(const Transaction& t,
                       const std::string& file = TRANSACTION_FILE);

bool transactionIdExists(const std::vector<Transaction>& transactions, int id);

void displayTransactions(const std::vector<Transaction>& transactions);
void findByCardID(const std::vector<Transaction>& transactions, int cardID);

#endif

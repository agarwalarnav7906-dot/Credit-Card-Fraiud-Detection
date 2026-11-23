#ifndef DEQUEMODULE_H
#define DEQUEMODULE_H

#include <string>
#include "transaction.h"

// ---------------------------------------------------------------
// DEQUE MODULE (double-ended queue) - fraud REVIEW queue
//
// Implemented as a circular array, so all four end-operations are O(1):
//     insertFront / insertRear / deleteFront / deleteRear
//
// How the fraud system uses it:
//   * CRITICAL FRAUD (score >= 70)  -> insertFront  (reviewed first)
//   * HIGH RISK / SUSPICIOUS        -> insertRear   (normal FIFO order)
//   * NORMAL transactions           -> not queued
//   * Analyst reviews with deleteFront (most urgent first).
//   * If the deque is FULL and a CRITICAL case arrives, the item at the
//     rear (least urgent / newest) is dropped to make room.
//
// The queue is saved to review_queue.csv so it survives restarts.
// ---------------------------------------------------------------

const int         DEQUE_CAPACITY    = 20;
const std::string REVIEW_QUEUE_FILE = "review_queue.csv";

struct QueueItem
{
    int         id = 0;
    int         cardNo = 0;
    double      amount = 0.0;
    int         score = 0;
    std::string status;       // SUSPICIOUS / HIGH RISK / CRITICAL FRAUD
    std::string timestamp;
};

class TransactionDeque
{
public:
    TransactionDeque();

    // ---- the four double-ended operations ----
    bool insertFront(const QueueItem& item);
    bool insertRear(const QueueItem& item);
    bool deleteFront(QueueItem& removed);
    bool deleteRear(QueueItem& removed);

    // ---- look without removing ----
    bool peekFront(QueueItem& item) const;
    bool peekRear(QueueItem& item) const;

    bool isEmpty() const { return count == 0; }
    bool isFull()  const { return count == DEQUE_CAPACITY; }
    int  size()    const { return count; }
    void clear();

    // Element at position 0..size()-1 counted from the front.
    bool at(int position, QueueItem& item) const;

    // ---- fraud-system routing ----
    // Decides front / rear / ignore from the transaction's score.
    // Returns true if the transaction was placed in the deque.
    // `evicted` is filled (and evictedAny set) if a rear item was dropped.
    bool enqueueForReview(const Transaction& t,
                          bool* evictedAny = nullptr,
                          QueueItem* evicted = nullptr);

    // ---- display / persistence ----
    void display() const;
    bool save(const std::string& file = REVIEW_QUEUE_FILE) const;
    bool load(const std::string& file = REVIEW_QUEUE_FILE);

private:
    QueueItem items[DEQUE_CAPACITY];
    int frontIdx;    // index of the front element
    int count;       // number of elements stored
};

// Builds a QueueItem from a finished transaction.
QueueItem makeQueueItem(const Transaction& t);

#endif

#ifndef HASHMODULE_H
#define HASHMODULE_H

#include <string>
#include <vector>

using namespace std;

struct Card
{
    int cardNo;
    string name;
    string accountType;
    string status;
    int points;
};

void loadCards();

bool addCard(
    int cardNo,
    string name,
    string accountType,
    string status,
    int points = 0
);

bool searchCard(int cardNo, Card &result);

bool updatePoints(int cardNo, int points);

void displayAllCards();

// Every card in the table (order = hash-table order). Used by the REST API.
vector<Card> getAllCards();

#endif
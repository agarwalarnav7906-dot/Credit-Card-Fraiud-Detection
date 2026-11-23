#include <iostream>
#include <fstream>
#include "atomic_file.h"
#include <sstream>
#include <string>
#include <vector>
#include "hashmodule.h"

using namespace std;

const int TABLE_SIZE = 100;

Card t[TABLE_SIZE];

/* Track which hash-table positions are occupied */
bool occupied[TABLE_SIZE] = {false};


/* =========================
   HASH FUNCTION
   ========================= */

int getIndex(int cardNo)
{
    // Negative IDs would give a negative index (out-of-bounds), so wrap them
    return ((cardNo % TABLE_SIZE) + TABLE_SIZE) % TABLE_SIZE;
}


/* =========================
   SAVE ENTIRE DATABASE
   ========================= */

bool saveAllCards()
{
    ostringstream file;

    file << "CardID,CardHolder,AccountType,Status,Points\n";

    for (int i = 0; i < TABLE_SIZE; i++)
    {
        if (occupied[i])
        {
            file << t[i].cardNo << ","
                 << t[i].name << ","
                 << t[i].accountType << ","
                 << t[i].status << ","
                 << t[i].points << "\n";
        }
    }

    if (!writeFileAtomically("cards.csv", file.str()))
    {
        cout << "Error: Could not save cards.csv\n";
        return false;
    }

    return true;
}


/* =========================
   LOAD CARDS FROM CSV
   ========================= */

void loadCards()
{
    ifstream file("cards.csv");

    if (!file.is_open())
    {
        cout << "Error: cards.csv not found.\n";
        return;
    }

    string line;

    // Skip header
    getline(file, line);

    while (getline(file, line))
    {
        if (line.empty())
            continue;

        stringstream ss(line);

        string cardID;
        string name;
        string accountType;
        string status;
        string points;

        getline(ss, cardID, ',');
        getline(ss, name, ',');
        getline(ss, accountType, ',');
        getline(ss, status, ',');
        getline(ss, points, ',');

        try
        {
            int cardNo = stoi(cardID);
            int pointValue = stoi(points);

            int index = getIndex(cardNo);

            // FIX: the old "while (occupied[index])" probe never ended once
            // the table was full (more than TABLE_SIZE rows in cards.csv), and
            // a repeated CardID was stored twice. Probe at most TABLE_SIZE
            // slots and skip duplicates.
            bool duplicate = false;
            int  probes = 0;

            while (occupied[index] && probes < TABLE_SIZE)
            {
                if (t[index].cardNo == cardNo) { duplicate = true; break; }
                index = (index + 1) % TABLE_SIZE;
                probes++;
            }

            if (duplicate)
            {
                cout << "Warning: duplicate card " << cardNo << " skipped.\n";
                continue;
            }

            if (occupied[index])
            {
                cout << "Warning: card table full, remaining cards skipped.\n";
                break;
            }

            t[index].cardNo = cardNo;
            t[index].name = name;
            t[index].accountType = accountType;
            t[index].status = status;
            t[index].points = pointValue;

            occupied[index] = true;
        }
        catch (...)
        {
            cout << "Warning: Invalid record skipped.\n";
        }
    }

    file.close();
}


/* =========================
   ADD CARD
   ========================= */

   bool addCard(
    int cardNo,
    string name,
    string accountType,
    string status,
    int points
)
{
    if (cardNo <= 0)
    {
        cout << "Card ID must be a positive number.\n";
        return false;
    }

    // Check if card already exists
    Card existing;

    if (searchCard(cardNo, existing))
    {
        cout << "Card already exists.\n";
        return false;
    }

    int index = getIndex(cardNo);

    // Linear probing
    for (int i = 0; i < TABLE_SIZE; i++)
    {
        if (!occupied[index])
        {
            t[index].cardNo = cardNo;
            t[index].name = name;
            t[index].accountType = accountType;
            t[index].status = status;
            t[index].points = points;

            occupied[index] = true;

            // FIX: the result of saveAllCards() was ignored, so a failed write
            // reported success while the card existed only in memory (and
            // vanished on restart). Roll back and report the failure.
            if (!saveAllCards())
            {
                occupied[index] = false;
                return false;
            }

            cout << "Card added successfully.\n";
            return true;
        }

        index = (index + 1) % TABLE_SIZE;
    }

    cout << "Hash table is full.\n";
    return false;
}


/* =========================
   SEARCH CARD
   ========================= */

bool searchCard(int cardNo, Card &result)
{
    int index = getIndex(cardNo);

    for (int i = 0; i < TABLE_SIZE; i++)
    {
        int current = (index + i) % TABLE_SIZE;

        if (!occupied[current])
            return false;

        if (t[current].cardNo == cardNo)
        {
            result = t[current];
            return true;
        }
    }

    return false;
}


/* =========================
   UPDATE POINTS
   ========================= */

   bool updatePoints(int cardNo, int points)
   {
       int index = getIndex(cardNo);
   
       for (int i = 0; i < TABLE_SIZE; i++)
       {
           int current = (index + i) % TABLE_SIZE;
   
           if (!occupied[current])
               return false;
   
           if (t[current].cardNo == cardNo)
           {
               // Update memory
               int oldPoints = t[current].points;
               t[current].points = points;
   
               // Save updated points permanently
               if (!saveAllCards())
               {
                   // keep memory and file in agreement
                   t[current].points = oldPoints;
                   return false;
               }
   
               cout << "Points updated successfully.\n";
   
               return true;
           }
       }
   
       cout << "Card not found.\n";
       return false;
   }


/* =========================
   DISPLAY ALL CARDS
   ========================= */

void displayAllCards()
{
    cout << "\n========== CARD DATABASE ==========\n";

    for (int i = 0; i < TABLE_SIZE; i++)
    {
        if (occupied[i])
        {
            cout << "\nCard ID      : " << t[i].cardNo << endl;
            cout << "Card Holder  : " << t[i].name << endl;
            cout << "Account Type : " << t[i].accountType << endl;
            cout << "Status       : " << t[i].status << endl;
            cout << "Points       : " << t[i].points << endl;
        }
    }
}


/* =========================
   LIST ALL CARDS (added for the REST API: GET /cards)
   ========================= */

vector<Card> getAllCards()
{
    vector<Card> all;

    for (int i = 0; i < TABLE_SIZE; i++)
        if (occupied[i]) all.push_back(t[i]);

    return all;
}

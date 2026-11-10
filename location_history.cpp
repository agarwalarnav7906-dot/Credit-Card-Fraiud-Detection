#include <iostream>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

#include "location_history.h"
#include "atomic_file.h"
#include "latlong.h"

using namespace std;

const string HISTORY_FILE = "location_history.csv";


bool getPreviousLocation(
    int cardID,
    PreviousLocation& previous)
{
    ifstream file(HISTORY_FILE);

    if (!file.is_open())
    {
        previous.found = false;
        return false;
    }

    string line;

    // Skip header
    getline(file, line);

    while (getline(file, line))
    {
        if (line.empty())
            continue;

        stringstream ss(line);

        string id;
        string location;
        string latitude;
        string longitude;
        string timestamp;

        getline(ss, id, ',');
        getline(ss, location, ',');
        getline(ss, latitude, ',');
        getline(ss, longitude, ',');
        getline(ss, timestamp, ',');

        try
        {
            if (stoi(id) == cardID)
            {
                previous.found = true;
                previous.location = location;
                previous.latitude = stod(latitude);
                previous.longitude = stod(longitude);
                previous.timestamp = stoll(timestamp);

                file.close();

                return true;
            }
        }
        catch (...)
        {
            continue;
        }
    }

    file.close();

    previous.found = false;

    return false;
}


bool saveLocation(
    int cardID,
    const string& location,
    double latitude,
    double longitude,
    long long timestamp)
{
    /*
        Read all existing records.
        If the card already exists, replace its record.
        Otherwise add a new record.
    */

    vector<PreviousLocation> records;
    vector<int> cardIDs;

    ifstream input(HISTORY_FILE);

    if (input.is_open())
    {
        string line;

        // Skip header
        getline(input, line);

        while (getline(input, line))
        {
            if (line.empty())
                continue;

            stringstream ss(line);

            string id;
            string loc;
            string lat;
            string lon;
            string time;

            getline(ss, id, ',');
            getline(ss, loc, ',');
            getline(ss, lat, ',');
            getline(ss, lon, ',');
            getline(ss, time, ',');

            try
            {
                PreviousLocation record;

                int currentCard = stoi(id);

                record.found = true;
                record.location = loc;
                record.latitude = stod(lat);
                record.longitude = stod(lon);
                record.timestamp = stoll(time);

                cardIDs.push_back(currentCard);
                records.push_back(record);
            }
            catch (...)
            {
                continue;
            }
        }

        input.close();
    }


    // Check if card already exists
    bool updated = false;

    for (size_t i = 0; i < cardIDs.size(); i++)
    {
        if (cardIDs[i] == cardID)
        {
            records[i].location = location;
            records[i].latitude = latitude;
            records[i].longitude = longitude;
            records[i].timestamp = timestamp;

            updated = true;
            break;
        }
    }


    // New card
    if (!updated)
    {
        PreviousLocation record;

        record.found = true;
        record.location = location;
        record.latitude = latitude;
        record.longitude = longitude;
        record.timestamp = timestamp;

        cardIDs.push_back(cardID);
        records.push_back(record);
    }


    // Rewrite file
    ostringstream output;

    output << "CardID,Location,Latitude,Longitude,Timestamp\n";

    for (size_t i = 0; i < records.size(); i++)
    {
        output << cardIDs[i] << ","
               << toUpperCase(records[i].location) << ","
               << records[i].latitude << ","
               << records[i].longitude << ","
               << records[i].timestamp << "\n";
    }

    if (!writeFileAtomically(HISTORY_FILE, output.str()))
    {
        cout << "ERROR: Could not save location history.\n";
        return false;
    }

    return true;
}
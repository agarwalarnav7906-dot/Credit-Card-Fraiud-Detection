#ifndef LOCATION_HISTORY_H
#define LOCATION_HISTORY_H

#include <string>

using namespace std;

struct PreviousLocation
{
    bool found;
    string location;
    double latitude;
    double longitude;
    long long timestamp;
};

bool getPreviousLocation(int cardID, PreviousLocation& previous);

bool saveLocation(
    int cardID,
    const string& location,
    double latitude,
    double longitude,
    long long timestamp
);

#endif

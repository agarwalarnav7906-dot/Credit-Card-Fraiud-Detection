#include <iostream>
#include "transaction_input.h"
#include "latlong.h"
#include "time_module.h"

using namespace std;

bool inputTransaction(Transaction& t)
{
    cout << "\n========== NEW TRANSACTION ==========\n";

    cout << "Transaction ID: ";
    cin >> t.id;

    cout << "Card ID: ";
    cin >> t.cardNo;

    cout << "Amount: ";
    cin >> t.amount;

    cout << "Merchant: ";
    cin >> t.merchant;

    cout << "Location: ";
    cin >> t.location;
    t.location = toUpperCase(t.location);

    cout << "Device ID: ";
    cin >> t.deviceId;

    Location locationData;
    if (!getLocation(t.location, locationData)) {
        cout << "ERROR: Location not found in locations.csv.\n";
        return false;
    }

    t.latitude = locationData.latitude;
    t.longitude = locationData.longitude;
    t.timestamp = getCurrentTimestamp();

    cout << "Coordinates: " << t.latitude << ", " << t.longitude << '\n';
    cout << "Automatic timestamp: " << t.timestamp << '\n';

    return true;
}

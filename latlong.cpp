#include <fstream>
#include <sstream>
#include <string>
#include <cmath>
#include <cctype>
#include "latlong.h"

using namespace std;

const double PI = 3.14159265358979323846;
const double EARTH_RADIUS = 6371.0;

string toUpperCase(const string& text)
{
    string s = text;
    while (!s.empty() && isspace(static_cast<unsigned char>(s.back())))
        s.pop_back();
    size_t start = 0;
    while (start < s.size() && isspace(static_cast<unsigned char>(s[start])))
        start++;
    s = s.substr(start);
    for (char& c : s)
        c = static_cast<char>(toupper(static_cast<unsigned char>(c)));
    return s;
}

bool getLocation(const string& locationName, Location& location)
{
    ifstream file("locations.csv");
    if (!file.is_open()) return false;

    string line;
    getline(file, line); // header

    while (getline(file, line)) {
        stringstream ss(line);
        string name, latitude, longitude;
        getline(ss, name, ',');
        getline(ss, latitude, ',');
        getline(ss, longitude, ',');

        if (toUpperCase(name) == toUpperCase(locationName)) {
            try {
                location.name = toUpperCase(name);
                location.latitude = stod(latitude);
                location.longitude = stod(longitude);
            } catch (...) {
                return false;
            }
            return true;
        }
    }
    return false;
}

bool loadAllLocations(vector<Location>& all)
{
    all.clear();

    ifstream file("locations.csv");
    if (!file.is_open()) return false;

    string line;
    getline(file, line); // header

    while (getline(file, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty()) continue;

        stringstream ss(line);
        string name, latitude, longitude;
        getline(ss, name, ',');
        getline(ss, latitude, ',');
        getline(ss, longitude, ',');

        try {
            Location loc;
            loc.name = name;
            loc.latitude = stod(latitude);
            loc.longitude = stod(longitude);
            all.push_back(loc);
        } catch (...) {
            // skip malformed row
        }
    }
    return true;
}

double toRadians(double degree)
{
    return degree * PI / 180.0;
}

double calculateDistance(double lat1, double lon1, double lat2, double lon2)
{
    lat1 = toRadians(lat1);
    lon1 = toRadians(lon1);
    lat2 = toRadians(lat2);
    lon2 = toRadians(lon2);

    double dLat = lat2 - lat1;
    double dLon = lon2 - lon1;

    double a = sin(dLat / 2) * sin(dLat / 2) +
               cos(lat1) * cos(lat2) *
               sin(dLon / 2) * sin(dLon / 2);

    double c = 2 * atan2(sqrt(a), sqrt(1 - a));
    return EARTH_RADIUS * c;
}

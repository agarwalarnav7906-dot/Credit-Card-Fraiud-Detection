#ifndef LATLONG_H
#define LATLONG_H

#include <string>
#include <vector>

struct Location {
    std::string name;
    double latitude = 0.0;
    double longitude = 0.0;
};

// Trims spaces and converts to UPPER CASE (used for all city names)
std::string toUpperCase(const std::string& text);

bool getLocation(const std::string& locationName, Location& location);

// Reads every row of locations.csv (used by the location graph).
bool loadAllLocations(std::vector<Location>& all);
double calculateDistance(double lat1, double lon1, double lat2, double lon2);

#endif

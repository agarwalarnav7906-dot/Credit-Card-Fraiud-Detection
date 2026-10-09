#ifndef TIME_MODULE_H
#define TIME_MODULE_H

#include <string>

std::string getCurrentTimestamp();
std::string getCurrentDate();
std::string getCurrentTime();

// Converts "YYYY-MM-DD HH:MM:SS" (local time) into epoch seconds.
// Returns false if the text is not a valid timestamp.
bool parseTimestamp(const std::string& text, long long& epochSeconds);

// Converts epoch seconds into "YYYY-MM-DD HH:MM:SS" (local time).
std::string formatTimestamp(long long epochSeconds);

#endif

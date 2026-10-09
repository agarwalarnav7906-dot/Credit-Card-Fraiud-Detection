#include "time_module.h"
#include <ctime>
#include <cstdio>
#include <string>

using namespace std;

string formatCurrent(const char* format)
{
    time_t timeNow = time(nullptr);

    tm* localTime = localtime(&timeNow);

    if (localTime == nullptr)
    {
        return "";
    }

    char buffer[100];

    strftime(
        buffer,
        sizeof(buffer),
        format,
        localTime
    );

    return string(buffer);
}

string getCurrentDate()
{
    return formatCurrent("%Y-%m-%d");
}

string getCurrentTime()
{
    return formatCurrent("%H:%M:%S");
}

string getCurrentTimestamp()
{
    return formatCurrent("%Y-%m-%d %H:%M:%S");
}

bool parseTimestamp(const string& text, long long& epochSeconds)
{
    int y, mo, d, h, mi, s;

    if (sscanf(text.c_str(), "%d-%d-%d %d:%d:%d",
               &y, &mo, &d, &h, &mi, &s) != 6)
    {
        return false;
    }

    tm t = {};
    t.tm_year = y - 1900;
    t.tm_mon = mo - 1;
    t.tm_mday = d;
    t.tm_hour = h;
    t.tm_min = mi;
    t.tm_sec = s;
    t.tm_isdst = -1;

    time_t value = mktime(&t);

    if (value == static_cast<time_t>(-1))
    {
        return false;
    }

    epochSeconds = static_cast<long long>(value);
    return true;
}

string formatTimestamp(long long epochSeconds)
{
    time_t value = static_cast<time_t>(epochSeconds);
    tm* localTime = localtime(&value);

    if (localTime == nullptr)
    {
        return "";
    }

    char buffer[100];
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", localTime);

    return string(buffer);
}

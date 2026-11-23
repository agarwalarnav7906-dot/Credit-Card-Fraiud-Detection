#include "time_module.h"
#include <ctime>
#include <cstdio>
#include <string>

using namespace std;

// FIX (integration): localtime() returns a pointer to one shared static buffer,
// so two HTTP worker threads formatting a timestamp at the same time could
// read each other's result. Use the reentrant variant on each platform.
static bool safeLocalTime(time_t value, tm& out)
{
#ifdef _WIN32
    return localtime_s(&out, &value) == 0;
#else
    return localtime_r(&value, &out) != nullptr;
#endif
}

string formatCurrent(const char* format)
{
    time_t timeNow = time(nullptr);

    tm localTime = {};

    if (!safeLocalTime(timeNow, localTime))
    {
        return "";
    }

    char buffer[100];

    strftime(
        buffer,
        sizeof(buffer),
        format,
        &localTime
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
    tm localTime = {};

    if (!safeLocalTime(value, localTime))
    {
        return "";
    }

    char buffer[100];
    strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &localTime);

    return string(buffer);
}

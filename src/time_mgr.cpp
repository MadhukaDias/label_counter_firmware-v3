#include "time_mgr.h"
#include "config.h"
#include <time.h>

// Must run after networkInit(): SNTP needs the LwIP stack. It syncs as soon
// as the station connects and every hour after that (SNTP default interval).
void timeInit(){configTzTime(TIME_TZ,NTP_SERVER_1,NTP_SERVER_2);}
// Before the first sync the clock starts at 1970; anything after 2024 is real.
bool timeSynced(){return time(nullptr)>1704067200;}
void timeFormatClock(char* out,size_t n){
    if(!timeSynced()){snprintf(out,n,"--:--");return;}
    time_t now=time(nullptr);struct tm local;localtime_r(&now,&local);
    strftime(out,n,"%H:%M:%S",&local);
}

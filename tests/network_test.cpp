#include "../src/network_mgr.cpp"
#include <cassert>
uint32_t fakeMillis=0;int fakePins[49];bool paused=false;
void webServerPause(){assert(!paused);paused=true;}
void webServerResume(){assert(paused);paused=false;}
int main(){
 networkInit();networkLoop();assert(!networkPortalActive());
 fakeMillis=10000;networkLoop();assert(networkPortalActive()&&paused);
 assert(networkPortalIP()=="192.168.4.1");
 fakeMillis=30000;networkLoop();assert(WiFi.retries==1&&paused);
 WiFi.state=WL_CONNECTED;networkLoop();assert(!networkPortalActive()&&!paused);
 assert(networkStationIP()=="192.168.1.50");
 networkOpenPortal();networkLoop();assert(networkPortalActive()&&paused);
 manager.closeOnProcess=true;networkLoop();assert(!paused);
 WiFi.state=0;fakeMillis=31000;networkLoop();assert(!networkPortalActive());
 fakeMillis=41000;networkLoop();assert(networkPortalActive()&&paused);
 puts("PASS: network controller: fallback, retry, recovery, port ownership, manual portal");
}

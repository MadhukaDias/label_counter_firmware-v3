#pragma once
struct WiFiManager{
 bool active=false,closeOnProcess=false;
 void setConfigPortalBlocking(bool){}void setConfigPortalTimeout(int){}
 void setConnectTimeout(int){}void setSaveConnectTimeout(int){}void setBreakAfterConfig(bool){}
 void startConfigPortal(const char*,const char*){active=true;}
 bool getConfigPortalActive(){return active;}
 void stopConfigPortal(){active=false;}
 void process(){if(closeOnProcess){active=false;closeOnProcess=false;}}
};

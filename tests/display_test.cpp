#include "../src/display_mgr.cpp"
#include <cassert>
uint32_t fakeMillis=0;int fakePins[49];
int main(){
 displayInit();fakeMillis=2000;displayUpdateClock("12:34");
 displayUpdateNetwork(true,true,true,"192.168.100.100",false,"");
 displayShowRunning();
 for(uint8_t s=0;s<5;s++)for(uint32_t n:{0u,1234u,99999u,UINT32_MAX}){
  displayUpdateMachine(s,0);displayUpdateCount(n);displayRender();
 }
 int before=tft.transfers;displayRender();assert(tft.transfers==before);
 for(uint8_t s=1;s<8;s++){
  displayShowCalibration(s,3,100,0,2);displayRender();
  displayShowCalibration(s,3,150,3,2);displayRender();
 }
 displayUpdateNetwork(false,true,false,"",true,"192.168.4.1");
 displayShowPortal("LabelCounter","192.168.4.1");displayRender();
 for(int i=0;i<3;i++){displayShowMenu(i);displayRender();}
 displayShowMenu(2,true);displayRender();
 displayShowConnecting("Saved network");displayRender();
 displayShowError("Test fault","Check wiring");displayRender();
 puts("PASS: all TFT screen text/rect bounds, 10-digit count, unchanged-frame suppression");
}

// Runs the actual main.cpp button handlers with simulated time and GPIO inputs.
#include "../src/main.cpp"
#include <cassert>
uint32_t fakeMillis=0;
int fakePins[49];
void displayShowMessage(const char*,const char*){}
void cfgSave(const AppConfig&){}
void cfgSaveCount(uint32_t){}
bool mqttIsConnected(){return false;}
void mqttPublishEventStr(const char*,const char*,const char*){}
void mqttPublishWaveform(uint32_t,const char*,const uint32_t*,uint16_t,const char*){}
uint32_t imuGetLastVibStart(){return 0;}
void networkOpenPortal(){}
void tick(uint32_t t){fakeMillis=t;handleButtons();}
void click(int pin,uint32_t at){fakePins[pin]=LOW;tick(at);tick(at+50);fakePins[pin]=HIGH;tick(at+100);tick(at+150);}
int main(){
    for(int &pin:fakePins)pin=HIGH;
    appCfg.count=10;
    click(PIN_BTN_INC,100);assert(appCfg.count==11);
    click(PIN_BTN_DEC,300);assert(appCfg.count==10);
    appCfg.count=0;click(PIN_BTN_DEC,500);assert(appCfg.count==0);
    appCfg.count=UINT32_MAX;click(PIN_BTN_INC,700);assert(appCfg.count==UINT32_MAX);
    appCfg.count=20;
    // Chord starts calibration and staggered releases cannot increment/decrement.
    fakePins[PIN_BTN_INC]=fakePins[PIN_BTN_DEC]=LOW;tick(1000);tick(1050);tick(3050);
    assert(calibState==CALIB_SAMPLING);
    fakePins[PIN_BTN_INC]=HIGH;tick(3100);tick(3150);
    fakePins[PIN_BTN_DEC]=HIGH;tick(3200);tick(3250);assert(appCfg.count==20);
    // BACK long press aborts calibration, release does not toggle views.
    fakePins[PIN_BTN_BACK]=LOW;tick(3300);tick(3350);tick(5350);
    assert(calibState==CALIB_IDLE&&appCfg.lastCalibStatus==2);
    fakePins[PIN_BTN_BACK]=HIGH;tick(5400);tick(5450);assert(!networkView);
    // Menu -> reset requires an explicit second confirmation.
    click(PIN_BTN_SELECT,5600);assert(menuOpen);
    click(PIN_BTN_DEC,5800);click(PIN_BTN_DEC,6000);assert(menuSelection==2);
    click(PIN_BTN_SELECT,6200);assert(resetConfirm&&appCfg.count==20);
    click(PIN_BTN_BACK,6400);assert(!resetConfirm&&appCfg.count==20);
    click(PIN_BTN_SELECT,6600);click(PIN_BTN_SELECT,6800);assert(!menuOpen&&appCfg.count==0);
    // Calibration SELECT capture and DEC undo never alter production count.
    calibState=CALIB_SEW_WAITING;click(PIN_BTN_SELECT,7000);assert(calibState==CALIB_SEW_DONE);
    calibState=CALIB_FINE_TUNE;fineTuneCount=3;click(PIN_BTN_DEC,7200);assert(fineTuneCount==2&&appCfg.count==0);
    puts("PASS: real handlers: +/- limits, chord releases, abort, menu/reset confirmation, capture/undo");
}

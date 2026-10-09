#include "display_mgr.h"
#include "config.h"
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_ST7735.h>
#include <cstring>
#include <algorithm>

namespace {
constexpr int W=160, H=128;
constexpr uint16_t BLACK=0x0000, WHITE=0xffff, GRAY=0x9cd3,
                   GREEN=0x07e0, AMBER=0xfd20, RED=0xf800, LINE=0x4208;
Adafruit_ST7735 tft(TFT_CS,TFT_DC,TFT_RST);
GFXcanvas16 canvas(W,H);
uint16_t previous[W*H];
bool ready=false, first=true, notificationError=false;
enum Mode { RUN, PORTAL, CONNECTING, CALIBRATION, MENU, MESSAGE, ERROR_SCREEN };
Mode mode=RUN;
uint32_t count=0, messageUntil=0;
bool wifi=false, mqttEnabled=true, mqtt=false, portal=false, confirm=false;
uint8_t machine=0, calStatus=0, calStage=0, captures=0, imu=0, locks=0, selection=0;
uint16_t progress=0;
char stationIP[20]="", portalIP[20]="", ap[33]="LabelCounter", msg1[40]="", msg2[40]="", clock_[9]="--:--";
void copy(char* out,size_t n,const char* in){snprintf(out,n,"%s",in?in:"");}
void text(int x,int y,const char* s,uint16_t color=WHITE,uint8_t size=1){
    canvas.setTextSize(size); canvas.setTextColor(color); canvas.setCursor(x,y); canvas.print(s);
}
void center(int y,const char* s,uint16_t color=WHITE,uint8_t size=1){
    int width=strlen(s)*6*size; text(std::max(0,(W-width)/2),y,s,color,size);
}
void header(const char* title,uint16_t color){
    text(4,3,title,color);
    text(W-4-int(strlen(clock_))*6,3,clock_,WHITE);
    text(4,17,wifi?"WiFi OK":"WiFi LOST",wifi?GREEN:RED);
    const char* status=!mqttEnabled?"MQTT OFF":mqtt?"MQTT OK":"MQTT LOST";
    text(94,17,status,!mqttEnabled?GRAY:mqtt?GREEN:RED);
    canvas.drawFastHLine(4,29,152,LINE);
}
void footerIP(){
    canvas.drawFastHLine(4,112,152,LINE);
    char line[27];
    if (portal) snprintf(line,sizeof(line),"AP %s",portalIP);
    else if(wifi) snprintf(line,sizeof(line),"IP %s",stationIP);
    else copy(line,sizeof(line),"WiFi reconnecting...");
    center(118,line,portal?AMBER:GRAY);
}
const char* machineName(){
    switch(machine){case 1:return "DETECTING";case 2:return "SEWING";case 3:return "CONFIRMING";case 4:return "EXCEEDING";default:return "IDLE";}
}
}
void displayInit(){
    if(!canvas.getBuffer()){Serial.println("[TFT] Framebuffer allocation failed");return;}
    SPI.begin(TFT_SCLK,-1,TFT_MOSI,TFT_CS);
    tft.initR(TFT_TAB); tft.setRotation(TFT_ROTATION); tft.setSPISpeed(TFT_SPI_HZ);
    canvas.setTextWrap(false); ready=true;
    displayShowMessage("LABEL COUNTER","Idea8 - TFT edition");
    displayRender();
}
void displayUpdateCount(uint32_t value){count=value;}
void displayUpdateNetwork(bool w,bool enabled,bool connected,const char* ip,bool active,const char* apIP){
    wifi=w; mqttEnabled=enabled; mqtt=w&&enabled&&connected; portal=active;
    copy(stationIP,sizeof(stationIP),ip);copy(portalIP,sizeof(portalIP),apIP);
}
void displayUpdateClock(const char* hhmm){copy(clock_,sizeof(clock_),hhmm);}
void displayUpdateMachine(uint8_t state,uint8_t status){machine=state;calStatus=status;}
void displayShowRunning(){mode=RUN;}
void displayShowPortal(const char* name,const char* ip){mode=PORTAL;copy(ap,sizeof(ap),name);copy(portalIP,sizeof(portalIP),ip);}
void displayShowConnecting(const char* ssid){mode=CONNECTING;copy(msg1,sizeof(msg1),ssid);}
void displayShowCalibration(uint8_t state,uint8_t ft,uint16_t p,uint8_t im,uint8_t lk){mode=CALIBRATION;calStage=state;captures=ft;progress=p;imu=im;locks=lk;}
void displayShowMenu(uint8_t sel,bool reset){mode=MENU;selection=sel;confirm=reset;}
void displayShowError(const char* l1,const char* l2){mode=ERROR_SCREEN;notificationError=true;copy(msg1,sizeof(msg1),l1);copy(msg2,sizeof(msg2),l2);messageUntil=millis()+1800;displayRender();}
void displayShowMessage(const char* l1,const char* l2){mode=MESSAGE;notificationError=false;copy(msg1,sizeof(msg1),l1);copy(msg2,sizeof(msg2),l2);messageUntil=millis()+1200;displayRender();}
void displayShowCfgIP(const char* ip){copy(stationIP,sizeof(stationIP),ip);}
void displayRender(){
    if(!ready)return;
    canvas.fillScreen(BLACK);
    // Timed notifications stay visible even when the loop requests another screen.
    if(int32_t(messageUntil-millis())>0){
        header(notificationError?"ERROR":"SYSTEM",notificationError?RED:AMBER);
        center(49,msg1,WHITE);center(69,msg2,GRAY);footerIP();
    }else if(mode==RUN){
        header(machineName(),machine==4?RED:machine?AMBER:GREEN);text(4,36,"COUNT",GRAY);
        char value[11];snprintf(value,sizeof(value),"%lu",(unsigned long)count);
        uint8_t size=4;while(strlen(value)*6*size>152&&size>1)--size;
        center(54,value,WHITE,size);
        text(4,100,calStatus==1?"CAL OK":calStatus==2?"CAL ABORT":"CAL NEEDED",calStatus==1?GREEN:AMBER);
        text(94,100,machineName(),machine==4?RED:machine?GREEN:GRAY);footerIP();
    }else if(mode==PORTAL){
        header("WI-FI SETUP",AMBER);center(39,"Connect to AP",GRAY);
        // SSID may be up to 32 characters: clamp to a display-safe label.
        char label[26];snprintf(label,sizeof(label),"%.25s",ap);center(54,label);
        center(74,"Open browser",GRAY);center(89,portalIP,AMBER);
        text(4,115,"BACK: count screen",GRAY);
    }else if(mode==CONNECTING){
        header("CONNECTING",AMBER);center(48,"Saved Wi-Fi network",WHITE);
        center(67,"Counting remains active",GRAY);footerIP();
    }else if(mode==MENU){
        header(confirm?"RESET COUNT?":"MENU",AMBER);
        if(confirm){center(47,"Set count to zero?");center(75,"SELECT: confirm",AMBER);}
        else{const char* items[]={"Start calibration","Wi-Fi setup","Reset count"};
            for(int i=0;i<3;i++){if(selection==i)canvas.fillRect(3,38+i*20,154,16,LINE);text(8,42+i*20,items[i],selection==i?GREEN:WHITE);}}
        text(4,116,"SELECT: OK  BACK: cancel",GRAY);
    }else if(mode==CALIBRATION){
        header("CALIBRATION",AMBER);char line[27];
        switch(calStage){
        case 1:
            text(4,39,"1 NOISE SCAN");text(4,57,"Keep machine off",GRAY);
            canvas.drawRect(4,80,152,10,LINE);canvas.fillRect(5,81,std::min(150,int(progress))*150/150,8,AMBER);break;
        case 3:
            // Info only: `locks` carries the saved "has lock solenoid" setting.
            text(4,49,"Lock solenoid",GRAY);text(4,69,locks?"DETECTED":"NOT PRESENT",locks?GREEN:AMBER);break;
        case 4:
            text(4,39,"2 FIRST SEW");text(4,57,"Sew one label",GRAY);
            text(4,77,imu==3?"CONFIRMING...":"Auto-detecting...",GREEN);text(4,96,"SELECT / +: capture",GRAY);break;
        case 5:
            text(4,39,"3 FINE TUNE");snprintf(line,sizeof(line),"%u of 5 captured",captures);text(4,57,line);
            text(4,77,imu==3?"CONFIRMING...":"Sew next label",GREEN);text(4,96,"+:capture  -:undo",GRAY);break;
        case 6:text(4,49,"Processing data...",AMBER);break;
        case 7:text(4,49,"No lock solenoid",AMBER);text(4,69,"Skipping lock scan",GRAY);break;
        default:text(4,49,"Please wait...",GRAY);break;
        }
        text(4,116,"Hold BACK 2s: abort",GRAY);
    }else{header("SYSTEM",AMBER);center(52,msg1);center(73,msg2,GRAY);}
    // Send only changed contiguous scanlines. No clear-screen flash.
    uint16_t* pixels=canvas.getBuffer();int top=H,bottom=-1;
    for(int y=0;y<H;y++)if(first||memcmp(pixels+y*W,previous+y*W,W*2)!=0){top=std::min(top,y);bottom=y;}
    if(bottom>=top){tft.drawRGBBitmap(0,top,pixels+top*W,W,bottom-top+1);memcpy(previous+top*W,pixels+top*W,(bottom-top+1)*W*2);}
    first=false;
}

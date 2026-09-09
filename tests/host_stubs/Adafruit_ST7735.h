#pragma once
#define INITR_BLACKTAB 0
#define INITR_REDTAB 1
#define INITR_GREENTAB 2
class Adafruit_ST7735{
public:
 int transfers=0;
 Adafruit_ST7735(int,int,int){}
 void initR(int){}void setRotation(int){}void setSPISpeed(int){}
 void drawRGBBitmap(int,int,uint16_t*,int,int){++transfers;}
};

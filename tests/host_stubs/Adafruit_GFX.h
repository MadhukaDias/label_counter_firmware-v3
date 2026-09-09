#pragma once
#include <Arduino.h>
#include <vector>
#include <algorithm>
#include <cassert>
// Minimal raster surrogate for bounds/change-region tests, not a font simulator.
class GFXcanvas16{
 int w,h,x=0,y=0,size=1;uint16_t color=0;std::vector<uint16_t> b;
public:
 GFXcanvas16(int W,int H):w(W),h(H),b(W*H){}
 uint16_t* getBuffer(){return b.data();}
 void setTextWrap(bool){}void setTextSize(uint8_t v){size=v;}
 void setTextColor(uint16_t v){color=v;}void setCursor(int X,int Y){x=X;y=Y;}
 void fillScreen(uint16_t c){std::fill(b.begin(),b.end(),c);}
 void fillRect(int X,int Y,int W,int H,uint16_t c){
  assert(X>=0&&Y>=0&&X+W<=w&&Y+H<=h);
  for(int j=Y;j<Y+H;j++)for(int i=X;i<X+W;i++)b[j*w+i]=c;
 }
 void drawFastHLine(int X,int Y,int W,uint16_t c){fillRect(X,Y,W,1,c);}
 void drawRect(int X,int Y,int W,int H,uint16_t c){fillRect(X,Y,W,1,c);fillRect(X,Y+H-1,W,1,c);fillRect(X,Y,1,H,c);fillRect(X+W-1,Y,1,H,c);}
 void print(const char* s){for(;*s;s++){fillRect(x,y,6*size,8*size,color^uint8_t(*s));x+=6*size;}}
};

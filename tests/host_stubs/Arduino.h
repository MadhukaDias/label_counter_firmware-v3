#pragma once
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <string>
using String=std::string;
using byte=uint8_t;
constexpr int LOW=0,HIGH=1,INPUT_PULLUP=2;
extern uint32_t fakeMillis;
extern int fakePins[49];
inline uint32_t millis(){return fakeMillis;}
inline void delay(uint32_t ms){fakeMillis+=ms;}
inline int digitalRead(int pin){return fakePins[pin];}
inline void pinMode(int,int){}
struct SerialStub {
 void begin(int){}
 template<typename... T> void printf(const char*,T...){}
 template<typename T>void print(T){}
 template<typename T>void println(T){}
 void println(){}
};
static SerialStub Serial;

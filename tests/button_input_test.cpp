#include "button_input.h"
#include <cassert>
#include <cstdio>
int main(){
    ButtonInput b;
    b.update(true,10); b.update(false,20);b.update(true,30);
    b.update(true,79);assert(!b.down&&!b.clicked);
    b.update(true,80);assert(b.down&&!b.clicked);
    b.update(false,100);b.update(true,110);b.update(false,120);
    b.update(false,170);assert(!b.down&&b.clicked);
    b.update(false,171);assert(!b.clicked);
    b.update(true,200);b.update(true,250);b.update(true,2250);
    assert(b.longPress&&b.held);b.update(true,2251);assert(!b.longPress);
    b.update(false,2300);b.update(false,2350);assert(!b.clicked);
    ButtonInput w;w.update(true,0xfffffff0u);w.update(true,40);
    assert(w.down);w.update(false,80);w.update(false,130);assert(w.clicked);
    puts("PASS: debounce, single release, long press suppression, timer wraparound");
}

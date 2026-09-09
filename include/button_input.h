#pragma once
#include <stdint.h>
// Platform-independent debouncer, active-low at the call site.
struct ButtonInput {
    bool raw=false, down=false, held=false, clicked=false, longPress=false;
    uint32_t changedAt=0, downAt=0;
    void update(bool pressed,uint32_t now,uint32_t debounce=50,uint32_t longMs=2000){
        clicked=longPress=false;
        if(pressed!=raw){raw=pressed;changedAt=now;}
        if(raw!=down&&uint32_t(now-changedAt)>=debounce){
            down=raw;
            if(down){downAt=now;held=false;}
            else if(!held)clicked=true;
        }
        if(down&&!held&&uint32_t(now-downAt)>=longMs){held=true;longPress=true;}
    }
};

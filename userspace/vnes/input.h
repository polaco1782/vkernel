#ifndef INPUT_H
#define INPUT_H

#include "types.h"

class Input {
public:
    Input();
    
    // Controller buttons
    enum Button {
        BUTTON_A      = 0x01,
        BUTTON_B      = 0x02,
        BUTTON_SELECT = 0x04,
        BUTTON_START  = 0x08,
        BUTTON_UP     = 0x10,
        BUTTON_DOWN   = 0x20,
        BUTTON_LEFT   = 0x40,
        BUTTON_RIGHT  = 0x80
    };
    
    void setButton(Button button, bool pressed);
    void clearButtons();
    
    // Controller strobe (called when CPU writes to $4016)
    void strobe();
    
    // Read controller state (called when CPU reads $4016)
    u8 read();
    
private:
    u8 controller_state;
    u8 controller_latch;
    u8 shift_count;
};

#endif // INPUT_H

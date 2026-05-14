#include "input.h"

Input::Input()
    : controller_state(0)
    , controller_latch(0)
    , shift_count(0)
{
}

void Input::setButton(Button button, bool pressed)
{
    if (pressed) {
        controller_state |= static_cast<u8>(button);
    } else {
        controller_state &= static_cast<u8>(~static_cast<u8>(button));
    }
}

void Input::clearButtons()
{
    controller_state = 0;
}

void Input::strobe()
{
    controller_latch = controller_state;
    shift_count = 0;
}

u8 Input::read()
{
    u8 value = (controller_latch & 0x01) ? 0x01 : 0x00;

    controller_latch >>= 1;
    shift_count++;

    if (shift_count > 8) {
        value = 0x01;
    }

    return value | 0x40;
}

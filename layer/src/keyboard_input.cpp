#include "keyboard_input.hpp"

#include "logger.hpp"

#ifndef GFF_X11
#define GFF_X11 1
#endif

#if GFF_X11
#include "keyboard_input_x11.hpp"
#endif

namespace vkBasalt
{
    uint32_t convertToKeySym(std::string key)
    {
#if GFF_X11
        return convertToKeySymX11(key);
#endif
        return 0u;
    }

    bool isKeyPressed(uint32_t ks)
    {
#if GFF_X11
        return isKeyPressedX11(ks);
#endif
        return false;
    }

    KeyboardState getKeyboardState()
    {
#if GFF_X11
        return getKeyboardStateX11();
#endif
        return KeyboardState();
    }
} // namespace vkBasalt

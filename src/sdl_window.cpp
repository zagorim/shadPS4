// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <fstream>
#include <iostream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_video.h>
#include "common/assert.h"
#include "common/config.h"
#include "common/elf_info.h"
#include "common/io_file.h"
#include "common/path_util.h"
#include "common/version.h"
#include "core/libraries/pad/pad.h"
#include "imgui/renderer/imgui_core.h"
#include "input/controller.h"
#include "sdl_window.h"
#include "video_core/renderdoc.h"

#ifdef __APPLE__
#include <SDL3/SDL_metal.h>
#endif

// +1 and +2 is taken
#define SDL_EVENT_MOUSE_WHEEL_UP SDL_EVENT_MOUSE_WHEEL + 3
#define SDL_EVENT_MOUSE_WHEEL_DOWN SDL_EVENT_MOUSE_WHEEL + 4
#define SDL_EVENT_MOUSE_WHEEL_LEFT SDL_EVENT_MOUSE_WHEEL + 5
#define SDL_EVENT_MOUSE_WHEEL_RIGHT SDL_EVENT_MOUSE_WHEEL + 6

#define LEFTJOYSTICK_HALFMODE 0x00010000
#define RIGHTJOYSTICK_HALFMODE 0x00020000

Uint32 getMouseWheelEvent(const SDL_Event* event) {
    if (event->type != SDL_EVENT_MOUSE_WHEEL)
        return 0;

    // std::cout << "We got a wheel event! ";
    if (event->wheel.y > 0) {
        return SDL_EVENT_MOUSE_WHEEL_UP;
    } else if (event->wheel.y < 0) {
        return SDL_EVENT_MOUSE_WHEEL_DOWN;
    } else if (event->wheel.x > 0) {
        return SDL_EVENT_MOUSE_WHEEL_RIGHT;
    } else if (event->wheel.x < 0) {
        return SDL_EVENT_MOUSE_WHEEL_LEFT;
    }
    return 0;
}

namespace Frontend {

using Libraries::Pad::OrbisPadButtonDataOffset;

KeyBinding::KeyBinding(const SDL_Event* event) {
    modifier = SDL_GetModState();
    key = 0;

    // std::cout << "Someone called the new binding ctor!\n";
    if (event->type == SDL_EVENT_KEY_DOWN || event->type == SDL_EVENT_KEY_UP) {
        key = event->key.key;
    } else if (event->type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
               event->type == SDL_EVENT_MOUSE_BUTTON_UP) {
        key = event->button.button;
    } else if (event->type == SDL_EVENT_MOUSE_WHEEL) {
        key = getMouseWheelEvent(event);
    } else {
        std::cout << "We don't support this event type!\n";
    }
}

bool KeyBinding::operator<(const KeyBinding& other) const {
    return std::tie(key, modifier) < std::tie(other.key, other.modifier);
}

// modifiers are bitwise or-d together, so we need to check if ours is in that
template <typename T>
typename std::map<KeyBinding, T>::const_iterator FindKeyAllowingPartialModifiers(
    const std::map<KeyBinding, T>& map, KeyBinding binding) {
    for (typename std::map<KeyBinding, T>::const_iterator it = map.cbegin(); it != map.cend();
         it++) {
        if ((it->first.key == binding.key) && (it->first.modifier & binding.modifier) != 0) {
            return it;
        }
    }
    return map.end(); // Return end if no match is found
}
template <typename T>
typename std::map<KeyBinding, T>::const_iterator FindKeyAllowingOnlyNoModifiers(
    const std::map<KeyBinding, T>& map, KeyBinding binding) {
    for (typename std::map<KeyBinding, T>::const_iterator it = map.cbegin(); it != map.cend();
         it++) {
        if (it->first.key == binding.key && it->first.modifier == SDL_KMOD_NONE) {
            return it;
        }
    }
    return map.end(); // Return end if no match is found
}

// Axis map: maps key+modifier to controller axis and axis value
struct AxisMapping {
    Input::Axis axis;
    int value; // Value to set for key press (+127 or -127 for movement)
};

// i strongly suggest you collapse these maps
std::map<std::string, u32> string_to_cbutton_map = {
    {"triangle", OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_TRIANGLE},
    {"circle", OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_CIRCLE},
    {"cross", OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_CROSS},
    {"square", OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_SQUARE},
    {"l1", OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_L1},
    {"l2", OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_L2},
    {"r1", OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_R1},
    {"r2", OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_R2},
    {"l3", OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_L3},
    {"r3", OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_R3},
    {"options", OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_OPTIONS},
    {"touchpad", OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_TOUCH_PAD},
    {"up", OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_UP},
    {"down", OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_DOWN},
    {"left", OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_LEFT},
    {"right", OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_RIGHT},
    {"leftjoystick_halfmode", LEFTJOYSTICK_HALFMODE},
    {"rightjoystick_halfmode", RIGHTJOYSTICK_HALFMODE},
};
std::map<std::string, AxisMapping> string_to_axis_map = {
    {"axis_left_x_plus", {Input::Axis::LeftX, 127}},
    {"axis_left_x_minus", {Input::Axis::LeftX, -127}},
    {"axis_left_y_plus", {Input::Axis::LeftY, 127}},
    {"axis_left_y_minus", {Input::Axis::LeftY, -127}},
    {"axis_right_x_plus", {Input::Axis::RightX, 127}},
    {"axis_right_x_minus", {Input::Axis::RightX, -127}},
    {"axis_right_y_plus", {Input::Axis::RightY, 127}},
    {"axis_right_y_minus", {Input::Axis::RightY, -127}},
};
std::map<std::string, u32> string_to_keyboard_key_map = {
    {"a", SDLK_A},
    {"b", SDLK_B},
    {"c", SDLK_C},
    {"d", SDLK_D},
    {"e", SDLK_E},
    {"f", SDLK_F},
    {"g", SDLK_G},
    {"h", SDLK_H},
    {"i", SDLK_I},
    {"j", SDLK_J},
    {"k", SDLK_K},
    {"l", SDLK_L},
    {"m", SDLK_M},
    {"n", SDLK_N},
    {"o", SDLK_O},
    {"p", SDLK_P},
    {"q", SDLK_Q},
    {"r", SDLK_R},
    {"s", SDLK_S},
    {"t", SDLK_T},
    {"u", SDLK_U},
    {"v", SDLK_V},
    {"w", SDLK_W},
    {"x", SDLK_X},
    {"y", SDLK_Y},
    {"z", SDLK_Z},
    {"0", SDLK_0},
    {"1", SDLK_1},
    {"2", SDLK_2},
    {"3", SDLK_3},
    {"4", SDLK_4},
    {"5", SDLK_5},
    {"6", SDLK_6},
    {"7", SDLK_7},
    {"8", SDLK_8},
    {"9", SDLK_9},
    {"comma", SDLK_COMMA},
    {"period", SDLK_PERIOD},
    {"question", SDLK_QUESTION},
    {"semicolon", SDLK_SEMICOLON},
    {"minus", SDLK_MINUS},
    {"underscore", SDLK_UNDERSCORE},
    {"lparenthesis", SDLK_LEFTPAREN},
    {"rparenthesis", SDLK_RIGHTPAREN},
    {"lbracket", SDLK_LEFTBRACKET},
    {"rbracket", SDLK_RIGHTBRACKET},
    {"lbrace", SDLK_LEFTBRACE},
    {"rbrace", SDLK_RIGHTBRACE},
    {"backslash", SDLK_BACKSLASH},
    {"dash", SDLK_SLASH},
    {"enter", SDLK_RETURN},
    {"space", SDLK_SPACE},
    {"tab", SDLK_TAB},
    {"backspace", SDLK_BACKSPACE},
    {"escape", SDLK_ESCAPE},
    {"left", SDLK_LEFT},
    {"right", SDLK_RIGHT},
    {"up", SDLK_UP},
    {"down", SDLK_DOWN},
    {"lctrl", SDLK_LCTRL},
    {"rctrl", SDLK_RCTRL},
    {"lshift", SDLK_LSHIFT},
    {"rshift", SDLK_RSHIFT},
    {"lalt", SDLK_LALT},
    {"ralt", SDLK_RALT},
    {"lmeta", SDLK_LGUI},
    {"rmeta", SDLK_RGUI},
    {"lwin", SDLK_LGUI},
    {"rwin", SDLK_RGUI},
    {"home", SDLK_HOME},
    {"end", SDLK_END},
    {"pgup", SDLK_PAGEUP},
    {"pgdown", SDLK_PAGEDOWN},
    {"leftbutton", SDL_BUTTON_LEFT},
    {"rightbutton", SDL_BUTTON_RIGHT},
    {"middlebutton", SDL_BUTTON_MIDDLE},
    {"sidebuttonback", SDL_BUTTON_X1},
    {"sidebuttonforward", SDL_BUTTON_X2},
    {"mousewheelup", SDL_EVENT_MOUSE_WHEEL_UP},
    {"mousewheeldown", SDL_EVENT_MOUSE_WHEEL_DOWN},
    {"mousewheelleft", SDL_EVENT_MOUSE_WHEEL_LEFT},
    {"mousewheelright", SDL_EVENT_MOUSE_WHEEL_RIGHT},
    {"kp0", SDLK_KP_0},
    {"kp1", SDLK_KP_1},
    {"kp2", SDLK_KP_2},
    {"kp3", SDLK_KP_3},
    {"kp4", SDLK_KP_4},
    {"kp5", SDLK_KP_5},
    {"kp6", SDLK_KP_6},
    {"kp7", SDLK_KP_7},
    {"kp8", SDLK_KP_8},
    {"kp9", SDLK_KP_9},
    {"kpperiod", SDLK_KP_PERIOD},
    {"kpdivide", SDLK_KP_DIVIDE},
    {"kpmultiply", SDLK_KP_MULTIPLY},
    {"kpminus", SDLK_KP_MINUS},
    {"kpplus", SDLK_KP_PLUS},
    {"kpenter", SDLK_KP_ENTER},
    {"kpequals", SDLK_KP_EQUALS},
    {"kpcomma", SDLK_KP_COMMA},
};
std::map<std::string, u32> string_to_keyboard_mod_key_map = {
    {"lshift", SDL_KMOD_LSHIFT}, {"rshift", SDL_KMOD_RSHIFT}, {"lctrl", SDL_KMOD_LCTRL},
    {"rctrl", SDL_KMOD_RCTRL},   {"lalt", SDL_KMOD_LALT},     {"ralt", SDL_KMOD_RALT},
    {"shift", SDL_KMOD_SHIFT},   {"ctrl", SDL_KMOD_CTRL},     {"alt", SDL_KMOD_ALT},
    {"l_meta", SDL_KMOD_LGUI},   {"r_meta", SDL_KMOD_RGUI},   {"meta", SDL_KMOD_GUI},
    {"lwin", SDL_KMOD_LGUI},     {"rwin", SDL_KMOD_RGUI},     {"win", SDL_KMOD_GUI},
    {"none", SDL_KMOD_NONE}, // if you want to be fancy
};

// i wrapped it in a function so I can collapse it
std::string getDefaultKeyboardConfig() {
    std::string default_config =
        R"(## SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
## SPDX-License-Identifier: GPL-2.0-or-later
 
#This is the default keybinding config
#To change per-game configs, modify the CUSAXXXXX.ini files
#To change the default config that applies to new games without already existing configs, modify default.ini
#If you don't like certain mappings, delete, change or comment them out.
#You can add any amount of KBM keybinds to a single controller input,
#but you can use each KBM keybind for one controller input.

#Keybinds used by the emulator (these are unchangeable):
#F11 : fullscreen
#F10 : FPS counter
#F9  : toggle mouse-to-joystick input 
#       (it overwrites everything else to that joystick, so this is required)
#F8  : reparse keyboard input(this)

#This is a mapping for Bloodborne, inspired by other Souls titles on PC.

#Specifies which joystick the mouse movement controls.
mouse_to_joystick = right;

#Use healing item, change status in inventory
triangle = f;
#Dodge, back in inventory
circle = space;
#Interact, select item in inventory
cross = e;
#Use quick item, remove item in inventory
square = r;

#Emergency extra bullets
up = w, lalt;
up = mousewheelup;
#Change quick item
down = s, lalt;
down = mousewheeldown;
#Change weapon in left hand
left = a, lalt;
left = mousewheelleft;
#Change weapon in right hand
right = d, lalt;
right = mousewheelright;
#Change into 'inventory mode', so you don't have to hold lalt every time you go into menus
modkey_toggle = i, lalt;

#Menu
options = escape;
#Gestures
touchpad = g;

#Transform
l1 = rightbutton, lshift;
#Shoot
r1 = leftbutton;
#Light attack
l2 = rightbutton;
#Heavy attack
r2 = leftbutton, lshift;
#Does nothing
l3 = x;
#Center cam, lock on
r3 = q;
r3 = middlebutton;

#Axis mappings
#Move
axis_left_x_minus = a;
axis_left_x_plus = d;
axis_left_y_minus = w;
axis_left_y_plus = s;
#Change to 'walk mode' by holding the following key:
leftjoystick_halfmode = lctrl;
)";
    return default_config;
}

// Button map: maps key+modifier to controller button
std::map<KeyBinding, u32> button_map = {};
std::map<KeyBinding, AxisMapping> axis_map = {};
std::map<SDL_Keycode, std::pair<SDL_Keymod, bool>> key_to_modkey_toggle_map = {};

int mouse_joystick_binding = 0;
Uint32 mouse_polling_id = 0;
bool mouse_enabled = false, leftjoystick_halfmode = false, rightjoystick_halfmode = false;
void WindowSDL::parseInputConfig(const std::string& filename) {

    // Read configuration file.
    // std::cout << "Reading keyboard config...\n";
    const auto config_file = Common::FS::GetUserPath(Common::FS::PathType::UserDir) / filename;
    if (!std::filesystem::exists(config_file)) {
        // create it
        std::ofstream file;
        file.open(config_file, std::ios::out);
        if (file.is_open()) {
            file.close();
            std::cout << "Config file generated.\n";
        } else {
            std::cerr << "Error creating file!\n";
        }
    }
    std::ifstream file(config_file);
    if (!file.is_open()) {
        std::cerr << "Error opening file: " << filename << std::endl;
        return;
    }

    button_map.clear();
    axis_map.clear();
    int lineCount = 0;
    std::string line;
    while (std::getline(file, line)) {
        lineCount++;
        // strip the ; and whitespace
        line.erase(std::remove(line.begin(), line.end(), ' '), line.end());
        if (line[line.length() - 1] == ';') {
            line = line.substr(0, line.length() - 1);
        }
        // Ignore comment lines
        if (line.empty() || line[0] == '#') {
            continue;
        }
        // Split the line by '='
        std::size_t equal_pos = line.find('=');
        if (equal_pos == std::string::npos) {
            std::cerr << "Invalid line format at line: " << lineCount << " data: " << line
                      << std::endl;
            continue;
        }

        std::string controller_input = line.substr(0, equal_pos);
        std::string kbm_input = line.substr(equal_pos + 1);
        KeyBinding binding = {0, SDL_KMOD_NONE};

        // special check for mouse to joystick input
        if (controller_input == "mouse_to_joystick") {
            if (kbm_input == "left") {
                mouse_joystick_binding = 1;
            } else if (kbm_input == "right") {
                mouse_joystick_binding = 2;
            } else {
                mouse_joystick_binding = 0; // default to 'none' or invalid
            }
            continue;
        }
        // first we parse the binding, and if its wrong, we skip to the next line
        std::size_t comma_pos = kbm_input.find(',');
        if (comma_pos != std::string::npos) {
            // Handle key + modifier
            std::string key = kbm_input.substr(0, comma_pos);
            std::string mod = kbm_input.substr(comma_pos + 1);

            auto key_it = string_to_keyboard_key_map.find(key);
            auto mod_it = string_to_keyboard_mod_key_map.find(mod);

            if (key_it != string_to_keyboard_key_map.end() &&
                mod_it != string_to_keyboard_mod_key_map.end()) {
                binding.key = key_it->second;
                binding.modifier = mod_it->second;
            } else {
                std::cerr << "Syntax error while parsing kbm inputs at line " << lineCount
                          << " line data: " << line << "\n";
                continue; // skip
            }
        } else {
            // Just a key without modifier
            auto key_it = string_to_keyboard_key_map.find(kbm_input);
            if (key_it != string_to_keyboard_key_map.end()) {
                binding.key = key_it->second;
            } else {
                std::cerr << "Syntax error while parsing kbm inputs at line " << lineCount
                          << " line data: " << line << "\n";
                continue; // skip
            }
        }

        // Check for axis mapping (example: axis_left_x_plus)
        auto axis_it = string_to_axis_map.find(controller_input);
        auto button_it = string_to_cbutton_map.find(controller_input);
        if (axis_it != string_to_axis_map.end()) {
            axis_map[binding] = axis_it->second;
        } else if (button_it != string_to_cbutton_map.end()) {
            button_map[binding] = button_it->second;
        } else {
            std::cerr << "Syntax error while parsing kbm inputs at line " << lineCount
                      << " line data: " << line << "\n";
            continue; // skip
        }
    }
    file.close();
}

Uint32 WindowSDL::keyRepeatCallback(void* param, Uint32 id, Uint32 interval) {
    auto* data = (std::pair<WindowSDL*, SDL_Event*>*)param;
    KeyBinding binding(data->second);
    if (data->second->type == SDL_EVENT_MOUSE_WHEEL) {

        auto button_it = button_map.find(binding);
        auto axis_it = axis_map.find(binding);
        if (button_it != button_map.end()) {
            data->first->updateButton(binding, button_it->second, true);
        } else if (axis_it != axis_map.end()) {

            data->first->controller->Axis(0, axis_it->second.axis, Input::GetAxis(-0x80, 0x80, 0));
        }
    }
    data->first->updateModKeyedInputsManually(binding);
    delete data->second;
    delete data;
    return 0; // Return 0 to stop the timer after firing once
}

Uint32 WindowSDL::mousePolling(void* param, Uint32 id, Uint32 interval) {
    auto* data = (WindowSDL*)param;
    data->updateMouse();
    return 33; // Return 0 to stop the timer after firing once
}

void WindowSDL::updateMouse() {
    if (!mouse_enabled)
        return;
    Input::Axis axis_x, axis_y;
    switch (mouse_joystick_binding) {
    case 1:
        axis_x = Input::Axis::LeftX;
        axis_y = Input::Axis::LeftY;
        break;
    case 2:
        axis_x = Input::Axis::RightX;
        axis_y = Input::Axis::RightY;
        break;
    case 0:
    default:
        return; // no update needed
    }

    float d_x = 0, d_y = 0;
    SDL_GetRelativeMouseState(&d_x, &d_y);

    float mouse_speed = SDL_clamp((sqrt(d_x * d_x + d_y * d_y) + 16) * 1, 64.0, 128.0);
    std::cout << "speed: " << mouse_speed << "\n";
    float angle = atan2(d_y, d_x);
    float a_x = cos(angle) * mouse_speed, a_y = sin(angle) * mouse_speed;

    if (d_x != 0 && d_y != 0) {
        controller->Axis(0, axis_x, Input::GetAxis(-0x80, 0x80, a_x));
        controller->Axis(0, axis_y, Input::GetAxis(-0x80, 0x80, a_y));
    } else {
        controller->Axis(0, axis_x, Input::GetAxis(-0x80, 0x80, 0));
        controller->Axis(0, axis_y, Input::GetAxis(-0x80, 0x80, 0));
    }
}

WindowSDL::WindowSDL(s32 width_, s32 height_, Input::GameController* controller_,
                     std::string_view window_title)
    : width{width_}, height{height_}, controller{controller_} {
    if (!SDL_Init(SDL_INIT_VIDEO)) {
        UNREACHABLE_MSG("Failed to initialize SDL video subsystem: {}", SDL_GetError());
    }
    SDL_InitSubSystem(SDL_INIT_AUDIO);

    SDL_PropertiesID props = SDL_CreateProperties();
    SDL_SetStringProperty(props, SDL_PROP_WINDOW_CREATE_TITLE_STRING,
                          std::string(window_title).c_str());
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_X_NUMBER, SDL_WINDOWPOS_CENTERED);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_Y_NUMBER, SDL_WINDOWPOS_CENTERED);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_WIDTH_NUMBER, width);
    SDL_SetNumberProperty(props, SDL_PROP_WINDOW_CREATE_HEIGHT_NUMBER, height);
    SDL_SetNumberProperty(props, "flags", SDL_WINDOW_VULKAN);
    SDL_SetBooleanProperty(props, SDL_PROP_WINDOW_CREATE_RESIZABLE_BOOLEAN, true);
    window = SDL_CreateWindowWithProperties(props);
    SDL_DestroyProperties(props);
    if (window == nullptr) {
        UNREACHABLE_MSG("Failed to create window handle: {}", SDL_GetError());
    }

    SDL_SetWindowFullscreen(window, Config::isFullscreenMode());

    SDL_InitSubSystem(SDL_INIT_GAMEPAD);
    controller->TryOpenSDLController();

#if defined(SDL_PLATFORM_WIN32)
    window_info.type = WindowSystemType::Windows;
    window_info.render_surface = SDL_GetPointerProperty(SDL_GetWindowProperties(window),
                                                        SDL_PROP_WINDOW_WIN32_HWND_POINTER, NULL);
#elif defined(SDL_PLATFORM_LINUX)
    if (SDL_strcmp(SDL_GetCurrentVideoDriver(), "x11") == 0) {
        window_info.type = WindowSystemType::X11;
        window_info.display_connection = SDL_GetPointerProperty(
            SDL_GetWindowProperties(window), SDL_PROP_WINDOW_X11_DISPLAY_POINTER, NULL);
        window_info.render_surface = (void*)SDL_GetNumberProperty(
            SDL_GetWindowProperties(window), SDL_PROP_WINDOW_X11_WINDOW_NUMBER, 0);
    } else if (SDL_strcmp(SDL_GetCurrentVideoDriver(), "wayland") == 0) {
        window_info.type = WindowSystemType::Wayland;
        window_info.display_connection = SDL_GetPointerProperty(
            SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WAYLAND_DISPLAY_POINTER, NULL);
        window_info.render_surface = SDL_GetPointerProperty(
            SDL_GetWindowProperties(window), SDL_PROP_WINDOW_WAYLAND_SURFACE_POINTER, NULL);
    }
#elif defined(SDL_PLATFORM_MACOS)
    window_info.type = WindowSystemType::Metal;
    window_info.render_surface = SDL_Metal_GetLayer(SDL_Metal_CreateView(window));
#endif
    // initialize kbm controls
    parseInputConfig("keyboardInputConfig.ini");
}

WindowSDL::~WindowSDL() = default;

void WindowSDL::waitEvent() {
    // Called on main thread
    SDL_Event event;
    if (mouse_polling_id == 0) {
        mouse_polling_id = SDL_AddTimer(33, mousePolling, (void*)this);
    }

    if (!SDL_WaitEvent(&event)) {
        return;
    }

    if (ImGui::Core::ProcessEvent(&event)) {
        return;
    }
    SDL_Event* event_copy = new SDL_Event();
    *event_copy = event;
    std::pair<WindowSDL*, SDL_Event*>* payload_to_timer =
        new std::pair<WindowSDL*, SDL_Event*>(this, event_copy);

    switch (event.type) {
    case SDL_EVENT_WINDOW_RESIZED:
    case SDL_EVENT_WINDOW_MAXIMIZED:
    case SDL_EVENT_WINDOW_RESTORED:
        onResize();
        break;
    case SDL_EVENT_WINDOW_MINIMIZED:
    case SDL_EVENT_WINDOW_EXPOSED:
        is_shown = event.type == SDL_EVENT_WINDOW_EXPOSED;
        onResize();
        break;
    case SDL_EVENT_MOUSE_WHEEL:
    case SDL_EVENT_MOUSE_BUTTON_UP:
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
        // native mouse update function goes here
        // as seen in pr #633
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
        SDL_AddTimer(33, keyRepeatCallback, (void*)payload_to_timer);
        onKeyPress(&event);
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
    case SDL_EVENT_GAMEPAD_ADDED:
    case SDL_EVENT_GAMEPAD_REMOVED:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_UP:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION:
        onGamepadEvent(&event);
        break;
    case SDL_EVENT_QUIT:
        is_open = false;
        break;
    default:
        break;
    }
}

void WindowSDL::onResize() {
    SDL_GetWindowSizeInPixels(window, &width, &height);
    ImGui::Core::OnResize();
}

void WindowSDL::updateButton(KeyBinding& binding, u32 button, bool is_pressed) {
    float x;
    Input::Axis axis;
    switch (button) {
    case OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_L2:
    case OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_R2:
        axis = (button == OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_R2) ? Input::Axis::TriggerRight
                                                                         : Input::Axis::TriggerLeft;
        // int axis_value = is_pressed ? 255 : 0;
        // int ax = Input::GetAxis(0, 0x80, is_pressed ? 255 : 0);
        controller->Axis(0, axis, Input::GetAxis(0, 0x80, is_pressed ? 255 : 0));
        break;
    case OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_TOUCH_PAD:
        x = Config::getBackButtonBehavior() == "left"    ? 0.25f
            : Config::getBackButtonBehavior() == "right" ? 0.75f
                                                         : 0.5f;
        controller->SetTouchpadState(0, true, x, 0.5f);
        controller->CheckButton(0, button, is_pressed);
        break;
    default: // is a normal key
        controller->CheckButton(0, button, is_pressed);
        break;
    }
}

void WindowSDL::onKeyPress(const SDL_Event* event) {
    // Extract key and modifier
    KeyBinding binding(event);
    bool input_down = event->type == SDL_EVENT_KEY_DOWN ||
                      event->type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
                      event->type == SDL_EVENT_MOUSE_WHEEL;

    u32 button = 0;
    Input::Axis axis = Input::Axis::AxisMax;
    int axis_value = 0;

    // Handle window controls outside of the input maps
    if (event->type == SDL_EVENT_KEY_DOWN) {
        // Toggle capture of the mouse
        if (binding.key == SDLK_F9) {
            SDL_SetWindowRelativeMouseMode(this->GetSdlWindow(),
                                           !SDL_GetWindowRelativeMouseMode(this->GetSdlWindow()));
        }
        // Reparse kbm inputs
        else if (binding.key == SDLK_F8) {
            parseInputConfig("keyboardInputConfig.ini");
        }
        // Toggle mouse movement input
        else if (binding.key == SDLK_F7) {
            mouse_enabled = !mouse_enabled;
        }
        // Toggle fullscreen
        else if (binding.key == SDLK_F11) {
            SDL_WindowFlags flag = SDL_GetWindowFlags(window);
            bool is_fullscreen = flag & SDL_WINDOW_FULLSCREEN;
            SDL_SetWindowFullscreen(window, !is_fullscreen);
        }
        // Trigger rdoc capture
        else if (binding.key == SDLK_F12) {
            VideoCore::TriggerCapture();
        }
    }

    // Check if the current key+modifier is a button or axis mapping
    // first only exact matches
    auto button_it = FindKeyAllowingPartialModifiers(button_map, binding);
    auto axis_it = FindKeyAllowingPartialModifiers(axis_map, binding);
    // then no mod key matches if we didn't find it in the previous pass
    if (button_it == button_map.end() && axis_it == axis_map.end()) {
        button_it = FindKeyAllowingOnlyNoModifiers(button_map, binding);
    }
    if (axis_it == axis_map.end() && button_it == button_map.end()) {
        axis_it = FindKeyAllowingOnlyNoModifiers(axis_map, binding);
    }

    if (button_it != button_map.end()) {
        // test
        if (button_it->second == LEFTJOYSTICK_HALFMODE) {
            leftjoystick_halfmode = input_down;
            // std::cout << "walk mode is " << (joystick_halfmode ? "on" : "off") << "\n";
        } else if (button_it->second == RIGHTJOYSTICK_HALFMODE) {
            rightjoystick_halfmode = input_down;
        } else {
            WindowSDL::updateButton(binding, button_it->second, input_down);
        }
    }
    if (axis_it != axis_map.end()) {
        Input::Axis axis = Input::Axis::AxisMax;
        int axis_value = 0;
        axis = axis_it->second.axis;
        float multiplier = 1.0;
        switch (axis) {
        case Input::Axis::LeftX:
        case Input::Axis::LeftY:
            multiplier = leftjoystick_halfmode ? 0.5 : 1.0;
            break;
        case Input::Axis::RightX:
        case Input::Axis::RightY:
            multiplier = rightjoystick_halfmode ? 0.5 : 1.0;
            break;
        default:
            break;
        }
        multiplier = leftjoystick_halfmode ? 0.5 : 1.0;
        axis_value = (input_down ? axis_it->second.value : 0) * multiplier;
        int ax = Input::GetAxis(-0x80, 0x80, axis_value);
        controller->Axis(0, axis, ax);
    }
}

// if we don't do this, then if we activate a mod keyed input and let go of the mod key first,
// the button will be stuck on the "on" state becuse the "turn off" signal would only come from
// the other key being unpressed
void WindowSDL::updateModKeyedInputsManually(Frontend::KeyBinding& binding) {
    bool mod_keyed_input_found = false;
    for (auto input : button_map) {
        if (input.first.modifier != SDL_KMOD_NONE) {
            if ((input.first.modifier & binding.modifier) == 0) {
                WindowSDL::updateButton(binding, input.second, false);
            } else if (input.first.key == binding.key) {
                mod_keyed_input_found = true;
            }
        }
    }
    for (auto input : axis_map) {
        if (input.first.modifier != SDL_KMOD_NONE) {
            if ((input.first.modifier & binding.modifier) == 0) {
                controller->Axis(0, input.second.axis, Input::GetAxis(-0x80, 0x80, 0));
            } else if (input.first.key == binding.key) {
                mod_keyed_input_found = true;
            }
        }
    }
    // if both non mod keyed and mod keyed inputs are used and you press the key and then the mod
    // key in a single frame, both will activate but the simple one will not deactivate, unless i
    // use this stupid looking workaround
    if (!mod_keyed_input_found)
        return; // in this case the fix for the fix for the wrong update order is not needed
    for (auto input : button_map) {
        if (input.first.modifier == SDL_KMOD_NONE) {
            WindowSDL::updateButton(binding, input.second, false);
        }
    }
    for (auto input : axis_map) {
        if (input.first.modifier == SDL_KMOD_NONE) {
            controller->Axis(0, input.second.axis, Input::GetAxis(-0x80, 0x80, 0));
        }
    }
    // also this sometimes leads to janky inputs but whoever decides to intentionally create a state
    // where this is needed should not deserve a smooth experience anyway
}

void WindowSDL::onGamepadEvent(const SDL_Event* event) {
    using Libraries::Pad::OrbisPadButtonDataOffset;

    u32 button = 0;
    Input::Axis axis = Input::Axis::AxisMax;
    switch (event->type) {
    case SDL_EVENT_GAMEPAD_ADDED:
    case SDL_EVENT_GAMEPAD_REMOVED:
        controller->TryOpenSDLController();
        break;
    case SDL_EVENT_GAMEPAD_TOUCHPAD_DOWN:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_UP:
    case SDL_EVENT_GAMEPAD_TOUCHPAD_MOTION:
        controller->SetTouchpadState(event->gtouchpad.finger,
                                     event->type != SDL_EVENT_GAMEPAD_TOUCHPAD_UP,
                                     event->gtouchpad.x, event->gtouchpad.y);
        break;
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP:
        button = sdlGamepadToOrbisButton(event->gbutton.button);
        if (button != 0) {
            if (event->gbutton.button == SDL_GAMEPAD_BUTTON_BACK) {
                std::string backButtonBehavior = Config::getBackButtonBehavior();
                if (backButtonBehavior != "none") {
                    float x = backButtonBehavior == "left"
                                  ? 0.25f
                                  : (backButtonBehavior == "right" ? 0.75f : 0.5f);
                    // trigger a touchpad event so that the touchpad emulation for back button works
                    controller->SetTouchpadState(0, true, x, 0.5f);
                    controller->CheckButton(0, button,
                                            event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
                }
            } else {
                controller->CheckButton(0, button, event->type == SDL_EVENT_GAMEPAD_BUTTON_DOWN);
            }
        }
        break;
    case SDL_EVENT_GAMEPAD_AXIS_MOTION:
        axis = event->gaxis.axis == SDL_GAMEPAD_AXIS_LEFTX           ? Input::Axis::LeftX
               : event->gaxis.axis == SDL_GAMEPAD_AXIS_LEFTY         ? Input::Axis::LeftY
               : event->gaxis.axis == SDL_GAMEPAD_AXIS_RIGHTX        ? Input::Axis::RightX
               : event->gaxis.axis == SDL_GAMEPAD_AXIS_RIGHTY        ? Input::Axis::RightY
               : event->gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER  ? Input::Axis::TriggerLeft
               : event->gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER ? Input::Axis::TriggerRight
                                                                     : Input::Axis::AxisMax;
        if (axis != Input::Axis::AxisMax) {
            if (event->gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER ||
                event->gaxis.axis == SDL_GAMEPAD_AXIS_RIGHT_TRIGGER) {
                controller->Axis(0, axis, Input::GetAxis(0, 0x8000, event->gaxis.value));

            } else {
                controller->Axis(0, axis, Input::GetAxis(-0x8000, 0x8000, event->gaxis.value));
            }
        }
        break;
    }
}

int WindowSDL::sdlGamepadToOrbisButton(u8 button) {
    using Libraries::Pad::OrbisPadButtonDataOffset;

    switch (button) {
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN:
        return OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_DOWN;
    case SDL_GAMEPAD_BUTTON_DPAD_UP:
        return OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_UP;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT:
        return OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_LEFT;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT:
        return OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_RIGHT;
    case SDL_GAMEPAD_BUTTON_SOUTH:
        return OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_CROSS;
    case SDL_GAMEPAD_BUTTON_NORTH:
        return OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_TRIANGLE;
    case SDL_GAMEPAD_BUTTON_WEST:
        return OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_SQUARE;
    case SDL_GAMEPAD_BUTTON_EAST:
        return OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_CIRCLE;
    case SDL_GAMEPAD_BUTTON_START:
        return OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_OPTIONS;
    case SDL_GAMEPAD_BUTTON_TOUCHPAD:
        return OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_TOUCH_PAD;
    case SDL_GAMEPAD_BUTTON_BACK:
        return OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_TOUCH_PAD;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER:
        return OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_L1;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER:
        return OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_R1;
    case SDL_GAMEPAD_BUTTON_LEFT_STICK:
        return OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_L3;
    case SDL_GAMEPAD_BUTTON_RIGHT_STICK:
        return OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_R3;
    default:
        return 0;
    }
}

} // namespace Frontend

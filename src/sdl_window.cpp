// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <SDL3/SDL_events.h>
#include <SDL3/SDL_init.h>
#include <SDL3/SDL_properties.h>
#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_video.h>
#include "common/assert.h"
#include "common/config.h"
#include "common/version.h"
#include "core/libraries/pad/pad.h"
#include "imgui/renderer/imgui_core.h"
#include "input/controller.h"
#include "sdl_window.h"
#include "video_core/renderdoc.h"

#ifdef __APPLE__
#include <SDL3/SDL_metal.h>
#endif

Uint32 getMouseWheelEvent(const SDL_Event* event) {
    if (event->type != SDL_EVENT_MOUSE_WHEEL)
        return 0;
    if (event->wheel.y > 0) {
        return SDL_MOUSE_WHEEL_UP;
    } else if (event->wheel.y < 0) {
        return SDL_MOUSE_WHEEL_DOWN;
    } else if (event->wheel.x > 0) {
        return SDL_MOUSE_WHEEL_RIGHT;
    } else if (event->wheel.x < 0) {
        return SDL_MOUSE_WHEEL_LEFT;
    }
    return 0;
}

namespace KBMConfig {
using Libraries::Pad::OrbisPadButtonDataOffset;

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

// Flags and values for varying purposes
int mouse_joystick_binding = 0;
float mouse_deadzone_offset = 0.5, mouse_speed = 1, mouse_speed_offset = 0.125;
Uint32 mouse_polling_id = 0;
bool mouse_enabled = false, leftjoystick_halfmode = false, rightjoystick_halfmode = false;

// A vector to store delayed actions by event ID
std::vector<DelayedAction> delayedActions;

KeyBinding::KeyBinding(const SDL_Event* event) {
    modifier = getCustomModState();
    key = 0;
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

SDL_Keymod KeyBinding::getCustomModState() {
    SDL_Keymod state = SDL_GetModState();
    for (auto mod_flag : KBMConfig::key_to_modkey_toggle_map) {
        if (mod_flag.second.second) {
            state |= mod_flag.second.first;
        }
    }
    return state;
}

void parseInputConfig(const std::string game_id = "") {
    // Read configuration file of the game, and if it doesn't exist, generate it from default
    // If that doesn't exist either, generate that from getDefaultConfig() and try again
    // If even the folder is missing, we start with that.
    const auto config_dir = Common::FS::GetUserPath(Common::FS::PathType::UserDir) / "kbmConfig";
    const auto config_file = config_dir / (game_id + ".ini");
    const auto default_config_file = config_dir / "default.ini";

    // Ensure the config directory exists
    if (!std::filesystem::exists(config_dir)) {
        std::filesystem::create_directories(config_dir);
    }

    // Try loading the game-specific config file
    if (!std::filesystem::exists(config_file)) {
        // If game-specific config doesn't exist, check for the default config
        if (!std::filesystem::exists(default_config_file)) {
            // If the default config is also missing, create it from getDefaultConfig()
            const auto default_config = getDefaultKeyboardConfig();
            std::ofstream default_config_stream(default_config_file);
            if (default_config_stream) {
                default_config_stream << default_config;
            }
        }

        // If default config now exists, copy it to the game-specific config file
        if (std::filesystem::exists(default_config_file) && !game_id.empty()) {
            std::filesystem::copy(default_config_file, config_file);
        }
    }
    // if we just called the function to generate the directory and the default .ini
    if (game_id.empty()) {
        return;
    }

    // we reset these here so in case the user fucks up or doesn't include this we can fall back to
    // default
    mouse_deadzone_offset = 0.5;
    mouse_speed = 1;
    mouse_speed_offset = 0.125;
    button_map.clear();
    axis_map.clear();
    key_to_modkey_toggle_map.clear();
    int lineCount = 0;

    std::ifstream file(config_file);
    std::string line = "";
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

        std::string before_equals = line.substr(0, equal_pos);
        std::string after_equals = line.substr(equal_pos + 1);
        std::size_t comma_pos = after_equals.find(',');
        KeyBinding binding = {0, SDL_KMOD_NONE};

        // special check for mouse to joystick input
        if (before_equals == "mouse_to_joystick") {
            if (after_equals == "left") {
                mouse_joystick_binding = 1;
            } else if (after_equals == "right") {
                mouse_joystick_binding = 2;
            } else {
                mouse_joystick_binding = 0; // default to 'none' or invalid
            }
            continue;
        }
        // mod key toggle
        if (before_equals == "modkey_toggle") {
            if (comma_pos != std::string::npos) {
                auto k = string_to_keyboard_key_map.find(after_equals.substr(0, comma_pos));
                auto m = string_to_keyboard_mod_key_map.find(after_equals.substr(comma_pos + 1));
                if (k != string_to_keyboard_key_map.end() &&
                    m != string_to_keyboard_mod_key_map.end()) {
                    key_to_modkey_toggle_map[k->second] = {m->second, false};
                    continue;
                }
            }
            std::cerr << "Invalid line format at line: " << lineCount << " data: " << line
                      << std::endl;
            continue;
        }
        // first we parse the binding, and if its wrong, we skip to the next line
        if (comma_pos != std::string::npos) {
            // Handle key + modifier
            std::string key = after_equals.substr(0, comma_pos);
            std::string mod = after_equals.substr(comma_pos + 1);

            auto key_it = string_to_keyboard_key_map.find(key);
            auto mod_it = string_to_keyboard_mod_key_map.find(mod);

            if (key_it != string_to_keyboard_key_map.end() &&
                mod_it != string_to_keyboard_mod_key_map.end()) {
                binding.key = key_it->second;
                binding.modifier = mod_it->second;
            } else if (before_equals == "mouse_movement_params") {
                // handle mouse movement params
                float p1 = 0.5, p2 = 1, p3 = 0.125;
                std::size_t second_comma_pos = after_equals.find(',');
                try {
                    p1 = std::stof(key);
                    p2 = std::stof(mod.substr(0, second_comma_pos));
                    p3 = std::stof(mod.substr(second_comma_pos + 1));
                    mouse_deadzone_offset = p1;
                    mouse_speed = p2;
                    mouse_speed_offset = p3;
                } catch (...) {
                    // fallback to default values
                    mouse_deadzone_offset = 0.5;
                    mouse_speed = 1;
                    mouse_speed_offset = 0.125;
                    std::cerr << "Parsing error while parsing kbm inputs at line " << lineCount
                              << " line data: " << line << "\n";
                }
                continue;
            } else {
                std::cerr << "Syntax error while parsing kbm inputs at line " << lineCount
                          << " line data: " << line << "\n";
                continue; // skip
            }
        } else {
            // Just a key without modifier
            auto key_it = string_to_keyboard_key_map.find(after_equals);
            if (key_it != string_to_keyboard_key_map.end()) {
                binding.key = key_it->second;
            } else {
                std::cerr << "Syntax error while parsing kbm inputs at line " << lineCount
                          << " line data: " << line << "\n";
                continue; // skip
            }
        }

        // Check for axis mapping (example: axis_left_x_plus)
        auto axis_it = string_to_axis_map.find(before_equals);
        auto button_it = string_to_cbutton_map.find(before_equals);
        if (axis_it != string_to_axis_map.end()) {
            axis_map[binding] = axis_it->second;
        } else if (button_it != string_to_cbutton_map.end()) {
            button_map[binding] = button_it->second;
        } else {
            std::cerr << "Syntax error while parsing kbm inputs at line " << lineCount
                      << " line data: " << line << "\n";
        }
    }
    file.close();
}

} // namespace KBMConfig

namespace Frontend {
using Libraries::Pad::OrbisPadButtonDataOffset;

using namespace KBMConfig;
using KBMConfig::AxisMapping;
using KBMConfig::KeyBinding;

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

void WindowSDL::handleDelayedActions() {
    // Uncomment at your own terminal's risk
    // std::cout << "I fear the amount of spam this line will generate\n";
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
    Uint32 currentTime = SDL_GetTicks();
    for (auto it = delayedActions.begin(); it != delayedActions.end();) {
        if (currentTime >= it->triggerTime) {
            if (it->event.type == SDL_EVENT_MOUSE_WHEEL) {
                SDL_Event* mouseEvent = &(it->event);
                KeyBinding binding(mouseEvent);

                auto button_it = button_map.find(binding);
                auto axis_it = axis_map.find(binding);

                if (button_it != button_map.end()) {
                    updateButton(binding, button_it->second, false);
                } else if (axis_it != axis_map.end()) {
                    controller->Axis(0, axis_it->second.axis, Input::GetAxis(-0x80, 0x80, 0));
                }
            } else {
                KeyBinding b(&(it->event));
                updateModKeyedInputsManually(b);
            }
            it = delayedActions.erase(it); // Erase returns the next iterator
        } else {
            ++it;
        }
    }
}

Uint32 WindowSDL::mousePolling(void* param, Uint32 id, Uint32 interval) {
    auto* data = (WindowSDL*)param;
    data->updateMouse();
    return 33;
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

    float output_speed =
        SDL_clamp((sqrt(d_x * d_x + d_y * d_y) + mouse_speed_offset * 128) * mouse_speed,
                  mouse_deadzone_offset * 128, 128.0);

    float angle = atan2(d_y, d_x);
    float a_x = cos(angle) * output_speed, a_y = sin(angle) * output_speed;

    if (d_x != 0 && d_y != 0) {
        controller->Axis(0, axis_x, Input::GetAxis(-0x80, 0x80, a_x));
        controller->Axis(0, axis_y, Input::GetAxis(-0x80, 0x80, a_y));
    } else {
        controller->Axis(0, axis_x, Input::GetAxis(-0x80, 0x80, 0));
        controller->Axis(0, axis_y, Input::GetAxis(-0x80, 0x80, 0));
    }
}

static Uint32 SDLCALL PollController(void* userdata, SDL_TimerID timer_id, Uint32 interval) {
    auto* controller = reinterpret_cast<Input::GameController*>(userdata);
    return controller->Poll();
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
}

WindowSDL::~WindowSDL() = default;

void WindowSDL::waitEvent() {
    // Called on main thread
    SDL_Event event;

    if (!SDL_WaitEvent(&event)) {
        return;
    }

    if (ImGui::Core::ProcessEvent(&event)) {
        return;
    }

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
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP:
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

void WindowSDL::initTimers() {
    SDL_AddTimer(100, &PollController, controller);
}

void WindowSDL::onResize() {
    SDL_GetWindowSizeInPixels(window, &width, &height);
    ImGui::Core::OnResize();
}

void WindowSDL::onKeyPress(const SDL_Event* event) {
    using Libraries::Pad::OrbisPadButtonDataOffset;

#ifdef __APPLE__
    // Use keys that are more friendly for keyboards without a keypad.
    // Once there are key binding options this won't be necessary.
    constexpr SDL_Keycode CrossKey = SDLK_N;
    constexpr SDL_Keycode CircleKey = SDLK_B;
    constexpr SDL_Keycode SquareKey = SDLK_V;
    constexpr SDL_Keycode TriangleKey = SDLK_C;
#else
    constexpr SDL_Keycode CrossKey = SDLK_KP_2;
    constexpr SDL_Keycode CircleKey = SDLK_KP_6;
    constexpr SDL_Keycode SquareKey = SDLK_KP_4;
    constexpr SDL_Keycode TriangleKey = SDLK_KP_8;
#endif

    u32 button = 0;
    Input::Axis axis = Input::Axis::AxisMax;
    switch (button) {
    case OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_L2:
    case OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_R2:
        axis = (button == OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_R2) ? Input::Axis::TriggerRight
                                                                         : Input::Axis::TriggerLeft;
        controller->Axis(0, axis, Input::GetAxis(0, 0x80, is_pressed ? 255 : 0));
        break;
    case SDLK_DOWN:
        button = OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_DOWN;
        break;
    case SDLK_LEFT:
        button = OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_LEFT;
        break;
    case SDLK_RIGHT:
        button = OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_RIGHT;
        break;
    case TriangleKey:
        button = OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_TRIANGLE;
        break;
    case CircleKey:
        button = OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_CIRCLE;
        break;
    case CrossKey:
        button = OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_CROSS;
        break;
    case SquareKey:
        button = OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_SQUARE;
        break;
    case SDLK_RETURN:
        button = OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_OPTIONS;
        break;
    case SDLK_A:
        axis = Input::Axis::LeftX;
        if (event->type == SDL_EVENT_KEY_DOWN) {
            axisvalue += -127;
        } else {
            axisvalue = 0;
        }
        ax = Input::GetAxis(-0x80, 0x80, axisvalue);
        break;
    case SDLK_D:
        axis = Input::Axis::LeftX;
        if (event->type == SDL_EVENT_KEY_DOWN) {
            axisvalue += 127;
        } else {
            axisvalue = 0;
        }
        ax = Input::GetAxis(-0x80, 0x80, axisvalue);
        break;
    case SDLK_W:
        axis = Input::Axis::LeftY;
        if (event->type == SDL_EVENT_KEY_DOWN) {
            axisvalue += -127;
        } else {
            axisvalue = 0;
        }
        ax = Input::GetAxis(-0x80, 0x80, axisvalue);
        break;
    case SDLK_S:
        axis = Input::Axis::LeftY;
        if (event->type == SDL_EVENT_KEY_DOWN) {
            axisvalue += 127;
        } else {
            axisvalue = 0;
        }
        ax = Input::GetAxis(-0x80, 0x80, axisvalue);
        break;
    case SDLK_J:
        axis = Input::Axis::RightX;
        if (event->type == SDL_EVENT_KEY_DOWN) {
            axisvalue += -127;
        } else {
            axisvalue = 0;
        }
        ax = Input::GetAxis(-0x80, 0x80, axisvalue);
        break;
    case SDLK_L:
        axis = Input::Axis::RightX;
        if (event->type == SDL_EVENT_KEY_DOWN) {
            axisvalue += 127;
        } else {
            axisvalue = 0;
        }
        ax = Input::GetAxis(-0x80, 0x80, axisvalue);
        break;
    case SDLK_I:
        axis = Input::Axis::RightY;
        if (event->type == SDL_EVENT_KEY_DOWN) {
            axisvalue += -127;
        } else {
            axisvalue = 0;
        }
        ax = Input::GetAxis(-0x80, 0x80, axisvalue);
        break;
    case SDLK_K:
        axis = Input::Axis::RightY;
        if (event->type == SDL_EVENT_KEY_DOWN) {
            axisvalue += 127;
        } else {
            axisvalue = 0;
        }
        ax = Input::GetAxis(-0x80, 0x80, axisvalue);
        break;
    case SDLK_X:
        button = OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_L3;
        break;
    case SDLK_M:
        button = OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_R3;
        break;
    case SDLK_Q:
        button = OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_L1;
        break;
    case SDLK_U:
        button = OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_R1;
        break;
    case SDLK_E:
        button = OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_L2;
        axis = Input::Axis::TriggerLeft;
        if (event->type == SDL_EVENT_KEY_DOWN) {
            axisvalue += 255;
        } else {
            axisvalue = 0;
        }
        ax = Input::GetAxis(0, 0x80, axisvalue);
        break;
    case SDLK_O:
        button = OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_R2;
        axis = Input::Axis::TriggerRight;
        if (event->type == SDL_EVENT_KEY_DOWN) {
            axisvalue += 255;
        } else {
            axisvalue = 0;
        }
        ax = Input::GetAxis(0, 0x80, axisvalue);
        break;
    case SDLK_SPACE:
        if (backButtonBehavior != "none") {
            float x = backButtonBehavior == "left" ? 0.25f
                                                   : (backButtonBehavior == "right" ? 0.75f : 0.5f);
            // trigger a touchpad event so that the touchpad emulation for back button works
            controller->SetTouchpadState(0, true, x, 0.5f);
            button = OrbisPadButtonDataOffset::ORBIS_PAD_BUTTON_TOUCH_PAD;
        } else {
            button = 0;
        }
        break;
    case SDLK_F11:
        if (event->type == SDL_EVENT_KEY_DOWN) {
            {
                SDL_WindowFlags flag = SDL_GetWindowFlags(window);
                bool is_fullscreen = flag & SDL_WINDOW_FULLSCREEN;
                SDL_SetWindowFullscreen(window, !is_fullscreen);
            }
        }
        break;
    case SDLK_F12:
        if (event->type == SDL_EVENT_KEY_DOWN) {
            // Trigger rdoc capture
            VideoCore::TriggerCapture();
        }
        break;
    default:
        break;
    }
    if (button != 0) {
        controller->CheckButton(0, button, event->type == SDL_EVENT_KEY_DOWN);
    }
    if (axis != Input::Axis::AxisMax) {
        controller->Axis(0, axis, ax);
    }
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

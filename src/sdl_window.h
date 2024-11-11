// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <string>
#include "common/types.h"

#include <SDL3/SDL_events.h>

struct SDL_Window;
struct SDL_Gamepad;
union SDL_Event;

namespace Input {
class GameController;
}

namespace Frontend {

class KeyBinding {
public:
    Uint32 key;
    SDL_Keymod modifier;
    KeyBinding(SDL_Keycode k, SDL_Keymod m) : key(k), modifier(m){};
    KeyBinding(const SDL_Event* event);
    bool operator<(const KeyBinding& other) const;
    ~KeyBinding(){};
};

enum class WindowSystemType : u8 {
    Headless,
    Windows,
    X11,
    Wayland,
    Metal,
};

struct WindowSystemInfo {
    // Connection to a display server. This is used on X11 and Wayland platforms.
    void* display_connection = nullptr;

    // Render surface. This is a pointer to the native window handle, which depends
    // on the platform. e.g. HWND for Windows, Window for X11. If the surface is
    // set to nullptr, the video backend will run in headless mode.
    void* render_surface = nullptr;

    // Scale of the render surface. For hidpi systems, this will be >1.
    float render_surface_scale = 1.0f;

    // Window system type. Determines which GL context or Vulkan WSI is used.
    WindowSystemType type = WindowSystemType::Headless;
};

class WindowSDL {
public:
    explicit WindowSDL(s32 width, s32 height, Input::GameController* controller,
                       std::string_view window_title);
    ~WindowSDL();

    s32 getWidth() const {
        return width;
    }

    s32 getHeight() const {
        return height;
    }

    bool isOpen() const {
        return is_open;
    }

    [[nodiscard]] SDL_Window* GetSdlWindow() const {
        return window;
    }

    WindowSystemInfo getWindowInfo() const {
        return window_info;
    }

    void waitEvent();
    void updateMouse();

private:
    void onResize();
    void onKeyPress(const SDL_Event* event);
    void onGamepadEvent(const SDL_Event* event);
    int sdlGamepadToOrbisButton(u8 button);

    void updateModKeyedInputsManually(KeyBinding& binding);
    void updateButton(KeyBinding& binding, u32 button, bool isPressed);
    static Uint32 keyRepeatCallback(void* param, Uint32 id, Uint32 interval);
    static Uint32 mousePolling(void* param, Uint32 id, Uint32 interval);

    void parseInputConfig(const std::string& filename);

private:
    s32 width;
    s32 height;
    Input::GameController* controller;
    WindowSystemInfo window_info{};
    SDL_Window* window{};
    bool is_shown{};
    bool is_open{true};
};

} // namespace Frontend

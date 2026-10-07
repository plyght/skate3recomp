#pragma once

// On-screen touch controls for iOS: a virtual Xbox 360 pad drawn over the
// game, fed into the guest through a rex input driver, with a drag-to-arrange
// layout editor (long-press the menu button). Hidden automatically while a
// physical controller is connected.

#include <filesystem>
#include <functional>
#include <memory>

#include <rex/input/input_system.h>

namespace rex::ui {
class ImGuiDrawer;
class Window;
}  // namespace rex::ui

namespace skate3::touch {

struct Callbacks {
  // Opens the in-game settings menu (the pad's menu button).
  std::function<void()> open_menu;
  // True while the settings menu (or any other modal UI) owns the screen.
  std::function<bool()> menu_visible;
};

class TouchControls;

// Wraps an input system factory so the touch pad driver is registered before
// the runtime starts polling (InputSystem::AddDriver is not thread-safe).
std::unique_ptr<rex::input::InputSystem> AddTouchDriver(
    std::unique_ptr<rex::input::InputSystem> input_system);

// Creates the overlay and starts listening for touches. Call once the window
// and imgui drawer exist. The layout is stored in layout_path.
void Attach(rex::ui::ImGuiDrawer* drawer, rex::ui::Window* window,
            const std::filesystem::path& layout_path, Callbacks callbacks);
void Detach();

}  // namespace skate3::touch

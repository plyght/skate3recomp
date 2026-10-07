#include "skate3_touch_controls.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <SDL3/SDL.h>
#include <imgui.h>
#include <toml++/toml.hpp>

#include <rex/cvar.h>
#include <rex/input/input_driver.h>
#include <rex/logging.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>

REXCVAR_DEFINE_BOOL(touch_controls, true, "Controls",
                    "Show the on-screen touch controls (hidden while a controller is connected)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace skate3::touch {
namespace {

using Clock = std::chrono::steady_clock;

enum class Kind { kStick, kButton, kTrigger, kDpad, kMenu };

// Static description of each control; positions/sizes live in the layout.
struct ControlSpec {
  const char* id;
  const char* label;
  Kind kind;
  uint16_t button;  // X_INPUT_GAMEPAD_* for kButton
  bool right;       // right stick / right trigger
  // Defaults: center (x of width, y of height) and diameter (of height).
  float x, y, size;
};

// Default layout for a ~19.5:9 landscape phone, clear of the Dynamic Island
// and the rounded corners. Right stick sits low and large for Flick-It.
constexpr ControlSpec kSpecs[] = {
    {"left_stick", "", Kind::kStick, 0, false, 0.14f, 0.68f, 0.40f},
    {"right_stick", "", Kind::kStick, 0, true, 0.70f, 0.70f, 0.44f},
    {"a", "A", Kind::kButton, rex::input::X_INPUT_GAMEPAD_A, false, 0.900f, 0.54f, 0.13f},
    {"b", "B", Kind::kButton, rex::input::X_INPUT_GAMEPAD_B, false, 0.950f, 0.41f, 0.13f},
    {"x", "X", Kind::kButton, rex::input::X_INPUT_GAMEPAD_X, false, 0.850f, 0.41f, 0.13f},
    {"y", "Y", Kind::kButton, rex::input::X_INPUT_GAMEPAD_Y, false, 0.900f, 0.28f, 0.13f},
    {"lt", "LT", Kind::kTrigger, 0, false, 0.08f, 0.13f, 0.14f},
    {"lb", "LB", Kind::kButton, rex::input::X_INPUT_GAMEPAD_LEFT_SHOULDER, false, 0.17f, 0.13f,
     0.13f},
    {"rb", "RB", Kind::kButton, rex::input::X_INPUT_GAMEPAD_RIGHT_SHOULDER, false, 0.83f, 0.13f,
     0.13f},
    {"rt", "RT", Kind::kTrigger, 0, true, 0.92f, 0.13f, 0.14f},
    {"back", "Back", Kind::kButton, rex::input::X_INPUT_GAMEPAD_BACK, false, 0.40f, 0.09f, 0.10f},
    {"menu", "Menu", Kind::kMenu, 0, false, 0.50f, 0.09f, 0.10f},
    {"start", "Start", Kind::kButton, rex::input::X_INPUT_GAMEPAD_START, false, 0.60f, 0.09f,
     0.10f},
    {"ls", "L3", Kind::kButton, rex::input::X_INPUT_GAMEPAD_LEFT_THUMB, false, 0.29f, 0.88f,
     0.10f},
    {"rs", "R3", Kind::kButton, rex::input::X_INPUT_GAMEPAD_RIGHT_THUMB, false, 0.49f, 0.88f,
     0.10f},
    {"dpad", "", Kind::kDpad, 0, false, 0.30f, 0.42f, 0.22f},
};
constexpr size_t kControlCount = std::size(kSpecs);

struct ControlLayout {
  float x, y, size;
  bool visible = true;
};

struct Layout {
  float opacity = 0.5f;
  float scale = 1.0f;
  std::array<ControlLayout, kControlCount> controls;
};

Layout DefaultLayout() {
  Layout layout;
  for (size_t i = 0; i < kControlCount; ++i) {
    layout.controls[i] = {kSpecs[i].x, kSpecs[i].y, kSpecs[i].size, true};
  }
  return layout;
}

// A finger currently bound to a control.
struct FingerBinding {
  size_t control;
  float origin_x, origin_y;  // stick: where the finger landed
  float x, y;                // latest position
  Clock::time_point down_time;
};

constexpr auto kMenuLongPress = std::chrono::milliseconds(700);
// Stick deflection reaches full scale at this fraction of the stick radius.
constexpr float kStickTravel = 0.85f;

}  // namespace

class TouchControls : public rex::ui::WindowInputListener {
 public:
  explicit TouchControls() : layout_(DefaultLayout()) {}

  void SetLayoutPath(std::filesystem::path path) {
    std::lock_guard lock(mutex_);
    layout_path_ = std::move(path);
    LoadLocked();
  }
  void SetCallbacks(Callbacks callbacks) { callbacks_ = std::move(callbacks); }

  // ---- Guest-thread side ----------------------------------------------------

  bool Connected() const { return active_.load(std::memory_order_relaxed); }

  void FillGamepad(rex::input::X_INPUT_GAMEPAD& pad) {
    std::lock_guard lock(mutex_);
    uint16_t buttons = 0;
    uint8_t lt = 0, rt = 0;
    float lx = 0, ly = 0, rx = 0, ry = 0;
    if (!editing_) {
      for (const auto& [id, finger] : fingers_) {
        const ControlSpec& spec = kSpecs[finger.control];
        switch (spec.kind) {
          case Kind::kButton:
            buttons |= spec.button;
            break;
          case Kind::kTrigger:
            (spec.right ? rt : lt) = 0xFF;
            break;
          case Kind::kStick: {
            float sx, sy;
            StickVectorLocked(finger, sx, sy);
            (spec.right ? rx : lx) = sx;
            (spec.right ? ry : ly) = sy;
            break;
          }
          case Kind::kDpad:
            buttons |= DpadButtonsLocked(finger);
            break;
          case Kind::kMenu:
            break;
        }
      }
    }
    auto axis = [](float v) {
      return int16_t(std::clamp(v, -1.0f, 1.0f) * 32767.0f);
    };
    pad.buttons = buttons;
    pad.left_trigger = lt;
    pad.right_trigger = rt;
    pad.thumb_lx = axis(lx);
    pad.thumb_ly = axis(ly);
    pad.thumb_rx = axis(rx);
    pad.thumb_ry = axis(ry);
  }

  // ---- UI-thread side --------------------------------------------------------

  void OnTouchEvent(rex::ui::TouchEvent& e) override {
    using Action = rex::ui::TouchEvent::Action;
    const uint32_t id = e.pointer_id();
    std::lock_guard lock(mutex_);

    if (e.action() == Action::kDown) {
      if (!visible_) {
        // A touch while hidden for a connected controller brings the pad back
        // (the player put the controller down).
        if (hidden_for_gamepad_ && !MenuVisible()) {
          shown_despite_gamepad_ = true;
        }
        return;
      }
      if (editing_ && PointInRect(e.x(), e.y(), toolbar_min_, toolbar_max_)) {
        return;  // imgui handles the editor toolbar.
      }
      const int hit = HitTestLocked(e.x(), e.y(), editing_);
      if (hit < 0) {
        return;
      }
      fingers_[id] = {size_t(hit), e.x(), e.y(), e.x(), e.y(), Clock::now()};
      if (editing_) {
        selected_ = hit;
      }
      e.set_handled(true);
      return;
    }

    auto it = fingers_.find(id);
    if (it == fingers_.end()) {
      return;
    }
    FingerBinding& finger = it->second;
    e.set_handled(true);

    if (e.action() == Action::kMove) {
      if (editing_) {
        ControlLayout& c = layout_.controls[finger.control];
        c.x = std::clamp(c.x + (e.x() - finger.x) / display_w_, 0.02f, 0.98f);
        c.y = std::clamp(c.y + (e.y() - finger.y) / display_h_, 0.02f, 0.98f);
      } else if (kSpecs[finger.control].kind == Kind::kButton ||
                 kSpecs[finger.control].kind == Kind::kTrigger) {
        // Slide between face buttons and bumpers without lifting.
        const int hit = HitTestLocked(e.x(), e.y(), false);
        if (hit >= 0 && (kSpecs[hit].kind == Kind::kButton || kSpecs[hit].kind == Kind::kTrigger)) {
          finger.control = size_t(hit);
        }
      }
      finger.x = e.x();
      finger.y = e.y();
      return;
    }

    // Up / cancel.
    const bool tap = e.action() == Action::kUp;
    const size_t control = finger.control;
    const bool long_press = Clock::now() - finger.down_time >= kMenuLongPress;
    fingers_.erase(it);
    if (editing_) {
      Save();
      return;
    }
    if (tap && kSpecs[control].kind == Kind::kMenu && !long_press && callbacks_.open_menu) {
      pending_open_menu_ = true;
    }
  }

  // Called every frame from the overlay dialog.
  void Draw(ImGuiIO& io) {
    std::function<void()> open_menu;
    {
      std::lock_guard lock(mutex_);
      display_w_ = std::max(io.DisplaySize.x, 1.0f);
      display_h_ = std::max(io.DisplaySize.y, 1.0f);
      UpdateVisibilityLocked();

      // Long-pressing the menu button opens the layout editor.
      for (auto& [id, finger] : fingers_) {
        if (!editing_ && kSpecs[finger.control].kind == Kind::kMenu &&
            Clock::now() - finger.down_time >= kMenuLongPress) {
          editing_ = true;
          selected_ = int(finger.control);
        }
      }
      if (pending_open_menu_) {
        pending_open_menu_ = false;
        open_menu = callbacks_.open_menu;
      }
      if (visible_) {
        DrawControlsLocked();
        if (editing_) {
          DrawEditorLocked();
        }
      }
    }
    if (open_menu) {
      open_menu();
    }
  }

  bool editing() const { return editing_; }

 private:
  bool MenuVisible() const { return callbacks_.menu_visible && callbacks_.menu_visible(); }

  void UpdateVisibilityLocked() {
    const bool gamepad = SDL_HasGamepad();
    if (gamepad && !had_gamepad_) {
      shown_despite_gamepad_ = false;  // A newly connected controller hides the pad.
    }
    had_gamepad_ = gamepad;
    hidden_for_gamepad_ = gamepad && !shown_despite_gamepad_;
    const bool visible =
        REXCVAR_GET(touch_controls) && !MenuVisible() && (!hidden_for_gamepad_ || editing_);
    if (!visible && visible_) {
      fingers_.clear();  // Release everything so no input sticks.
    }
    visible_ = visible;
    active_.store(visible_, std::memory_order_relaxed);
  }

  float RadiusPx(size_t i) const {
    return layout_.controls[i].size * layout_.scale * display_h_ * 0.5f;
  }
  float CenterX(size_t i) const { return layout_.controls[i].x * display_w_; }
  float CenterY(size_t i) const { return layout_.controls[i].y * display_h_; }

  int HitTestLocked(float x, float y, bool include_hidden) const {
    // Smallest containing control wins, so buttons near a stick zone stay
    // reachable. Buttons get a slightly generous hit radius.
    int best = -1;
    float best_radius = 0;
    for (size_t i = 0; i < kControlCount; ++i) {
      if (!layout_.controls[i].visible && !include_hidden) {
        continue;
      }
      float r = RadiusPx(i);
      if (kSpecs[i].kind != Kind::kStick) {
        r *= 1.15f;
      }
      const float dx = x - CenterX(i), dy = y - CenterY(i);
      if (dx * dx + dy * dy <= r * r && (best < 0 || r < best_radius)) {
        best = int(i);
        best_radius = r;
      }
    }
    return best;
  }

  void StickVectorLocked(const FingerBinding& f, float& sx, float& sy) const {
    // Floating stick: the origin is where the finger landed, so a flick starts
    // from neutral wherever the thumb comes down.
    const float travel = std::max(RadiusPx(f.control) * kStickTravel, 1.0f);
    sx = (f.x - f.origin_x) / travel;
    sy = -(f.y - f.origin_y) / travel;  // Screen y grows downward.
    const float len = std::sqrt(sx * sx + sy * sy);
    if (len > 1.0f) {
      sx /= len;
      sy /= len;
    }
  }

  uint16_t DpadButtonsLocked(const FingerBinding& f) const {
    const float dx = f.x - CenterX(f.control), dy = f.y - CenterY(f.control);
    if (dx * dx + dy * dy < 0.04f * RadiusPx(f.control) * RadiusPx(f.control)) {
      return 0;  // Dead center.
    }
    // 8-way: 45-degree sectors, diagonals press both directions.
    const float angle = std::atan2(-dy, dx);  // Counter-clockwise from right.
    const int sector = int(std::lround(angle / (3.14159265f / 4.0f)) + 8) % 8;
    using namespace rex::input;
    constexpr uint16_t kSectors[8] = {
        X_INPUT_GAMEPAD_DPAD_RIGHT,
        X_INPUT_GAMEPAD_DPAD_RIGHT | X_INPUT_GAMEPAD_DPAD_UP,
        X_INPUT_GAMEPAD_DPAD_UP,
        X_INPUT_GAMEPAD_DPAD_UP | X_INPUT_GAMEPAD_DPAD_LEFT,
        X_INPUT_GAMEPAD_DPAD_LEFT,
        X_INPUT_GAMEPAD_DPAD_LEFT | X_INPUT_GAMEPAD_DPAD_DOWN,
        X_INPUT_GAMEPAD_DPAD_DOWN,
        X_INPUT_GAMEPAD_DPAD_DOWN | X_INPUT_GAMEPAD_DPAD_RIGHT,
    };
    return kSectors[sector];
  }

  const FingerBinding* FingerOnLocked(size_t control) const {
    for (const auto& [id, finger] : fingers_) {
      if (finger.control == control) {
        return &finger;
      }
    }
    return nullptr;
  }

  void DrawControlsLocked() {
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    const float alpha = std::clamp(layout_.opacity, 0.1f, 1.0f);
    auto col = [alpha](int r, int g, int b, float a) {
      return IM_COL32(r, g, b, int(std::clamp(a * alpha, 0.0f, 1.0f) * 255.0f));
    };
    for (size_t i = 0; i < kControlCount; ++i) {
      const bool shown = layout_.controls[i].visible;
      if (!shown && !editing_) {
        continue;
      }
      const ControlSpec& spec = kSpecs[i];
      const ImVec2 c(CenterX(i), CenterY(i));
      const float r = RadiusPx(i);
      const FingerBinding* finger = FingerOnLocked(i);
      const bool pressed = finger != nullptr && !editing_;
      const float fill = shown ? (pressed ? 0.75f : 0.35f) : 0.12f;
      const float line = shown ? 0.9f : 0.35f;

      switch (spec.kind) {
        case Kind::kStick: {
          draw->AddCircleFilled(c, r, col(20, 20, 24, fill * 0.6f), 48);
          draw->AddCircle(c, r, col(255, 255, 255, line * 0.6f), 48, 2.0f);
          ImVec2 knob = c;
          if (pressed) {
            float sx, sy;
            StickVectorLocked(*finger, sx, sy);
            knob = ImVec2(c.x + sx * r * kStickTravel, c.y - sy * r * kStickTravel);
          }
          draw->AddCircleFilled(knob, r * 0.38f, col(235, 235, 240, pressed ? 0.85f : 0.55f), 32);
          if (spec.right) {
            const char* hint = "FLICK";
            const ImVec2 ts = ImGui::CalcTextSize(hint);
            draw->AddText(ImVec2(c.x - ts.x * 0.5f, c.y + r * 0.55f), col(255, 255, 255, 0.5f),
                          hint);
          }
          break;
        }
        case Kind::kDpad: {
          const float arm = r * 0.36f;
          const ImU32 fill_col = col(20, 20, 24, fill);
          draw->AddRectFilled(ImVec2(c.x - arm, c.y - r), ImVec2(c.x + arm, c.y + r), fill_col,
                              arm * 0.4f);
          draw->AddRectFilled(ImVec2(c.x - r, c.y - arm), ImVec2(c.x + r, c.y + arm), fill_col,
                              arm * 0.4f);
          const uint16_t held = pressed ? DpadButtonsLocked(*finger) : 0;
          using namespace rex::input;
          const struct {
            uint16_t bit;
            float dx, dy;
          } arrows[] = {{X_INPUT_GAMEPAD_DPAD_UP, 0, -1},
                        {X_INPUT_GAMEPAD_DPAD_DOWN, 0, 1},
                        {X_INPUT_GAMEPAD_DPAD_LEFT, -1, 0},
                        {X_INPUT_GAMEPAD_DPAD_RIGHT, 1, 0}};
          for (const auto& a : arrows) {
            const ImVec2 p(c.x + a.dx * r * 0.68f, c.y + a.dy * r * 0.68f);
            draw->AddCircleFilled(p, arm * 0.55f,
                                  col(255, 255, 255, (held & a.bit) ? 0.95f : line * 0.5f), 16);
          }
          break;
        }
        default: {
          ImU32 tint = col(20, 20, 24, fill);
          if (spec.kind == Kind::kButton) {
            switch (spec.button) {
              case rex::input::X_INPUT_GAMEPAD_A: tint = col(60, 170, 70, fill + 0.1f); break;
              case rex::input::X_INPUT_GAMEPAD_B: tint = col(200, 60, 55, fill + 0.1f); break;
              case rex::input::X_INPUT_GAMEPAD_X: tint = col(50, 110, 210, fill + 0.1f); break;
              case rex::input::X_INPUT_GAMEPAD_Y: tint = col(220, 180, 40, fill + 0.1f); break;
              default: break;
            }
          }
          draw->AddCircleFilled(c, r, tint, 32);
          draw->AddCircle(c, r, col(255, 255, 255, line * 0.7f), 32, 2.0f);
          const ImVec2 ts = ImGui::CalcTextSize(spec.label);
          draw->AddText(ImVec2(c.x - ts.x * 0.5f, c.y - ts.y * 0.5f),
                        col(255, 255, 255, shown ? 1.0f : 0.5f), spec.label);
          break;
        }
      }
      if (editing_ && int(i) == selected_) {
        draw->AddCircle(c, r + 6.0f, IM_COL32(255, 210, 60, 255), 48, 3.0f);
      }
    }
  }

  void DrawEditorLocked() {
    const ImGuiWindowFlags flags = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                   ImGuiWindowFlags_AlwaysAutoResize |
                                   ImGuiWindowFlags_NoSavedSettings |
                                   ImGuiWindowFlags_NoFocusOnAppearing;
    ImGui::SetNextWindowPos(ImVec2(display_w_ * 0.5f, display_h_ * 0.5f), ImGuiCond_Always,
                            ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowBgAlpha(0.85f);
    if (ImGui::Begin("##skate3_touch_layout_editor", nullptr, flags)) {
      ImGui::TextUnformatted("Touch layout: drag controls to move them");
      const char* name = selected_ >= 0 ? kSpecs[selected_].id : "none";
      ImGui::Text("Selected: %s", name);
      const ImVec2 button(display_h_ * 0.16f, display_h_ * 0.08f);
      if (selected_ >= 0) {
        ControlLayout& c = layout_.controls[size_t(selected_)];
        if (ImGui::Button("Smaller", button)) c.size = std::max(0.05f, c.size * 0.9f);
        ImGui::SameLine();
        if (ImGui::Button("Bigger", button)) c.size = std::min(0.8f, c.size * 1.1f);
        ImGui::SameLine();
        if (ImGui::Button(c.visible ? "Hide" : "Show", button)) c.visible = !c.visible;
      }
      if (ImGui::Button("Fainter", button)) layout_.opacity = std::max(0.1f, layout_.opacity - 0.1f);
      ImGui::SameLine();
      if (ImGui::Button("Bolder", button)) layout_.opacity = std::min(1.0f, layout_.opacity + 0.1f);
      ImGui::SameLine();
      ImGui::Text("Opacity %d%%", int(layout_.opacity * 100.0f + 0.5f));
      if (ImGui::Button("All smaller", button)) layout_.scale = std::max(0.5f, layout_.scale * 0.92f);
      ImGui::SameLine();
      if (ImGui::Button("All bigger", button)) layout_.scale = std::min(2.0f, layout_.scale * 1.08f);
      ImGui::SameLine();
      if (ImGui::Button("Reset", button)) {
        layout_ = DefaultLayout();
        selected_ = -1;
      }
      if (ImGui::Button("Done", ImVec2(button.x * 3.0f + 16.0f, button.y))) {
        editing_ = false;
        selected_ = -1;
        fingers_.clear();
        Save();
      }
      const ImVec2 pos = ImGui::GetWindowPos();
      const ImVec2 size = ImGui::GetWindowSize();
      toolbar_min_ = pos;
      toolbar_max_ = ImVec2(pos.x + size.x, pos.y + size.y);
    }
    ImGui::End();
  }

  static bool PointInRect(float x, float y, ImVec2 min, ImVec2 max) {
    return x >= min.x && y >= min.y && x <= max.x && y <= max.y;
  }

  void LoadLocked() {
    layout_ = DefaultLayout();
    std::error_code ec;
    if (layout_path_.empty() || !std::filesystem::exists(layout_path_, ec)) {
      return;
    }
    try {
      const toml::table table = toml::parse_file(layout_path_.string());
      layout_.opacity = float(table["opacity"].value_or(double(layout_.opacity)));
      layout_.scale = float(table["scale"].value_or(double(layout_.scale)));
      if (const auto* controls = table["controls"].as_table()) {
        for (size_t i = 0; i < kControlCount; ++i) {
          const auto* entry = (*controls)[kSpecs[i].id].as_table();
          if (!entry) {
            continue;
          }
          ControlLayout& c = layout_.controls[i];
          c.x = std::clamp(float((*entry)["x"].value_or(double(c.x))), 0.0f, 1.0f);
          c.y = std::clamp(float((*entry)["y"].value_or(double(c.y))), 0.0f, 1.0f);
          c.size = std::clamp(float((*entry)["size"].value_or(double(c.size))), 0.03f, 0.9f);
          c.visible = (*entry)["visible"].value_or(c.visible);
        }
      }
    } catch (const toml::parse_error& error) {
      REXLOG_WARN("Ignoring unreadable touch layout {}: {}", layout_path_.string(),
                  std::string(error.description()));
      layout_ = DefaultLayout();
    }
  }

  void Save() const {
    if (layout_path_.empty()) {
      return;
    }
    toml::table controls;
    for (size_t i = 0; i < kControlCount; ++i) {
      const ControlLayout& c = layout_.controls[i];
      controls.insert(kSpecs[i].id, toml::table{{"x", double(c.x)},
                                                {"y", double(c.y)},
                                                {"size", double(c.size)},
                                                {"visible", c.visible}});
    }
    toml::table root{{"opacity", double(layout_.opacity)}, {"scale", double(layout_.scale)}};
    root.insert("controls", std::move(controls));
    std::error_code ec;
    std::filesystem::create_directories(layout_path_.parent_path(), ec);
    std::ofstream out(layout_path_, std::ios::trunc);
    out << root << "\n";
  }

  std::mutex mutex_;
  Layout layout_;
  std::filesystem::path layout_path_;
  Callbacks callbacks_;
  std::unordered_map<uint32_t, FingerBinding> fingers_;
  std::atomic<bool> active_{false};
  bool visible_ = false;
  std::atomic<bool> editing_{false};
  bool had_gamepad_ = false;
  bool hidden_for_gamepad_ = false;
  bool shown_despite_gamepad_ = false;
  bool pending_open_menu_ = false;
  int selected_ = -1;
  float display_w_ = 1.0f, display_h_ = 1.0f;
  ImVec2 toolbar_min_{0, 0}, toolbar_max_{0, 0};
};

namespace {

std::shared_ptr<TouchControls>& Shared() {
  static std::shared_ptr<TouchControls> controls = std::make_shared<TouchControls>();
  return controls;
}

using rex::X_RESULT;
using rex::X_STATUS;

class TouchInputDriver final : public rex::input::InputDriver {
 public:
  explicit TouchInputDriver(std::shared_ptr<TouchControls> controls)
      : InputDriver(nullptr, 0), controls_(std::move(controls)) {}

  X_STATUS Setup() override { return X_STATUS_SUCCESS; }

  X_RESULT GetCapabilities(uint32_t user_index, uint32_t,
                           rex::input::X_INPUT_CAPABILITIES* out_caps) override {
    if (user_index != 0 || !controls_->Connected()) {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    if (out_caps) {
      std::memset(out_caps, 0, sizeof(*out_caps));
      out_caps->type = 0x01;
      out_caps->sub_type = 0x01;
      out_caps->gamepad.buttons = 0xFFFF;
      out_caps->gamepad.left_trigger = 0xFF;
      out_caps->gamepad.right_trigger = 0xFF;
      out_caps->gamepad.thumb_lx = int16_t(0x7FFF);
      out_caps->gamepad.thumb_ly = int16_t(0x7FFF);
      out_caps->gamepad.thumb_rx = int16_t(0x7FFF);
      out_caps->gamepad.thumb_ry = int16_t(0x7FFF);
    }
    return X_ERROR_SUCCESS;
  }

  X_RESULT GetState(uint32_t user_index, rex::input::X_INPUT_STATE* out_state) override {
    // Not connected while hidden, so a neutral touch pad never merges over a
    // real controller (InputSystem ORs every connected driver).
    if (user_index != 0 || !controls_->Connected()) {
      return X_ERROR_DEVICE_NOT_CONNECTED;
    }
    if (out_state) {
      std::memset(out_state, 0, sizeof(*out_state));
      if (is_active()) {
        controls_->FillGamepad(out_state->gamepad);
      }
      out_state->packet_number = ++packet_number_;
    }
    return X_ERROR_SUCCESS;
  }

  // Touches reach the settings UI through imgui directly.
  X_RESULT GetStateUi(uint32_t, rex::input::X_INPUT_STATE*) override {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }
  X_RESULT SetState(uint32_t user_index, rex::input::X_INPUT_VIBRATION*) override {
    return user_index == 0 && controls_->Connected() ? X_ERROR_SUCCESS
                                                     : X_ERROR_DEVICE_NOT_CONNECTED;
  }
  X_RESULT GetKeystroke(uint32_t, uint32_t, rex::input::X_INPUT_KEYSTROKE*) override {
    return X_ERROR_DEVICE_NOT_CONNECTED;
  }

 private:
  std::shared_ptr<TouchControls> controls_;
  uint32_t packet_number_ = 0;
};

class TouchOverlayDialog final : public rex::ui::ImGuiDialog {
 public:
  TouchOverlayDialog(rex::ui::ImGuiDrawer* drawer, std::shared_ptr<TouchControls> controls)
      : ImGuiDialog(drawer), controls_(std::move(controls)) {}

  // Guest frames already repaint at the display rate; only the editor (which
  // can be open while the guest is idle in a menu) needs continuous repaint.
  bool WantsContinuousRepaint() const override { return controls_->editing(); }

 protected:
  void OnDraw(ImGuiIO& io) override { controls_->Draw(io); }

 private:
  std::shared_ptr<TouchControls> controls_;
};

std::unique_ptr<TouchOverlayDialog> g_overlay;
rex::ui::Window* g_window = nullptr;

// Above the imgui drawer (z 64) so touches on controls never reach imgui;
// touches elsewhere fall through to it (and to the settings menu).
constexpr size_t kTouchListenerZOrder = 128;

}  // namespace

std::unique_ptr<rex::input::InputSystem> AddTouchDriver(
    std::unique_ptr<rex::input::InputSystem> input_system) {
  if (input_system) {
    input_system->AddDriver(std::make_unique<TouchInputDriver>(Shared()));
  }
  return input_system;
}

void Attach(rex::ui::ImGuiDrawer* drawer, rex::ui::Window* window,
            const std::filesystem::path& layout_path, Callbacks callbacks) {
  if (!drawer || !window || g_overlay) {
    return;
  }
  auto& controls = Shared();
  controls->SetLayoutPath(layout_path);
  controls->SetCallbacks(std::move(callbacks));
  window->AddInputListener(controls.get(), kTouchListenerZOrder);
  g_window = window;
  g_overlay = std::make_unique<TouchOverlayDialog>(drawer, controls);
  REXLOG_INFO("Touch controls attached (layout {})", layout_path.string());
}

void Detach() {
  if (g_window) {
    g_window->RemoveInputListener(Shared().get());
    g_window = nullptr;
  }
  g_overlay.reset();
}

}  // namespace skate3::touch

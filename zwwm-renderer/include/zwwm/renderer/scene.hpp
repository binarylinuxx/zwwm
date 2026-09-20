#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <vector>

namespace zwwm::renderer {

struct Point {
  std::int32_t x = 0;
  std::int32_t y = 0;
};

struct Size {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
};

struct Rect {
  Point origin;
  Size size;

  [[nodiscard]] bool empty() const;
  [[nodiscard]] bool intersects(const Rect& other) const;
};

class DamageRegion {
 public:
  void add(Rect damage);
  void clear();
  [[nodiscard]] bool empty() const;
  [[nodiscard]] bool intersects(const Rect& bounds) const;
  [[nodiscard]] const std::vector<Rect>& rectangles() const;

 private:
  std::vector<Rect> rectangles_;
};

using NodeId = std::uint64_t;
using TextureHandle = std::uint64_t;

enum class Layer : std::uint8_t {
  background,
  bottom,
  toplevel,
  top,
  overlay,
};

struct SceneNode {
  NodeId id = 0;
  Layer layer = Layer::toplevel;
  std::uint32_t z_index = 0;
  Rect bounds;
  float opacity = 1.0F;
  std::array<float, 4> color{1.0F, 1.0F, 1.0F, 1.0F};
  std::optional<TextureHandle> texture;
  bool visible = true;
  float corner_radius = 0.0F;
};

// Scene owns presentation order only. Buffer import and GL texture ownership
// belong to the OpenGL backend added after the Wayland buffer lifecycle exists.
class Scene {
 public:
  [[nodiscard]] bool upsert(SceneNode node);
  [[nodiscard]] bool remove(NodeId id);
  [[nodiscard]] const SceneNode* get(NodeId id) const;
  [[nodiscard]] std::vector<SceneNode> ordered_nodes() const;

 private:
  struct StoredNode {
    SceneNode node;
    std::uint64_t insertion_order = 0;
  };

  std::vector<StoredNode> nodes_;
  std::uint64_t next_insertion_order_ = 0;
};

}  // namespace zwwm::renderer

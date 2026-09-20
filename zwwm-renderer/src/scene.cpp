#include "zwwm/renderer/scene.hpp"

#include <algorithm>
#include <cmath>
#include <limits>

namespace zwwm::renderer {

bool Rect::empty() const { return size.width == 0 || size.height == 0; }

bool Rect::intersects(const Rect& other) const {
  if (empty() || other.empty()) {
    return false;
  }
  const auto right = static_cast<std::int64_t>(origin.x) + size.width;
  const auto bottom = static_cast<std::int64_t>(origin.y) + size.height;
  const auto other_right = static_cast<std::int64_t>(other.origin.x) + other.size.width;
  const auto other_bottom = static_cast<std::int64_t>(other.origin.y) + other.size.height;
  return origin.x < other_right && other.origin.x < right && origin.y < other_bottom && other.origin.y < bottom;
}

void DamageRegion::add(Rect damage) {
  if (!damage.empty()) {
    rectangles_.push_back(damage);
  }
}

void DamageRegion::clear() { rectangles_.clear(); }
bool DamageRegion::empty() const { return rectangles_.empty(); }

bool DamageRegion::intersects(const Rect& bounds) const {
  return std::any_of(rectangles_.begin(), rectangles_.end(), [&bounds](const Rect& damage) {
    return damage.intersects(bounds);
  });
}

const std::vector<Rect>& DamageRegion::rectangles() const { return rectangles_; }

bool Scene::upsert(SceneNode node) {
  const bool valid_color = std::all_of(node.color.begin(), node.color.end(), [](float channel) {
    return std::isfinite(channel) && channel >= 0.0F && channel <= 1.0F;
  });
  if (node.id == 0 || node.bounds.empty() || !std::isfinite(node.opacity) || node.opacity < 0.0F || node.opacity > 1.0F ||
      !valid_color || (node.texture.has_value() && *node.texture == 0) || !std::isfinite(node.corner_radius) ||
      node.corner_radius < 0.0F) {
    return false;
  }
  const auto existing = std::find_if(nodes_.begin(), nodes_.end(), [id = node.id](const StoredNode& stored) {
    return stored.node.id == id;
  });
  if (existing != nodes_.end()) {
    existing->node = node;
    return true;
  }
  nodes_.push_back({node, next_insertion_order_++});
  return true;
}

bool Scene::remove(NodeId id) {
  const auto existing = std::find_if(nodes_.begin(), nodes_.end(), [id](const StoredNode& stored) {
    return stored.node.id == id;
  });
  if (existing == nodes_.end()) {
    return false;
  }
  nodes_.erase(existing);
  return true;
}

const SceneNode* Scene::get(NodeId id) const {
  const auto existing = std::find_if(nodes_.begin(), nodes_.end(), [id](const StoredNode& stored) {
    return stored.node.id == id;
  });
  return existing == nodes_.end() ? nullptr : &existing->node;
}

std::vector<SceneNode> Scene::ordered_nodes() const {
  std::vector<const StoredNode*> ordered;
  ordered.reserve(nodes_.size());
  for (const StoredNode& node : nodes_) {
    ordered.push_back(&node);
  }
  std::stable_sort(ordered.begin(), ordered.end(), [](const StoredNode* left, const StoredNode* right) {
    if (left->node.layer != right->node.layer) {
      return left->node.layer < right->node.layer;
    }
    if (left->node.z_index != right->node.z_index) {
      return left->node.z_index < right->node.z_index;
    }
    return left->insertion_order < right->insertion_order;
  });

  std::vector<SceneNode> nodes;
  nodes.reserve(ordered.size());
  for (const StoredNode* node : ordered) {
    nodes.push_back(node->node);
  }
  return nodes;
}

}  // namespace zwwm::renderer

#include "zwwm/renderer/dmabuf.hpp"
#include "zwwm/renderer/opengl.hpp"

#include <cassert>
#include <limits>

using namespace zwwm::renderer;

namespace {

SceneNode make_node(NodeId id, Layer layer, std::uint32_t z_index, Rect bounds) {
  SceneNode node;
  node.id = id;
  node.layer = layer;
  node.z_index = z_index;
  node.bounds = bounds;
  return node;
}

}  // namespace

int main() {
  Scene scene;
  assert(scene.upsert(make_node(1, Layer::top, 0, {{0, 0}, {100, 100}})));
  assert(scene.upsert(make_node(2, Layer::background, 0, {{0, 0}, {100, 100}})));
  assert(scene.upsert(make_node(3, Layer::toplevel, 1, {{50, 50}, {100, 100}})));
  assert(!scene.upsert(make_node(0, Layer::toplevel, 0, {{0, 0}, {1, 1}})));
  assert(!scene.upsert(make_node(4, Layer::toplevel, 0, {{0, 0}, {0, 1}})));
  SceneNode invalid_color = make_node(5, Layer::toplevel, 0, {{0, 0}, {1, 1}});
  invalid_color.color[0] = -0.1F;
  assert(!scene.upsert(invalid_color));
  SceneNode invalid_radius = make_node(6, Layer::toplevel, 0, {{0, 0}, {1, 1}});
  invalid_radius.corner_radius = -0.1F;
  assert(!scene.upsert(invalid_radius));
  invalid_radius.corner_radius = std::numeric_limits<float>::infinity();
  assert(!scene.upsert(invalid_radius));
  invalid_radius.corner_radius = std::numeric_limits<float>::quiet_NaN();
  assert(!scene.upsert(invalid_radius));

  const auto ordered = scene.ordered_nodes();
  assert(ordered.size() == 3);
  assert(ordered[0].id == 2);
  assert(ordered[1].id == 3);
  assert(ordered[2].id == 1);

  DamageRegion damage;
  damage.add({{75, 75}, {10, 10}});
  OpenGlRenderer renderer;
  const FramePlan frame = renderer.build_frame(scene, damage);
  assert(frame.draws.size() == 3);
  assert(frame.draws[0].node_id == 2);
  assert(frame.draws[1].node_id == 3);
  assert(frame.draws[2].node_id == 1);
  assert(OpenGlRenderer::texture_target() == GL_TEXTURE_2D);
  assert(border_ring_contains({100, 60}, 14.0F, 4.0F, 50.0F, 2.0F));
  assert(!border_ring_contains({100, 60}, 14.0F, 4.0F, 50.0F, 30.0F));
  assert(border_ring_contains({100, 60}, 14.0F, 4.0F, 6.0F, 6.0F));
  assert(!border_ring_contains({100, 60}, 14.0F, 0.0F, 50.0F, 2.0F));

  Scene surface_scene;
  SceneNode surface = make_node(10, Layer::toplevel, 0, {{0, 0}, {20, 20}});
  surface.texture = 42;
  surface.corner_radius = 12.0F;
  assert(surface_scene.upsert(surface));
  assert(renderer.bind_texture(42, 77));
  DamageRegion surface_damage;
  surface_damage.add({{0, 0}, {20, 20}});
  const FramePlan surface_frame = renderer.build_frame(surface_scene, surface_damage);
  assert(surface_frame.draws.size() == 1);
  assert(surface_frame.draws[0].texture == 77);
  assert(surface_frame.draws[0].corner_radius == 10.0F);
  assert((surface_frame.draws[0].source_uv == std::array<float, 4>{0.0F, 0.0F, 1.0F, 1.0F}));
  renderer.unbind_texture(42);
  assert(!renderer.build_frame(surface_scene, surface_damage).draws[0].texture.has_value());

  Scene border_scene;
  SceneNode border = make_node(11, Layer::toplevel, 0, {{0, 0}, {100, 60}});
  border.corner_radius = 14.0F;
  assert(border_scene.upsert(border));
  SceneNode inset_surface = make_node(12, Layer::toplevel, 1, {{4, 4}, {92, 52}});
  inset_surface.texture = 42;
  inset_surface.corner_radius = 10.0F;
  assert(border_scene.upsert(inset_surface));
  assert(renderer.bind_texture(42, 77));
  const FramePlan border_frame = renderer.build_frame(border_scene, surface_damage);
  assert(border_frame.draws.size() == 2);
  assert(!border_frame.draws[0].texture.has_value());
  assert(border_frame.draws[0].corner_radius == 14.0F);
  assert(border_frame.draws[1].texture == 77);
  assert(border_frame.draws[1].corner_radius == 10.0F);
  renderer.unbind_texture(42);

  DmabufAttributes dmabuf{1920, 1080, 0x34325258, {{3, 7680, 0, 0}}};
  assert(dmabuf.valid());
  dmabuf.planes[0].stride = 0;
  assert(!dmabuf.valid());

  assert(scene.remove(3));
  assert(!scene.remove(3));
}

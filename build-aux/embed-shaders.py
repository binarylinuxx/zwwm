#!/usr/bin/env python3

import pathlib
import sys


def main() -> int:
    if len(sys.argv) != 9:
        raise SystemExit("usage: embed-shaders.py OUTPUT SURFACE_VERT SURFACE_FRAG KAWASE_VERT KAWASE_FRAG GLASS_FRAG GLASS_PHYSICS ROUNDED_RECT")

    output = pathlib.Path(sys.argv[1])
    surface_vertex = pathlib.Path(sys.argv[2]).read_text()
    surface_fragment = pathlib.Path(sys.argv[3]).read_text()
    kawase_vertex = pathlib.Path(sys.argv[4]).read_text()
    kawase_fragment = pathlib.Path(sys.argv[5]).read_text()
    glass_fragment = pathlib.Path(sys.argv[6]).read_text()
    glass_physics = pathlib.Path(sys.argv[7]).read_text()
    rounded_rect = pathlib.Path(sys.argv[8]).read_text()

    surface_fragment = surface_fragment.replace('#include "rounded_rect.glsl"', rounded_rect)
    glass_fragment = glass_fragment.replace('#include "rounded_rect.glsl"', rounded_rect)
    glass_fragment = glass_fragment.replace('#include "glass_physics.glsl"', glass_physics)

    values = {
        "surface_vertex": surface_vertex,
        "surface_fragment": surface_fragment,
        "glass_fragment": glass_fragment,
        "kawase_vertex": kawase_vertex,
        "kawase_fragment": kawase_fragment,
    }
    lines = ["#pragma once", "", "namespace zwwm::renderer::shader {", ""]
    for name, source in values.items():
        lines.extend((f'inline constexpr char {name}[] = R"zwwm(', source, ')zwwm";', ""))
    lines.append("}  // namespace zwwm::renderer::shader")
    output.write_text("\n".join(lines) + "\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

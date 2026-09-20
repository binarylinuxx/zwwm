# zwwm-renderer

`zwwm-renderer` owns backend-neutral scene ordering, damage planning, and
OpenGL 3.3 drawing. It has no wlroots dependency.

`Scene` stores visible surfaces and compositor decorations.
`OpenGlRenderer::build_frame()` converts damaged scene nodes into ordered draw
calls. Initialization, rendering, and shutdown run with the same current GL
context.

The renderer supports:

- Imported client textures and solid compositor geometry
- Premultiplied-alpha blending
- Antialiased rounded clipping
- Decoration borders composed from ordered scene nodes
- Offscreen scene rendering and dual-Kawase backdrop blur

Texture import remains backend-owned. `EglDmabufImporter` creates EGLImages and
GL textures from DMA-BUF attributes but does not own client plane file
descriptors.

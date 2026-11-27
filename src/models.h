#pragma once
#include <stdint.h>

/* A mesh for the 3-D viewer. scripts/stl2h.py fills MODELS[] in
 * models_gen.h from src/stl/*.stl: Y up, centred, unit bounding sphere. */
struct Model {
  const char *name;
  uint16_t nv, nt, ne;   /* vertices, triangles, wireframe edges */
  const float *v;        /* nv * xyz */
  const int8_t *vn;      /* nv * xyz, smooth vertex normals x127 */
  const int8_t *fn;      /* nt * xyz, face normals x127 */
  const uint16_t *tri;   /* nt * 3 vertex indices, CCW from outside */
  const uint16_t *edge;  /* ne * 2 vertex indices */
  uint32_t color;        /* base material colour, 0xRRGGBB */
};

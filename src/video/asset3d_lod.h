/* Stub of the OpenTTD_3D LOD library: no reduced meshes yet, so every model
 * stays at full detail and VehicleLOD3D resolves to the input model.
 */
#ifndef ASSET3D_LOD_H
#define ASSET3D_LOD_H
#include "asset3d_generated.h"
namespace ArtLOD3D {
using Asset3D::Face;
using Asset3D::Model;

inline constexpr Model models[] = {{Asset3D::missing, std::size(Asset3D::missing)}};

struct Entry { uint16_t model; float error; };
struct Levels { Entry level[2]; float radius, height; };
inline constexpr Levels levels[] = {{}};
}
#endif

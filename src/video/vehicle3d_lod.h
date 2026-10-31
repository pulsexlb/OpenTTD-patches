/* This file is part of OpenTTD and is licensed under the GNU General Public License version 2. */
/** @file vehicle3d_lod.h Conservative screen-space limits for optional reduced library meshes. */
#ifndef VIDEO_VEHICLE3D_LOD_H
#define VIDEO_VEHICLE3D_LOD_H
#include <algorithm>
#include <cmath>

namespace Terrain3D {
/** The largest singular value of the orthographic screen projection, in pixels/world unit.
 * Local X stretching is bounded separately; yaw and instance direction are rotations. */
inline float ModelPixelScale(float scale, float tilt, float length)
{
	return std::abs(scale) * std::max(1.0f, std::abs(length)) * std::max(std::sqrt(8.0f), std::sqrt(2 * tilt * tilt + 1));
}

/** Only distant models at most 64 pixels across, with at most half a pixel vertex displacement. */
inline bool ModelLODAllowed(float error, float radius, float height, float scale, float tilt, float length)
{
	float pixels = ModelPixelScale(scale, tilt, length);
	return scale < 1 && std::isfinite(pixels) && error * pixels <= 0.5f &&
		std::sqrt(4 * radius * radius + height * height) * pixels <= 64;
}
}
#endif

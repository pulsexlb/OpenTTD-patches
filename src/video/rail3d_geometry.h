/* This file is part of OpenTTD and is licensed under the GNU General Public License version 2. */
#ifndef RAIL3D_GEOMETRY_H
#define RAIL3D_GEOMETRY_H

namespace Rail3D {
/** Edge midpoints in engine DiagDirection order; track pairs in engine Track order. */
inline constexpr float EDGES[4][2] = {{0,8},{8,16},{16,8},{8,0}};
inline constexpr int PATHS[6][2] = {{0,2},{3,1},{0,3},{2,1},{3,2},{0,1}};
inline constexpr float DIRECTIONS[8][2] = {{-0.70710678f,-0.70710678f},{-1,0},{-0.70710678f,0.70710678f},{0,1},{0.70710678f,0.70710678f},{1,0},{0.70710678f,-0.70710678f},{0,-1}};
/** Intersect each offset rail edge with the tile boundaries before sampling it.
 * Clamping a perpendicular-offset centreline endpoint instead leaves a gap on
 * the inside of diagonal joins and bends the rail on the outside. */
inline std::array<float, 2> StripPoint(int track, float fraction, float side)
{
	const auto &a = EDGES[PATHS[track][0]], &b = EDGES[PATHS[track][1]];
	float dx = b[0] - a[0], dy = b[1] - a[1];
	float length = std::hypot(dx, dy);
	float nx = -dy / length, ny = dx / length;
	auto endpoint = [&](int edge) -> std::array<float, 2> {
		float x = EDGES[edge][0] + nx * side, y = EDGES[edge][1] + ny * side;
		if (edge == 0 || edge == 2) {
			y += (EDGES[edge][0] - x) * dy / dx;
			x = EDGES[edge][0];
		} else {
			x += (EDGES[edge][1] - y) * dx / dy;
			y = EDGES[edge][1];
		}
		return {x, y};
	};
	auto start = endpoint(PATHS[track][0]), end = endpoint(PATHS[track][1]);
	return {start[0] + (end[0] - start[0]) * fraction, start[1] + (end[1] - start[1]) * fraction};
}
/** Continuous bridge-ramp surface; direction uses the engine's DiagDirection order. */
inline float RampHeight(int direction, bool flat, int foundation_z, float x, float y)
{
	float along = direction==0 ? 16-x : direction==1 ? y : direction==2 ? x : 16-y;
	return foundation_z + (flat ? along*0.5f : 8.0f);
}
}
#endif

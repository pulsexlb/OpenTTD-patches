/* This file is part of OpenTTD and is licensed under the GNU General Public License version 2. */
/** @file terrain3d_math.h Shared orthographic and perspective cameras and triangle picking mathematics. */
#ifndef VIDEO_TERRAIN3D_MATH_H
#define VIDEO_TERRAIN3D_MATH_H
#include <array>
#include <algorithm>
#include <cmath>

namespace Terrain3D {
/** Inclusive chunk rectangle affected by a tile and its shared corners. */
inline std::array<int, 4> DirtyChunks(int x, int y, int width, int height, int chunk_size)
{
	return {std::max(0, x - 1) / chunk_size, std::max(0, y - 1) / chunk_size,
		std::min(width - 1, x + 1) / chunk_size, std::min(height - 1, y + 1) / chunk_size};
}
struct Vec {
	float x, y, z;
	bool operator==(const Vec &) const = default;
};
/** (cx, cy, pz) is the rotation pivot; any point on the classic centre ray gives the classic view at yaw 0, tilt 1.
 * A perspective camera (distance > 0) instead uses its view-projection matrix, and culls beyond its draw distance. */
struct Camera {
	float cx, cy, scale, width, height, cosine, sine, tilt, pz = 0;
	std::array<float, 16> matrix{}; ///< Column-major view-projection of a perspective camera, as the shaders take it.
	Vec eye{0, 0, 0}; ///< Perspective eye position.
	float distance = 0; ///< Perspective draw distance; 0 is orthographic.
	Vec hidden{0, 0, 0}; ///< A cab view's own vehicle, which it does not draw: the vehicle drawn exactly here.
	bool hides = false; ///< Whether #hidden is set.
	float dark = 0; ///< How far a perspective view has faded to black, 0 clear to 1 black, as in a tunnel.
	bool Perspective() const { return distance > 0; }
	/** Clip-space row of the perspective matrix. */
	std::array<float, 4> Row(int i) const { return {matrix[i], matrix[4 + i], matrix[8 + i], matrix[12 + i]}; }
	/** Screen position, and a depth that grows towards the viewer as the orthographic one does. */
	Vec Project(Vec p) const
	{
		if (Perspective()) {
			auto clip = [&](int i) { auto r = Row(i); return r[0] * p.x + r[1] * p.y + r[2] * p.z + r[3]; };
			float w = std::max(clip(3), 0.0001f);
			return {(clip(0) / w + 1) * width / 2, (1 - clip(1) / w) * height / 2, -clip(2) / w};
		}
		float x = (p.x - cx) * cosine - (p.y - cy) * sine;
		float y = (p.x - cx) * sine + (p.y - cy) * cosine;
		return {width / 2 + 2 * (y - x) * scale, height / 2 + ((x + y) * tilt - (p.z - pz)) * scale, x + y + 2 * tilt * p.z};
	}
};
/** Conservative orthographic pointer test for a world-space box. */
inline bool HitBox(const Camera &camera, Vec low, Vec high, float x, float y)
{
	float left = 1e30f, top = 1e30f, right = -1e30f, bottom = -1e30f;
	for (int i = 0; i < 8; ++i) {
		auto p = camera.Project({(i & 1) ? high.x : low.x, (i & 2) ? high.y : low.y, (i & 4) ? high.z : low.z});
		left = std::min(left, p.x); right = std::max(right, p.x);
		top = std::min(top, p.y); bottom = std::max(bottom, p.y);
	}
	return x >= left && x <= right && y >= top && y <= bottom;
}

/** Yaw in degrees that turns a vehicle facing `direction` (0 = DIR_N, clockwise in eighths) to head up the screen. */
inline float HeadingYaw(int direction)
{
	return 45.0f * direction;
}
/** Ground direction a heading in degrees points to, clockwise from north (up the classic screen) as HeadingYaw. */
inline std::array<float, 2> HeadingForward(float heading)
{
	float a = heading * 3.14159265359f / 180;
	return {-(std::cos(a) + std::sin(a)) / std::sqrt(2.0f), (std::sin(a) - std::cos(a)) / std::sqrt(2.0f)};
}
/** Perspective eye looking along a heading, pitched up (positive) or down in degrees, with a vertical field of view.
 * Screen right is to the right of the heading and world height is up; nothing nearer than one unit is drawn. */
inline Camera PerspectiveCamera(Vec eye, float heading, float pitch, float fov, float width, float height, float distance)
{
	constexpr float near_plane = 1;
	auto ground = HeadingForward(heading);
	float p = std::clamp(pitch, -89.0f, 89.0f) * 3.14159265359f / 180;
	Vec f{ground[0] * std::cos(p), ground[1] * std::cos(p), std::sin(p)};
	Vec r{ground[1], -ground[0], 0}; // f x up, already unit length.
	Vec u{r.y * f.z - r.z * f.y, r.z * f.x - r.x * f.z, r.x * f.y - r.y * f.x};
	float t = 1 / std::tan(fov * 3.14159265359f / 360), aspect = width / std::max(height, 1.0f);
	auto dot = [&](Vec a) { return a.x * eye.x + a.y * eye.y + a.z * eye.z; };
	std::array<std::array<float, 4>, 3> view{{{r.x, r.y, r.z, -dot(r)}, {u.x, u.y, u.z, -dot(u)}, {-f.x, -f.y, -f.z, dot(f)}}};
	float a = (distance + near_plane) / (near_plane - distance), b = 2 * distance * near_plane / (near_plane - distance);
	Camera camera{eye.x, eye.y, 1, width, height, 1, 0, 1};
	for (int c = 0; c < 4; ++c) {
		camera.matrix[c * 4 + 0] = t / aspect * view[0][c];
		camera.matrix[c * 4 + 1] = t * view[1][c];
		camera.matrix[c * 4 + 2] = a * view[2][c] + (c == 3 ? b : 0);
		camera.matrix[c * 4 + 3] = -view[2][c];
	}
	camera.eye = eye;
	camera.distance = distance;
	return camera;
}
/** What a chase or cab camera looks at: the followed vehicle's position, heading and grade in degrees (up positive),
 * smoothed for presentation only. */
struct ChaseTarget {
	float x, y, z, heading, grade = 0;
	bool operator==(const ChaseTarget &) const = default;
};
/** Chase placement: the eye trails CHASE_BACK behind and CHASE_UP above the target and looks at a point CHASE_AHEAD in
 * front of it, CHASE_LOOK_UP above it; it stays CHASE_CLEARANCE above the ground under it. */
constexpr float CHASE_BACK = 56, CHASE_UP = 24, CHASE_AHEAD = 16, CHASE_LOOK_UP = 4, CHASE_CLEARANCE = 8, CHASE_FOV = 60;
/** Chase smoothing: the position eases over CHASE_POSITION_TIME seconds and trails by at most CHASE_LAG; the heading
 * eases over CHASE_TURN_TIME seconds or CHASE_TURN_DISTANCE travelled, whichever comes first, at most CHASE_TURN_RATE
 * degrees a second. A vehicle moving further than CHASE_TICK_STEP a game tick has jumped (a train reversing puts its
 * lead vehicle at the other end): the camera eases that out over CHASE_JUMP_TIME. A jump beyond CHASE_SNAP starts again. */
constexpr float CHASE_POSITION_TIME = 0.12f, CHASE_LAG = 24, CHASE_TURN_TIME = 0.8f, CHASE_TURN_DISTANCE = 24, CHASE_TURN_RATE = 240;
constexpr float CHASE_TICK_STEP = 8, CHASE_JUMP_TIME = 0.3f, CHASE_SNAP = 256;
/** How a camera follows its vehicle; see FollowChase. */
struct FollowTiming {
	float position_time, lag, turn_time, turn_distance, turn_rate;
	bool cut_jumps; ///< Start again at the vehicle after a jump instead of easing it out.
};
constexpr FollowTiming CHASE_TIMING{CHASE_POSITION_TIME, CHASE_LAG, CHASE_TURN_TIME, CHASE_TURN_DISTANCE, CHASE_TURN_RATE, false};
/** A cab sits on its vehicle: the position only smooths out the steps between game ticks, the heading turns within about
 * a tile through a curve, and a jump (a train reversing hands the driver the other end) cuts to it. */
constexpr FollowTiming CAB_TIMING{0.05f, 4, 0.35f, 12, 240, true};
/** Presentation state of a chase camera between frames. */
struct ChaseFollow {
	ChaseTarget smooth; ///< What the camera looks at.
	ChaseTarget last; ///< The vehicle at the previous update.
	Vec offset{0, 0, 0}; ///< What is left of the vehicle's jumps, still being eased out.
	bool operator==(const ChaseFollow &) const = default;
};
/** Ease a chase camera towards its vehicle, `ticks` game ticks and `seconds` after the previous update.
 * Vehicles move in steps between game ticks and face only 8 directions, so the heading turns the short way gradually. */
inline ChaseFollow FollowChase(ChaseFollow follow, ChaseTarget target, int ticks, float seconds, const FollowTiming &timing = CHASE_TIMING)
{
	Vec step{target.x - follow.last.x, target.y - follow.last.y, target.z - follow.last.z};
	float distance = std::sqrt(step.x * step.x + step.y * step.y + step.z * step.z);
	bool jumped = distance > ticks * CHASE_TICK_STEP + 1;
	if (distance > CHASE_SNAP || (jumped && timing.cut_jumps)) return {target, target};
	float travelled = std::hypot(step.x, step.y);
	if (jumped) {
		follow.offset = {follow.offset.x - step.x, follow.offset.y - step.y, follow.offset.z - step.z};
		travelled = 0;
	}
	float fade = std::exp(-seconds / CHASE_JUMP_TIME);
	auto settle = [](float v) { return std::abs(v) < 0.05f ? 0.0f : v; };
	follow.offset = {settle(follow.offset.x * fade), settle(follow.offset.y * fade), settle(follow.offset.z * fade)};
	float a = 1 - std::exp(-seconds / timing.position_time);
	auto ease = [&](float from, float to) {
		float next = std::clamp(from + (to - from) * a, to - timing.lag, to + timing.lag);
		return std::abs(to - next) < 0.05f ? to : next;
	};
	const ChaseTarget &smooth = follow.smooth;
	ChaseTarget next{ease(smooth.x, target.x + follow.offset.x), ease(smooth.y, target.y + follow.offset.y), ease(smooth.z, target.z + follow.offset.z), target.heading, target.grade};
	float turn = std::remainder(target.heading - smooth.heading, 360.0f);
	float b = 1 - std::exp(-seconds / timing.turn_time - travelled / timing.turn_distance);
	float limit = timing.turn_rate * seconds;
	float by = std::clamp(turn * b, -limit, limit);
	if (std::abs(turn - by) >= 0.05f) next.heading = std::fmod(smooth.heading + by + 360.0f, 360.0f);
	/* The grade eases like the heading, so a cab tips gradually onto and off a slope. */
	float tip = std::clamp((target.grade - smooth.grade) * b, -limit, limit);
	if (std::abs(target.grade - smooth.grade - tip) >= 0.05f) next.grade = smooth.grade + tip;
	return {next, target, follow.offset};
}
/** The chase eye for a target: world x, y and z, heading, pitch and field of view, as a fixed camera stores it.
 * `ground` is the ground height under the eye, which keeps it out of hills behind the vehicle. */
inline std::array<float, 6> ChaseEye(ChaseTarget target, float ground)
{
	auto forward = HeadingForward(target.heading);
	float z = std::max(target.z + CHASE_UP, ground + CHASE_CLEARANCE);
	float pitch = -std::atan2(z - target.z - CHASE_LOOK_UP, CHASE_BACK + CHASE_AHEAD) * 180 / 3.14159265359f;
	return {target.x - forward[0] * CHASE_BACK, target.y - forward[1] * CHASE_BACK, z, target.heading, pitch, CHASE_FOV};
}
/** Cab placement: the eye is `ahead` in front of the target and `up` above it, raised or lowered with the grade, and
 * looks along the heading CAB_PITCH below the grade. */
constexpr float CAB_PITCH = -6, CAB_FOV = 70;
/** The cab eye for a target, in the same form as ChaseEye. */
inline std::array<float, 6> CabEye(ChaseTarget target, float ahead, float up)
{
	auto forward = HeadingForward(target.heading);
	float rise = ahead * std::tan(target.grade * 3.14159265359f / 180);
	return {target.x + forward[0] * ahead, target.y + forward[1] * ahead, target.z + up + rise, target.heading, CAB_PITCH + target.grade, CAB_FOV};
}
/** Tunnel fade: a view goes black over the last TUNNEL_FADE_DISTANCE before its eye reaches the point where the vehicle
 * hides in the tunnel, and fades back in over TUNNEL_FADE_IN_TIME seconds after the vehicle comes out. Without a
 * distance to go (the vehicle hid on a curve), it goes black over TUNNEL_FADE_OUT_TIME. */
constexpr float TUNNEL_FADE_DISTANCE = 8, TUNNEL_FADE_IN_TIME = 0.4f, TUNNEL_FADE_OUT_TIME = 0.2f;
/** How dark a view is `seconds` after it was `dark`, 0 clear to 1 black. `inside` is whether its vehicle is hidden in a
 * tunnel and `to_go` how far its eye still is from where the vehicle hides on the way in (0 once past it), negative when
 * it is not heading in. */
inline float TunnelDark(float dark, bool inside, float to_go, float seconds)
{
	/* A stalled frame fades a quarter of the way at most, so the fade is still seen. */
	float step = std::min(seconds, TUNNEL_FADE_IN_TIME / 4) / (inside ? TUNNEL_FADE_OUT_TIME : TUNNEL_FADE_IN_TIME);
	dark = inside ? std::min(1.0f, dark + step) : std::max(0.0f, dark - step);
	if (to_go >= 0) dark = std::max(dark, std::clamp(1 - to_go / TUNNEL_FADE_DISTANCE, 0.0f, 1.0f));
	return dark;
}
/** Classic scroll delta that moves a rotated view by the screen delta (dx, dy) at constant height. */
inline std::array<float, 2> ClassicScrollDelta(float dx, float dy, float cosine, float sine, float tilt)
{
	float rx = (dy / tilt - dx / 2) / 2, ry = (dy / tilt + dx / 2) / 2;
	float x = rx * cosine + ry * sine, y = -rx * sine + ry * cosine;
	return {2 * (y - x), x + y};
}
/** Conservative frustum test; GPU clipping handles partially visible chunks. A perspective camera's far plane is its draw distance. */
inline bool Visible(Vec low, Vec high, const Camera &camera)
{
	if (camera.Perspective()) {
		/* Each plane is row 3 plus or minus another row; a box is outside when its corner furthest inside is still outside one. */
		auto w = camera.Row(3);
		for (int i = 0; i < 6; ++i) {
			auto r = camera.Row(i / 2);
			float sign = i % 2 == 0 ? 1.0f : -1.0f;
			std::array<float, 4> plane{w[0] + sign * r[0], w[1] + sign * r[1], w[2] + sign * r[2], w[3] + sign * r[3]};
			float d = plane[0] * (plane[0] > 0 ? high.x : low.x) + plane[1] * (plane[1] > 0 ? high.y : low.y) + plane[2] * (plane[2] > 0 ? high.z : low.z) + plane[3];
			if (d < 0) return false;
		}
		return true;
	}
	float min_x=1e30f,min_y=1e30f,max_x=-1e30f,max_y=-1e30f;
	for (int i=0;i<8;++i) {
		auto p=camera.Project({(i&1)?high.x:low.x,(i&2)?high.y:low.y,(i&4)?high.z:low.z});
		min_x=std::min(min_x,p.x); max_x=std::max(max_x,p.x);
		min_y=std::min(min_y,p.y); max_y=std::max(max_y,p.y);
	}
	return max_x>=0 && max_y>=0 && min_x<=camera.width && min_y<=camera.height;
}

/** Barycentric screen hit; depth uses the same affine interpolation as OpenGL. */
inline bool Hit(const std::array<Vec, 3> &p, float x, float y, std::array<float, 3> &weights)
{
	float d = (p[1].y - p[2].y) * (p[0].x - p[2].x) + (p[2].x - p[1].x) * (p[0].y - p[2].y);
	if (std::abs(d) < 0.00001f) return false;
	weights[0] = ((p[1].y - p[2].y) * (x - p[2].x) + (p[2].x - p[1].x) * (y - p[2].y)) / d;
	weights[1] = ((p[2].y - p[0].y) * (x - p[2].x) + (p[0].x - p[2].x) * (y - p[2].y)) / d;
	weights[2] = 1 - weights[0] - weights[1];
	return weights[0] >= -0.00001f && weights[1] >= -0.00001f && weights[2] >= -0.00001f;
}
}
#endif

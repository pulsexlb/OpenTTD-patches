/* Stub of the OpenTTD_3D Phase 8 model library: no authored models yet, every
 * lookup misses so the renderer falls back to the generated geometry. The
 * interface mirrors the real generated header (tools/3d_assets/convert_gltf.py).
 */
#ifndef ASSET3D_ART_H
#define ASSET3D_ART_H
#include "asset3d_generated.h"
namespace Art3D {
using Asset3D::Face;
using Asset3D::Model;

/** Identity + state -> model index; empty here so Find always reports -1. */
struct Entry { uint16_t id; uint8_t state; uint16_t model; };

inline constexpr Entry houses[] = {{0, 0, 0xFFFF}};
inline constexpr Entry industry_tiles[] = {{0, 0, 0xFFFF}};
inline constexpr Entry industry_parts[] = {{0, 0, 0xFFFF}};
inline constexpr Entry effects[] = {{0, 0, 0xFFFF}};
inline constexpr Entry engines[] = {{0, 0, 0xFFFF}};
inline constexpr Entry trees[] = {{0, 0, 0xFFFF}};
inline constexpr Entry furniture[] = {{0, 0, 0xFFFF}};
inline constexpr Entry objects[] = {{0, 0, 0xFFFF}};
inline constexpr Entry landscape[] = {{0, 0, 0xFFFF}};
inline constexpr Entry stations[] = {{0, 0, 0xFFFF}};
inline constexpr Entry airport_tiles[] = {{0, 0, 0xFFFF}};
inline constexpr Entry bridges[] = {{0, 0, 0xFFFF}};
inline constexpr Entry tunnels[] = {{0, 0, 0xFFFF}};

template <size_t N> constexpr int Find(const Entry (&table)[N], unsigned id, unsigned state)
{
	static_assert(N == 1, "Stub tables must stay empty");
	return -1;
}

template <size_t N> constexpr bool Has(const Entry (&table)[N], unsigned id)
{
	static_assert(N == 1, "Stub tables must stay empty");
	return false;
}

constexpr int House(unsigned house, unsigned stage) { return Find(houses, house, stage); }
constexpr bool HasHouse(unsigned house) { return Has(houses, house); }
constexpr int IndustryTile(unsigned gfx, unsigned stage) { return Find(industry_tiles, gfx, stage); }
constexpr int IndustryPart(unsigned gfx, unsigned step) { return Find(industry_parts, gfx, step); }
constexpr unsigned IndustryPartSteps(unsigned gfx) { return 0; }
constexpr int Effect(unsigned id, unsigned frame) { return Find(effects, id, frame); }
constexpr int Engine(unsigned engine, bool loaded) { return -1; }
constexpr int Tree(unsigned type, unsigned growth) { return Find(trees, type, growth); }
constexpr int Signal(unsigned variant, unsigned type, bool green) { return Find(furniture, variant * 8 + type, green ? 1 : 0); }
constexpr int Crossing(bool closed) { return Find(furniture, 16, closed ? 1 : 0); }
constexpr int Catenary(unsigned piece) { return Find(furniture, 17, piece); }
constexpr int Object(unsigned id) { return Find(objects, id, 0); }
constexpr int Fence(unsigned type) { return Find(landscape, type, 0); }
constexpr int Rocks() { return Find(landscape, 16, 0); }
constexpr int Station(unsigned id, unsigned part = 0) { return Find(stations, id, part); }
constexpr int AirportTile(unsigned gfx, unsigned frame) { return -1; }
constexpr bool HasAirportTiles() { return false; }
constexpr int BridgePiece(unsigned type, unsigned piece) { return Find(bridges, type, piece); }
constexpr int BridgePillar(unsigned type, unsigned piece) { return Find(bridges, type, 8 + piece); }
constexpr int BridgeRamp(unsigned type, bool sloped) { return Find(bridges, type, sloped ? 6 : 7); }
constexpr int Tunnel(unsigned id) { return Find(tunnels, id, 0); }

/** No authored models: indices 0..N-1 come from here and are all missing. */
inline constexpr Model models[] = {{Asset3D::missing, std::size(Asset3D::missing)}};
}
#endif

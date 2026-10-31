/* This file is part of OpenTTD and is licensed under the GNU General Public License version 2. */
/** @file terrain3d_impl.h CPU terrain snapshots, chunk meshes and picking, owned by the GL viewport pass. */
#include "terrain3d_mesh.h"
#include "terrain3d_dirty.h"
#include "terrain3d_cache.h"
#include "ground3d.h"
#include "ground3d_textures.h"
namespace {
using Terrain3D::Vec;
struct TerrainFace {
	std::array<Vec, 3> p;
	std::array<float, 3> colour;
	TileIndex tile;
	TransparencyOption transparency = TransparencyOption::Invalid;
	bool ground = false;
	int layer = -1; ///< Ground texture layer, plus Ground3D::NO_BLEND when it never blends; -1 is a flat colour.
	bool operator==(const TerrainFace &) const = default;
};
struct TerrainTile {
	Slope ground, surface;
	int ground_z, surface_z;
	TileType type;
	TrackBits tracks = TRACK_BIT_NONE;
	int depot = -1;
	bool station = false;
	uint8_t ground_material = to_underlying(ClearGround::Grass);
	uint8_t ground_layer = Ground3D::GRASS_3; ///< Ground3D::Layer, plus Ground3D::NO_BLEND for open water.
	int facility = -1, water_depot = -1, lock = -1;
	bool hangar = false, water_surface = false;
	uint16_t signals = 0, green_signals = 0;
	uint32_t signal_kinds = 0; ///< Per track, 4 bits: SignalType | SignalVariant << 3.
	RoadBits roads = ROAD_NONE;
	int road_depot = -1, portal = -1, ramp = -1;
	bool road_stop = false, crossing = false, barred = false;
	int deck_z = -1, deck_axis = 0;
	bool deck_road = false, deck_rail = false;
	int deck_type = -1, deck_piece = 0; ///< Original BridgeType and BridgePieces of the bridge above, or -1 (aqueduct, NewGRF sprites).
	int ramp_type = -1; ///< Original BridgeType of the bridge head on this tile, or -1.
	int portal_kind = -1; ///< Tunnel portal model: 0 rail, 1 road.
	int asset = -1, asset_stage = 3, tree_count = 0;
	int art = -1; ///< Phase 8 model index in Art3D::models; takes precedence over \c asset.
	uint16_t fences = 0; ///< Field fence type per side, 3 bits each: NW, NE, SE, SW.
	Owner owner = OWNER_NONE; ///< Company colours for map objects, stations and depots.
	DiagDirection art_turn = DiagDirection::SE; ///< Where a station or depot model's local +Y points.
	bool art_on_ground = false; ///< The station model follows the sloped tile surface (dock land part).
	bool art_bare = false; ///< A modelled set with nothing to draw on this tile (plain airport apron or grass): no placeholder.
	std::vector<RailCatenaryPiece> catenary; ///< Pylons and wires where the classic view draws them (CollectRailCatenary).
	bool operator==(const TerrainTile &) const = default;
};
/** Affine library placement. Ground-following models keep their per-vertex deformation. */
struct Static3DInstance {
	const Asset3D::Model *model;
	size_t model_index;
	Vec origin;
	float dx, dy, length, height;
	std::array<std::array<float, 3>, 2> company;
	TileIndex tile;
	TransparencyOption transparency = TransparencyOption::Invalid;
	std::array<Vec, 2> p; ///< Exact transformed bounds, also used by the packed-span updater.
	bool operator==(const Static3DInstance &) const = default;
};
static bool _terrain_static_instances = true;
static std::vector<Static3DInstance> *_terrain_capture_instances = nullptr;
static TransparencyOption _terrain_capture_option = TransparencyOption::Invalid;
struct TerrainChunk {
	Terrain3D::DirtyTileSet dirty;
	Terrain3D::MeshChanges mesh_changes;
	Terrain3D::MeshChanges gpu_changes; ///< Tiles rebuilt since the retained buffer was last written.
	uint64_t used = 0;
	bool resident = false;
	uint64_t revision = 0;
	uint64_t instance_revision = 0; ///< Changes only when a shared placement changes, independently of terrain faces.
	std::vector<TerrainTile> tiles;
	std::vector<TerrainFace> faces;
	std::vector<Static3DInstance> instances;
	std::vector<Terrain3D::TileMeshSpan> instance_spans;
	std::vector<Terrain3D::TileMeshSpan> spans;
	Vec low{1e30f,1e30f,1e30f}, high{-1e30f,-1e30f,-1e30f};
};
static std::vector<TerrainChunk> _terrain_chunks;
static int _terrain_chunk_size = 16, _terrain_map_x = 0, _terrain_map_y = 0;
static int _terrain_revision = 0;
/* Presentation-only orbit camera: current, animation target, and the angle Home returns to. */
static float _terrain_yaw = 0, _terrain_tilt = 100, _terrain_pivot_z = 0;
static float _terrain_target_yaw = 0, _terrain_target_tilt = 100;
static float _terrain_orbit_yaw = 45, _terrain_orbit_tilt = 100;
/* Trailer zoom between zoom levels: extra magnification of the shared camera about the view centre. */
static float _terrain_magnify = 1;
static int _terrain_highlight = -1;
static bool _terrain_wireframe = false;
static int _terrain_last_rebuilt = 0;
static bool _terrain_reset = true, _terrain_verify = false;
/* Ground layer per tile for neighbour blending in the shader; the dirty rectangle is pending GPU upload. */
static std::vector<uint8_t> _ground_map;
static int _ground_dirty_x0 = 0, _ground_dirty_y0 = 0, _ground_dirty_x1 = -1, _ground_dirty_y1 = -1;
static uint64_t _terrain_generation = 0;
static uint64_t _terrain_mesh_serial = 0;

/** Whether the shared management camera matches the classic projection. */
static bool ClassicTerrainCamera()
{
	return _terrain_yaw == 0 && _terrain_tilt == 100;
}

/** Orientation of one view: yaw in degrees, tilt in percent, and the pivot height on the centre ray.
 * A field of view makes it a perspective view from the eye along the heading instead. */
struct TerrainAngle {
	float yaw, tilt, pz;
	Vec eye{0, 0, 0};
	float heading = 0, pitch = 0, fov = 0;
	Vec hidden{0, 0, 0}; ///< A cab view leaves out its own vehicle, the one drawn exactly here.
	bool hides = false;
	float dark = 0; ///< How far a perspective view has faded to black.
	float magnify = 1; ///< Extra orthographic magnification, for trailer zooms.
};

/** How far a perspective view draws, fading into fog before it. */
static constexpr float TERRAIN_DRAW_DISTANCE = 64 * TILE_SIZE;

/** The shared orbit camera; the pivot sits at the ground height under the view, so orbiting a hill keeps it centred. */
static TerrainAngle SharedTerrainAngle()
{
	TerrainAngle view{_terrain_yaw, _terrain_tilt, ClassicTerrainCamera() ? 0 : _terrain_pivot_z};
	view.magnify = _terrain_magnify;
	return view;
}

/** Whether a view has its own Phase 9 camera; such views are never picked. */
static bool OwnTerrainCamera(const Viewport &vp)
{
	return vp.camera_3d != ViewportCamera3D::Normal && Vehicle::IsValidID(vp.camera_vehicle);
}

/** Grade of a vehicle on a sloped tile: one tile height over one tile. */
static const float VEHICLE_SLOPE_GRADE = std::atan2(static_cast<float>(TILE_HEIGHT), static_cast<float>(TILE_SIZE)) * 180 / 3.14159265359f;

/** Where the vehicle a chase or cab camera follows is now, unsmoothed. Only road and rail vehicles know they are on a slope. */
static Terrain3D::ChaseTarget ChaseVehicleTarget(const Vehicle &v)
{
	float grade = 0;
	if (v.IsGroundVehicle()) {
		auto flags = v.GetGroundVehicleFlags();
		grade = flags.Test(GroundVehicleFlag::GoingUp) ? VEHICLE_SLOPE_GRADE : flags.Test(GroundVehicleFlag::GoingDown) ? -VEHICLE_SLOPE_GRADE : 0;
	}
	return {static_cast<float>(v.x_pos), static_cast<float>(v.y_pos), static_cast<float>(v.z_pos), Terrain3D::HeadingYaw(to_underlying(v.direction)), grade};
}

static std::array<float, 2> CabMount3D(const Vehicle &v);

/** The cab eye for a smoothed target, on the front of the vehicle's model. */
static std::array<float, 6> CabTerrainEye(const Vehicle &v, Terrain3D::ChaseTarget target)
{
	auto [ahead, up] = CabMount3D(v);
	return Terrain3D::CabEye(target, ahead, up);
}

/** The chase eye for a smoothed target, kept above the ground under it. */
static std::array<float, 6> ChaseTerrainEye(Terrain3D::ChaseTarget target)
{
	auto eye = Terrain3D::ChaseEye(target, 0);
	return Terrain3D::ChaseEye(target, static_cast<float>(GetSlopePixelZOutsideMap(static_cast<int>(eye[0]), static_cast<int>(eye[1]))));
}

/** Whether a vehicle is hidden inside a tunnel, from its tunnel and hidden state. */
static bool VehicleInTunnel3D(const Vehicle &v)
{
	return v.vehstatus.Test(VehState::Hidden) && IsTileType(v.tile, TileType::TunnelBridge) && IsTunnel(v.tile);
}

/** How far an eye `ahead` of a road or rail vehicle still is from where the vehicle will hide in the tunnel it is heading
 * straight into, from the portal tile or the tile before it: 0 once past it, negative when not heading into a tunnel. */
static float TunnelToGo3D(const Vehicle &v, float ahead)
{
	if (!v.IsGroundVehicle() || v.vehstatus.Test(VehState::Hidden) || !IsDiagonalDirection(v.direction)) return -1;
	DiagDirection dir = DirToDiagDir(v.direction);
	/* Units moved onto the tile, as VehicleEnter_TunnelBridge counts them. */
	int pos = (DiagDirToAxis(dir) == Axis::X ? v.x_pos : v.y_pos) & TILE_UNIT_MASK;
	int frame = (dir == DiagDirection::NE || dir == DiagDirection::NW) ? TILE_SIZE - 1 - pos : pos;
	auto heading_in = [&](TileIndex t) { return IsValidTile(t) && IsTileType(t, TileType::TunnelBridge) && IsTunnel(t) && GetTunnelBridgeDirection(t) == dir; };
	TileIndex tile = TileVirtXY(v.x_pos, v.y_pos);
	int to_hide = _tunnel_visibility_frame[dir] - frame;
	if (!heading_in(tile)) {
		if (!heading_in(TileAddByDiagDir(tile, dir))) return -1;
		to_hide += TILE_SIZE;
	}
	return std::max(0.0f, to_hide - ahead);
}

/** Whether a cab eye `ahead` of a road or rail vehicle that has just come out of a tunnel is still inside the portal,
 * on the tile the vehicle came out onto; the view stays black until it is out. */
static bool TunnelPortalAhead3D(const Vehicle &v, float ahead)
{
	if (!v.IsGroundVehicle() || v.vehstatus.Test(VehState::Hidden) || !IsDiagonalDirection(v.direction)) return false;
	TileIndex tile = TileVirtXY(v.x_pos, v.y_pos);
	DiagDirection dir = DirToDiagDir(v.direction);
	if (!IsTileType(tile, TileType::TunnelBridge) || !IsTunnel(tile) || GetTunnelBridgeDirection(tile) != ReverseDiagDir(dir)) return false;
	int pos = (DiagDirToAxis(dir) == Axis::X ? v.x_pos : v.y_pos) & TILE_UNIT_MASK;
	int frame = (dir == DiagDirection::NE || dir == DiagDirection::NW) ? TILE_SIZE - 1 - pos : pos;
	return frame + ahead < TILE_SIZE;
}

/** Whether a chase or cab view stops riding with its vehicle and snaps back to normal: it crashed or is in a depot or
 * hangar, where there is nothing to see. */
static bool RideEnded3D(const Vehicle &v)
{
	return v.vehstatus.Test(VehState::Crashed) || (v.IsPrimaryVehicle() && v.IsInDepot());
}

/** The view's own camera, or the shared one. Fixed, chase and cab views are perspective cameras from their stored eye; a
 * chase or cab view not yet updated looks from where its vehicle is now. A cab view leaves out its own vehicle.
 * A view faded to black keeps following, so the ground around it stays ready for when it comes out. */
static TerrainAngle ViewTerrainAngle(const Viewport &vp)
{
	if (!OwnTerrainCamera(vp)) return SharedTerrainAngle();
	const Vehicle *v = Vehicle::Get(vp.camera_vehicle);
	bool cab = vp.camera_3d == ViewportCamera3D::Cab;
	auto e = vp.camera_eye;
	if (!vp.camera_follow_set && vp.camera_3d != ViewportCamera3D::Fixed) e = cab ? CabTerrainEye(*v, ChaseVehicleTarget(*v)) : ChaseTerrainEye(ChaseVehicleTarget(*v));
	TerrainAngle view{e[3], 0, 0, {e[0], e[1], e[2]}, e[3], e[4], e[5]};
	if (cab) {
		view.hidden = {static_cast<float>(v->x_pos), static_cast<float>(v->y_pos), static_cast<float>(v->z_pos)};
		view.hides = true;
	}
	if (vp.camera_3d != ViewportCamera3D::Fixed) view.dark = vp.camera_follow_set ? vp.camera_dark : VehicleInTunnel3D(*v) ? 1.0f : 0.0f;
	return view;
}

static bool _chase_log = false; ///< Log every chase and cab camera update, for the Phase 9.3 and 9.4 checks.

static Terrain3D::Camera TerrainCamera(int width, int height, int left, int top, int zoom, TerrainAngle view)
{
	if (view.fov > 0) {
		auto camera = Terrain3D::PerspectiveCamera(view.eye, view.heading, view.pitch, view.fov, static_cast<float>(width), static_cast<float>(height), TERRAIN_DRAW_DISTANCE);
		camera.hidden = view.hidden;
		camera.hides = view.hides;
		camera.dark = view.dark;
		return camera;
	}
	float scale = static_cast<float>(ZOOM_BASE) / (1U << zoom) * view.magnify;
	float sx = (left + width * (1U << zoom) * 0.5f) / ZOOM_BASE;
	float sy = (top + height * (1U << zoom) * 0.5f) / ZOOM_BASE;
	float angle = view.yaw * 3.14159265359f / 180;
	return {(sy - sx / 2 + view.pz) / 2, (sy + sx / 2 + view.pz) / 2, scale, static_cast<float>(width), static_cast<float>(height), std::cos(angle), std::sin(angle), view.tilt / 100.0f, view.pz};
}

/** The camera a window viewport is drawn with now. */
static Terrain3D::Camera ViewportTerrainCamera(const Viewport &vp)
{
	return TerrainCamera(vp.width, vp.height, vp.virtual_left & ScaleByZoom(-1, vp.zoom), vp.virtual_top & ScaleByZoom(-1, vp.zoom), static_cast<int>(vp.zoom), ViewTerrainAngle(vp));
}

static void AddTerrainFace(std::vector<TerrainFace> &faces, std::array<Vec, 3> p, std::array<float, 3> colour, TileIndex tile)
{
	Vec a{p[1].x-p[0].x,p[1].y-p[0].y,p[1].z-p[0].z};
	Vec b{p[2].x-p[0].x,p[2].y-p[0].y,p[2].z-p[0].z};
	Vec n{a.y*b.z-a.z*b.y,a.z*b.x-a.x*b.z,a.x*b.y-a.y*b.x};
	float length = std::sqrt(n.x*n.x+n.y*n.y+n.z*n.z);
	if (length < 0.0001f) return;
	if (n.z < 0) { n.x=-n.x; n.y=-n.y; n.z=-n.z; }
	float light = 0.62f + 0.38f * std::max(0.0f, (-0.4f*n.x-0.5f*n.y+0.768f*n.z)/length);
	/* Glowing model colours (flames, sparks) come offset by 2 and are drawn unlit. */
	if (colour[0] > 1.5f) {
		for (auto &c : colour) c -= 2;
	} else {
		for (auto &c : colour) c *= light;
	}
	faces.push_back({p, colour, tile});
}

/** A textured ground face: the colour is the layer's mean colour, so the flat fallback keeps the same tone. */
static void AddGroundFace(std::vector<TerrainFace> &faces, std::array<Vec, 3> p, int layer, TileIndex tile)
{
	const auto &mean = Ground3D::average[layer % Ground3D::NO_BLEND];
	size_t count = faces.size();
	AddTerrainFace(faces, p, {mean[0], mean[1], mean[2]}, tile);
	if (faces.size() != count) faces.back().layer = layer;
}

/** The authoritative ground under a tile, as the classic DrawTile_* functions choose its sprite. */
static uint8_t ReadGround3D(TileIndex tile)
{
	using Ground3D::Kind;
	bool arctic = _settings_game.game_creation.landscape == LandscapeType::Arctic;
	Kind snow_or_desert = arctic ? Kind::Snow : Kind::Desert;
	switch (GetTileType(tile)) {
		case TileType::Clear: {
			uint density = GetClearDensity(tile);
			if (IsSnowTile(tile)) return Ground3D::LayerOf(Kind::Snow, density);
			switch (GetClearGround(tile)) {
				case ClearGround::Grass: return Ground3D::LayerOf(Kind::Grass, density);
				case ClearGround::Rough: return Ground3D::LayerOf(Kind::Rough);
				case ClearGround::Rocks: return Ground3D::LayerOf(Kind::Rocks);
				case ClearGround::Fields: return Ground3D::LayerOf(Kind::Fields, 3, GetFieldType(tile));
				case ClearGround::Desert: return Ground3D::LayerOf(Kind::Desert, density);
				default: return Ground3D::GRASS_3;
			}
		}
		case TileType::Trees: {
			uint density = GetTreeDensity(tile);
			switch (GetTreeGround(tile)) {
				case TreeGround::Grass: return Ground3D::LayerOf(Kind::Grass, density);
				case TreeGround::Rough: return Ground3D::LayerOf(Kind::Rough);
				case TreeGround::SnowOrDesert: return Ground3D::LayerOf(snow_or_desert, density);
				case TreeGround::RoughSnow: return Ground3D::LayerOf(Kind::Snow, density);
				case TreeGround::Shore: return Ground3D::BEACH;
				default: return Ground3D::GRASS_3;
			}
		}
		case TileType::Railway:
			switch (GetRailGroundType(tile)) {
				case RailGroundType::Barren: return Ground3D::GRASS_0;
				case RailGroundType::SnowOrDesert: return Ground3D::LayerOf(snow_or_desert);
				default: return Ground3D::GRASS_3;
			}
		case TileType::Road:
			if (!IsRoadDepot(tile)) {
				if (IsOnSnowOrDesert(tile)) return Ground3D::LayerOf(snow_or_desert);
				switch (GetRoadside(tile)) {
					case Roadside::Barren: return Ground3D::GRASS_0;
					case Roadside::Grass: case Roadside::GrassRoadWorks: return Ground3D::GRASS_3;
					default: return Ground3D::CONCRETE;
				}
			}
			return Ground3D::CONCRETE;
		case TileType::Station:
			if (IsBuoy(tile) || IsOilRig(tile) || (IsDock(tile) && IsDockWaterPart(tile))) return Ground3D::WATER | Ground3D::NO_BLEND;
			if (IsAirport(tile)) {
				/* Base airport tiles on grass (airports_temperate.py GRASS); taxiway pieces show the rest as an overlay. */
				static constexpr std::array<uint8_t, 22> grass{4, 5, 10, 11, 13, 20, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40, 41, 42, 44, 73};
				StationGfx gfx = GetAirportGfx(tile);
				return std::ranges::find(grass, gfx) != grass.end() ? Ground3D::GRASS_3 : Ground3D::CONCRETE;
			}
			if (IsAnyRoadStop(tile)) return Ground3D::CONCRETE;
			return Ground3D::GRASS_3;
		case TileType::Industry: return Ground3D::CONCRETE;
		case TileType::Object: return IsObjectType(tile, OBJECT_OWNED_LAND) ? Ground3D::GRASS_0 : Ground3D::GRASS_3; // owned land shows bare earth
		case TileType::Water: return IsCoast(tile) ? Ground3D::BEACH : Ground3D::WATER | Ground3D::NO_BLEND;
		default: return Ground3D::GRASS_3;
	}
}

/** Four triangular sectors preserve both possible diagonal ridges, including saddle slopes. */
static std::array<Vec, 5> TerrainPoints(Slope slope, int z, int x, int y)
{
	Slope basic = RemoveHalftileSlope(slope);
	return {{{float(x),float(y),float(z+GetSlopeZInCorner(basic,CORNER_N)*TILE_HEIGHT)},
		{float(x+TILE_SIZE),float(y),float(z+GetSlopeZInCorner(basic,CORNER_W)*TILE_HEIGHT)},
		{float(x+TILE_SIZE),float(y+TILE_SIZE),float(z+GetSlopeZInCorner(basic,CORNER_S)*TILE_HEIGHT)},
		{float(x),float(y+TILE_SIZE),float(z+GetSlopeZInCorner(basic,CORNER_E)*TILE_HEIGHT)},
		{float(x+8),float(y+8),float(z+GetPartialPixelZ(8,8,basic))}}};
}

/** Mark only structure faces; rails and ground remain readable through them. */
static void TagStructure3D(std::vector<TerrainFace> &faces, size_t first, TransparencyOption option)
{
	for (size_t i = first; i < faces.size(); ++i) faces[i].transparency = option;
}

static void BuildTerrainTile(std::vector<TerrainFace> &faces, const TerrainTile &t, int tx, int ty)
{
	if (t.type == TileType::Void) return;
	int x=tx*TILE_SIZE, y=ty*TILE_SIZE;
	TileIndex tile=TileXY(tx,ty);
	auto p=TerrainPoints(t.surface,t.surface_z,x,y);
	auto ground=TerrainPoints(t.ground,t.ground_z,x,y);
	for (int i=0;i<4;++i) {
		int j=(i+1)%4;
		std::array<Vec,3> tri{p[i],p[j],p[4]};
		bool upper=false;
		if (IsHalftileSlope(t.surface)) {
			float mx=(tri[0].x+tri[1].x+tri[2].x)/3-x, my=(tri[0].y+tri[1].y+tri[2].y)/3-y;
			switch (GetHalftileSlopeCorner(t.surface)) {
				case CORNER_N: upper=mx+my<16; break;
				case CORNER_S: upper=mx+my>16; break;
				case CORNER_W: upper=mx>my; break;
				case CORNER_E: upper=mx<my; break;
				default: break;
			}
			if (upper) for (auto &v:tri) v.z=float(t.surface_z+GetSlopeMaxPixelZ(t.surface));
		}
		int layer=t.ground_layer;
		if (t.type==TileType::Water || t.water_surface) {
			/* Coasts: the sea-level sectors are water, the raised ones beach. A lock's sloped middle is all water. */
			bool wet=tri[0].z+tri[1].z+tri[2].z<=0.01f || t.ground==SLOPE_FLAT || t.lock>=0;
			layer=wet ? Ground3D::WATER | Ground3D::NO_BLEND : Ground3D::BEACH;
		}
		AddGroundFace(faces,tri,layer,tile);
		/* Foundation skirts, also the internal vertical face of a half-tile platform. */
		for (int edge=0;edge<(upper?3:1);++edge) {
			int next=(edge+1)%3;
			Vec a=tri[edge],b=tri[next];
			Vec low_a=(edge==0?ground[i]:edge==1?ground[j]:ground[4]);
			Vec low_b=(next==0?ground[i]:next==1?ground[j]:ground[4]);
			if (edge!=0) { low_a=(edge==1?p[j]:p[4]); low_b=(next==0?p[i]:p[4]); }
			if (tx==0 || ty==0 || tx==int(Map::MaxX())-1 || ty==int(Map::MaxY())-1) {
				if (edge==0 && ((tx==0&&i==3)||(ty==0&&i==0)||(tx==int(Map::MaxX())-1&&i==1)||(ty==int(Map::MaxY())-1&&i==2))) low_a.z=low_b.z=-8;
			}
			AddTerrainFace(faces,{a,low_a,low_b},{0.45f,0.41f,0.32f},tile);
			AddTerrainFace(faces,{a,low_b,b},{0.45f,0.41f,0.32f},tile);
		}
	}
}

#include "rail3d_impl.h"

static void BuildTileGeometry(std::vector<TerrainFace> &faces, const TerrainTile &snapshot, int x, int y, std::vector<Static3DInstance> *instances = nullptr)
{
	size_t first = faces.size();
	BuildTerrainTile(faces, snapshot, x, y);
	for (size_t i = first; i < faces.size(); ++i) faces[i].ground = true;
	BuildRail3D(faces, snapshot, TileXY(x, y));
	_terrain_capture_instances = instances;
	_terrain_capture_option = snapshot.type == TileType::Clear ? TransparencyOption::Invalid : snapshot.type == TileType::Trees ? TransparencyOption::Trees :
		snapshot.type == TileType::House ? TransparencyOption::Houses : snapshot.type == TileType::Object ? TransparencyOption::Structures :
		snapshot.type == TileType::Industry ? TransparencyOption::Industries : TransparencyOption::Buildings;
	BuildAsset3D(faces, snapshot, TileXY(x, y));
	_terrain_capture_instances = nullptr;
}

/** Box holding everything a tile can draw: its terrain, bridge decks and any compiled asset. */
static std::pair<Vec, Vec> TerrainTileBounds(const TerrainTile &tile, int x, int y)
{
	static const Vec padding = [] {
		Vec p{16,16,32}; // Infrastructure, ramps, portals, signals and foundation skirts.
		for (std::span<const Asset3D::Model> models : {std::span<const Asset3D::Model>(Asset3D::models), std::span<const Asset3D::Model>(Art3D::models)}) {
			for (const auto &model : models) for (size_t i = 0; i < model.count; ++i) for (const auto &v : model.faces[i].p) {
				p.x = std::max(p.x, std::abs(v[0])); p.y = std::max(p.y, std::abs(v[1])); p.z = std::max(p.z, std::abs(v[2]));
			}
		}
		return p;
	}();
	float bottom = float(std::min({-8, tile.ground_z, tile.surface_z}));
	float top = float(std::max({tile.ground_z + int(GetSlopeMaxPixelZ(tile.ground)), tile.surface_z + int(GetSlopeMaxPixelZ(tile.surface)), tile.deck_z + int(TILE_HEIGHT)}));
	return {{x * TILE_SIZE - padding.x, y * TILE_SIZE - padding.y, bottom - padding.z}, {(x + 1) * TILE_SIZE + padding.x, (y + 1) * TILE_SIZE + padding.y, top + padding.z}};
}

/** Expand current bounds before visibility testing, including newly raised terrain,
 * bridge decks and every compiled asset. Old geometry remains covered until rebuilt. */
static void ExpandTerrainBounds(TerrainChunk &chunk, const TerrainTile &tile, int x, int y)
{
	auto [low, high] = TerrainTileBounds(tile, x, y);
	chunk.low = {std::min(chunk.low.x, low.x), std::min(chunk.low.y, low.y), std::min(chunk.low.z, low.z)};
	chunk.high = {std::max(chunk.high.x, high.x), std::max(chunk.high.y, high.y), std::max(chunk.high.z, high.z)};
}

/** Build only the geometry required by this view. Each caller supplies its own
 * camera; secondary/vehicle views and screenshot views cannot share a visibility flag. */
static void PrepareTerrainMeshes(const Terrain3D::Camera *camera = nullptr)
{
	auto start = std::chrono::steady_clock::now();
	int nx = (_terrain_map_x + _terrain_chunk_size - 1) / _terrain_chunk_size;
	int mesh_checks = 0, mesh_mismatches = 0;
	std::vector<TerrainFace> scratch;
	std::vector<Static3DInstance> instance_scratch;
	for (size_t c = 0; c < _terrain_chunks.size(); ++c) {
		auto &chunk = _terrain_chunks[c];
		if (camera != nullptr && !Terrain3D::Visible(chunk.low, chunk.high, *camera)) continue;
		chunk.used = _proof_frame_serial;
		if (chunk.resident && chunk.mesh_changes.Empty()) continue;
		int cx = int(c % nx), cy = int(c / nx);
		int width = std::min(_terrain_chunk_size, _terrain_map_x - cx * _terrain_chunk_size);
		auto changed_tiles = chunk.mesh_changes.Take(chunk.tiles.size(), !chunk.resident || !_proof_tile_meshes);
		_proof_cold_chunks += !chunk.resident;
		chunk.revision = ++_terrain_mesh_serial;
		chunk.resident = true;
		++_proof_chunks_rebuilt;
		std::vector<std::vector<Static3DInstance>> built_instances(chunk.tiles.size());
		auto build = [&](std::vector<TerrainFace> &faces, size_t index) {
			BuildTileGeometry(faces, chunk.tiles[index], cx * _terrain_chunk_size + static_cast<int>(index % width), cy * _terrain_chunk_size + static_cast<int>(index / width), _terrain_static_instances && _proof_retained ? &built_instances[index] : nullptr);
		};
		Terrain3D::PatchTileMesh(chunk.faces, chunk.spans, changed_tiles, scratch, build);
		bool instances_changed = chunk.instance_spans.size() != changed_tiles.size();
		if (!instances_changed) for (size_t i = 0; i < changed_tiles.size(); ++i) {
			if (!changed_tiles[i]) continue;
			size_t first = i == 0 ? 0 : chunk.instance_spans[i - 1].end;
			if (!std::equal(built_instances[i].begin(), built_instances[i].end(), chunk.instances.begin() + first,
					chunk.instances.begin() + chunk.instance_spans[i].end)) { instances_changed = true; break; }
		}
		Terrain3D::PatchTileMesh(chunk.instances, chunk.instance_spans, changed_tiles, instance_scratch,
			[&](auto &out, size_t index) { out.insert(out.end(), built_instances[index].begin(), built_instances[index].end()); });
		if (instances_changed) chunk.instance_revision = chunk.revision;
		chunk.gpu_changes.Merge(changed_tiles);
		_proof_tiles_rebuilt += std::count(changed_tiles.begin(), changed_tiles.end(), true);
		_proof_tiles_reused += std::count(changed_tiles.begin(), changed_tiles.end(), false);
		chunk.low={1e30f,1e30f,1e30f}; chunk.high={-1e30f,-1e30f,-1e30f};
		for (const auto *spans : {&chunk.spans, &chunk.instance_spans}) for (const auto &span : *spans) {
			chunk.low={std::min(chunk.low.x,span.low.x),std::min(chunk.low.y,span.low.y),std::min(chunk.low.z,span.low.z)};
			chunk.high={std::max(chunk.high.x,span.high.x),std::max(chunk.high.y,span.high.y),std::max(chunk.high.z,span.high.z)};
		}
		if (_terrain_verify) {
			/* Independently regenerate the entire chunk; CPU/GPU image checks alone
			 * would both consume the same possibly incorrect cached geometry. */
			std::vector<TerrainFace> reference;
			std::vector<TerrainFace> expanded;
			for (size_t i = 0; i < chunk.tiles.size(); ++i) {
				BuildTileGeometry(reference, chunk.tiles[i], cx * _terrain_chunk_size + int(i % width), cy * _terrain_chunk_size + int(i / width));
				size_t begin = i == 0 ? 0 : chunk.spans[i - 1].end;
				expanded.insert(expanded.end(), chunk.faces.begin() + begin, chunk.faces.begin() + chunk.spans[i].end);
				for (size_t n = i == 0 ? 0 : chunk.instance_spans[i - 1].end; n < chunk.instance_spans[i].end; ++n) {
					const auto &v = chunk.instances[n];
					size_t first = expanded.size();
					AppendModel3D(expanded, *v.model, v.tile, v.origin, v.dx, v.dy, v.length, v.height, v.company);
					for (size_t f = first; f < expanded.size(); ++f) expanded[f].transparency = v.transparency;
				}
			}
			Vec low{1e30f,1e30f,1e30f}, high{-1e30f,-1e30f,-1e30f};
			for (const auto &face : reference) for (auto p : face.p) {
				low={std::min(low.x,p.x),std::min(low.y,p.y),std::min(low.z,p.z)};
				high={std::max(high.x,p.x),std::max(high.y,p.y),std::max(high.z,p.z)};
			}
			++mesh_checks;
			mesh_mismatches += expanded != reference || chunk.low != low || chunk.high != high;
		}
	}
	if (_terrain_verify) Debug(driver, 1, "PHASE7 mesh-reference chunks={} mismatches={}", mesh_checks, mesh_mismatches);
	_proof_mesh_us += ProofMicros(start);
}

/** Compare authoritative slope/corner-derived snapshots: a shared corner invalidates every affected tile, including across chunks. */
static void RefreshTerrain(bool full_scan = false)
{
	int nx=(Map::SizeX()+_terrain_chunk_size-1)/_terrain_chunk_size, ny=(Map::SizeY()+_terrain_chunk_size-1)/_terrain_chunk_size;
	if (_terrain_reset || _terrain_map_x!=int(Map::SizeX()) || _terrain_map_y!=int(Map::SizeY())) {
		_terrain_reset = false;
		++_terrain_generation;
		ClearProofImages();
		_terrain_chunks.clear(); _terrain_map_x=Map::SizeX(); _terrain_map_y=Map::SizeY();
		_ground_map.assign(Map::Size(), Ground3D::GRASS_3);
		_ground_dirty_x0 = _ground_dirty_y0 = 0;
		_ground_dirty_x1 = _terrain_map_x - 1; _ground_dirty_y1 = _terrain_map_y - 1;
	}
	_terrain_chunks.resize(nx*ny);
	bool changed=false;
	/* Changed tiles' old and new extents, for redrawing only the views that see them; a first read redraws all. */
	std::vector<ProofBox> boxes;
	bool whole = false;
	_terrain_last_rebuilt=0;
	for (int cy=0;cy<ny;++cy) for (int cx=0;cx<nx;++cx) {
		auto &chunk=_terrain_chunks[cy*nx+cx];
		if (!full_scan && _proof_retained && chunk.dirty.Empty()) continue;
		int width = std::min(_terrain_chunk_size, _terrain_map_x - cx * _terrain_chunk_size);
		int height = std::min(_terrain_chunk_size, _terrain_map_y - cy * _terrain_chunk_size);
		_proof_tiles_scan_candidates += width * height;
		bool initial = chunk.tiles.size() != size_t(width * height);
		whole |= initial;
		auto changed_tiles = chunk.dirty.Refresh(chunk.tiles, width * height,
			full_scan || !_proof_retained || !_proof_dirty_tiles, [&](size_t i) {
			int x = cx * _terrain_chunk_size + static_cast<int>(i % width);
			int y = cy * _terrain_chunk_size + static_cast<int>(i / width);
			++_proof_tiles_scanned;
			TileIndex tile=TileXY(x,y);
			auto [ground,gz]=GetTilePixelSlope(tile);
			auto [surface,sz]=GetFoundationPixelSlope(tile);
			TerrainTile snapshot{ground,surface,gz,sz,GetTileType(tile)};
			if (snapshot.type == TileType::Clear) snapshot.ground_material = to_underlying(IsSnowTile(tile) ? ClearGround::Snow : GetClearGround(tile));
			if (snapshot.type == TileType::Trees) {
				auto ground_type = GetTreeGround(tile);
				if (ground_type == TreeGround::SnowOrDesert || ground_type == TreeGround::RoughSnow) snapshot.ground_material = to_underlying(_settings_game.game_creation.landscape == LandscapeType::Arctic ? ClearGround::Snow : ClearGround::Desert);
			}
			snapshot.ground_layer = ReadGround3D(tile);
			ReadRail3D(tile, snapshot);
			ReadAsset3D(tile, snapshot);
			if (_infrastructure_audit && snapshot.crossing && (chunk.revision == 0 || snapshot.barred != chunk.tiles[i].barred)) {
				Debug(driver,1,"PHASE4 CROSSING tile=({}, {}) barred={}",x,y,snapshot.barred);
			}
			if (full_scan && !(chunk.tiles[i] == snapshot)) {
				const auto &a = chunk.tiles[i], &b = snapshot;
				Debug(driver, 0, "PHASE7 missed tile=({}, {}) type={}/{} material={}/{} layer={}/{} stage={}/{} trees={}/{} signals={}/{} green={}/{} ground={}/{} surface={}/{} asset={}/{}",
					x, y,
					int(a.type), int(b.type), a.ground_material, b.ground_material, a.ground_layer, b.ground_layer, a.asset_stage, b.asset_stage, a.tree_count, b.tree_count,
					a.signals, b.signals, a.green_signals, b.green_signals, int(a.ground), int(b.ground), int(a.surface), int(b.surface), a.asset, b.asset);
			}
			if (!initial && !(chunk.tiles[i] == snapshot)) {
				auto [old_low, old_high] = TerrainTileBounds(chunk.tiles[i], x, y);
				auto [low, high] = TerrainTileBounds(snapshot, x, y);
				boxes.push_back({{low.x, low.y, std::min(low.z, old_low.z)}, {high.x, high.y, std::max(high.z, old_high.z)}});
			}
			return snapshot;
		});
		if (std::count(changed_tiles.begin(), changed_tiles.end(), true) == 0) continue;
		changed = true;
		++_terrain_last_rebuilt;
		chunk.mesh_changes.Merge(changed_tiles);
		for (size_t i = 0; i < changed_tiles.size(); ++i) if (changed_tiles[i]) {
			int x = cx * _terrain_chunk_size + int(i % width), y = cy * _terrain_chunk_size + int(i / width);
			ExpandTerrainBounds(chunk, chunk.tiles[i], x, y);
			uint8_t &ground = _ground_map[TileXY(x, y).base()];
			if (ground == chunk.tiles[i].ground_layer) continue;
			ground = chunk.tiles[i].ground_layer;
			if (_ground_dirty_x1 < _ground_dirty_x0) {
				_ground_dirty_x0 = _ground_dirty_x1 = x; _ground_dirty_y0 = _ground_dirty_y1 = y;
			} else {
				_ground_dirty_x0 = std::min(_ground_dirty_x0, x); _ground_dirty_x1 = std::max(_ground_dirty_x1, x);
				_ground_dirty_y0 = std::min(_ground_dirty_y0, y); _ground_dirty_y1 = std::max(_ground_dirty_y1, y);
			}
		}

	}
	if (changed) {
		++_terrain_revision;
		if (whole) InvalidateProof3DImages(); else InvalidateProof3DRegions(boxes);
		Debug(driver, 1, "3D terrain revision {}: rebuilt {} chunks of {}x{}", _terrain_revision, _terrain_last_rebuilt, _terrain_chunk_size, _terrain_chunk_size);
	}
	if (_terrain_verify && !full_scan && _proof_retained) {
		RefreshTerrain(true);
		Debug(driver, 1, "PHASE7 dirty-reference missed_chunks={}", _terrain_last_rebuilt);
	}
	if (_terrain_verify && full_scan) {
		int checks = 0, failures = 0;
		std::vector<TerrainFace> reference;
		for (int cy = 0; cy < ny; ++cy) for (int cx = 0; cx < nx; ++cx) {
			const auto &chunk = _terrain_chunks[cy * nx + cx];
			if (chunk.mesh_changes.Empty()) continue;
			int width = std::min(_terrain_chunk_size, _terrain_map_x - cx * _terrain_chunk_size);
			reference.clear();
			for (size_t i = 0; i < chunk.tiles.size(); ++i) BuildTileGeometry(reference, chunk.tiles[i], cx * _terrain_chunk_size + int(i % width), cy * _terrain_chunk_size + int(i / width));
			++checks;
			for (const auto &face : reference) for (auto p : face.p) {
				failures += p.x < chunk.low.x || p.y < chunk.low.y || p.z < chunk.low.z || p.x > chunk.high.x || p.y > chunk.high.y || p.z > chunk.high.z;
			}
		}
		Debug(driver, 1, "PHASE7 bounds-reference chunks={} failures={}", checks, failures);
	}
	if (!_proof_deferred && !full_scan) PrepareTerrainMeshes();
}

/** Where a cab sits on its vehicle: how far its front is ahead of the vehicle's position, and the eye height, from the
 * model the vehicle is drawn with as RefreshTrains3D picks it. The eye is CAB_EYE_HEIGHT of the way up the front part. */
static std::array<float, 2> CabMount3D(const Vehicle &v)
{
	constexpr float CAB_EYE_HEIGHT = 0.8f, CAB_FRONT_PART = 0.3f;
	bool ground = v.IsGroundVehicle();
	float half = ground ? std::max(1.0f, v.GetGroundVehicleCache()->cached_veh_length * 0.5f - 0.4f) : 8.0f;
	/* As RefreshTrains3D: a flipped unit or a rear dual head is drawn facing backwards, so its front is the model's back. */
	bool reversed = v.type == VehicleType::Train && Train::From(&v)->IsRearDualheaded() != Train::From(&v)->flags.Test(VehicleRailFlag::Flipped);
	int art = VehicleArt3D(v.engine_type, v.cargo.StoredCount() > 0);
	int asset = ground ? VehicleAsset3D(v.engine_type) : -1;
	if (art < 0 && asset < 0) return {half, 5}; // The fallback boxes are 4 to 6.5 high.
	size_t index = art >= 0 ? size_t(art) : std::size(Art3D::models) + size_t(asset);
	float length = art >= 0 && !ground ? 1.0f : half / 3.6f;
	static std::map<std::pair<size_t, bool>, std::array<float, 2>> fronts; // Model front and front part height, in model units.
	auto [it, added] = fronts.try_emplace({index, reversed});
	if (added) {
		const auto &model = VehicleModel3D(index);
		float front = -1e30f, back = 1e30f, height = 0;
		for (size_t i = 0; i < model.count; ++i) for (const auto &p : model.faces[i].p) {
			front = std::max(front, reversed ? -p[0] : p[0]);
			back = std::min(back, reversed ? -p[0] : p[0]);
		}
		for (size_t i = 0; i < model.count; ++i) for (const auto &p : model.faces[i].p) {
			if ((reversed ? -p[0] : p[0]) >= front - (front - back) * CAB_FRONT_PART) height = std::max(height, p[2]);
		}
		it->second = model.count == 0 ? std::array<float, 2>{half / length, 5 / CAB_EYE_HEIGHT} : std::array<float, 2>{front, height};
	}
	return {it->second[0] * length, it->second[1] * CAB_EYE_HEIGHT};
}
}

void SetTerrain3DVerification(bool enabled) { _terrain_verify = enabled; }

void ResetTerrain3D()
{
	_terrain_reset = true;
	/* Screenshots may request a viewport outside the normal drawing pass. */
	_proof_refreshed = false;
}

void MarkTerrain3DDirty(int x, int y)
{
	_proof_refreshed = false;
	if (_terrain_reset || _terrain_chunks.empty()) return;
	/* A corner contributes to adjacent tiles, including across chunk boundaries. */
	int nx = (_terrain_map_x + _terrain_chunk_size - 1) / _terrain_chunk_size;
	Terrain3D::MarkDirtyTiles(x, y, _terrain_map_x, _terrain_map_y, _terrain_chunk_size,
		[&](int cx, int cy, size_t index) { _terrain_chunks[cy * nx + cx].dirty.Mark(index); });
}

static float WrapYaw(float yaw)
{
	yaw = std::fmod(yaw, 360.0f);
	return yaw < 0 ? yaw + 360 : yaw;
}

static void InvalidateTerrain3DCamera()
{
	/* The direct layer can rotate without repainting UI pixels, but map labels live in
	 * that UI layer. Repaint every shared viewport so old projected text is cleared. */
	for (const Window *w : Window::Iterate()) {
		if (w->viewport == nullptr || w->viewport->camera_3d != ViewportCamera3D::Normal) continue;
		const auto &vp = *w->viewport;
		SetDirtyBlocks(vp.left, vp.top, vp.left + vp.width, vp.top + vp.height);
	}
	/* Camera hints change when leaving or returning to the classic angle. */
	static bool last_classic = true;
	InvalidateProof3DImages();
	bool classic = IsTerrain3DCameraClassic();
	if (classic != last_classic) MarkWholeScreenDirty();
	last_classic = classic;
}

/** Move the pivot to the ground at the main viewport centre without changing the picture. */
static void RecentreTerrain3DPivot()
{
	Window *w = GetMainWindow();
	if (w == nullptr || w->viewport == nullptr) return;
	ViewportData &vp = *w->viewport;
	Point ground = PickTerrain3D(vp, vp.left + vp.width / 2, vp.top + vp.height / 2, true);
	if (ground.x < 0 || ground.y < 0) return;
	int z = GetSlopePixelZOutsideMap(ground.x, ground.y);
	/* The ground point is at the centre in both the classic and rotated views once it lies on the classic centre ray. */
	ScrollWindowTo(ground.x, ground.y, z, w, true);
	/* ScrollWindowTo skips an unchanged destination; apply it now so this frame's camera already uses it. */
	vp.virtual_left = vp.scrollpos_x = vp.dest_scrollpos_x;
	vp.virtual_top = vp.scrollpos_y = vp.dest_scrollpos_y;
	_terrain_pivot_z = static_cast<float>(z);
}

static bool ClassicTerrainTarget()
{
	return WrapYaw(_terrain_target_yaw) == 0 && _terrain_target_tilt == 100;
}

/** Animate to the classic camera along the shortest turn, remembering the orbit angle for Home. */
static void ReturnTerrain3DToClassic()
{
	if (ClassicTerrainTarget()) return;
	_terrain_orbit_yaw = WrapYaw(_terrain_target_yaw);
	_terrain_orbit_tilt = _terrain_target_tilt;
	_terrain_target_yaw = std::round(_terrain_yaw / 360) * 360;
	_terrain_target_tilt = 100;
}

void SetTerrain3DCamera(int yaw, int tilt)
{
	if (ClassicTerrainCamera()) RecentreTerrain3DPivot();
	_terrain_yaw = _terrain_target_yaw = WrapYaw(static_cast<float>(yaw));
	_terrain_tilt = _terrain_target_tilt = static_cast<float>(std::clamp(tilt, 40, 180));
	InvalidateTerrain3DCamera();
}

void OrbitTerrain3DCamera(float yaw, float tilt, bool animate)
{
	static std::chrono::steady_clock::time_point last_input{};
	auto now = std::chrono::steady_clock::now();
	/* Picking scans every face, so only recentre when a new gesture starts. */
	if (now - last_input > std::chrono::milliseconds(300)) RecentreTerrain3DPivot();
	last_input = now;
	_terrain_target_yaw += yaw;
	_terrain_target_tilt = std::clamp(_terrain_target_tilt + tilt, 40.0f, 180.0f);
	if (!animate) {
		_terrain_yaw = _terrain_target_yaw = WrapYaw(_terrain_target_yaw);
		_terrain_tilt = _terrain_target_tilt;
		InvalidateTerrain3DCamera();
	}
}

void ToggleTerrain3DCamera()
{
	if (!ClassicTerrainTarget()) {
		ReturnTerrain3DToClassic();
		return;
	}
	if (ClassicTerrainCamera()) RecentreTerrain3DPivot();
	/* Turn the short way from wherever the camera currently is. */
	_terrain_target_yaw = _terrain_yaw + std::remainder(_terrain_orbit_yaw - _terrain_yaw, 360.0f);
	_terrain_target_tilt = _terrain_orbit_tilt;
}

void ReturnTerrain3DCamera()
{
	if (!ClassicTerrainTarget()) ReturnTerrain3DToClassic();
}

void SetTerrain3DMagnify(float magnify)
{
	if (_terrain_magnify == magnify) return;
	_terrain_magnify = magnify;
	InvalidateTerrain3DCamera();
}

void PlaceTerrain3DCamera(float yaw, float tilt)
{
	if (ClassicTerrainCamera() && (yaw != 0 || tilt != 100)) RecentreTerrain3DPivot();
	_terrain_yaw = _terrain_target_yaw = yaw;
	_terrain_tilt = _terrain_target_tilt = std::clamp(tilt, 40.0f, 180.0f);
	InvalidateTerrain3DCamera();
}

std::pair<float, float> GetTerrain3DCamera()
{
	return {_terrain_yaw, _terrain_tilt};
}

bool IsTerrain3DCameraClassic()
{
	return !IsProof3DEnabled() || ClassicTerrainCamera();
}

Point Terrain3DScrollDelta(int dx, int dy)
{
	if (IsTerrain3DCameraClassic()) return {dx, dy};
	/* Small drags would otherwise lose their fractions to rounding at every event. */
	static float remainder_x = 0, remainder_y = 0;
	float angle = _terrain_yaw * 3.14159265359f / 180;
	auto delta = Terrain3D::ClassicScrollDelta(float(dx), float(dy), std::cos(angle), std::sin(angle), _terrain_tilt / 100.0f);
	remainder_x += delta[0];
	remainder_y += delta[1];
	Point result{static_cast<int>(remainder_x), static_cast<int>(remainder_y)};
	remainder_x -= result.x;
	remainder_y -= result.y;
	return result;
}

/** Ease the management camera towards its target, independently of the active tool. */
static const IntervalTimer<TimerWindow> _terrain_camera_animation(std::chrono::milliseconds(15), [](auto count) {
	if (!IsProof3DEnabled()) return;
	if (_terrain_yaw == _terrain_target_yaw && _terrain_tilt == _terrain_target_tilt) return;
	float step = 1 - std::exp(-15.0f * count / 70);
	_terrain_yaw += (_terrain_target_yaw - _terrain_yaw) * step;
	_terrain_tilt += (_terrain_target_tilt - _terrain_tilt) * step;
	if (std::abs(_terrain_target_yaw - _terrain_yaw) < 0.05f && std::abs(_terrain_target_tilt - _terrain_tilt) < 0.05f) {
		_terrain_yaw = _terrain_target_yaw = WrapYaw(_terrain_target_yaw);
		_terrain_tilt = _terrain_target_tilt;
		Debug(driver, 1, "3D camera settled yaw={} tilt={} pivot_z={}", _terrain_yaw, _terrain_tilt, _terrain_pivot_z);
	}
	InvalidateTerrain3DCamera();
});

std::string AuditAssets3D()
{
	RefreshTerrain();
	int houses=0, industries=0, trees=0, missing=0, construction=0;
	for (const Town *town:Town::Iterate()) Debug(driver,1,"PHASE5 TOWN tile=({}, {})",TileX(town->xy),TileY(town->xy));
	std::array<int,std::size(Asset3D::models)> counts{};
	for (const auto &chunk:_terrain_chunks) for (const auto &t:chunk.tiles) {
		if (t.asset<0) continue;
		houses+=t.type==TileType::House; industries+=t.type==TileType::Industry; trees+=t.tree_count;
		missing+=t.asset==Asset3D::MISSING;
		construction+=t.tree_count==0 && t.asset_stage<3;
		++counts[t.asset];
	}
	for (size_t i=0;i<counts.size();++i) if (counts[i]) Debug(driver,1,"PHASE5 MODEL id={} tiles={}",i,counts[i]);
	for (const Train *v:Train::Iterate()) Debug(driver,1,"PHASE5 VEHICLE engine={} model={}",v->engine_type,VehicleAsset3D(v->engine_type));
	for (const RoadVehicle *v:RoadVehicle::Iterate()) Debug(driver,1,"PHASE5 VEHICLE engine={} model={}",v->engine_type,VehicleAsset3D(v->engine_type));
	auto result=fmt::format("PHASE5 ASSETS houses={} industry_tiles={} trees={} missing={} construction={}",houses,industries,trees,missing,construction);
	Debug(driver,1,"{}",result);
	return result;
}

/** Enumerated live coverage, including absent classes rather than inferred support. */
std::string AuditCoverage3D()
{
	RefreshTerrain();
	RefreshTrains3D();
	std::array<int, static_cast<size_t>(StationType::End)> stations{};
	std::array<int, 6> ground{};
	int slopes=0, steep=0, water_depots=0, locks=0, missing=0;
	for (const auto &chunk : _terrain_chunks) for (const auto &t : chunk.tiles) {
		if (t.facility >= 0) ++stations[t.facility];
		if (t.type == TileType::Clear) ++ground[t.ground_material];
		slopes += t.ground != SLOPE_FLAT;
		steep += IsSteepSlope(t.ground);
		water_depots += t.water_depot >= 0;
		locks += t.lock >= 0;
		missing += t.asset == Asset3D::MISSING;
	}
	for (size_t i=0; i<stations.size(); ++i) Debug(driver,0,"PHASE6 STATION type={} tiles={}",i,stations[i]);
	for (size_t i=0; i<ground.size(); ++i) Debug(driver,0,"PHASE6 GROUND material={} tiles={}",i,ground[i]);
	/* Phase 8 ground texture layers in use, with one example tile each for review captures. */
	std::array<int, Ground3D::LAYER_COUNT> layers{};
	std::array<uint32_t, Ground3D::LAYER_COUNT> first{};
	for (size_t i=0; i<_ground_map.size(); ++i) {
		int layer=_ground_map[i] % Ground3D::NO_BLEND;
		if (layers[layer]++ == 0) first[layer]=static_cast<uint32_t>(i);
	}
	for (size_t i=0; i<layers.size(); ++i) if (layers[i]>0) {
		Debug(driver,0,"PHASE8 GROUND layer={} tiles={} first=({}, {})",Ground3D::layer_names[i],layers[i],TileX(TileIndex(first[i])),TileY(TileIndex(first[i])));
	}
	/* Phase 8.6 map objects, field fence edges and rock clusters, with the model each resolves to. */
	for (const Object *o : Object::Iterate()) {
		TerrainTile s{SLOPE_FLAT,SLOPE_FLAT,0,0,TileType::Object};
		ReadAsset3D(o->location.tile,s);
		Debug(driver,0,"PHASE8 OBJECT type={} tile=({}, {}) art={}",o->type,TileX(o->location.tile),TileY(o->location.tile),s.art);
	}
	int fence_edges=0, fence_missing=0, rock_tiles=0;
	TileIndex first_fence=INVALID_TILE, first_rocks=INVALID_TILE;
	for (uint _y3d = 0; _y3d < Map::SizeY(); ++_y3d) for (uint _x3d = 0; _x3d < Map::SizeX(); ++_x3d) {
		TileIndex tile = TileXY(_x3d, _y3d);
		if (!IsTileType(tile,TileType::Clear)) continue;
		TerrainTile s{SLOPE_FLAT,SLOPE_FLAT,0,0,TileType::Clear};
		ReadAsset3D(tile,s);
		if (s.art>=0 && rock_tiles++==0) first_rocks=tile;
		for (int slot=0;slot<4;++slot) if (GB(s.fences,3*slot,3)!=0) {
			if (fence_edges++==0) first_fence=tile;
			fence_missing+=Art3D::Fence(GB(s.fences,3*slot,3))<0;
		}
	}
	Debug(driver,0,"PHASE8 LANDSCAPE fence_edges={} fence_missing={} first_fence=({}, {}) rock_tiles={} first_rocks=({}, {})",fence_edges,fence_missing,
		first_fence==INVALID_TILE?-1:int(TileX(first_fence)),first_fence==INVALID_TILE?-1:int(TileY(first_fence)),
		rock_tiles,first_rocks==INVALID_TILE?-1:int(TileX(first_rocks)),first_rocks==INVALID_TILE?-1:int(TileY(first_rocks)));
	/* Phase 8.6 station, stop, depot and lock pieces: identity * 4 + part, as ReadStation3D. */
	std::map<int, std::pair<int, TileIndex>> pieces;
	int piece_missing=0;
	for (uint _y3d = 0; _y3d < Map::SizeY(); ++_y3d) for (uint _x3d = 0; _x3d < Map::SizeX(); ++_x3d) {
		TileIndex tile = TileXY(_x3d, _y3d);
		TerrainTile s{SLOPE_FLAT,SLOPE_FLAT,0,0,GetTileType(tile)};
		int piece=ReadStation3D(tile,s);
		if (piece<0) continue;
		auto &entry=pieces.try_emplace(piece,0,tile).first->second;
		++entry.first;
		piece_missing+=s.art<0 && !s.art_bare;
	}
	for (const auto &[piece, entry] : pieces) {
		TerrainTile s{SLOPE_FLAT,SLOPE_FLAT,0,0,GetTileType(entry.second)};
		ReadStation3D(entry.second,s);
		Debug(driver,0,"PHASE8 STATION id={} part={} tiles={} first=({}, {}) turn={} art={}{}",piece/4,piece%4,entry.first,TileX(entry.second),TileY(entry.second),to_underlying(s.art_turn),s.art,s.art_bare?" bare":"");
	}
	Debug(driver,0,"PHASE8 STATION missing={}",piece_missing);
	/* Phase 8.6 bridge middles (kind 0, by BridgePieces), ramps (kind 1, piece 1 = sloped) and tunnel portals (kind 2),
	 * as ReadRail3D; type -1 is an aqueduct or NewGRF bridge drawn with the placeholder. */
	struct SpanEntry { int tiles; TileIndex first; int art; };
	std::map<std::tuple<int, int, int>, SpanEntry> spans;
	int span_missing=0;
	for (uint _y3d = 0; _y3d < Map::SizeY(); ++_y3d) for (uint _x3d = 0; _x3d < Map::SizeX(); ++_x3d) {
		TileIndex tile = TileXY(_x3d, _y3d);
		auto [ground,gz]=GetTilePixelSlope(tile);
		auto [surface,sz]=GetFoundationPixelSlope(tile);
		TerrainTile s{ground,surface,gz,sz,GetTileType(tile)};
		ReadRail3D(tile,s);
		auto count=[&](int kind, int type, int piece, int art) {
			auto &e=spans.try_emplace({kind,type,piece},SpanEntry{0,tile,art}).first->second;
			++e.tiles;
			span_missing+=type>=0 && art<0;
		};
		if (s.deck_z>=0) count(0,s.deck_type,s.deck_piece,s.deck_type>=0?Art3D::BridgePiece(s.deck_type,s.deck_piece):-1);
		if (s.ramp>=0) count(1,s.ramp_type,s.surface==SLOPE_FLAT,s.ramp_type>=0?Art3D::BridgeRamp(s.ramp_type,s.surface==SLOPE_FLAT):-1);
		if (s.portal>=0) count(2,s.portal_kind,0,s.portal_kind>=0?Art3D::Tunnel(s.portal_kind):-1);
	}
	for (const auto &[key, e] : spans) {
		Debug(driver,0,"PHASE8 BRIDGE kind={} type={} piece={} tiles={} first=({}, {}) art={}",std::get<0>(key),std::get<1>(key),std::get<2>(key),e.tiles,TileX(e.first),TileY(e.first),e.art);
	}
	Debug(driver,0,"PHASE8 BRIDGE missing={}",span_missing);
	/* Phase 8.6 catenary: pylons by model (0 straight, 1 diagonal position) and wires (2 pylons at both ends, 3 at one). */
	std::array<int, 4> catenary{};
	int catenary_tiles=0, catenary_missing=0;
	TileIndex first_catenary=INVALID_TILE;
	for (uint _y3d = 0; _y3d < Map::SizeY(); ++_y3d) for (uint _x3d = 0; _x3d < Map::SizeX(); ++_x3d) {
		TileIndex tile = TileXY(_x3d, _y3d);
		std::vector<RailCatenaryPiece> found;
		CollectRailCatenary(tile,found);
		if (found.empty()) continue;
		if (catenary_tiles++==0) first_catenary=tile;
		for (const auto &c : found) {
			int piece=CatenaryPiece3D(c);
			++catenary[piece];
			catenary_missing+=Art3D::Catenary(piece)<0;
		}
	}
	Debug(driver,0,"PHASE8 CATENARY tiles={} first=({}, {}) pylons={} pylons_diagonal={} wires_both={} wires_half={} missing={}",catenary_tiles,
		first_catenary==INVALID_TILE?-1:int(TileX(first_catenary)),first_catenary==INVALID_TILE?-1:int(TileY(first_catenary)),
		catenary[0],catenary[1],catenary[2],catenary[3],catenary_missing);
	/* Phase 8.7: effect vehicles by sequence (EFFECT3D_SEQUENCES order) and industry moving parts now drawn. */
	RefreshEffects3D();
	std::array<int, std::size(EFFECT3D_SEQUENCES)> effects{};
	int parts=0;
	for (const auto &v : _effect3d_instances) {
		bool effect=false;
		for (size_t i=0;i<effects.size();++i) for (uint f=0;f<EFFECT3D_SEQUENCES[i].second;++f) if (Art3D::Effect(uint(i),f)==int(v.model)) { ++effects[i]; effect=true; }
		parts+=!effect;
	}
	Debug(driver,0,"PHASE8 EFFECTS chimney={} steam={} diesel={} spark={} smoke={} explosion_large={} breakdown={} explosion_small={} bulldozer={} industry_parts={} missing={}",
		effects[0],effects[1],effects[2],effects[3],effects[4],effects[5],effects[6],effects[7],effects[8],parts,_effect3d_missing);
	for (const Vehicle *v : Vehicle::Iterate()) {
		if (to_underlying(v->type) > to_underlying(VehicleType::Aircraft) || !v->IsPrimaryVehicle()) continue;
		Debug(driver,0,"PHASE6 VEHICLE id={} type={} x={} y={} z={} hidden={} depot={}",v->index,v->type,v->x_pos,v->y_pos,v->z_pos,v->vehstatus.Test(VehState::Hidden),v->IsInDepot());
	}
	auto result=fmt::format("PHASE6 COVERAGE climate={} slopes={} steep={} ship_depots={} locks={} missing_assets={} rendered_vehicles={}",static_cast<int>(_settings_game.game_creation.landscape),slopes,steep,water_depots,locks,missing,_train3d_snapshots.size());
	Debug(driver,0,"{}",result);
	return result;
}

std::string AuditInfrastructure3D()
{
	RefreshTerrain();
	_infrastructure_audit=true;
	_infrastructure_observations.clear();
	int roads=0,crossings=0,portals=0,ramps=0,decks=0,checks=0,failures=0;
	for (TileIndex tile=TileIndex{0}; tile < Map::Size(); ++tile) {
		auto [ground,gz]=GetTilePixelSlope(tile);
		auto [surface,sz]=GetFoundationPixelSlope(tile);
		TerrainTile t{ground,surface,gz,sz,GetTileType(tile)};
		ReadRail3D(tile,t);
		roads+=t.roads!=ROAD_NONE; crossings+=t.crossing; portals+=t.portal>=0; ramps+=t.ramp>=0; decks+=t.deck_z>=0;
		if (t.ramp<0 && t.portal<0) continue;
		for (int y=0;y<16;++y) for (int x=0;x<16;++x) {
			float z=t.portal>=0?float(t.ground_z):Rail3D::RampHeight(t.ramp,t.surface==SLOPE_FLAT,t.surface_z,float(x),float(y));
			++checks;
			if (std::abs(z-GetSlopePixelZ(TileX(tile)*16+x,TileY(tile)*16+y,true))>0.51f) ++failures;
		}
	}
	auto result=fmt::format("PHASE4 AUDIT roads={} crossings={} portals={} ramps={} deck_tiles={} height_checks={} failures={}",roads,crossings,portals,ramps,decks,checks,failures);
	Debug(driver,1,"{}",result);
	return result;
}

void SetTerrain3DChunkSize(int size)
{
	_terrain_chunk_size=(size==32?32:16); ResetTerrain3D(); InvalidateProof3DImages();
}

void SetTerrain3DWireframe(bool enabled)
{
	_terrain_wireframe=enabled; InvalidateProof3DImages();
}

void SetViewportCamera3DLog(bool enabled)
{
	_chase_log = enabled;
}

static RideEvent3DHook _ride_event_hook = nullptr; ///< Test scripts: told of ride-along view events.

void SetViewportCamera3DEventHook(RideEvent3DHook hook)
{
	_ride_event_hook = hook;
}

/** Test scripts: what a ride-along view last saw of its vehicle, to tell its events. */
struct RideWatch3D {
	uint8_t state = 0; ///< Aircraft movement state, or whether a road vehicle is overtaking.
	std::vector<uint8_t> loop; ///< Aircraft positions flown through since it started flying.
	bool holding = false; ///< Whether this flight came back round to a position: a holding pattern.
};

/** Test scripts: tell the hook of fade changes, of an aircraft taking off, landing or holding, and of a road vehicle
 * overtaking or turning round. */
static void WatchRide3D(const Viewport &vp, const Vehicle &v, float before, float dark)
{
	if (_ride_event_hook == nullptr) return;
	static std::map<const Viewport *, RideWatch3D> watches;
	auto event = [&](std::string_view name) { _ride_event_hook(v.index, name, dark); };
	if (before < 0.5f && dark >= 0.5f) event("entering");
	if (before < 1 && dark >= 1) event("black");
	if (before >= 0.5f && dark < 0.5f) event("leaving");
	if (before > 0 && dark == 0) event("clear");
	bool fresh = !vp.camera_follow_set;
	auto &watch = watches[&vp];
	if (v.type == VehicleType::Aircraft) {
		const Aircraft *a = Aircraft::From(&v);
		if (!fresh && a->state != watch.state) {
			if (a->state == AS_FLYING_TAKEOFF || a->state == AS_FLYING_HELICOPTER_TAKEOFF) event(a->subtype == AIR_HELICOPTER ? "helitakeoff" : "takeoff");
			if (a->state == AS_FLYING_LANDING || a->state == AS_FLYING_HELICOPTER_LANDING) event(a->subtype == AIR_HELICOPTER ? "helilanding" : "landing");
			watch.loop.clear();
			watch.holding = false;
		}
		if (a->state == AS_FLYING && (watch.loop.empty() || watch.loop.back() != to_underlying(a->next_pos.pos))) {
			if (!watch.holding && std::ranges::find(watch.loop, to_underlying(a->next_pos.pos)) != watch.loop.end()) {
				watch.holding = true;
				event("holding");
			}
			watch.loop.push_back(to_underlying(a->next_pos.pos));
		}
		watch.state = to_underlying(a->state);
	} else if (v.type == VehicleType::Road) {
		const RoadVehicle *rv = RoadVehicle::From(&v);
		uint8_t now = (rv->overtaking != 0 ? 1 : 0) | (rv->state < RVSB_IN_ROAD_STOP && IsReversingRoadTrackdir(static_cast<Trackdir>(rv->state & RVSB_TRACKDIR_MASK)) ? 2 : 0);
		if (!fresh && (now & 1) && !(watch.state & 1)) event("overtaking");
		if (!fresh && (now & 2) && !(watch.state & 2)) event("turning");
		watch.state = now;
	}
}

/** Test scripts: the vehicle's own movement state for the chase log; aircraft state and position, road vehicle state and
 * overtaking, train track. */
static std::pair<int, int> RideState3D(const Vehicle &v)
{
	switch (v.type) {
		case VehicleType::Aircraft: return {to_underlying(Aircraft::From(&v)->state), to_underlying(Aircraft::From(&v)->next_pos.pos)};
		case VehicleType::Road: return {RoadVehicle::From(&v)->state, RoadVehicle::From(&v)->overtaking};
		case VehicleType::Train: return {static_cast<int>(Train::From(&v)->track), 0};
		default: return {0, 0};
	}
}

/** Ease a chase or cab view towards its vehicle once per frame, fading it out in tunnels, and return it to normal
 * when the ride ends; presentation only. Returns whether its camera moved. */
bool UpdateViewportCamera3D(Viewport &vp, uint32_t delta_ms)
{
	bool cab = vp.camera_3d == ViewportCamera3D::Cab;
	if ((!cab && vp.camera_3d != ViewportCamera3D::Chase) || !Vehicle::IsValidID(vp.camera_vehicle)) {
		vp.camera_follow_set = false;
		return false;
	}
	const Vehicle *v = Vehicle::Get(vp.camera_vehicle);
	if (RideEnded3D(*v)) {
		if (_chase_log) Debug(driver, 0, "PHASE9 SNAPBACK vehicle={} camera={} crashed={} depot={}", v->index.base(), cab ? "cab" : "chase", v->vehstatus.Test(VehState::Crashed), v->IsPrimaryVehicle() && v->IsInDepot());
		if (_ride_event_hook != nullptr) _ride_event_hook(v->index, "snapback", 0);
		vp.camera_3d = ViewportCamera3D::Normal;
		vp.camera_follow_set = false;
		return true;
	}
	float ahead = cab ? CabMount3D(*v)[0] : 0;
	bool inside = VehicleInTunnel3D(*v) || (cab && TunnelPortalAhead3D(*v, ahead));
	float to_go = TunnelToGo3D(*v, ahead);
	float dark = Terrain3D::TunnelDark(vp.camera_follow_set ? vp.camera_dark : inside ? 1.0f : 0.0f, inside, to_go, vp.camera_follow_set ? delta_ms / 1000.0f : 0);
	auto target = ChaseVehicleTarget(*v);
	auto &f = vp.camera_follow;
	Terrain3D::ChaseFollow follow{target, target};
	if (vp.camera_follow_set) {
		follow = {{f[0], f[1], f[2], f[3], f[4]}, {f[5], f[6], f[7], f[8], f[9]}, {f[10], f[11], f[12]}};
		follow = Terrain3D::FollowChase(follow, target, static_cast<int>(std::min<uint64_t>((uint)_tick_counter - vp.camera_follow_tick, 1000)), delta_ms / 1000.0f, cab ? Terrain3D::CAB_TIMING : Terrain3D::CHASE_TIMING);
	}
	const auto &smooth = follow.smooth;
	auto eye = cab ? CabTerrainEye(*v, smooth) : ChaseTerrainEye(smooth);
	bool moved = !vp.camera_follow_set || eye != vp.camera_eye || dark != vp.camera_dark;
	WatchRide3D(vp, *v, vp.camera_follow_set ? vp.camera_dark : dark, dark);
	if (_chase_log) {
		auto [state, detail] = RideState3D(*v);
		Debug(driver, 0, "PHASE9 CHASE vehicle={} type={} tile_type={} ground={} ms={} target={},{},{},{} smooth={},{},{},{} eye={},{},{} pitch={} camera={} grade={},{} dark={} inside={} to_go={} state={},{}", v->index.base(), int(v->type), int(GetTileType(v->tile)),
			GetSlopePixelZOutsideMap(v->x_pos, v->y_pos), delta_ms, target.x, target.y, target.z, target.heading, smooth.x, smooth.y, smooth.z, smooth.heading, eye[0], eye[1], eye[2], eye[4], cab ? "cab" : "chase", target.grade, smooth.grade, dark, inside, to_go, state, detail);
	}
	f = {smooth.x, smooth.y, smooth.z, smooth.heading, smooth.grade, target.x, target.y, target.z, target.heading, target.grade, follow.offset.x, follow.offset.y, follow.offset.z};
	vp.camera_follow_tick = (uint)_tick_counter;
	vp.camera_eye = eye;
	vp.camera_dark = dark;
	vp.camera_follow_set = true;
	return moved;
}

Point ProjectTerrain3DLabel(const Viewport &vp, int x, int y, int z)
{
	if (ClassicTerrainCamera() && _terrain_magnify == 1) return RemapCoords(x, y, z);
	auto p = ViewportTerrainCamera(vp).Project({float(x), float(y), float(z)});
	int mask = ScaleByZoom(-1, vp.zoom);
	return {(vp.virtual_left & mask) + int(std::lround(p.x * ScaleByZoom(1, vp.zoom))),
		(vp.virtual_top & mask) + int(std::lround(p.y * ScaleByZoom(1, vp.zoom)))};
}

Point PickTerrain3D(const Viewport &vp, int x, int y, bool clamp_to_map)
{
	/* Cab and chase views have no picking: nothing under the cursor, so no highlight or tool target. */
	if (vp.camera_3d != ViewportCamera3D::Normal) return {-1, -1};
	/* Construction uses the authoritative ground inverse, exactly as classic does.
	 * Roof/deck geometry must not move the tool onto an adjacent tile. */
	if (_terrain_yaw == 0 && _terrain_tilt == 100 && _terrain_magnify == 1) return InverseRemapCoords2(
		ScaleByZoom(x - vp.left, vp.zoom) + vp.virtual_left,
		ScaleByZoom(y - vp.top, vp.zoom) + vp.virtual_top, clamp_to_map);
	RefreshTerrain();
	auto camera=ViewportTerrainCamera(vp);
	PrepareTerrainMeshes(&camera);
	float closest=-1e30f;
	Point result{-1,-1};
	for (const auto &chunk : _terrain_chunks) {
		if (!Terrain3D::HitBox(camera, chunk.low, chunk.high, float(x - vp.left), float(y - vp.top))) continue;
		for (const auto &face : chunk.faces) {
			if (!face.ground) continue;
			std::array<Vec,3> p{camera.Project(face.p[0]),camera.Project(face.p[1]),camera.Project(face.p[2])};
			std::array<float,3> weights;
			if (!Terrain3D::Hit(p,float(x-vp.left),float(y-vp.top),weights)) continue;
			float depth=0,wx=0,wy=0;
			for (int i=0;i<3;++i) { depth+=weights[i]*p[i].z; wx+=weights[i]*face.p[i].x; wy+=weights[i]*face.p[i].y; }
			if (depth<=closest) continue;
			closest=depth;
			/* Clamp face boundary hits to their owner, never to a neighbouring void tile. */
			result={std::clamp(int(wx),int(TileX(face.tile)*TILE_SIZE),int(TileX(face.tile)*TILE_SIZE+15)),std::clamp(int(wy),int(TileY(face.tile)*TILE_SIZE),int(TileY(face.tile)*TILE_SIZE+15))};
		}
	}
	if (result.x < 0 && clamp_to_map) {
		/* Scroll/zoom requests outside the island still get a valid edge location.
		 * Ordinary clicks deliberately keep the miss, rather than building at an edge. */
		auto delta = Terrain3D::ClassicScrollDelta((x - vp.left - vp.width / 2.0f) / camera.scale,
			(y - vp.top - vp.height / 2.0f - camera.pz * camera.scale) / camera.scale,
			camera.cosine, camera.sine, camera.tilt);
		float wx = camera.cx + (delta[1] - delta[0] / 2) / 2;
		float wy = camera.cy + (delta[1] + delta[0] / 2) / 2;
		int minimum = _settings_game.construction.freeform_edges ? TILE_SIZE : 0;
		result = {std::clamp(int(wx), minimum, int(Map::MaxX() * TILE_SIZE) - 1),
			std::clamp(int(wy), minimum, int(Map::MaxY() * TILE_SIZE) - 1)};
	}
	return result;
}

Vehicle *PickVehicle3D(const Viewport &vp, int x, int y)
{
	if (vp.camera_3d != ViewportCamera3D::Normal || x < vp.left || y < vp.top || x >= vp.left + vp.width || y >= vp.top + vp.height) return nullptr;
	RefreshTrains3D();
	auto camera = ViewportTerrainCamera(vp);
	float closest = -1e30f;
	Vehicle *result = nullptr;
	auto hit = [&](const std::array<Vec, 3> &world, VehicleID id) {
		Vehicle *v = Vehicle::GetIfValid(id);
		if (v == nullptr || v->vehstatus.Any({VehState::Hidden, VehState::Unclickable})) return;
		std::array<Vec, 3> p{camera.Project(world[0]), camera.Project(world[1]), camera.Project(world[2])};
		std::array<float, 3> w;
		if (!Terrain3D::Hit(p, float(x - vp.left), float(y - vp.top), w)) return;
		float depth = w[0] * p[0].z + w[1] * p[1].z + w[2] * p[2].z;
		if (depth > closest) { closest = depth; result = v; }
	};
	/* Cache library bounds once. Only models whose projected box contains the
	 * pointer need triangle tests; use exactly the GPU's placement and scale. */
	static const auto bounds = [] {
		std::vector<std::pair<Vec, Vec>> out;
		for (size_t i = 0; i < FullVehicleModelCount3D(); ++i) {
			Vec low{1e30f, 1e30f, 1e30f}, high{-1e30f, -1e30f, -1e30f};
			const auto &model = VehicleModel3D(i);
			for (size_t f = 0; f < model.count; ++f) for (const auto &p : model.faces[f].p) {
				low = {std::min(low.x, p[0]), std::min(low.y, p[1]), std::min(low.z, p[2])};
				high = {std::max(high.x, p[0]), std::max(high.y, p[1]), std::max(high.z, p[2])};
			}
			out.emplace_back(low, high);
		}
		return out;
	}();
	for (const auto &v : _vehicle3d_instances) {
		auto transform = [&](Vec p) { return Vec{v.origin.x + p.x * v.length * v.dx - p.y * v.dy,
			v.origin.y + p.x * v.length * v.dy + p.y * v.dx, v.origin.z + p.z}; };
		auto [low, high] = bounds[v.model];
		Vec a{1e30f, 1e30f, low.z + v.origin.z}, b{-1e30f, -1e30f, high.z + v.origin.z};
		for (int i = 0; i < 4; ++i) {
			auto p = transform({(i & 1) ? high.x : low.x, (i & 2) ? high.y : low.y, 0});
			a.x = std::min(a.x, p.x); a.y = std::min(a.y, p.y);
			b.x = std::max(b.x, p.x); b.y = std::max(b.y, p.y);
		}
		if (!Terrain3D::HitBox(camera, a, b, float(x - vp.left), float(y - vp.top))) continue;
		const auto &model = VehicleModel3D(v.model);
		for (size_t i = 0; i < model.count; ++i) {
			const auto &p = model.faces[i].p;
			hit({transform({p[0][0], p[0][1], p[0][2]}), transform({p[1][0], p[1][1], p[1][2]}), transform({p[2][0], p[2][1], p[2][2]})}, v.vehicle);
		}
	}
	for (size_t i = 0; i < _train3d_faces.size(); ++i) hit(_train3d_faces[i].p, _train3d_face_vehicles[i]);
	return result;
}

/** Independent world anchors exercise the actual pointer inverse, including real foundations and slopes. */
void CheckTerrain3DManagement(std::string_view tag)
{
	RefreshTerrain();
	for (const Window *w : Window::Iterate()) {
		if (w->viewport == nullptr || w->IsShaded()) continue;
		const auto &vp = *w->viewport;
		auto camera = ViewportTerrainCamera(vp);
		int checks = 0, failures = 0, occluded = 0, slopes = 0, foundations = 0, bridges = 0;
		auto start = std::chrono::steady_clock::now();
		if (vp.camera_3d != ViewportCamera3D::Normal) {
			for (int y = 1; y <= 3; ++y) for (int x = 1; x <= 3; ++x) {
				int sx = vp.left + vp.width * x / 4, sy = vp.top + vp.height * y / 4;
				++checks;
				failures += PickTerrain3D(vp, sx, sy, true).x != -1 || PickVehicle3D(vp, sx, sy) != nullptr;
			}
		} else {
			/* Bounded diagnostic workload, even on the 512x512 performance fixture. */
			for (int ty = std::max(0, int(camera.cy / TILE_SIZE) - 12); ty < std::min(int(Map::MaxY()), int(camera.cy / TILE_SIZE) + 13); ++ty) {
				for (int tx = std::max(0, int(camera.cx / TILE_SIZE) - 12); tx < std::min(int(Map::MaxX()), int(camera.cx / TILE_SIZE) + 13); ++tx) {
					TileIndex tile = TileXY(tx, ty);
					if (!IsValidTile(tile)) continue;
					int wx = tx * TILE_SIZE + 7, wy = ty * TILE_SIZE + 9;
					auto [slope, z] = GetFoundationPixelSlope(tile);
					Vec world{float(wx), float(wy), float(z + GetPartialPixelZ(7, 9, slope))};
					auto p = camera.Project(world);
					int sx = int(std::lround(p.x)), sy = int(std::lround(p.y));
					if (sx < 3 || sy < 3 || sx >= vp.width - 3 || sy >= vp.height - 3) continue;
					++checks;
					slopes += slope != SLOPE_FLAT;
					foundations += GetTilePixelSlope(tile) != GetFoundationPixelSlope(tile);
					bridges += IsBridgeAbove(tile) || IsTileType(tile, TileType::TunnelBridge);
					Point hit = PickTerrain3D(vp, vp.left + sx, vp.top + sy, false);
					bool ok = hit.x >= 0 && TileVirtXY(hit.x, hit.y) == tile;
					if (!ok && hit.x >= 0) {
						TileIndex other = TileVirtXY(hit.x, hit.y);
						auto [hs, hz] = GetFoundationPixelSlope(other);
						auto hp = camera.Project({float(hit.x), float(hit.y), float(hz + GetPartialPixelZ(hit.x & TILE_UNIT_MASK, hit.y & TILE_UNIT_MASK, hs))});
						/* A nearer hill can cover the anchor; it must still lie on the same screen ray. */
						ok = hp.z > p.z + 1 && std::abs(hp.x - sx) <= 4 * camera.scale + 1 && std::abs(hp.y - sy) <= 4 * camera.scale + 1;
						occluded += ok;
					}
					if (!ok) {
						++failures;
						if (failures < 8) Debug(driver, 0, "PHASE95E PICKFAIL tag={} expected={},{} hit={},{} screen={},{}", tag, tx, ty, hit.x, hit.y, sx, sy);
					}
				}
			}
			/* Clamping is for camera movement only; an ordinary void click has no target. */
			Point edge = PickTerrain3D(vp, vp.left - 100000, vp.top - 100000, true);
			++checks;
			failures += edge.x < 0 || !IsValidTile(TileVirtXY(edge.x, edge.y));
		}
		auto micros = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
		Debug(driver, 0, "PHASE95E GROUND tag={} view={}:{} camera={} yaw={} tilt={} checks={} failures={} occluded={} slopes={} foundations={} bridges={} us={} tool={}",
			tag, int(w->window_class), static_cast<int32_t>(w->window_number), int(vp.camera_3d), _terrain_yaw, _terrain_tilt, checks, failures, occluded, slopes, foundations, bridges, micros, int(_thd.place_mode));
	}
}

void HighlightTerrain3D(int x, int y)
{
	_terrain_highlight=(x>=0 && y>=0 && x<int(Map::MaxX()) && y<int(Map::MaxY())) ? int(TileXY(x,y).base()) : -1;
	InvalidateProof3DImages();
}

/** Read-only integration diagnostics, including independent engine height queries. */
std::string CheckTerrain3D(const Viewport &vp)
{
	RefreshTerrain();
	int height_checks=0, height_failures=0, pick_checks=0, pick_failures=0, edge_hits=0;
	int slope_picks=0, water_picks=0, foundation_picks=0;
	/* Exercise every legal slope shape and every half-platform corner without changing the map. */
	for (Slope slope : {SLOPE_FLAT,SLOPE_N,SLOPE_W,SLOPE_S,SLOPE_E,SLOPE_NW,SLOPE_NE,SLOPE_SW,SLOPE_SE,SLOPE_NS,SLOPE_EW,SLOPE_ENW,SLOPE_SEN,SLOPE_WSE,SLOPE_NWS,SLOPE_ELEVATED,SLOPE_STEEP_N,SLOPE_STEEP_E,SLOPE_STEEP_S,SLOPE_STEEP_W}) {
		for (int half=-1;half<4;++half) {
			Slope surface=half<0?slope:HalftileSlope(slope,static_cast<Corner>(half));
			std::vector<TerrainFace> faces;
			BuildTerrainTile(faces,{slope,surface,0,0,TileType::Clear},2,2);
			for (int y=1;y<16;y+=2) for (int x=1;x<16;x+=2) {
				/* Half-platform exact diagonal boundaries belong to different halves by engine convention. */
				if (half>=0 && (x==y || x+y==16)) continue;
				float height=-100;
				for (const auto &face:faces) {
					std::array<float,3> w;
					if (!Terrain3D::Hit(face.p,float(32+x),float(32+y),w)) continue;
					float z=0; for(int i=0;i<3;++i) z+=w[i]*face.p[i].z;
					height=std::max(height,z);
				}
				++height_checks;
				if (std::abs(height-float(GetPartialPixelZ(x,y,surface)))>0.51f) ++height_failures;
			}
		}
	}
	/* Compare classic camera picks with the existing height-aware inverse on visible interior points. */
	float yaw=_terrain_yaw, tilt=_terrain_tilt;
	_terrain_yaw=0; _terrain_tilt=100;
	for (int y=80;y<vp.height-30;y+=47) for (int x=40;x<vp.width-30;x+=43) {
		Point actual=TranslateXYToTileCoord(&vp,vp.left+x,vp.top+y);
		Point expected=InverseRemapCoords2(ScaleByZoom(x,vp.zoom)+vp.virtual_left,ScaleByZoom(y,vp.zoom)+vp.virtual_top,false);
		if (expected.x<0 || expected.y<0 || actual.x<0 || actual.y<0) continue;
		/* The 3D map has visible side walls. The old view's inverse returns void/outside behind them. */
		if (expected.x>=int(Map::MaxX()*TILE_SIZE) || expected.y>=int(Map::MaxY()*TILE_SIZE) || !IsValidTile(TileVirtXY(expected.x,expected.y))) {
			++edge_hits;
			if (!IsValidTile(TileVirtXY(actual.x,actual.y))) ++pick_failures;
			continue;
		}
		if ((expected.x%16)<2 || (expected.x%16)>13 || (expected.y%16)<2 || (expected.y%16)>13) continue;
		++pick_checks;
		TileIndex expected_tile=TileVirtXY(expected.x,expected.y);
		if (GetTileSlope(expected_tile)!=SLOPE_FLAT) ++slope_picks;
		if (IsTileType(expected_tile,TileType::Water)) ++water_picks;
		if (GetTilePixelSlope(expected_tile)!=GetFoundationPixelSlope(expected_tile)) ++foundation_picks;
		if (TileVirtXY(actual.x,actual.y)!=TileVirtXY(expected.x,expected.y)) {
			++pick_failures;
			Debug(driver,0,"3D PICK mismatch screen {},{} actual {},{} expected {},{} type {} height {} vs {}",x,y,actual.x,actual.y,expected.x,expected.y,int(GetTileType(TileVirtXY(expected.x,expected.y))),GetSlopePixelZ(actual.x,actual.y),GetSlopePixelZ(expected.x,expected.y));
		}
	}
	_terrain_yaw=yaw; _terrain_tilt=tilt;
	std::string timings;
	for (int size:{16,32}) {
		SetTerrain3DChunkSize(size);
		auto start=std::chrono::steady_clock::now(); RefreshTerrain();
		auto build=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-start).count();
		start=std::chrono::steady_clock::now(); for(int i=0;i<10;++i) RefreshTerrain();
		auto scan=std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now()-start).count()/10;
		timings+=fmt::format(" {}x{} build={}us unchanged-scan={}us;",size,size,build,scan);
	}
	SetTerrain3DChunkSize(16);
	auto result=fmt::format("3D CHECK: heights={} failures={}; interior classic picks={} failures={} (slopes={} water={} foundations={}); valid map-edge hits={};{}",height_checks,height_failures,pick_checks,pick_failures,slope_picks,water_picks,foundation_picks,edge_hits,timings);
	Debug(driver,0,"{}",result);
	return result;
}

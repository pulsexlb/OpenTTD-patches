/* This file is part of OpenTTD and is licensed under the GNU General Public License version 2. */
/** @file asset3d_impl.h Read-only base-content asset projection; included in the terrain namespace. */

using CompanyColours3D = std::array<std::array<float, 3>, 2>;

/** Primary and secondary company colours; neutral grey for unowned objects. */
static CompanyColours3D CompanyColours3DOf(Owner owner)
{
	const Company *c = Company::GetIfValid(owner);
	if (c == nullptr) return {{{.55f, .55f, .55f}, {.75f, .75f, .75f}}};
	const Livery &livery = c->livery[LiveryScheme::Default];
	CompanyColours3D out;
	for (int i = 0; i < 2; ++i) {
		const auto &rgb = _cur_palette.palette[GetColourGradient(i == 0 ? livery.colour1 : livery.colour2, Shade::Normal).p];
		out[i] = {rgb.r / 255.0f, rgb.g / 255.0f, rgb.b / 255.0f};
	}
	return out;
}

/** Shared full-detail library identity, without ordering or subtracting unrelated array pointers. */
static size_t StaticModelIndex3D(const Asset3D::Model *model)
{
	static const auto indices = [] {
		std::map<const Asset3D::Model *, size_t> out;
		for (size_t i = 0; i < std::size(Art3D::models); ++i) out.emplace(&Art3D::models[i], i);
		for (size_t i = 0; i < std::size(Asset3D::models); ++i) out.emplace(&Asset3D::models[i], std::size(Art3D::models) + i);
		return out;
	}();
	return indices.at(model);
}

/** Place a model: local X/Y are rotated by (dx, dy), X is scaled by \p length and Z by \p height.
 * Faces with a negative colour take company colour 1 or 2. */
static void AppendModel3D(std::vector<TerrainFace> &faces, const Asset3D::Model &model, TileIndex tile, Vec origin, float dx, float dy, float length, float height, const CompanyColours3D &company)
{
	if (_terrain_capture_instances != nullptr) {
		Static3DInstance instance{&model, StaticModelIndex3D(&model), origin, dx, dy, length, height, company, tile, _terrain_capture_option,
			{{{1e30f, 1e30f, 1e30f}, {-1e30f, -1e30f, -1e30f}}}};
		/* Use the original expansion once for exact bounds, including discarded degenerate faces.
		 * The expanded triangles are scratch data and are never retained by a chunk. */
		auto *capture = _terrain_capture_instances;
		_terrain_capture_instances = nullptr;
		std::vector<TerrainFace> bounds;
		AppendModel3D(bounds, model, tile, origin, dx, dy, length, height, company);
		_terrain_capture_instances = capture;
		for (const auto &face : bounds) for (auto p : face.p) {
			instance.p[0] = {std::min(instance.p[0].x, p.x), std::min(instance.p[0].y, p.y), std::min(instance.p[0].z, p.z)};
			instance.p[1] = {std::max(instance.p[1].x, p.x), std::max(instance.p[1].y, p.y), std::max(instance.p[1].z, p.z)};
		}
		if (!bounds.empty()) capture->push_back(instance);
		return;
	}

	for (size_t i=0;i<model.count;++i) {
		const auto &face=model.faces[i];
		std::array<Vec,3> points;
		for (int j=0;j<3;++j) {
			const auto &p=face.p[j];
			points[j]={origin.x+p[0]*length*dx-p[1]*dy,origin.y+p[0]*length*dy+p[1]*dx,origin.z+p[2]*height};
		}
		std::array<float,3> colour{face.colour[0],face.colour[1],face.colour[2]};
		if (colour[0] < 0) colour = company[colour[0] < -1.5f ? 1 : 0];
		AddTerrainFace(faces,points,colour,tile);
	}
}

/** A one-tile model turned about the tile centre so its local +Y points at \p turn, standing at height \p z. */
static void AppendTurnedModel3D(std::vector<TerrainFace> &faces, const Asset3D::Model &model, TileIndex tile, DiagDirection turn, float z, const CompanyColours3D &company)
{
	/* Local X direction per DiagDirection (NE, SE, SW, NW) of local +Y. */
	static constexpr float dirs[4][2]={{0,1},{1,0},{0,-1},{-1,0}};
	float dx=dirs[to_underlying(turn)][0], dy=dirs[to_underlying(turn)][1];
	AppendModel3D(faces,model,tile,{TileX(tile)*16+8-8*dx+8*dy,TileY(tile)*16+8-8*dy-8*dx,z},dx,dy,1,1,company);
}

/** Height of the rendered tile surface at local pixel (lx, ly), on the same four sectors as BuildTerrainTile. */
static float SurfaceZ3D(const std::array<Vec, 5> &c, float lx, float ly)
{
	float u = lx - 8, v = ly - 8;
	int i = std::abs(v) >= std::abs(u) ? (v < 0 ? 0 : 2) : (u > 0 ? 1 : 3);
	const Vec &a = c[i], &b = c[(i + 1) % 4], &m = c[4];
	float px = c[0].x + lx, py = c[0].y + ly;
	float d = (b.y - m.y) * (a.x - m.x) + (m.x - b.x) * (a.y - m.y);
	float wa = ((b.y - m.y) * (px - m.x) + (m.x - b.x) * (py - m.y)) / d;
	float wb = ((m.y - a.y) * (px - m.x) + (a.x - m.x) * (py - m.y)) / d;
	return wa * a.z + wb * b.z + (1 - wa - wb) * m.z;
}

/** Place a model whose every vertex keeps its height above the sloped tile surface (fences, rocks, flags).
 * Local X/Y are rotated by (dx, dy) and then offset by (ox, oy) tile pixels. */
static void AppendModelOnGround3D(std::vector<TerrainFace> &faces, const Asset3D::Model &model, TileIndex tile, const TerrainTile &t, float ox, float oy, float dx, float dy, const CompanyColours3D &company)
{
	auto corners = TerrainPoints(t.surface, t.surface_z, TileX(tile) * TILE_SIZE, TileY(tile) * TILE_SIZE);
	for (size_t i = 0; i < model.count; ++i) {
		const auto &face = model.faces[i];
		std::array<Vec, 3> points;
		for (int j = 0; j < 3; ++j) {
			const auto &p = face.p[j];
			float lx = std::clamp(ox + p[0] * dx - p[1] * dy, 0.0f, 16.0f), ly = std::clamp(oy + p[0] * dy + p[1] * dx, 0.0f, 16.0f);
			points[j] = {corners[0].x + lx, corners[0].y + ly, SurfaceZ3D(corners, lx, ly) + p[2]};
		}
		std::array<float, 3> colour{face.colour[0], face.colour[1], face.colour[2]};
		if (colour[0] < 0) colour = company[colour[0] < -1.5f ? 1 : 0];
		AddTerrainFace(faces, points, colour, tile);
	}
}

static void AppendAsset3D(std::vector<TerrainFace> &faces, Asset3D::ID id, TileIndex tile, Vec origin, float dx=1, float dy=0, float length=1, float height=1)
{
	AppendModel3D(faces,Asset3D::models[id],tile,origin,dx,dy,length,height,CompanyColours3DOf(INVALID_OWNER));
}

static int VehicleAsset3D(EngineID id)
{
	const Engine *engine=Engine::Get(id);
	if (engine->GetGRF() != nullptr) return -1;
	/* Original engine table identities, not sprite IDs. Other vehicles retain the Phase 4 placeholder. */
	switch (id.base()) {
		case 0: case 9: return Asset3D::LOCOMOTIVE;
		case 29: case 59: case 91: return Asset3D::WAGON;
		case 123: case 124: case 125: return Asset3D::TRUCK;
		default: return -1;
	}
}

/** Phase 8 model for a base engine, or -1. */
static int VehicleArt3D(EngineID id, bool loaded)
{
	if (Engine::Get(id)->GetGRF() != nullptr) return -1;
	return Art3D::Engine(id.base(), loaded);
}

/** Station, stop, depot and lock identities in Art3D::stations; STATION_IDS in tools/3d_assets/convert_gltf.py. */
enum Station3D : int {
	ST3D_PLATFORM, ST3D_BUILDING, ST3D_ROOF_A, ST3D_ROOF_B, ST3D_WAYPOINT, ST3D_TRUCK_BAY, ST3D_TRUCK_DRIVE, ST3D_BUS_BAY,
	ST3D_BUS_DRIVE, ST3D_ROAD_WAYPOINT, ST3D_DOCK, ST3D_BUOY, ST3D_DEPOT_RAIL, ST3D_DEPOT_ROAD, ST3D_DEPOT_SHIP, ST3D_LOCK,
	ST3D_AIRPORT, ///< Audit identity of base airport tile graphic 0; graphic n is ST3D_AIRPORT + n.
};

/** Models with track or road along local X turn their local +Y to the SE for the X axis and to the SW for the Y axis. */
static DiagDirection AxisTurn3D(Axis axis)
{
	return axis == Axis::X ? DiagDirection::SE : DiagDirection::SW;
}

/** Original station, stop, depot and lock pieces: the model, its turn and owner.
 * @return The identity * 4 + part when the tile has an original piece (modelled or not), else -1. */
static int ReadStation3D(TileIndex tile, TerrainTile &t)
{
	int id=-1, part=0;
	DiagDirection turn=DiagDirection::SE;
	if (IsTileType(tile,TileType::Station)) {
		switch (GetStationType(tile)) {
			case StationType::Rail:
				/* Rail station graphics 0-7: platform, building, large-building roofs A and B, each for both axes. */
				if (IsCustomStationSpecIndex(tile) || GetStationGfx(tile)>=8) return -1;
				id=GetStationGfx(tile)/2;
				turn=AxisTurn3D(GetRailStationAxis(tile));
				break;
			case StationType::RailWaypoint:
				if (IsCustomStationSpecIndex(tile)) return -1;
				id=ST3D_WAYPOINT;
				turn=AxisTurn3D(GetRailStationAxis(tile));
				break;
			case StationType::Truck: case StationType::Bus: case StationType::RoadWaypoint:
				if (IsCustomRoadStopSpecIndex(tile)) return -1;
				if (IsDriveThroughStopTile(tile)) {
					id=IsRoadWaypoint(tile) ? ST3D_ROAD_WAYPOINT : IsTruckStop(tile) ? ST3D_TRUCK_DRIVE : ST3D_BUS_DRIVE;
					turn=AxisTurn3D(GetDriveThroughStopAxis(tile));
				} else {
					if (IsRoadWaypoint(tile)) return -1;
					id=IsTruckStop(tile) ? ST3D_TRUCK_BAY : ST3D_BUS_BAY;
					turn=GetBayRoadStopDir(tile);
				}
				break;
			case StationType::Dock:
				id=ST3D_DOCK;
				if (!IsDockWaterPart(tile)) {
					turn=GetDockDirection(tile);
					t.art_on_ground=true;
				} else {
					/* The water part only stores its axis; the land part beside it gives the direction. */
					part=1;
					Axis axis=static_cast<Axis>(GetStationGfx(tile)-GFX_DOCK_BASE_WATER_PART);
					turn=AxisToDiagDir(axis);
					for (DiagDirection d : {AxisToDiagDir(axis), ReverseDiagDir(AxisToDiagDir(axis))}) {
						TileIndex land=TileAddByDiagDir(tile,ReverseDiagDir(d));
						if (IsValidTile(land) && IsDockTile(land) && !IsDockWaterPart(land) && GetDockDirection(land)==d) turn=d;
					}
				}
				break;
			case StationType::Buoy:
				id=ST3D_BUOY;
				break;
			case StationType::Airport: {
				/* Base airport tile graphics, untouched by NewGRF tiles; authored in world axes (no turn). The radar and
				 * wind sock pick their model by animation frame. Plain apron and grass have no model: only ground. */
				StationGfx gfx=GetAirportGfx(tile);
				if (gfx>=NEW_AIRPORTTILE_OFFSET) return -1;
				t.art=Art3D::AirportTile(gfx,GetAnimationFrame(tile));
				t.art_bare=t.art<0 && Art3D::HasAirportTiles();
				t.art_turn=DiagDirection::SE;
				t.owner=GetTileOwner(tile);
				return (ST3D_AIRPORT+gfx)*4;
			}
			default:
				return -1;
		}
	} else if (IsRailDepotTile(tile)) {
		id=ST3D_DEPOT_RAIL;
		turn=GetRailDepotDirection(tile);
	} else if (IsRoadDepotTile(tile)) {
		id=ST3D_DEPOT_ROAD;
		turn=GetRoadDepotDirection(tile);
	} else if (IsShipDepotTile(tile)) {
		/* Authored along local X with the north part first; the Y axis turn runs local X towards the north. */
		id=ST3D_DEPOT_SHIP;
		Axis axis=GetShipDepotAxis(tile);
		part=(GetShipDepotPart(tile)==DepotPart::North) == (axis==Axis::X) ? 0 : 1;
		turn=AxisTurn3D(axis);
	} else if (IsTileType(tile,TileType::Water) && IsLock(tile)) {
		id=ST3D_LOCK;
		part=to_underlying(GetLockPart(tile));
		turn=GetLockDirection(tile);
	} else {
		return -1;
	}
	t.art=Art3D::Station(id,part);
	t.art_turn=turn;
	t.owner=GetTileOwner(tile);
	return id*4+part;
}

static void ReadAsset3D(TileIndex tile, TerrainTile &t)
{
	if (t.type==TileType::Station || t.type==TileType::Water || t.type==TileType::Railway || t.type==TileType::Road) {
		ReadStation3D(tile,t);
		return;
	}
	if (t.type==TileType::House) {
		HouseID house=GetHouseType(tile);
		t.asset=Asset3D::House(house);
		t.asset_stage=GetHouseBuildingStage(tile);
		/* A Phase 8 house is one whole model drawn from its north tile; its other tiles draw only ground. */
		HouseID north=house;
		bool is_north=house>=NEW_HOUSE_OFFSET || GetHouseNorthPart(north)==0;
		if (north<NEW_HOUSE_OFFSET) {
			int art=Art3D::House(north,t.asset_stage);
			if (art>=0) {
				t.art=is_north?art:-1;
				t.asset=-1;
			}
		}
	} else if (t.type==TileType::Industry) {
		IndustryGfx gfx=GetIndustryGfx(tile);
		t.asset=Asset3D::Industry(GetIndustryType(tile),gfx);
		t.asset_stage=GetIndustryConstructionStage(tile);
		if (gfx<NEW_INDUSTRYTILEOFFSET) t.art=Art3D::IndustryTile(gfx,t.asset_stage);
	} else if (t.type==TileType::Trees) {
		/* Base trees share a curated silhouette. Growth and count remain authoritative. */
		int growth=int(GetTreeGrowth(tile));
		t.asset=growth>=4?Asset3D::TREE_DEAD:Asset3D::TREE;
		t.asset_stage=std::min(3,growth);
		t.tree_count=GetTreeCount(tile);
		t.art=Art3D::Tree(GetTreeType(tile),growth);
	} else if (t.type==TileType::Object) {
		ObjectType type=GetObjectType(tile);
		if (type>=NEW_OBJECT_OFFSET) return;
		t.owner=GetTileOwner(tile);
		if (type==OBJECT_HQ) {
			/* The HQ size is kept in the animation frame; the 2x2 model is drawn from the north tile only. */
			if (tile==Object::GetByTile(tile)->location.tile) t.art=Art3D::Object(OBJECT_HQ+GetAnimationFrame(tile));
		} else {
			t.art=Art3D::Object(type);
		}
	} else if (t.type==TileType::Clear && !IsSnowTile(tile)) {
		/* As DrawTile_Clear: rock clusters on rocky land, hedges and fences round fields; none under snow. */
		if (IsClearGround(tile,ClearGround::Rocks)) {
			t.art=Art3D::Rocks();
		} else if (IsClearGround(tile,ClearGround::Fields)) {
			int slot=0;
			for (DiagDirection side : {DiagDirection::NW,DiagDirection::NE,DiagDirection::SE,DiagDirection::SW}) SB(t.fences,3*slot++,3,GetFence(tile,side));
		}
	}
}

/** Rocks and field fences on clear land; like their classic sprites, transparency settings leave them alone. */
static void BuildLandscape3D(std::vector<TerrainFace> &faces, const TerrainTile &t, TileIndex tile)
{
	static const CompanyColours3D unowned=CompanyColours3DOf(INVALID_OWNER);
	if (t.art>=0) {
		/* Turn the cluster about the tile centre by the tile hash so neighbouring rocky tiles differ. */
		static constexpr float turn[4][2]={{1,0},{0,1},{-1,0},{0,-1}};
		const auto &r=turn[TileHash(TileX(tile),TileY(tile))&3];
		AppendModelOnGround3D(faces,Art3D::models[t.art],tile,t,8-8*r[0]+8*r[1],8-8*r[1]-8*r[0],r[0],r[1],unowned);
	}
	/* Fence models run along local X on the y = 0 edge, inside the tile: origin and turn per side (NW, NE, SE, SW). */
	static constexpr float place[4][4]={{0,0,1,0},{0,16,0,-1},{16,16,-1,0},{16,0,0,1}};
	for (int slot=0;slot<4;++slot) {
		uint type=GB(t.fences,3*slot,3);
		int art=type==0 ? -1 : Art3D::Fence(type);
		if (art>=0) AppendModelOnGround3D(faces,Art3D::models[art],tile,t,place[slot][0],place[slot][1],place[slot][2],place[slot][3],unowned);
	}
}

/** A station, stop, depot or lock piece, turned so local +Y points at \c art_turn about the tile centre.
 * Water pieces sit at the water level, the dock land part on its slope, the rest on their foundation. */
static void BuildStation3D(std::vector<TerrainFace> &faces, const TerrainTile &t, TileIndex tile)
{
	/* Local X direction per DiagDirection (NE, SE, SW, NW) of local +Y. */
	static constexpr float turn[4][2]={{0,1},{1,0},{0,-1},{-1,0}};
	float dx=turn[to_underlying(t.art_turn)][0], dy=turn[to_underlying(t.art_turn)][1];
	float ox=8-8*dx+8*dy, oy=8-8*dy-8*dx;
	size_t first=faces.size();
	auto company=CompanyColours3DOf(t.owner);
	const auto &model=Art3D::models[t.art];
	if (t.art_on_ground) {
		AppendModelOnGround3D(faces,model,tile,t,ox,oy,dx,dy,company);
	} else {
		float z=t.type==TileType::Water || t.water_surface ? float(t.surface_z) : float(t.surface_z+GetSlopeMaxPixelZ(t.surface));
		AppendTurnedModel3D(faces,model,tile,t.art_turn,z,company);
	}
	TagStructure3D(faces,first,TransparencyOption::Buildings);
}

static void BuildAsset3D(std::vector<TerrainFace> &faces, const TerrainTile &t, TileIndex tile)
{
	if (t.type==TileType::Clear) {
		BuildLandscape3D(faces,t,tile);
		return;
	}
	if (t.type==TileType::Station || t.type==TileType::Water || t.type==TileType::Railway || t.type==TileType::Road) {
		if (t.art>=0) BuildStation3D(faces,t,tile);
		return;
	}
	if (t.asset<0 && t.art<0) return;
	size_t first = faces.size();
	float x=TileX(tile)*16,y=TileY(tile)*16;
	static const CompanyColours3D unowned=CompanyColours3DOf(INVALID_OWNER);
	if (t.tree_count>0) {
		constexpr int positions[4][2]={{4,4},{11,10},{4,12},{12,4}};
		for (int i=0;i<t.tree_count;++i) {
			int px=positions[i][0],py=positions[i][1];
			float z=t.ground_z+GetPartialPixelZ(px,py,t.ground);
			/* Phase 8 trees model each growth state at its own size. */
			if (t.art>=0) AppendModel3D(faces,Art3D::models[t.art],tile,{x+px,y+py,z},1,0,1,1,unowned);
			else AppendAsset3D(faces,static_cast<Asset3D::ID>(t.asset),tile,{x+px,y+py,z},1,0,1,.4f+.2f*t.asset_stage);
		}
	} else if (t.type==TileType::Object) {
		/* Owned land has no foundation, so its flag follows the slope; the other objects stand on their foundation. */
		auto company=CompanyColours3DOf(t.owner);
		if (t.art==Art3D::Object(OBJECT_OWNED_LAND)) AppendModelOnGround3D(faces,Art3D::models[t.art],tile,t,0,0,1,0,company);
		else AppendModel3D(faces,Art3D::models[t.art],tile,{x,y,float(t.surface_z+GetSlopeMaxPixelZ(t.surface))},1,0,1,1,company);
	} else if (t.art>=0) {
		AppendModel3D(faces,Art3D::models[t.art],tile,{x,y,float(t.surface_z+GetSlopeMaxPixelZ(t.surface))},1,0,1,1,unowned);
	} else {
		auto id=t.asset_stage<3?Asset3D::CONSTRUCTION:static_cast<Asset3D::ID>(t.asset);
		AppendAsset3D(faces,id,tile,{x,y,float(t.surface_z+GetSlopeMaxPixelZ(t.surface))});
	}
	TagStructure3D(faces, first, t.type == TileType::Trees ? TransparencyOption::Trees : t.type == TileType::House ? TransparencyOption::Houses : t.type == TileType::Object ? TransparencyOption::Structures : TransparencyOption::Industries);
}

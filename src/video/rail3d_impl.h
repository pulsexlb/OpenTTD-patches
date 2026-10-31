/* This file is part of OpenTTD and is licensed under the GNU General Public License version 2. */
/** @file rail3d_impl.h Read-only rail/road infrastructure and vehicle presentation; included inside the terrain namespace. */

#include "asset3d_impl.h"

/** Bridge piece of a middle tile, in the same order as CalcBridgePiece (tunnelbridge_cmd.cpp). */
static BridgePieces BridgePiece3D(uint north, uint south)
{
	if (north == 1) return BRIDGE_PIECE_NORTH;
	if (south == 1) return BRIDGE_PIECE_SOUTH;
	if (north < south) return north & 1 ? BRIDGE_PIECE_INNER_SOUTH : BRIDGE_PIECE_INNER_NORTH;
	if (north > south) return south & 1 ? BRIDGE_PIECE_INNER_NORTH : BRIDGE_PIECE_INNER_SOUTH;
	return north & 1 ? BRIDGE_PIECE_MIDDLE_EVEN : BRIDGE_PIECE_MIDDLE_ODD;
}

/** Bridge type with Phase 8 models for this piece, or -1 for aqueducts and NewGRF sprite tables. */
static int BridgeType3D(TileIndex end, BridgePieces piece)
{
	if (GetTunnelBridgeTransportType(end) == TransportType::Water) return -1;
	BridgeType type = GetBridgeType(end);
	const BridgeSpec *spec = GetBridgeSpec(type);
	if (piece < spec->sprite_table.size() && !spec->sprite_table[piece].empty()) return -1;
	return type;
}

/** Local X of bridge and tunnel models runs along \p dir; this is the turn (local +Y) that gives it. */
static DiagDirection TravelTurn3D(int dir)
{
	return static_cast<DiagDirection>((dir + 3) % to_underlying(DiagDirection::End));
}

/** Catenary model for a piece: 0 pylon, 1 pylon at a diagonal position (2.83 from its control point), 2 wire held at both
 * ends, 3 wire held at one end (the middle of a two-tile span). */
static int CatenaryPiece3D(const RailCatenaryPiece &c)
{
	if (c.pylon) return std::hypot(float(c.bx - c.ax), float(c.by - c.ay)) > 3.5f ? 0 : 1;
	return c.pylons == 3 ? 2 : 3;
}

/** Append a model authored along local X from 0 to 16 so that it runs from \p a to \p b: stretched along the span and
 * sheared to the end heights, with local Y level and square to the span. */
static void AppendSpanModel3D(std::vector<TerrainFace> &faces, const Asset3D::Model &model, TileIndex tile, Vec a, Vec b)
{
	float dx = b.x - a.x, dy = b.y - a.y, length = std::hypot(dx, dy);
	float nx = -dy / length, ny = dx / length;
	for (size_t i = 0; i < model.count; ++i) {
		const auto &face = model.faces[i];
		std::array<Vec, 3> points;
		for (int j = 0; j < 3; ++j) {
			const auto &p = face.p[j];
			float u = p[0] / TILE_SIZE;
			points[j] = {a.x + dx * u + nx * p[1], a.y + dy * u + ny * p[1], a.z + (b.z - a.z) * u + p[2]};
		}
		AddTerrainFace(faces, points, {face.colour[0], face.colour[1], face.colour[2]}, tile);
	}
}

static void ReadRail3D(TileIndex tile, TerrainTile &snapshot)
{
	CollectRailCatenary(tile, snapshot.catenary);
	if (IsTileType(tile, TileType::Station)) {
		snapshot.facility = static_cast<int>(GetStationType(tile));
		snapshot.hangar = IsHangarTile(tile);
		snapshot.water_surface = IsBuoy(tile) || IsOilRig(tile) || (IsDock(tile) && IsDockWaterPart(tile));
	}
	if (IsShipDepotTile(tile)) snapshot.water_depot = to_underlying(GetShipDepotAxis(tile));
	if (IsTileType(tile, TileType::Water) && IsLock(tile)) snapshot.lock = to_underlying(GetLockDirection(tile));
	if (IsNormalRoadTile(tile)) snapshot.roads = GetAllRoadBits(tile);
	if (IsLevelCrossingTile(tile)) {
		snapshot.roads = GetCrossingRoadBits(tile);
		snapshot.tracks = GetCrossingRailBits(tile);
		snapshot.crossing = true;
		snapshot.barred = IsCrossingBarred(tile);
	}
	if (IsRoadDepotTile(tile)) {
		snapshot.road_depot = to_underlying(GetRoadDepotDirection(tile));
		snapshot.roads = GetAnyRoadBits(tile, RoadTramType::Road);
	}
	if (IsAnyRoadStopTile(tile)) {
		snapshot.road_stop = true;
		snapshot.roads = GetAnyRoadBits(tile, RoadTramType::Road) | GetAnyRoadBits(tile, RoadTramType::Tram);
	}
	if (IsTileType(tile, TileType::TunnelBridge)) {
		auto dir = GetTunnelBridgeDirection(tile);
		if (GetTunnelBridgeTransportType(tile) == TransportType::Rail) snapshot.tracks = DiagDirToDiagTrackBits(dir);
		if (GetTunnelBridgeTransportType(tile) == TransportType::Road) snapshot.roads = DiagDirToAxis(dir) == Axis::X ? ROAD_X : ROAD_Y;
		if (IsTunnel(tile)) {
			snapshot.portal = to_underlying(dir);
			if (GetTunnelBridgeTransportType(tile) != TransportType::Water) snapshot.portal_kind = GetTunnelBridgeTransportType(tile) == TransportType::Road ? 1 : 0;
		} else {
			snapshot.ramp = to_underlying(dir);
			snapshot.ramp_type = BridgeType3D(tile, BRIDGE_PIECE_HEAD);
		}
	}
	if (IsBridgeAbove(tile)) {
		auto end = GetNorthernBridgeEnd(tile);
		auto south = GetSouthernBridgeEnd(tile);
		snapshot.deck_z = GetBridgePixelHeight(end);
		snapshot.deck_axis = to_underlying(GetBridgeAxis(tile));
		snapshot.deck_road = GetTunnelBridgeTransportType(end) == TransportType::Road;
		snapshot.deck_rail = GetTunnelBridgeTransportType(end) == TransportType::Rail;
		BridgePieces piece = BridgePiece3D(GetTunnelBridgeLength(tile, end) + 1, GetTunnelBridgeLength(tile, south) + 1);
		snapshot.deck_piece = to_underlying(piece);
		snapshot.deck_type = BridgeType3D(south, piece);
	}
	if (IsPlainRailTile(tile)) {
		snapshot.tracks = GetTrackBits(tile);
		for (Track track = TRACK_BEGIN; track < TRACK_END; track++) {
			if (!HasBit(snapshot.tracks, track)) continue;
			for (int reverse : {0, 8}) {
				auto td = static_cast<Trackdir>(track + reverse);
				if (!HasSignalOnTrackdir(tile, td)) continue;
				snapshot.signals |= 1U << td;
				if (GetSignalStateByTrackdir(tile, td) == SignalState::Green) snapshot.green_signals |= 1U << td;
				snapshot.signal_kinds |= (to_underlying(GetSignalType(tile, track)) | to_underlying(GetSignalVariant(tile, track)) << 3) << (4 * track);
			}
		}
	} else if (IsRailDepotTile(tile)) {
		snapshot.depot = to_underlying(GetRailDepotDirection(tile));
		snapshot.tracks = TrackToTrackBits(GetRailDepotTrack(tile));
	} else if (IsRailStationTile(tile) || IsRailWaypointTile(tile)) {
		snapshot.station = true;
		if (!IsStationTileBlocked(tile)) snapshot.tracks = GetRailStationTrackBits(tile);
	}
}

static void RailQuad(std::vector<TerrainFace> &faces, Vec a, Vec b, Vec c, Vec d, std::array<float,3> colour, TileIndex tile)
{
	AddTerrainFace(faces, {a,b,c}, colour, tile);
	AddTerrainFace(faces, {a,c,d}, colour, tile);
}

static void RailBox(std::vector<TerrainFace> &faces, Vec low, Vec high, std::array<float,3> colour, TileIndex tile)
{
	Vec a{low.x,low.y,low.z}, b{high.x,low.y,low.z}, c{high.x,high.y,low.z}, d{low.x,high.y,low.z};
	Vec e{low.x,low.y,high.z}, f{high.x,low.y,high.z}, g{high.x,high.y,high.z}, h{low.x,high.y,high.z};
	RailQuad(faces,a,b,f,e,colour,tile); RailQuad(faces,b,c,g,f,colour,tile);
	RailQuad(faces,c,d,h,g,colour,tile); RailQuad(faces,d,a,e,h,colour,tile);
	RailQuad(faces,e,f,g,h,colour,tile);
}

/** Straight chords follow the six engine track pieces, including every junction combination.
 * Sample heights across each strip so inclined/half-tile foundations retain their shape. */
static void BuildRail3D(std::vector<TerrainFace> &faces, const TerrainTile &t, TileIndex tile)
{
	if (t.tracks==TRACK_BIT_NONE && t.roads==ROAD_NONE && !t.station && t.depot<0 && t.road_depot<0 && t.deck_z<0 && t.type!=TileType::Industry && t.facility<0 && t.water_depot<0 && t.lock<0) return;
	const float x = TileX(tile)*TILE_SIZE, y = TileY(tile)*TILE_SIZE;
	std::vector<TerrainFace> surface;
	BuildTerrainTile(surface,t,TileX(tile),TileY(tile));
	auto point = [&](float px, float py, float lift) -> Vec {
		float height = float(t.surface_z);
		for (const auto &face : surface) {
			std::array<float,3> weights;
			if (!Terrain3D::Hit(face.p,x+px,y+py,weights)) continue;
			float z=0;
			for (int i=0;i<3;++i) z+=weights[i]*face.p[i].z;
			height=std::max(height,z);
		}
		if (t.portal >= 0) height = float(t.ground_z);
		if (t.ramp >= 0) {
			/* Same foundation/ramp rule as GetSlopePixelZ_TunnelBridge. Continuous
			 * edge coordinates join the next tile without the pixel-query wrap. */
			height = Rail3D::RampHeight(t.ramp,t.surface==SLOPE_FLAT,t.surface_z,px,py);
		}
		return {x+px,y+py,height+lift};
	};
	if (t.facility == static_cast<int>(StationType::Airport) && t.art < 0 && !t.art_bare) {
		RailQuad(faces,point(0,0,.3f),point(16,0,.3f),point(16,16,.3f),point(0,16,.3f),{.3f,.32f,.34f},tile);
		RailQuad(faces,point(7.5f,3,.4f),point(8.5f,3,.4f),point(8.5f,13,.4f),point(7.5f,13,.4f),{.86f,.84f,.7f},tile);
		if (t.hangar) {
			size_t first = faces.size();
			RailBox(faces,point(1,1,.5f),point(15,15,9),{.48f,.55f,.6f},tile);
			TagStructure3D(faces,first,TransparencyOption::Buildings);
		}
	}
	/* Placeholders for station, depot and lock pieces that have no Phase 8 model (see BuildStation3D). */
	if ((t.facility == static_cast<int>(StationType::Dock) && t.art < 0) || t.facility == static_cast<int>(StationType::Oilrig)) {
		size_t first = faces.size();
		RailBox(faces,point(2,2,.5f),point(14,14,2),{.58f,.5f,.35f},tile);
		for (int a : {3,12}) for (int b : {3,12}) RailBox(faces,point(a,b,0),point(a+1,b+1,4),{.4f,.37f,.3f},tile);
		TagStructure3D(faces,first,TransparencyOption::Buildings);
	}
	if (t.facility == static_cast<int>(StationType::Buoy) && t.art < 0) {
		RailBox(faces,point(5,5,.5f),point(11,11,2),{.85f,.2f,.12f},tile);
		RailBox(faces,point(7,7,2),point(9,9,7),{.9f,.75f,.15f},tile);
	}
	if ((t.water_depot >= 0 || t.lock >= 0) && t.art < 0) {
		size_t first = faces.size();
		bool ax = t.water_depot >= 0 ? to_underlying(Axis::X) == t.water_depot : DiagDirToAxis(static_cast<DiagDirection>(t.lock)) == Axis::X;
		for (int side : {1,14}) RailBox(faces,point(ax?0:side,ax?side:0,0),point(ax?16:side+1,ax?side+1:16,5),{.48f,.5f,.52f},tile);
		if (t.water_depot >= 0) RailBox(faces,point(0,0,6),point(16,16,7),{.3f,.4f,.5f},tile);
		TagStructure3D(faces,first,TransparencyOption::Buildings);
	}
	static const CompanyColours3D unowned = CompanyColours3DOf(INVALID_OWNER);
	if (t.deck_z >= 0) {
		size_t first = faces.size();
		bool ax = t.deck_axis == to_underlying(Axis::X);
		int art = t.deck_type >= 0 ? Art3D::BridgePiece(t.deck_type, t.deck_piece) : -1;
		if (art >= 0) {
			/* Phase 8 piece, authored along local X from the bridge's north end with its deck top at z = 0. */
			DiagDirection turn = TravelTurn3D(to_underlying(ax ? DiagDirection::SW : DiagDirection::SE));
			AppendTurnedModel3D(faces,Art3D::models[art],tile,turn,float(t.deck_z),unowned);
			/* 8-high pillar segments stacked from under the deck to the ground; none where they would
			 * stand in a route crossing below or on a station, ramp or portal. */
			int pillar = Art3D::BridgePillar(t.deck_type, t.deck_piece);
			bool clear = (t.tracks & ~(ax ? TRACK_BIT_X : TRACK_BIT_Y)) == TRACK_BIT_NONE && (t.roads & ~(ax ? ROAD_X : ROAD_Y)) == ROAD_NONE &&
				t.facility < 0 && !t.station && t.depot < 0 && t.road_depot < 0 && t.portal < 0 && t.ramp < 0 && t.lock < 0 && t.water_depot < 0;
			if (pillar >= 0 && clear) {
				for (float z = float(t.deck_z) - 1.5f; z > float(t.ground_z); z -= TILE_HEIGHT) AppendTurnedModel3D(faces,Art3D::models[pillar],tile,turn,z,unowned);
			}
		} else {
			RailBox(faces,{x+(ax?0:2),y+(ax?2:0),float(t.deck_z)-2},{x+(ax?16:14),y+(ax?14:16),float(t.deck_z)-0.1f},{0.52f,0.54f,0.57f},tile);
			/* Side piers leave the central underpass unobstructed. */
			for (int side : {2,12}) {
				if (t.roads==ROAD_NONE && t.tracks==TRACK_BIT_NONE && t.portal<0 && t.ramp<0) RailBox(faces,{x+(ax?7:side),y+(ax?side:7),float(t.ground_z)},{x+(ax?9:side+2),y+(ax?side+2:9),float(t.deck_z)-2},{0.47f,0.48f,0.49f},tile);
				RailBox(faces,{x+(ax?0:side),y+(ax?side:0),float(t.deck_z)},{x+(ax?16:side+0.5f),y+(ax?side+0.5f:16),float(t.deck_z)+2},{0.65f,0.67f,0.69f},tile);
			}
		}
		TerrainTile deck{SLOPE_FLAT,SLOPE_FLAT,t.deck_z,t.deck_z,TileType::Clear};
		if (t.deck_road) deck.roads = ax ? ROAD_X : ROAD_Y;
		else if (t.deck_rail) deck.tracks = ax ? TRACK_BIT_X : TRACK_BIT_Y;
		BuildRail3D(faces,deck,tile);
		TagStructure3D(faces, first, TransparencyOption::Bridges);
	}
	int ramp_art = t.ramp_type >= 0 ? Art3D::BridgeRamp(t.ramp_type, t.surface == SLOPE_FLAT) : -1;
	if (ramp_art >= 0) {
		/* Authored as the northern head of an X bridge: rising along local X from the tile's base (RampHeight). */
		AppendTurnedModel3D(faces,Art3D::models[ramp_art],tile,TravelTurn3D(t.ramp),float(t.surface_z),unowned);
	} else if (t.ramp >= 0) {
		bool ax = t.ramp == to_underlying(DiagDirection::NE) || t.ramp == to_underlying(DiagDirection::SW);
		RailQuad(faces,point(ax?0:2,ax?2:0,0),point(ax?16:14,ax?2:0,0),point(ax?16:14,ax?14:16,0),point(ax?0:2,ax?14:16,0),{0.52f,0.54f,0.57f},tile);
		for (int side : {2,14}) {
			Vec a=point(ax?0:side,ax?side:0,0), b=point(ax?16:side,ax?side:16,0);
			RailQuad(faces,a,b,{b.x,b.y,float(t.surface_z)},{a.x,a.y,float(t.surface_z)},{0.52f,0.54f,0.57f},tile);
		}
	}
	if (t.roads.Any() || t.road_depot >= 0) {
		auto roadquad = [&](float lx,float ly,float hx,float hy,float lift,std::array<float,3> colour) {
			RailQuad(faces,point(lx,ly,lift),point(hx,ly,lift),point(hx,hy,lift),point(lx,hy,lift),colour,tile);
		};
		roadquad(3,3,13,13,0.3f,{0.23f,0.25f,0.27f});
		const unsigned roads = t.roads.base();
		for (int dir=0;dir<4;++dir) {
			if (!(roads & (ROAD_NE.base() >> dir))) continue;
			float lx=dir==0?0:dir==2?8:3, hx=dir==0?8:dir==2?16:13;
			float ly=dir==3?0:dir==1?8:3, hy=dir==3?8:dir==1?16:13;
			roadquad(lx,ly,hx,hy,0.3f,{0.23f,0.25f,0.27f});
			if (!t.crossing) roadquad(dir%2?7.8f:lx,dir%2?ly:7.8f,dir%2?8.2f:hx,dir%2?hy:8.2f,0.36f,{0.86f,0.81f,0.59f});
		}
	}
	int crossing_art=t.crossing ? Art3D::Crossing(t.barred) : -1;
	if (crossing_art>=0) {
		/* Authored with the road along X; a Y road turns the model 90 degrees about the tile. */
		static const CompanyColours3D unowned=CompanyColours3DOf(INVALID_OWNER);
		bool ax=t.roads==ROAD_X;
		Vec origin=point(8,8,0);
		AppendModel3D(faces,Art3D::models[crossing_art],tile,{ax?x:x+16,y,origin.z},ax?1:0,ax?0:1,1,1,unowned);
	} else if (t.crossing) {
		bool ax=t.roads==ROAD_X;
		for (int side : {2,13}) {
			Vec p=point(ax?side:2,ax?2:side,0.5f);
			RailBox(faces,p,{p.x+0.6f,p.y+0.6f,p.z+4},{0.9f,0.15f,0.1f},tile);
			RailBox(faces,{p.x,p.y,p.z+4},{p.x+(t.barred&&!ax?10:0.5f),p.y+(t.barred&&ax?10:0.5f),p.z+(t.barred?4.5f:9)},{0.95f,0.87f,0.78f},tile);
		}
	}
	if (t.road_stop && t.art < 0) {
		size_t first = faces.size();
		bool ax=(t.roads & ROAD_X)!=ROAD_NONE;
		RailBox(faces,point(1,1,0.4f),point(ax?15:3,ax?3:15,1.2f),{0.75f,0.74f,0.64f},tile);
		RailBox(faces,point(ax?5:1,ax?1:5,1),point(ax?11:3,ax?3:11,6),{0.18f,0.55f,0.65f},tile);
		TagStructure3D(faces, first, TransparencyOption::Buildings);
	}
	int portal_art = t.portal_kind >= 0 ? Art3D::Tunnel(t.portal_kind) : -1;
	if (portal_art >= 0) {
		/* Authored with the entrance on the local x = 0 edge and the tunnel running along local X. */
		AppendTurnedModel3D(faces,Art3D::models[portal_art],tile,TravelTurn3D(t.portal),float(t.ground_z),unowned);
	} else if (t.portal >= 0) {
		/* Entrance frame at the low edge; the dark inset sits inside the hill. */
		float dx=Rail3D::EDGES[t.portal][0]-8,dy=Rail3D::EDGES[t.portal][1]-8;
		dx/=8; dy/=8;
		std::vector<TerrainFace> local;
		RailBox(local,{-1,-7,0},{2,-4,9},{0.53f,0.52f,0.48f},tile);
		RailBox(local,{-1,4,0},{2,7,9},{0.53f,0.52f,0.48f},tile);
		RailBox(local,{-1,-7,7},{2,7,10},{0.65f,0.63f,0.57f},tile);
		RailQuad(local,{3,-4,0},{3,4,0},{3,4,7},{3,-4,7},{0.035f,0.04f,0.045f},tile);
		for (auto &face : local) {
			for (auto &p:face.p) p={x+8-dx*7+p.x*dx-p.y*dy,y+8-dy*7+p.x*dy+p.y*dx,t.ground_z+p.z};
			faces.push_back(face);
		}
	}
	/* NE, SE, SW, NW edge midpoints, in engine DiagDirection order. */
	const auto &ends = Rail3D::EDGES;
	const auto &paths = Rail3D::PATHS;
	for (Track track = TRACK_BEGIN; track < TRACK_END; track++) {
		if (!HasBit(t.tracks,track)) continue;
		float ax=ends[paths[track][0]][0], ay=ends[paths[track][0]][1];
		float bx=ends[paths[track][1]][0], by=ends[paths[track][1]][1];
		float length=std::hypot(bx-ax,by-ay), nx=-(by-ay)/length, ny=(bx-ax)/length;
		auto strip = [&](float offset,float half_width,float lift,std::array<float,3> colour) {
			for (int i=0;i<8;++i) {
				float u=i/8.0f,v=(i+1)/8.0f;
				auto p = [&](float f,float side) {
					auto xy = Rail3D::StripPoint(track, f, side);
					return point(xy[0], xy[1], lift);
				};
				RailQuad(faces,p(u,offset-half_width),p(v,offset-half_width),p(v,offset+half_width),p(u,offset+half_width),colour,tile);
			}
		};
		strip(0,3.1f,0.25f,{0.35f,0.32f,0.28f});
		strip(-1.6f,0.3f,0.65f,{0.78f,0.81f,0.84f});
		strip(1.6f,0.3f,0.65f,{0.78f,0.81f,0.84f});
		for (int reverse : {0,8}) {
			int td=track+reverse;
			if (!HasBit(t.signals,td)) continue;
			bool toward_b = to_underlying(TrackdirToExitdir(static_cast<Trackdir>(td))) == paths[track][1];
			float u=toward_b ? 0.8f : 0.2f, side=toward_b ? 4.2f : -4.2f;
			Vec p=point(ax+(bx-ax)*u+nx*side,ay+(by-ay)*u+ny*side,0.5f);
			unsigned kind=GB(t.signal_kinds,4*track,4);
			int art=Art3D::Signal(kind>>3,kind&7,HasBit(t.green_signals,td));
			if (art>=0) {
				/* Local +X is the direction of travel; the lamps face -X, towards the approaching train. */
				float dx=(bx-ax)/length,dy=(by-ay)/length;
				if (!toward_b) { dx=-dx; dy=-dy; }
				static const CompanyColours3D unowned=CompanyColours3DOf(INVALID_OWNER);
				AppendModel3D(faces,Art3D::models[art],tile,{p.x,p.y,p.z-0.5f},dx,dy,1,1,unowned);
				continue;
			}
			RailBox(faces,{p.x-0.35f,p.y-0.35f,p.z},{p.x+0.35f,p.y+0.35f,p.z+6},{0.25f,0.25f,0.27f},tile);
			auto colour=HasBit(t.green_signals,td) ? std::array<float,3>{0.15f,1.0f,0.2f} : std::array<float,3>{1.0f,0.1f,0.08f};
			RailBox(faces,{p.x-0.8f,p.y-0.8f,p.z+5},{p.x+0.8f,p.y+0.8f,p.z+7},colour,tile);
		}
	}
	if (t.station && t.art < 0) {
		size_t first = faces.size();
		bool along_x = t.tracks == TRACK_BIT_X;
		AppendAsset3D(faces,Asset3D::PLATFORM,tile,{x+(along_x?0:16),y,float(t.surface_z)},along_x?1:0,along_x?0:1);
		TagStructure3D(faces, first, TransparencyOption::Buildings);
	}
	if (!t.catenary.empty()) {
		/* Phase 8.6 catenary, where the classic view draws it. Pylons turn their cantilever towards the control point;
		 * wire_half is held at its start, so a wire held only at b runs from b. */
		size_t first = faces.size();
		for (const auto &c : t.catenary) {
			int piece = CatenaryPiece3D(c);
			int art = Art3D::Catenary(piece);
			if (art < 0) continue;
			Vec a{float(c.ax), float(c.ay), float(c.az)}, b{float(c.bx), float(c.by), float(c.bz)};
			if (c.pylon) {
				float dx = b.x - a.x, dy = b.y - a.y, length = std::hypot(dx, dy);
				AppendModel3D(faces, Art3D::models[art], tile, a, dx / length, dy / length, 1, 1, unowned);
				continue;
			}
			if (c.pylons == 2) std::swap(a, b);
			AppendSpanModel3D(faces, Art3D::models[art], tile, a, b);
		}
		TagStructure3D(faces, first, TransparencyOption::Catenary);
	}
	if ((t.depot >= 0 || t.road_depot >= 0) && t.art < 0) {
		size_t first = faces.size();
		/* Open entrance follows the authoritative depot direction; three walls and a roof. */
		for (int side=0;side<4;++side) {
			if (side==(t.depot>=0?t.depot:t.road_depot)) continue;
			float lx=side==2?14:1, ly=side==1?14:1;
			float hx=side==0?2:15, hy=side==3?2:15;
			RailBox(faces,point(lx,ly,0),point(hx,hy,9),{0.65f,0.30f,0.20f},tile);
		}
		RailBox(faces,point(1,1,9),point(15,15,10.5f),{0.28f,0.31f,0.35f},tile);
		TagStructure3D(faces, first, TransparencyOption::Buildings);
	}

}

struct Train3DSnapshot {
	int x, y, z, direction, length;
	bool engine;
	bool road = false;
	int model = -1;
	bool loaded = false;
	VehicleType type = VehicleType::Train;
	bool helicopter = false;
	int art = -1; ///< Phase 8 model, drawn instead of the placeholder and Phase 5 model.
	Owner owner = INVALID_OWNER;
	VehicleID vehicle = VehicleID::Invalid();
	bool operator==(const Train3DSnapshot &) const = default;
};
static std::vector<Train3DSnapshot> _train3d_snapshots;
static std::vector<TerrainFace> _train3d_faces;
static std::vector<VehicleID> _train3d_face_vehicles; ///< Owner of each fallback triangle for picking.

/** A vehicle drawn from the shared model library; the GPU path transforms it in the vertex shader. */
struct Vehicle3DInstance {
	size_t model; ///< Library index: Art3D::models, then Asset3D::models.
	Vec origin;
	float dx, dy, length;
	CompanyColours3D company;
	TileIndex tile;
	float stipple = 0; ///< 1 draws it stippled, as a transparent structure.
	VehicleID vehicle = VehicleID::Invalid(); ///< Effects have no clickable vehicle.
};
static std::vector<Vehicle3DInstance> _vehicle3d_instances;
static bool _vehicle3d_lod = false; ///< Reduced models remain opt-in pending visual approval.

static constexpr size_t FullVehicleModelCount3D() { return std::size(Art3D::models) + std::size(Asset3D::models); }

static const Asset3D::Model &VehicleModel3D(size_t index)
{
	if (index >= FullVehicleModelCount3D()) return ArtLOD3D::models[index - FullVehicleModelCount3D()];
	return index < std::size(Art3D::models) ? Art3D::models[index] : Asset3D::models[index - std::size(Art3D::models)];
}

/** Each view chooses independently, using its camera and the instance's actual length. */
static size_t VehicleLODModel3D(const Vehicle3DInstance &v, const Terrain3D::Camera &camera)
{
	/* The LOD bounds assume an orthographic scale; perspective views draw full models until distance LOD (9.6). */
	if (!_vehicle3d_lod || camera.Perspective() || v.model >= std::size(ArtLOD3D::levels)) return v.model;
	const auto &levels = ArtLOD3D::levels[v.model];
	for (int i = 1; i >= 0; --i) {
		const auto &level = levels.level[i];
		if (level.model != 0xFFFF && Terrain3D::ModelLODAllowed(level.error, levels.radius, levels.height, camera.scale, camera.tilt, v.length)) {
			return FullVehicleModelCount3D() + level.model;
		}
	}
	return v.model;
}

/** Whether a view leaves this vehicle out: a cab view does not draw the vehicle it sits on. */
static bool HiddenVehicle3D(const Vehicle3DInstance &v, const Terrain3D::Camera &camera)
{
	return camera.hides && v.origin == camera.hidden;
}

/** The CPU reference: the same instances expanded to world faces. */
static void AppendVehicleInstances3D(std::vector<TerrainFace> &faces, const Terrain3D::Camera &camera)
{
	for (const auto &v : _vehicle3d_instances) if (!HiddenVehicle3D(v, camera)) AppendModel3D(faces, VehicleModel3D(VehicleLODModel3D(v, camera)), v.tile, v.origin, v.dx, v.dy, v.length, 1, v.company);
}

/** Effects and industry moving parts, drawn from the library like vehicles; rebuilt when the state they show changes. */
static std::vector<Vehicle3DInstance> _effect3d_instances;

/** The CPU reference for effects and industry parts; stippled parts are tagged with the industry option. */
static void AppendEffectInstances3D(std::vector<TerrainFace> &faces, const Terrain3D::Camera &camera)
{
	for (const auto &v : _effect3d_instances) {
		size_t first = faces.size();
		AppendModel3D(faces, VehicleModel3D(VehicleLODModel3D(v, camera)), v.tile, v.origin, v.dx, v.dy, v.length, 1, v.company);
		if (v.stipple > 0) TagStructure3D(faces, first, TransparencyOption::Industries);
	}
}

static bool _infrastructure_audit = false;
struct InfrastructureObservation { uint32_t id; bool hidden = false; std::vector<uint64_t> seen; };
static std::vector<InfrastructureObservation> _infrastructure_observations;

/** Opt-in read-only runtime evidence, independently comparing vehicle z with bridge deck z.
 * One record per vehicle/category/tile, so a long demo does not flood the diagnostics log. */
static void AuditInfrastructureVehicle(const Vehicle *vehicle)
{
	if (!_infrastructure_audit) return;
	auto it=std::ranges::find(_infrastructure_observations,vehicle->index.base(),&InfrastructureObservation::id);
	if (it==_infrastructure_observations.end()) {
		_infrastructure_observations.push_back({vehicle->index.base()});
		it=std::prev(_infrastructure_observations.end());
	}
	TileIndex tile=TileVirtXY(vehicle->x_pos,vehicle->y_pos);
	bool hidden=vehicle->vehstatus.Test(VehState::Hidden), depot=vehicle->IsInDepot();
	uint32_t category=0;
	int deck=-1;
	if (depot) category=1;
	else if (hidden) category=2;
	else if (it->hidden) category=128;
	else if (IsBridgeAbove(tile)) {
		deck=GetBridgePixelHeight(GetNorthernBridgeEnd(tile));
		category=vehicle->z_pos==deck ? 4 : 8;
	} else if (IsTunnelTile(tile)) category=16;
	else if (IsLevelCrossingTile(tile)) category=32;
	it->hidden=hidden&&!depot;
	uint64_t key=uint64_t(tile.base())*256+category;
	if (category==0 || std::ranges::find(it->seen,key)!=it->seen.end()) return;
	it->seen.push_back(key);
	Debug(driver,1,"PHASE4 VEHICLE id={} type={} event={} tile=({}, {}) z={} deck={} rendered={}",vehicle->index,vehicle->type,category,TileX(tile),TileY(tile),vehicle->z_pos,deck,!hidden&&!depot);
}

/** World box holding an instance as drawn: as the draw-time culling test, from the model's horizontal reach scaled by its length and its height range. */
static ProofBox InstanceBounds3D(const Vehicle3DInstance &v)
{
	struct Reach { float radius = 0, low = 0, high = 0; };
	static const std::vector<Reach> reaches = [] {
		std::vector<Reach> result(std::size(Art3D::models) + std::size(Asset3D::models));
		for (size_t index = 0; index < result.size(); ++index) {
			const auto &model = VehicleModel3D(index);
			auto &reach = result[index];
			for (size_t i = 0; i < model.count; ++i) for (const auto &p : model.faces[i].p) {
				reach.radius = std::max(reach.radius, std::sqrt(p[0] * p[0] + p[1] * p[1]));
				reach.low = std::min(reach.low, p[2]); reach.high = std::max(reach.high, p[2]);
			}
		}
		return result;
	}();
	const auto &reach = reaches[v.model];
	float r = reach.radius * std::max(1.0f, v.length) + 1;
	return {{v.origin.x - r, v.origin.y - r, v.origin.z + reach.low - 1}, {v.origin.x + r, v.origin.y + r, v.origin.z + reach.high + 1}};
}

/** Add the bounds of every instance drawn in one list but not identically in the other: moved, added, removed or restyled. */
static void CollectChangedInstances3D(std::vector<ProofBox> &boxes, const std::vector<Vehicle3DInstance> &before, const std::vector<Vehicle3DInstance> &after)
{
	auto key = [](const Vehicle3DInstance &v) { return std::tie(v.model, v.origin.x, v.origin.y, v.origin.z, v.dx, v.dy, v.length, v.company, v.stipple); };
	auto less = [&](const Vehicle3DInstance *a, const Vehicle3DInstance *b) { return key(*a) < key(*b); };
	std::vector<const Vehicle3DInstance *> a, b;
	for (const auto &v : before) a.push_back(&v);
	for (const auto &v : after) b.push_back(&v);
	std::ranges::sort(a, less);
	std::ranges::sort(b, less);
	size_t i = 0, j = 0;
	while (i < a.size() || j < b.size()) {
		if (j == b.size() || (i < a.size() && less(a[i], b[j]))) {
			boxes.push_back(InstanceBounds3D(*a[i++]));
		} else if (i == a.size() || less(b[j], a[i])) {
			boxes.push_back(InstanceBounds3D(*b[j++]));
		} else {
			++i; ++j;
		}
	}
}

/** Add the bounds of each placeholder face that differs by position in the list. */
static void CollectChangedFaces3D(std::vector<ProofBox> &boxes, const std::vector<TerrainFace> &before, const std::vector<TerrainFace> &after)
{
	for (size_t i = 0; i < std::max(before.size(), after.size()); ++i) {
		if (i < before.size() && i < after.size() && before[i] == after[i]) continue;
		for (const auto *list : {&before, &after}) {
			if (i >= list->size()) continue;
			const auto &p = (*list)[i].p;
			boxes.push_back({{std::min({p[0].x, p[1].x, p[2].x}), std::min({p[0].y, p[1].y, p[2].y}), std::min({p[0].z, p[1].z, p[2].z})},
				{std::max({p[0].x, p[1].x, p[2].x}), std::max({p[0].y, p[1].y, p[2].y}), std::max({p[0].z, p[1].z, p[2].z})}});
		}
	}
}

/** Each wagon uses its own simulation position, never an inferred offset from the locomotive. */
static void RefreshTrains3D()
{
	std::vector<Train3DSnapshot> current;
	for (const Train *train : Train::Iterate()) {
		AuditInfrastructureVehicle(train);
		if (train->vehstatus.Test(VehState::Hidden) || train->IsInDepot() || train->direction >= Direction::End) continue;
		bool loaded=train->cargo.StoredCount()>0;
		/* As Train::GetImage: a rear dual head uses the reversed sprite set, and a flipped unit faces the other way. */
		bool reversed=train->IsRearDualheaded()!=train->flags.Test(VehicleRailFlag::Flipped);
		current.push_back({train->x_pos,train->y_pos,train->z_pos,to_underlying(reversed?ReverseDir(train->direction):train->direction),train->gcache.cached_veh_length,train->IsEngine(),false,VehicleAsset3D(train->engine_type),loaded,VehicleType::Train,false,VehicleArt3D(train->engine_type,loaded),train->owner,train->index});
	}
	for (const RoadVehicle *vehicle : RoadVehicle::Iterate()) {
		AuditInfrastructureVehicle(vehicle);
		if (vehicle->vehstatus.Test(VehState::Hidden) || vehicle->IsInDepot() || vehicle->direction >= Direction::End) continue;
		bool loaded=vehicle->cargo.StoredCount()>0;
		current.push_back({vehicle->x_pos,vehicle->y_pos,vehicle->z_pos,to_underlying(vehicle->direction),vehicle->gcache.cached_veh_length,false,true,VehicleAsset3D(vehicle->engine_type),loaded,VehicleType::Road,false,VehicleArt3D(vehicle->engine_type,loaded),vehicle->owner,vehicle->index});
	}
	for (const Ship *vehicle : Ship::Iterate()) {
		if (vehicle->vehstatus.Test(VehState::Hidden) || vehicle->IsInDepot() || vehicle->direction >= Direction::End) continue;
		bool loaded = vehicle->cargo.StoredCount() > 0;
		current.push_back({vehicle->x_pos, vehicle->y_pos, vehicle->z_pos, to_underlying(vehicle->direction), 16, false, false, -1, loaded, VehicleType::Ship, false, VehicleArt3D(vehicle->engine_type, loaded), vehicle->owner,vehicle->index});
	}
	for (const Aircraft *vehicle : Aircraft::Iterate()) {
		/* Shadow and rotor child vehicles are not separate flying bodies. */
		if (!vehicle->IsNormalAircraft() || vehicle->vehstatus.Test(VehState::Hidden) || vehicle->direction >= Direction::End) continue;
		bool loaded = vehicle->cargo.StoredCount() > 0;
		current.push_back({vehicle->x_pos, vehicle->y_pos, vehicle->z_pos, to_underlying(vehicle->direction), 16, false, false, -1, loaded, VehicleType::Aircraft, vehicle->subtype == AIR_HELICOPTER, VehicleArt3D(vehicle->engine_type, loaded), vehicle->owner,vehicle->index});
	}
	if (current == _train3d_snapshots) return;
	_train3d_snapshots = std::move(current);
	auto old_faces = std::move(_train3d_faces);
	auto old_instances = std::move(_vehicle3d_instances);
	_train3d_faces.clear();
	_train3d_face_vehicles.clear();
	_vehicle3d_instances.clear();
	for (const auto &train : _train3d_snapshots) {
		const auto &directions = Rail3D::DIRECTIONS;
		float dx=directions[train.direction][0],dy=directions[train.direction][1];
		float half=std::max(1.0f,train.length*0.5f-0.4f);
		std::vector<TerrainFace> local;
		TileIndex tile=TileVirtXY(train.x,train.y);
		if (train.art >= 0) {
			/* Rail and road models are authored for a standard 8-unit vehicle; ships and aircraft at world size. */
			float length = train.type == VehicleType::Ship || train.type == VehicleType::Aircraft ? 1.0f : half / 3.6f;
			_vehicle3d_instances.push_back({size_t(train.art), {float(train.x), float(train.y), float(train.z)}, dx, dy, length, CompanyColours3DOf(train.owner), tile, 0, train.vehicle});
			continue;
		}
		RailBox(local,{-half,-2,0.6f},{half,2,1.8f},{0.12f,0.14f,0.16f},tile);
		RailBox(local,{-half+0.2f,-1.8f,1.8f},{half-0.2f,1.8f,train.engine?5.5f:4.2f},train.engine?std::array<float,3>{0.15f,0.38f,0.75f}:std::array<float,3>{0.57f,0.29f,0.12f},tile);
		if (train.engine) RailBox(local,{half-2,-1.5f,5.5f},{half-0.3f,1.5f,6.5f},{0.75f,0.85f,0.9f},tile);
		if (train.road) {
			local.clear();
			RailBox(local,{-half,-1.7f,0.5f},{half,1.7f,1.3f},{0.12f,0.14f,0.16f},tile);
			RailBox(local,{-half,-1.6f,1.3f},{half-2,1.6f,4.5f},{0.88f,0.64f,0.16f},tile);
			RailBox(local,{half-2,-1.6f,1.3f},{half,1.6f,4},{0.18f,0.65f,0.73f},tile);
		}
		if (train.type == VehicleType::Ship) {
			local.clear();
			RailBox(local, {-7,-3,0}, {5,3,2}, {.18f,.28f,.38f}, tile);
			RailQuad(local, {5,-3,2}, {8,0,2}, {8,0,0}, {5,-3,0}, {.18f,.28f,.38f}, tile);
			RailQuad(local, {8,0,2}, {5,3,2}, {5,3,0}, {8,0,0}, {.18f,.28f,.38f}, tile);
			AddTerrainFace(local, {{{5,-3,2},{8,0,2},{5,3,2}}}, {.7f,.65f,.5f}, tile);
			RailBox(local, {-6,-2,2}, {-1,2,5}, {.85f,.85f,.75f}, tile);
			if (train.loaded) RailBox(local, {0,-2,2}, {4,2,3}, {.4f,.25f,.12f}, tile);
		} else if (train.type == VehicleType::Aircraft) {
			local.clear();
			RailBox(local, {-6,-1.5f,0}, {6,1.5f,2.5f}, {.85f,.88f,.9f}, tile);
			RailBox(local, {3,-1.4f,2}, {5,1.4f,3}, {.2f,.4f,.6f}, tile);
			RailBox(local, {-5,-.4f,2}, {-3,.4f,5}, {.2f,.4f,.7f}, tile);
			if (train.helicopter) {
				RailBox(local, {-1,-.3f,3}, {1,.3f,5}, {.2f,.23f,.26f}, tile);
				RailBox(local, {-1,-9,5}, {1,9,5.3f}, {.2f,.23f,.26f}, tile);
			} else {
				RailBox(local, {-2,-9,1}, {1,9,1.6f}, {.65f,.7f,.8f}, tile);
				RailBox(local, {-5,-4,2}, {-3,4,2.6f}, {.2f,.4f,.7f}, tile);
			}
		}
		if (train.model >= 0) {
			static const CompanyColours3D unowned=CompanyColours3DOf(INVALID_OWNER);
			_vehicle3d_instances.push_back({std::size(Art3D::models)+size_t(train.model),{float(train.x),float(train.y),float(train.z)},dx,dy,half/3.6f,unowned,tile,0,train.vehicle});
			if (train.model==Asset3D::WAGON && train.loaded) {
				local.clear();
				RailBox(local,{-half+.3f,-1.4f,2},{half-.3f,1.4f,3.6f},{.12f,.14f,.16f},tile);
			} else continue;
		}
		for (auto &face : local) {
			for (auto &p : face.p) p={train.x+p.x*dx-p.y*dy,train.y+p.x*dy+p.y*dx,train.z+p.z};
			_train3d_faces.push_back(face);
			_train3d_face_vehicles.push_back(train.vehicle);
		}
	}
	std::vector<ProofBox> boxes;
	CollectChangedInstances3D(boxes, old_instances, _vehicle3d_instances);
	CollectChangedFaces3D(boxes, old_faces, _train3d_faces);
	InvalidateProof3DRegions(boxes);
}

/** Effect vehicle sprite sequences, in the order of EFFECT_IDS in tools/3d_assets/convert_gltf.py (Art3D::Effect). */
static constexpr std::pair<SpriteID, uint8_t> EFFECT3D_SEQUENCES[] = {
	{SPR_CHIMNEY_SMOKE_0, 8}, {SPR_STEAM_SMOKE_0, 5}, {SPR_DIESEL_SMOKE_0, 6}, {SPR_ELECTRIC_SPARK_0, 6}, {SPR_SMOKE_0, 5},
	{SPR_EXPLOSION_LARGE_0, 16}, {SPR_BREAKDOWN_SMOKE_0, 4}, {SPR_EXPLOSION_SMALL_0, 12}, {SPR_BULLDOZER_NE, 4},
};
static constexpr int EFFECT3D_BULLDOZER = 8;

static int _effect3d_missing = 0; ///< Effect vehicles of a base sequence without a model, for the coverage audit.

/** Step of a base industry tile graphic's moving part, from the tile's own animation state; -1 draws none. */
static int IndustryPartStep3D(TileIndex tile, IndustryGfx gfx, unsigned steps)
{
	uint8_t frame = GetAnimationFrame(tile);
	switch (gfx) {
		case GFX_POWERPLANT_SPARKS: return frame >= 1 && frame <= steps ? frame - 1 : -1; // frame 0: no spark
		case GFX_OILWELL_NOT_ANIMATED: return 0;
		case GFX_OILWELL_ANIMATED_1: case GFX_OILWELL_ANIMATED_2: case GFX_OILWELL_ANIMATED_3:
			return ((gfx - GFX_OILWELL_ANIMATED_1) * 4 + (frame & 3)) % steps; // three graphics of four frames each
		case 20: case GFX_OILRIG_4: // flare stacks have no animation: the flame flickers with the game tick
			return ((uint)(_tick_counter / 8) + TileHash(TileX(tile), TileY(tile))) % steps;
		default: return frame % steps; // the headframe wheel steps with the frame counter
	}
}

/** Whether a base industry type has a tile graphic with a moving part, cached per type. */
static bool IndustryHasParts3D(IndustryType type)
{
	static std::array<int8_t, NUM_INDUSTRYTYPES> cache{};
	if (cache[type] == 0) {
		bool any = false;
		for (const auto &layout : GetIndustrySpec(type)->layouts) {
			for (const auto &it : layout) any |= it.gfx < NEW_INDUSTRYTILEOFFSET && Art3D::IndustryPartSteps(it.gfx) > 0;
		}
		cache[type] = any ? 1 : -1;
	}
	return cache[type] > 0;
}

/** Collect effect vehicles and industry moving parts. Effects hide as in the classic view when their transparency
 * option is set; industry parts follow the industry option like their tile. */
static void RefreshEffects3D()
{
	static const CompanyColours3D unowned = CompanyColours3DOf(INVALID_OWNER);
	std::vector<Vehicle3DInstance> current;
	int missing = 0;
	for (const EffectVehicle *v : EffectVehicle::Iterate()) {
		if (v->vehstatus.Test(VehState::Hidden)) continue;
		TransparencyOption to = v->GetTransparencyOption();
		if (to != TransparencyOption::Invalid && (IsTransparencySet(to) || IsInvisibilitySet(to))) continue;
		SpriteID sprite = v->sprite_seq.seq[0].sprite;
		int id = -1, frame = 0;
		for (int i = 0; i < int(std::size(EFFECT3D_SEQUENCES)); ++i) {
			if (sprite >= EFFECT3D_SEQUENCES[i].first && sprite < EFFECT3D_SEQUENCES[i].first + EFFECT3D_SEQUENCES[i].second) {
				id = i;
				frame = sprite - EFFECT3D_SEQUENCES[i].first;
			}
		}
		if (id < 0) continue; // bubbles (toyland, Phase 10)
		float dx = 1, dy = 0;
		if (id == EFFECT3D_BULLDOZER) {
			/* One model along +X; the sprite gives its direction (NE, SE, SW, NW). */
			const auto &d = Rail3D::DIRECTIONS[to_underlying(DiagDirToDir(static_cast<DiagDirection>(frame)))];
			dx = d[0]; dy = d[1];
			frame = 0;
		}
		int art = Art3D::Effect(id, frame);
		/* Diagnostics: the first sighting of each sequence, so a fixture run shows which effects were drawn. */
		static std::array<bool, std::size(EFFECT3D_SEQUENCES)> seen{};
		if (!seen[id]) {
			seen[id] = true;
			Debug(driver, 1, "PHASE8 EFFECT first id={} frame={} model={} x={} y={} z={}", id, frame, art, v->x_pos, v->y_pos, v->z_pos);
		}
		if (art < 0) { ++missing; continue; }
		current.push_back({size_t(art), {float(v->x_pos), float(v->y_pos), float(v->z_pos)}, dx, dy, 1, unowned, TileVirtXY(v->x_pos, v->y_pos)});
	}
	if (!IsInvisibilitySet(TransparencyOption::Industries)) {
		float stipple = IsTransparencySet(TransparencyOption::Industries) ? 1.0f : 0.0f;
		for (const Industry *ind : Industry::Iterate()) {
			if (!IndustryHasParts3D(ind->type)) continue;
			for (TileIndex tile : ind->location) {
				if (!IsTileType(tile, TileType::Industry) || GetIndustryIndex(tile) != ind->index || !IsIndustryCompleted(tile)) continue;
				IndustryGfx gfx = GetIndustryGfx(tile);
				if (gfx >= NEW_INDUSTRYTILEOFFSET) continue;
				unsigned steps = Art3D::IndustryPartSteps(gfx);
				int step = steps > 0 ? IndustryPartStep3D(tile, gfx, steps) : -1;
				if (step < 0) continue;
				/* As the static tile: from the tile corner, on top of its foundation. */
				Vec origin{float(TileX(tile) * TILE_SIZE), float(TileY(tile) * TILE_SIZE), float(GetTileMaxPixelZ(tile))};
				current.push_back({size_t(Art3D::IndustryPart(gfx, step)), origin, 1, 0, 1, unowned, tile, stipple});
			}
		}
	}
	_effect3d_missing = missing;
	auto same = [](const Vehicle3DInstance &a, const Vehicle3DInstance &b) {
		return a.model == b.model && a.origin == b.origin && a.dx == b.dx && a.dy == b.dy && a.stipple == b.stipple;
	};
	if (std::ranges::equal(current, _effect3d_instances, same)) return;
	std::vector<ProofBox> boxes;
	CollectChangedInstances3D(boxes, _effect3d_instances, current);
	_effect3d_instances = std::move(current);
	InvalidateProof3DRegions(boxes);
}

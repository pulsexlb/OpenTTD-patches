/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file blueprint.h Blueprint (copy and paste of infrastructure) data structures. */

#ifndef BLUEPRINT_H
#define BLUEPRINT_H

#include <chrono>
#include <optional>

#include "string_type.h"

#include "bridge.h"
#include "direction_type.h"
#include "map_type.h"
#include "rail_type.h"
#include "road_type.h"
#include "signal_type.h"
#include "station_type.h"
#include "tile_type.h"
#include "tilearea_type.h"
#include "track_type.h"
#include "air_type.h"
#include "transport_type.h"

/** Number of blueprint slots on one page; the toolbar shows exactly one page at a time. */
static const uint NUM_BLUEPRINT_SLOTS = 8;

/** Number of slot pages the toolbar can switch between. */
static const uint NUM_BLUEPRINT_SLOT_PAGES = 6;

/** Number of available blueprint slots across every page. */
static const uint NUM_BLUEPRINT_TOTAL_SLOTS = NUM_BLUEPRINT_SLOTS * NUM_BLUEPRINT_SLOT_PAGES;

/** Maximum size of a blueprint along either axis. */
static const uint MAX_BLUEPRINT_DIMENSION = 255;

/** Maximum absolute value of the paste height offset. */
static const int8_t MAX_BLUEPRINT_HEIGHT_OFFSET = 8;

/** The maximum length of a blueprint name in characters including '\0'. */
static const uint MAX_LENGTH_BLUEPRINT_NAME_CHARS = 32;

/** How the landscape is adjusted while pasting a blueprint. */
enum class BlueprintTerraformMode : uint8_t {
	None,    ///< Do not terraform; only paste what fits the current landscape.
	Minimal, ///< Terraform only the tiles that receive blueprint elements.
	Full,    ///< Terraform the whole pasted area to match the copied landscape.
};

/**
 * Options controlling how a blueprint is pasted.
 *
 * The transformation is an element of the dihedral group D4, stored as
 * "reflect against the NW-SE axis first (if #reflected), then rotate
 * #rotation quarter turns anticlockwise".
 */
struct BlueprintPasteOptions {
	bool paste_rail = true;        ///< Paste rail transport infrastructure.
	bool paste_road = true;        ///< Paste road transport infrastructure.
	bool paste_water = true;       ///< Paste water transport infrastructure.
	bool paste_air = true;         ///< Paste air transport infrastructure.
	bool convert_railtype = false; ///< Convert rail to the current rail type when pasting.
	bool mirror_signals = false;   ///< Mirror signals when pasting.
	bool upgrade_bridges = false;  ///< Upgrade bridges when pasting.
	bool with_stations = true;     ///< Paste stations and waypoints too.
	BlueprintTerraformMode terraform_mode = BlueprintTerraformMode::None; ///< Landscape adjustment mode.
	uint8_t rotation = 0;          ///< Quarter turns anticlockwise, applied after the reflection.
	bool reflected = false;        ///< Whether to reflect against the NW-SE axis before rotating.
	int8_t height_offset = 0;      ///< Height offset applied when pasting.

	/**
	 * Compose an additional rotation onto the current transformation.
	 * @param quarter_turns_ccw Number of quarter turns anticlockwise (may be negative).
	 */
	void AddRotation(int quarter_turns_ccw)
	{
		this->rotation = static_cast<uint8_t>((this->rotation + quarter_turns_ccw + 4) % 4);
	}

	/**
	 * Compose an additional reflection onto the current transformation.
	 * Any reflection axis of D4 can be written as "reflect against NW-SE, then rotate".
	 * @param axis_rotation Quarter turns anticlockwise the reflection axis is rotated relative to the NW-SE axis times two (0 = NW-SE, 2 = NE-SW).
	 */
	void AddReflection(int axis_rotation)
	{
		/* F_axis = R^axis_rotation * F  and  F * R^r = R^(-r) * F, hence
		 * F_axis * (R^r * F^f) = R^((axis_rotation - r) mod 4) * F^(1 - f). */
		this->rotation = static_cast<uint8_t>((axis_rotation - this->rotation + 4) % 4);
		this->reflected = !this->reflected;
	}

	/** Reset the transformation to the identity. */
	void ResetTransformation()
	{
		this->rotation = 0;
		this->reflected = false;
	}
};

/** A plain rail track piece (also the rail part of a level crossing). */
struct BlueprintRailTrack {
	TileIndexDiffC offset; ///< Tile offset relative to the blueprint origin.
	Track track;           ///< Track piece on the tile.
	RailType railtype;     ///< Rail type of the track.
};

/** Stable identity of a NewGRF style; zero GRFID means the original style. */
struct BlueprintSpecRef {
	uint32_t grfid = 0;
	uint16_t local_id = 0;
	bool operator==(const BlueprintSpecRef &) const = default;
};

/** Signals on one track piece. */
struct BlueprintSignal {
	TileIndexDiffC offset;  ///< Tile offset relative to the blueprint origin.
	Track track;            ///< Track piece the signals are on.
	SignalType type;        ///< Type of the signals.
	SignalVariant variant;  ///< Electric / semaphore.
	bool along;             ///< Signal along the track direction of #track.
	bool against;           ///< Signal against the track direction of #track.
	BlueprintSpecRef spec{}; ///< Explicit NewGRF signal style, independent of the game's style slot.
};

/** One complete NewGRF object, anchored at the northern tile of its footprint. */
struct BlueprintObject {
	TileIndexDiffC offset;
	BlueprintSpecRef spec;
	uint8_t view;
	uint8_t views; ///< Available views when copied; transformations never depend on local GRFs.
	uint8_t w;
	uint8_t h;
};

/** A rail depot. */
struct BlueprintRailDepot {
	TileIndexDiffC offset; ///< Tile offset relative to the blueprint origin.
	DiagDirection dir;     ///< Direction the depot exits to.
	RailType railtype;     ///< Rail type of the depot.
};

/** A bridge or tunnel; both ends lie within the copied area. */
struct BlueprintTunnelBridge {
	TileIndexDiffC offset;    ///< Tile offset of the northern end.
	TileIndexDiffC other_end; ///< Tile offset of the southern end.
	TransportType transport;  ///< Rail / road / water (aqueduct; bridge only).
	bool is_bridge;           ///< True for bridges, false for tunnels.
	BridgeType bridge_type;   ///< Bridge type (only valid when #is_bridge).
	RailType railtype;        ///< Rail type (only valid for rail transport).
	RoadType roadtype;        ///< Road type (only valid for road transport, may be INVALID_ROADTYPE).
	RoadType tramtype;        ///< Tram type (only valid for road transport, may be INVALID_ROADTYPE).
};

/** Road or tram bits on one tile (also the road part of a level crossing). */
struct BlueprintRoad {
	TileIndexDiffC offset; ///< Tile offset relative to the blueprint origin.
	RoadBits bits;         ///< Road pieces on the tile.
	RoadType roadtype;     ///< Road or tram type of the pieces.
};

/** A road depot. */
struct BlueprintRoadDepot {
	TileIndexDiffC offset; ///< Tile offset relative to the blueprint origin.
	DiagDirection dir;     ///< Direction the depot exits to.
	RoadType roadtype;     ///< Road or tram type of the depot.
};

/** A canal tile. */
struct BlueprintCanal {
	TileIndexDiffC offset; ///< Tile offset relative to the blueprint origin.
};

/** A lock (anchored at its middle part). */
struct BlueprintLock {
	TileIndexDiffC offset; ///< Tile offset of the middle part.
	DiagDirection dir;     ///< Direction of the lock.
};

/** A ship depot (anchored at its northern part). */
struct BlueprintShipDepot {
	TileIndexDiffC offset; ///< Tile offset of the northern part.
	Axis axis;             ///< Axis of the depot.
};

/** One tile of a rail station or rail waypoint. */
struct BlueprintStationTile {
	TileIndexDiffC offset; ///< Tile offset relative to the blueprint origin.
	bool is_waypoint;      ///< True for waypoint tiles, false for station tiles.
	Axis axis;             ///< Axis of the platform.
	RailType railtype;     ///< Rail type of the platform.
	StationID station;     ///< Original station; parts of one station are joined again when pasting.
	BlueprintSpecRef spec{}; ///< NewGRF platform or waypoint style.
};

/** A road stop (bus / truck / road waypoint). */
struct BlueprintRoadStop {
	TileIndexDiffC offset; ///< Tile offset relative to the blueprint origin.
	StationType type;      ///< StationType::Bus, StationType::Truck or StationType::RoadWaypoint.
	bool drive_through;    ///< True for drive-through stops.
	DiagDirection dir;     ///< Entrance direction (bay stops only).
	Axis axis;             ///< Axis (drive-through stops only).
	RoadType roadtype;     ///< Road type on the stop (may be INVALID_ROADTYPE).
	RoadType tramtype;     ///< Tram type on the stop (may be INVALID_ROADTYPE).
	StationID station;     ///< Original station; parts of one station are joined again when pasting.
	BlueprintSpecRef spec{}; ///< NewGRF stop or road waypoint style.
};

/** A dock (anchored at its land part). */
struct BlueprintDock {
	TileIndexDiffC offset; ///< Tile offset of the land part.
	DiagDirection dir;     ///< Direction of the dock.
	StationID station;     ///< Original station; parts of one station are joined again when pasting.
};

/** A buoy. */
struct BlueprintBuoy {
	TileIndexDiffC offset; ///< Tile offset relative to the blueprint origin.
};

/** One tile of a custom (multitile) airport. */
struct BlueprintAirportTile {
	TileIndexDiffC offset; ///< Tile offset relative to the blueprint origin.
	uint8_t m4;            ///< Raw m4 of the airport tile (airtype gfx id).
	uint8_t m5;            ///< Raw m5 of the airport tile (gfx + airport tile type).
	uint8_t m6;            ///< Raw m6 of the airport tile (airtype gfx flag + ground counter).
	uint16_t m8;           ///< Raw m8 of the airport tile, with reservation bits cleared.
};

/** An airport (anchored at its northern tile). */
struct BlueprintAirport {
	TileIndexDiffC offset; ///< Tile offset of the northern airport tile.
	uint8_t type;          ///< Airport type; AT_CUSTOM for custom (multitile) airports.
	uint8_t layout;        ///< Airport layout (rotation).
	uint8_t rotation;      ///< DiagDirection rotation of spec airports.
	uint8_t air_type;      ///< AirType of the airport.
	uint16_t w;            ///< Size of the airport along the X axis.
	uint16_t h;            ///< Size of the airport along the Y axis.
	StationID station;     ///< Original station; parts of one station are joined again when pasting.
	std::vector<BlueprintAirportTile> tiles; ///< Per-tile state; only filled for custom airports.
};

/** Contents of one blueprint: infrastructure copied from a rectangular map area. */
struct Blueprint {
	uint16_t width = 0;  ///< Size of the copied area along the X axis.
	uint16_t height = 0; ///< Size of the copied area along the Y axis.
	std::string name;    ///< Player-given name; empty when never named. Not counted as content by #IsEmpty.

	std::vector<BlueprintRailTrack> rail_tracks;
	std::vector<BlueprintSignal> signals;
	std::vector<BlueprintRailDepot> rail_depots;
	std::vector<BlueprintTunnelBridge> tunnel_bridges;
	std::vector<BlueprintRoad> roads;
	std::vector<BlueprintRoadDepot> road_depots;
	std::vector<BlueprintCanal> canals;
	std::vector<BlueprintLock> locks;
	std::vector<BlueprintShipDepot> ship_depots;
	std::vector<BlueprintStationTile> station_tiles;
	std::vector<BlueprintRoadStop> road_stops;
	std::vector<BlueprintDock> docks;
	std::vector<BlueprintBuoy> buoys;
	std::vector<BlueprintAirport> airports;
	std::vector<BlueprintObject> objects;

	/** Heights of the tile corners of the copied area, (width + 1) x (height + 1) values, index = y * (width + 1) + x. */
	std::vector<uint8_t> corner_heights;

	uint CountItems() const;
	void Clear();

	/** Is there anything stored in this blueprint? */
	bool IsEmpty() const { return this->CountItems() == 0; }
};

/**
 * Everything a paste command needs: a blueprint that already has the
 * transformation, the paste filters and the rail/signal/bridge options
 * applied, plus the landscape adjustment settings.
 */
struct BlueprintPasteData {
	Blueprint blueprint;                                                  ///< Pre-transformed and pre-filtered blueprint.
	BlueprintTerraformMode terraform_mode = BlueprintTerraformMode::None; ///< Landscape adjustment mode.
	int8_t height_offset = 0;                                             ///< Height offset applied when pasting.

	static constexpr bool command_payload_as_ref = true; ///< Passed to the command handler by reference.
	void Serialise(struct BufferSerialisationRef buffer) const;
	bool Deserialise(struct DeserialisationBuffer &buffer, StringValidationSettings default_string_validation);
	void fmt_format_value(struct format_target &buf) const;
};

/** One rectangle of equal station tiles, used to rebuild stations/waypoints. */
struct BlueprintStationRect {
	TileIndexDiffC offset; ///< Northern tile of the rectangle.
	uint16_t w;            ///< Extent along the X axis.
	uint16_t h;            ///< Extent along the Y axis.
	bool is_waypoint;      ///< True for waypoints.
	Axis axis;             ///< Platform axis.
	RailType railtype;     ///< Rail type.
	StationID station;     ///< Original station; parts of one station are joined again when pasting.
	BlueprintSpecRef spec{}; ///< All tiles in this rectangle share the same style.
};

/**
 * Everything an undo command needs: the parts a paste built, and whether the
 * landscape should be put back as well.
 *
 * The payload travels from the client that pasted, so it must be assumed
 * forgeable. That is safe as long as every removal it drives goes through a
 * stock command as the issuing company, where it is ownership-checked and
 * priced normally.
 */
struct BlueprintUndoData {
	Blueprint built;              ///< What the paste built; #Blueprint::corner_heights holds the heights to restore.
	bool restore_terrain = false; ///< Whether to restore those heights, rather than only removing the infrastructure.

	static constexpr bool command_payload_as_ref = true; ///< Passed to the command handler by reference.
	void Serialise(struct BufferSerialisationRef buffer) const;
	bool Deserialise(struct DeserialisationBuffer &buffer, StringValidationSettings default_string_validation);
	void fmt_format_value(struct format_target &buf) const;
};

/** Which kinds of infrastructure an undo is allowed to remove. */
struct BlueprintUndoFilters {
	bool rail = true;  ///< Rail track, signals, rail depots, rail stations and waypoints, rail bridges and tunnels.
	bool road = true;  ///< Road and tram bits, road depots, road stops and waypoints, road bridges and tunnels.
	bool water = true; ///< Canals, locks, ship depots, docks, buoys, aqueducts.
	bool air = true;   ///< Airports.
	bool objects = true; ///< NewGRF objects.

	/** Is every kind allowed, so that an undo consumes its whole record? */
	bool All() const { return this->rail && this->road && this->water && this->air && this->objects; }

	/** Is nothing allowed, so that an undo would do nothing at all? */
	bool None() const { return !this->rail && !this->road && !this->water && !this->air && !this->objects; }
};

/** Number of pastes that can be undone per company; older ones are dropped. */
static const uint MAX_BLUEPRINT_UNDO_DEPTH = 8;

/** How long an undoable paste stays undoable, in real time. */
static const std::chrono::minutes BLUEPRINT_UNDO_LIFETIME{60};

/**
 * One undoable paste: the parts that actually went in, and the landscape they
 * were built on.
 *
 * This is client-side scratch state, not game state. It is filled while the
 * paste command executes, is never saved, never synchronised and never
 * survives a restart, and it must never influence the result of a command —
 * doing so would make it a desync source. Its only purpose is to supply the
 * payload of the undo command.
 */
struct BlueprintUndoRecord {
	TileIndex origin = INVALID_TILE; ///< Northern tile of the pasted area; the offsets in #built are relative to it.

	/**
	 * The parts that were actually built. #Blueprint::width and
	 * #Blueprint::height are those of the pasted blueprint rather than of the
	 * built subset, so that the corner grid keeps its geometry;
	 * #Blueprint::corner_heights holds the corner heights as they were
	 * *before* terraforming, and is empty when the paste did not terraform.
	 */
	Blueprint built;

	std::chrono::steady_clock::time_point when; ///< Real time of the paste, for the expiry. Client-side only, never serialised.

	/** Is there anything here to undo? */
	bool IsEmpty() const { return this->built.IsEmpty() && this->built.corner_heights.empty(); }
};

Blueprint &GetBlueprint(uint slot);
uint CopyAreaToBlueprint(TileArea area, Blueprint &blueprint);
void ClearBlueprintUndoStack();
const BlueprintUndoRecord *GetBlueprintUndoRecord();
const std::vector<BlueprintUndoRecord> &GetBlueprintUndoStack();
void CollectBlueprintUndoTiles(const BlueprintUndoRecord &record, const BlueprintUndoFilters &filters, std::vector<TileIndex> &tiles);
void ConsumeBlueprintUndoRecord(TileIndex origin, const BlueprintUndoFilters &filters);
void FilterBlueprintUndoData(Blueprint &built, const BlueprintUndoFilters &filters);

/**
 * Recording hand-built work so that it can be undone.
 *
 * The command layer brackets every top-level command with
 * #BeginBlueprintUndoGroup and #EndBlueprintUndoGroup, and the leaf
 * construction commands report what they built in between. One user action is
 * therefore one undo step, however many sub-commands it runs: a rail drag lays
 * a track piece per tile and still undoes in one go.
 */
void BeginBlueprintUndoGroup();
void EndBlueprintUndoGroup(bool success);
void AbandonBlueprintUndoGroup();

void RecordBlueprintUndoRailTrack(TileIndex tile, Track track, RailType railtype);
void RecordBlueprintUndoSignal(TileIndex tile, Track track);
void RecordBlueprintUndoObject(TileIndex tile);
void RecordBlueprintUndoRailDepot(TileIndex tile, DiagDirection dir, RailType railtype);
void RecordBlueprintUndoRoad(TileIndex tile, RoadBits bits, RoadType roadtype);
void RecordBlueprintUndoRoadDepot(TileIndex tile, DiagDirection dir, RoadType roadtype);
void RecordBlueprintUndoTunnelBridge(TileIndex tile, TileIndex other_end, bool is_bridge, TransportType transport);
void RecordBlueprintUndoCanal(TileIndex tile);
void RecordBlueprintUndoLock(TileIndex tile, DiagDirection dir);
void RecordBlueprintUndoShipDepot(TileIndex tile, Axis axis);
void RecordBlueprintUndoBuoy(TileIndex tile);
void RecordBlueprintUndoDock(TileIndex tile, DiagDirection dir);
void RecordBlueprintUndoAirport(TileIndex tile, uint8_t type, uint8_t layout, uint8_t rotation, uint8_t air_type, uint16_t w, uint16_t h);
void RecordBlueprintUndoCustomAirportTile(TileIndex tile, AirType air_type);
void RecordBlueprintUndoStationTile(TileIndex tile, bool is_waypoint, Axis axis, RailType railtype);
void RecordBlueprintUndoRoadStop(TileIndex tile, StationType type, bool drive_through, DiagDirection dir, Axis axis);
void RecordBlueprintUndoTerraform(TileIndex corner, uint8_t height_before);
std::vector<BlueprintStationRect> GroupStationTiles(const Blueprint &blueprint);
std::optional<uint8_t> ResolveBlueprintSignalStyle(const BlueprintSpecRef &spec);
const struct ObjectSpec *ResolveBlueprintObject(const BlueprintSpecRef &spec);

TileIndexDiffC TransformBlueprintPoint(TileIndexDiffC point, uint16_t bounds_x, uint16_t bounds_y, uint8_t rotation, bool reflected);
Track TransformBlueprintTrack(Track track, uint8_t rotation, bool reflected);
DiagDirection TransformBlueprintDiagDir(DiagDirection dir, uint8_t rotation, bool reflected);
Axis TransformBlueprintAxis(Axis axis, uint8_t rotation);
RoadBits TransformBlueprintRoadBits(RoadBits bits, uint8_t rotation, bool reflected);
Blueprint TransformBlueprint(const Blueprint &blueprint, uint8_t rotation, bool reflected);
BlueprintPasteData PrepareBlueprintPaste(const Blueprint &blueprint, const BlueprintPasteOptions &options, RailType convert_to);

#endif /* BLUEPRINT_H */

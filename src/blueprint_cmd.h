/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file blueprint_cmd.h Command definitions related to pasting blueprints. */

#ifndef BLUEPRINT_CMD_H
#define BLUEPRINT_CMD_H

#include "blueprint.h"
#include "string_type.h"
#include "command_type.h"
#include "core/format.hpp"
#include "core/serialisation.hpp"

/* The Blueprint patch was written against the pre-enum-class command names. */
constexpr Commands CMD_BUILD_SINGLE_RAIL = Commands::BuildRail;
constexpr Commands CMD_REMOVE_SINGLE_RAIL = Commands::RemoveRail;
constexpr Commands CMD_LANDSCAPE_CLEAR = Commands::LandscapeClear;
constexpr Commands CMD_BUILD_BRIDGE = Commands::BuildBridge;
constexpr Commands CMD_BUILD_RAIL_STATION = Commands::BuildRailStation;
constexpr Commands CMD_BUILD_TRAIN_DEPOT = Commands::BuildRailDepot;
constexpr Commands CMD_BUILD_SINGLE_SIGNAL = Commands::BuildSignal;
constexpr Commands CMD_REMOVE_SINGLE_SIGNAL = Commands::RemoveSignal;
constexpr Commands CMD_TERRAFORM_LAND = Commands::TerraformLand;
constexpr Commands CMD_BUILD_TUNNEL = Commands::BuildTunnel;
constexpr Commands CMD_REMOVE_FROM_RAIL_STATION = Commands::RemoveFromRailStation;
constexpr Commands CMD_BUILD_RAIL_WAYPOINT = Commands::BuildRailWaypoint;
constexpr Commands CMD_REMOVE_FROM_RAIL_WAYPOINT = Commands::RemoveFromRailWaypoint;
constexpr Commands CMD_BUILD_ROAD_WAYPOINT = Commands::BuildRoadWaypoint;
constexpr Commands CMD_REMOVE_FROM_ROAD_WAYPOINT = Commands::RemoveFromRoadWaypoint;
constexpr Commands CMD_BUILD_ROAD_STOP = Commands::BuildRoadStop;
constexpr Commands CMD_REMOVE_ROAD_STOP = Commands::RemoveRoadStop;
constexpr Commands CMD_REMOVE_LONG_ROAD = Commands::RemoveRoadLong;
constexpr Commands CMD_BUILD_ROAD = Commands::BuildRoad;
constexpr Commands CMD_BUILD_ROAD_DEPOT = Commands::BuildRoadDepot;
constexpr Commands CMD_BUILD_AIRPORT = Commands::BuildAirport;
constexpr Commands CMD_BUILD_DOCK = Commands::BuildDock;
constexpr Commands CMD_BUILD_SHIP_DEPOT = Commands::BuildShipDepot;
constexpr Commands CMD_BUILD_BUOY = Commands::BuildBuoy;
constexpr Commands CMD_BUILD_CANAL = Commands::BuildCanal;
constexpr Commands CMD_BUILD_LOCK = Commands::BuildLock;

CommandCost CmdPasteBlueprint(DoCommandFlags flags, TileIndex origin, const BlueprintPasteData &data);
CommandCost CmdUndoBlueprintPaste(DoCommandFlags flags, TileIndex origin, const BlueprintUndoData &data);

DEF_CMD_TUPLE(Commands::PasteBlueprint, CmdPasteBlueprint, CMD_NO_TEST, CommandType::LandscapeConstruction, CmdDataT<BlueprintPasteData>) // the landscape may change between test and execution
DEF_CMD_TUPLE(Commands::UndoBlueprintPaste, CmdUndoBlueprintPaste, CMD_NO_TEST, CommandType::LandscapeConstruction, CmdDataT<BlueprintUndoData>) // likewise; parts that are no longer what the paste left are skipped

/* Serialization of a blueprint for the paste command. */

/** Maximum number of items read back per blueprint vector; large enough for the corner heights of a maximum-size blueprint. */
static const uint32_t MAX_BLUEPRINT_VECTOR_LENGTH = (MAX_BLUEPRINT_DIMENSION + 1) * (MAX_BLUEPRINT_DIMENSION + 1);

/** Serialized size (in bytes) above which a paste command does not fit a network packet any more. */
static const size_t MAX_BLUEPRINT_COMMAND_SIZE = 30000;

inline void SerialiseBlueprintOffset(BufferSerialisationRef buffer, const TileIndexDiffC &data)
{
	buffer.Send_generic_seq(data.x, data.y);
}

inline void DeserialiseBlueprintOffset(DeserialisationBuffer &buffer, TileIndexDiffC &data, StringValidationSettings settings)
{
	buffer.Recv_generic_seq(settings, data.x, data.y);
}

inline void SerialiseBlueprintItem(BufferSerialisationRef buffer, const BlueprintRailTrack &data)
{
	SerialiseBlueprintOffset(buffer, data.offset);
	buffer.Send_generic_seq(data.track, data.railtype);
}

inline void DeserialiseBlueprintItem(DeserialisationBuffer &buffer, BlueprintRailTrack &data, StringValidationSettings settings)
{
	DeserialiseBlueprintOffset(buffer, data.offset, settings);
	buffer.Recv_generic_seq(settings, data.track, data.railtype);
}

inline void SerialiseBlueprintItem(BufferSerialisationRef buffer, const BlueprintSignal &data)
{
	SerialiseBlueprintOffset(buffer, data.offset);
	buffer.Send_generic_seq(data.track, data.type, data.variant, data.along, data.against);
	buffer.Send_generic_seq(data.spec.grfid, data.spec.local_id);
}

inline void DeserialiseBlueprintItem(DeserialisationBuffer &buffer, BlueprintSignal &data, StringValidationSettings settings, bool with_specs = true)
{
	DeserialiseBlueprintOffset(buffer, data.offset, settings);
	buffer.Recv_generic_seq(settings, data.track, data.type, data.variant, data.along, data.against);
	if (with_specs) buffer.Recv_generic_seq(settings, data.spec.grfid, data.spec.local_id);
}

inline void SerialiseBlueprintItem(BufferSerialisationRef buffer, const BlueprintRailDepot &data)
{
	SerialiseBlueprintOffset(buffer, data.offset);
	buffer.Send_generic_seq(data.dir, data.railtype);
}

inline void DeserialiseBlueprintItem(DeserialisationBuffer &buffer, BlueprintRailDepot &data, StringValidationSettings settings)
{
	DeserialiseBlueprintOffset(buffer, data.offset, settings);
	buffer.Recv_generic_seq(settings, data.dir, data.railtype);
}

inline void SerialiseBlueprintItem(BufferSerialisationRef buffer, const BlueprintTunnelBridge &data)
{
	SerialiseBlueprintOffset(buffer, data.offset);
	SerialiseBlueprintOffset(buffer, data.other_end);
	buffer.Send_generic_seq(data.transport, data.is_bridge, data.bridge_type, data.railtype, data.roadtype, data.tramtype);
}

inline void DeserialiseBlueprintItem(DeserialisationBuffer &buffer, BlueprintTunnelBridge &data, StringValidationSettings settings)
{
	DeserialiseBlueprintOffset(buffer, data.offset, settings);
	DeserialiseBlueprintOffset(buffer, data.other_end, settings);
	buffer.Recv_generic_seq(settings, data.transport, data.is_bridge, data.bridge_type, data.railtype, data.roadtype, data.tramtype);
}

inline void SerialiseBlueprintItem(BufferSerialisationRef buffer, const BlueprintRoad &data)
{
	SerialiseBlueprintOffset(buffer, data.offset);
	buffer.Send_generic_seq(data.bits, data.roadtype);
}

inline void DeserialiseBlueprintItem(DeserialisationBuffer &buffer, BlueprintRoad &data, StringValidationSettings settings)
{
	DeserialiseBlueprintOffset(buffer, data.offset, settings);
	buffer.Recv_generic_seq(settings, data.bits, data.roadtype);
}

inline void SerialiseBlueprintItem(BufferSerialisationRef buffer, const BlueprintRoadDepot &data)
{
	SerialiseBlueprintOffset(buffer, data.offset);
	buffer.Send_generic_seq(data.dir, data.roadtype);
}

inline void DeserialiseBlueprintItem(DeserialisationBuffer &buffer, BlueprintRoadDepot &data, StringValidationSettings settings)
{
	DeserialiseBlueprintOffset(buffer, data.offset, settings);
	buffer.Recv_generic_seq(settings, data.dir, data.roadtype);
}

inline void SerialiseBlueprintItem(BufferSerialisationRef buffer, const BlueprintCanal &data)
{
	SerialiseBlueprintOffset(buffer, data.offset);
}

inline void DeserialiseBlueprintItem(DeserialisationBuffer &buffer, BlueprintCanal &data, StringValidationSettings settings)
{
	DeserialiseBlueprintOffset(buffer, data.offset, settings);
}

inline void SerialiseBlueprintItem(BufferSerialisationRef buffer, const BlueprintLock &data)
{
	SerialiseBlueprintOffset(buffer, data.offset);
	buffer.Send_generic(data.dir);
}

inline void DeserialiseBlueprintItem(DeserialisationBuffer &buffer, BlueprintLock &data, StringValidationSettings settings)
{
	DeserialiseBlueprintOffset(buffer, data.offset, settings);
	buffer.Recv_generic(data.dir);
}

inline void SerialiseBlueprintItem(BufferSerialisationRef buffer, const BlueprintShipDepot &data)
{
	SerialiseBlueprintOffset(buffer, data.offset);
	buffer.Send_generic(data.axis);
}

inline void DeserialiseBlueprintItem(DeserialisationBuffer &buffer, BlueprintShipDepot &data, StringValidationSettings settings)
{
	DeserialiseBlueprintOffset(buffer, data.offset, settings);
	buffer.Recv_generic(data.axis);
}

inline void SerialiseBlueprintItem(BufferSerialisationRef buffer, const BlueprintStationTile &data)
{
	SerialiseBlueprintOffset(buffer, data.offset);
	buffer.Send_generic_seq(data.is_waypoint, data.axis, data.railtype, data.station);
	buffer.Send_generic_seq(data.spec.grfid, data.spec.local_id);
}

inline void DeserialiseBlueprintItem(DeserialisationBuffer &buffer, BlueprintStationTile &data, StringValidationSettings settings, bool with_specs = true)
{
	DeserialiseBlueprintOffset(buffer, data.offset, settings);
	buffer.Recv_generic_seq(settings, data.is_waypoint, data.axis, data.railtype, data.station);
	if (with_specs) buffer.Recv_generic_seq(settings, data.spec.grfid, data.spec.local_id);
}

inline void SerialiseBlueprintItem(BufferSerialisationRef buffer, const BlueprintRoadStop &data)
{
	SerialiseBlueprintOffset(buffer, data.offset);
	buffer.Send_generic_seq(data.type, data.drive_through, data.dir, data.axis, data.roadtype, data.tramtype, data.station);
	buffer.Send_generic_seq(data.spec.grfid, data.spec.local_id);
}

inline void DeserialiseBlueprintItem(DeserialisationBuffer &buffer, BlueprintRoadStop &data, StringValidationSettings settings, bool with_specs = true)
{
	DeserialiseBlueprintOffset(buffer, data.offset, settings);
	buffer.Recv_generic_seq(settings, data.type, data.drive_through, data.dir, data.axis, data.roadtype, data.tramtype, data.station);
	if (with_specs) buffer.Recv_generic_seq(settings, data.spec.grfid, data.spec.local_id);
}

inline void SerialiseBlueprintItem(BufferSerialisationRef buffer, const BlueprintDock &data)
{
	SerialiseBlueprintOffset(buffer, data.offset);
	buffer.Send_generic_seq(data.dir, data.station);
}

inline void DeserialiseBlueprintItem(DeserialisationBuffer &buffer, BlueprintDock &data, StringValidationSettings settings)
{
	DeserialiseBlueprintOffset(buffer, data.offset, settings);
	buffer.Recv_generic_seq(settings, data.dir, data.station);
}

inline void SerialiseBlueprintItem(BufferSerialisationRef buffer, const BlueprintBuoy &data)
{
	SerialiseBlueprintOffset(buffer, data.offset);
}

inline void DeserialiseBlueprintItem(DeserialisationBuffer &buffer, BlueprintBuoy &data, StringValidationSettings settings)
{
	DeserialiseBlueprintOffset(buffer, data.offset, settings);
}


void SerialiseBlueprintItem(BufferSerialisationRef buffer, const Blueprint &data);
bool DeserialiseBlueprintItem(DeserialisationBuffer &buffer, Blueprint &data, StringValidationSettings settings);

inline void SerialiseBlueprintItem(BufferSerialisationRef buffer, const uint8_t &data)
{
	buffer.Send_generic_integer(data);
}

inline void DeserialiseBlueprintItem(DeserialisationBuffer &buffer, uint8_t &data, StringValidationSettings)
{
	buffer.Recv_generic_integer(data);
}

inline void SerialiseBlueprintItem(BufferSerialisationRef buffer, const BlueprintObject &data)
{
	SerialiseBlueprintOffset(buffer, data.offset);
	buffer.Send_generic_seq(data.spec.grfid, data.spec.local_id, data.view, data.views, data.w, data.h);
}

inline void DeserialiseBlueprintItem(DeserialisationBuffer &buffer, BlueprintObject &data, StringValidationSettings settings)
{
	DeserialiseBlueprintOffset(buffer, data.offset, settings);
	buffer.Recv_generic_seq(settings, data.spec.grfid, data.spec.local_id, data.view, data.views, data.w, data.h);
}

/** Write a length-prefixed blueprint vector to a command buffer. */
template <typename T>
inline void WriteBlueprintVector(BufferSerialisationRef buffer, const std::vector<T> &data)
{
	buffer.Send_generic_integer(data.size());
	for (const T &item : data) SerialiseBlueprintItem(buffer, item);
}

/** Read a length-prefixed blueprint vector from a command buffer; the length is capped against malicious data. */
template <typename T>
inline bool ReadBlueprintVector(DeserialisationBuffer &buffer, std::vector<T> &data, StringValidationSettings settings, bool with_specs = true)
{
	uint32_t count = 0;
	buffer.Recv_generic_integer(count);
	if (buffer.error) return false;
	count = std::min(count, MAX_BLUEPRINT_VECTOR_LENGTH);
	data.clear();
	data.reserve(count);
	for (uint32_t i = 0; i < count; i++) {
		T item{};
		if constexpr (std::is_same_v<T, BlueprintStationTile> || std::is_same_v<T, BlueprintRoadStop> || std::is_same_v<T, BlueprintSignal>) {
			DeserialiseBlueprintItem(buffer, item, settings, with_specs);
		} else {
			DeserialiseBlueprintItem(buffer, item, settings);
		}
		if (buffer.error) return false;
		data.push_back(item);
	}
	return true;
}

inline void SerialiseBlueprintItem(BufferSerialisationRef buffer, const BlueprintAirportTile &data)
{
	SerialiseBlueprintOffset(buffer, data.offset);
	buffer.Send_generic_seq(data.m4, data.m5, data.m6, data.m8);
}

inline void DeserialiseBlueprintItem(DeserialisationBuffer &buffer, BlueprintAirportTile &data, StringValidationSettings settings)
{
	DeserialiseBlueprintOffset(buffer, data.offset, settings);
	buffer.Recv_generic_seq(settings, data.m4, data.m5, data.m6, data.m8);
}

inline void SerialiseBlueprintItem(BufferSerialisationRef buffer, const BlueprintAirport &data)
{
	SerialiseBlueprintOffset(buffer, data.offset);
	buffer.Send_generic_seq(data.type, data.layout, data.rotation, data.air_type, data.w, data.h, data.station);
	WriteBlueprintVector(buffer, data.tiles);
}

inline void DeserialiseBlueprintItem(DeserialisationBuffer &buffer, BlueprintAirport &data, StringValidationSettings settings)
{
	DeserialiseBlueprintOffset(buffer, data.offset, settings);
	buffer.Recv_generic_seq(settings, data.type, data.layout, data.rotation, data.air_type, data.w, data.h, data.station);
	if (!ReadBlueprintVector(buffer, data.tiles, settings)) buffer.RaiseDeserialisationError();
}

inline void SerialiseBlueprint(BufferSerialisationRef buffer, const Blueprint &data)
{
	/* An impossible width distinguishes styled blueprints from legacy data,
	 * including when several blueprints are nested in a slot set. */
	buffer.Send_generic(uint16_t{UINT16_MAX - 1}); // Version 5, including explicit signal styles and complete objects.
	buffer.Send_generic_seq(data.width, data.height, data.name);
	WriteBlueprintVector(buffer, data.rail_tracks);
	WriteBlueprintVector(buffer, data.signals);
	WriteBlueprintVector(buffer, data.rail_depots);
	WriteBlueprintVector(buffer, data.tunnel_bridges);
	WriteBlueprintVector(buffer, data.roads);
	WriteBlueprintVector(buffer, data.road_depots);
	WriteBlueprintVector(buffer, data.canals);
	WriteBlueprintVector(buffer, data.locks);
	WriteBlueprintVector(buffer, data.ship_depots);
	WriteBlueprintVector(buffer, data.station_tiles);
	WriteBlueprintVector(buffer, data.road_stops);
	WriteBlueprintVector(buffer, data.docks);
	WriteBlueprintVector(buffer, data.buoys);
	WriteBlueprintVector(buffer, data.airports);
	WriteBlueprintVector(buffer, data.corner_heights);
	WriteBlueprintVector(buffer, data.objects);
}

inline bool DeserialiseBlueprint(DeserialisationBuffer &buffer, Blueprint &data, StringValidationSettings settings)
{
	buffer.Recv_generic(data.width);
	bool with_signal_specs = data.width == UINT16_MAX - 1;
	bool with_specs = data.width == UINT16_MAX || with_signal_specs;
	if (with_specs) buffer.Recv_generic(data.width);
	buffer.Recv_generic_seq(settings, data.height, data.name);
	bool ok = ReadBlueprintVector(buffer, data.rail_tracks, settings);
	ok &= ReadBlueprintVector(buffer, data.signals, settings, with_signal_specs);
	ok &= ReadBlueprintVector(buffer, data.rail_depots, settings);
	ok &= ReadBlueprintVector(buffer, data.tunnel_bridges, settings);
	ok &= ReadBlueprintVector(buffer, data.roads, settings);
	ok &= ReadBlueprintVector(buffer, data.road_depots, settings);
	ok &= ReadBlueprintVector(buffer, data.canals, settings);
	ok &= ReadBlueprintVector(buffer, data.locks, settings);
	ok &= ReadBlueprintVector(buffer, data.ship_depots, settings);
	ok &= ReadBlueprintVector(buffer, data.station_tiles, settings, with_specs);
	ok &= ReadBlueprintVector(buffer, data.road_stops, settings, with_specs);
	ok &= ReadBlueprintVector(buffer, data.docks, settings);
	ok &= ReadBlueprintVector(buffer, data.buoys, settings);
	ok &= ReadBlueprintVector(buffer, data.airports, settings);
	ok &= ReadBlueprintVector(buffer, data.corner_heights, settings);
	data.objects.clear();
	if (with_signal_specs) ok &= ReadBlueprintVector(buffer, data.objects, settings);
	return ok;
}

inline void SerialiseBlueprintItem(BufferSerialisationRef buffer, const Blueprint &data)
{
	SerialiseBlueprint(buffer, data);
}

inline bool DeserialiseBlueprintItem(DeserialisationBuffer &buffer, Blueprint &data, StringValidationSettings settings)
{
	return DeserialiseBlueprint(buffer, data, settings);
}

inline void BlueprintPasteData::Serialise(BufferSerialisationRef buffer) const
{
	SerialiseBlueprint(buffer, this->blueprint);
	buffer.Send_generic_seq(this->terraform_mode, this->height_offset);
}

inline bool BlueprintPasteData::Deserialise(DeserialisationBuffer &buffer, StringValidationSettings default_string_validation)
{
	if (!DeserialiseBlueprint(buffer, this->blueprint, default_string_validation)) return false;
	buffer.Recv_generic_seq(default_string_validation, this->terraform_mode, this->height_offset);
	return !buffer.error;
}

inline void BlueprintPasteData::fmt_format_value(struct format_target &buf) const
{
	buf.format("paste: {}x{}, terraform {}, height {}", this->blueprint.width, this->blueprint.height, to_underlying(this->terraform_mode), this->height_offset);
}

/* The undo payload reuses the blueprint serialization above: the parts a paste
 * built are shaped exactly like a blueprint, so no second format is needed. */

inline void BlueprintUndoData::Serialise(BufferSerialisationRef buffer) const
{
	SerialiseBlueprint(buffer, this->built);
	buffer.Send_generic(this->restore_terrain);
}

inline bool BlueprintUndoData::Deserialise(DeserialisationBuffer &buffer, StringValidationSettings default_string_validation)
{
	if (!DeserialiseBlueprint(buffer, this->built, default_string_validation)) return false;
	buffer.Recv_generic(this->restore_terrain);
	return !buffer.error;
}

inline void BlueprintUndoData::fmt_format_value(struct format_target &buf) const
{
	buf.format("undo: {}x{}, restore_terrain {}", this->built.width, this->built.height, this->restore_terrain);
}

/* Measure the serialized size of a payload, to reject pastes that would not fit a network packet. */
template <typename T>
inline size_t GetBlueprintSerialisedSize(const T &data)
{
	std::vector<uint8_t> scratch;
	BufferSerialisationRef buffer(scratch);
	data.Serialise(buffer);
	return scratch.size();
}

#endif /* BLUEPRINT_CMD_H */

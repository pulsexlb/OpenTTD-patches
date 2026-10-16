/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file airport_cmd.h Command definitions related to airports. */

#ifndef AIRPORT_CMD_H
#define AIRPORT_CMD_H

#include "command_type.h"
#include "air.h"
#include "air_type.h"
#include "direction_type.h"
#include "station_type.h"
#include "track_type.h"
#include "table/airporttile_ids.h"

CommandCost CmdChangeAirportTiles(DoCommandFlags flags, TileIndex start_tile, TileIndex end_tile, AirType air_type, AirportTileType air_tile_type, AirportTiles infra_type, DiagDirection direction, bool adding, bool diagonal);
CommandCost CmdAddRemoveAirportTiles(DoCommandFlags flags, TileIndex start_tile, TileIndex end_tile, bool adding, AirType at, StationID station_to_join, bool adjacent);
CommandCost CmdAddRemoveTracksToAirport(DoCommandFlags flags, TileIndex start_tile, TileIndex end_tile, AirType air_type, bool add, Track track);
CommandCost CmdChangeAirType(DoCommandFlags flags, TileIndex tile, AirType air_type);
CommandCost CmdAirportChangeTrackGFX(DoCommandFlags flags, TileIndex start_tile, TileIndex end_tile, AirType air_type, uint8_t gfx_index, bool diagonal);
CommandCost CmdAirportToggleGround(DoCommandFlags flags, TileIndex start_tile, TileIndex end_tile, AirType air_type, bool diagonal);

CommandCost CmdBuildAirport(DoCommandFlags flags, TileIndex tile, uint8_t airport_type, uint8_t layout, AirType air_type, DiagDirection rotation, StationID station_to_join, bool allow_adjacent);
CommandCost CmdOpenCloseAirport(DoCommandFlags flags, StationID station_id);

DEF_CMD_TUPLE(Commands::ChangeAirport,          CmdChangeAirportTiles,       CMD_AUTO,                CommandType::LandscapeConstruction, CmdDataT<TileIndex, AirType, AirportTileType, AirportTiles, DiagDirection, bool, bool>)
DEF_CMD_TUPLE(Commands::AddRemoveAirportTiles,  CmdAddRemoveAirportTiles,    CMD_AUTO,                CommandType::LandscapeConstruction, CmdDataT<TileIndex, bool, AirType, StationID, bool>)
DEF_CMD_TUPLE(Commands::AddRemoveTracksAirport, CmdAddRemoveTracksToAirport, CMD_AUTO,                CommandType::LandscapeConstruction, CmdDataT<TileIndex, AirType, bool, Track>)
DEF_CMD_TUPLE(Commands::ChangeAirType,          CmdChangeAirType,            CMD_AUTO,                CommandType::LandscapeConstruction, CmdDataT<AirType>)
DEF_CMD_TUPLE(Commands::AirportChangeTrackGfx,  CmdAirportChangeTrackGFX,    CMD_AUTO,                CommandType::LandscapeConstruction, CmdDataT<TileIndex, AirType, uint8_t, bool>)
DEF_CMD_TUPLE(Commands::AirportToggleGround,    CmdAirportToggleGround,      CMD_AUTO,                CommandType::LandscapeConstruction, CmdDataT<TileIndex, AirType, bool>)

DEF_CMD_TUPLE(Commands::BuildAirport,           CmdBuildAirport,             CMD_AUTO | CMD_NO_WATER, CommandType::LandscapeConstruction, CmdDataT<uint8_t, uint8_t, AirType, DiagDirection, StationID, bool>)
DEF_CMD_TUPLE_NT(Commands::OpenCloseAirport,    CmdOpenCloseAirport,         {},                      CommandType::RouteManagement,       CmdDataT<StationID>)


#endif /* AIRPORT_CMD_H */

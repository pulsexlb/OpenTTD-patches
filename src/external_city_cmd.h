/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef EXTERNAL_CITY_CMD_H
#define EXTERNAL_CITY_CMD_H
#include "command_type.h"
#include "external_city_type.h"
#include "depot_type.h"
#include "direction_type.h"
DEF_CMD_TUPLE_NT(Commands::ManageExternalCity, CmdManageExternalCityRegion, {}, CommandType::OtherManagement, CmdDataT<DepotID, ExternalCityID, ExternalCityOperation, uint16_t, std::string, ExternalBoundaryID, DiagDirection, uint16_t, uint16_t>)
inline CommandCost CmdManageExternalCity(DoCommandFlags flags, DepotID portal, ExternalCityID city, ExternalCityOperation operation, uint16_t minutes, const std::string &name)
{
	return CmdManageExternalCityRegion(flags, portal, city, operation, minutes, name, ExternalBoundaryID::Invalid(), DiagDirection::NE, 0, 0);
}
#endif

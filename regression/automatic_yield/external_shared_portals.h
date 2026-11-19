/* Set up two foreign portals, then exercise the normal vehicle-order permission check. */
#include "tile_cmd.h"
#include "water_map.h"
#include "company_func.h"
#include "company_base.h"

inline void SetupSharedExternalPortals(Vehicle *vehicle, TileIndex entry, TileIndex exit)
{
 const CompanyID infrastructure_owner = DoStartupNewCompany(DSNC_NONE)->index;
 for (TileIndex tile : {entry, exit}) {
  assert(GetTileOwner(tile) == vehicle->owner);
  if (vehicle->type == VehicleType::Ship) ChangeTileOwner(GetOtherShipDepotTile(tile), vehicle->owner, infrastructure_owner);
  ChangeTileOwner(tile, vehicle->owner, infrastructure_owner);
  assert(GetTileOwner(tile) == infrastructure_owner);
 }
 auto &sharing = _settings_game.economy.infrastructure_sharing[to_underlying(vehicle->type)];
 sharing = false;
 Order order;
 order.MakeGoToDepot(GetDepotIndex(entry), {OrderDepotTypeFlag::PartOfOrders}, vehicle->IsGroundVehicle() ? ONSF_NO_STOP_AT_INTERMEDIATE_STATIONS : ONSF_STOP_EVERYWHERE);
 assert(CmdInsertOrder({}, InsertOrderCmdData(vehicle->index, VehicleOrderID{0}, order)).Failed());
 sharing = true;
 assert(CmdInsertOrder({}, InsertOrderCmdData(vehicle->index, VehicleOrderID{0}, order)).Succeeded());
}

inline void CheckExternalReturnSharing(Vehicle *vehicle, VehicleOrderID index, DepotID exit)
{
 auto &sharing = _settings_game.economy.infrastructure_sharing[to_underlying(vehicle->type)];
 sharing = false;
 assert(CmdModifyOrder({}, OrderTargetType::Vehicle, vehicle->index.base(), index, MOF_EXTERNAL_RETURN_DEPOT, exit.base(), INVALID_CARGO, {}).Failed());
 sharing = true;
}

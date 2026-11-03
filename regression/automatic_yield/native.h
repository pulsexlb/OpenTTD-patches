#include "rail_cmd.h"
#include "station_cmd.h"
#include "station_map.h"
#include "depot_map.h"
#include "order_cmd.h"
#include "vehicle_cmd.h"
#include "train_cmd.h"
#include "town_cmd.h"
#include "signs_cmd.h"
#include "tracerestrict_overtaking.h"
#include "tracerestrict_early_route.h"
#include "pathfinder/yapf/yapf.h"
#include "pbs.h"
#include "rail_map.h"
#include "engine_base.h"
#include "newgrf_config.h"
#include "signal_func.h"
#include "window_func.h"
void AutoYieldDemoTick();
void TraceRestrictCheckRefreshSignals(const TraceRestrictProgram *, size_t, TraceRestrictProgramActionsUsedFlags);

extern bool ValidateVehicleTileHash(const Vehicle *v);

#include "misc_cmd.h"

#include "core/random_func.hpp"
#include "core/checksum_func.hpp"
#include "tracerestrict_cmd.h"

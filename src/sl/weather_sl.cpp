/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file weather_sl.cpp Code handling saving and loading of the (cosmetic) weather state. */

#include "../stdafx.h"

#include "saveload.h"

#include "../cheat_type.h"
#include "../date_func.h"
#include "../weather.h"

#include "../safeguards.h"

static const NamedSaveLoad _weather_desc[] = {
	NSL("auto_raining", SLEG_VAR(_weather_state.auto_raining, SLE_BOOL)),
	NSL("seed",         SLEG_VAR(_weather_state.seed, SLE_UINT32)),
	NSL("period_start", SLEG_VAR(_weather_state.period_start, SLE_INT64)),
	/* The forced-weather sandbox cheat, persisted like the other cheats. */
	NSL("rain_mode",    SLEG_VAR(_cheats.rain_mode, SLE_UINT8)),
	NSL("rain_used",    SLEG_VAR(_cheats.rain.been_used, SLE_BOOL)),
};

static void Save_WTHR()
{
	SlSaveTableObjectChunk(_weather_desc);
}

static void Load_WTHR()
{
	if (!SlIsTableChunk()) {
		SlSkipChunkContents();
		return;
	}
	SlLoadTableObjectChunk(_weather_desc);

	/* Jump the shading straight to the loaded weather, without a fade. */
	WeatherSnapShade();
}

extern const ChunkHandler weather_chunk_handlers[] = {
	{ 'WTHR', Save_WTHR, Load_WTHR, nullptr, nullptr, CH_TABLE },
};

extern const ChunkHandlerTable _weather_chunk_handlers(weather_chunk_handlers);

/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file weather.h Functions related to weather. */

#ifndef WEATHER_H
#define WEATHER_H

/** Forced weather state chosen by the sandbox cheat. */
enum class RainForcing : uint8_t {
	Auto = 0,   ///< Follow the automatic (random) weather.
	Raining = 1,///< Force rain all the time.
	Sunny = 2,  ///< Force clear skies all the time.
};

/**
 * State of the (cosmetic) automatic weather simulation.
 * This is part of the game state and is persisted in savegames.
 */
struct WeatherState {
	bool auto_raining;    ///< Currently in a rainy period.
	uint32_t seed;        ///< Deterministic RNG state; seeded from the map generation seed.
	int64_t period_start; ///< Start tick (StateTicks base) of the current weather period.
};

extern WeatherState _weather_state;

/**
 * Current world shading level, 0 = clear skies (no shading) to RAIN_SHADE_LEVELS
 * (fully darkened). Changed gradually by WeatherTick; read when drawing viewports.
 */
extern uint8_t _rain_shade_level;

/**
 * Get the shading nominator (of 256) of the current level, i.e. the factor the
 * colour channels are scaled with. Only meaningful when _rain_shade_level > 0.
 */
uint8_t RainShadeNom();

/** Seed used by the (local, cosmetic) weather randomness. */
uint32_t RainSeed();

/**
 * Fade progress of the rain visuals, 0 (no rain) to 255 (fully raining).
 * Follows _rain_shade_level, so the overlay fades in/out with the shading.
 */
uint8_t RainShadeProgress();

void WeatherTick();
bool IsRainActive();

/** Jump the shading level instantly to the current weather's target (e.g. after loading a savegame). */
void WeatherSnapShade();

#endif /* WEATHER_H */

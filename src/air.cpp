/*
 * This file is part of OpenTTD.
 * OpenTTD is free software; you can redistribute it and/or modify it under the terms of the GNU General Public License as published by the Free Software Foundation, version 2.
 * OpenTTD is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details. You should have received a copy of the GNU General Public License along with OpenTTD. If not, see <https://www.gnu.org/licenses/old-licenses/gpl-2.0>.
 */

/** @file air.cpp Implementation of air specific functions. */

#include "stdafx.h"
#include "air.h"
#include "company_func.h"

/**
 * Validate functions for air building.
 * @param air the airtype to check.
 * @return true if the current company may build the air.
 */
bool ValParamAirType(const AirType air)
{
	return air < AIRTYPE_END;
}

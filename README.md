# OpenTTD Patch Pack

[![License: GPL v2](https://img.shields.io/badge/License-GPL%20v2-blue.svg)](https://www.gnu.org/licenses/old-licenses/gpl-2.0.en.html)
[![GitHub release (latest by date)](https://img.shields.io/github/v/release/pulsexlb/OpenTTD-patches)](https://github.com/pulsexlb/OpenTTD-patches/releases)
[![GitHub all releases](https://img.shields.io/github/downloads/pulsexlb/OpenTTD-patches/total)](https://github.com/pulsexlb/OpenTTD-patches/releases)
[![GitHub issues](https://img.shields.io/github/issues/pulsexlb/OpenTTD-patches)](https://github.com/pulsexlb/OpenTTD-patches/issues)
[![GitHub pull requests](https://img.shields.io/github/issues-pr/pulsexlb/OpenTTD-patches)](https://github.com/pulsexlb/OpenTTD-patches/pulls)
[![Ask DeepWiki](https://deepwiki.com/badge.svg)](https://deepwiki.com/pulsexlb/OpenTTD-patches)

This patch pack builds on top of [JGR's Patch Pack](https://github.com/JGRennison/OpenTTD-patches/) and adds three gameplay features that extend vanilla OpenTTD and JGRPP.

## Train decoupling

Trains can be decoupled during gameplay, allowing shunting, partial unloading, or splitting a consist without sending the whole train to a depot. This feature is based on the [YPS Decouple branch](https://github.com/Palo123/OpenTTD-YPS/tree/Decouple) by Palo123.

## Modular airports

Instead of placing a single pre-designed airport, players can build airports tile by tile. Runways, taxiways, aprons, terminals, and hangars can be placed and connected freely. This feature is based on the [Multitile Airports branch](https://github.com/J0anJosep/OpenTTD/tree/MultitileAirports) by J0anJosep.

## Depot control and station servicing

When enabled in the advanced settings, trains will not path to depots during normal operation. A train that enters a depot will automatically stop and remain stopped instead of continuing its schedule. Optionally, trains can be allowed to receive servicing at stations, reducing the need to visit a depot for routine maintenance. This prevents depots from being used as unlimited holding pens or waypoints.

## License

This project is licensed under the GPL v2. All derivative works must also be distributed under GPL v2.

Contributions are welcome. Please open an issue or pull request on GitHub.

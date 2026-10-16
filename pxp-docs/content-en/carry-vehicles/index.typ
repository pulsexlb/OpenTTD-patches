#title[Vehicle Transport]

*Note: the vehicle transport feature originates from the #link("https://github.com/Babel-TTT/OpenTTD-patches/tree/feature/road-veh-transport/")[Road Veh Transport patch]; px-patch is based on this patch, with optimizations and feature improvements.*

Turn on the `Vehicle transport` option in the settings to transport vehicles in the game.

#figure(
  image("../../static/images/veh-carry-setting.png", width: 80%),
)

= Simple vehicle transport orders

A vehicle can be transported simply by setting `Wait to be transported` and `Be unloaded here`.

#figure(
  image("../../static/images/road-veh-carry-order.png", width: 80%),
)

For a carrier vehicle, you first need to refit it to vehicle transport.

#figure(
  image("../../static/images/carry-veh-refit.png", width: 40%),
)

For vehicles refitted to vehicle transport, the orders added by default are all Load vehicles / Unload vehicles.

#figure(
  image("../../static/images/carry-veh-order.png", width: 80%),
)

= Restrictions
== Weight restriction

A carrier vehicle cannot transport vehicles without limit; it is restricted by the total weight. When an aircraft transports road vehicles, the program simply calculates whether the road vehicle still fits into the aircraft; if not, the vehicle will not be transported. When trains or ships transport road vehicles, the program tries to put the road vehicles one by one into a wagon/hold until a suitable wagon/hold is found. When a ship transports trains, the program tries to put every wagon into a hold one by one; the transport is only possible when all locomotives have been placed into holds (they may be different holds).

== Unloading station restriction

For road vehicles, buses are not allowed to enter goods stations, trucks are not allowed to enter bus stations, and articulated vehicles are not allowed to enter bay-type stations (the same as the road vehicle station requirements in vanilla OpenTTD).

For trains, the rail type must match, and the platform length must be greater than the train length.

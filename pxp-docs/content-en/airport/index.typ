#title[Modular Airports]

*Note: the train coupling and decoupling features originate from the #link("https://github.com/J0anJosep/OpenTTD/tree/MultitileAirports")[OpenTTD Multitile Airport patch]; px-patch ports this patch to the current version and improves upon its features.*

= Enabling the modular airport feature
The modular airport feature is not enabled by default; you need to turn on the `Allow building customized airports` option in the settings.

#figure(
  image("../../static/images/custom-airport-setting.png", width: 80%),
)

Once this feature is enabled, you will see a drop-down menu in the airport building toolbar and be able to build custom airports.

= Defining an airport area
Before building a custom airport, you need to define an airport area through the construction menu.

#figure(
  image("../../static/images/airport-area.png", width: 80%),
)

= Aircraft takeoff and landing restrictions
If you want the airport to handle conventional (taxiing) aircraft, you must build at least one runway that *allows landing*, plus one parking position. If you want the airport to handle VTOL aircraft, you must build at least one apron.

= Defining taxiways
When taxiing on the ground, aircraft taxi via taxiways. Taxiways need to be built, similar to railway tracks; use taxiways to connect runways, aprons, hangars, and so on.

#figure(
  image("../../static/images/airport-tracks.png", width: 80%),
)

Aircraft are allowed to make right-angle and sharp turns, but their speed will be affected.

= Airport landscaping features
After building aprons and runways and drawing the taxiways, aircraft should already be able to use the airport normally. However, you can also use some landscaping features, such as changing the ground graphics:

#figure(
  image("../../static/images/airport-change-graphic.png", width: 80%),
)

Building waiting rooms:

#figure(
  image("../../static/images/airport-cargo-catchment.png", width: 80%),
)

Building airport facilities:

#figure(
  image("../../static/images/airport-infra.png", width: 80%),
)

#title[Vehicles Running Outside the Map]

Trains, road vehicles, aircraft, and ships all support running outside the map, but the way to enable it differs.

= Rail and road

Rail and road are fairly simple to create: just lay the railway/road perpendicular to the map boundary, then click the entrance when adding the vehicle's orders.

#figure(
  image("../../static/images/ext-connect-train-bus.png", width: 80%),
)

= Air

In an aircraft's orders, choose `Go to external city` from the `Go to` drop-down menu to add an order flying to an external city, then select an external city.

#figure(
  image("../../static/images/ext-connect-air.png", width: 60%),
)

= Water

In the water construction toolbar, choose `Sea route`, then click ocean on the map boundary to create a water entrance leading outside the map. Finally, click the entrance you just created when adding the ship's orders.

#figure(
  image("../../static/images/ext-connect-water.png", width: 60%),
)

= Returning to the map through a different entrance

By default, when a vehicle returns to the map from outside, it returns through the original entrance. However, sometimes players do not want the vehicle to return through the original entrance (for example, with one-way railways). In that case, in the `Manage commands` drop-down menu of the Go to external city order, change `Return through the original entrance` to `Choose exit on map...`, and click a map exit.

#figure(
  image("../../static/images/ext-connect-return.png", width: 80%),
)

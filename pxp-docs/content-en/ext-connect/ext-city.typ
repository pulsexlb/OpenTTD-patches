#import "@preview/letmeout:0.2.0": *

#title[External Cities and Boundaries]

= The external cities window

#figure(
  image("../../static/images/ext-connect-window-menu.png", width: 40%),
)

Choose `External cities and boundaries` from the map drop-down menu to open the external cities window.


#figure(
  image("../../static/images/ext-connect-window.png", width: 80%),
)

= One-way distance, city stop time, and city names

These are the basic parameters of an external city. The `one-way distance` and the `city stop time` affect the time spent travelling and stopping at the station outside the map.

These three parameters can only be modified by one company; once the first company has modified them, only that company can modify them. It is recommended that the admin company sets them.

= Cargo, passengers, and industries

This window shows the external city's passengers, industries, cargo status, and other information.

#figure(
  image("../../static/images/ext-connect-cargo1.png", width: 80%),
)
#figure(
  image("../../static/images/ext-connect-cargo2.png", width: 80%),
)


= Viewing off-map vehicles

In the off-map vehicles window, you can view the list of vehicles currently running outside the map, their exit and entrance, the remaining time, and other information.

#figure(
  image("../../static/images/ext-connect-vehicle-outside-window.png", width: 80%),
)

= Editing city boundaries

Note: this feature is only available after turning on `Allow players to edit external city boundaries` in the settings. After editing, the entrances of other companies may become "Unassigned city". It is recommended to turn this on only temporarily while the server admin edits the map, and turn it off once the server goes live.

After clicking the `Edit city boundaries...` button, draw new city boundary segments on the map edge, or delete existing segments; a city may have multiple boundary segments.

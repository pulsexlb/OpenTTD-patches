#title[Settings]

#figure(
  image("../../static/images/ext-connect-settings.png", width: 80%),
)

= Enabling external connections

To use the external connections feature, you need to enable the `External connections` option in the game.

= External city economy options

This option determines the production and cargo-accepting behavior of external cities. There are currently two choices.

== Industry supply and demand

The city profile is determined by the enabled industries. Industry outputs become the city's export cargo, industry inputs become the city's demanded cargo; passengers/mail are produced and accepted according to town rules. Delivering cargo into the city feeds the industry recipes; output is limited by the recipes, stock limits, and the production cycle.

== Reciprocal trade

The city becomes a fixed exchange market. The virtual city randomly generates 3 demanded cargoes and 3 supplied cargoes. After delivering the demanded cargo, the player earns the city's `Shared industrial collection allowance`, and can then exchange the allowance for the supplied cargo.

= Default one-way external distance

The distance from an automatically generated city to the map edge.

= Default external stop

The city stop time of automatically generated cities, in minutes.

= Allow players to edit external city boundaries

Whether players may modify the boundaries of external cities. When enabled, players can edit the extent of external cities on the map boundary in game.

Once this option is enabled, the external city boundaries modified by one company may turn the entrances of other companies into "Unassigned city". It is recommended to turn this option on only temporarily while the server admin edits the map, and turn it off once the server goes live.

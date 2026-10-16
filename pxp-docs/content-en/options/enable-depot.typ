#title[Ban Trains from Waiting in Depots and Station Servicing]
Since depots can swallow and spit out trains without limit, which breaks game balance, px-patch adds setting options to control whether trains are allowed to wait in depots temporarily, and whether trains are allowed to be serviced at stations:

#figure(
  image("../../static/images/ban-depot.png", width: 80%),
)

Once this option is enabled, train pathfinding will treat depots as unreachable (unless the train is explicitly ordered to go to a depot), and once a train enters a depot, its movement will always be stopped.

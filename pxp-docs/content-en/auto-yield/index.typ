#import "../../shared/auto-yield-diagrams.typ": flow, layout

#title[Automatic Yielding Path Restriction]

px-patch provides a simpler automatic yielding path restriction feature, allowing you to easily create sidings so that faster trains can pass sooner.

Automatic yielding is a signal path restriction that also uses the slot (token) feature. Before using it, please make sure you are already familiar with JGRPP's path restrictions (programmable signals) and the slot feature.

= Enabling automatic yielding

To use automatic yielding, you need to turn on the `Automatic yielding through trace restrictions` option in the game settings.

#figure(
  image("../../static/images/auto-yield-enable.png", width: 80%),
)

Before using automatic yielding, you need to prepare a `slot` for the siding.

= How automatic yielding works

The basic principle of the automatic yielding option is: before passing the siding (at a greater distance), an `approach detector` (registering) signal is set up; this signal claims a slot for all passing trains, and sets information such as the speed difference at which trains on this siding should yield. After passing this approach detector, the train keeps checking whether there are trains that simultaneously hold the configured slot and are faster than it and meet the speed difference conditions; if there are, the `main-line guard` signal is set to disallow passage for this train, forcing it into the siding (or platform), and the `siding exit` signal of the siding (or platform) also checks in real time whether there are still trains that need to yield; if so, the train will be held there. After the end of the siding, a `slot release` signal is set up; it returns the slot and cleans up the data, completing the yielding.

The diagram below shows the railway layout and the positions and roles of the four signals (①—④):

#figure(
  layout("en"),
  caption: [Automatic yielding layout and signal positions],
)

The decision flow of a yielding train is as follows:

#figure(
  flow("en"),
  caption: [Automatic yielding decision flow],
)

#figure(
  image("../../static/images/auto-yield.png", width: 100%),
)

= Automatic yielding signals explained

== Approach detector signal

#figure(
  image("../../static/images/auto-yield-approach.png", width: 80%),
)

The approach detector path restriction is applied some distance before the siding, to register trains and set the yielding parameters.

- *Speed difference:* set the condition on the speed difference at which trains should yield.
- *Detection cap:* set the cap for the speed difference detection. For example, with the cap set to 160 km/h, the speed difference between a 160 km/h train and a 300 km/h train is 0%.
- *Slot:* set the slot of the siding; all passing trains will claim this slot.
- *Slot registration timing:* when trains register the slot, the same as the slot claiming path restriction.

== Main-line guard signal

#figure(
  image("../../static/images/auto-yield-main-line.png", width: 80%),
)

The main-line guard signal is placed on the main line, to restrict trains that need to yield from passing. The slot used for the siding needs to be set.

== Siding exit signal

#figure(
  image("../../static/images/auto-yieldsiding-exit.png", width: 80%),
)

The siding exit signal is placed on the siding, and comes in two types: `Ordinary siding` and `Platform siding`. If there is a platform on the siding, choose `Platform siding`; otherwise choose `Ordinary siding`.

Except for trains that need to enter the station, trains will preferentially yield on the `Ordinary siding`. If there is a platform on the siding track but `Ordinary siding` is set, problems such as incorrect behavior of trains stopped at the platform may occur.

== Slot release signal

#figure(
  image("../../static/images/auto-yield-release-slot.png", width: 80%),
)

Set after the siding merges back into the main line. After a train passes, it returns the slot and cleans up the data, completing the yielding.

*⚠️Note: currently, between claiming the yielding slot and returning it, trains should not perform reversing operations; otherwise the trains may become jammed.*

#title[Simple Train Coupling]

= Introduction to train coupling orders
Trains use two different order entries to perform coupling: `Wait for couple` and `Go to couple`. Both can be selected and added from the `Go to` drop-down menu.

#figure(
  image("../../static/images/couple-orders.png", width: 40%),
)

Coupling can only happen inside a station.

== Wait for couple
If a train's order is Wait for couple, the train will wait in place for other trains to couple with it.

#figure(
  image("../../static/images/wait-couple.png", width: 80%),
)

This order only works while the train is within a platform. When the train is waiting for coupling outside a platform, or the train is longer than the platform, it will show `Can't couple outside the station`

#figure(
  image("../../static/images/cant-couple-outside-st.png", width: 50%),
)

== Go to couple
When a train's order is Go to couple, it will by default search *the whole map* for all trains waiting for coupling. Once a target is found, the train will head to the waiting train's position and slow down to couple.

You can also add restrictions to the Go to couple order to restrict which trains it couples with. See also #link("./couple-operation.html")[Train Coupling Restrictions]

*Note: if a train cannot find any waiting train that meets the requirements, the train will be set to the `stuck` state. Simply put, the train will be blocked by any signal until a matching train appears*

You can click the `Diagnostics` button to see why the train is stuck instead of going to couple. When the train has found a target, it will show the coupling target and other information:

#figure(
  image("../../static/images/couple-dia.png", width: 50%),
)

= Train coupling requirements
To ensure trains behave normally after decoupling, the game will not perform coupling in the following cases:
+ The coupled train does not satisfy start stop (often used in newgrf)

Currently, the game does not give any feedback when coupling fails.

*Note: after coupling, dual-headed locomotives may reverse the order of some vehicles. This depends on the coupling direction and only occurs with dual-headed locomotives*

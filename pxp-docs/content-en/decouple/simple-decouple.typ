#title[Simple Train Decoupling]

= Creating a decouple order
Train decoupling means a train splits into two parts after stopping at a station; you can control the number of vehicles to decouple and the orders of both trains afterwards. To add a decouple order, first add an order entry to go to the station, then select the decouple option, and the train will decouple at that station.
#figure(
  image("../../static/images/add-decouple-order.png", width: 80%),
)
Select the decouple order entry to edit the decoupling information.
#figure(
  image("../../static/images/decouple-editor.png", width: 80%),
)
In the decouple order, you can edit the orders of both parts after decoupling, as well as the number of vehicles to decouple.
*Note: when the number of vehicles to decouple is 0, the leading locomotive will be decoupled automatically*

= Train decoupling requirements
To ensure trains behave normally after decoupling, the game will not perform the decoupling in the following cases:
+ The train does not fully stop within the platform (train length > platform length)
+ The decoupling position is inside a dual-headed locomotive
+ The trains after decoupling do not satisfy the start stop rule (the train cannot start, usually newgrf trains)

Currently, the game does not give any feedback when decoupling fails.

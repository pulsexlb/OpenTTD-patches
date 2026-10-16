#title[Vehicles Using Custom Order Lists]

= Using them directly in an order
In a vehicle's order, you can directly select "Execute order list" and pick a custom order list. When this entry is executed, the train's order will be set to the order of that order list.

#figure(
  image("../../static/images/execute-orderlist.png", width: 80%),
)

= Using custom order lists after decoupling
See also: #link("../decouple/decouple-operation.html")[Choosing the orders after decoupling]

= What actually happens when a vehicle executes a custom order list
The "Execute order list" entry only executes the target order list *once*, then jumps back to the *vehicle's original order list*. Therefore, in this order, the second "go to depot" entry can still be executed normally:

#figure(
  image("../../static/images/execute-orderlist-out.png", width: 80%),
)

When a vehicle is currently executing a custom order list, you can also select the last entry of the order and click "Exitexecution" to jump back to the vehicle's original order:

#figure(
  image("../../static/images/execute-orderlist-exit.png", width: 80%),
)

If the vehicle's original order only contains a single "Execute order list" entry, this is equivalent to executing the custom order list repeatedly.

*Note: when a custom order list finishes executing, it jumps back to the vehicle's most original order list, not the parent order list. That is, if a custom order list contains an "Execute order list" entry that executes another order list, once the other order list finishes, it will jump back to the vehicle's original order list rather than this custom order list*

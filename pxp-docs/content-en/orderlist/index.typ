#title[Custom Order Lists]
In vanilla OpenTTD, an order list is always bound to vehicles: a vehicle can only have one order list, and multiple vehicles may share the same order list. In px-patch, players can create order lists without creating vehicles, and use custom order lists on vehicles.

= The order list manager window
From the map drop-down menu, you can open the order list manager window:

#figure(
  image("../../static/images/orderlist-manager.png", width: 80%),
)

In the order list manager window, you can create, delete, and rename order lists, or make an order list publicly visible.

*⚠️Note: once an order list has been made publicly visible, it cannot be set back to private*

*⚠️Note: an order list cannot be deleted while it is in use by vehicles*

#title[Editing Order Lists]

= Editing an order list
== Editing orders

#figure(
  image("../../static/images/edit-order.png", width: 80%),
)

The editing interface for custom order lists is basically identical to the order list editor of vehicles in vanilla OpenTTD, and works the same way.
However, since custom orders are not bound to vehicles, a custom order list can contain order entries of any type, without the restrictions imposed by the vehicle type.
If the order list a vehicle is executing contains entries the vehicle cannot perform, the vehicle will fail to find a route or silently skip them.

== Editing the timetable
#figure(
  image("../../static/images/edit-timetable.png", width: 80%),
)

The timetable editor for custom orders is basically identical to the timetable editor of vehicles in vanilla OpenTTD, works the same way, and the timed order feature works as usual.
However, since custom orders are not bound to vehicles, the timetable cannot be set to "Automatic" or "Auto-fill".

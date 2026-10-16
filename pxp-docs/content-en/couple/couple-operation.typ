#title[Train Coupling Operations]

= Train coupling restrictions
#figure(
  image("../../static/images/goto-couple-order.png", width: 80%),
)

Without coupling restrictions, the train will by default search *the whole map* for all trains waiting for coupling. The following restrictions are available:
- *Couple empty / Couple full:* restrict coupling to empty or fully loaded trains
- *Cargo type:* restrict coupling to trains that can load the specified cargo
- *Slot:* restrict coupling to trains holding the specified slot
- *Units couple:* restrict coupling to trains with a certain number of units
- *Station:* restrict coupling to trains waiting for coupling at a specified station

= Orders after coupling
By default, the coupled train will use the orders of the train that was heading to couple, but you can also tick the `Use the waiting train's schedule` option in `Manage orders`

#figure(
  image("../../static/images/use-wait-train-schedule.png", width: 50%),
)

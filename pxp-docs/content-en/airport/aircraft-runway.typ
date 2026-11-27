#title[Aircraft Runway Length Limits]

The game allows limiting the minimum takeoff/landing runway length of aircraft through the aircraft's maximum speed and aircraft type. Aircraft cannot land at an airport whose runway is too short.

= Settings

#figure(
  image("../../static/images/runway-limit-settings.png", width: 80%),
)

- *Speed to runway length factor:* the larger the factor, the shorter the runway required for the same maximum speed. When this value is 0, all aircraft can take off from and land on runways of any length.
- *Runway length factor for small aircraft:* on top of the result of the base factor calculation, controls an extra factor for the runway length of small aircraft.
- *Runway length factor for large aircraft:* on top of the result of the base factor calculation, controls an extra factor for the runway length of large aircraft.

= Aircraft runway length restrictions

When buying an aircraft, and in the aircraft's details window, you can see the aircraft's required runway length.

#figure(
  image("../../static/images/runway-limit-runway-too-short.png", width: 80%),
)

When you try to add an airport whose runway is too short, you will be told `Runway too short`

= Leaving the runway early

When landing, an aircraft may leave the runway early once it has travelled the minimum required runway length. Building a taxiway that overlaps or is perpendicular to the runway lets the aircraft leave the runway early instead of travelling the full distance.

#figure(
  image("../../static/images/runway-limit-exit-runway.png", width: 80%),
)

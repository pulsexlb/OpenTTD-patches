#!/usr/bin/env -S typst compile --features bundle,html --format bundle
#import "@preview/haita:0.4.0": *
#book(
  html-renderer: new-hamber.html-renderer,
  base-url: "https://pulsexlb.github.io/OpenTTD-PXPatch-Docs/",
  tree: (
    [= PX-Patch Docs
    OpenTTD - http://www.openttd.org/ - with additional patches],
    chapter("index", content: include "index.typ"),
    chapter(
      "en/",
      content: include "content-en/index.typ",
      children: (
        chapter(
          "en/dechoule",
          content: include "content-en/decouple/index.typ",
          children: (
            chapter(
              "en/decouple/simple-decouple",
              content: include "content-en/decouple/simple-decouple.typ",
            ),
            chapter(
              "en/decouple/decouple-operation",
              content: include "content-en/decouple/decouple-operation.typ",
            ),
            chapter(
              "en/decouple/decouple-passenger",
              content: include "content-en/decouple/decouple-passenger.typ",
            ),
          )
        ),
        chapter(
          "en/couple",
          content: include "content-en/couple/index.typ",
          children: (
            chapter(
              "en/couple/simple-couple",
              content: include "content-en/couple/simple-couple.typ",
            ),
            chapter(
              "en/couple/couple-operation",
              content: include "content-en/couple/couple-operation.typ",
            ),
            chapter(
              "en/couple/cross-company-couple",
              content: include "content-en/couple/cross-company-couple.typ",
            ),
          )
        ),
        chapter(
          "en/airport",
          content: include "content-en/airport/index.typ",
        ),
        chapter(
          "en/orderlist",
          content: include "content-en/orderlist/index.typ",
          children: (
            chapter(
              "en/orderlist/orderlist-edit",
              content: include "content-en/orderlist/orderlist-edit.typ",
            ),
            chapter(
              "en/orderlist/orderlist-vehicle",
              content: include "content-en/orderlist/orderlist-vehicle.typ",
            ),
          )
        ),
        chapter(
          "en/veh-carry",
          content: include "content-en/carry-vehicles/index.typ",
          children: (
            chapter(
              "en/veh-carry/orders",
              content: include "content-en/carry-vehicles/orders.typ",
            ),
            chapter(
              "en/veh-carry/cross-company",
              content: include "content-en/carry-vehicles/cross-comp.typ",
            ),
          )
        ),
        chapter(
          "en/other-options",
          content: include "content-en/options/index.typ",
          children: (
            chapter(
              "en/other-options/enable-depot",
              content: include "content-en/options/enable-depot.typ",
            ),
            chapter(
              "en/other-options/invite-code-alive",
              content: include "content-en/options/invite-code-alive.typ",
            ),
            chapter(
              "en/other-options/rainy",
              content: include "content-en/options/rainy.typ",
            ),
          )
        ),
      ),
    ),
    divider(),
    chapter(
      "zh/",
      content: include "content-zh/index.typ",
      children: (
        chapter(
          "zh/dechoule",
          content: include "content-zh/decouple/index.typ",
          children: (
            chapter(
              "zh/decouple/simple-decouple",
              content: include "content-zh/decouple/simple-decouple.typ",
            ),
            chapter(
              "zh/decouple/decouple-operation",
              content: include "content-zh/decouple/decouple-operation.typ",
            ),
            chapter(
              "zh/decouple/decouple-passenger",
              content: include "content-zh/decouple/decouple-passenger.typ",
            ),
          )
        ),
        chapter(
          "zh/couple",
          content: include "content-zh/couple/index.typ",
          children: (
            chapter(
              "zh/couple/simple-couple",
              content: include "content-zh/couple/simple-couple.typ",
            ),
            chapter(
              "zh/couple/couple-operation",
              content: include "content-zh/couple/couple-operation.typ",
            ),
            chapter(
              "zh/couple/cross-company-couple",
              content: include "content-zh/couple/cross-company-couple.typ",
            ),
          )
        ),
        chapter(
          "zh/airport",
          content: include "content-zh/airport/index.typ",
        ),
        chapter(
          "zh/orderlist",
          content: include "content-zh/orderlist/index.typ",
          children: (
            chapter(
              "zh/orderlist/orderlist-edit",
              content: include "content-zh/orderlist/orderlist-edit.typ",
            ),
            chapter(
              "zh/orderlist/orderlist-vehicle",
              content: include "content-zh/orderlist/orderlist-vehicle.typ",
            ),
          )
        ),
        chapter(
          "zh/veh-carry",
          content: include "content-zh/carry-vehicles/index.typ",
          children: (
            chapter(
              "zh/veh-carry/orders",
              content: include "content-zh/carry-vehicles/orders.typ",
            ),
            chapter(
              "zh/veh-carry/cross-company",
              content: include "content-zh/carry-vehicles/cross-comp.typ",
            ),
          )
        ),
        chapter(
          "zh/other-options",
          content: include "content-zh/options/index.typ",
          children: (
            chapter(
              "zh/other-options/enable-depot",
              content: include "content-zh/options/enable-depot.typ",
            ),
            chapter(
              "zh/other-options/invite-code-alive",
              content: include "content-zh/options/invite-code-alive.typ",
            ),
            chapter(
              "zh/other-options/rainy",
              content: include "content-zh/options/rainy.typ",
            ),
          )
        ),
      ),
    ),
    // you can also add arbitrary content
    [#link("https://github.com/pulsexlb/OpenTTD-patches")[GitHub]#linebreak()PX-Patch Docs - CC BY-NC-SA By PulseX#linebreak()Made with Haita.],
    // you can add more chapters afterwards.
  ),
)

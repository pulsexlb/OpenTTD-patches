// Native-Typst schematic diagrams for the automatic yielding documentation.
// Drawn with plain Typst primitives (place/line/rect/circle/curve) so that both
// the PDF/bundle and the HTML export render them without extra packages.

// Signal accent colors (shared between the layout and the flow diagram).
#let _c-approach = rgb("#1f6feb") // ① approach detector: blue
#let _c-guard = rgb("#1a7f37") // ② main-line guard: green
#let _c-exit = rgb("#eab308") // ③ siding exit: yellow
#let _c-release = rgb("#d1242f") // ④ slot release: red
#let _c-slow = _c-release // slow train: red
#let _c-fast = _c-guard // fast train: green

// All geometry is designed on a 560pt wide canvas. Everything is shrunk by _k
// so that it also fits the documentation page's ~510pt text width. The factor
// is applied inside _pt() and the fixed sizes below instead of via `scale()`,
// because scale() misplaces its content inside html.frame + figure.
#let _k = 0.9
#let _pt(x) = x * _k * 1pt

// The site is published through Typst's HTML export, where `place` is ignored.
// `html.frame` renders the drawing with the paged renderer into an embedded
// SVG instead, so the diagrams survive the HTML build too. The frame context
// has no usable width, so the HTML path needs an explicit block width.
#let _diagram(canvas-height, body) = context {
  let is-html = target() == "html"
  let framed = block(
    width: if is-html { 505pt } else { 100% },
    height: canvas-height * _k,
    {
      // Opaque card so the diagram stays readable on the site's dark theme.
      place(rect(width: 100%, height: 100%, fill: white, radius: _pt(6), stroke: 0.5pt + luma(80%)))
      body
    },
  )
  if is-html { html.frame(framed) } else { framed }
}

// Absolute-positioned helpers. Coordinates are in pt from the top-left corner.
#let _seg(x1, y1, x2, y2, stroke: 2.5pt + black) = {
  place(line(start: (_pt(x1), _pt(y1)), end: (_pt(x2), _pt(y2)), stroke: stroke))
}

// A filled triangle pointing along `angle` (0deg = up, 90deg = right).
#let _arrowhead(x, y, angle, col: black, size: 5pt) = {
  place(dx: _pt(x), dy: _pt(y), rotate(angle, polygon.regular(
    size: size * _k,
    vertices: 3,
    fill: col,
    stroke: none,
  )))
}

// A straight segment with an arrowhead at its end.
#let _arrow(x1, y1, x2, y2, stroke: 1pt + luma(40%), size: 5pt) = {
  let angle = calc.atan2(y2 - y1, x2 - x1)
  _seg(x1, y1, x2, y2, stroke: stroke)
  let paint = if stroke == auto { black } else { stroke.paint }
  _arrowhead(x2, y2, angle + 90deg, col: paint, size: size)
}

#let _at(x, y, w, h, body) = {
  place(dx: _pt(x), dy: _pt(y), box(width: _pt(w), height: _pt(h), align(center + horizon, body)))
}

// A numbered signal disc sitting on a track point.
#let _signal(x, y, num, col) = {
  place(dx: _pt(x - 8), dy: _pt(y - 8), circle(
    radius: _pt(8),
    fill: col.lighten(85%),
    stroke: 1.1pt + col,
  ))
  place(dx: _pt(x - 8), dy: _pt(y - 8), box(width: _pt(16), height: _pt(16), align(
    center + horizon,
    text(size: 8.5pt, weight: "bold", fill: col.darken(20%), num),
  )))
}

// A train drawn as a rounded rectangle across [x1, x2] centered on track y.
#let _train(x1, x2, y, col, label) = {
  place(dx: _pt(x1), dy: _pt(y - 7), rect(
    width: _pt(x2 - x1),
    height: _pt(14),
    radius: 4pt,
    fill: col.lighten(75%),
    stroke: 1pt + col,
  ))
  place(dx: _pt(x1), dy: _pt(y - 7), box(width: _pt(x2 - x1), height: _pt(14), align(
    center + horizon,
    text(size: 7pt, weight: "bold", fill: col.darken(30%), label),
  )))
}

// A two-line callout label with a thin leader line down to its anchor point.
#let _callout(x, y, w, col, title, sub, anchor: none) = {
  place(dx: _pt(x), dy: _pt(y), box(width: _pt(w), height: _pt(30), {
    text(size: 8pt, weight: "bold", fill: col.darken(20%), title)
    linebreak()
    text(size: 7pt, fill: luma(60%), sub)
  }))
  if anchor != none {
    let cx = x + w / 2
    let ax = anchor.at(0)
    let ay = anchor.at(1)
    // Draw the leader on the side of the label that faces the anchor point.
    let (sy, ey) = if ay < y { (y - 1, ay + 9) } else { (y + 31, ay - 9) }
    place(line(start: (_pt(cx), _pt(sy)), end: (_pt(ax), _pt(ey)), stroke: 0.7pt + luma(40%)))
  }
}

#let _box(x, y, w, h, tint, body) = {
  place(dx: _pt(x), dy: _pt(y), rect(
    width: _pt(w),
    height: _pt(h),
    radius: _pt(4),
    fill: tint.lighten(88%),
    stroke: 0.8pt + tint.darken(10%),
    align(center + horizon, body),
  ))
}

#let _flow-body(title, sub) = {
  text(size: 7.5pt, weight: "bold", title)
  linebreak()
  text(size: 7pt, fill: luma(60%), sub)
}

#let _labels = (
  zh: (
    dir: "行车方向",
    main: "正线",
    siding: "待避股道",
    platform: "站台（可选）",
    slow: "慢车",
    fast: "快车",
    forced: "被迫待避",
    s1: ("① 接近检测信号", "领取路签·登记速度差"),
    s2: ("② 正线控制信号", "扣下需待避的慢车"),
    s3: ("③ 待避股道出口信号", "仍有快车要过则继续扣留"),
    s4: ("④ 退出路签信号", "返还路签·完成待避"),
  ),
  en: (
    dir: "Direction of travel",
    main: "Main line",
    siding: "Siding",
    platform: "Platform (optional)",
    slow: "Slow train",
    fast: "Fast train",
    forced: "forced to yield",
    s1: ("① Approach detector", "claims slot · registers speed gap"),
    s2: ("② Main-line guard", "holds trains that must yield"),
    s3: ("③ Siding exit", "keeps holding while faster trains come"),
    s4: ("④ Slot release", "returns slot · yielding complete"),
  ),
  zh-flow: (
    a: ([慢车经过 ① 接近检测], [领取路签并登记]),
    b: ([② 正线控制信号检查], [同路签中是否有更快的列车？]),
    e: ([否：正常通过正线], []),
    c: ([是：被迫驶入待避股道], [③ 出口信号持续扣留]),
    f: ([快车通过后], [③ 出口信号放行]),
    g: ([经过 ④ 退出路签], [返还路签·完成待避]),
    yes: "是",
    no: "否",
  ),
  en-flow: (
    a: ([Slow train passes the ① detector], [claims the slot and registers]),
    b: ([② Main-line guard checks], [a faster train on the same slot?]),
    e: ([No: passes on the main line], []),
    c: ([Yes: forced into the siding], [held by the ③ exit signal]),
    f: ([After the fast train has passed], [the ③ exit releases it]),
    g: ([Passes the ④ slot release], [slot returned · yielding complete]),
    yes: "yes",
    no: "no",
  ),
)

// Overall layout: main line, siding/platform, and the four signal positions.
#let layout(lang) = {
  let t = _labels.at(lang)
  _diagram(250pt, {
    // --- track layout ---
    _seg(10, 140, 550, 140) // main line
    _seg(200, 140, 230, 190) // diverge
    _seg(230, 190, 370, 190) // siding
    _seg(370, 190, 400, 140) // merge

    // direction of travel
    _arrow(14, 140, 56, 140, stroke: 2pt + black, size: 7pt)
    _at(10, 152, 90, 12, text(size: 7pt, fill: luma(60%), t.dir))

    // siding / platform names
    _at(235, 206, 60, 12, text(size: 7pt, fill: luma(60%), t.siding))
    _at(480, 118, 60, 12, text(size: 7pt, fill: luma(60%), t.main))

    // platform alongside the siding (optional)
    place(dx: _pt(240), dy: _pt(168), rect(
      width: _pt(100),
      height: _pt(12),
      fill: luma(80%),
      stroke: 0.6pt + luma(55%),
    ))
    _at(240, 168, 100, 12, text(size: 6.5pt, fill: luma(40%), t.platform))

    // --- trains ---
    _train(250, 335, 190, _c-slow, t.slow)
    _train(245, 325, 140, _c-fast, t.fast)

    // forced entry: dashed arrow where the main line drops into the siding
    place(curve(
      curve.move((_pt(212), _pt(148))),
      curve.quad((_pt(225), _pt(180)), (_pt(240), _pt(181))),
      stroke: (thickness: 1pt, paint: _c-slow, dash: "dashed"),
    ))
    _arrowhead(242, 181, 93deg, col: _c-slow, size: 5.5pt)
    _at(138, 158, 60, 12, text(size: 6.5pt, fill: _c-slow.darken(10%), t.forced))

    // --- signals (② sits on the main line parallel to ③, ahead of the merge) ---
    _signal(70, 140, "①", _c-approach)
    _signal(350, 140, "②", _c-guard)
    _signal(350, 190, "③", _c-exit)
    _signal(450, 140, "④", _c-release)

    // --- callouts ---
    _callout(0, 64, 115, _c-approach, ..t.s1, anchor: (70, 140))
    _callout(285, 64, 130, _c-guard, ..t.s2, anchor: (350, 140))
    _callout(330, 214, 130, _c-exit, ..t.s3, anchor: (350, 190))
    _callout(420, 64, 130, _c-release, ..t.s4, anchor: (450, 140))
  })
}

// Decision flow of a yielding train, from registration to slot release.
#let flow(lang) = {
  let t = _labels.at(lang + "-flow")
  _diagram(142pt, {
    _box(10, 26, 135, 40, luma(30%), _flow-body(..t.a))
    _box(175, 26, 170, 40, luma(30%), _flow-body(..t.b))
    _box(385, 26, 165, 40, luma(30%), _flow-body(..t.e))
    _box(385, 92, 165, 40, luma(30%), _flow-body(..t.c))
    _box(210, 92, 135, 40, luma(30%), _flow-body(..t.f))
    _box(20, 92, 150, 40, luma(30%), _flow-body(..t.g))

    _arrow(145, 46, 173, 46)
    _arrow(345, 46, 383, 46)
    _at(348, 30, 30, 12, text(size: 7pt, weight: "bold", fill: luma(30%), t.no))

    place(curve(
      curve.move((_pt(310), _pt(66))),
      curve.quad((_pt(345), _pt(96)), (_pt(381), _pt(100))),
      stroke: 1pt + luma(40%),
    ))
    _arrowhead(383, 101, calc.atan2(5, 36) + 90deg, col: luma(40%), size: 5pt)
    _at(315, 72, 30, 12, text(size: 7pt, weight: "bold", fill: luma(30%), t.yes))

    _arrow(385, 112, 347, 112)
    _arrow(210, 112, 172, 112)
  })
}

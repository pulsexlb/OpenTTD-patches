#title[模块化机场]

*注：列车对接与解挂功能原创于 #link("https://github.com/J0anJosep/OpenTTD/tree/MultitileAirports")[OpenTTD Multitile Airport 补丁]，px-patch 在此补丁的基础上，进行了新版移植于功能完善。*

= 启用模块化机场功能
模块化机场功能默认不会被启用，你需要在设置中开启`允许建设自定义机场`选项。

#figure(
  image("../../static/images/custom-airport-setting.png", width: 80%),
)

启用此功能后，你将可以在建造机场按钮中看到下拉菜单，并建造自定义机场。

= 划定一个机场区域
建设自定义机场前，你需要通过建设菜单划定一个机场区域

#figure(
  image("../../static/images/airport-area.png", width: 80%),
)

= 机场起降飞机限制
如果你希望机场能够起降滑行机，那么你至少需要建造一条*允许降落的*跑道，以及一个停机位，如果你希望机场能够起降垂直起降机，那么你至少需要建造一个停机坪。

= 定义飞机滑行道
飞机在地面滑行时，将通过滑行道进行滑行，需要建造滑行道，这类似于火车轨道，通过滑行道连接跑到，停机坪，机库等

#figure(
  image("../../static/images/airport-tracks.png", width: 80%),
)

飞机允许直角和锐角转弯，不过速度会受到影响

= 机场造景功能
在建设完成停机坪、跑道，画好滑行道之后，飞机应该已经能够正常使用机场了。不过你还可以使用一些造景功能，例如改变地面图像：

#figure(
  image("../../static/images/airport-change-graphic.png", width: 80%),
)

建设候车室：

#figure(
  image("../../static/images/airport-cargo-catchment.png", width: 80%),
)

建设机场设施：

#figure(
  image("../../static/images/airport-infra.png", width: 80%),
)

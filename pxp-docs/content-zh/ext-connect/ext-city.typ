#import "@preview/letmeout:0.2.0": *

#title[图外城市与边界]

= 图外城市窗口

#figure(
  image("../../static/images/ext-connect-window-menu.png", width: 40%),
)

在地图下拉菜单中选择`图外城市与边界`，将打开图外城市窗口。


#figure(
  image("../../static/images/ext-connect-window.png", width: 80%),
)

= 单程距离、图外停站时间与城市名称

这三者为图外城市的基本参数，`单程距离`和`图外停站`影响在图外车站行驶和停站的时间。

这三个参数只能由一个公司修改，第一个公司修改后，就只能通过那一个公司修改。建议由管理员公司进行设置。

= 客货与产业

这个窗口中展示了图外城市的乘客、产业与客货状态等信息。

#figure(
  image("../../static/images/ext-connect-cargo1.png", width: 80%),
)
#figure(
  image("../../static/images/ext-connect-cargo2.png", width: 80%),
)


= 查看图外载具

在图外载具窗口中，可以查看正在地图外运行的载具列表、离开口与进入口、剩余时间等信息。

#figure(
  image("../../static/images/ext-connect-vehicle-outside-window.png", width: 80%),
)

= 编辑城市边界

注：此功能仅在设置选项中开启`允许玩家编辑图外城市边界`后才可使用，编辑后可能导致其它公司的出入口变为“未分配城市”，建议只在服务器管理员编辑地图时临时开启，服务器上线后关闭。

点击`编辑城市边界`按钮后，在地图边缘绘制新的城市边界段，或删除已有边界段，一个城市允许有多个边界段。


#title[载具运输]

*注：载具对接功能原创于 #link("https://github.com/Babel-TTT/OpenTTD-patches/tree/feature/road-veh-transport/")[Road Veh Transport 补丁]，px-patch 在此补丁的基础上，进行了优化和功能完善。*

在设置选项中开启`载具运输`功能，即可在游戏中运输载具运输。

#figure(
  image("../../static/images/veh-carry-setting.png", width: 80%),
)

= 简单的运输载具调度计划

载具可以简单的通过设置`等待被装载`和`在此被卸下`，来实现被运输。

#figure(
  image("../../static/images/road-veh-carry-order.png", width: 80%),
)

对于承运载具，需要先将其改装为运输载具

#figure(
  image("../../static/images/carry-veh-refit.png", width: 40%),
)

改装为运输载具的载具，添加的调度计划默认均为装卸载具

#figure(
  image("../../static/images/carry-veh-order.png", width: 80%),
)

= 限制
== 重量限制

一个承运载具无法无限运输载具，受总重量限制。对于飞机运输汽车，程序会简单的计算该汽车是否还能放入飞机中，如果不能载具将不会被运输；对于火车运输汽车和船只运输汽车，程序会逐一尝试将汽车放入一个车厢/船舱中，直到找到合适的车厢/船舱；对于轮船运输火车，程序会为每一节车厢都逐一尝试放入船舱中，只有所有机车都放入船舱（可以是不同的船舱）时，才能够运输。

== 卸载车站限制

对于汽车，要求公交车不能进入货车站，货车不能进入公交车站，以及铰接式车辆不能进入泊位式车站（与 OpenTTD 原版对汽车车站要求一致）。

对于火车，要求轨道类型匹配的同时，站台长度大于列车长度。

#title[列车解挂]

*注：列车对接与解挂功能原创于 #link("https://github.com/dylansan/OpenTTD-YPS/tree/Decouple")[OpenTTD YPS 补丁]，px-patch 在此补丁的基础上，进行了新版移植于功能完善。*

OpenTTD px-patch 为 OpenTTD 实现了列车对接与解挂的功能，玩家可以在车站中让两辆列车对接或解挂，调整编组，调度计划等。
基于此功能，我们可以实现：
1. 换向时在车站解挂机车，在站台的另一侧重新对接
2. 两列车对接后一起行驶
3. 列车解挂后执行不同的调度计划
4. 电气化/非电气化机车换挂
5. ...


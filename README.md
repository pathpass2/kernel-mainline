从主线fork

添加了编译立创泰山派boot.img所需的文件

驱动模块所需的firmware需要自行从armbian提取，这里没有上传。

config是从armbian获取的，在armbian-config的基础上开启了zram，方便移植到fedroa

可fork后在action直接编译，也可拉取到本地编译

dts改动：
参照armbian更改了mmc1(sd卡)的参数，可以从sd卡启动armbian-6.16.0，拔掉sd卡则从emmc启动系统
将sdhci(emmc)设为了mmcblk0， 使用boot.img的情况下  dts里面  rootfs引导设置为了mmcblk0p3，需要修改的话自行修改。波特率修改为了115200。

通过emmc启动的fedora42
<img width="1497" height="858" alt="image" src="https://github.com/user-attachments/assets/1c937dcc-557b-4758-a3c3-8cd348dc8e42" />

如果是用于armbian的dts则未修改rootfs分区和波特率，以armbian镜像里的armbianENV.txt为主


只测试了6.16、v6.16-rc7。6.16-rc7的WiFi有点问题。在v6.16的基础上加的tspi文件，重新保存为v6.16.0(ap6255)、v6.16.1(ap6212)


本地编译：
v6.16.0改了WiFi-ap6255/ap6256,其他与v6.16.1一样
如果是原装的ap6212-WiFi模块，建议切到v6.16.1 tag 根据需求修改config和dts后再编译


actions编译：
如果是原装的ap6212-WiFi模块，选择ap6212-6.16.1_kernel.yml  
ps：手里没有ap6212，买来就换了ap6255，所以没办法测试ap6212的蓝牙、WiFi


ps：主线内核的dtb名称跟armbian内核的dtb名称不一样，记得修改
<img width="1026" height="732" alt="image" src="https://github.com/user-attachments/assets/3e25b008-e547-4662-b662-c6967a262d9e" />

通过sd卡启动的armbian
<img width="1239" height="876" alt="d94e0ee5c0448b20ed7180c561af322c" src="https://github.com/user-attachments/assets/4c9fdd7e-b69f-4fe7-bea9-32206f17d8bc" />


# bme680
Data collection daemon for Bosch BME680 temperature, humidity, atmospheric pressure, air quality sensor

## Dependencies

* BME680 driver - https://github.com/BoschSensortec/BME680_driver

* BSEC library - https://www.bosch-sensortec.com/software-tools/software/bsec/

* Lib i2c - https://git.kernel.org/pub/scm/utils/i2c-tools/i2c-tools.git/snapshot/i2c-tools-4.2.tar.gz

* RRD database running under inetd

* web server + php-rrd + php-fpm for hosting php files to display the data


## Compiling for Raspberry Pi running 64-bit OS (Ubuntu 18.04)

As of July 2020, BSEC library v1.4.8.0 for Raspberry Pi is 32-bit, which would fail to link in 64-bit OS without extra steps.

1. Install and run arch-test, which will show the architectures supported by the kernel

```
sudo apt install arch-test
arch-test
arm64
armhf
```

2. Install gcc-multilib for the second architecture (armhf - which is 32-bit arm)

```
sudo apt install gcc-8-multilib-arm-linux-gnueabihf
```

3. All other linked libs should be 32-bit as well. Because bsec.c uses i2c library to talk to the sensor, it has to be 32-bit flavor. Download the source code of libi2c from https://git.kernel.org/pub/scm/utils/i2c-tools/i2c-tools.git/snapshot/i2c-tools-4.2.tar.gz. Uncompress and edit Makefile. Note the cross-compiler name and -marm option to produce 32-bit code:

```
CC := /usr/bin/arm-linux-gnueabihf-gcc-8
CFLAGS ?= -O2 -marm
BUILD_DYNAMIC_LIB ?= 0
BUILD_STATIC_LIB ?= 1
```

then build the static i2c library using make.

Finally, compile and statically link my code - bsec.c file (again using  the cross-compiler from gcc-multilib and -march option) together with the driver bme680.c. Note the -lm and -static options, which will statically link libm and libc:

```
/usr/bin/arm-linux-gnueabihf-gcc-8 -marm -O3 -o bsec bsec.c bme680.c -L ../../i2c-tools-4.2/lib -L . -li2c -lalgobsec -lm -static
```

Check the resulting binary, which should run just fine in 64-bit Ubintu kernel:

```
file bsec
bsec: ELF 32-bit LSB executable, ARM, EABI5 version 1 (GNU/Linux), statically linked, for GNU/Linux 3.2.0, BuildID[sha1]=0cf8e44276b114a9f271244a7b111a63c12a57f1, not stripped
```

## 64-bit alpine

In 64-bit alpine need to create chroot and install 32-bit alpine into it. Details are in https://wiki.alpinelinux.org/wiki/Alpine_Linux_in_a_chroot

```
curl -L0 https://dl-cdn.alpinelinux.org/alpine/v3.14/main/armhf/apk-tools-static-2.12.7-r0.apk

sudo mkdir /armhf
tar -xzf apk-tools-static-*.apk
./sbin/apk.static -X https://dl-cdn.alpinelinux.org/latest-stable/main -U --allow-untrusted -p /armhf --initdb add alpine-base

vi /etc/fstab
/dev    /armhf/dev      none    bind    0       0
none    /armhf/proc     proc    rw,nosuid,nodev,noexec,relatime 0       0
/sys    /armhf/sys      none    bind    0       0

mount -a

cp -L /etc/resolv.conf /armhf/etc/
mkdir -p /armhf/etc/apk
vi /armhf/etc/apk/repositories
https://dl-cdn.alpinelinux.org/alpine/v3.14/main

vi /etc/sysctl.conf
kernel.grsecurity.chroot_deny_chmod = 0

sysctl -p

chroot /armhf ash -l
apk update

rc-update add devfs sysinit
rc-update add dmesg sysinit
rc-update add mdev sysinit

rc-update add hwclock boot
rc-update add modules boot
rc-update add sysctl boot
rc-update add hostname boot
rc-update add bootmisc boot
rc-update add syslog boot

rc-update add mount-ro shutdown
rc-update add killprocs shutdown
rc-update add savecache shutdown

apk add -arch amrhf alpine-sdk linux-headers wget util-linux findutils-locate git
wget https://git.kernel.org/pub/scm/utils/i2c-tools/i2c-tools.git/snapshot/i2c-tools-4.2.tar.gz
tar -zxf i2c-tools-4.2.tar.gz
cd i2c-tools-4.2/
vi Makefile
USE_STATIC_LIB ?= 1

make

git clone --depth 1 https://github.com/creatica-soft/bme680.git
cd bme680/bsec
gcc -O3 -o bsec bsec.c bme680.c -L ../../i2c-tools-4.2/lib -L . -li2c -lalgobsec -lm -static
```

## RRD config

Create rrd database

```
sudo apt install rrdtool, php-rrd, inetutils-inetd
mkdir /var/rrd
chmod 755 env.rrd.sh
env.rrd.sh
```

Run rrd under inetd

```
sudo vi /etc/services
rrdsrv          13900/tcp                       # RRD server

sudo echo "127.0.0.1:rrdsrv stream tcp nowait root /usr/bin/rrdtool rrdtool - /var/rrd" >> /etc/inetd.conf
sudo systemctl enable inetutils-inetd
sudo systemctl start inetutils-inetd
```

## Configure bsec daemon

Review bsec config file

```
sudo cp bsec.conf /etc/
sudo vi /etc/bsec.conf
```

Create user and group to run bsec daemon under and a folder for config files and logging

```
sudo groupadd -r bsec
sudo useradd -r -g bsec bsec
sudo mkdir -p /var/bsec
```

Copy config folder from bsec library under /var/bsec

```
sudo cp -r BSEC_1.4.8.0_Raspb_x64_Release/config /var/bsec/
sudo chown -R bsec:bsec /var/bsec
```

Create bsec.service

```
sudo cp bsec.service /lib/systemd/system/bsec.service
systemctl enable bsec
systemctl start bsec
systemctl status bsec
```

Check the log files

```
tail -f /var/bsec/bsec.log
```
## Display the data

Install nginx and php-fpm

```
sudo apt install nginx php-fpm
sudo systemctl enable php-fpm
sudo systemctl start php-fpm
sudo systemctl enable nginx
sudo systemctl start nginx
sudo cp *.php env.html nginx/html
sudo mkdir nginx/html/images
sudo chgrp www-data nginx/html/images
```

Open env.html in a browser http://localhost/env.html

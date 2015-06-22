export ARCH=arm
export PATH=/home/yjiao/arm-eabi-4.6/bin:$PATH
export CROSS_COMPILE=arm-eabi-
make goldfish_armv7_defconfig

make menuconfig

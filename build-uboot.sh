#Cross compiler prefix
export CROSS_COMPILE=/source/exynos4412/gcc-arm-9.2-2019.12-x86_64-arm-none-linux-gnueabihf/bin/arm-none-linux-gnueabihf-
export ARCH=arm
#export PATH=$PATH:/source/exynos4412/gcc-arm-9.2-2019.12-x86_64-arm-none-linux-gnueabihf/bin/
if [ $1 == "distclean" ];then
make O=itop4412 distclean
echo "Distclean"

elif [ $1 == "defconfig" ];then
make O=itop4412 itop4412_defconfig
echo "Config itop4412"

elif [ $1 == "make" ];then	
make O=itop4412
cp -rf itop4412/spl/itop4412-spl.bin  /source/exynos4412/eg
cp -rf itop4412/u-boot.bin /source/exynos4412/eg
cp -rf itop4412/spl/itop4412-spl.bin  /source/exynos4412/build
cp -rf itop4412/u-boot.bin /source/exynos4412/build
cd /source/exynos4412/build
./build_uboot.sh
echo "Make!!!"

elif [ $1 == "menuconfig" ];then	
make O=itop4412 menuconfig
echo "Menu Config!!!"

else
echo "No Operation!!!"
fi

#make O=am335x distclean
#make O=am335x am335x_evm_defconfig
#make O=am335x all

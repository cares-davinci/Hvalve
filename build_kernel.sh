#!/bin/bash
export KERNELDIR=`readlink -f .`
export RAMFS_SOURCE=`readlink -f $KERNELDIR/ramdisk`
export PARTITION_SIZE=134217728

export OS="12.0.0"
export SPL="2021-10"

echo "kerneldir = $KERNELDIR"
echo "ramfs_source = $RAMFS_SOURCE"

RAMFS_TMP="/tmp/arter97-lahaina-hdk-ramdisk"

echo "ramfs_tmp = $RAMFS_TMP"
cd $KERNELDIR

if [[ "${1}" == "skip" ]] ; then
	echo "Skipping Compilation"
else
	echo "Compiling kernel"
	if echo "$0" | grep -q debug; then
		grep -vP "CONFIG_SERIAL_MSM_GENI.*CONSOLE" defconfig > .config
		echo "CONFIG_SERIAL_MSM_GENI_EARLY_CONSOLE=y" >> .config
		echo "CONFIG_SERIAL_MSM_GENI_CONSOLE=y" >> .config
		echo "earlycon is enabled for debugging!"
	else
		cp defconfig .config
	fi
	make "$@" || exit 1
fi

echo "Building new ramdisk"
#remove previous ramfs files
rm -rf '$RAMFS_TMP'*
rm -rf $RAMFS_TMP
rm -rf $RAMFS_TMP.cpio
#copy ramfs files to tmp directory
cp -axpP $RAMFS_SOURCE $RAMFS_TMP
cd $RAMFS_TMP

#clear git repositories in ramfs
find . -name .git -exec rm -rf {} \;
find . -name EMPTY_DIRECTORY -exec rm -rf {} \;

$KERNELDIR/ramdisk_fix_permissions.sh 2>/dev/null

cd $KERNELDIR
rm -rf $RAMFS_TMP/tmp/*

cd $RAMFS_TMP
find . | fakeroot cpio -H newc -o | pigz > $RAMFS_TMP.cpio.gz
ls -lh $RAMFS_TMP.cpio.gz
cd $KERNELDIR

echo "Making new boot image"
mkbootimg \
    --kernel $KERNELDIR/arch/arm64/boot/Image \
    --ramdisk $RAMFS_TMP.cpio.gz \
    --pagesize 4096 \
    --os_version     $OS \
    --os_patch_level $SPL \
    --header_version 3 \
    -o $KERNELDIR/boot.img

find arch/arm64/boot/dts/vendor/qcom/ -name '*.dtb' -exec cat {} + > dtb
mkbootimg \
    --vendor_ramdisk vendor_boot/ramdisk.cpio.gz \
    --base 0x00000000 \
    --dtb dtb \
    --dtb_offset 0x01f00000 \
    --header_version 3 \
    --kernel_offset 0x00008000 \
    --pagesize 4096 \
    --ramdisk_offset 0x01000000 \
    --tags_offset 0x00000100 \
    --vendor_cmdline 'console=ttyMSM0,115200n8 androidboot.hardware=qcom androidboot.console=ttyMSM0 androidboot.memcg=1 lpm_levels.sleep_disabled=1 video=vfb:640x400,bpp=32,memsize=3072000 msm_rtb.filter=0x237 service_locator.enable=1 androidboot.usbcontroller=a600000.dwc3 swiotlb=0 loop.max_part=7 cgroup.memory=nokmem,nosocket pcie_ports=compat loop.max_part=7 iptable_raw.raw_before_defrag=1 ip6table_raw.raw_before_defrag=1 buildvariant=userdebug' \
    --vendor_boot vendor_boot.img

ln -f arch/arm64/boot/dtbo.img .

GENERATED_SIZE=$(stat -c %s boot.img)
if [[ $GENERATED_SIZE -gt $PARTITION_SIZE ]]; then
	echo "boot.img size larger than partition size!" 1>&2
	exit 1
fi

echo "done"
ls -al boot.img dtbo.img vendor_boot.img
echo ""

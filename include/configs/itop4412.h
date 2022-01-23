/*
 * Copyright (C) 2011 Samsung Electronics
 *
 * Configuration settings for the SAMSUNG ITOP4412 (EXYNOS4412) board.
 *
 * SPDX-License-Identifier: GPL-2.0+
 */

#ifndef __CONFIG_ITOP4412_H
#define __CONFIG_ITOP4412_H

#include <configs/exynos4-common.h>

#define CONFIG_SUPPORT_EMMC_BOOT 1

/* High Level Configuration Options */
#define CONFIG_EXYNOS4210       1   /* which is a EXYNOS4210 SoC */
#define CONFIG_ITOP4412         1   /* working with ITOP4412*/

#define CONFIG_SYS_DCACHE_OFF       1

/* itop-4412 has 4 bank of DRAM */
#define CONFIG_NR_DRAM_BANKS        4
#define CONFIG_SYS_SDRAM_BASE       0x40000000
#define PHYS_SDRAM_1            CONFIG_SYS_SDRAM_BASE
#define SDRAM_BANK_SIZE         (256 << 20) /* 256 MB */

/* memtest works on */
#define CONFIG_SYS_MEMTEST_START    CONFIG_SYS_SDRAM_BASE
#define CONFIG_SYS_MEMTEST_END      (CONFIG_SYS_SDRAM_BASE + 0x6000000)
#define CONFIG_SYS_LOAD_ADDR        (CONFIG_SYS_SDRAM_BASE + 0x00100000)

//#define CONFIG_SYS_TEXT_BASE        0x43E00000

/* #define MACH_TYPE_ITOP4412       0xffffffff */
#define CONFIG_MACH_TYPE        MACH_TYPE_ITOP4412

/* select serial console configuration */
#define CONFIG_SERIAL2

/* Console configuration */
#define CONFIG_DEFAULT_CONSOLE      "console=ttySAC1,115200n8\0"

#define CONFIG_SYS_MEM_TOP_HIDE (1 << 20)   /* ram console */

#define CONFIG_SYS_MONITOR_BASE 0x00000000

/* Power Down Modes */
#define S5P_CHECK_SLEEP         0x00000BAD
#define S5P_CHECK_DIDLE         0xBAD00000
#define S5P_CHECK_LPA           0xABAD0000

//#define CONFIG_SUPPORT_RAW_INITRD

/* MMC SPL */
#define COPY_BL2_FNPTR_ADDR     0x02020030
#define CONFIG_SPL_TEXT_BASE    0x02023400 /* 0x02021410 */


#if 1
#define NET_CONFIG_ENV \
		"ethaddr=00:d8:1c:04:55:60\0" \
		"ipaddr=192.168.1.200\0" \
		"serverip=192.168.1.199\0" 

#define MMC_PARTION_ENV \
		"ubootcnt=0\0" \
		"ubootblocks=0x440\0" \
		"fdtcnt=0x440\0" \
		"fdtblocks=0xa0\0" \
		"kernelcnt=0x4e0\0" \
		"kernelblocks=0x4000\0" 

#define USERARGE \
		"usb_init=usb start;"\
			"usb reset;\0"\
		"download_uboot=mmc dev 1;"\
			"mmc partconf 1 1 1 1;"\
			"mmc partconf 1;"\
			"mw 0x48000000 0 0x88000;"\
			"tftpboot 0x48000000 ${serverip}:uboot_emmc.bin;"\
			"mmc erase ${ubootcnt} ${ubootblocks};"\
			"mmc write 0x48000000 ${ubootcnt} ${ubootblocks};"\
			"mmc partconf 1 1 1 0\0"\
		"download_dtb=mmc dev 1;"\
			"mw 0x48000000 0 0x14000;"\
			"tftpboot 0x48000000 ${serverip}:exynos4412-itop-elite.dtb;"\
			"mmc write 0x48000000 ${fdtcnt} ${fdtblocks}\0"\
		"download_kernel=mmc dev 1;"\
			"mw 0x48000000 0 0x800000;"\
			"tftpboot 0x48000000 ${serverip}:uImage;"\
			"mmc write 0x48000000 ${kernelcnt} ${kernelblocks}\0"\
		"download_rootfs=mmc dev 1;"\
			"tftpboot 0x48000000 ${serverip}:rootfs.img;"\
			"ext4decompress mmc 1:2 ${ramdiskaddr}\0"
			
#define USERACTION \
		"run usb_init\0"
#endif



#define CONFIG_EXTRA_ENV_SETTINGS \
    "loadaddr=0x40007000\0" \
    "ramdiskaddr=0x48000000\0" \
    "console=ttySAC2,115200n8\0" \
    "mmcdev=1\0" \
    "bootenv=uEnv.txt\0" \
    "dtb_addr=0x41000000\0" \
    "dtb_name=exynos4412-itop-elite.dtb\0" \
    "loadbootenv=load mmc ${mmcdev} ${loadaddr} ${bootenv}\0" \
    "importbootenv=echo Importing environment from mmc ...; " \
    "env import -t $loadaddr $filesize\0" \
    "loadbootscript=load mmc ${mmcdev} ${loadaddr} boot.scr\0" \
    "bootscript=echo Running bootscript from mmc${mmcdev} ...; " \
    "source ${loadaddr}\0" \
    NET_CONFIG_ENV \
    MMC_PARTION_ENV \
    USERARGE \
    USERACTION
    
#if 0   
	"bootargs=console=ttySAC2,115200n8 earlyprintk\0" \

#define CONFIG_BOOTCOMMAND \
    "if mmc rescan; then " \
        "echo SD/MMC found on device ${mmcdev};" \
        "if run loadbootenv; then " \
            "echo Loaded environment from ${bootenv};" \
            "run importbootenv;" \
        "fi;" \
        "if test -n $uenvcmd; then " \
            "echo Running uenvcmd ...;" \
            "run uenvcmd;" \
        "fi;" \
        "if run loadbootscript; then " \
            "run bootscript; " \
        "fi; " \
    "fi;" \
    "mmc read ${loadaddr} 0x1000 0x4000; mmc read ${dtb_addr} 0x800 0xa0; bootm ${loadaddr} - ${dtb_addr}" \
    "load mmc ${mmcdev} ${loadaddr} uImage; load mmc ${mmcdev} ${dtb_addr} ${dtb_name}; bootm ${loadaddr} - ${dtb_addr}"
#endif


#if 0

#define CONFIG_BOOTCOMMAND \
	"mmc dev ${mmcdev};" \
    "mmc read ${loadaddr} ${kernelcnt} ${kernelblocks};" \
    "mmc read ${dtb_addr} ${fdtcnt} ${fdtblocks};" \
    "bootm ${loadaddr} - ${dtb_addr} "
    
#else    

#define CONFIG_BOOTCOMMAND \
	"run usb_init;" \
    "tftpboot ${dtb_addr} ${serverip}:exynos4412-itop-elite.dtb;" \
    "tftpboot ${loadaddr} ${serverip}:uImage;" \
    "bootm ${loadaddr} - ${dtb_addr} "
#endif



#define CONFIG_CLK_1000_400_200

/* MIU (Memory Interleaving Unit) */
#define CONFIG_MIU_2BIT_21_7_INTERLEAVED

#define CONFIG_SYS_MMC_ENV_DEV		1
#define RESERVE_BLOCK_SIZE			(512)
#define BL1_SIZE					(8 << 10) /*8 K reserved for BL1*/
#define BL2_SIZE					(16 << 10) /*16 K reserved for BL2 */

#define CONFIG_SPL_MAX_FOOTPRINT    (14 * 1024)

#define CONFIG_SPL_STACK            0x02040000
#define UBOOT_SIZE                  (2 << 20)
#define CONFIG_SYS_INIT_SP_ADDR     (CONFIG_SYS_TEXT_BASE+UBOOT_SIZE-0x1000)

/* U-Boot copy size from boot Media to DRAM. */
#define COPY_BL2_SIZE       0x80000    //512k
#define BL2_START_OFFSET    ((RESERVE_BLOCK_SIZE + BL1_SIZE + BL2_SIZE)/512)
#define BL2_SIZE_BLOC_COUNT (COPY_BL2_SIZE/512)


//#define CONFIG_ENV_SIZE			0x2000  //8k
//#define CONFIG_ENV_OFFSET			(RESERVE_BLOCK_SIZE + BL1_SIZE + BL2_SIZE + COPY_BL2_SIZE)
                                    //0x86200

/* USB */
#define CONFIG_USB_EHCI_EXYNOS

#endif  /* __CONFIG_H */

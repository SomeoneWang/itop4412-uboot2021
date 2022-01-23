/*
 * Copyright (c) 2010 Samsung Electronics Co., Ltd.
 *              http://www.samsung.com/
 *
 * fdisk command for U-boot
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 */

#include <common.h>
#include <blk.h>
#include <config.h>
#include <exports.h>
#include <fat.h>
#include <fs.h>
#include <log.h>
#include <asm/byteorder.h>
#include <part.h>
#include <malloc.h>
#include <memalign.h>
#include <asm/cache.h>
#include <linux/compiler.h>
#include <linux/ctype.h>
#include <command.h>
#include <mapmem.h>

//#include <part_dos.h>


#define DOS_PART_DISKSIG_OFFSET	0x1b8
#define DOS_PART_TBL_OFFSET	0x1be
#define DOS_PART_MAGIC_OFFSET	0x1fe
#define DOS_PBR_FSTYPE_OFFSET	0x36
#define DOS_PBR32_FSTYPE_OFFSET	0x52
#define DOS_PBR_MEDIA_TYPE_OFFSET	0x15
#define DOS_MBR	0
#define DOS_PBR	1

#define BYTE_PER_SEC	512
#define RESERVED_CNT	32


/* FAT Boot Recode */
#define mincls(fat)  ((fat) == 12 ? MINCLS12 :	\
		      (fat) == 16 ? MINCLS16 :	\
				    MINCLS32)

#define maxcls(fat)  ((fat) == 12 ? MAXCLS12 :	\
		      (fat) == 16 ? MAXCLS16 :	\
				    MAXCLS32)

#define mk1(p, x)				\
    (p) = (__u8)(x)

#define mk2(p, x)				\
    (p)[0] = (__u8)(x),			\
    (p)[1] = (__u8)((x) >> 010)

#define mk4(p, x)				\
    (p)[0] = (__u8)(x),			\
    (p)[1] = (__u8)((x) >> 010),		\
    (p)[2] = (__u8)((x) >> 020),		\
    (p)[3] = (__u8)((x) >> 030)

#define argto1(arg, lo, msg)  argtou(arg, lo, 0xff, msg)
#define argto2(arg, lo, msg)  argtou(arg, lo, 0xffff, msg)
#define argto4(arg, lo, msg)  argtou(arg, lo, 0xffffffff, msg)
#define argtox(arg, lo, msg)  argtou(arg, lo, UINT_MAX, msg)

struct bs {
    __u8 jmp[3];		/* bootstrap entry point */
    __u8 oem[9];		/* OEM name and version */
};

struct bsbpb {
    __u8 bps[2];		/* bytes per sector */
    __u8 spc;			/* sectors per cluster */
    __u8 res[2];		/* reserved sectors */
    __u8 nft;			/* number of FATs */
    __u8 rde[2];		/* root directory entries */
    __u8 sec[2];		/* total sectors */
    __u8 mid;			/* media descriptor */
    __u8 spf[2];		/* sectors per FAT */
    __u8 spt[2];		/* sectors per track */
    __u8 hds[2];		/* drive heads */
    __u8 hid[4];		/* hidden sectors */
    __u8 bsec[6];		/* big total sectors */
};

/* For FAT32 */
struct bsxbpb {
    __u8 bspf[4];		/* big sectors per FAT */
    __u8 xflg[2];		/* FAT control flags */
    __u8 vers[2];		/* file system version */
    __u8 rdcl[4];		/* root directory start cluster */
    __u8 infs[2];		/* file system info sector */
    __u8 bkbs[2];		/* backup boot sector */
    __u8 rsvd[12];		/* reserved */
};

struct bsx {
    __u8 drv;		/* drive number */
    __u8 rsvd;		/* reserved */
    __u8 sig;		/* extended boot signature */
    __u8 volid[4];		/* volume ID number */
    __u8 label[11]; 	/* volume label */
    __u8 type[8];		/* file system type */
};

struct de {
    __u8 namext[11];	/* name and extension */
    __u8 attr;		/* attributes */
    __u8 rsvd[10];		/* reserved */
    __u8 time[2];		/* creation time */
    __u8 date[2];		/* creation date */
    __u8 clus[2];		/* starting cluster */
    __u8 size[4];		/* size */
};

struct bpb {
    __u32 bps;			/* bytes per sector */
    __u32 spc;			/* sectors per cluster */
    __u32 res;			/* reserved sectors */
    __u32 nft;			/* number of FATs */
    __u32 rde;			/* root directory entries */
    __u32 sec;			/* total sectors */
    __u32 mid;			/* media descriptor */
    __u32 spf;			/* sectors per FAT */
    __u32 spt;			/* sectors per track */
    __u32 hds;			/* drive heads */
    __u32 hid;			/* hidden sectors */
    __u32 bsec; 		/* big total sectors */
    __u32 bspf; 		/* big sectors per FAT */
    __u32 rdcl; 		/* root directory start cluster */
    __u32 infs; 		/* file system info sector */
    __u32 bkbs; 		/* backup boot sector */
};



static struct blk_desc *fs_dev_desc;
static int fs_dev_part;
static struct disk_partition fs_partition;


/*
 * Copy string, padding with spaces.
 */
static void
setstr(u_int8_t *dest, const char *src, size_t len)
{
    while (len--)
	*dest++ = *src ? *src++ : ' ';
}


static int write_pbr(struct blk_desc *dev_desc, struct disk_partition *info)
{
	struct bs *bs;
	struct bsbpb *bsbpb;
	struct bsxbpb *bsxbpb;
	struct bsx *bsx;
	__u8 *img;
	int img_offset = 0;
	//int reserved_cnt= 0;
	int i;
	int fat_size = 0;

	img = malloc(sizeof(__u8)*512);
	if(img == NULL) {
		printf("Can't make img buffer~~!!\n");
		return -1;
	}
	memset(img, 0x0, sizeof(__u8)*512);


	/* Erase Reserved Sector(PBR) */
	for (i = 0;i < RESERVED_CNT; i++) {
		//if (dev_desc->block_write(dev_desc->dev, info->start + i, 1, (ulong *)img) != 1) {
		if (blk_dwrite(dev_desc, info->start + i, 1, (ulong *)img) != 1) {
			printf ("Can't erase reserved sector~~~!!!\n");
			return -1;
		}
	}

	/* Set bs */
	bs = (struct bs *)img;
	img_offset += sizeof(struct bs) - 1;

	mk1(bs->jmp[0], 0xeb);
	mk1(bs->jmp[1], 0x58);
	mk1(bs->jmp[2], 0x90); /* Jump Boot Code */
	setstr(bs->oem, "SAMSUNG", sizeof(bs->oem)); /* OEM Name */

	uint spc;
	/* Set bsbpb */
	bsbpb = (struct bsbpb *)(img + img_offset);
	img_offset += sizeof(struct bsbpb) - 2;

	mk2(bsbpb->bps, 512); /* Byte Per Sector */

	printf("size checking ...\n");
	/* Sector Per Cluster */
	if (info->size < 0x10000) { /* partition size >= 32Mb */
		printf("Can't format less than 32Mb partition!!\n");
		return -1;
	}
	if (info->size <= 0x20000) { /* under 64M -> 512 bytes */
		printf("Under 64M\n");
		mk1(bsbpb->spc, 1);
		spc = 1;
	}
	else if (info->size <= 0x40000) { /* under 128M -> 1K */
		printf("Under 128M\n");
		mk1(bsbpb->spc, 2);
		spc = 2;
	}
	else if (info->size <= 0x80000) { /* under 256M -> 2K */
		printf("Under 256M\n");
		mk1(bsbpb->spc, 4);
		spc = 4;
	}
	else if (info->size <= 0xFA0000) { /* under 8G -> 4K */
		printf("Under 8G\n");
		mk1(bsbpb->spc, 8);
		spc = 8;
	}
	else if (info->size <= 0x1F40000) { /* under 16G -> 8K */
		printf("Under 16G\n");
		mk1(bsbpb->spc, 16);
		spc = 16;
	}
	else {
		printf("16G~\n");
		mk1(bsbpb->spc, 32);
		spc = 32;
	}

	printf("write FAT info: %d\n",RESERVED_CNT);
	mk2(bsbpb->res, RESERVED_CNT); /* Reserved Sector Count */
	mk1(bsbpb->nft, 2); /* Number of FATs */
	mk2(bsbpb->rde, 0); /* Root Directory Entry Count : It's no use in FAT32 */
	mk2(bsbpb->sec, 0); /* Total Sector : It's no use in FAT32 */
	mk1(bsbpb->mid, 0xF8); /* Media */
	mk2(bsbpb->spf, 0); /* FAT Size 16 : It's no use in FAT32 */
	mk2(bsbpb->spt, 0); /* Sector Per Track */
	mk2(bsbpb->hds, 0); /* Number Of Heads */
	mk4(bsbpb->hid, 0); /* Hidden Sector */
	mk4(bsbpb->bsec, info->size); /* Total Sector For FAT32 */

	/* Set bsxbpb */
	bsxbpb = (struct bsxbpb *)(img + img_offset);
	img_offset += sizeof(struct bsxbpb);

	mk4(bsxbpb->bspf, (info->size / (spc * 128))); /* FAT Size 32 */
	fat_size = info->size / (spc * 128);
	printf("Fat size : 0x%lx\n", info->size / (spc * 128));
	mk2(bsxbpb->xflg, 0); /* Ext Flags */
	mk2(bsxbpb->vers, 0); /* File System Version */
	mk4(bsxbpb->rdcl, 2); /* Root Directory Cluster */
	mk2(bsxbpb->infs, 1); /* File System Information */
	mk2(bsxbpb->bkbs, 0); /* Boot Record Backup Sector */

	/* Set bsx */
	bsx = (struct bsx *)(img + img_offset);
	mk1(bsx->drv, 0); /* Drive Number */
	mk1(bsx->sig, 0x29); /* Boot Signature */
	mk4(bsx->volid, 0x3333); /* Volume ID : 0x3333 means nothing */
	setstr(bsx->label, "NO NAME ", sizeof(bsx->label)); /* Volume Label */
	setstr(bsx->type, "FAT32", sizeof(bsx->type)); /* File System Type */

	/* Set Magic Number */
	mk2(img + BYTE_PER_SEC - 2, 0xaa55); /* Signature */

/*	
	printf("Print Boot Recode\n");
	for(i = 0;i<512;i++) {
		if(img[i] == 0)
			printf("00 ");
		else
			printf("%2x ", img[i]);
		if (!((i+1) % 16))
			printf("\n");
	}
*/	

	//if (dev_desc->block_write(dev_desc->dev, info->start, 1, (ulong *)img) != 1) {
	if (blk_dwrite(dev_desc, info->start, 1, (ulong *)img) != 1) {
		printf ("Can't write PBR~~~!!!\n");
		return -1;
	}

	return fat_size;
}

static int write_reserved(struct blk_desc *dev_desc, struct disk_partition *info)
{
	/* Set Reserved Region */
	__u8 *img;
	//int i;
	img = malloc(sizeof(__u8)*512);
	if(img == NULL) {
		printf("Can't make img buffer~~(reserved)!!\n");
		return -1;
	}

	memset(img, 0x0, sizeof(__u8)*512);

	mk4(img, 0x41615252); /* Lead Signature */
	mk4(img + BYTE_PER_SEC - 28, 0x61417272); /* Struct Signature */
	mk4(img + BYTE_PER_SEC - 24, 0xffffffff); /* Free Cluster Count */
	mk4(img + BYTE_PER_SEC - 20, 0x3); /* Next Free Cluster */
	mk2(img + BYTE_PER_SEC - 2, 0xaa55); /* Trail Signature */

	/*
	printf("Print Reserved Region\n");
	for(i = 0;i<512;i++) {
		if(img[i] == 0)
			printf("00 ");
		else
			printf("%2x ", img[i]);
		if (!((i+1) % 16))
			printf("\n");
	}
	*/

	/* Write Reserved region */
	//if (dev_desc->block_write(dev_desc->dev, info->start+1, 1, (ulong *)img) != 1) {
	if (blk_dwrite(dev_desc, info->start+1, 1, (ulong *)img) != 1) {
		printf ("Can't write reserved region~~~!!!\n");
		return -1;
	}

	return 1;
}

#if 1
static int write_fat(struct blk_desc *dev_desc, struct disk_partition *info, int
fat_size)
{
	__u8 *dummy;
	__u8 *img;
	int i;

	/* Create buffer for FAT */
	img = malloc(sizeof(__u8)*512);
	if(img == NULL) {
		printf("Can't make img buffer~~!!\n");
		return -1;
	}
	memset(img, 0x0, sizeof(__u8) * 512);

	/* Create buffer for erase */
	dummy = malloc(sizeof(__u8) * 8192);
	if(dummy == NULL) {
		printf("Can't make dummy buffer~~!!\n");
		return -1;
	}
	memset(dummy, 0x0, sizeof(__u8) * 8192);

	/* Erase FAT Region */
	int erase_block_cnt = (fat_size * 2);
	//int temp = 0;
	printf("Erase FAT region");
	for (i = 0;i < erase_block_cnt + 10; i+=16) {
		/*if (dev_desc->block_write(dev_desc->dev, info->start +
			RESERVED_CNT + i, 16, (ulong *)dummy) != 16) {*/
		if (blk_dwrite(dev_desc, info->start +
			RESERVED_CNT + i, 16, (ulong *)dummy) != 16) {
			printf ("Can't erase FAT region~~!!!\n");
		}
		if((i % 160) == 0)
			printf(".");

	}
	printf("\n");

	mk4(img, 0x0ffffff8);
	mk4(img+4, 0x0fffffff);
	mk4(img+8, 0x0fffffff); /* Root Directory */

	/*
	printf("Print FAT Region\n");
	for(i = 0;i<512;i++) {
		if(img[i] == 0)
			printf("00 ");
		else
			printf("%2x ", img[i]);
		if (!((i+1) % 16))
			printf("\n");
	}
	*/
	/* Write FAT Region */
	//if (dev_desc->block_write(dev_desc->dev, info->start + RESERVED_CNT, 1, (ulong *)img) != 1) {
	if (blk_dwrite(dev_desc, info->start + RESERVED_CNT, 1, (ulong *)img) != 1) {
		printf ("Can't write FAT~~~!!!\n");
		return -1;
	}

	return 1;
}
#endif

int
fat_format_device(struct blk_desc        *dev_desc, struct disk_partition info)
{
	#define SECTOR_SIZE 512
	unsigned char buffer[SECTOR_SIZE];

#if 0
	if (!dev_desc->block_read)
		return -1;
	//cur_dev = dev_desc;
	/* check if we have a MBR (on floppies we have only a PBR) */
	if (dev_desc->block_read (dev_desc, 0, 1, (ulong *) buffer) != 1) {
		printf ("** Can't read from device %d **\n", dev_desc->devnum);
		return -1;
	}
#endif
	if (blk_dread(dev_desc, 0, 1, buffer) != 1) {
		printf ("** Can't read from device %d **\n", dev_desc->devnum);
		return -1;
	}
	
	if (buffer[DOS_PART_MAGIC_OFFSET] != 0x55 ||
		buffer[DOS_PART_MAGIC_OFFSET + 1] != 0xaa) {
		printf("** MBR is broken **\n");
		/* no signature found */
		return -1;
	}
	
	printf("Partition: Start Address(0x%lx), Size(0x%lx)\n", info.start, info.size);


	int fat_size;
	fat_size = write_pbr(dev_desc, &info);

	if(fat_size < 0)
		return -1;
	if(write_reserved(dev_desc, &info) < 0)
		return -1;

	if(write_fat(dev_desc, &info, fat_size) < 0)
		return -1;
	printf("Partition%d format complete.\n", fs_dev_part);
	return 0;
}


int do_fat_format(struct cmd_tbl *cmdtp, int flag, int argc,
		      char *const argv[])
{
	if (argc < 2) {
		printf ("usage : fatformat <interface> <dev[:part]>\n");
		return(0);
	}

	fs_dev_part = blk_get_device_part_str(argv[1], (argc >= 3) ? argv[2] : NULL, &fs_dev_desc,
					&fs_partition, 1);
	if (fs_dev_part < 0)
		return -1;

	if (fs_dev_desc == NULL) {
		puts ("\n ** Invalid boot device **\n");
		return 1;
	}

	printf("Start format MMC%d partition%d ...\n", fs_dev_desc->devnum, fs_dev_part);
	if (fat_format_device(fs_dev_desc, fs_partition) != 0) {
		printf("Format failure!!!\n");
	}

	return 0;
}


U_BOOT_CMD(
	fatformat, 3, 0, do_fat_format,
	"fatformat - disk format by FAT32\n",
	"<interface(only support mmc)> <dev:partition num>\n"
	"	- format by FAT32 on 'interface'\n"
);


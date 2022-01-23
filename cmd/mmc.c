// SPDX-License-Identifier: GPL-2.0+
/*
 * (C) Copyright 2003
 * Kyle Harris, kharris@nexus-tech.net
 */

#include <common.h>
#include <blk.h>
#include <command.h>
#include <console.h>
#include <memalign.h>
#include <mmc.h>
#include <part.h>
#include <sparse_format.h>
#include <image-sparse.h>

/*mmc fdisk*/
#define		BLOCK_SIZE			512
#define		BLOCK_END			0xFFFFFFFF
#define		_10MB				(10*1024*1024)
#define		_100MB				(100*1024*1024)
#define		_300MB				(300*1024*1024)
#define		_8_4GB				(1023*254*63)
#define		_1GB				(1024*1024*1024)
#define		DISK_START			(16*1024*1024)
#define		SYSTEM_PART_SIZE		_1GB //_300MB
#define		USER_DATA_PART_SIZE		_1GB //_300MB //_1GB
#define		CACHE_PART_SIZE			_300MB

#define		CHS_MODE			0
#define		LBA_MODE			!(CHS_MODE)

typedef struct
{
	int		C_start;
	int		H_start;
	int		S_start;

	int		C_end;
	int		H_end;
	int		S_end;

	int		available_block;
	int		unit;
	int		total_block_count;
	int		addr_mode;	// LBA_MODE or CHS_MODE
} SDInfo;

typedef struct
{
	unsigned char bootable;
	unsigned char partitionId;

	int		C_start;
	int		H_start;
	int		S_start;

	int		C_end;
	int		H_end;
	int		S_end;

	int		block_start;
	int		block_count;
	int		block_end;
} PartitionInfo;
/*mmc fdisk end*/
static int curr_device = -1;

static void print_mmcinfo(struct mmc *mmc)
{
	int i;

	printf("Device: %s\n", mmc->cfg->name);
	printf("Manufacturer ID: %x\n", mmc->cid[0] >> 24);
	printf("OEM: %x\n", (mmc->cid[0] >> 8) & 0xffff);
	printf("Name: %c%c%c%c%c \n", mmc->cid[0] & 0xff,
			(mmc->cid[1] >> 24), (mmc->cid[1] >> 16) & 0xff,
			(mmc->cid[1] >> 8) & 0xff, mmc->cid[1] & 0xff);

	printf("Bus Speed: %d\n", mmc->clock);
#if CONFIG_IS_ENABLED(MMC_VERBOSE)
	printf("Mode: %s\n", mmc_mode_name(mmc->selected_mode));
	mmc_dump_capabilities("card capabilities", mmc->card_caps);
	mmc_dump_capabilities("host capabilities", mmc->host_caps);
#endif
	printf("Rd Block Len: %d\n", mmc->read_bl_len);

	printf("%s version %d.%d", IS_SD(mmc) ? "SD" : "MMC",
			EXTRACT_SDMMC_MAJOR_VERSION(mmc->version),
			EXTRACT_SDMMC_MINOR_VERSION(mmc->version));
	if (EXTRACT_SDMMC_CHANGE_VERSION(mmc->version) != 0)
		printf(".%d", EXTRACT_SDMMC_CHANGE_VERSION(mmc->version));
	printf("\n");

	printf("High Capacity: %s\n", mmc->high_capacity ? "Yes" : "No");
	puts("Capacity: ");
	print_size(mmc->capacity, "\n");

	printf("Bus Width: %d-bit%s\n", mmc->bus_width,
			mmc->ddr_mode ? " DDR" : "");

#if CONFIG_IS_ENABLED(MMC_WRITE)
	puts("Erase Group Size: ");
	print_size(((u64)mmc->erase_grp_size) << 9, "\n");
#endif

	if (!IS_SD(mmc) && mmc->version >= MMC_VERSION_4_41) {
		bool has_enh = (mmc->part_support & ENHNCD_SUPPORT) != 0;
		bool usr_enh = has_enh && (mmc->part_attr & EXT_CSD_ENH_USR);
		ALLOC_CACHE_ALIGN_BUFFER(u8, ext_csd, MMC_MAX_BLOCK_LEN);
		u8 wp;
		int ret;

#if CONFIG_IS_ENABLED(MMC_HW_PARTITIONING)
		puts("HC WP Group Size: ");
		print_size(((u64)mmc->hc_wp_grp_size) << 9, "\n");
#endif

		puts("User Capacity: ");
		print_size(mmc->capacity_user, usr_enh ? " ENH" : "");
		if (mmc->wr_rel_set & EXT_CSD_WR_DATA_REL_USR)
			puts(" WRREL\n");
		else
			putc('\n');
		if (usr_enh) {
			puts("User Enhanced Start: ");
			print_size(mmc->enh_user_start, "\n");
			puts("User Enhanced Size: ");
			print_size(mmc->enh_user_size, "\n");
		}
		puts("Boot Capacity: ");
		print_size(mmc->capacity_boot, has_enh ? " ENH\n" : "\n");
		puts("RPMB Capacity: ");
		print_size(mmc->capacity_rpmb, has_enh ? " ENH\n" : "\n");

		for (i = 0; i < ARRAY_SIZE(mmc->capacity_gp); i++) {
			bool is_enh = has_enh &&
				(mmc->part_attr & EXT_CSD_ENH_GP(i));
			if (mmc->capacity_gp[i]) {
				printf("GP%i Capacity: ", i+1);
				print_size(mmc->capacity_gp[i],
					   is_enh ? " ENH" : "");
				if (mmc->wr_rel_set & EXT_CSD_WR_DATA_REL_GP(i))
					puts(" WRREL\n");
				else
					putc('\n');
			}
		}
		ret = mmc_send_ext_csd(mmc, ext_csd);
		if (ret)
			return;
		wp = ext_csd[EXT_CSD_BOOT_WP_STATUS];
		for (i = 0; i < 2; ++i) {
			printf("Boot area %d is ", i);
			switch (wp & 3) {
			case 0:
				printf("not write protected\n");
				break;
			case 1:
				printf("power on protected\n");
				break;
			case 2:
				printf("permanently protected\n");
				break;
			default:
				printf("in reserved protection state\n");
				break;
			}
			wp >>= 2;
		}
	}
}
static struct mmc *init_mmc_device(int dev, bool force_init)
{
	struct mmc *mmc;
	mmc = find_mmc_device(dev);
	if (!mmc) {
		printf("no mmc device at slot %x\n", dev);
		return NULL;
	}

	if (!mmc_getcd(mmc))
		force_init = true;

	if (force_init)
		mmc->has_init = 0;
	if (mmc_init(mmc))
		return NULL;

#ifdef CONFIG_BLOCK_CACHE
	struct blk_desc *bd = mmc_get_blk_desc(mmc);
	blkcache_invalidate(bd->if_type, bd->devnum);
#endif

	return mmc;
}

static int do_mmcinfo(struct cmd_tbl *cmdtp, int flag, int argc,
		      char *const argv[])
{
	struct mmc *mmc;

	if (curr_device < 0) {
		if (get_mmc_num() > 0)
			curr_device = 0;
		else {
			puts("No MMC device available\n");
			return 1;
		}
	}

	mmc = init_mmc_device(curr_device, false);
	if (!mmc)
		return CMD_RET_FAILURE;

	print_mmcinfo(mmc);
	return CMD_RET_SUCCESS;
}

#if CONFIG_IS_ENABLED(CMD_MMC_RPMB)
static int confirm_key_prog(void)
{
	puts("Warning: Programming authentication key can be done only once !\n"
	     "         Use this command only if you are sure of what you are doing,\n"
	     "Really perform the key programming? <y/N> ");
	if (confirm_yesno())
		return 1;

	puts("Authentication key programming aborted\n");
	return 0;
}

static int do_mmcrpmb_key(struct cmd_tbl *cmdtp, int flag,
			  int argc, char *const argv[])
{
	void *key_addr;
	struct mmc *mmc = find_mmc_device(curr_device);

	if (argc != 2)
		return CMD_RET_USAGE;

	key_addr = (void *)simple_strtoul(argv[1], NULL, 16);
	if (!confirm_key_prog())
		return CMD_RET_FAILURE;
	if (mmc_rpmb_set_key(mmc, key_addr)) {
		printf("ERROR - Key already programmed ?\n");
		return CMD_RET_FAILURE;
	}
	return CMD_RET_SUCCESS;
}

static int do_mmcrpmb_read(struct cmd_tbl *cmdtp, int flag,
			   int argc, char *const argv[])
{
	u16 blk, cnt;
	void *addr;
	int n;
	void *key_addr = NULL;
	struct mmc *mmc = find_mmc_device(curr_device);

	if (argc < 4)
		return CMD_RET_USAGE;

	addr = (void *)simple_strtoul(argv[1], NULL, 16);
	blk = simple_strtoul(argv[2], NULL, 16);
	cnt = simple_strtoul(argv[3], NULL, 16);

	if (argc == 5)
		key_addr = (void *)simple_strtoul(argv[4], NULL, 16);

	printf("\nMMC RPMB read: dev # %d, block # %d, count %d ... ",
	       curr_device, blk, cnt);
	n =  mmc_rpmb_read(mmc, addr, blk, cnt, key_addr);

	printf("%d RPMB blocks read: %s\n", n, (n == cnt) ? "OK" : "ERROR");
	if (n != cnt)
		return CMD_RET_FAILURE;
	return CMD_RET_SUCCESS;
}

static int do_mmcrpmb_write(struct cmd_tbl *cmdtp, int flag,
			    int argc, char *const argv[])
{
	u16 blk, cnt;
	void *addr;
	int n;
	void *key_addr;
	struct mmc *mmc = find_mmc_device(curr_device);

	if (argc != 5)
		return CMD_RET_USAGE;

	addr = (void *)simple_strtoul(argv[1], NULL, 16);
	blk = simple_strtoul(argv[2], NULL, 16);
	cnt = simple_strtoul(argv[3], NULL, 16);
	key_addr = (void *)simple_strtoul(argv[4], NULL, 16);

	printf("\nMMC RPMB write: dev # %d, block # %d, count %d ... ",
	       curr_device, blk, cnt);
	n =  mmc_rpmb_write(mmc, addr, blk, cnt, key_addr);

	printf("%d RPMB blocks written: %s\n", n, (n == cnt) ? "OK" : "ERROR");
	if (n != cnt)
		return CMD_RET_FAILURE;
	return CMD_RET_SUCCESS;
}

static int do_mmcrpmb_counter(struct cmd_tbl *cmdtp, int flag,
			      int argc, char *const argv[])
{
	unsigned long counter;
	struct mmc *mmc = find_mmc_device(curr_device);

	if (mmc_rpmb_get_counter(mmc, &counter))
		return CMD_RET_FAILURE;
	printf("RPMB Write counter= %lx\n", counter);
	return CMD_RET_SUCCESS;
}

static struct cmd_tbl cmd_rpmb[] = {
	U_BOOT_CMD_MKENT(key, 2, 0, do_mmcrpmb_key, "", ""),
	U_BOOT_CMD_MKENT(read, 5, 1, do_mmcrpmb_read, "", ""),
	U_BOOT_CMD_MKENT(write, 5, 0, do_mmcrpmb_write, "", ""),
	U_BOOT_CMD_MKENT(counter, 1, 1, do_mmcrpmb_counter, "", ""),
};

static int do_mmcrpmb(struct cmd_tbl *cmdtp, int flag,
		      int argc, char *const argv[])
{
	struct cmd_tbl *cp;
	struct mmc *mmc;
	char original_part;
	int ret;

	cp = find_cmd_tbl(argv[1], cmd_rpmb, ARRAY_SIZE(cmd_rpmb));

	/* Drop the rpmb subcommand */
	argc--;
	argv++;

	if (cp == NULL || argc > cp->maxargs)
		return CMD_RET_USAGE;
	if (flag == CMD_FLAG_REPEAT && !cmd_is_repeatable(cp))
		return CMD_RET_SUCCESS;

	mmc = init_mmc_device(curr_device, false);
	if (!mmc)
		return CMD_RET_FAILURE;

	if (!(mmc->version & MMC_VERSION_MMC)) {
		printf("It is not an eMMC device\n");
		return CMD_RET_FAILURE;
	}
	if (mmc->version < MMC_VERSION_4_41) {
		printf("RPMB not supported before version 4.41\n");
		return CMD_RET_FAILURE;
	}
	/* Switch to the RPMB partition */
#ifndef CONFIG_BLK
	original_part = mmc->block_dev.hwpart;
#else
	original_part = mmc_get_blk_desc(mmc)->hwpart;
#endif
	if (blk_select_hwpart_devnum(IF_TYPE_MMC, curr_device, MMC_PART_RPMB) !=
	    0)
		return CMD_RET_FAILURE;
	ret = cp->cmd(cmdtp, flag, argc, argv);

	/* Return to original partition */
	if (blk_select_hwpart_devnum(IF_TYPE_MMC, curr_device, original_part) !=
	    0)
		return CMD_RET_FAILURE;
	return ret;
}
#endif

static int do_mmc_read(struct cmd_tbl *cmdtp, int flag,
		       int argc, char *const argv[])
{
	struct mmc *mmc;
	u32 blk, cnt, n;
	void *addr;

	if (argc != 4)
		return CMD_RET_USAGE;

	addr = (void *)simple_strtoul(argv[1], NULL, 16);
	blk = simple_strtoul(argv[2], NULL, 16);
	cnt = simple_strtoul(argv[3], NULL, 16);

	mmc = init_mmc_device(curr_device, false);
	if (!mmc)
		return CMD_RET_FAILURE;

	printf("\nMMC read: dev # %d, block # %d, count %d ... ",
	       curr_device, blk, cnt);

	n = blk_dread(mmc_get_blk_desc(mmc), blk, cnt, addr);
	printf("%d blocks read: %s\n", n, (n == cnt) ? "OK" : "ERROR");

	return (n == cnt) ? CMD_RET_SUCCESS : CMD_RET_FAILURE;
}

#if CONFIG_IS_ENABLED(CMD_MMC_SWRITE)
static lbaint_t mmc_sparse_write(struct sparse_storage *info, lbaint_t blk,
				 lbaint_t blkcnt, const void *buffer)
{
	struct blk_desc *dev_desc = info->priv;

	return blk_dwrite(dev_desc, blk, blkcnt, buffer);
}

static lbaint_t mmc_sparse_reserve(struct sparse_storage *info,
				   lbaint_t blk, lbaint_t blkcnt)
{
	return blkcnt;
}

static int do_mmc_sparse_write(struct cmd_tbl *cmdtp, int flag,
			       int argc, char *const argv[])
{
	struct sparse_storage sparse;
	struct blk_desc *dev_desc;
	struct mmc *mmc;
	char dest[11];
	void *addr;
	u32 blk;

	if (argc != 3)
		return CMD_RET_USAGE;

	addr = (void *)simple_strtoul(argv[1], NULL, 16);
	blk = simple_strtoul(argv[2], NULL, 16);

	if (!is_sparse_image(addr)) {
		printf("Not a sparse image\n");
		return CMD_RET_FAILURE;
	}

	mmc = init_mmc_device(curr_device, false);
	if (!mmc)
		return CMD_RET_FAILURE;

	printf("\nMMC Sparse write: dev # %d, block # %d ... ",
	       curr_device, blk);

	if (mmc_getwp(mmc) == 1) {
		printf("Error: card is write protected!\n");
		return CMD_RET_FAILURE;
	}

	dev_desc = mmc_get_blk_desc(mmc);
	sparse.priv = dev_desc;
	sparse.blksz = 512;
	sparse.start = blk;
	sparse.size = dev_desc->lba - blk;
	sparse.write = mmc_sparse_write;
	sparse.reserve = mmc_sparse_reserve;
	sparse.mssg = NULL;
	sprintf(dest, "0x" LBAF, sparse.start * sparse.blksz);

	if (write_sparse_image(&sparse, dest, addr, NULL))
		return CMD_RET_FAILURE;
	else
		return CMD_RET_SUCCESS;
}
#endif

#if CONFIG_IS_ENABLED(MMC_WRITE)
static int do_mmc_write(struct cmd_tbl *cmdtp, int flag,
			int argc, char *const argv[])
{
	struct mmc *mmc;
	u32 blk, cnt, n;
	void *addr;

	if (argc != 4)
		return CMD_RET_USAGE;

	addr = (void *)simple_strtoul(argv[1], NULL, 16);
	blk = simple_strtoul(argv[2], NULL, 16);
	cnt = simple_strtoul(argv[3], NULL, 16);

	mmc = init_mmc_device(curr_device, false);
	if (!mmc)
		return CMD_RET_FAILURE;

	printf("\nMMC write: dev # %d, block # %d, count %d ... ",
	       curr_device, blk, cnt);

	if (mmc_getwp(mmc) == 1) {
		printf("Error: card is write protected!\n");
		return CMD_RET_FAILURE;
	}
	n = blk_dwrite(mmc_get_blk_desc(mmc), blk, cnt, addr);
	printf("%d blocks written: %s\n", n, (n == cnt) ? "OK" : "ERROR");

	return (n == cnt) ? CMD_RET_SUCCESS : CMD_RET_FAILURE;
}

static int do_mmc_erase(struct cmd_tbl *cmdtp, int flag,
			int argc, char *const argv[])
{
	struct mmc *mmc;
	u32 blk, cnt, n;

	if (argc != 3)
		return CMD_RET_USAGE;

	blk = simple_strtoul(argv[1], NULL, 16);
	cnt = simple_strtoul(argv[2], NULL, 16);

	mmc = init_mmc_device(curr_device, false);
	if (!mmc)
		return CMD_RET_FAILURE;

	printf("\nMMC erase: dev # %d, block # %d, count %d ... ",
	       curr_device, blk, cnt);

	if (mmc_getwp(mmc) == 1) {
		printf("Error: card is write protected!\n");
		return CMD_RET_FAILURE;
	}
	n = blk_derase(mmc_get_blk_desc(mmc), blk, cnt);
	printf("%d blocks erased: %s\n", n, (n == cnt) ? "OK" : "ERROR");

	return (n == cnt) ? CMD_RET_SUCCESS : CMD_RET_FAILURE;
}
#endif

static int do_mmc_rescan(struct cmd_tbl *cmdtp, int flag,
			 int argc, char *const argv[])
{
	struct mmc *mmc;

	mmc = init_mmc_device(curr_device, true);
	if (!mmc)
		return CMD_RET_FAILURE;

	return CMD_RET_SUCCESS;
}

static int do_mmc_part(struct cmd_tbl *cmdtp, int flag,
		       int argc, char *const argv[])
{
	struct blk_desc *mmc_dev;
	struct mmc *mmc;

	mmc = init_mmc_device(curr_device, false);
	if (!mmc)
		return CMD_RET_FAILURE;

	mmc_dev = blk_get_devnum_by_type(IF_TYPE_MMC, curr_device);
	if (mmc_dev != NULL && mmc_dev->type != DEV_TYPE_UNKNOWN) {
		part_print(mmc_dev);
		return CMD_RET_SUCCESS;
	}

	puts("get mmc type error!\n");
	return CMD_RET_FAILURE;
}

static int do_mmc_dev(struct cmd_tbl *cmdtp, int flag,
		      int argc, char *const argv[])
{
	int dev, part = 0, ret;
	struct mmc *mmc;

	if (argc == 1) {
		dev = curr_device;
	} else if (argc == 2) {
		dev = simple_strtoul(argv[1], NULL, 10);
	} else if (argc == 3) {
		dev = (int)simple_strtoul(argv[1], NULL, 10);
		part = (int)simple_strtoul(argv[2], NULL, 10);
		if (part > PART_ACCESS_MASK) {
			printf("#part_num shouldn't be larger than %d\n",
			       PART_ACCESS_MASK);
			return CMD_RET_FAILURE;
		}
	} else {
		return CMD_RET_USAGE;
	}

	mmc = init_mmc_device(dev, true);
	if (!mmc)
		return CMD_RET_FAILURE;

	ret = blk_select_hwpart_devnum(IF_TYPE_MMC, dev, part);
	printf("switch to partitions #%d, %s\n",
	       part, (!ret) ? "OK" : "ERROR");
	if (ret)
		return 1;

	curr_device = dev;
	if (mmc->part_config == MMCPART_NOAVAILABLE)
		printf("mmc%d is current device\n", curr_device);
	else
		printf("mmc%d(part %d) is current device\n",
		       curr_device, mmc_get_blk_desc(mmc)->hwpart);

	return CMD_RET_SUCCESS;
}

static int do_mmc_list(struct cmd_tbl *cmdtp, int flag,
		       int argc, char *const argv[])
{
	print_mmc_devices('\n');
	return CMD_RET_SUCCESS;
}

#if CONFIG_IS_ENABLED(MMC_HW_PARTITIONING)
static int parse_hwpart_user(struct mmc_hwpart_conf *pconf,
			     int argc, char *const argv[])
{
	int i = 0;

	memset(&pconf->user, 0, sizeof(pconf->user));

	while (i < argc) {
		if (!strcmp(argv[i], "enh")) {
			if (i + 2 >= argc)
				return -1;
			pconf->user.enh_start =
				simple_strtoul(argv[i+1], NULL, 10);
			pconf->user.enh_size =
				simple_strtoul(argv[i+2], NULL, 10);
			i += 3;
		} else if (!strcmp(argv[i], "wrrel")) {
			if (i + 1 >= argc)
				return -1;
			pconf->user.wr_rel_change = 1;
			if (!strcmp(argv[i+1], "on"))
				pconf->user.wr_rel_set = 1;
			else if (!strcmp(argv[i+1], "off"))
				pconf->user.wr_rel_set = 0;
			else
				return -1;
			i += 2;
		} else {
			break;
		}
	}
	return i;
}

static int parse_hwpart_gp(struct mmc_hwpart_conf *pconf, int pidx,
			   int argc, char *const argv[])
{
	int i;

	memset(&pconf->gp_part[pidx], 0, sizeof(pconf->gp_part[pidx]));

	if (1 >= argc)
		return -1;
	pconf->gp_part[pidx].size = simple_strtoul(argv[0], NULL, 10);

	i = 1;
	while (i < argc) {
		if (!strcmp(argv[i], "enh")) {
			pconf->gp_part[pidx].enhanced = 1;
			i += 1;
		} else if (!strcmp(argv[i], "wrrel")) {
			if (i + 1 >= argc)
				return -1;
			pconf->gp_part[pidx].wr_rel_change = 1;
			if (!strcmp(argv[i+1], "on"))
				pconf->gp_part[pidx].wr_rel_set = 1;
			else if (!strcmp(argv[i+1], "off"))
				pconf->gp_part[pidx].wr_rel_set = 0;
			else
				return -1;
			i += 2;
		} else {
			break;
		}
	}
	return i;
}

static int do_mmc_hwpartition(struct cmd_tbl *cmdtp, int flag,
			      int argc, char *const argv[])
{
	struct mmc *mmc;
	struct mmc_hwpart_conf pconf = { };
	enum mmc_hwpart_conf_mode mode = MMC_HWPART_CONF_CHECK;
	int i, r, pidx;

	mmc = init_mmc_device(curr_device, false);
	if (!mmc)
		return CMD_RET_FAILURE;

	if (argc < 1)
		return CMD_RET_USAGE;
	i = 1;
	while (i < argc) {
		if (!strcmp(argv[i], "user")) {
			i++;
			r = parse_hwpart_user(&pconf, argc-i, &argv[i]);
			if (r < 0)
				return CMD_RET_USAGE;
			i += r;
		} else if (!strncmp(argv[i], "gp", 2) &&
			   strlen(argv[i]) == 3 &&
			   argv[i][2] >= '1' && argv[i][2] <= '4') {
			pidx = argv[i][2] - '1';
			i++;
			r = parse_hwpart_gp(&pconf, pidx, argc-i, &argv[i]);
			if (r < 0)
				return CMD_RET_USAGE;
			i += r;
		} else if (!strcmp(argv[i], "check")) {
			mode = MMC_HWPART_CONF_CHECK;
			i++;
		} else if (!strcmp(argv[i], "set")) {
			mode = MMC_HWPART_CONF_SET;
			i++;
		} else if (!strcmp(argv[i], "complete")) {
			mode = MMC_HWPART_CONF_COMPLETE;
			i++;
		} else {
			return CMD_RET_USAGE;
		}
	}

	puts("Partition configuration:\n");
	if (pconf.user.enh_size) {
		puts("\tUser Enhanced Start: ");
		print_size(((u64)pconf.user.enh_start) << 9, "\n");
		puts("\tUser Enhanced Size: ");
		print_size(((u64)pconf.user.enh_size) << 9, "\n");
	} else {
		puts("\tNo enhanced user data area\n");
	}
	if (pconf.user.wr_rel_change)
		printf("\tUser partition write reliability: %s\n",
		       pconf.user.wr_rel_set ? "on" : "off");
	for (pidx = 0; pidx < 4; pidx++) {
		if (pconf.gp_part[pidx].size) {
			printf("\tGP%i Capacity: ", pidx+1);
			print_size(((u64)pconf.gp_part[pidx].size) << 9,
				   pconf.gp_part[pidx].enhanced ?
				   " ENH\n" : "\n");
		} else {
			printf("\tNo GP%i partition\n", pidx+1);
		}
		if (pconf.gp_part[pidx].wr_rel_change)
			printf("\tGP%i write reliability: %s\n", pidx+1,
			       pconf.gp_part[pidx].wr_rel_set ? "on" : "off");
	}

	if (!mmc_hwpart_config(mmc, &pconf, mode)) {
		if (mode == MMC_HWPART_CONF_COMPLETE)
			puts("Partitioning successful, "
			     "power-cycle to make effective\n");
		return CMD_RET_SUCCESS;
	} else {
		puts("Failed!\n");
		return CMD_RET_FAILURE;
	}
}
#endif

#ifdef CONFIG_SUPPORT_EMMC_BOOT
static int do_mmc_bootbus(struct cmd_tbl *cmdtp, int flag,
			  int argc, char *const argv[])
{
	int dev;
	struct mmc *mmc;
	u8 width, reset, mode;

	if (argc != 5)
		return CMD_RET_USAGE;
	dev = simple_strtoul(argv[1], NULL, 10);
	width = simple_strtoul(argv[2], NULL, 10);
	reset = simple_strtoul(argv[3], NULL, 10);
	mode = simple_strtoul(argv[4], NULL, 10);

	mmc = init_mmc_device(dev, false);
	if (!mmc)
		return CMD_RET_FAILURE;

	if (IS_SD(mmc)) {
		puts("BOOT_BUS_WIDTH only exists on eMMC\n");
		return CMD_RET_FAILURE;
	}

	/* acknowledge to be sent during boot operation */
	return mmc_set_boot_bus_width(mmc, width, reset, mode);
}

static int do_mmc_boot_resize(struct cmd_tbl *cmdtp, int flag,
			      int argc, char *const argv[])
{
	int dev;
	struct mmc *mmc;
	u32 bootsize, rpmbsize;

	if (argc != 4)
		return CMD_RET_USAGE;
	dev = simple_strtoul(argv[1], NULL, 10);
	bootsize = simple_strtoul(argv[2], NULL, 10);
	rpmbsize = simple_strtoul(argv[3], NULL, 10);

	mmc = init_mmc_device(dev, false);
	if (!mmc)
		return CMD_RET_FAILURE;

	if (IS_SD(mmc)) {
		printf("It is not an eMMC device\n");
		return CMD_RET_FAILURE;
	}

	if (mmc_boot_partition_size_change(mmc, bootsize, rpmbsize)) {
		printf("EMMC boot partition Size change Failed.\n");
		return CMD_RET_FAILURE;
	}

	printf("EMMC boot partition Size %d MB\n", bootsize);
	printf("EMMC RPMB partition Size %d MB\n", rpmbsize);
	return CMD_RET_SUCCESS;
}

static int mmc_partconf_print(struct mmc *mmc)
{
	u8 ack, access, part;

	if (mmc->part_config == MMCPART_NOAVAILABLE) {
		printf("No part_config info for ver. 0x%x\n", mmc->version);
		return CMD_RET_FAILURE;
	}

	access = EXT_CSD_EXTRACT_PARTITION_ACCESS(mmc->part_config);
	ack = EXT_CSD_EXTRACT_BOOT_ACK(mmc->part_config);
	part = EXT_CSD_EXTRACT_BOOT_PART(mmc->part_config);

	printf("EXT_CSD[179], PARTITION_CONFIG:\n"
		"BOOT_ACK: 0x%x\n"
		"BOOT_PARTITION_ENABLE: 0x%x\n"
		"PARTITION_ACCESS: 0x%x\n", ack, part, access);

	return CMD_RET_SUCCESS;
}

static int do_mmc_partconf(struct cmd_tbl *cmdtp, int flag,
			   int argc, char *const argv[])
{
	int dev;
	struct mmc *mmc;
	u8 ack, part_num, access;

	if (argc != 2 && argc != 5)
		return CMD_RET_USAGE;

	dev = simple_strtoul(argv[1], NULL, 10);

	mmc = init_mmc_device(dev, false);
	if (!mmc)
		return CMD_RET_FAILURE;

	if (IS_SD(mmc)) {
		puts("PARTITION_CONFIG only exists on eMMC\n");
		return CMD_RET_FAILURE;
	}

	if (argc == 2)
		return mmc_partconf_print(mmc);

	ack = simple_strtoul(argv[2], NULL, 10);
	part_num = simple_strtoul(argv[3], NULL, 10);
	access = simple_strtoul(argv[4], NULL, 10);

	/* acknowledge to be sent during boot operation */
	return mmc_set_part_conf(mmc, ack, part_num, access);
}

static int do_mmc_rst_func(struct cmd_tbl *cmdtp, int flag,
			   int argc, char *const argv[])
{
	int dev;
	struct mmc *mmc;
	u8 enable;

	/*
	 * Set the RST_n_ENABLE bit of RST_n_FUNCTION
	 * The only valid values are 0x0, 0x1 and 0x2 and writing
	 * a value of 0x1 or 0x2 sets the value permanently.
	 */
	if (argc != 3)
		return CMD_RET_USAGE;

	dev = simple_strtoul(argv[1], NULL, 10);
	enable = simple_strtoul(argv[2], NULL, 10);

	if (enable > 2) {
		puts("Invalid RST_n_ENABLE value\n");
		return CMD_RET_USAGE;
	}

	mmc = init_mmc_device(dev, false);
	if (!mmc)
		return CMD_RET_FAILURE;

	if (IS_SD(mmc)) {
		puts("RST_n_FUNCTION only exists on eMMC\n");
		return CMD_RET_FAILURE;
	}

	return mmc_set_rst_n_function(mmc, enable);
}
#endif
static int do_mmc_setdsr(struct cmd_tbl *cmdtp, int flag,
			 int argc, char *const argv[])
{
	struct mmc *mmc;
	u32 val;
	int ret;

	if (argc != 2)
		return CMD_RET_USAGE;
	val = simple_strtoul(argv[1], NULL, 16);

	mmc = find_mmc_device(curr_device);
	if (!mmc) {
		printf("no mmc device at slot %x\n", curr_device);
		return CMD_RET_FAILURE;
	}
	ret = mmc_set_dsr(mmc, val);
	printf("set dsr %s\n", (!ret) ? "OK, force rescan" : "ERROR");
	if (!ret) {
		mmc->has_init = 0;
		if (mmc_init(mmc))
			return CMD_RET_FAILURE;
		else
			return CMD_RET_SUCCESS;
	}
	return ret;
}

#ifdef CONFIG_CMD_BKOPS_ENABLE
static int do_mmc_bkops_enable(struct cmd_tbl *cmdtp, int flag,
			       int argc, char *const argv[])
{
	int dev;
	struct mmc *mmc;

	if (argc != 2)
		return CMD_RET_USAGE;

	dev = simple_strtoul(argv[1], NULL, 10);

	mmc = init_mmc_device(dev, false);
	if (!mmc)
		return CMD_RET_FAILURE;

	if (IS_SD(mmc)) {
		puts("BKOPS_EN only exists on eMMC\n");
		return CMD_RET_FAILURE;
	}

	return mmc_set_bkops_enable(mmc);
}
#endif

static int do_mmc_boot_wp(struct cmd_tbl *cmdtp, int flag,
			  int argc, char * const argv[])
{
	int err;
	struct mmc *mmc;

	mmc = init_mmc_device(curr_device, false);
	if (!mmc)
		return CMD_RET_FAILURE;
	if (IS_SD(mmc)) {
		printf("It is not an eMMC device\n");
		return CMD_RET_FAILURE;
	}
	err = mmc_boot_wp(mmc);
	if (err)
		return CMD_RET_FAILURE;
	printf("boot areas protected\n");
	return CMD_RET_SUCCESS;
}

static struct cmd_tbl cmd_mmc[] = {
	U_BOOT_CMD_MKENT(info, 1, 0, do_mmcinfo, "", ""),
	U_BOOT_CMD_MKENT(read, 4, 1, do_mmc_read, "", ""),
	U_BOOT_CMD_MKENT(wp, 1, 0, do_mmc_boot_wp, "", ""),
#if CONFIG_IS_ENABLED(MMC_WRITE)
	U_BOOT_CMD_MKENT(write, 4, 0, do_mmc_write, "", ""),
	U_BOOT_CMD_MKENT(erase, 3, 0, do_mmc_erase, "", ""),
#endif
#if CONFIG_IS_ENABLED(CMD_MMC_SWRITE)
	U_BOOT_CMD_MKENT(swrite, 3, 0, do_mmc_sparse_write, "", ""),
#endif
	U_BOOT_CMD_MKENT(rescan, 1, 1, do_mmc_rescan, "", ""),
	U_BOOT_CMD_MKENT(part, 1, 1, do_mmc_part, "", ""),
	U_BOOT_CMD_MKENT(dev, 3, 0, do_mmc_dev, "", ""),
	U_BOOT_CMD_MKENT(list, 1, 1, do_mmc_list, "", ""),
#if CONFIG_IS_ENABLED(MMC_HW_PARTITIONING)
	U_BOOT_CMD_MKENT(hwpartition, 28, 0, do_mmc_hwpartition, "", ""),
#endif
#ifdef CONFIG_SUPPORT_EMMC_BOOT
	U_BOOT_CMD_MKENT(bootbus, 5, 0, do_mmc_bootbus, "", ""),
	U_BOOT_CMD_MKENT(bootpart-resize, 4, 0, do_mmc_boot_resize, "", ""),
	U_BOOT_CMD_MKENT(partconf, 5, 0, do_mmc_partconf, "", ""),
	U_BOOT_CMD_MKENT(rst-function, 3, 0, do_mmc_rst_func, "", ""),
#endif
#if CONFIG_IS_ENABLED(CMD_MMC_RPMB)
	U_BOOT_CMD_MKENT(rpmb, CONFIG_SYS_MAXARGS, 1, do_mmcrpmb, "", ""),
#endif
	U_BOOT_CMD_MKENT(setdsr, 2, 0, do_mmc_setdsr, "", ""),
#ifdef CONFIG_CMD_BKOPS_ENABLE
	U_BOOT_CMD_MKENT(bkops-enable, 2, 0, do_mmc_bkops_enable, "", ""),
#endif
};

static int do_mmcops(struct cmd_tbl *cmdtp, int flag, int argc,
		     char *const argv[])
{
	struct cmd_tbl *cp;

	cp = find_cmd_tbl(argv[1], cmd_mmc, ARRAY_SIZE(cmd_mmc));

	/* Drop the mmc command */
	argc--;
	argv++;

	if (cp == NULL || argc > cp->maxargs)
		return CMD_RET_USAGE;
	if (flag == CMD_FLAG_REPEAT && !cmd_is_repeatable(cp))
		return CMD_RET_SUCCESS;

	if (curr_device < 0) {
		if (get_mmc_num() > 0) {
			curr_device = 0;
		} else {
			puts("No MMC device available\n");
			return CMD_RET_FAILURE;
		}
	}
	return cp->cmd(cmdtp, flag, argc, argv);
}

U_BOOT_CMD(
	mmc, 29, 1, do_mmcops,
	"MMC sub system",
	"info - display info of the current MMC device\n"
	"mmc read addr blk# cnt\n"
	"mmc write addr blk# cnt\n"
#if CONFIG_IS_ENABLED(CMD_MMC_SWRITE)
	"mmc swrite addr blk#\n"
#endif
	"mmc erase blk# cnt\n"
	"mmc rescan\n"
	"mmc part - lists available partition on current mmc device\n"
	"mmc dev [dev] [part] - show or set current mmc device [partition]\n"
	"mmc list - lists available devices\n"
	"mmc wp - power on write protect boot partitions\n"
#if CONFIG_IS_ENABLED(MMC_HW_PARTITIONING)
	"mmc hwpartition [args...] - does hardware partitioning\n"
	"  arguments (sizes in 512-byte blocks):\n"
	"    [user [enh start cnt] [wrrel {on|off}]] - sets user data area attributes\n"
	"    [gp1|gp2|gp3|gp4 cnt [enh] [wrrel {on|off}]] - general purpose partition\n"
	"    [check|set|complete] - mode, complete set partitioning completed\n"
	"  WARNING: Partitioning is a write-once setting once it is set to complete.\n"
	"  Power cycling is required to initialize partitions after set to complete.\n"
#endif
#ifdef CONFIG_SUPPORT_EMMC_BOOT
	"mmc bootbus dev boot_bus_width reset_boot_bus_width boot_mode\n"
	" - Set the BOOT_BUS_WIDTH field of the specified device\n"
	"mmc bootpart-resize <dev> <boot part size MB> <RPMB part size MB>\n"
	" - Change sizes of boot and RPMB partitions of specified device\n"
	"mmc partconf dev [boot_ack boot_partition partition_access]\n"
	" - Show or change the bits of the PARTITION_CONFIG field of the specified device\n"
	"mmc rst-function dev value\n"
	" - Change the RST_n_FUNCTION field of the specified device\n"
	"   WARNING: This is a write-once field and 0 / 1 / 2 are the only valid values.\n"
#endif
#if CONFIG_IS_ENABLED(CMD_MMC_RPMB)
	"mmc rpmb read addr blk# cnt [address of auth-key] - block size is 256 bytes\n"
	"mmc rpmb write addr blk# cnt <address of auth-key> - block size is 256 bytes\n"
	"mmc rpmb key <address of auth-key> - program the RPMB authentication key.\n"
	"mmc rpmb counter - read the value of the write counter\n"
#endif
	"mmc setdsr <value> - set DSR register value\n"
#ifdef CONFIG_CMD_BKOPS_ENABLE
	"mmc bkops-enable <dev> - enable background operations handshake on device\n"
	"   WARNING: This is a write-once setting.\n"
#endif
	);

/* Old command kept for compatibility. Same as 'mmc info' */
U_BOOT_CMD(
	mmcinfo, 1, 0, do_mmcinfo,
	"display MMC info",
	"- display info of the current MMC device"
);

static unsigned int calc_unit(unsigned int length, SDInfo sdInfo)
{
	if (sdInfo.addr_mode == CHS_MODE)
		return ( (length / BLOCK_SIZE / sdInfo.unit + 1 ) * sdInfo.unit);
	else
		return ( (length / BLOCK_SIZE) );
}

static void encode_chs(int C, int H, int S, unsigned char *result)
{
	*result++ = (unsigned char) H;
	*result++ = (unsigned char) ( S + ((C & 0x00000300) >> 2) );
	*result   = (unsigned char) (C & 0x000000FF);
}

static void encode_partitionInfo(PartitionInfo partInfo, unsigned char *result)
{
	*result++ = partInfo.bootable;

	encode_chs(partInfo.C_start, partInfo.H_start, partInfo.S_start, result);
	result +=3;
	*result++ = partInfo.partitionId;

	encode_chs(partInfo.C_end, partInfo.H_end, partInfo.S_end, result);
	result += 3;

	memcpy(result, (unsigned char *)&(partInfo.block_start), 4);
	result += 4;

	memcpy(result, (unsigned char *)&(partInfo.block_count), 4);
}

static void decode_partitionInfo(unsigned char *in, PartitionInfo *partInfo)
{
	partInfo->bootable	= *in;
	partInfo->partitionId	= *(in + 4);

	memcpy((unsigned char *)&(partInfo->block_start), (in + 8), 4);
	memcpy((unsigned char *)&(partInfo->block_count), (in +12), 4);
}

static void get_SDInfo(int block_count, SDInfo *sdInfo)
{
    int C, H, S;

    int C_max = 1023, H_max = 255, S_max = 63;
    int H_start = 1, S_start = 1;
    int diff_min = 0, diff = 0;

    if(block_count >= _8_4GB)
            sdInfo->addr_mode = LBA_MODE;
    else
            sdInfo->addr_mode = CHS_MODE;

//-----------------------------------------------------
    if (sdInfo->addr_mode == CHS_MODE)
    {
        diff_min = C_max;

        for (H = H_start; H <= H_max; H++)
            for (S  = S_start; S <= S_max; S++)
            {
                C = block_count / (H * S);

                if ( (C <= C_max) )
                {
                    diff = C_max - C;
                    if (diff <= diff_min)
                    {
                        diff_min = diff;
                        sdInfo->C_end = C;
                        sdInfo->H_end = H;
                        sdInfo->S_end = S;
                    }
                }
            }
    }
//-----------------------------------------------------
    else
    {
        sdInfo->C_end = 1023;
        sdInfo->H_end = 254;
        sdInfo->S_end = 63;
    }

//-----------------------------------------------------
    sdInfo->C_start                 = 0;
    sdInfo->H_start                 = 1;
    sdInfo->S_start                 = 1;

    sdInfo->total_block_count       = block_count;
    sdInfo->available_block         = sdInfo->C_end * sdInfo->H_end * sdInfo->S_end;
    sdInfo->unit                    = sdInfo->H_end * sdInfo->S_end;
}

static void make_partitionInfo(int LBA_start, int count, SDInfo sdInfo, PartitionInfo *partInfo)
{
        unsigned int             temp = 0;
        unsigned int             _10MB_unit;

        partInfo->block_start   = LBA_start;

//-----------------------------------------------------
        if (sdInfo.addr_mode == CHS_MODE)
        {
            partInfo->C_start       = partInfo->block_start / (sdInfo.H_end * sdInfo.S_end);
            temp                    = partInfo->block_start % (sdInfo.H_end * sdInfo.S_end);
            partInfo->H_start       = temp / sdInfo.S_end;
            partInfo->S_start       = temp % sdInfo.S_end + 1;

            if (count == BLOCK_END)
            {
                _10MB_unit = calc_unit(_10MB, sdInfo);
                partInfo->block_end     = sdInfo.C_end * sdInfo.H_end * sdInfo.S_end - _10MB_unit - 1;
                partInfo->block_count   = partInfo->block_end - partInfo->block_start + 1;

                partInfo->C_end = partInfo->block_end / sdInfo.unit;
                partInfo->H_end = sdInfo.H_end - 1;
                partInfo->S_end = sdInfo.S_end;
            }
            else
            {
                partInfo->block_count   = count;

                partInfo->block_end     = partInfo->block_start + count - 1;
                partInfo->C_end         = partInfo->block_end / sdInfo.unit;

                temp                    = partInfo->block_end % sdInfo.unit;
                partInfo->H_end         = temp / sdInfo.S_end;
                partInfo->S_end         = temp % sdInfo.S_end + 1;
            }
        }
//-----------------------------------------------------
        else
        {
            partInfo->C_start       = 0;
            partInfo->H_start       = 1;
            partInfo->S_start       = 1;

            partInfo->C_end         = 1023;
            partInfo->H_end         = 254;
            partInfo->S_end         = 63;

            if (count == BLOCK_END)
            {
                _10MB_unit = calc_unit(_10MB, sdInfo);
                partInfo->block_end     = sdInfo.total_block_count - _10MB_unit - 1;
                partInfo->block_count   = partInfo->block_end - partInfo->block_start + 1;
            }
            else
            {
                partInfo->block_count   = count;
                partInfo->block_end     = partInfo->block_start + count - 1;
            }
        }
}


static int make_mmc_partition(int total_block_count, unsigned char *mbr, int flag, char * const argv[])
{
	unsigned int  block_start = 0, block_offset;
	SDInfo		  sdInfo;
	PartitionInfo partInfo[4];

	/*初始化sdinfo*/
	memset((unsigned char *)&sdInfo, 0x00, sizeof(SDInfo));

	/*根据容量大小生成sdInfo*/
	get_SDInfo(total_block_count, &sdInfo);

/*1*//*计算预留空间后的起始地址***************************************************/
	block_start	= calc_unit(DISK_START, sdInfo);

	/*分配分区空间大小*/
	if (flag)  /*动态*/
		block_offset = calc_unit((unsigned long long)simple_strtoul(argv[3], NULL, 0)*1024*1024, sdInfo);
	else       /*静态*/
		block_offset = calc_unit(SYSTEM_PART_SIZE, sdInfo);

	/*非活动分区*/
	partInfo[0].bootable	= 0x00;
	/*linux文件系统分区*/
	partInfo[0].partitionId	= 0x83;

	/*生成一项分区表*/
	make_partitionInfo(block_start, block_offset, sdInfo, &partInfo[0]);



/*2*//*计算下一分区启动地址***************************************************/
	block_start += block_offset;

	/*分配分区空间大小*/
	if (flag) /*动态*/
		block_offset = calc_unit((unsigned long long)simple_strtoul(argv[4], NULL, 0)*1024*1024, sdInfo);
	else      /*静态*/
	{
		if (strcmp(argv[2], "1") == 0)// TF card
			block_offset = calc_unit(_300MB, sdInfo);
		else
			block_offset = calc_unit(USER_DATA_PART_SIZE, sdInfo);
	}

	/*非活动分区*/
	partInfo[1].bootable	= 0x00;
	/*linux文件系统分区*/
	partInfo[1].partitionId	= 0x83;
	/*生成一项分区表*/
	make_partitionInfo(block_start, block_offset, sdInfo, &partInfo[1]);



/*3*//*计算下一分区启动地址***************************************************/
	block_start += block_offset;

	/*分配分区空间大小*/
	if(flag)  /*动态*/
		block_offset = calc_unit((unsigned long long)simple_strtoul(argv[5], NULL, 0)*1024*1024, sdInfo);
	else      /*静态*/
		block_offset = calc_unit(CACHE_PART_SIZE, sdInfo);

	/*非活动分区*/
	partInfo[2].bootable	= 0x00;
	/*linux文件系统分区*/
	partInfo[2].partitionId	= 0x83;
	/*生成一项分区表*/
	make_partitionInfo(block_start, block_offset, sdInfo, &partInfo[2]);



/*4*//*计算最终分区启动地址***************************************************/
	block_start += block_offset;
	/*剩余所有容量*/
	block_offset = BLOCK_END;

	/*非活动分区*/
	partInfo[3].bootable	= 0x00;
	/*增强型FAT32文件系统*/
	partInfo[3].partitionId	= 0x0C;
	/*生成一项分区表*/
	make_partitionInfo(block_start, block_offset, sdInfo, &partInfo[3]);

	/*生成mbr分区表*/
	memset(mbr, 0x00, sizeof(*mbr)*512);// liang, clean the mem again
	/*mbr分区表尾标志*/
	mbr[510] = 0x55; mbr[511] = 0xAA;

	/*填入四项分区表*/
	encode_partitionInfo(partInfo[0], &mbr[0x1CE]);/*分区2*/
	encode_partitionInfo(partInfo[1], &mbr[0x1DE]);/*分区3*/
	encode_partitionInfo(partInfo[2], &mbr[0x1EE]);/*分区4*/
	encode_partitionInfo(partInfo[3], &mbr[0x1BE]);/*分区1*/

	return 0;
}

static int get_mmc_block_count(char *device_name)
{
	struct mmc *mmc;
	int block_count = 0;
	int dev_num;

	dev_num = simple_strtoul(device_name, NULL, 0);

	mmc = find_mmc_device(dev_num);
	if (!mmc)
	{
		printf("mmc/sd device is NOT founded.\n");
		return -1;
	}

	//block_count = mmc->capacity * (mmc->read_bl_len / BLOCK_SIZE);
	block_count = mmc->capacity / BLOCK_SIZE;

//	printf("block_count = %d\n", block_count);
	return block_count;
}

static int get_mmc_mbr(char *device_name, unsigned char *mbr)
{
	int rv;
	struct mmc *mmc;
	int dev_num;

	dev_num = simple_strtoul(device_name, NULL, 0);

	mmc = find_mmc_device(dev_num);
	if (!mmc)
	{
		printf("mmc/sd device is NOT founded.\n");
		return -1;
	}

	rv = blk_dread(mmc_get_blk_desc(mmc), 0, 1, mbr);
	//rv = mmc->block_dev.block_read(dev_num, 0, 1, mbr);

	if(rv == 1)
		return 0;
	else
		return -1;
}

static int put_mmc_mbr(unsigned char *mbr, char *device_name)
{
	int rv;
	struct mmc *mmc;
	int dev_num;

	dev_num = simple_strtoul(device_name, NULL, 0);
	mmc = find_mmc_device(dev_num);

	if (!mmc)
	{
		printf("mmc/sd device is NOT founded.\n");
		return -1;
	}

	rv = blk_dwrite(mmc_get_blk_desc(mmc), 0, 1, mbr);
	//rv = mmc->block_dev.block_write(dev_num, 0, 1, mbr);

	if(rv == 1)
		return 0;
	else
		return -1;
}

static int get_mmc_part_info(char *device_name, int part_num, int *block_start, int *block_count, unsigned char *part_Id)
{
	int		rv;
	PartitionInfo	partInfo;
	unsigned char	mbr[512];

	rv = get_mmc_mbr(device_name, mbr);
	if(rv !=0)
		return -1;

	switch(part_num)
	{
		case 1:
			decode_partitionInfo(&mbr[0x1BE], &partInfo);
			*block_start	= partInfo.block_start;
			*block_count	= partInfo.block_count;
			*part_Id 	= partInfo.partitionId;
			break;
		case 2:
			decode_partitionInfo(&mbr[0x1CE], &partInfo);
			*block_start	= partInfo.block_start;
			*block_count	= partInfo.block_count;
			*part_Id 	= partInfo.partitionId;
			break;

		case 3:
			decode_partitionInfo(&mbr[0x1DE], &partInfo);
			*block_start	= partInfo.block_start;
			*block_count	= partInfo.block_count;
			*part_Id 	= partInfo.partitionId;
			break;
		case 4:
			decode_partitionInfo(&mbr[0x1EE], &partInfo);
			*block_start	= partInfo.block_start;
			*block_count	= partInfo.block_count;
			*part_Id 	= partInfo.partitionId;
			break;
		default:
			return -1;
	}

	return 0;
}


static int print_mmc_part_info(int argc, char *const argv[])
{
	int		rv;
	PartitionInfo	partInfo[4];

	rv = get_mmc_part_info(argv[2], 1, &(partInfo[0].block_start), &(partInfo[0].block_count),
			&(partInfo[0].partitionId) );

	rv = get_mmc_part_info(argv[2], 2, &(partInfo[1].block_start), &(partInfo[1].block_count),
			&(partInfo[1].partitionId) );

	rv = get_mmc_part_info(argv[2], 3, &(partInfo[2].block_start), &(partInfo[2].block_count),
			&(partInfo[2].partitionId) );

	rv = get_mmc_part_info(argv[2], 4, &(partInfo[3].block_start), &(partInfo[3].block_count),
			&(partInfo[3].partitionId) );

	printf("\n");
	printf("partion #    size(MB)     block start #    block count    partition_Id \n");

	if ( (partInfo[0].block_start !=0) && (partInfo[0].block_count != 0) )
		printf("   1        %6d         %8d        %8d          0x%.2X \n",
			(partInfo[0].block_count / 2048), partInfo[0].block_start,
			partInfo[0].block_count, partInfo[0].partitionId);

	if ( (partInfo[1].block_start !=0) && (partInfo[1].block_count != 0) )
		printf("   2        %6d         %8d        %8d          0x%.2X \n",
			(partInfo[1].block_count / 2048), partInfo[1].block_start,
			partInfo[1].block_count, partInfo[1].partitionId);

	if ( (partInfo[2].block_start !=0) && (partInfo[2].block_count != 0) )
		printf("   3        %6d         %8d        %8d          0x%.2X \n",
			(partInfo[2].block_count / 2048), partInfo[2].block_start,
			partInfo[2].block_count, partInfo[2].partitionId);

	if ( (partInfo[3].block_start !=0) && (partInfo[3].block_count != 0) )
		printf("   4        %6d         %8d        %8d          0x%.2X \n",
			(partInfo[3].block_count / 2048), partInfo[3].block_start,
			partInfo[3].block_count, partInfo[3].partitionId);

	return 1;
}

static int create_mmc_fdisk(int argc, char *const argv[])
{
	int		rv;
	int		total_block_count;
	unsigned char	mbr[512];
	memset(mbr, 0x00, 512);

	total_block_count = get_mmc_block_count(argv[2]);
	if (total_block_count < 0)
		return -1;
	make_mmc_partition(total_block_count, mbr, (argc==6?1:0), argv);

	rv = put_mmc_mbr(mbr, argv[2]);
	if (rv != 0)
		return -1;

	printf("fdisk is completed\n");

	argv[1][1] = 'p';
	print_mmc_part_info(argc, argv);
	return 0;
}

static int do_fdisk(struct cmd_tbl *cmdtp, int flag, int argc,
		      char *const argv[])
{
	if ( argc == 3 || argc ==6 )
	{
		if ( strcmp(argv[1], "-c") == 0 )
			return create_mmc_fdisk(argc, argv);
		else if ( strcmp(argv[1], "-p") == 0 )
			return print_mmc_part_info(argc, argv);
	}
	else
	{
		printf("Usage:\nfdisk <-p> <device_num>\n");
		printf("fdisk <-c> <device_num> [<sys. part size(MB)> <user part size> <cache part size>]\n");
	}
	return 0;
}



U_BOOT_CMD(
	fdisk, 6, 0, do_fdisk,
	"fdisk\t- fdisk for sd/mmc.\n",
	"-c <device_num> [<sys. part size(MB)> <user part size> <cache part size>]\t- create partition.\n"
	"fdisk -p <device_num>\t- print partition information\n"
);


#define I2SCON		0x0
#define I2SMOD		0x4
#define I2SFIC		0x8
#define I2SPSR		0xc
#define I2STXD		0x10
#define I2SRXD		0x14
#define I2SFICS		0x18
#define I2STXDS		0x1c
#define I2SAHB		0x20
#define I2SSTR0		0x24
#define I2SSIZE		0x28
#define I2STRNCNT	0x2c
#define I2SLVL0ADDR	0x30
#define I2SLVL1ADDR	0x34
#define I2SLVL2ADDR	0x38
#define I2SLVL3ADDR	0x3c
#define I2SSTR1		0x40
#define I2SVER		0x44
#define I2SFIC1		0x48
#define I2STDM		0x4c
#define I2SFSTA		0x50



static void i2s_debug1(void)
{
	void *addr = (unsigned int *)0x03830000;
	u32 con = readl(addr + I2SCON);
	u32 mod = readl(addr + I2SMOD);
	u32 fic = readl(addr + I2SFIC);
	u32 psr = readl(addr + I2SPSR);
	u32 ahb = readl(addr + I2SAHB);
	u32 str0 = readl(addr + I2SSTR0);
	u32 lvl0 = readl(addr + I2SLVL0ADDR);
	u32 lvl1 = readl(addr + I2SLVL1ADDR);
	u32 lvl2 = readl(addr + I2SLVL2ADDR);
	u32 lvl3 = readl(addr + I2SLVL3ADDR);
	u32 str1 = readl(addr + I2SSTR1);


	printk("-----------------------------------------------------------------------------------------------------------\n");
	printk("i2s---reg con:0x%x    mod:0x%x   fic:0x%x  psr:0x%x   ahb:0x%x   str0:0x%x\n", con, mod, fic, psr, ahb, str0);
	printk("i2s---reg lvl0:0x%x   lvl1:0x%x    lvl2:0x%x    lvl3:0x%x   str1:0x%x\n", lvl0, lvl1, lvl2, lvl3, str1);
	printk("-----------------------------------------------------------------------------------------------------------\n");
}


static void i2s_ass_debug(void)
{
    void *ass_reg = (unsigned int *)0x03810000;

    u32 asc = readl(ass_reg);
	u32 acd = readl(ass_reg+0x04);
	u32 acg = readl(ass_reg+0x08);

    printk("**********************************************************************************************************\n");
	printk("asc:0x%x  acd:0x%x	acg:0x%x!\n", asc, acd, acg);
	printk("**********************************************************************************************************\n");
}

static int do_i2s(struct cmd_tbl *cmdtp, int flag, int argc,
		      char *const argv[])
{
	void *i2s_reg = (unsigned int *)0x03830000;
	void *ass_reg = (unsigned int *)0x03810000;

	u32 asc = 0<<2 | 0<<0;
	writel(asc, ass_reg);

	u32 acd = 0<<8 | 0<<4 | 0<<0;
	writel(acd, ass_reg+0x04);

	u32 acg = 1<<8 | 1<<7 | 1<<6 | 1<<3 | 1<<2 | 1<<1 | 1<<0;
	writel(acg, ass_reg+0x08);
    i2s_ass_debug();
	//printk("asc:0x%x  acd:0x%x	acg:0x%x!\n", asc, acd, acg);


	u32 mod = 3<<30 | 0<<12 | 0<<1;
	writel(mod, i2s_reg+I2SMOD);

	u32 psr = 1<<15 | 0<<8;
	writel(psr, i2s_reg+I2SPSR);

	u32 con = 1<<31 | 1<<0;
	writel(con, i2s_reg);
	i2s_debug1();

	return 0;
}



U_BOOT_CMD(
	runi2s, 6, 0, do_i2s,
	"fdisk\t- fdisk for sd/mmc.\n",
	"-c <device_num> [<sys. part size(MB)> <user part size> <cache part size>]\t- create partition.\n"
	"fdisk -p <device_num>\t- print partition information\n"
);


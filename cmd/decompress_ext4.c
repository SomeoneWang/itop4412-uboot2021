/* copyright (c) 2010 Samsung Electronics Co., Ltd.
 *              http://www.samsung.com/
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
*/

#include <common.h>
#include <part.h>
#include <config.h>
#include <command.h>
#include <image.h>
#include <linux/ctype.h>
#include <asm/byteorder.h>
#include <asm/io.h>
#include <asm/types.h>
#include <mmc.h>

#define SECTOR_BITS		9	/* 512B */

#define EXT4_FILE_HEADER_MAGIC	0xED26FF3A
#define EXT4_FILE_HEADER_MAJOR	0x0001
#define EXT4_FILE_HEADER_MINOR	0x0000
#define EXT4_FILE_BLOCK_SIZE	0x1000

#define EXT4_FILE_HEADER_SIZE	(sizeof(struct _ext4_file_header))
#define EXT4_CHUNK_HEADER_SIZE	(sizeof(struct _ext4_chunk_header))


#define EXT4_CHUNK_TYPE_RAW			0xCAC1
#define EXT4_CHUNK_TYPE_FILL		0xCAC2
#define EXT4_CHUNK_TYPE_NONE		0xCAC3

typedef struct _ext4_file_header {
	unsigned int magic;
	unsigned short major;
	unsigned short minor;
	unsigned short file_header_size;
	unsigned short chunk_header_size;
	unsigned int block_size;
	unsigned int total_blocks;
	unsigned int total_chunks;
	unsigned int crc32;
}ext4_file_header;


typedef struct _ext4_chunk_header {
	unsigned short type;
	unsigned short reserved;
	unsigned int chunk_size;
	unsigned int total_size;
}ext4_chunk_header;


static struct blk_desc *fs_dev_desc;
static int fs_dev_part;
static struct disk_partition fs_partition;


static int write_raw_chunk(char* data, unsigned int sector, unsigned int sector_size, struct blk_desc * dev);


static int check_compress_ext4(char *img_base, unsigned int parti_size) {
	ext4_file_header *file_header;

	file_header = (ext4_file_header*)img_base;

	if (file_header->magic != EXT4_FILE_HEADER_MAGIC) {
		return -1;
	}

	if (file_header->major != EXT4_FILE_HEADER_MAJOR) {
		printf("Invalid Version Info! 0x%2x\n", file_header->major);
		return -1;
	}

	if (file_header->file_header_size != EXT4_FILE_HEADER_SIZE) {
		printf("Invalid File Header Size! 0x%8x\n",
								file_header->file_header_size);
		return -1;
	}

	if (file_header->chunk_header_size != EXT4_CHUNK_HEADER_SIZE) {
		printf("Invalid Chunk Header Size! 0x%8x\n",
								file_header->chunk_header_size);
		return -1;
	}

	if (file_header->block_size != EXT4_FILE_BLOCK_SIZE) {
		printf("Invalid Block Size! 0x%8x\n", file_header->block_size);
		return -1;
	}

	if ((parti_size/file_header->block_size)  < file_header->total_blocks) {
		printf("Invalid Volume Size! Image is bigger than partition size!\n");
		printf("partion size %d , image size %d \n",
			(parti_size/file_header->block_size), file_header->total_blocks);
		while(1);
	}

	/* image is compressed ext4 */
	return 0;
}

static int write_raw_chunk(char* data, unsigned int sector, unsigned int sector_size, struct blk_desc * dev) {
	//char run_cmd[64];
	printf("write raw data in %d size %d \n", sector, sector_size);
	/*sprintf(run_cmd,"mmc write %s 0x%x 0x%x 0x%x", dev_number_write ? "1" : "0",
			(int)data, sector, sector_size);
	err = run_command(run_cmd, 0);*/

	if (blk_dwrite(dev, sector, sector_size, (const void *)data) != sector_size) 
	{
		printf("Can't write raw chunk!!!\n");
		return -1;
	}
	
	return (0); //mj
}

static int write_compressed_ext4(char* img_base, unsigned int sector_base, struct blk_desc * dev) {
	unsigned int sector_size;
	int total_chunks;
	ext4_chunk_header *chunk_header;
	ext4_file_header *file_header;
	int err;
	
	file_header = (ext4_file_header*)img_base;
	total_chunks = file_header->total_chunks;

	printf("total chunk = %d \n", total_chunks);

	img_base += EXT4_FILE_HEADER_SIZE;

	while(total_chunks) {
		chunk_header = (ext4_chunk_header*)img_base;
		sector_size = (chunk_header->chunk_size * file_header->block_size) >> SECTOR_BITS;

		switch(chunk_header->type)
		{
		case EXT4_CHUNK_TYPE_RAW:
			printf("raw_chunk \n");
			err = write_raw_chunk(img_base + EXT4_CHUNK_HEADER_SIZE,
							sector_base, sector_size, dev);
			if(err)//mj for emergency
			{
				printf("[ERROR] System image write fail.please try again..\n");
				return err;
			}
			else
			{
				sector_base += sector_size;
				break;
			}
		case EXT4_CHUNK_TYPE_FILL:
			printf("fill_chunk \n");
			sector_base += sector_size;
			break;

		case EXT4_CHUNK_TYPE_NONE:
			printf("none chunk \n");
			sector_base += sector_size;
			break;

		default:
			printf("unknown chunk type \n");
			sector_base += sector_size;
			break;
		}
		total_chunks--;
		printf("remain chunks = %d \n", total_chunks);

		img_base += chunk_header->total_size;
	};

	printf("write done \n");
	return 0;
}



static int do_decompress(struct cmd_tbl *cmdtp, int flag, int argc,
		      char *const argv[])
{
	if (argc != 4) {
		printf ("usage : ext4decompress <interface> <dev[:part]> <addr>\n");
		return(0);
	}
	
	void * addr = (void *)simple_strtoul(argv[3], NULL, 16);
	fs_dev_part = blk_get_device_part_str(argv[1], (argc >= 3) ? argv[2] : NULL, &fs_dev_desc,
					&fs_partition, 1);
	
	if (fs_dev_part < 0)
		return -1;

	if (fs_dev_desc == NULL) {
		puts ("\n ** Invalid boot device **\n");
		return 1;
	}

	printf("Start decompess %s%d partition%d ...\n", argv[1], fs_dev_desc->devnum, fs_dev_part);
	
	if (check_compress_ext4((char*)addr, fs_partition.size * fs_partition.blksz) != 0)
	{
		//ret = do_mmcops(NULL, 0, 6, argv);
		if (blk_dwrite(fs_dev_desc, fs_partition.start, fs_partition.size, (ulong *)addr) != fs_partition.size) {
				printf("Can't write !!!\n");
			}
	} 
	else 
	{
		printf("Compressed ext4 image\n");
		write_compressed_ext4((char*)addr, fs_partition.start, fs_dev_desc);
	}

	return 0;
}

U_BOOT_CMD(
	ext4decompress, 4, 0, do_decompress,
	"ext4decompress - decompress disk format ext4\n",
	"	- ext4decompress <interface> <dev[:part]> <addr>\n"
);


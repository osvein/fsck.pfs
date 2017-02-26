#include <errno.h>
#include <stdio.h>
#include <sysclib.h>
#include <iomanX.h>
#include <hdd-ioctl.h>

#include "pfs-opt.h"
#include "libpfs.h"
#include "bitmap.h"

#define NUM_BITMAP_ENTRIES		5
#define BITMAP_BUFFER_SIZE		256
#define BITMAP_BUFFER_SIZE_BYTES	(BITMAP_BUFFER_SIZE * 512)

extern u32 pfsMetaSize;
extern int pfsBlockSize;

static pfs_bitmap_t *pfsBitmapData;
static void *pfsBitmapBuffer;
static int PfsTempBitmapFD;

//0x00004ce8
pfs_cache_t *pfsBitmapReadPartition(pfs_mount_t *mount, unsigned short int subpart, unsigned int chunk)
{
	int result;
	return pfsCacheGetData(mount, subpart, chunk + (1 << mount->inode_scale) + (0x2000 >> pfsBlockSize), PFS_CACHE_FLAG_BITMAP, &result);
}

//0x000052c0
static int pfsBitmapTransfer(pfs_bitmap_t *bitmap, int mode)
{
	hddIoctl2Transfer_t xferParams;
	int result;

	xferParams.sub = 0;
	xferParams.sector = (bitmap->index << 8);
	xferParams.size = BITMAP_BUFFER_SIZE;
	xferParams.mode = mode;
	xferParams.buffer = bitmap->bitmap;

	if((result = ioctl2(PfsTempBitmapFD, HIOCTRANSFER, &xferParams, 0, NULL, 0)) < 0)
		printf("fsck: error: could not read/write bitmap.\n");
	else
		bitmap->isDirty = 0;

	return result;
}

//0x00005380
pfs_bitmap_t *pfsBitmapRead(u32 index)
{
	unsigned int i;
	pfs_bitmap_t *pBitmap;

	for(i = 1, pBitmap = NULL; i < NUM_BITMAP_ENTRIES; i++)
	{
		if(pfsBitmapData[i].index == index)
		{
			pBitmap = &pfsBitmapData[i];
			break;
		}
	}

	if(pBitmap != NULL)
	{
		if(pBitmap->nused == 0)
			pBitmap = (pfs_bitmap_t*)pfsCacheUnLink((pfs_cache_t*)pBitmap);

		pBitmap->nused++;
	}else{
		pBitmap = pfsBitmapData->next;
		if(pBitmap->isDirty != 0)
		{
			if(pfsBitmapTransfer(pBitmap, PFS_IO_MODE_WRITE) < 0)
				return NULL;
		}

		pBitmap->index = index;
		pBitmap->isDirty = 0;
		pBitmap->nused = 1;
		if(pfsBitmapTransfer(pBitmap, PFS_IO_MODE_READ) < 0)
			return NULL;
		pBitmap = (pfs_bitmap_t*)pfsCacheUnLink((pfs_cache_t*)pBitmap);
	}

	return pBitmap;
}

//0x00005484
void pfsBitmapFree(pfs_bitmap_t *bitmap)
{
	if(bitmap->nused == 0)
	{
		printf("fsck: error: unused cache returned\n");
	}else{
		bitmap->nused--;
		if(bitmap->nused == 0)
			pfsCacheLink((pfs_cache_t*)pfsBitmapData->prev, (pfs_cache_t*)bitmap);
	}
}

//0x000054f0
u32 *pfsGetBitmapEntry(u32 index)
{
	pfs_bitmap_t *bitmap;
	u32 *result;

	if((bitmap = pfsBitmapRead(index >> 20)) != NULL)
	{
		pfsBitmapFree(bitmap);
		result = &bitmap->bitmap[(index >> 5) & 0x7FFF];
	}else{
		result = NULL;
	}

	return result;
}

//0x0000567c
int pfsBitmapPartInit(u32 size)
{
	int i, result;
	unsigned int bitmapCount;

	bitmapCount = (size >> 20) + (0 < ((size >> 3) & 0x0001FFFF));

	for(i = 1 ; i < NUM_BITMAP_ENTRIES; i++)
	{
		pfsBitmapData[i].isDirty = 0;
		pfsBitmapData[i].nused = 0;
		pfsBitmapData[i].index = i - 1;
		memset(pfsBitmapData[i].bitmap, 0, BITMAP_BUFFER_SIZE_BYTES);
	}

	if(bitmapCount >= NUM_BITMAP_ENTRIES)
	{
		for(i = 1 ; i < NUM_BITMAP_ENTRIES; i++)
		{
			if((result = pfsBitmapTransfer(&pfsBitmapData[i], PFS_IO_MODE_WRITE)) < 0)
			{
				printf("fsck: error: could not initialize bitmap.\n");
				return result;
			}
		}

		for(i = NUM_BITMAP_ENTRIES-1 ; i < bitmapCount; i++)
		{
			pfsBitmapData[NUM_BITMAP_ENTRIES-1].index = i;
			if((result = pfsBitmapTransfer(&pfsBitmapData[NUM_BITMAP_ENTRIES-1], PFS_IO_MODE_WRITE)) < 0)
			{
				printf("fsck: error: could not initialize bitmap.\n");
				return result;
			}
		}

		result = 0;
	}else{
		result = 0;
	}

	return result;
}

//0x000057bc
int pfsBitmapInit(void)
{
	int i;

#ifdef FSCK100
	remove("hdd0:_tmp");

	if((PfsTempBitmapFD = open("hdd0:_tmp,,,128M,PFS", O_CREAT|O_RDWR, 0)) < 0)	//FIXME: There is no mode argument, but our definition of open() strictly requires one (unlike the SCE open function).
		printf("fsck: error: could not create temporary partition.\n");
#else
	if((PfsTempBitmapFD = open("hdd0:__mbr", O_RDWR, 0)) < 0)	//FIXME: There is no mode argument, but our definition of open() strictly requires one (unlike the SCE open function).
	{
		printf("fsck: error: could not open mbr partition.\n");
		return PfsTempBitmapFD;
	}
#endif

	if((pfsBitmapBuffer = pfsAllocMem((NUM_BITMAP_ENTRIES-1) * BITMAP_BUFFER_SIZE_BYTES)) == NULL || (pfsBitmapData = pfsAllocMem(NUM_BITMAP_ENTRIES * sizeof(pfs_bitmap_t))) == NULL)
	{
		return -ENOMEM;
	}

	memset(pfsBitmapData, 0, NUM_BITMAP_ENTRIES * sizeof(pfs_bitmap_t));

	pfsBitmapData->next = pfsBitmapData;
	pfsBitmapData->prev = pfsBitmapData;

	for(i = 1 ; i < NUM_BITMAP_ENTRIES; i++)
	{
		pfsBitmapData[i].bitmap = (u32*)((unsigned char*)pfsBitmapBuffer + BITMAP_BUFFER_SIZE_BYTES * i);
		pfsCacheLink((pfs_cache_t*)pfsBitmapData[0].prev, (pfs_cache_t*)&pfsBitmapData[i]);
	}

	return 0;
}

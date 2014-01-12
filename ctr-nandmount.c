#include <fuse.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <ctype.h>

int nand_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi);
int nand_getattr(const char *path, struct stat *stbuf);
int nand_open(const char *path, struct fuse_file_info *fi);
int nand_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi);
int nand_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi);
int nand_truncate(const char *path, off_t size);

static struct fuse_operations nand_operations = {
	.getattr = nand_getattr,
	.readdir = nand_readdir,
	.open    = nand_open,
	.read    = nand_read,
	.write   = nand_write,
	.truncate = nand_truncate
};

typedef struct {
	int enabled;
	unsigned int imageoffset;
	unsigned int size;
	FILE *fxor;
} ncsdpart_context_struct;

FILE *fnand;
off_t nandimage_size, nandimage_baseoffset=0;
int nand_readonly = 0;
int using_multipartitions = 0;

ncsdpart_context_struct ncsd_partitions[8];

typedef struct//The below ncsd structs are based on the structs from ctrtool ncsd.h.
{
	unsigned int offset;
	unsigned int size;
} ncsd_partition_geometry;

typedef struct
{
	unsigned char signature[0x100];
	unsigned char magic[4];
	unsigned char mediasize[4];
	unsigned char mediaid[8];
	unsigned char partitionfstype[8];
	unsigned char partitioncrypttype[8];
	ncsd_partition_geometry partitiongeometry[8];
	unsigned char extendedheaderhash[0x20];
	unsigned char additionalheadersize[4];
	unsigned char sectorzerooffset[4];
	unsigned char flags[8];
	unsigned char partitionid[0x40];
	unsigned char reserved[0x30];
} ctr_ncsdheader;

int main(int argc, char *argv[])
{
	int fargc, argi;
	int i, len;
	int partindex;
	int fail;
	int noncsdhdr=0;
	int offset_set;
	unsigned long long tmpval=0;
	unsigned int mediaunitsize;
	char *ptr;
	char **fargv;
	struct stat filestat;
	FILE *fxorpad;
	ctr_ncsdheader ncsdhdr;

	if(argc<4)
	{
		printf("ctr-nandmount by yellows8\n");
		printf("FUSE tool for accessing a mounted plaintext NAND image + plaintext mounted partition(s) with an encrypted NAND image + xorpad(s).\n");
		printf("Usage:\n");
		printf("ctr-nandmount <imagepath> <xorpad> <mount-point + FUSE options>\n");
		printf("<xorpad> can be a list of NCSD partition xorpads in the following structure: '0=partindex0.xorpad,1=partindex1.xorpad ...' This can be used to override the default image-offset loaded from the NCSD header(required when using the --noncsdhdr option): '0=0x<offset>:partindex0.xorpad ...'\n");
		printf("Options:\n");
		printf("--readonly Open the NAND image in read-only mode and disable writing for the mounted image.\n");
		printf("--noncsdhdr Don't load partition offsets and sizes from the NCSD header, load these during xorpad initialization instead.\n");
		printf("--nandimgsize=0x<size> Use the specified size for the NAND image size instead of loading it with stat.\n");
		printf("--nandoff=0x<offset> Set the base offset(default is zero) of the actual image within the specified file/device. If stat() is sucessfully used for getting the imagesize, the imagesize is subtracted by this nandoff.\n");
		return 0;
	}

	memset(ncsd_partitions, 0, sizeof(ncsdpart_context_struct)*8);

	fargc = argc - 2;
	fargv = (char **) malloc(fargc * sizeof(char *));
	fargv[0] = argv[0];

	fargc = 1;
	i = 1;
	for(argi=3; argi<argc; argi++)
	{
		if(strncmp(argv[argi], "--readonly", 10)==0)
		{
			nand_readonly = 1;
		}
		else if(strncmp(argv[argi], "--noncsdhdr", 11)==0)
		{
			noncsdhdr = 1;
		}
		else if(strncmp(argv[argi], "--nandimgsize=", 14)==0)
		{
			if(argv[argi][14]!='0' || argv[argi][14+1]!='x')
			{
				printf("Input value for nandimgsize is invalid.\n");
			}
			else
			{
				sscanf(&argv[argi][14+2], "%llx", &tmpval);
				nandimage_size = (off_t)tmpval;
			}
		}
		else if(strncmp(argv[argi], "--nandoff=", 10)==0)
		{
			if(argv[argi][10]!='0' || argv[argi][10+1]!='x')
			{
				printf("Input value for nandoff is invalid.\n");
			}
			else
			{
				sscanf(&argv[argi][10+2], "%llx", &tmpval);
				nandimage_baseoffset = (off_t)tmpval;
			}
		}
		else
		{
			fargv[i] = argv[argi];
			i++;
			fargc++;
		}
	}

	if(nand_readonly==0)
	{
		fnand = fopen(argv[1], "r+");
	}
	else
	{
		fnand = fopen(argv[1], "rb");
	}
	if(fnand==NULL)
	{
		printf("Failed to open NAND image: %s\n", argv[1]);
		free(fargv);
		return 1;
	}
	
	if(nandimage_size==0)
	{
		stat(argv[1], &filestat);
		nandimage_size = filestat.st_size;

		if(filestat.st_size)nandimage_size-= nandimage_baseoffset;
	}

	if(nandimage_size==0)
	{
		printf("Invalid NAND image size, using default imagesize of 0x3af00000.\n");
		nandimage_size = 0x3af00000;
	}

	printf("Using NAND image base offset 0x%llx.\n", (unsigned long long)nandimage_baseoffset);

	if(noncsdhdr==0)
	{
		fseeko(fnand, nandimage_baseoffset, SEEK_SET);

		if(fread(&ncsdhdr, 1, sizeof(ctr_ncsdheader), fnand)!=sizeof(ctr_ncsdheader))
		{
			fclose(fnand);
			free(fargv);
			printf("Failed to read NCSD header.\n");
			return 2;
		}

		if(strncmp((char*)ncsdhdr.magic, "NCSD", 4)!=0)
		{
			fclose(fnand);
			free(fargv);
			printf("Invalid NCSD magic.\n");
			return 2;
		}

		mediaunitsize = 1<<(9+ncsdhdr.flags[6]);//Based on code from ctrtool ncsd.c.

		for(partindex=0; partindex<8; partindex++)
		{
			if(ncsdhdr.partitiongeometry[partindex].size==0)continue;

			ncsd_partitions[partindex].imageoffset = ncsdhdr.partitiongeometry[partindex].offset * mediaunitsize;
			ncsd_partitions[partindex].size = ncsdhdr.partitiongeometry[partindex].size * mediaunitsize;
		}
	}
	
	if(!(isdigit(argv[2][0]) && argv[2][1] == '='))
	{
		fxorpad = fopen(argv[2], "rb");
		if(fxorpad==NULL)
		{
			printf("Failed to open xorpad: %s\n", argv[2]);
			fclose(fnand);
			free(fargv);
			return 1;
		}
		stat(argv[2], &filestat);

		ncsd_partitions[0].enabled = 1;
		ncsd_partitions[0].imageoffset = 0;
		ncsd_partitions[0].size = filestat.st_size;
		ncsd_partitions[0].fxor = fxorpad;

		using_multipartitions = 0;
	}
	else
	{
		len = strlen(argv[2]);
		i = 0;
		fail = 0;

		using_multipartitions = 1;

		while(i<len)
		{
			if(!(isdigit(argv[2][i]) && argv[2][i+1] == '='))break;

			partindex = argv[2][i] - '0';

			i+=2;

			if(ncsd_partitions[partindex].enabled)
			{
				printf("Xorpad for partindex %d is already set.\n", partindex);
			}
			else
			{
				offset_set = !noncsdhdr;

				ptr = strchr(&argv[2][i], ',');
				if(ptr)*ptr = 0;

				ptr = strchr(&argv[2][i], ':');
				if(ptr)
				{
					if(argv[2][i]=='0' && argv[2][i+1]=='x')
					{
						sscanf(&argv[2][i+2], "%x", &ncsd_partitions[partindex].imageoffset);
						offset_set = 1;
					}
					else
					{
						printf("The image-offset value between the '=' and ':' for part%d is invalid, using the default image-offset from the NCSD header(if --noncsdhdr wasn't specified) instead.\n", partindex);
					}

					*ptr = 0;
					i+= strlen(&argv[2][i]) + 1;
				}

				if(!offset_set)
				{
					printf("Offset paramater is missing for part%d and the --noncsdhdr option was specified.\n", partindex);
					fail = 1;
					break;
				}

				fxorpad = fopen(&argv[2][i], "rb");
				if(fxorpad==NULL)
				{
					printf("Failed to open xorpad: %s\n", &argv[2][i]);
					fail = 1;
					break;
				}
				stat(&argv[2][i], &filestat);

				ncsd_partitions[partindex].enabled = 1;
				if(!noncsdhdr)
				{
					if(ncsd_partitions[partindex].size < filestat.st_size)
					{
						printf("Warning: xorpad for part%d is larger than the NCSD partition size, the NCSD partition size will be used instead of the xorpad size.\n", partindex);
					}
					else
					{
						if(ncsd_partitions[partindex].size != filestat.st_size)
						{
							printf("Warning: xorpad size is smaller than the NCSD partition size: xorpadsz=0x%x, ncsdpartsz=0x%x\n", (unsigned int)filestat.st_size, ncsd_partitions[partindex].size);
							ncsd_partitions[partindex].size = filestat.st_size;
						}
					}
				}
				else
				{
					ncsd_partitions[partindex].size = filestat.st_size;
				}

				ncsd_partitions[partindex].fxor = fxorpad;
			}

			printf("Successfully initialized part%d xorpad: imageoffset=0x%x, size=0x%x\n", partindex, ncsd_partitions[partindex].imageoffset, ncsd_partitions[partindex].size);

			i+= strlen(&argv[2][i]) + 1;
		}
		
		if(fail)
		{
			for(partindex=0; partindex<8; partindex++)
			{
				if(ncsd_partitions[partindex].enabled && ncsd_partitions[partindex].fxor)fclose(ncsd_partitions[partindex].fxor);
			}

			fclose(fnand);
			free(fargv);
			return 1;
		}
	}

	return fuse_main(fargc, fargv, &nand_operations);
}

int nand_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi)
{
	int partindex;
	char filename[32];

	if(strcmp(path, "/")==0)
	{
		filler(buf, ".", NULL, 0);
		filler(buf, "..", NULL, 0);
		filler(buf, "image.plain", NULL, 0);

		if(using_multipartitions)
		{
			for(partindex=0; partindex<8; partindex++)
			{
				if(ncsd_partitions[partindex].enabled==0)continue;

				memset(filename, 0, 32);
				snprintf(filename, 31, "image_part%d.plain", partindex);
				filler(buf, filename, NULL, 0);
			}
		}
	}
	else
	{
		return -ENOENT;
	}

	return 0;
}

int nand_getattr(const char *path, struct stat *stbuf)
{
	int partindex;

	memset(stbuf, 0, sizeof(struct stat));

	if(strcmp(path, "/") == 0)
	{
		stbuf->st_mode = S_IFDIR | 0445;
		stbuf->st_nlink = 2 + 1;
		return 0;
	}

	if(nand_readonly==0)
	{
		stbuf->st_mode = S_IFREG | 0666;
	}
	else
	{
		stbuf->st_mode = S_IFREG | 0444;
	}

	if(strcmp(path, "/image.plain") == 0)
	{
		stbuf->st_nlink = 1;
		stbuf->st_size = nandimage_size;
	}
	else if(strncmp(path, "/image_part", 11) == 0 && strcmp(&path[12], ".plain") == 0)
	{
		if(!using_multipartitions)return -ENOENT;
		if(path[11] < '0' || path[11] > '7')return -ENOENT;

		partindex = path[11] - '0';
		if(ncsd_partitions[partindex].enabled==0)return -ENOENT;

		stbuf->st_nlink = 1;
		stbuf->st_size = ncsd_partitions[partindex].size;
	}
	else
	{
		return -ENOENT;
	}

	return 0;
}

int nand_open(const char *path, struct fuse_file_info *fi)
{
	int partindex;

	if(strcmp(path, "/image.plain") == 0)return 0;

	if(strncmp(path, "/image_part", 11) == 0 && strcmp(&path[12], ".plain") == 0)
	{
		if(!using_multipartitions)return -ENOENT;
		if(path[11] < '0' || path[11] > '7')return -ENOENT;

		partindex = path[11] - '0';
		if(ncsd_partitions[partindex].enabled==0)return -ENOENT;

		return 0;
	}

	return -ENOENT;
}

int nand_truncate(const char *path, off_t size)
{
	return nand_open(path, NULL);
}

int nand_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
	char *xorbuf;
	size_t transize, tmpsize = size, chunksize, pos=0;
	int i, partindex;
	int found;
	int using_mainimage = 0;
	FILE *fxorpad;
	ncsdpart_context_struct *part;

	memset(buf, 0, size);

	if(strcmp(path, "/image.plain")==0)
	{
		if(offset+size > nandimage_size)return -EINVAL;
		using_mainimage = 1;
	}
	else
	{
		if(strncmp(path, "/image_part", 11) == 0 && strcmp(&path[12], ".plain") == 0)
		{
			if(!using_multipartitions)return -ENOENT;
			if(path[11] < '0' || path[11] > '7')return -ENOENT;

			partindex = path[11] - '0';
			if(ncsd_partitions[partindex].enabled==0)return -ENOENT;
		}
		else
		{
			return -ENOENT;
		}
	}

	xorbuf = (char*)malloc(size);
	if(xorbuf==NULL)return -ENOMEM;
	memset(xorbuf, 0, size);

	while(tmpsize)
	{
		if(using_mainimage)
		{
			found = 0;
			for(partindex=0; partindex<8; partindex++)
			{
				part = &ncsd_partitions[partindex];
				if(part->enabled==0)continue;

				if(offset >= part->imageoffset && offset < part->imageoffset + part->size)
				{
					found = 1;
					break;
				}
			}

			if(!found)
			{
				free(xorbuf);
				if(pos==0)return -EINVAL;
				return pos;
			}
		}
		else
		{
			part = &ncsd_partitions[partindex];
			if(offset+tmpsize > part->size)return -EINVAL;
		}

		fxorpad = part->fxor;

		chunksize = tmpsize;
		if(using_mainimage)
		{
			if(offset + chunksize > part->imageoffset + part->size)chunksize -= (part->imageoffset + part->size) - offset;
		}

		if(using_mainimage)
		{
			if(fseeko(fnand, nandimage_baseoffset + offset, SEEK_SET)==-1)
			{
				free(xorbuf);
				return -EIO;
			}
			if(fseeko(fxorpad, offset - part->imageoffset, SEEK_SET)==-1)
			{
				free(xorbuf);
				return -EIO;
			}
		}
		else
		{
			if(fseeko(fnand, nandimage_baseoffset + offset + part->imageoffset, SEEK_SET)==-1)
			{
				free(xorbuf);
				return -EIO;
			}
			if(fseeko(fxorpad, offset, SEEK_SET)==-1)
			{
				free(xorbuf);
				return -EIO;
			}
		}

		transize = fread(&buf[pos], 1, chunksize, fnand);
		if(transize!=chunksize)
		{
			free(xorbuf);
			return -EIO;
		}

		transize = fread(&xorbuf[pos], 1, chunksize, fxorpad);
		if(transize!=chunksize)
		{
			free(xorbuf);
			return -EIO;
		}

		for(i=0; i<chunksize; i++)buf[pos+i] ^= xorbuf[pos+i];

		tmpsize-= chunksize;
		offset+= chunksize;
		pos+= chunksize;
	}

	free(xorbuf);

	return size;
}

int nand_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *fi)
{
	char *xorbuf;
	size_t transize, tmpsize = size, chunksize, pos=0;
	int i, partindex;
	int found;
	int using_mainimage = 0;
	FILE *fxorpad;
	ncsdpart_context_struct *part;

	if(strcmp(path, "/image.plain")==0)
	{
		if(offset+size > nandimage_size)return -EINVAL;
		using_mainimage = 1;
	}
	else
	{
		if(strncmp(path, "/image_part", 11) == 0 && strcmp(&path[12], ".plain") == 0)
		{
			if(!using_multipartitions)return -ENOENT;
			if(path[11] < '0' || path[11] > '7')return -ENOENT;

			partindex = path[11] - '0';
		}
		else
		{
			return -ENOENT;
		}
	}

	if(nand_readonly)return -EACCES;

	xorbuf = (char*)malloc(size);
	if(xorbuf==NULL)return -ENOMEM;
	memset(xorbuf, 0, size);

	while(tmpsize)
	{
		if(using_mainimage)
		{
			found = 0;
			for(partindex=0; partindex<8; partindex++)
			{
				part = &ncsd_partitions[partindex];
				if(part->enabled==0)continue;

				if(offset >= part->imageoffset && offset < part->imageoffset + part->size)
				{
					found = 1;
					break;
				}
			}

			if(!found)
			{
				free(xorbuf);
				if(pos==0)return -EINVAL;
				return pos;
			}
		}
		else
		{
			part = &ncsd_partitions[partindex];
			if(offset+tmpsize > part->size)return -EINVAL;
		}

		fxorpad = part->fxor;

		chunksize = tmpsize;
		if(using_mainimage)
		{
			if(offset + chunksize > part->imageoffset + part->size)chunksize -= (part->imageoffset + part->size) - offset;
		}

		if(using_mainimage)
		{
			if(fseeko(fnand, nandimage_baseoffset + offset, SEEK_SET)==-1)
			{
				free(xorbuf);
				return -EIO;
			}
			if(fseeko(fxorpad, offset - part->imageoffset, SEEK_SET)==-1)
			{
				free(xorbuf);
				return -EIO;
			}
		}
		else
		{
			if(fseeko(fnand, nandimage_baseoffset + offset + part->imageoffset, SEEK_SET)==-1)
			{
				free(xorbuf);
				return -EIO;
			}
			if(fseeko(fxorpad, offset, SEEK_SET)==-1)
			{
				free(xorbuf);
				return -EIO;
			}
		}

		transize = fread(&xorbuf[pos], 1, chunksize, fxorpad);
		if(transize!=chunksize)
		{
			free(xorbuf);
			return -EIO;
		}

		for(i=0; i<chunksize; i++)xorbuf[pos+i] ^= buf[pos+i];

		transize = fwrite(&xorbuf[pos], 1, chunksize, fnand);
		if(transize!=chunksize)
		{
			free(xorbuf);
			return -EIO;
		}

		tmpsize-= chunksize;
		offset+= chunksize;
		pos+= chunksize;
	}

	free(xorbuf);

	return size;
}


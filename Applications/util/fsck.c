#include <stdio.h>
#include <strings.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <mntent.h>

#define swizzle16(x)	(x)
#define swizzle32(x)	(x)

typedef uint16_t	blkno_t;

struct filesys {
    uint16_t      s_mounted;
    uint16_t      s_isize;
    uint16_t      s_fsize;
    int16_t       s_nfree;
    blkno_t     s_free[50];
    int16_t       s_ninode;
    uint16_t      s_inode[50];
    uint8_t       s_fmod;
#define FMOD_DIRTY	1
#define FMOD_CLEAN	2
    uint8_t	s_timeh;	/* top bits of time */
    uint32_t      s_time;
    blkno_t     s_tfree;
    uint16_t      s_tinode;
    uint16_t      s_mntpt;
};

#define ROOTINODE 1
#define SMOUNTED 12742   /* Magic number to specify mounted filesystem */
#define SMOUNTED_WRONGENDIAN 50737U   /* byteflipped */

struct dinode {
    uint16_t i_mode;
    uint16_t i_nlink;
    uint16_t i_uid;
    uint16_t i_gid;
    uint32_t    i_size;
    uint32_t   i_atime;
    uint32_t   i_mtime;
    uint32_t   i_ctime;
    blkno_t  i_addr[20];
};               /* Exactly 64 bytes long! */

#define F_REG   0100000
#define F_DIR   040000
#define F_PIPE  010000
#define F_BDEV  060000
#define F_CDEV  020000

#define F_MASK  0170000

struct direct {
        uint16_t   d_ino;
        char     d_name[30];
};

#define MAXDEPTH 20	/* Maximum depth of directory tree to search */

/* This checks a filesystem */

static int dev = 0;
static struct filesys superblock;
static long offset;
static int dev_fd;
static int error;
static int rootfs;
static int aflag;

static unsigned char *bitmap;
static int16_t *linkmap;
static char *daread(uint16_t blk);
static void dwrite(uint16_t blk, char *addr);
static void iread(uint16_t ino, struct dinode *buf);
static void iwrite(uint16_t ino, struct dinode *buf);
static void setblkno(struct dinode *ino, blkno_t num, blkno_t dnum);
static void ckdir(uint16_t inum, uint16_t pnum, char *name);
static void dirread(struct dinode *ino, uint16_t j, struct direct *dentry);
static void dirwrite(struct dinode *ino, uint16_t j, struct direct *dentry);
static void mkentry(uint16_t inum);
static blkno_t blk_alloc0(struct filesys *filesys);
static blkno_t getblkno(struct dinode *ino, blkno_t num);

static void pass1(void);
static void pass2(void);
static void pass3(void);
static void pass4(void);
static void pass5(void);

static int yes_noerror(void)
{
    static char buf[16];
    fflush(stdout);
    do {
        if (fgets(buf, 15, stdin) == NULL)
            exit(1);
        if (isupper(*buf))
            *buf = tolower(*buf);
    } while(*buf != 'n' && *buf != 'y');
    return  (*buf == 'y') ? 1 : 0;
}

static int yes(void) {
    int ret = yes_noerror();
    if (ret)
        error |= 1;
    else
        error |= 4;
    return ret;
}

static void bitset(uint16_t b)
{
    bitmap[b >> 3] |= (1 << (b & 7));
}

static void bitclear(uint16_t b)
{
    bitmap[b >> 3] &= ~(1 << (b & 7));
}

static int bittest(uint16_t b)
{
    return (bitmap[b >> 3] & (1 << (b & 7))) ? 1 : 0;
}

static void panic(char *s)
{
	fprintf(stderr, "panic: %s\n", s);
	exit(error | 8);
}

const char *mntpoint(const char *mount)
{
    FILE *fp;
    struct mntent *mnt;

    fp = setmntent("/etc/fstab", "r");
    if (fp) {
        while (mnt = getmntent(fp)) {
            if (strcmp(mnt->mnt_dir, mount) == 0) {
                endmntent(fp);
                return mnt->mnt_fsname;
            }
        }
        endmntent(fp);
    }
    return NULL;
}

static int fd_open(char *name, int search)
{
	char *sd;
	const char *p;
	int bias = 0;
	struct stat rootst;
	struct stat work;

	sd = strrchr(name, ':');
	if (sd) {
		*sd = 0;
		sd++;
		bias = atoi(sd);
	} else {
	    if (search) {
		p = mntpoint(name);
		if (p)
		    name = (char *)p;
            }
        }

	offset = bias;
	dev_fd = open(name, O_RDWR | O_CREAT, 0666);

	if (dev_fd < 0)
		return -1;

	if (stat("/", &rootst) == -1)
	    panic("stat /");
        if (fstat(dev_fd, &work) == -1)
            panic("statfd");

        if (rootst.st_dev == work.st_rdev) {
            puts("Checking root file system.");
            rootfs = 1;
        }

	return 0;
}

int perform_fsck(char *name, int search)
{
    char *buf;

    if (fd_open(name, search)){
        puts("Cannot open file\n");
        return 16;
    }

    buf = daread(1);
    memcpy((char *) &superblock, buf, sizeof(struct filesys));

    if (superblock.s_fmod == FMOD_DIRTY) {
        puts("Filesystem was not cleanly unmounted.");
        error |= 1;
    }
    else if (aflag)
        return 0;

    /* Verify the fsize and isize parameters */
    if (superblock.s_mounted == SMOUNTED_WRONGENDIAN) {
        panic("Reversed byte order.\n");
    }

    if (swizzle16(superblock.s_mounted) != SMOUNTED) {
        printf("Device %u has invalid magic number %u. Fix? ", dev, superblock.s_mounted);
        if (!yes())
            exit(error|32);
        superblock.s_mounted = swizzle16(SMOUNTED);
        dwrite((blkno_t) 1, (char *) &superblock);
    }
    printf("Device %u has fsize = %u and isize = %u. Continue? ",
            dev, swizzle16(superblock.s_fsize), swizzle16(superblock.s_isize));
    if (!yes_noerror())
        return (error |= 32);

    bitmap = calloc((swizzle16(superblock.s_fsize) + 7UL) / 8, sizeof(char));
    linkmap = (int16_t *) calloc(8 * swizzle16(superblock.s_isize), sizeof(int16_t));

    if (!bitmap || !linkmap) {
        fputs("Not enough memory.\n", stderr);
        return(error |= 8);
    }

    puts("Pass 1: Checking inodes...");
    pass1();

    puts("Pass 2: Rebuilding free list...");
    pass2();

    puts("Pass 3: Checking block allocation...");
    pass3();

    puts("Pass 4: Checking directory entries...");
    pass4();

    puts("Pass 5: Checking link counts...");
    pass5();

    /* If we fixed things, and no errors were left uncorrected */
    if ((error & 5) == 1) {
        superblock.s_fmod = FMOD_CLEAN;
        dwrite((blkno_t) 1, (char *) &superblock);
        if (rootfs) {
            error |= 2;
            puts("**** Root filesystem was modified, immediate reboot required.");
            sleep(5);
            /* Sync it all out and reboot */
            uadmin(A_REBOOT, 0, 0);
        }
    }
    return error;
}

int main(int argc, char *argv[])
{
    struct mntent *mnt;
    if (argc > 1 && strcmp(argv[1],"-a") == 0) {
        argc--;
        argv++;
        aflag = 1;
    }

    if (argc == 1 && aflag) {
        FILE *fp = setmntent("/etc/fstab", "r");
        if (fp == NULL) {
            perror("/etc/fstab");
            exit(1);
        }
        while((mnt = getmntent(fp)) != NULL) {
            if (perform_fsck(mnt->mnt_fsname, 0))
                exit(error);
        }
        endmntent(fp);
    } else {
        if(argc != 2) {
            fputs("syntax: fsck[-a] [devfile][:offset]\n", stderr);
            return 16;
        }
        perform_fsck(argv[1], 0);
    }
    puts("Done.");
    exit(error);
}



/*
 *  Pass 1 checks each inode independently for validity, zaps bad block
 *  numbers in the inodes, and builds the block allocation map.
 */

static void pass1(void)
{
    uint16_t n;
    struct dinode ino;
    uint16_t mode;
    blkno_t b;
    blkno_t bno;
    uint16_t icount;
    blkno_t *buf;
    uint16_t imax =  8 * (swizzle16(superblock.s_isize) - 2);
    uint32_t bmax;

    icount = 0;

    for (n = ROOTINODE; n < imax; ++n) {
        iread(n, &ino);
        linkmap[n] = -1;
        if (ino.i_mode == 0)
            continue;

        mode = swizzle16(ino.i_mode) & F_MASK;
        /* FIXME: named pipes.. */

        /* Check mode */
        if (mode != F_REG && mode != F_DIR && mode != F_BDEV && mode != F_CDEV) {
            printf("Inode %u with mode 0%o is not of correct type. Zap? ",
                    n, swizzle16(ino.i_mode));
            if (yes()) {
                ino.i_mode = 0;
                ino.i_nlink = 0;
                iwrite(n, &ino);
                continue;
            }
        }
        linkmap[n] = 0;
        ++icount;
        /* Check size */

        if (swizzle32(ino.i_size) < 0) {
            printf("Inode %u offset is negative with value of %ld. Fix? ",
                    n, (long)swizzle32(ino.i_size));
            if (yes()) {
                ino.i_size = 0;
                iwrite(n, &ino);
            }
        }
        /* Check blocks and build free block map */
        if (mode == F_REG || mode == F_DIR) {
            /* Check singly indirect blocks */

            for (b = 18; b < 20; ++b) {
                if (ino.i_addr[b] != 0 && 
                    (swizzle16(ino.i_addr[b]) < swizzle16(superblock.s_isize) ||
                            swizzle16(ino.i_addr[b]) >= swizzle16(superblock.s_fsize))) {
                    printf("Inode %u singly ind. blk %u out of range, val = %u. Zap? ",
                            n, b, swizzle16(ino.i_addr[b]));
                    if (yes()) {
                        ino.i_addr[b] = 0;
                        iwrite(n, &ino);
                    }
                }
                if (ino.i_addr[b] != 0 && swizzle32(ino.i_size) < 18*512) {
                    printf("Inode %u singly ind. blk %u past end of file, val = %u. Zap? ",
                            n, b, swizzle16(ino.i_addr[b]));
                    if (yes()) {
                        ino.i_addr[b] = 0;
                        iwrite(n, &ino);
                    }
                }
                if (ino.i_addr[b] != 0)
                    bitset(swizzle16(ino.i_addr[b]));
            }

            /* Check the double indirect blocks */
            if (ino.i_addr[19] != 0) {
                buf = (blkno_t *) daread(swizzle16(ino.i_addr[19]));
                for (b = 0; b < 256; ++b) {
                    if (buf[b] != 0 && (swizzle16(buf[b]) < swizzle16(superblock.s_isize) ||
                                swizzle16(buf[b]) >= swizzle16(superblock.s_fsize))) {
                        printf("Inode %u doubly ind. blk %u is ", n, b);
                        printf("out of range, val = %u. Zap? ", swizzle16(buf[b]));
                        /* 1.4.98 - line split.  HFB */
                        if (yes()) {
                            buf[b] = 0;
                            dwrite(b, (char *) buf);
                        }
                    }
                    if (buf[b] != 0)
                        bitset(swizzle16(buf[b]));
                }
            }
            /* Check the rest */
            bmax = swizzle32(ino.i_size)/512;
            for (bno = 0; bno <= bmax ; ++bno) {
                b = getblkno(&ino, bno);

                if (b != 0 && (b < swizzle16(superblock.s_isize) || b >= swizzle16(superblock.s_fsize))) {
                    printf("Inode %u block %u out of range, val = %u. Zap? ",
                            n, bno, b);
                    if (yes()) {
                        setblkno(&ino, bno, 0);
                        iwrite(n, &ino);
                    }
                }
                if (b != 0)
                    bitset(b);
            }
        }
    }
    /* Fix free inode count in superblock block */
    if (swizzle16(superblock.s_tinode) != 8 * (swizzle16(superblock.s_isize) - 2) - ROOTINODE - icount) {
        printf("Free inode count in superblock block is %u, should be %u. Fix? ",
                swizzle16(superblock.s_tinode), 8 * (swizzle16(superblock.s_isize) - 2) - ROOTINODE - icount);

        if (yes()) {
            superblock.s_tinode = swizzle16(8 * (swizzle16(superblock.s_isize) - 2) - ROOTINODE - icount);
            dwrite((blkno_t) 1, (char *) &superblock);
        }
    }
}


/* Clear inode free list, rebuild block free list using bit map. */
static void pass2(void)
{
    blkno_t j;
    blkno_t oldtfree;
    int s;
    uint16_t imax = swizzle16(superblock.s_isize);

    printf("Rebuild free list? ");
    if (!yes_noerror())
        return;

    error |= 1;
    oldtfree = swizzle16(superblock.s_tfree);

    /* Initialize the superblock-block */

    superblock.s_ninode = 0;
    superblock.s_nfree = swizzle16(1);
    superblock.s_free[0] = 0;
    superblock.s_tfree = 0;

    /* Free each block, building the free list */

    for (j = swizzle16(superblock.s_fsize) - 1; j >= imax; --j) {
        if (bittest(j) == 0) {
            if (swizzle16(superblock.s_nfree) == 50) {
                dwrite(j, (char *) &superblock.s_nfree);
                superblock.s_nfree = 0;
            }
            superblock.s_tfree = swizzle16(swizzle16(superblock.s_tfree)+1);
            s = swizzle16(superblock.s_nfree);
            superblock.s_free[s++] = swizzle16(j);
            superblock.s_nfree = swizzle16(s);
        }
    }

    dwrite((blkno_t) 1, (char *) &superblock);

    if (oldtfree != swizzle16(superblock.s_tfree))
        printf("During free list regeneration s_tfree was changed to %u from %u.\n",
                swizzle16(superblock.s_tfree), oldtfree);

}

/* Pass 3 finds and fixes multiply allocated blocks. */
static void pass3(void)
{
    uint16_t n;
    struct dinode ino;
    uint16_t mode;
    blkno_t b;
    blkno_t bno;
    blkno_t newno;
    uint16_t bmax = swizzle16(superblock.s_fsize);
    uint16_t imax = 8 * (swizzle16(superblock.s_isize) - 2);
    uint32_t nmax;
    /*--- was blk_alloc ---*/

    /* FIXME:
        1. Set the bits below s_isize
     */
    for (b = swizzle16(superblock.s_isize); b < bmax; ++b)
        bitclear(b);

    for (n = ROOTINODE; n < imax; ++n) {
        iread(n, &ino);

        mode = swizzle16(ino.i_mode) & F_MASK;
        if (mode != F_REG && mode != F_DIR)
            continue;

        /* Check singly indirect blocks */

        for (b = 18; b < 20; ++b) {
            if (ino.i_addr[b] != 0) {
                if (bittest(swizzle16(ino.i_addr[b])) != 0) {
                    printf("Indirect block %u in inode %u value %u multiply allocated. Fix? ",
                            b, n, swizzle16(ino.i_addr[b]));
                    if (yes()) {
                        newno = blk_alloc0(&superblock);
                        if (newno == 0) {
                            puts("Sorry... No more free blocks.");
                            error |= 4;
                        } else {
                            dwrite(newno, daread(swizzle16(ino.i_addr[b])));
                            ino.i_addr[b] = swizzle16(newno);
                            iwrite(n, &ino);
                        }
                    }
                } else
                    bitset(swizzle16(ino.i_addr[b]));
            }
        }

        /* Check the rest */
        nmax = swizzle32(ino.i_size)/512;
        for (bno = 0; bno <= nmax; ++bno) {
            b = getblkno(&ino, bno);

            if (b != 0) {
                if (bittest(b)) {
                    printf("Block %u in inode %u value %u multiply allocated. Fix? ",
                            bno, n, b);
                    if (yes()) {
                        newno = blk_alloc0(&superblock);
                        if (newno == 0) {
                            puts("Sorry... No more free blocks.");
                            error |= 4;
                        } else {
                            dwrite(newno, daread(b));
                            setblkno(&ino, bno, newno);
                            iwrite(n, &ino);
                        }
                    }
                } else
                    bitset(b);
            }
        }

    }

}

static int depth;

/*
 *  Pass 4 traverses the directory tree, fixing bad directory entries
 *  and finding the actual number of references to each inode.
 */

static void pass4(void)
{
    depth = 0;
    linkmap[ROOTINODE] = 1;
    ckdir(ROOTINODE, ROOTINODE, "/");
    if (depth != 0)
        panic("Inconsistent depth");
}


/* This recursively checks the directories */

static void ckdir(uint16_t inum, uint16_t pnum, char *name)
{
    struct dinode ino;
    struct direct dentry;
    uint16_t j;
    int c;
    uint8_t i;
    int nentries;
    char *ename;

    iread(inum, &ino);
    if ((swizzle16(ino.i_mode) & F_MASK) != F_DIR)
        return;
    ++depth;

    if (swizzle32(ino.i_size) % 32 != 0) {
        printf("Directory inode %u has improper length. Fix? ", inum);
        if (yes()) {
            ino.i_size = swizzle32(swizzle32(ino.i_size) & ~0x1f);
            iwrite(inum, &ino);
        }
    }
    nentries = swizzle32(ino.i_size)/32;

    for (j = 0; j < nentries; ++j) {
        dirread(&ino, j, &dentry);

        for (i = 0; i < 30; ++i) if (dentry.d_name[i] == '\0') break;
        for (     ; i < 30; ++i) dentry.d_name[i] = '\0';
        dirwrite(&ino, j, &dentry);

        if (dentry.d_ino == 0)
            continue;

        if (swizzle16(dentry.d_ino) < ROOTINODE ||
                swizzle16(dentry.d_ino) >= 8 * swizzle16(superblock.s_isize)) {
            printf("Directory entry %s%-1.14s has out-of-range inode %u. Zap? ",
                    name, dentry.d_name, swizzle16(dentry.d_ino));
            if (yes()) {
                dentry.d_ino = 0;
                dentry.d_name[0] = '\0';
                dirwrite(&ino, j, &dentry);
                continue;
            }
        }
        if (dentry.d_ino && linkmap[swizzle16(dentry.d_ino)] == -1) {
            printf("Directory entry %s%-1.14s points to bogus inode %u. Zap? ",
                    name, dentry.d_name, swizzle16(dentry.d_ino));
            if (yes()) {
                dentry.d_ino = 0;
                dentry.d_name[0] = '\0';
                dirwrite(&ino, j, &dentry);
                continue;
            }
        }
        ++linkmap[swizzle16(dentry.d_ino)];

        for (c = 0; c < 30 && dentry.d_name[c]; ++c) {
            if (dentry.d_name[c] == '/') {
                printf("Directory entry %s%-1.30s contains slash. Fix? ",
                        name, dentry.d_name);
                if (yes()) {
                    dentry.d_name[c] = 'X';
                    dirwrite(&ino, j, &dentry);
                }
            }
        }

        if (strncmp(dentry.d_name, ".", 30) == 0 && swizzle16(dentry.d_ino) != inum) {
            printf("Dot entry %s%-1.30s points to wrong place. Fix? ",
                    name, dentry.d_name);
            if (yes()) {
                dentry.d_ino = swizzle16(inum);
                dirwrite(&ino, j, &dentry);
            }
        }
        if (strncmp(dentry.d_name, "..", 30) == 0 && swizzle16(dentry.d_ino) != pnum) {
            printf("DotDot entry %s%-1.30s points to wrong place. Fix? ",
                    name, dentry.d_name);
            if (yes()) {
                dentry.d_ino = swizzle16(pnum);
                dirwrite(&ino, j, &dentry);
            }
        }
        if (swizzle16(dentry.d_ino) != pnum &&
                swizzle16(dentry.d_ino) != inum && depth < MAXDEPTH) {
            ename = malloc(strlen(name) + strlen(dentry.d_name) + 2);
            if (ename == NULL) {
                fprintf(stderr, "Not enough memory.\n");
                exit(error |= 8);
            }
            strcpy(ename, name);
            strcat(ename, dentry.d_name);
            strcat(ename, "/");
            ckdir(swizzle16(dentry.d_ino), inum, ename);
            free(ename);
        }
    }
    --depth;
}


/* Pass 5 compares the link counts found in pass 4 with the inodes. */
static void pass5(void)
{
    uint16_t n;
    struct dinode ino;

    for (n = ROOTINODE; n < 8 * (swizzle16(superblock.s_isize) - 2); ++n) {
        iread(n, &ino);

        if (ino.i_mode == 0) {
            if (linkmap[n] != -1)
                panic("Inconsistent linkmap");
            continue;
        }

        if (linkmap[n] == -1 && ino.i_mode != 0)
            panic("Inconsistent linkmap");

        if (linkmap[n] > 0 && swizzle16(ino.i_nlink) != linkmap[n]) {
            printf("Inode %u has link count %u should be %u. Fix? ",
                    n, swizzle16(ino.i_nlink), linkmap[n]);
            if (yes()) {
                ino.i_nlink = swizzle16(linkmap[n]);
                iwrite(n, &ino);
            }
        }

        if (linkmap[n] == 0) {
            if ((swizzle16(ino.i_mode) & F_MASK) == F_BDEV ||
                    (swizzle16(ino.i_mode) & F_MASK) == F_CDEV ||
                    (ino.i_size == 0)) {
                printf("Useless inode %u with mode 0%o has become detached. Link count is %u. Zap? ",
                        n, swizzle16(ino.i_mode), swizzle16(ino.i_nlink));
                if (yes()) {
                    ino.i_nlink = 0;
                    ino.i_mode = 0;
                    iwrite(n, &ino);
                    superblock.s_tinode =
                                swizzle16(swizzle16(superblock.s_tinode) + 1);
                    dwrite((blkno_t) 1, (char *) &superblock);
                }
            } else {
#if 0
                printf("Inode %u has become detached. Link count is %u. Fix? ",
                        n, swizzle16(ino.i_nlink));
                if (yes()) {
                    ino.i_nlink = 1;
                    iwrite(n, &ino);
                    mkentry(n);
                }
#else
                printf("Inode %u has become detached. Link count is %u. ",
                        n, swizzle16(ino.i_nlink));
                if (ino.i_nlink == 0)
                    printf("Zap? ");
                else
                    printf("Fix? ");
                if (yes()) {
                    if (ino.i_nlink == 0) {
                        ino.i_nlink = 0;
                        ino.i_mode = 0;
                        iwrite(n, &ino);
                        superblock.s_tinode =
                                swizzle16(swizzle16(superblock.s_tinode) + 1);
                        dwrite((blkno_t) 1, (char *) &superblock);
                    } else {
                        ino.i_nlink = swizzle16(1);
                        iwrite(n, &ino);
                        mkentry(n);
                    }
                }
#endif
            }
        }

    }
}


/* This makes an entry in "lost+found" for inode n */
static void mkentry(uint16_t inum)
{
    struct dinode rootino;
    struct direct dentry;
    uint16_t d;
    uint32_t dmax;

    iread(ROOTINODE, &rootino);
    dmax = swizzle32(rootino.i_size)/32;
    for (d = 0; d < dmax; ++d) {
        dirread(&rootino, d, &dentry);
        if (dentry.d_ino == 0 && dentry.d_name[0] == '\0') {
            dentry.d_ino = swizzle16(inum);
            sprintf(dentry.d_name, "l+f%u", inum);
            dirwrite(&rootino, d, &dentry);
            return;
        }
    }
    puts("Sorry... No empty slots in root directory.");
    error |= 4;
}

/*
 *  Getblkno gets a pointer index, and a number of a block in the file.
 *  It returns the number of the block on the disk.  A value of zero
 *  means an unallocated block.
 */

static blkno_t getblkno(struct dinode *ino, blkno_t num)
{
    blkno_t indb;
    blkno_t dindb;
    blkno_t *buf;

    if (num < 18) {		/* Direct block */
        return swizzle16(ino->i_addr[num]);
    }
    if (num < 256 + 18) {	/* Single indirect */
        indb = swizzle16(ino->i_addr[18]);
        if (indb == 0)
            return (0);
        buf = (blkno_t *) daread(indb);
        return swizzle16(buf[num - 18]);
    }
    /* Double indirect */
    indb = swizzle16(ino->i_addr[19]);
    if (indb == 0)
        return (0);

    buf = (blkno_t *) daread(indb);

    dindb = swizzle16(buf[(num - (18 + 256)) >> 8]);
    if (dindb == 0)
        return 0;

    buf = (blkno_t *) daread(dindb);

    return swizzle16(buf[(num - (18 + 256)) & 0x00ff]);
}


/*
 *  Setblkno sets the given block number of the given file to the given
 *  disk block number, possibly creating or modifiying the indirect blocks.
 *  A return of zero means there were no blocks available to create an 
 *  indirect block. This should never happen in fsck.
 */
static void setblkno(struct dinode *ino, blkno_t num, blkno_t dnum)
{
    blkno_t indb;
    blkno_t dindb;
    blkno_t *buf;
    /*--    char *zerobuf();--*/


    if (num < 18) {			/* Direct block */
        ino->i_addr[num] = swizzle16(dnum);
    } else if (num < 256 + 18) {	/* Single indirect */
        indb = swizzle16(ino->i_addr[18]);
        if (indb == 0)
            panic("Missing indirect block");

        buf = (blkno_t *) daread(indb);
        buf[num - 18] = swizzle16(dnum);
        dwrite(indb, (char *) buf);
    } else {				/* Double indirect */
        indb = swizzle16(ino->i_addr[19]);
        if (indb == 0)
            panic("Missing indirect block");

        buf = (blkno_t *) daread(indb);
        dindb = swizzle16(buf[(num - (18 + 256)) >> 8]);
        if (dindb == 0)
            panic("Missing indirect block");

        buf = (blkno_t *) daread(dindb);
        buf[(num - (18 + 256)) & 0x00ff] = swizzle16(num);
        dwrite(indb, (char *) buf);
    }
}


/*
 *  blk_alloc0 allocates an unused block.
 *  A returned block number of zero means no more blocks.
 */

/*--- was blk_alloc ---*/
static blkno_t blk_alloc0(struct filesys *filesys)
{
    blkno_t newno;
    blkno_t *buf;
    int16_t j;

    filesys->s_nfree = swizzle16(swizzle16(filesys->s_nfree) - 1);
    newno = swizzle16(filesys->s_free[--filesys->s_nfree]);
    if (!newno) {
        filesys->s_nfree = swizzle16(swizzle16(filesys->s_nfree) + 1);
        return (0);
    }

    /* See if we must refill the s_free array */

    if (!filesys->s_nfree) {
        buf = (blkno_t *) daread(newno);
        filesys->s_nfree = buf[0];
        for (j = 0; j < 50; j++) {
            filesys->s_free[j] = buf[j + 1];
        }
    }

    filesys->s_tfree = swizzle16(swizzle16(filesys->s_tfree) - 1);

    if (newno < swizzle16(filesys->s_isize) || newno >= swizzle16(filesys->s_fsize)) {
        puts("Free list is corrupt.  Did you rebuild it?");
        return (0);
    }
    dwrite((blkno_t) 1, (char *) filesys);
    return (newno);
}


static uint16_t lblk = 0;

static char *daread(uint16_t blk)
{
    static char da_buf[512];

    if (blk == 0)
        panic("reading block 0");

    if (blk == lblk)
        return da_buf;

    if (lseek(dev_fd, offset + blk * 512L, 0) == -1) {
        perror("lseek");
        exit(1);
    }
    if(read(dev_fd, da_buf, 512) != 512) {
        perror("read");
        exit(1);
    }
    lblk = blk;
    return da_buf;
}

static void dwrite(uint16_t blk, char *addr)
{
    if (lseek(dev_fd, offset + blk * 512L, 0) == -1) {
        perror("lseek");
        exit(1);
    }
    if(write(dev_fd, addr, 512) != 512) {
        perror("write");
        exit(1);
    }
    lblk = 0;
}

static void iread(uint16_t ino, struct dinode *buf)
{
    struct dinode *addr;

    addr = (struct dinode *) daread((ino >> 3) + 2);
    memcpy(buf, &addr[ino & 7], sizeof(struct dinode));
}

static void iwrite(uint16_t ino, struct dinode *buf)
{
    struct dinode *addr;

    addr = (struct dinode *) daread((ino >> 3) + 2);
    memcpy(&addr[ino & 7], buf, sizeof(struct dinode));
    dwrite((ino >> 3) + 2, (char *) addr);
}

static void dirread(struct dinode *ino, uint16_t j, struct direct *dentry)
{
    blkno_t blkno;
    char *buf;

    blkno = getblkno(ino, (blkno_t) j / 16);
    if (blkno == 0)
        panic("Missing block in directory");
    buf = daread(blkno);
    memcpy(dentry, buf + 32 * (j % 16), 32);
}

static void dirwrite(struct dinode *ino, uint16_t j, struct direct *dentry)
{
    blkno_t blkno;
    char *buf;

    blkno = getblkno(ino, (blkno_t) j / 16);
    if (blkno == 0)
        panic("Missing block in directory");
    buf = daread(blkno);
    memcpy(buf + 32 * (j % 16), dentry, 32);
    dwrite(blkno, buf);
}

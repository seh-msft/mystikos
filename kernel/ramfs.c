#include <stdlib.h>
#include <fcntl.h>
#include <limits.h>
#include <string.h>
#include <assert.h>
#include <libos/fs.h>
#include <libos/ramfs.h>
#include <dirent.h>
#include "eraise.h"
#include "strings.h"
#include "bufu64.h"
#include "buf.h"
#include "round.h"

#define BLKSIZE 512

/*
**==============================================================================
**
** ramfs_t:
**
**==============================================================================
*/

#define RAMFS_MAGIC 0x28F21778D1E711EA

typedef struct inode inode_t;

typedef struct ramfs
{
    libos_fs_t base;
    uint64_t magic;
    inode_t* root;
}
ramfs_t;

static bool _ramfs_valid(const ramfs_t* ramfs)
{
    return ramfs && ramfs->magic == RAMFS_MAGIC;
}

/*
**==============================================================================
**
** inode_t
**
**==============================================================================
*/

#define INODE_MAGIC 0xcdfbdd61258a4c9d

struct inode
{
    uint64_t magic;
    uint32_t mode; /* Type and mode */
    size_t nlink; /* number of hard links to this inode */
    size_t nopens; /* number of times file is currently opened */
    libos_buf_t buf; /* file or directory data */
};

static bool _inode_valid(const inode_t* inode)
{
    return inode && inode->magic == INODE_MAGIC;
}

static void _inode_free(inode_t* inode)
{
    if (inode)
    {
        libos_buf_release(&inode->buf);
        free(inode);
    }
}

static int _inode_new(
    inode_t* parent,
    const char* name,
    uint32_t mode,
    inode_t** inode_out)
{
    int ret = 0;
    inode_t* inode = NULL;

    if (inode_out)
        *inode_out = NULL;

    if (!name)
        ERAISE(-EINVAL);

    if (!(inode = calloc(1, sizeof(inode_t))))
        ERAISE(-ENOMEM);

    inode->magic = INODE_MAGIC;
    inode->mode = mode;
    inode->nlink = 0;

    /* The root directory is its own parent */
    if (!parent)
        parent = inode;

    /* If new inode is a directory, add the "." and ".." elements */
    if (S_ISDIR(mode))
    {
        /* Add the "." entry */
        {
            struct dirent ent =
            {
                .d_ino = (ino_t)inode,
                .d_off = (off_t)inode->buf.size,
                .d_reclen = sizeof(struct dirent),
                .d_type = DT_DIR,
            };

            if (STRLCPY(ent.d_name, ".") >= sizeof(ent.d_name))
                ERAISE(-ENAMETOOLONG);

            if (libos_buf_append(&inode->buf, &ent, sizeof(ent)) != 0)
                ERAISE(-ENOMEM);

            inode->nlink++;
        }

        /* Add the ".." entry */
        {
            struct dirent ent =
            {
                .d_ino = (ino_t)parent,
                .d_off = (off_t)inode->buf.size,
                .d_reclen = sizeof(struct dirent),
                .d_type = DT_DIR,
            };

            if (STRLCPY(ent.d_name, "..") >= sizeof(ent.d_name))
                ERAISE(-ENAMETOOLONG);

            if (libos_buf_append(&inode->buf, &ent, sizeof(ent)) != 0)
                ERAISE(-ENOMEM);

            parent->nlink++;
        }
    }

    /* Add this inode to the parent inode's directory table */
    if (parent != inode)
    {
        struct dirent ent =
        {
            .d_ino = (ino_t)inode,
            .d_off = (off_t)parent->buf.size,
            .d_reclen = sizeof(struct dirent),
            .d_type = S_ISDIR(mode) ? DT_DIR : DT_REG,
        };

        if (STRLCPY(ent.d_name, name) >= sizeof(ent.d_name))
            ERAISE(-ENAMETOOLONG);

        if (libos_buf_append(&parent->buf, &ent, sizeof(ent)) != 0)
            ERAISE(-ENOMEM);

        parent->nlink++;
    }

    if (inode_out)
        *inode_out = inode;

    inode = NULL;

done:

    if (inode)
        _inode_free(inode);

    return ret;
}

static inode_t* _inode_find_child(
    const inode_t* inode,
    const char* name)
{
    struct dirent* ents = (struct dirent*)inode->buf.data;
    size_t nents = inode->buf.size / sizeof(struct dirent);

    for (size_t i = 0; i < nents; i++)
    {
        if (strcmp(ents[i].d_name, name) == 0)
            return (inode_t*)ents[i].d_ino;
    }

    /* Not found */
    return NULL;
}

/* release this inode and all of its children */
static void _inode_release(inode_t* inode, uint8_t d_type)
{
    struct dirent* ents = (struct dirent*)inode->buf.data;
    size_t nents = inode->buf.size / sizeof(struct dirent);

    /* Free the children first */
    if (d_type == DT_DIR)
    {
        for (size_t i = 0; i < nents; i++)
        {
            const struct dirent* ent = &ents[i];
            inode_t* child;

            if (strcmp(ent->d_name, ".") == 0 || strcmp(ent->d_name, "..") == 0)
                continue;

            child = (inode_t*)ent->d_ino;
            assert(child);
            assert(_inode_valid(child));

            if (child != inode)
                _inode_release(child, ent->d_type);
        }
    }

    /* Free self */
    _inode_free(inode);
}

/*
**==============================================================================
**
** libos_file_t
**
**==============================================================================
*/

#define FILE_MAGIC  0xdfe1d5c160064f8e

struct libos_file
{
    uint64_t magic;
    inode_t* inode;
    size_t offset; /* the current file offset (files) */
    uint32_t access; /* (O_RDONLY | O_RDWR | O_WRONLY) */
};

static bool _file_valid(const libos_file_t* file)
{
    return file && file->magic == FILE_MAGIC;
}

static void* _file_data(const libos_file_t* file)
{
    return file->inode->buf.data;
}

static size_t _file_size(const libos_file_t* file)
{
    return file->inode->buf.size;
}

static void* _file_current(libos_file_t* file)
{
    return (uint8_t*)_file_data(file) + file->offset;
}

/*
**==============================================================================
**
** local definitions:
**
**==============================================================================
*/

#define MODE_R   (S_IRUSR | S_IRGRP | S_IROTH)
#define MODE_W   (S_IWUSR | S_IWGRP | S_IWOTH)
#define MODE_X   (S_IXUSR | S_IXGRP | S_IXOTH)
#define MODE_RWX (MODE_R | MODE_W | MODE_X)

/* Assume that d_ino is 8 bytes (big enough to hold a pointer) */
_Static_assert(sizeof(((struct dirent*)0)->d_ino) == 8, "d_ino");

/* Assume struct dirent is eight-byte aligned */
_Static_assert(sizeof(struct dirent) % 8 == 0, "dirent");

static int _split_path(
    const char* path,
    char dirname[PATH_MAX],
    char basename[PATH_MAX])
{
    int ret = 0;
    char* slash;

    /* Reject paths that are too long. */
    if (strlen(path) >= PATH_MAX)
        ERAISE(-EINVAL);

    /* Reject paths that are not absolute */
    if (path[0] != '/')
        ERAISE(-EINVAL);

    /* Handle root directory up front */
    if (strcmp(path, "/") == 0)
    {
        strlcpy(dirname, "/", PATH_MAX);
        strlcpy(basename, "/", PATH_MAX);
        goto done;
    }

    /* This cannot fail (prechecked) */
    if (!(slash = strrchr(path, '/')))
        ERAISE(-EINVAL);

    /* If path ends with '/' character */
    if (!slash[1])
        ERAISE(-EINVAL);

    /* Split the path */
    {
        if (slash == path)
        {
            strlcpy(dirname, "/", PATH_MAX);
        }
        else
        {
            size_t index = (size_t)(slash - path);
            strlcpy(dirname, path, PATH_MAX);

            if (index < PATH_MAX)
                dirname[index] = '\0';
            else
                dirname[PATH_MAX - 1] = '\0';
        }

        strlcpy(basename, slash + 1, PATH_MAX);
    }

done:
    return ret;
}

static int _path_to_inode(
    ramfs_t* ramfs,
    const char* path,
    inode_t** inode_out)
{
    int ret = 0;
    const char* elements[PATH_MAX];
    const size_t MAX_ELEMENTS = sizeof(elements) / sizeof(elements[0]);
    size_t nelements = 0;
    char buf[PATH_MAX];
    inode_t* inode = NULL;

    if (inode_out)
        *inode_out = NULL;

    if (!path || !inode_out)
        ERAISE_QUIET(-EINVAL);

    /* Fail if path does not begin with '/' */
    if (path[0] != '/')
        ERAISE_QUIET(-EINVAL);

    /* Copy the path */
    if (STRLCPY(buf, path) >= sizeof(buf))
        ERAISE_QUIET(-ENAMETOOLONG);

    /* The first element is the root directory */
    elements[nelements++] = "/";

    /* Split the path into components */
    {
        char* p;
        char* save;

        for (p = strtok_r(buf, "/", &save); p; p = strtok_r(NULL, "/", &save))
        {
            assert(nelements < MAX_ELEMENTS);
            elements[nelements++] = p;
        }
    }

    /* First element should be "/" */
    assert(strcmp(elements[0], "/") == 0);

    /* search for the inode */
    {
        if (nelements == 1)
        {
            inode = ramfs->root;
        }
        else
        {
            inode_t* current = ramfs->root;

            for (size_t i = 1; i < nelements; i++)
            {
                if (!(current = _inode_find_child(current, elements[i])))
                    ERAISE_QUIET(-ENOENT);

                if (i + 1 == nelements)
                {
                    inode = current;
                    break;
                }
            }
        }

        if (!inode)
            ERAISE_QUIET(-ENOENT);
    }

    *inode_out = inode;

done:
    return ret;
}

/*
**==============================================================================
**
** interface:
**
**==============================================================================
*/

static int _fs_release(libos_fs_t* fs)
{
    int ret = 0;
    ramfs_t* ramfs = (ramfs_t*)fs;

    if (!_ramfs_valid(ramfs))
        ERAISE(-EINVAL);

    _inode_release(ramfs->root, DT_DIR);

    free(ramfs);

done:
    return ret;
}

static int _fs_creat(
    libos_fs_t* fs,
    const char* pathname,
    mode_t mode,
    libos_file_t** file)
{
    int ret = 0;
    const int flags = O_CREAT | O_WRONLY | O_TRUNC;

    if (!fs)
        ERAISE(-EINVAL);

    ECHECK((*fs->fs_open)(fs, pathname, flags, mode, file));

done:
    return ret;
}

static int _fs_open(
    libos_fs_t* fs,
    const char* pathname,
    int flags,
    mode_t mode,
    libos_file_t** file_out)
{
    ramfs_t* ramfs = (ramfs_t*)fs;
    inode_t* inode = NULL;
    libos_file_t* file = NULL;
    int ret = 0;
    int errnum;
    bool is_i_new = false;

    if (file_out)
        *file_out = NULL;

    if (!_ramfs_valid(ramfs))
        ERAISE(-EINVAL);

    if (!pathname)
        ERAISE(-EINVAL);

    if (!file_out)
        ERAISE(-EINVAL);

    /* Create the file object */
    if (!(file = calloc(1, sizeof(libos_file_t))))
        ERAISE(-ENOMEM);

    errnum = _path_to_inode(ramfs, pathname, &inode);

    /* If the file already exists */
    if (errnum == 0)
    {
        if ((flags & O_CREAT) && (flags & O_EXCL))
            ERAISE(-EEXIST);

        if ((flags & O_DIRECTORY) && !S_ISDIR(inode->mode))
            ERAISE(-ENOTDIR);

        if ((flags & O_TRUNC))
            libos_buf_clear(&inode->buf);

        if ((flags & O_APPEND))
            file->offset = inode->buf.size;
    }
    else if (errnum == -ENOENT)
    {
        char dirname[PATH_MAX];
        char basename[PATH_MAX];
        inode_t* parent;

        is_i_new = true;

        if (!(flags & O_CREAT))
            ERAISE(-ENOENT);

        /* Split the path into parent directory and file name */
        ECHECK(_split_path(pathname, dirname, basename));

        /* Get the inode of the parent directory. */
        ECHECK(_path_to_inode(ramfs, dirname, &parent));

        /* Create the new file inode */
        ECHECK(_inode_new(parent, basename, (S_IFREG | mode), &inode));
    }
    else
    {
        ERAISE(-errnum);
    }

    /* Initialize the file */
    file->magic = FILE_MAGIC;
    file->inode = inode;
    file->access = (flags & (O_RDONLY | O_RDWR | O_WRONLY));
    inode->nopens++;

    assert(_file_valid(file));

    *file_out = file;
    file = NULL;
    inode = NULL;

done:

    if (inode && is_i_new)
        free(inode);

    if (file)
        free(file);

    return ret;
}

static off_t _fs_lseek(
    libos_fs_t* fs,
    libos_file_t* file,
    off_t offset,
    int whence)
{
    ramfs_t* ramfs = (ramfs_t*)fs;
    off_t ret = 0;
    off_t new_offset;

    if (!_ramfs_valid(ramfs) || !_file_valid(file))
        ERAISE(-EINVAL);

    switch (whence)
    {
        case SEEK_SET:
        {
            new_offset = offset;
            break;
        }
        case SEEK_CUR:
        {
            new_offset = (off_t)file->offset + offset;
            break;
        }
        case SEEK_END:
        {
            new_offset = (off_t)_file_size(file) + offset;
            break;
        }
        default:
        {
            ERAISE(-EINVAL);
        }
    }

    /* ATTN: seeking beyond the end (to create a hole) is not supported */

    /* Check whether new offset if out of range */
    if (new_offset < 0 || new_offset > (off_t)file->offset)
        ERAISE(-EINVAL);

    file->offset = (size_t)new_offset;

    ret = new_offset;

done:
    return ret;
}

static ssize_t _fs_read(
    libos_fs_t* fs,
    libos_file_t* file,
    void* buf,
    size_t count)
{
    ramfs_t* ramfs = (ramfs_t*)fs;
    ssize_t ret = 0;
    size_t n;

    if (!_ramfs_valid(ramfs))
        ERAISE(-EINVAL);

    if (!_file_valid(file))
        ERAISE(-EINVAL);

    if (!buf && count)
        ERAISE(-EINVAL);

    /* reading zero bytes is okay */
    if (!count)
        goto done;

    /* Verify that the offset is in bounds */
    if (file->offset > _file_size(file))
        ERAISE(-EINVAL);

    /* Read count bytes from the file or directory */
    {
        size_t remaining = _file_size(file) - file->offset;

        if (remaining == 0)
        {
            /* end of file */
            goto done;
        }

        n = (count < remaining) ? count : remaining;
        memcpy(buf, _file_current(file), n);
        file->offset += n;
    }

    ret = (ssize_t)n;

done:
    return ret;
}

static ssize_t _fs_write(
    libos_fs_t* fs,
    libos_file_t* file,
    const void* buf,
    size_t count)
{
    ramfs_t* ramfs = (ramfs_t*)fs;
    ssize_t ret = 0;

    if (!_ramfs_valid(ramfs))
        ERAISE(-EINVAL);

    if (!_file_valid(file))
        ERAISE(-EINVAL);

    if (!buf && count)
        ERAISE(-EINVAL);

    /* reading zero bytes is okay */
    if (!count)
        goto done;

    /* Verify that the offset is in bounds */
    if (file->offset > _file_size(file))
        ERAISE(-EINVAL);

    /* Write count bytes to the file or directory */
    {
        size_t new_offset = file->offset + count;

        if (new_offset > _file_size(file))
        {
            if (libos_buf_resize(&file->inode->buf, new_offset) != 0)
                ERAISE(-ENOMEM);
        }

        memcpy(_file_current(file), buf, count);
        file->offset = new_offset;
    }

    ret = (ssize_t)count;

done:
    return ret;
}

static ssize_t _fs_readv(
    libos_fs_t* fs,
    libos_file_t* file,
    struct iovec* iov,
    int iovcnt)
{
    ssize_t ret = 0;
    ramfs_t* ramfs = (ramfs_t*)fs;
    ssize_t total = 0;

    if (!_ramfs_valid(ramfs) || !_file_valid(file))
        ERAISE(-EINVAL);

    for (int i = 0; i < iovcnt; i++)
    {
        ssize_t n;
        void* buf = iov[i].iov_base;
        size_t count = iov[i].iov_len;

        ECHECK((n = (*fs->fs_read)(fs, file, buf, count)));

        total += n;

        if ((size_t)n < count)
            break;
    }

    ret = total;

done:
    return ret;
}

static ssize_t _fs_writev(
    libos_fs_t* fs,
    libos_file_t* file,
    const struct iovec* iov,
    int iovcnt)
{
    ssize_t ret = 0;
    ramfs_t* ramfs = (ramfs_t*)fs;
    ssize_t total = 0;

    if (!_ramfs_valid(ramfs) || !_file_valid(file))
        ERAISE(-EINVAL);

    for (int i = 0; i < iovcnt; i++)
    {
        ssize_t n;
        const void* buf = iov[i].iov_base;
        size_t count = iov[i].iov_len;

        ECHECK((n = (*fs->fs_write)(fs, file, buf, count)));

        total += n;

        if ((size_t)n < count)
            break;
    }

    ret = total;

done:
    return ret;
}

static int _fs_close(libos_fs_t* fs, libos_file_t* file)
{
    int ret = 0;
    ramfs_t* ramfs = (ramfs_t*)fs;

    if (!_ramfs_valid(ramfs) || !_file_valid(file))
        ERAISE(-EINVAL);

    assert(file->inode);
    assert(_inode_valid(file->inode));
    assert(file->inode->nopens > 0);
    file->inode->nopens--;

    memset(file, 0xdd, sizeof(libos_file_t));
    free(file);

done:
    return ret;
}

static int _stat(inode_t* inode, struct stat* statbuf)
{
    int ret = 0;
    struct stat buf;

    if (!_inode_valid(inode) || !statbuf)
        ERAISE(-EINVAL);

    memset(&buf, 0, sizeof(buf));
    buf.st_dev = 0;
    buf.st_ino = (ino_t)inode;
    buf.st_mode = inode->mode;
    buf.st_nlink = inode->nlink;
    buf.st_uid = 0; /* ATTN: assumes root uid */
    buf.st_gid = 0; /* ATTN: assumes root gid */
    buf.st_rdev = 0;
    buf.st_size = (off_t)inode->buf.size;
    buf.st_blksize = BLKSIZE;
    buf.st_blocks = libos_round_up_off(buf.st_size, BLKSIZE) / BLKSIZE;
    memset(&buf.st_atim, 0, sizeof(buf.st_atim)); /* ATTN: unsupported */
    memset(&buf.st_mtim, 0, sizeof(buf.st_mtim)); /* ATTN: unsupported */
    memset(&buf.st_ctim, 0, sizeof(buf.st_ctim)); /* ATTN: unsupported */

    *statbuf = buf;

done:
    return ret;
}

static int _fs_stat(libos_fs_t* fs, const char* pathname, struct stat* statbuf)
{
    int ret = 0;
    ramfs_t* ramfs = (ramfs_t*)fs;
    inode_t* inode;

    if (!_ramfs_valid(ramfs) || !pathname || !statbuf)
        ERAISE(-EINVAL);

    ECHECK(_path_to_inode(ramfs, pathname, &inode));
    ECHECK(_stat(inode, statbuf));

done:
    return ret;
}

static int _fs_fstat(libos_fs_t* fs, libos_file_t* file, struct stat* statbuf)
{
    int ret = 0;
    ramfs_t* ramfs = (ramfs_t*)fs;

    if (!_ramfs_valid(ramfs) || !_file_valid(file) || !statbuf)
        ERAISE(-EINVAL);

    assert(_inode_valid(file->inode));
    ECHECK(_stat(file->inode, statbuf));

done:
    return ret;
}

/*============================================================================*/

#pragma GCC diagnostic ignored "-Wunused-parameter"

static int _fs_link(libos_fs_t* fs, const char* oldpath, const char* newpath)
{
    return -EINVAL;
}

static int _fs_unlink(libos_fs_t* fs, const char* pathname)
{
    return -EINVAL;
}

static int _fs_rename(libos_fs_t* fs, const char* oldpath, const char* newpath)
{
    return -EINVAL;
}

static int _fs_truncate(libos_fs_t* fs, const char* path, off_t length)
{
    return -EINVAL;
}

static int _fs_ftruncate(libos_fs_t* fs, int fd, off_t length)
{
    return -EINVAL;
}

static int _fs_mkdir(libos_fs_t* fs, const char* pathname, mode_t mode)
{
    int ret = 0;
    ramfs_t* ramfs = (ramfs_t*)fs;
    char dirname[PATH_MAX];
    char basename[PATH_MAX];
    inode_t* parent_inode;

    if (!_ramfs_valid(ramfs) || !pathname)
        ERAISE(EINVAL);

    ECHECK(_split_path(pathname, dirname, basename));
    ECHECK(_path_to_inode(ramfs, dirname, &parent_inode));

    /* The parent must be a directory */
    if (!S_ISDIR(parent_inode->mode))
        ERAISE(ENOTDIR);

    /* Check whether the pathname already exists */
    if (_inode_find_child(parent_inode, basename) != NULL)
        ERAISE(EEXIST);

    /* create the directory */
    ECHECK(_inode_new(parent_inode, basename, (S_IFDIR | mode), NULL));

done:
    return ret;
}

static int _fs_rmdir(libos_fs_t* fs, const char* pathname)
{
    int ret = 0;
    ramfs_t* ramfs = (ramfs_t*)fs;
    char dirname[PATH_MAX];
    char basename[PATH_MAX];
    inode_t* parent_inode;
    inode_t* child_inode;

    if (!_ramfs_valid(ramfs) || !pathname)
        ERAISE(EINVAL);

    /* Get the child inode */
    ECHECK(_path_to_inode(ramfs, pathname, &child_inode));

    /* The child must be a directory */
    if (!S_ISDIR(child_inode->mode))
        ERAISE(ENOTDIR);

    /* Make sure the directory has no children */
    if (child_inode->buf.size > (2 * sizeof(struct dirent)))
        ERAISE(ENOTEMPTY);

    /* Get the parent inode */
    ECHECK(_split_path(pathname, dirname, basename));
    ECHECK(_path_to_inode(ramfs, dirname, &parent_inode));

    /* Find and remove the directory entry */
    {
        struct dirent* ents = (struct dirent*)parent_inode->buf.data;
        size_t nents = parent_inode->buf.size / sizeof(struct dirent);
        bool found = false;

        for (size_t i = 0; i < nents; i++)
        {
            if (strcmp(ents[i].d_name, basename) == 0)
            {
                const size_t pos = i * sizeof(struct dirent);
                const size_t size = sizeof(struct dirent);
                if (libos_buf_remove(&parent_inode->buf, pos, size) != 0)
                {
                    assert(0);
                    ERAISE(ENOMEM);
                }
                found = true;
                break;
            }
        }

        if (!found)
        {
            assert("unexpected" == NULL);
            ERAISE(ENOENT);
        }
    }

    printf("nnnnnnnnnnnnnnnnnnnnnnnnnnnnn=%zu\n", child_inode->nlink);

    /* If only link that remains is self-reference, then free inode */
    if (child_inode->nlink-- == 1)
        _inode_free(child_inode);

done:
    return ret;
}

static int _fs_getdents64(
    libos_fs_t* fs,
    libos_file_t* file,
    struct dirent* dirp,
    size_t count)
{
    int ret = 0;
    ramfs_t* ramfs = (ramfs_t*)fs;
    size_t n = count / sizeof(struct dirent);
    size_t bytes = 0;

    if (!_ramfs_valid(ramfs) || !_file_valid(file) || !dirp)
        ERAISE(-EINVAL);

    if (count == 0)
        goto done;

    for (size_t i = 0; i < n; i++)
    {
        struct dirent ent;
        ssize_t r;

        /* Read next entry and break on end-of-file */
        if ((r = _fs_read(fs, file, &ent, sizeof(ent))) == 0)
            break;

        /* Fail if exactly one entry was not read */
        if (r != sizeof(ent))
        {
            assert("unexpected panic" == NULL);
            ERAISE(-EINVAL);
        }

        *dirp = ent;
        bytes += sizeof(struct dirent);
        dirp++;
    }

    ret = (int)bytes;

done:
    return ret;
}

int libos_init_ramfs(libos_fs_t** fs_out)
{
    int ret = 0;
    ramfs_t* ramfs = NULL;
    static libos_fs_t _base =
    {
        _fs_release,
        _fs_creat,
        _fs_open,
        _fs_lseek,
        _fs_read,
        _fs_write,
        _fs_readv,
        _fs_writev,
        _fs_close,
        _fs_stat,
        _fs_fstat,
        _fs_link,
        _fs_unlink,
        _fs_rename,
        _fs_truncate,
        _fs_ftruncate,
        _fs_mkdir,
        _fs_rmdir,
        _fs_getdents64,
    };
    inode_t* root_inode = NULL;

    if (fs_out)
        *fs_out = NULL;

    if (!fs_out)
        ERAISE(-EINVAL);

    if (!(ramfs = calloc(1, sizeof(ramfs_t))))
        ERAISE(-ENOMEM);

    ECHECK(_inode_new(NULL, "/", (S_IFDIR | MODE_RWX), &root_inode));

    ramfs->magic = RAMFS_MAGIC;
    ramfs->base = _base;
    ramfs->root = root_inode;
    root_inode = NULL;

    *fs_out = &ramfs->base;
    ramfs = NULL;

done:

    if (ramfs)
        free(ramfs);

    if (root_inode)
        free(root_inode);

    return ret;
}

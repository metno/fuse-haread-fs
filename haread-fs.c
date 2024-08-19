
/*
 * HAREAD-FS  High Availability Read only file system
 *
 *
 * Initialized from https://github.com/libfuse/libfuse/wiki/Filesystems . Specially the rofs one
 * LICENCE : GPL v3
 */

#define FUSE_USE_VERSION 26

static const char *rofsVersion = "2024.09.24";

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <stdio.h>
#include <strings.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/xattr.h>
#include <dirent.h>
#include <unistd.h>
#include <fuse.h>
#include <features.h>
#include <signal.h>
#include <setjmp.h>
#include <time.h>
#include <pthread.h>
#include <glib.h>

// Debug flag
#define DEBUG_ON 1

#if DEBUG_ON
#define DEBUG(fmt, ...) fprintf(stderr, "DEBUG: %s:%d:%s(): \n" fmt, \
                                __FILE__, __LINE__, __func__, __VA_ARGS__)
#else
#define DEBUG(fmt, ...) /* Nothing */
#endif

char *Currfs;
int Fscount;
char **Fss; // Underlying filesystems


// Key: file system path. Value 1 = fs Ok, 0 = fs Blocks
GHashTable *FSOkMap;

// This function was generated with ChatGPT which seems like a copy and paste from here Haha :
// https://stackoverflow.com/questions/9210528/split-string-with-delimiters-in-c
char **split_string(char *str, const char *delimiter)
{
    char **result = 0;
    size_t count = 0;
    char *tmp = str;
    char *last_comma = 0;
    char delim[2];
    delim[0] = delimiter[0];
    delim[1] = 0;

    while (*tmp)
    {
        if (delim[0] == *tmp)
        {
            count++;
            last_comma = tmp;
        }
        tmp++;
    }

    count += last_comma < (str + strlen(str) - 1);
    count++;

    result = malloc(sizeof(char *) * count);

    if (result)
    {
        size_t idx = 0;
        char *token = strtok(str, delim);

        while (token)
        {
            *(result + idx++) = strdup(token);
            token = strtok(0, delim);
        }
        *(result + idx) = 0;
    }

    return result;
}

// Translate an fs path into it's underlying filesystem path
// Fix todo no need to use a global var
static char *translate_path(const char *path)
{

    char *rPath = malloc(sizeof(char) * (strlen(path) + strlen(Currfs) + 1));

    strcpy(rPath, Currfs);
    if (rPath[strlen(rPath) - 1] == '/')
    {
        rPath[strlen(rPath) - 1] = '\0';
    }
    strcat(rPath, path);

    return rPath;
}



int retrieve_from_hash_table(GHashTable *hash_table, char *key)
{
    if (g_hash_table_contains(hash_table, key))
    {
        return GPOINTER_TO_INT(g_hash_table_lookup(hash_table, key));
    }
    else
    {
        return -1; // or any other value that indicates the key doesn't exist
    }
}



/******************************
 *
 * Callbacks for FUSE
 *
 ******************************/

typedef struct
{
    const char *path;
    struct stat *stbuf;
} getattr_args;

// The struct to pass arguments to the thread
typedef struct arg_struct_lstat
{
    char *path;
    struct stat *st_data;
    int res;
    int errnum;
} arg_struct_lstat;

// The lstat function to run in a separate thread
void *thread_lstat(void *arguments)
{
    arg_struct_lstat *args = (arg_struct_lstat *)arguments;
    args->res = lstat(args->path, args->st_data);
    if (args->res == -1)
    {
        args->errnum = errno;
    }
    return NULL;
}

static int callback_getattr(const char *path, struct stat *st_data)
{
    DEBUG("CALLLBACK_GETATRR %s\n", "sd");

    int res = 0;
    char *ipath = NULL;
    arg_struct_lstat args;

    int all_timed_out = 1;
    for (int i = 0; i < Fscount; i++)
    {
        Currfs = Fss[i];

        
        int fs_status = retrieve_from_hash_table(FSOkMap, Currfs);
        if (fs_status == 0)
        {
            continue;
        }
        ipath = translate_path(path);

        pthread_t thread_id;

        args.path = ipath;
        args.st_data = st_data;

        // Create a new thread to call lstat
        pthread_create(&thread_id, NULL, thread_lstat, &args);

        // Set the timeout to 1 second
        struct timespec timeout;
        clock_gettime(CLOCK_REALTIME, &timeout);
        timeout.tv_sec += 5;

        // Wait for the thread to complete with a timeout
        if (pthread_timedjoin_np(thread_id, NULL, &timeout) != 0)
        {
            // The call to lstat timed out
            DEBUG("Timeout on  %s\n", Currfs);
           
           
            continue;
        }
        
        all_timed_out = 0;
        res = args.res;
        free(ipath);

        if (res == 0)
        {
            return 0;
        }
    }

    if  (all_timed_out ) {
        return - ETIMEDOUT;
    }
    if (res == -1)
    {
        return -args.errnum;
    }
    return 0;
}

static int callback_readlink(const char *path, char *buf, size_t size)
{
   
    int res;
    char *ipath;
    ipath = translate_path(path);

    res = readlink(ipath, buf, size - 1);
    free(ipath);
    if (res == -1)
    {
        return -errno;
    }
    buf[res] = '\0';
    return 0;
}

// The struct to pass directory path to the thread
typedef struct arg_struct_opendir
{
    char *path;
    DIR *dp;
    int res;
} arg_struct_opendir;

// The opendir function to run in a separate thread
void *thread_opendir(void *arguments)
{
    arg_struct_opendir *args = (arg_struct_opendir *)arguments;
    args->dp = opendir(args->path);
    if (args->dp == NULL)
    {
        args->res = errno;
    }
    else
    {
        args->res = 0;
    }
    return NULL;
}




// Function to insert key-value pair into hash table
void insert_to_hash_table(GHashTable *hash_table, char *key, int value)
{
    g_hash_table_insert(hash_table, g_strdup(key), GINT_TO_POINTER(value));
}

int filldir(const char *path, void *buf, fuse_fill_dir_t filler, GHashTable *filesMap)
{
    // DIR *dp;
    struct dirent *de;
    pthread_t thread_id;
    arg_struct_opendir args;
    char *ipath;

    ipath = translate_path(path);

    args.path = ipath;

    // Create a new thread to open the directory
    pthread_create(&thread_id, NULL, thread_opendir, &args);

    // Set the timeout to 1 second
    struct timespec timeout;
    clock_gettime(CLOCK_REALTIME, &timeout);
    timeout.tv_sec += 5;

    // Wait for the thread to complete with a timeout
    if (pthread_timedjoin_np(thread_id, NULL, &timeout) != 0)
    {
        // The call to opendir timed out
        DEBUG("Call to opendir(%s) timed out\n", ipath);
        free(ipath);
        return ETIMEDOUT;
    }
    
    free(ipath);

    if (args.dp == NULL)
    {
        return args.res;
    }

    while ((de = readdir(args.dp)) != NULL)
    {
        struct stat st;
        memset(&st, 0, sizeof(st));
        st.st_ino = de->d_ino;
        st.st_mode = de->d_type << 12;
        if (g_hash_table_contains(filesMap, de->d_name))
        {
            continue;
        }
        if (filler(buf, de->d_name, &st, 0))
            break;
        insert_to_hash_table(filesMap, de->d_name, 1);
    }

    closedir(args.dp);
    
    return 0;
}



static int callback_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi)
{

    (void)offset;
    (void)fi;

    GHashTable *filesMap = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    Currfs = Fss[0];
    int fs_status = retrieve_from_hash_table(FSOkMap, Currfs);
  

    int ret1 = 0;
    if (fs_status == 1)
    { // Fs Ok
        ret1 = filldir(path, buf, filler, filesMap);
        if (ret1 != 0 && (ret1 != ENOENT && ret1 != ETIMEDOUT))
        {
            return -ret1;
        }
    }

    // Next fs
    Currfs = Fss[1];
    fs_status = retrieve_from_hash_table(FSOkMap, Currfs);
    int ret2 = 0;
    if (fs_status == 1)
    {
        ret2 = filldir(path, buf, filler, filesMap);
    }

    g_hash_table_destroy(filesMap);

    if (ret1 == 0 || ret2 == 0)
    {
        return 0;
    }

    if (ret2 != 0)
    {
        return -ret2;
    }

    return 0;
}

static int callback_mknod(const char *path, mode_t mode, dev_t rdev)
{
    (void)path;
    (void)mode;
    (void)rdev;
    return -EROFS;
}

static int callback_mkdir(const char *path, mode_t mode)
{
    (void)path;
    (void)mode;
    return -EROFS;
}

static int callback_unlink(const char *path)
{
    (void)path;
    return -EROFS;
}

static int callback_rmdir(const char *path)
{
    (void)path;
    return -EROFS;
}

static int callback_symlink(const char *from, const char *to)
{
    (void)from;
    (void)to;
    return -EROFS;
}

static int callback_rename(const char *from, const char *to)
{
    (void)from;
    (void)to;
    return -EROFS;
}

static int callback_link(const char *from, const char *to)
{
    (void)from;
    (void)to;
    return -EROFS;
}

static int callback_chmod(const char *path, mode_t mode)
{
    (void)path;
    (void)mode;
    return -EROFS;
}

static int callback_chown(const char *path, uid_t uid, gid_t gid)
{
    (void)path;
    (void)uid;
    (void)gid;
    return -EROFS;
}

static int callback_truncate(const char *path, off_t size)
{
    (void)path;
    (void)size;
    return -EROFS;
}

static int callback_utime(const char *path, struct utimbuf *buf)
{
    (void)path;
    (void)buf;
    return -EROFS;
}

typedef struct arg_struct_open
{
    char *path;
    int flags;
    int res;
    int errnum;

} arg_struct_open;

void *thread_open(void *arguments)
{
    arg_struct_open *args = (arg_struct_open *)arguments;
    args->res = open(args->path, args->flags);
    if (args->res == -1)
    {
        args->errnum = errno;
    }
    return NULL;
}

static int callback_open(const char *path, struct fuse_file_info *finfo)
{

    int flags = finfo->flags;

    if ((flags & O_WRONLY) || (flags & O_RDWR) || (flags & O_CREAT) || (flags & O_EXCL) || (flags & O_TRUNC) || (flags & O_APPEND))
    {
        return -EROFS;
    }
   
    DEBUG("CALLLBACK_OPEN %s\n", path);

    arg_struct_open args;
    int all_timed_out = 1;
    for (int i = 0; i < Fscount; i++) // Try open .
    {
        Currfs = Fss[i];
        char *ipath = translate_path(path);
        pthread_t thread_id;
        args.path = ipath;
        args.flags = flags;
        pthread_create(&thread_id, NULL, thread_open, &args);

        struct timespec timeout;
        clock_gettime(CLOCK_REALTIME, &timeout);
        timeout.tv_sec += 5;

        if (pthread_timedjoin_np(thread_id, NULL, &timeout) != 0)
        {
            DEBUG("callback_read: open(%s) timed out. Trying next fs if any\n", ipath);
            free(ipath);
            continue;
        }
        all_timed_out = 0;
        free(ipath);

        if (args.res != -1  ) {
            close(args.res);
            break;
        }
        if ( args.res == -1 && args.errnum == ENOENT) {
            continue; // Try next fs
        } else if ( args.res == -1 && args.errnum != ENOENT) {
            return - args.errnum;
        }    
    }

    if (all_timed_out) {
        return -ETIMEDOUT;
    }
    return 0;
}

static int callback_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *finfo)
{

    int res;
    (void)finfo;
    char *ipath;
    
    arg_struct_open args;

    for (int i = 0; i < Fscount; i++) // Try open .
    {
        Currfs = Fss[i];
        int fs_status = retrieve_from_hash_table(FSOkMap, Currfs);
        if (fs_status == 0)
        {
            continue;
        }

        pthread_t thread_id;
        ipath = translate_path(path);
        args.path = ipath;
        args.flags = O_RDONLY;

       
        //DEBUG("CALLLBACK_READ %s\n", ipath);

        pthread_create(&thread_id, NULL, thread_open, &args);

        struct timespec timeout;
        clock_gettime(CLOCK_REALTIME, &timeout);
        timeout.tv_sec += 5;

        if (pthread_timedjoin_np(thread_id, NULL, &timeout) != 0)
        {
            DEBUG("callback_read: open(%s) timed out. Trying next fs if any\n", ipath);
            free(ipath);
            continue;
        }
        free(ipath);
        if (args.res != -1  ) {
            break;
        }
        if ( args.res == -1 && args.errnum == ENOENT) {
            continue; // Try next fs
        } else if ( args.res == -1 && args.errnum != ENOENT) {
            return - args.errnum;
        }
    }

    if (args.res == -1)
    {
        return -args.errnum;
    }

 
    res = pread(args.res, buf, size, offset);
    close(args.res);
    
    if (res == -1)
    {
        return -errno;
    }
   
    return res;
}

static int callback_write(const char *path, const char *buf, size_t size, off_t offset, struct fuse_file_info *finfo)
{
    (void)path;
    (void)buf;
    (void)size;
    (void)offset;
    (void)finfo;
    return -EROFS;
}

static int callback_statfs(const char *path, struct statvfs *st_buf)
{
    DEBUG("CALLLBACK_STATFS %s", "sd");
    int res;
    char *ipath;
    ipath = translate_path(path);

    res = statvfs(path, st_buf);
    free(ipath);
    if (res == -1)
    {
        return -errno;
    }
    return 0;
}

static int callback_release(const char *path, struct fuse_file_info *finfo)
{
    (void)path;
    (void)finfo;
    return 0;
}

static int callback_fsync(const char *path, int crap, struct fuse_file_info *finfo)
{
    (void)path;
    (void)crap;
    (void)finfo;
    return 0;
}

static int callback_access(const char *path, int mode)

{
    
    int res;
    char *ipath;
    ipath = translate_path(path);
    DEBUG("CALLLBACK_ACCESS %s\n", ipath);
    if (mode & W_OK)
    {
        return -EROFS;
    }
    res = access(ipath, mode);
    free(ipath);
    if (res == -1)
    {
        return -errno;
    }
    return res;
}

/*
 * Set the value of an extended attribute
 */
static int callback_setxattr(const char *path, const char *name, const char *value, size_t size, int flags)
{
    (void)path;
    (void)name;
    (void)value;
    (void)size;
    (void)flags;
    return -EROFS;
}

/*
 * Get the value of an extended attribute.
 */
static int callback_getxattr(const char *path, const char *name, char *value, size_t size)
{
    DEBUG("CALLLBACK_GETXATTR %s\n", "sd");
    int res;
    char *ipath;

    ipath = translate_path(path);
    res = lgetxattr(ipath, name, value, size);
    free(ipath);
    if (res == -1)
    {
        return -errno;
    }
    return res;
}

/*
 * List the supported extended attributes.
 */
static int callback_listxattr(const char *path, char *list, size_t size)
{
    DEBUG("CALLLBACK_LISTXATTR %s", "sd");
    int res;
    char *ipath;

    ipath = translate_path(path);
    res = llistxattr(ipath, list, size);
    free(ipath);
    if (res == -1)
    {
        return -errno;
    }
    return res;
}

/*
 * Remove an extended attribute.
 */
static int callback_removexattr(const char *path, const char *name)
{
    (void)path;
    (void)name;
    return -EROFS;
}

struct fuse_operations callback_oper = {
    .getattr = callback_getattr,
    .readlink = callback_readlink,
    .readdir = callback_readdir,
    .mknod = callback_mknod,
    .mkdir = callback_mkdir,
    .symlink = callback_symlink,
    .unlink = callback_unlink,
    .rmdir = callback_rmdir,
    .rename = callback_rename,
    .link = callback_link,
    .chmod = callback_chmod,
    .chown = callback_chown,
    .truncate = callback_truncate,
    .utime = callback_utime,
    .open = callback_open,
    .read = callback_read,
    .write = callback_write,
    .statfs = callback_statfs,
    .release = callback_release,
    .fsync = callback_fsync,
    .access = callback_access,

    /* Extended attributes support for userland interaction */
    .setxattr = callback_setxattr,
    .getxattr = callback_getxattr,
    .listxattr = callback_listxattr,
    .removexattr = callback_removexattr

};
enum
{
    KEY_HELP,
    KEY_VERSION,
};

static void usage(const char *progname)
{
    fprintf(stdout,
            "usage: %s paths mountpoint [options]\n"
            "\n"
            "   Mounts paths as a read-only mount at mountpoint\n"
            "\n"
            "general options:\n"
            "   -o opt,[opt...]     mount options\n"
            "   -h  --help          print help\n"
            "   -V  --version       print version\n"
            "\n",
            progname);
}

static int rofs_parse_opt(void *data, const char *arg, int key,
                          struct fuse_args *outargs)
{
    (void)data;

    switch (key)
    {
    case FUSE_OPT_KEY_NONOPT:
        if (Currfs == 0)
        {
            Currfs = strdup(arg);
            return 0;
        }
        else
        {
            return 1;
        }
    case FUSE_OPT_KEY_OPT:
        return 1;
    case KEY_HELP:
        usage(outargs->argv[0]);
        exit(0);
    case KEY_VERSION:
        fprintf(stdout, "ROFS version %s\n", rofsVersion);
        exit(0);
    default:
        fprintf(stderr, "see `%s -h' for usage\n", outargs->argv[0]);
        exit(1);
    }
    return 1;
}

static struct fuse_opt rofs_opts[] = {
    FUSE_OPT_KEY("-h", KEY_HELP),
    FUSE_OPT_KEY("--help", KEY_HELP),
    FUSE_OPT_KEY("-V", KEY_VERSION),
    FUSE_OPT_KEY("--version", KEY_VERSION),
    FUSE_OPT_END};


void closedir_wrapper(void *dp) {
    if (dp != NULL) {
        closedir((DIR *)dp);
    }
}

void *thread_opendir_with_cleanup(void *arguments)
{
    arg_struct_opendir *args = (arg_struct_opendir *)arguments;
    args->dp = opendir(args->path);

    // Setup cleanup handler
    pthread_cleanup_push(closedir_wrapper, args->dp);

    if (args->dp == NULL)
    {
        args->res = errno;
    }
    else
    {
        args->res = 0;
    }

    // Remove cleanup handler
    // If non-zero param is passed, cleanup handler is executed
    pthread_cleanup_pop(1);

    return NULL;
}

void *check_if_filesystem_blocks(void *fsno)
{
    pthread_t thread_id;
    arg_struct_opendir args;
    long currfs = (long)fsno;

    args.path = Fss[currfs];

   
    struct timespec timeout;

    // Wait for the thread to complete with a timeout

   
    // Wait for the thread to complete with a timeout
    while (1)
    {
        // Create a new thread to open the directory
        int ret = pthread_create(&thread_id, NULL, thread_opendir_with_cleanup, &args);
        if ( ret != 0 ) {
            perror("pthread_create");
            exit(1);
        }
        clock_gettime(CLOCK_REALTIME, &timeout);
        // Set the timeout
        timeout.tv_sec += 2;
        if (pthread_timedjoin_np(thread_id, NULL, &timeout) != 0)
        {
            DEBUG("Call to opendir(%s) timed out\n", Fss[currfs]);
            insert_to_hash_table(FSOkMap, args.path, 0);

        }
        else
        {
            if ( args.res == 0 ) {
                insert_to_hash_table(FSOkMap, args.path, 1);
            } else {
                printf("Error thread_opendir: %s\n", strerror(args.res));
                insert_to_hash_table(FSOkMap, args.path, 0);
            }
        }
        pthread_testcancel(); // Cancellation point
        sleep(1);
    }
   
}


int main(int argc, char *argv[])
{

    struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
    int res;

    FSOkMap = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);

    res = fuse_opt_parse(&args, &Currfs, rofs_opts, rofs_parse_opt);
    if (res != 0)
    {
        fprintf(stderr, "Invalid arguments\n");
        fprintf(stderr, "see `%s -h' for usage\n", argv[0]);
        exit(1);
    }
    if (Currfs == 0)
    {
        fprintf(stderr, "Missing path\n");
        fprintf(stderr, "see `%s -h' for usage\n", argv[0]);
        exit(1);
    }
    Fss = split_string(argv[1], ",");
    Currfs = Fss[0];

    int i;
    for (i = 0; *(Fss + i); i++)
    {
        Fscount++;
    }

    // "Remove" first command line arg
    argc--;
    argv++;
    
    // monitor_fs();

    // Monitor file systems . Does it block ?
    int rc;
    long t;
    pthread_t threads[Fscount];
    for (t = 0; t < Fscount; t++)
    {
        rc = pthread_create(&threads[t], NULL, check_if_filesystem_blocks, (void *)t);
        if (rc)
        {
            printf("ERROR; return code from pthread_create() is %d\n", rc);
            exit(-1);
        }
    }

#if FUSE_VERSION >= 26
    fuse_main(args.argc, args.argv, &callback_oper, NULL);
#else
    fuse_main(args.argc, args.argv, &callback_oper);
#endif
    /* Now, wait for all threads to finish */

    for (t = 0; t < Fscount; t++)
    {
        rc = pthread_join(threads[t], NULL);
        if (rc)
        {
            printf("ERROR; return code from pthread_join() is %d\n", rc);
            exit(-1);
        }
    }

    pthread_exit(NULL);

    return 0;
}

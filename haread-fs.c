/*
 * HAREAD-FS  High Availability Read only file system
 *
 *
 * Initialized from https://github.com/libfuse/libfuse/wiki/Filesystems . Specially the rofs one
 * LICENCE : GPL v3
 */

 #define FUSE_USE_VERSION 26

 static const char *hareadFsVersion = "2024.08.20-fixed";
 
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
 #define DEBUG_ON 0
 
 #if DEBUG_ON
 #define DEBUG(fmt, ...) fprintf(stderr, "DEBUG: %s:%d:%s(): \n" fmt, \
                                 __FILE__, __LINE__, __func__, __VA_ARGS__)
 #else
 #define DEBUG(fmt, ...) /* Nothing */
 #endif
 
 int Fscount;
 char **Fss; // Underlying filesystems
 
 // Key: file system path. Values:  1 => fs Ok, 0 => fs Blocks, -1 => Key does not exist 
 GHashTable *FSOkMap = NULL;
 
 // Mutex for protecting global variables
 pthread_mutex_t global_mutex = PTHREAD_MUTEX_INITIALIZER;
 pthread_mutex_t hash_mutex = PTHREAD_MUTEX_INITIALIZER;
 
 static inline void LOG(const char *fmt, ...) 
 {
     time_t rawtime;
     struct tm * timeinfo;
     char buffer[80];
     
     time(&rawtime);
     timeinfo = gmtime(&rawtime);
     
     strftime(buffer,80,"%Y-%m-%d %H:%M:%S",timeinfo);
     printf("%s UTC: ", buffer);
     
     va_list args;
     va_start(args, fmt);
     vprintf(fmt, args);
     va_end(args);
     
     printf("\n");
     fflush(stdout);
 }
 
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
     if (!result)
     {
         return NULL;
     }
 
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
 static char *translate_path(const char *path, const char *fs_root)
 {
     if (!path || !fs_root)
     {
         return NULL;
     }
 
     size_t root_len = strlen(fs_root);
     size_t path_len = strlen(path);
     
     // Remove trailing slash if present
     int has_trailing_slash = (root_len > 0 && fs_root[root_len - 1] == '/') ? 1 : 0;
     size_t alloc_size = root_len + path_len + 1 - has_trailing_slash;
     
     char *rPath = malloc(alloc_size);
     if (!rPath)
     {
         return NULL;
     }
 
     strcpy(rPath, fs_root);
     if (has_trailing_slash)
     {
         rPath[root_len - 1] = '\0';
     }
     strcat(rPath, path);
 
     return rPath;
 }
 
 int retrieve_from_hash_table(GHashTable *hash_table, char *key)
 {
     pthread_mutex_lock(&hash_mutex);
     int result = -1;
     
     if (g_hash_table_contains(hash_table, key))
     {
         result = GPOINTER_TO_INT(g_hash_table_lookup(hash_table, key));
     }
     
     pthread_mutex_unlock(&hash_mutex);
     return result;
 }
 
 // Function to insert key-value pair into hash table
 void insert_to_hash_table(GHashTable *hash_table, char *key, int value)
 {
     pthread_mutex_lock(&hash_mutex);
     g_hash_table_insert(hash_table, g_strdup(key), GINT_TO_POINTER(value));
     pthread_mutex_unlock(&hash_mutex);
 }
 
 /******************************
  *
  * Callbacks for FUSE
  *
  ******************************/
 
 // The struct to pass arguments to the thread - allocated on heap
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
 
 #define MAX_THREADS 5
 #define MAX_FS 10
 #define OPERATION_TIMEOUT_SEC 5
 
 static int callback_getattr(const char *path, struct stat *st_data)
 {
     char *ipath = NULL;
     arg_struct_lstat *args = NULL;
     pthread_t thread_id = 0;
     int all_timed_out = 1;
     int result = -ENOENT;
     int timed_out_last_iteration[MAX_FS] = {0};
     
     for (int i = 0; i < Fscount && i < MAX_FS; i++)
     {
         int fs_status = retrieve_from_hash_table(FSOkMap, Fss[i]);
         if (fs_status != 1) // Only try if filesystem is explicitly marked as OK
         {
             continue;
         }
         
         ipath = translate_path(path, Fss[i]);
         if (!ipath)
         {
             continue;
         }
 
         // Allocate on heap to avoid use-after-scope
         args = malloc(sizeof(arg_struct_lstat));
         if (!args)
         {
             free(ipath);
             continue;
         }
 
         args->path = ipath;
         args->st_data = st_data;
         args->res = -1;
         args->errnum = 0;
 
         // Create a new thread to call lstat
         if (pthread_create(&thread_id, NULL, thread_lstat, args) != 0)
         {
             free(args);
             free(ipath);
             continue;
         }
 
         // Set the timeout
         struct timespec timeout;
         clock_gettime(CLOCK_REALTIME, &timeout);
         timeout.tv_sec += OPERATION_TIMEOUT_SEC;
 
         // Wait for the thread to complete with a timeout
         if (pthread_timedjoin_np(thread_id, NULL, &timeout) != 0)
         {
             // The call to lstat timed out
             LOG("callback_getattr: Timeout on %s\n", Fss[i]);
             timed_out_last_iteration[i]++;
             
             // Cancel the thread to prevent use-after-free
             pthread_cancel(thread_id);
             pthread_join(thread_id, NULL); // Wait for cancellation
             
             free(args);
             free(ipath);
             continue;
         } 
         
         if (timed_out_last_iteration[i]) {
             LOG("callback_getattr: %s back online after timed out %d times\n", Fss[i], timed_out_last_iteration[i]);
             timed_out_last_iteration[i] = 0;
         }
         
         all_timed_out = 0;
         
         if (args->res == 0)
         {
             result = 0;
             free(args);
             free(ipath);
             return result;
         }
         
         result = -args->errnum;
         free(args);
         free(ipath);
     }
 
     if (all_timed_out)
     {
         return -ETIMEDOUT;
     }
     
     return result;
 }
 
 static int callback_readlink(const char *path, char *buf, size_t size)
 {
     DEBUG("CALLLBACK_READLINK %s\n", path);
 
     int res;
     char *ipath;
     
     // Try all filesystems to find the symlink
     for (int i = 0; i < Fscount && i < MAX_FS; i++)
     {
         int fs_status = retrieve_from_hash_table(FSOkMap, Fss[i]);
         if (fs_status != 1)
         {
             continue;
         }
         
         ipath = translate_path(path, Fss[i]);
         if (!ipath)
         {
             continue;
         }
 
         res = readlink(ipath, buf, size - 1);
         free(ipath);
         
         if (res != -1)
         {
             buf[res] = '\0';
             return 0;
         }
         
         // Continue to next filesystem if ENOENT
         if (errno != ENOENT)
         {
             return -errno;
         }
     }
     
     return -ENOENT;
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
 
 int filldir(const char *path, void *buf, fuse_fill_dir_t filler, GHashTable *filesMap, const char *fs_root)
 {
     struct dirent *de;
     pthread_t thread_id = 0;
     arg_struct_opendir *args = NULL;
     char *ipath = NULL;
 
     ipath = translate_path(path, fs_root);
     if (!ipath)
     {
         return ENOMEM;
     }
 
     args = malloc(sizeof(arg_struct_opendir));
     if (!args)
     {
         free(ipath);
         return ENOMEM;
     }
 
     args->path = ipath;
     args->dp = NULL;
     args->res = -1;
 
     // Create a new thread to open the directory
     if (pthread_create(&thread_id, NULL, thread_opendir, args) != 0)
     {
         free(args);
         free(ipath);
         return errno;
     }
 
     // Set the timeout
     struct timespec timeout;
     clock_gettime(CLOCK_REALTIME, &timeout);
     timeout.tv_sec += OPERATION_TIMEOUT_SEC;
 
     // Wait for the thread to complete with a timeout
     if (pthread_timedjoin_np(thread_id, NULL, &timeout) != 0)
     {
         // The call to opendir timed out
         LOG("filldir(): Call to opendir(%s) timed out\n", ipath);
         
         pthread_cancel(thread_id);
         pthread_join(thread_id, NULL);
         
         free(args);
         free(ipath);
         return ETIMEDOUT;
     }
     
     free(ipath);
 
     if (args->dp == NULL)
     {
         int res = args->res;
         free(args);
         return res;
     }
 
     while ((de = readdir(args->dp)) != NULL)
     {
         struct stat st;
         memset(&st, 0, sizeof(st));
         st.st_ino = de->d_ino;
         st.st_mode = de->d_type << 12;
         
         pthread_mutex_lock(&hash_mutex);
         int contains = g_hash_table_contains(filesMap, de->d_name);
         pthread_mutex_unlock(&hash_mutex);
         
         if (contains)
         {
             continue;
         }
         
         if (filler(buf, de->d_name, &st, 0))
             break;
             
         pthread_mutex_lock(&hash_mutex);
         g_hash_table_insert(filesMap, g_strdup(de->d_name), GINT_TO_POINTER(1));
         pthread_mutex_unlock(&hash_mutex);
     }
 
     closedir(args->dp);
     free(args);
     
     return 0;
 }
 
 static int callback_readdir(const char *path, void *buf, fuse_fill_dir_t filler, off_t offset, struct fuse_file_info *fi)
 {
     (void)offset;
     (void)fi;
 
     GHashTable *filesMap = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
     if (!filesMap)
     {
         return -ENOMEM;
     }
 
     int success_count = 0;
     int last_error = ENOENT;
 
     // Loop through all filesystems
     for (int i = 0; i < Fscount && i < MAX_FS; i++)
     {
         int fs_status = retrieve_from_hash_table(FSOkMap, Fss[i]);
         
         if (fs_status != 1) // Fs not OK
         {
             continue;
         }
 
         int ret = filldir(path, buf, filler, filesMap, Fss[i]);
         
         if (ret == 0)
         {
             success_count++;
         }
         else if (ret != ENOENT && ret != ETIMEDOUT)
         {
             last_error = ret;
         }
     }
    
     g_hash_table_destroy(filesMap);
 
     if (success_count > 0)
     {
         return 0;
     }
 
     return -last_error;
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
 
     int all_timed_out = 1;
     int result = -ENOENT;
     
     for (int i = 0; i < Fscount && i < MAX_FS; i++)
     {
         char *ipath = translate_path(path, Fss[i]);
         if (!ipath)
         {
             continue;
         }
         
         arg_struct_open *args = malloc(sizeof(arg_struct_open));
         if (!args)
         {
             free(ipath);
             continue;
         }
         
         pthread_t thread_id;
         args->path = ipath;
         args->flags = flags;
         args->res = -1;
         args->errnum = 0;
         
         if (pthread_create(&thread_id, NULL, thread_open, args) != 0)
         {
             free(args);
             free(ipath);
             continue;
         }
 
         struct timespec timeout;
         clock_gettime(CLOCK_REALTIME, &timeout);
         timeout.tv_sec += OPERATION_TIMEOUT_SEC;
 
         if (pthread_timedjoin_np(thread_id, NULL, &timeout) != 0)
         {
             LOG("callback_open: open(%s) timed out. Trying next fs if any\n", ipath);
             
             pthread_cancel(thread_id);
             pthread_join(thread_id, NULL);
             
             free(args);
             free(ipath);
             continue;
         }
         
         all_timed_out = 0;
 
         if (args->res != -1)
         {
             close(args->res);
             result = 0;
             free(args);
             free(ipath);
             return result;
         }
         
         if (args->res == -1 && args->errnum == ENOENT)
         {
             free(args);
             free(ipath);
             continue; // Try next fs
         }
         else if (args->res == -1 && args->errnum != ENOENT)
         {
             result = -args->errnum;
             free(args);
             free(ipath);
             return result;
         }
         
         free(args);
         free(ipath);
     }
 
     if (all_timed_out)
     {
         return -ETIMEDOUT;
     }
     
     return result;
 }
 
 typedef struct arg_struct_read
 {
     char *path;
     int flags;
     int res;
     int errnum;
     char *buf;
     size_t size;
     off_t offset;
 } arg_struct_read;
 
 void *thread_read(void *arguments)
 {
     arg_struct_read *args = (arg_struct_read *)arguments;
     int fd = open(args->path, args->flags);
     if (fd == -1)
     {
         args->errnum = errno;
         args->res = -1;
         return NULL;
     }
 
     args->res = pread(fd, args->buf, args->size, args->offset); 
     if (args->res == -1)
     {
         args->errnum = errno;
     }
     
     close(fd);
     return NULL;
 }
 
 static int callback_read(const char *path, char *buf, size_t size, off_t offset, struct fuse_file_info *finfo)
 {
     (void)finfo;
     
     int result = -ENOENT;
     
     for (int i = 0; i < Fscount && i < MAX_FS; i++) 
     {
         int fs_status = retrieve_from_hash_table(FSOkMap, Fss[i]);
         if (fs_status != 1) // Only try if filesystem is explicitly marked as OK
         {
             continue;
         }
 
         char *ipath = translate_path(path, Fss[i]);
         if (!ipath)
         {
             continue;
         }
         
         arg_struct_read *args = malloc(sizeof(arg_struct_read));
         if (!args)
         {
             free(ipath);
             continue;
         }
         
         args->buf = buf;
         args->size = size;
         args->offset = offset;
         args->flags = O_RDONLY;
         args->path = ipath;
         args->res = -1;
         args->errnum = 0;
        
         pthread_t thread_id;
         if (pthread_create(&thread_id, NULL, thread_read, args) != 0)
         {
             free(args);
             free(ipath);
             continue;
         }
 
         struct timespec timeout;
         clock_gettime(CLOCK_REALTIME, &timeout);
         timeout.tv_sec += OPERATION_TIMEOUT_SEC;
 
         if (pthread_timedjoin_np(thread_id, NULL, &timeout) != 0) 
         {
             LOG("callback_read: read(%s) timed out. Trying next fs if any\n", ipath);
             
             pthread_cancel(thread_id);
             pthread_join(thread_id, NULL);
             
             free(args);
             free(ipath);
             continue;
         }
         
         if (args->res != -1)
         {
             result = args->res;
             free(args);
             free(ipath);
             return result;
         }
         
         if (args->res == -1 && args->errnum == ENOENT)
         {
             free(args);
             free(ipath);
             continue; 
         }
         else if (args->res == -1 && args->errnum != ENOENT)
         {
             result = -args->errnum;  // Fixed: was args.res
             free(args);
             free(ipath);
             return result;
         }
         
         free(args);
         free(ipath);
     }
     
     return result;
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
     
     ipath = translate_path(path, Fss[0]);
     if (!ipath)
     {
         return -ENOMEM;
     }
 
     res = statvfs(ipath, st_buf);  // Fixed: was statvfs(path, st_buf)
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
     
     // Read-only filesystem - reject write access requests immediately
     if (mode & W_OK)
     {
         return -EROFS;
     }
     
     // Try all filesystems to find the file
     for (int i = 0; i < Fscount && i < MAX_FS; i++)
     {
         int fs_status = retrieve_from_hash_table(FSOkMap, Fss[i]);
         if (fs_status != 1) // Only try if filesystem is explicitly marked as OK
         {
             continue;
         }
         
         ipath = translate_path(path, Fss[i]);
         if (!ipath)
         {
             continue;
         }
         
         DEBUG("CALLLBACK_ACCESS %s\n", ipath);
         
         res = access(ipath, mode);
         free(ipath);
         
         if (res == 0)
         {
             return 0; // Success - file exists and is accessible
         }
         // Continue to next filesystem if ENOENT
         if (res == -1 && errno != ENOENT)
         {
             return -errno; // Real error, not just "file not found"
         }
     }
     
     // File not found in any filesystem
     return -ENOENT;
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
     DEBUG("CALLLBACK_GETXATTR %s\n", path);
     int res;
     char *ipath;
 
     // Try all filesystems to find the file
     for (int i = 0; i < Fscount && i < MAX_FS; i++)
     {
         int fs_status = retrieve_from_hash_table(FSOkMap, Fss[i]);
         if (fs_status != 1)
         {
             continue;
         }
         
         ipath = translate_path(path, Fss[i]);
         if (!ipath)
         {
             continue;
         }
         
         res = lgetxattr(ipath, name, value, size);
         free(ipath);
         
         if (res != -1)
         {
             return res;
         }
         
         // Continue to next filesystem if ENOENT or ENODATA
         if (errno != ENOENT && errno != ENODATA)
         {
             return -errno;
         }
     }
     
     return -ENODATA; // Attribute not found in any filesystem
 }
 
 /*
  * List the supported extended attributes.
  */
 static int callback_listxattr(const char *path, char *list, size_t size)
 {
     DEBUG("CALLLBACK_LISTXATTR %s", "sd");
     int res;
     char *ipath;
 
     // Try all filesystems to find the file
     for (int i = 0; i < Fscount && i < MAX_FS; i++)
     {
         int fs_status = retrieve_from_hash_table(FSOkMap, Fss[i]);
         if (fs_status != 1)
         {
             continue;
         }
         
         ipath = translate_path(path, Fss[i]);
         if (!ipath)
         {
             continue;
         }
         
         res = llistxattr(ipath, list, size);
         free(ipath);
         
         if (res != -1)
         {
             return res;
         }
         
         // Continue to next filesystem if ENOENT
         if (errno != ENOENT)
         {
             return -errno;
         }
     }
     
     return -ENOENT;
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
             "usage: %s comma,separated,list,of,underlying-fss-paths mountpoint [options]\n"
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
 
 static int hareadfs_parse_opt(void *data, const char *arg, int key,
                           struct fuse_args *outargs)
 {
     char **fs_paths_ptr = (char **)data;
 
     switch (key)
     {
     case FUSE_OPT_KEY_NONOPT:
         if (*fs_paths_ptr == NULL)
         {
             *fs_paths_ptr = strdup(arg);
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
         fprintf(stdout, "hareadfs version %s\n", hareadFsVersion);
         exit(0);
     default:
         fprintf(stderr, "see `%s -h' for usage\n", outargs->argv[0]);
         exit(1);
     }
     return 1;
 }
 
 static struct fuse_opt hareadfs_opts[] = {
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
 
     if (args->dp == NULL)
     {
         args->res = errno;
     }
     else
     {
         // Setup cleanup handler only if opendir() was successful
         pthread_cleanup_push(closedir_wrapper, args->dp);
         args->res = 0;
 
         // Remove cleanup handler
         // If non-zero param is passed, cleanup handler is executed
         pthread_cleanup_pop(1);
     }
 
     return NULL;
 }
 
 void *check_if_filesystem_blocks(void *fsno)
 {
     pthread_t thread_ids[MAX_THREADS] = {0};
     arg_struct_opendir args[MAX_THREADS] = {0};
     int current_thread = 0;
     
     int timed_out_last_iteration[MAX_FS] = {0};
     long fs_index = (long)fsno;
     
     if (fs_index >= MAX_FS || fs_index >= Fscount)
     {
         LOG("check_if_filesystem_blocks: Invalid filesystem index %ld\n", fs_index);
         return NULL;
     }
     
     while (1)
     {
         pthread_testcancel(); // Cancellation point
 
         // Wait for the current thread to finish
         if (thread_ids[current_thread] != 0)
         {
             struct timespec timeout;
             clock_gettime(CLOCK_REALTIME, &timeout);
             timeout.tv_sec += 2;
 
             if (pthread_timedjoin_np(thread_ids[current_thread], NULL, &timeout) != 0)
             {
                 timed_out_last_iteration[fs_index]++;
                 LOG("Call to opendir(%s) timed out (%d times since last success)\n", 
                     Fss[fs_index], timed_out_last_iteration[fs_index]);
                     
                 pthread_cancel(thread_ids[current_thread]);
                 pthread_join(thread_ids[current_thread], NULL);
                 thread_ids[current_thread] = 0;
                 
                 insert_to_hash_table(FSOkMap, Fss[fs_index], 0);
             } 
             else 
             {
                 if (timed_out_last_iteration[fs_index]) 
                 {
                     LOG("check_if_filesystem_blocks: %s back online after timed out\n", Fss[fs_index]);
                     timed_out_last_iteration[fs_index] = 0;
                 }
                 
                 if (args[current_thread].res == 0) 
                 {
                     insert_to_hash_table(FSOkMap, Fss[fs_index], 1);
                 } 
                 else 
                 {
                     // Too many open files . But checking /proc/<pid>/fd/ only 4 file descriptors are used. So it something with
                     // dirs are nfs mounts (I believe). Anyways, seems to work and seems to hook up when nfs server finally comes back up 
                     if (EMFILE == args[current_thread].res) 
                     {
                         DEBUG("check_if_filesystem_blocks: Warning (Linux NFS client stuff? ) thread_opendir: %s\n", 
                               strerror(args[current_thread].res));
                     } 
                     else 
                     {
                         LOG("check_if_filesystem_blocks: Warning thread_opendir %s: %s\n", 
                             Fss[fs_index], strerror(args[current_thread].res));
                     }
                     insert_to_hash_table(FSOkMap, Fss[fs_index], 0);
                 }
             }
         }
 
         // Setup arguments for the new thread
         args[current_thread].path = Fss[fs_index];
         args[current_thread].dp = NULL;
         args[current_thread].res = -1;
 
         // Create a new thread to open the directory
         int ret = pthread_create(&thread_ids[current_thread], NULL, thread_opendir_with_cleanup, &args[current_thread]);
 
         if (ret != 0)
         {
             LOG("pthread_create failed: %s\n", strerror(ret));
             insert_to_hash_table(FSOkMap, Fss[fs_index], 0);
         }
 
         // Move to the next thread
         current_thread = (current_thread + 1) % MAX_THREADS;
 
         pthread_testcancel(); // Cancellation point
         sleep(1);
     }
 }
 
 int main(int argc, char *argv[])
 {
     struct fuse_args args = FUSE_ARGS_INIT(argc, argv);
     int res;
     char *fs_paths_arg = NULL;
 
     FSOkMap = g_hash_table_new_full(g_str_hash, g_str_equal, g_free, NULL);
     if (!FSOkMap)
     {
         fprintf(stderr, "Failed to create hash table\n");
         exit(1);
     }
 
     res = fuse_opt_parse(&args, &fs_paths_arg, hareadfs_opts, hareadfs_parse_opt);
     if (res != 0)
     {
         fprintf(stderr, "Invalid arguments\n");
         fprintf(stderr, "see `%s -h' for usage\n", argv[0]);
         exit(1);
     }
     
     if (fs_paths_arg == NULL)
     {
         fprintf(stderr, "Missing path\n");
         fprintf(stderr, "see `%s -h' for usage\n", argv[0]);
         exit(1);
     }
     
     Fss = split_string(fs_paths_arg, ",");
     if (!Fss)
     {
         fprintf(stderr, "Failed to parse filesystem paths\n");
         exit(1);
     }
 
     Fscount = 0;
     for (int i = 0; Fss[i] != NULL; i++)
     {
         Fscount++;
         if (Fscount >= MAX_FS)
         {
             LOG("Warning: Maximum of %d filesystems supported, ignoring rest\n", MAX_FS);
             break;
         }
     }
     
     if (Fscount == 0)
     {
         fprintf(stderr, "No filesystems specified\n");
         exit(1);
     }
 
     LOG("Initialized with %d filesystem(s)\n", Fscount);
     for (int i = 0; i < Fscount; i++)
     {
         LOG("  FS[%d]: %s\n", i, Fss[i]);
         // Initialize all filesystems as OK (status 1)
         // Monitoring threads will update status if they detect issues
         insert_to_hash_table(FSOkMap, Fss[i], 1);
     }
     
     // Monitor file systems . Does it block ?
     pthread_t threads[MAX_FS];
     for (long t = 0; t < Fscount; t++)
     {
         int rc = pthread_create(&threads[t], NULL, check_if_filesystem_blocks, (void *)t);
         if (rc)
         {
             LOG("ERROR; return code from pthread_create() is %d\n", rc);
             exit(-1);
         }
     }
 
 #if FUSE_VERSION >= 26
     fuse_main(args.argc, args.argv, &callback_oper, NULL);
 #else
     fuse_main(args.argc, args.argv, &callback_oper);
 #endif
 
     /* Cleanup on exit (typically never reached) */
     for (long t = 0; t < Fscount; t++)
     {
         pthread_cancel(threads[t]);
         pthread_join(threads[t], NULL);
     }
 
     if (FSOkMap)
     {
         g_hash_table_destroy(FSOkMap);
     }
     
     if (Fss)
     {
         for (int i = 0; Fss[i] != NULL; i++)
         {
             free(Fss[i]);
         }
         free(Fss);
     }
     
     if (fs_paths_arg)
     {
         free(fs_paths_arg);
     }
 
     pthread_mutex_destroy(&global_mutex);
     pthread_mutex_destroy(&hash_mutex);
 
     return 0;
 }
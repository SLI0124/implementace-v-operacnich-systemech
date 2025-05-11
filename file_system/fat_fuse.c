/*
  FUSE: Filesystem in Userspace
  FAT filesystem wrapper implementation
*/

#define FUSE_USE_VERSION 30

#include <fuse.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <stddef.h>
#include <assert.h>
#include <unistd.h>
#include <limits.h>  // For PATH_MAX

// Include your FAT implementation headers here
// #include "fat.h"

// Global variables
static const char *fat_img_path = NULL;
static int fat_img_fd = -1;

// Forward declarations for FAT operations
// These functions should be implemented in your FAT library
// static int fat_read_directory(const char *path, void *buffer, fuse_fill_dir_t filler);
// static int fat_get_attributes(const char *path, struct stat *stbuf);
// static int fat_open_file(const char *path, int flags);
// static int fat_read_file(const char *path, char *buf, size_t size, off_t offset);

static void *fat_fuse_init(struct fuse_conn_info *conn, struct fuse_config *cfg)
{
    (void) conn;
    cfg->kernel_cache = 1;
    
    printf("Opening FAT image: %s\n", fat_img_path);
    
    // Get current working directory for debugging
    char cwd[PATH_MAX];
    if (getcwd(cwd, sizeof(cwd)) != NULL) {
        printf("Current working directory: %s\n", cwd);
    }
    
    // Check if the file exists first
    if (access(fat_img_path, F_OK) == -1) {
        fprintf(stderr, "Error: File '%s' does not exist\n", fat_img_path);
        if (getcwd(cwd, sizeof(cwd)) != NULL) {
            fprintf(stderr, "Current working directory: %s\n", cwd);
        }
        exit(1);
    }
    
    // Open the FAT image file
    fat_img_fd = open(fat_img_path, O_RDONLY);
    if (fat_img_fd == -1) {
        perror("Error opening FAT image");
        exit(1);
    }
    
    // Initialize your FAT filesystem here
    // fat_init(fat_img_fd);
    
    return NULL;
}

static void fat_fuse_destroy(void *private_data)
{
    (void) private_data;
    
    // Clean up FAT resources
    // fat_cleanup();
    
    // Close the image file
    if (fat_img_fd != -1) {
        close(fat_img_fd);
    }
}

static int fat_fuse_getattr(const char *path, struct stat *stbuf,
                          struct fuse_file_info *fi)
{
    (void) fi;
    memset(stbuf, 0, sizeof(struct stat));
    
    // Root directory
    if (strcmp(path, "/") == 0) {
        stbuf->st_mode = S_IFDIR | 0755;
        stbuf->st_nlink = 2;
        return 0;
    }
    
    // Use your FAT implementation to get file attributes
    // return fat_get_attributes(path, stbuf);
    
    // Example implementation for testing
    if (strcmp(path, "/hello.txt") == 0) {
        stbuf->st_mode = S_IFREG | 0444;
        stbuf->st_nlink = 1;
        stbuf->st_size = 13;
        return 0;
    }
    
    return -ENOENT;
}

static int fat_fuse_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                          off_t offset, struct fuse_file_info *fi,
                          enum fuse_readdir_flags flags)
{
    (void) offset;
    (void) fi;
    (void) flags;
    
    // Check if this is the root directory
    if (strcmp(path, "/") != 0)
        return -ENOENT;
    
    // Add the standard directory entries
    filler(buf, ".", NULL, 0, 0);
    filler(buf, "..", NULL, 0, 0);
    
    // Use your FAT implementation to list directory contents
    // return fat_read_directory(path, buf, filler);
    
    // Example implementation for testing
    filler(buf, "hello.txt", NULL, 0, 0);
    
    return 0;
}

static int fat_fuse_open(const char *path, struct fuse_file_info *fi)
{
    // Check if the file exists
    // Use your FAT implementation to check file existence
    // return fat_open_file(path, fi->flags);
    
    // Example implementation for testing
    if (strcmp(path, "/hello.txt") != 0)
        return -ENOENT;
    
    // Only allow read-only access for now
    if ((fi->flags & O_ACCMODE) != O_RDONLY)
        return -EACCES;
    
    return 0;
}

static int fat_fuse_read(const char *path, char *buf, size_t size,
                       off_t offset, struct fuse_file_info *fi)
{
    (void) fi;
    
    // Use your FAT implementation to read file contents
    // return fat_read_file(path, buf, size, offset);
    
    // Example implementation for testing
    if (strcmp(path, "/hello.txt") != 0)
        return -ENOENT;
    
    const char *hello_str = "Hello, World!\n";
    size_t hello_len = strlen(hello_str);
    
    if (offset >= hello_len)
        return 0;
    
    if (offset + size > hello_len)
        size = hello_len - offset;
    
    memcpy(buf, hello_str + offset, size);
    
    return size;
}

// Remove unused structures and functions that were causing warnings
// Only keeping the essential FUSE operations

static const struct fuse_operations fat_fuse_oper = {
    .init       = fat_fuse_init,
    .destroy    = fat_fuse_destroy,
    .getattr    = fat_fuse_getattr,
    .readdir    = fat_fuse_readdir,
    .open       = fat_fuse_open,
    .read       = fat_fuse_read,
};

int main(int argc, char *argv[])
{
    // Need at least 3 arguments: program name, image path, mount point
    if (argc < 3) {
        fprintf(stderr, "Usage: %s [FUSE options] <fat_image> <mountpoint>\n", argv[0]);
        return 1;
    }
    
    // Find the position of the image path and mount point
    int img_pos = -1;
    int mount_pos = -1;
    
    // Assume the last two non-option arguments are image and mount point
    for (int i = argc - 1; i > 0; i--) {
        if (argv[i][0] != '-') {
            if (mount_pos == -1) {
                mount_pos = i;
            } else if (img_pos == -1) {
                img_pos = i;
                break;
            }
        }
    }
    
    if (img_pos == -1 || mount_pos == -1) {
        fprintf(stderr, "Missing image file or mount point\n");
        fprintf(stderr, "Usage: %s [FUSE options] <fat_image> <mountpoint>\n", argv[0]);
        return 1;
    }
    
    // Extract the FAT image path and convert to absolute path if needed
    char *image_path = argv[img_pos];
    char image_abs_path[PATH_MAX];
    
    // If the path is not already absolute, make it absolute
    if (image_path[0] != '/') {
        char cwd[PATH_MAX];
        if (getcwd(cwd, sizeof(cwd)) != NULL) {
            snprintf(image_abs_path, PATH_MAX, "%s/%s", cwd, image_path);
            image_path = image_abs_path;
        }
    } else if (strncmp(image_path, "/vojte/", 7) == 0) {
        // Fix paths that start with /vojte/ instead of /home/vojte/
        snprintf(image_abs_path, PATH_MAX, "/home%s", image_path);
        image_path = image_abs_path;
        printf("Corrected image path: %s\n", image_path);
    }
    
    // Check if the file exists before starting FUSE
    if (access(image_path, F_OK) == -1) {
        fprintf(stderr, "Error: Image file '%s' does not exist\n", image_path);
        char cwd[PATH_MAX];
        if (getcwd(cwd, sizeof(cwd)) != NULL) {
            fprintf(stderr, "Current working directory: %s\n", cwd);
        }
        fprintf(stderr, "Checking for file in file_system subdirectory...\n");
        
        // Try looking for the file in file_system subdirectory
        char alt_path[PATH_MAX];
        snprintf(alt_path, PATH_MAX, "file_system/%s", image_path);
        if (access(alt_path, F_OK) != -1) {
            fprintf(stderr, "Found image at %s, using this path instead\n", alt_path);
            image_path = strdup(alt_path);
        } else {
            fprintf(stderr, "Could not find image file in current directory or file_system subdirectory\n");
            return 1;
        }
    }
    
    // Store the resolved FAT image path
    fat_img_path = strdup(image_path);
    
    // Make sure the mount point exists
    if (access(argv[mount_pos], F_OK) == -1) {
        fprintf(stderr, "Error: Mount point '%s' does not exist\n", argv[mount_pos]);
        fprintf(stderr, "Please create it with: mkdir -p %s\n", argv[mount_pos]);
        free((void*)fat_img_path);
        return 1;
    }
    
    // Check if the mount point is accessible
    if (access(argv[mount_pos], R_OK | W_OK) == -1) {
        fprintf(stderr, "Error: No permission to access mount point '%s'\n", argv[mount_pos]);
        fprintf(stderr, "Please check permissions or try running with sudo\n");
        free((void*)fat_img_path);
        return 1;
    }
    
    // Remove trailing slash from mount point if present
    size_t len = strlen(argv[mount_pos]);
    if (len > 1 && argv[mount_pos][len-1] == '/') {
        argv[mount_pos][len-1] = '\0';
        printf("Removed trailing slash from mount point path\n");
    }
    
    // Use absolute path for the mount point
    char mount_abs_path[PATH_MAX];
    if (argv[mount_pos][0] != '/') {
        char cwd[PATH_MAX];
        if (getcwd(cwd, sizeof(cwd)) != NULL) {
            snprintf(mount_abs_path, PATH_MAX, "%s/%s", cwd, argv[mount_pos]);
            // Replace the relative mount point with absolute path
            argv[mount_pos] = mount_abs_path;
            printf("Absolute mount point path: %s\n", mount_abs_path);
        }
    } else {
        // Check if the path starts with "/vojte/" instead of "/home/vojte/"
        if (strncmp(argv[mount_pos], "/vojte/", 7) == 0) {
            snprintf(mount_abs_path, PATH_MAX, "/home%s", argv[mount_pos]);
            argv[mount_pos] = mount_abs_path;
            printf("Corrected absolute mount point path: %s\n", mount_abs_path);
        }
    }
    
    // Create a new args structure for FUSE that excludes the image path
    struct fuse_args args = FUSE_ARGS_INIT(0, NULL);
    
    // Add program name
    fuse_opt_add_arg(&args, argv[0]);
    
    // Copy all arguments except the image path
    for (int i = 1; i < argc; i++) {
        if (i != img_pos) {
            fuse_opt_add_arg(&args, argv[i]);
        }
    }
    
    printf("Using FAT image: %s\n", fat_img_path);
    printf("Mount point: %s\n", argv[mount_pos]);
    
    // Run FUSE main with the modified arguments
    int ret = fuse_main(args.argc, args.argv, &fat_fuse_oper, NULL);
    
    if (ret != 0) {
        fprintf(stderr, "fuse_main failed with error code: %d\n", ret);
        fprintf(stderr, "Try running with sudo if it's a permission issue\n");
    }
    
    // Clean up
    free((void*)fat_img_path);
    fuse_opt_free_args(&args);
    
    return ret;
}
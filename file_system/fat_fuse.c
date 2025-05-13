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

#include "fat.h"

// Global variables
static const char *fat_img_path = NULL;

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
    
    // Initialize the FAT filesystem
    if (init_file_system(fat_img_path) != 0) {
        fprintf(stderr, "Failed to initialize FAT filesystem\n");
        exit(1);
    }
    
    return NULL;
}

static void fat_fuse_destroy(void *private_data)
{
    (void) private_data;
    
    // Clean up FAT resources
    if (fs.fp) {
        fclose(fs.fp);
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
    
    // Save current directory
    uint16_t saved_cluster = current_dir_cluster;
    char saved_path[256];
    strcpy(saved_path, current_path);
    
    // Handle paths with directories
    char path_copy[PATH_MAX];
    strncpy(path_copy, path, PATH_MAX-1);
    path_copy[PATH_MAX-1] = '\0';
    
    // Extract directory path and filename
    char *filename = path_copy;
    char *last_slash = strrchr(path_copy, '/');
    
    if (last_slash != NULL) {
        // Split path into directory and filename parts
        *last_slash = '\0';
        filename = last_slash + 1;
        
        // If path is just "/", we're looking in root
        if (strlen(path_copy) == 0) {
            // Already at root, do nothing
        } else {
            // Change to specified directory
            if (change_dir(path_copy + 1) != 0) {
                // Restore original directory
                current_dir_cluster = saved_cluster;
                strcpy(current_path, saved_path);
                return -ENOENT;
            }
        }
    }
    
    // Now search for the file/directory in current directory
    uint32_t entry_count;
    Fat16Entry* entries = read_directory(current_dir_cluster, &entry_count);
    if (!entries) {
        // Restore original directory
        current_dir_cluster = saved_cluster;
        strcpy(current_path, saved_path);
        return -ENOENT;
    }
    
    // Find the entry
    Fat16Entry* entry = find_entry_by_name(entries, entry_count, filename);
    if (!entry) {
        free(entries);
        // Restore original directory
        current_dir_cluster = saved_cluster;
        strcpy(current_path, saved_path);
        return -ENOENT;
    }
    
    // Set file attributes based on the directory entry
    if (entry->attributes & 0x10) {
        // It's a directory
        stbuf->st_mode = S_IFDIR | 0755;
        stbuf->st_nlink = 2;
    } else {
        // It's a regular file
        stbuf->st_mode = S_IFREG | 0444;
        stbuf->st_nlink = 1;
        stbuf->st_size = entry->file_size;
    }
    
    // Set timestamps
    time_t modified_time = fat_date_time_to_unix(entry->modify_date, entry->modify_time);
    stbuf->st_mtime = modified_time;
    stbuf->st_atime = modified_time;
    stbuf->st_ctime = modified_time;
    
    free(entries);
    
    // Restore original directory
    current_dir_cluster = saved_cluster;
    strcpy(current_path, saved_path);
    
    return 0;
}

static int fat_fuse_readdir(const char *path, void *buf, fuse_fill_dir_t filler,
                          off_t offset, struct fuse_file_info *fi,
                          enum fuse_readdir_flags flags)
{
    (void) offset;
    (void) fi;
    (void) flags;
    
    // Save current directory
    uint16_t saved_cluster = current_dir_cluster;
    char saved_path[256];
    strcpy(saved_path, current_path);
    
    // Change to the requested directory
    if (strcmp(path, "/") != 0) {
        // Remove leading slash for change_dir
        if (change_dir(path + 1) != 0) {
            // Restore original directory
            current_dir_cluster = saved_cluster;
            strcpy(current_path, saved_path);
            return -ENOENT;
        }
    } else {
        // Explicitly go to root directory
        current_dir_cluster = 0;
        strcpy(current_path, "/");
    }
    
    // Add the standard directory entries
    filler(buf, ".", NULL, 0, 0);
    filler(buf, "..", NULL, 0, 0);
    
    // Read directory entries
    uint32_t entry_count;
    Fat16Entry* entries = read_directory(current_dir_cluster, &entry_count);
    if (!entries) {
        // Restore original directory
        current_dir_cluster = saved_cluster;
        strcpy(current_path, saved_path);
        return -ENOENT;
    }
    
    // Iterate through all directory entries
    for (uint32_t i = 0; i < entry_count; i++) {
        // Skip deleted or empty entries
        if (entries[i].filename[0] == 0x00 || entries[i].filename[0] == 0xE5) {
            continue;
        }
        
        // Skip the "." and ".." entries as we've already added them
        if (entries[i].filename[0] == '.' && 
            (entries[i].filename[1] == ' ' || 
             (entries[i].filename[1] == '.' && entries[i].filename[2] == ' '))) {
            continue;
        }
        
        // Get a clean filename
        char name_buf[13];
        clean_fat_name(name_buf, entries[i].filename, entries[i].ext);
        
        // Add this entry to the result
        if (filler(buf, name_buf, NULL, 0, 0)) {
            break; // Buffer full
        }
    }
    
    free(entries);
    
    // Restore original directory
    current_dir_cluster = saved_cluster;
    strcpy(current_path, saved_path);
    
    return 0;
}

static int fat_fuse_open(const char *path, struct fuse_file_info *fi)
{
    // Save current directory
    uint16_t saved_cluster = current_dir_cluster;
    char saved_path[256];
    strcpy(saved_path, current_path);
    
    // Handle paths with directories
    char path_copy[PATH_MAX];
    strncpy(path_copy, path, PATH_MAX-1);
    path_copy[PATH_MAX-1] = '\0';
    
    // Extract directory path and filename
    char *filename = path_copy;
    char *last_slash = strrchr(path_copy, '/');
    
    if (last_slash != NULL) {
        // Split path into directory and filename parts
        *last_slash = '\0';
        filename = last_slash + 1;
        
        // If path is just "/", we're looking in root
        if (strlen(path_copy) == 0) {
            // Already at root, do nothing
        } else {
            // Change to specified directory
            if (change_dir(path_copy + 1) != 0) {
                // Restore original directory
                current_dir_cluster = saved_cluster;
                strcpy(current_path, saved_path);
                return -ENOENT;
            }
        }
    }
    
    // Now search for the file in current directory
    uint32_t entry_count;
    Fat16Entry* entries = read_directory(current_dir_cluster, &entry_count);
    if (!entries) {
        // Restore original directory
        current_dir_cluster = saved_cluster;
        strcpy(current_path, saved_path);
        return -ENOENT;
    }
    
    // Find the entry
    Fat16Entry* entry = find_entry_by_name(entries, entry_count, filename);
    if (!entry || (entry->attributes & 0x10)) { // If not found or is a directory
        free(entries);
        // Restore original directory
        current_dir_cluster = saved_cluster;
        strcpy(current_path, saved_path);
        return -ENOENT;
    }
    
    free(entries);
    
    // Restore original directory
    current_dir_cluster = saved_cluster;
    strcpy(current_path, saved_path);
    
    // For now, only allow read-only access
    if ((fi->flags & O_ACCMODE) != O_RDONLY)
        return -EACCES;
    
    return 0;
}

static int fat_fuse_read(const char *path, char *buf, size_t size,
                       off_t offset, struct fuse_file_info *fi)
{
    (void) fi;
    
    // Save current directory
    uint16_t saved_cluster = current_dir_cluster;
    char saved_path[256];
    strcpy(saved_path, current_path);
    
    // Handle paths with directories
    char path_copy[PATH_MAX];
    strncpy(path_copy, path, PATH_MAX-1);
    path_copy[PATH_MAX-1] = '\0';
    
    // Extract directory path and filename
    char *filename = path_copy;
    char *last_slash = strrchr(path_copy, '/');
    
    if (last_slash != NULL) {
        // Split path into directory and filename parts
        *last_slash = '\0';
        filename = last_slash + 1;
        
        // If path is just "/", we're looking in root
        if (strlen(path_copy) == 0) {
            // Already at root, do nothing
        } else {
            // Change to specified directory
            if (change_dir(path_copy + 1) != 0) {
                // Restore original directory
                current_dir_cluster = saved_cluster;
                strcpy(current_path, saved_path);
                return -ENOENT;
            }
        }
    }
    
    // Find the file in current directory
    uint32_t entry_count;
    Fat16Entry* entries = read_directory(current_dir_cluster, &entry_count);
    if (!entries) {
        // Restore original directory
        current_dir_cluster = saved_cluster;
        strcpy(current_path, saved_path);
        return -ENOENT;
    }
    
    Fat16Entry* entry = find_entry_by_name(entries, entry_count, filename);
    if (!entry || (entry->attributes & 0x10)) { // If not found or is a directory
        free(entries);
        // Restore original directory
        current_dir_cluster = saved_cluster;
        strcpy(current_path, saved_path);
        return -ENOENT;
    }
    
    // Check if offset is beyond file size
    if (offset >= entry->file_size) {
        free(entries);
        // Restore original directory
        current_dir_cluster = saved_cluster;
        strcpy(current_path, saved_path);
        return 0;
    }
    
    // Adjust size if reading beyond end of file
    if (offset + size > entry->file_size) {
        size = entry->file_size - offset;
    }
    
    // Allocate buffer for the whole file
    uint8_t* file_buffer = malloc(entry->file_size);
    if (!file_buffer) {
        free(entries);
        // Restore original directory
        current_dir_cluster = saved_cluster;
        strcpy(current_path, saved_path);
        return -ENOMEM;
    }
    
    // Read file clusters
    uint16_t cluster = entry->starting_cluster;
    uint32_t bytes_read = 0;
    uint32_t cluster_size = fs.bs.sectors_per_cluster * fs.bs.sector_size;
    
    while (cluster >= 0x0002 && cluster < 0xFFF8 && bytes_read < entry->file_size) {
        // Calculate cluster position
        uint32_t cluster_offset = fs.data_area_offset + (cluster - 2) * cluster_size;
        
        // Calculate how much to read from this cluster
        uint32_t to_read = (entry->file_size - bytes_read) > cluster_size 
                           ? cluster_size 
                           : (entry->file_size - bytes_read);
        
        // Read the cluster
        fseek(fs.fp, cluster_offset, SEEK_SET);
        fread(file_buffer + bytes_read, to_read, 1, fs.fp);
        
        bytes_read += to_read;
        
        // Get next cluster
        cluster = get_fat_entry(cluster);
    }
    
    // Copy requested portion to the output buffer
    memcpy(buf, file_buffer + offset, size);
    
    free(file_buffer);
    free(entries);
    
    // Restore original directory
    current_dir_cluster = saved_cluster;
    strcpy(current_path, saved_path);
    
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
            if (snprintf(image_abs_path, PATH_MAX, "%s/%s", cwd, image_path) >= PATH_MAX) {
                fprintf(stderr, "Error: Image path is too long\n");
                return 1;
            }
            image_path = image_abs_path;
        }
    } else if (strncmp(image_path, "/vojte/", 7) == 0) {
        // Fix paths that start with /vojte/ instead of /home/vojte/
        if (snprintf(image_abs_path, PATH_MAX, "/home%s", image_path) >= PATH_MAX) {
            fprintf(stderr, "Error: Corrected image path is too long\n");
            return 1;
        }
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
        if (snprintf(alt_path, PATH_MAX, "file_system/%s", image_path) >= PATH_MAX) {
            fprintf(stderr, "Error: Alternate image path is too long\n");
            return 1;
        }
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
            if (snprintf(mount_abs_path, PATH_MAX, "%s/%s", cwd, argv[mount_pos]) >= PATH_MAX) {
                fprintf(stderr, "Error: Mount point path is too long\n");
                free((void*)fat_img_path);
                return 1;
            }
            // Replace the relative mount point with absolute path
            argv[mount_pos] = mount_abs_path;
            printf("Absolute mount point path: %s\n", mount_abs_path);
        }
    } else {
        // Check if the path starts with "/vojte/" instead of "/home/vojte/"
        if (strncmp(argv[mount_pos], "/vojte/", 7) == 0) {
            if (snprintf(mount_abs_path, PATH_MAX, "/home%s", argv[mount_pos]) >= PATH_MAX) {
                fprintf(stderr, "Error: Corrected mount point path is too long\n");
                free((void*)fat_img_path);
                return 1;
            }
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
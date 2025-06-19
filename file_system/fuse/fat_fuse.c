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
#include <limits.h>
#include <time.h>

#include "fat.h"

// Global variables
static const char *fat_img_path = NULL;

// Forward declarations for helper functions
static int is_binary_file(const char* filename);
static int is_animated_gif(const char* filename);

static void *fat_fuse_init(struct fuse_conn_info *conn, struct fuse_config *cfg)
{
    (void) conn;
    cfg->kernel_cache = 1;
    
    printf("Opening FAT image: %s\n", fat_img_path);
    
    // Verify that the FAT image exists
    if (access(fat_img_path, F_OK) == -1) {
        char cwd[PATH_MAX];
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
    
    // Clean up FAT resources when unmounting
    if (fs.fp) {
        fclose(fs.fp);
    }
}

static int fat_fuse_getattr(const char *path, struct stat *stbuf,
                          struct fuse_file_info *fi)
{
    (void) fi;
    memset(stbuf, 0, sizeof(struct stat));
    
    // Handle root directory specially
    if (strcmp(path, "/") == 0) {
        stbuf->st_mode = S_IFDIR | 0755;
        stbuf->st_nlink = 2;
        return 0;
    }
    
    // Save current directory state to restore later
    uint16_t saved_cluster = current_dir_cluster;
    char saved_path[256];
    strcpy(saved_path, current_path);
    
    // Make a copy of the path for manipulation
    char path_copy[PATH_MAX];
    strncpy(path_copy, path, PATH_MAX-1);
    path_copy[PATH_MAX-1] = '\0';
    
    // Split path into directory and filename parts
    char *filename = path_copy;
    char *last_slash = strrchr(path_copy, '/');
    
    if (last_slash != NULL) {
        *last_slash = '\0';
        filename = last_slash + 1;
        
        // Handle parent directory navigation
        if (strlen(path_copy) == 0) {
            // Root directory, no navigation needed
        } else {
            // Navigate to the specified directory
            if (change_dir(path_copy + 1) != 0) {
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
        
        // Special handling for binary files
        if (is_binary_file(filename)) {
            stbuf->st_mode = S_IFREG | 0444;
            
            // For animated GIFs, set a hint for viewers
            if (is_animated_gif(filename)) {
                stbuf->st_mode |= 0111;  // Add executable bit as a hint for some viewers
            }
        }
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
    
    // Save current directory state to restore later
    uint16_t saved_cluster = current_dir_cluster;
    char saved_path[256];
    strcpy(saved_path, current_path);
    
    // Change to the requested directory
    if (strcmp(path, "/") != 0) {
        if (change_dir(path + 1) != 0) {
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
    // Save current directory state to restore later
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
    
    // Allow any type of read access, including direct_io for binary files
    if ((fi->flags & O_ACCMODE) == O_RDONLY) {
        // Enable direct I/O for multimedia files for better streaming
        if (strstr(filename, ".gif") || strstr(filename, ".GIF") ||
            strstr(filename, ".jpg") || strstr(filename, ".JPG") ||
            strstr(filename, ".png") || strstr(filename, ".PNG") ||
            strstr(filename, ".bmp") || strstr(filename, ".BMP") ||
            strstr(filename, ".mp3") || strstr(filename, ".MP3") ||
            strstr(filename, ".mp4") || strstr(filename, ".MP4")) {
            fi->direct_io = 1;
        }
        return 0;
    }
    
    // For write operations, check if write support is enabled
    if ((fi->flags & O_ACCMODE) != O_RDONLY && 
        ((fi->flags & O_ACCMODE) == O_WRONLY || (fi->flags & O_ACCMODE) == O_RDWR)) {
        return 0;  // Allow write operations
    }
    
    return -EACCES;
}

static int fat_fuse_read(const char *path, char *buf, size_t size,
                       off_t offset, struct fuse_file_info *fi)
{
    (void) fi;
    
    // Save current directory state to restore later
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
        size_t bytes_actually_read = fread(file_buffer + bytes_read, 1, to_read, fs.fp);
        
        // Verify if the read was successful
        if (bytes_actually_read != to_read) {
            fprintf(stderr, "Warning: Read %zu bytes instead of %u from cluster %u\n", 
                    bytes_actually_read, to_read, cluster);
            
            // Still count what we were able to read
            bytes_read += bytes_actually_read;
            break;
        }
        
        bytes_read += to_read;
        
        // Get next cluster
        uint16_t next_cluster = get_fat_entry(cluster);
        
        // Detect cluster loops (corrupted FAT chain)
        if (next_cluster == cluster) {
            fprintf(stderr, "Warning: FAT chain loop detected at cluster %u\n", cluster);
            break;
        }
        
        cluster = next_cluster;
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

// Creates a new file in the FAT filesystem
static int fat_fuse_create(const char *path, mode_t mode, struct fuse_file_info *fi)
{
    (void) fi;
    (void) mode;
    
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
    
    // Check if file already exists
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
    if (entry) {
        // File already exists
        free(entries);
        // Restore original directory
        current_dir_cluster = saved_cluster;
        strcpy(current_path, saved_path);
        return -EEXIST;
    }
    
    free(entries);
    
    // Find a free directory entry
    entries = read_directory(current_dir_cluster, &entry_count);
    int free_entry_idx = -1;
    for (uint32_t i = 0; i < entry_count; i++) {
        if (entries[i].filename[0] == 0x00 || entries[i].filename[0] == 0xE5) {
            free_entry_idx = i;
            break;
        }
    }
    
    if (free_entry_idx == -1) {
        // No free entry in directory
        free(entries);
        // Restore original directory
        current_dir_cluster = saved_cluster;
        strcpy(current_path, saved_path);
        return -ENOSPC;
    }
    
    // Find a free cluster
    uint16_t total_clusters = (fs.bs.total_sectors_short ? fs.bs.total_sectors_short : fs.bs.total_sectors_int) 
                             / fs.bs.sectors_per_cluster;
    uint16_t first_free_cluster = 0;
    for (uint16_t c = 2; c < total_clusters; c++) {
        if (get_fat_entry(c) == 0x0000) {
            first_free_cluster = c;
            break;
        }
    }
    
    if (first_free_cluster == 0) {
        // No free cluster
        free(entries);
        // Restore original directory
        current_dir_cluster = saved_cluster;
        strcpy(current_path, saved_path);
        return -ENOSPC;
    }
    
    // Create new entry
    Fat16Entry new_entry = {0};
    char fname[9], ext[4];
    format_to_fat_name(filename, fname, ext);
    memcpy(new_entry.filename, fname, 8);
    memcpy(new_entry.ext, ext, 3);
    new_entry.attributes = 0x20; // Archive
    new_entry.starting_cluster = first_free_cluster;
    new_entry.file_size = 0;
    
    // Set date and time
    time_t now = time(NULL);
    struct tm *tm_now = localtime(&now);
    new_entry.modify_date = ((tm_now->tm_year - 80) << 9) | ((tm_now->tm_mon + 1) << 5) | tm_now->tm_mday;
    new_entry.modify_time = (tm_now->tm_hour << 11) | (tm_now->tm_min << 5) | (tm_now->tm_sec / 2);
    
    // Write entry to directory
    uint32_t dir_offset;
    if (current_dir_cluster == 0) {
        dir_offset = fs.root_dir_offset + free_entry_idx * sizeof(Fat16Entry);
    } else {
        dir_offset = fs.data_area_offset + (current_dir_cluster - 2) * fs.bs.sectors_per_cluster * fs.bs.sector_size 
                     + free_entry_idx * sizeof(Fat16Entry);
    }
    
    fseek(fs.fp, dir_offset, SEEK_SET);
    fwrite(&new_entry, sizeof(Fat16Entry), 1, fs.fp);
    fflush(fs.fp);
    
    // Mark end of cluster chain in FAT
    uint32_t fat_offset = first_free_cluster * 2;
    uint32_t fat_sector = fs.bs.reserved_sectors + (fat_offset / fs.bs.sector_size);
    uint32_t entry_offset = fat_offset % fs.bs.sector_size;
    
    for (int f = 0; f < fs.bs.number_of_fats; f++) {
        fseek(fs.fp, (fat_sector * fs.bs.sector_size) + fs.pt[0].start_sector * 512 
              + entry_offset + f * fs.bs.fat_size_sectors * fs.bs.sector_size, SEEK_SET);
        uint16_t val = 0xFFFF;  // End of chain
        fwrite(&val, 2, 1, fs.fp);
        fflush(fs.fp);
    }
    
    free(entries);
    
    // Restore original directory
    current_dir_cluster = saved_cluster;
    strcpy(current_path, saved_path);
    
    return 0;
}

// Write data to a file
static int fat_fuse_write(const char *path, const char *buf, size_t size,
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
    
    // Save entry's directory index for later
    uint32_t entry_idx = entry - entries;
    
    // Get current file size
    uint32_t current_file_size = entry->file_size;
    uint32_t new_file_size = offset + size > current_file_size ? offset + size : current_file_size;
    
    // Allocate a buffer for the entire file
    uint8_t* file_buffer = NULL;
    
    if (offset > 0 || offset < current_file_size) {
        // If we're not overwriting the whole file, read the existing content first
        file_buffer = malloc(new_file_size);
        if (!file_buffer) {
            free(entries);
            // Restore original directory
            current_dir_cluster = saved_cluster;
            strcpy(current_path, saved_path);
            return -ENOMEM;
        }
        
        // Initialize buffer with zeros
        memset(file_buffer, 0, new_file_size);
        
        // Read existing file content
        uint16_t cluster = entry->starting_cluster;
        uint32_t bytes_read = 0;
        uint32_t cluster_size = fs.bs.sectors_per_cluster * fs.bs.sector_size;
        
        while (cluster >= 0x0002 && cluster < 0xFFF8 && bytes_read < current_file_size) {
            // Calculate cluster position
            uint32_t cluster_offset = fs.data_area_offset + (cluster - 2) * cluster_size;
            
            // Calculate how much to read from this cluster
            uint32_t to_read = (current_file_size - bytes_read) > cluster_size 
                               ? cluster_size 
                               : (current_file_size - bytes_read);
            
            // Read the cluster
            fseek(fs.fp, cluster_offset, SEEK_SET);
            fread(file_buffer + bytes_read, to_read, 1, fs.fp);
            
            bytes_read += to_read;
            
            // Get next cluster
            cluster = get_fat_entry(cluster);
        }
    } else {
        // If overwriting the whole file or writing to empty file, just allocate needed size
        file_buffer = malloc(new_file_size);
        if (!file_buffer) {
            free(entries);
            // Restore original directory
            current_dir_cluster = saved_cluster;
            strcpy(current_path, saved_path);
            return -ENOMEM;
        }
        
        // Initialize buffer with zeros
        memset(file_buffer, 0, new_file_size);
    }
    
    // Copy new data to buffer at specified offset
    memcpy(file_buffer + offset, buf, size);
    
    // Calculate how many clusters we need
    uint32_t cluster_size = fs.bs.sectors_per_cluster * fs.bs.sector_size;
    uint32_t clusters_needed = (new_file_size + cluster_size - 1) / cluster_size;
    
    // Free existing clusters if file size changed
    if (new_file_size != current_file_size) {
        uint16_t cluster = entry->starting_cluster;
        while (cluster >= 0x0002 && cluster < 0xFFF8) {
            uint16_t next_cluster = get_fat_entry(cluster);
            
            // Free this cluster in all FAT copies
            for (int f = 0; f < fs.bs.number_of_fats; f++) {
                uint32_t fat_offset = cluster * 2;
                uint32_t fat_sector = fs.bs.reserved_sectors + (fat_offset / fs.bs.sector_size);
                uint32_t entry_offset = fat_offset % fs.bs.sector_size;
                
                fseek(fs.fp, (fat_sector * fs.bs.sector_size) + fs.pt[0].start_sector * 512 
                      + entry_offset + f * fs.bs.fat_size_sectors * fs.bs.sector_size, SEEK_SET);
                uint16_t val = 0x0000;  // Free cluster
                fwrite(&val, 2, 1, fs.fp);
                fflush(fs.fp);
            }
            
            cluster = next_cluster;
        }
        
        entry->starting_cluster = 0;
    }
    
    // Allocate new clusters
    uint16_t first_cluster = 0;
    uint16_t current_cluster = 0;
    uint16_t total_clusters = (fs.bs.total_sectors_short ? fs.bs.total_sectors_short : fs.bs.total_sectors_int) 
                             / fs.bs.sectors_per_cluster;
    
    for (uint32_t i = 0; i < clusters_needed; i++) {
        // Find a free cluster
        uint16_t new_cluster = 0;
        for (uint16_t c = 2; c < total_clusters; c++) {
            if (get_fat_entry(c) == 0x0000) {
                new_cluster = c;
                break;
            }
        }
        
        if (new_cluster == 0) {
            // No free cluster found
            free(file_buffer);
            free(entries);
            // Restore original directory
            current_dir_cluster = saved_cluster;
            strcpy(current_path, saved_path);
            return -ENOSPC;
        }
        
        // Link it to the previous cluster if needed
        if (current_cluster != 0) {
            // Update FAT entry for current cluster to point to the new one
            for (int f = 0; f < fs.bs.number_of_fats; f++) {
                uint32_t fat_offset = current_cluster * 2;
                uint32_t fat_sector = fs.bs.reserved_sectors + (fat_offset / fs.bs.sector_size);
                uint32_t entry_offset = fat_offset % fs.bs.sector_size;
                
                fseek(fs.fp, (fat_sector * fs.bs.sector_size) + fs.pt[0].start_sector * 512 
                      + entry_offset + f * fs.bs.fat_size_sectors * fs.bs.sector_size, SEEK_SET);
                fwrite(&new_cluster, 2, 1, fs.fp);
                fflush(fs.fp);
            }
        } else {
            // This is the first cluster
            first_cluster = new_cluster;
        }
        
        // Mark the new cluster as the end of the chain
        for (int f = 0; f < fs.bs.number_of_fats; f++) {
            uint32_t fat_offset = new_cluster * 2;
            uint32_t fat_sector = fs.bs.reserved_sectors + (fat_offset / fs.bs.sector_size);
            uint32_t entry_offset = fat_offset % fs.bs.sector_size;
            
            fseek(fs.fp, (fat_sector * fs.bs.sector_size) + fs.pt[0].start_sector * 512 
                  + entry_offset + f * fs.bs.fat_size_sectors * fs.bs.sector_size, SEEK_SET);
            uint16_t val = 0xFFFF;  // End of chain
            fwrite(&val, 2, 1, fs.fp);
            fflush(fs.fp);
        }
        
        // Write data to this cluster
        uint32_t cluster_offset = fs.data_area_offset + (new_cluster - 2) * cluster_size;
        uint32_t bytes_to_write = (i == clusters_needed - 1) 
                                ? (new_file_size - i * cluster_size) 
                                : cluster_size;
        
        fseek(fs.fp, cluster_offset, SEEK_SET);
        fwrite(file_buffer + (i * cluster_size), bytes_to_write, 1, fs.fp);
        fflush(fs.fp);
        
        current_cluster = new_cluster;
    }
    
    // Update directory entry
    entry->starting_cluster = first_cluster;
    entry->file_size = new_file_size;
    
    // Set date and time
    time_t now = time(NULL);
    struct tm *tm_now = localtime(&now);
    entry->modify_date = ((tm_now->tm_year - 80) << 9) | ((tm_now->tm_mon + 1) << 5) | tm_now->tm_mday;
    entry->modify_time = (tm_now->tm_hour << 11) | (tm_now->tm_min << 5) | (tm_now->tm_sec / 2);
    
    // Write updated directory entry
    uint32_t dir_offset;
    if (current_dir_cluster == 0) {
        dir_offset = fs.root_dir_offset + entry_idx * sizeof(Fat16Entry);
    } else {
        dir_offset = fs.data_area_offset + (current_dir_cluster - 2) * fs.bs.sectors_per_cluster * fs.bs.sector_size 
                     + entry_idx * sizeof(Fat16Entry);
    }
    
    fseek(fs.fp, dir_offset, SEEK_SET);
    fwrite(entry, sizeof(Fat16Entry), 1, fs.fp);
    fflush(fs.fp);
    
    free(file_buffer);
    free(entries);
    
    // Restore original directory
    current_dir_cluster = saved_cluster;
    strcpy(current_path, saved_path);
    
    return size;
}

// Creates a new directory in the FAT filesystem
static int fat_fuse_mkdir(const char *path, mode_t mode)
{
    (void) mode;
    
    // Save current directory state to restore later
    uint16_t saved_cluster = current_dir_cluster;
    char saved_path[256];
    strcpy(saved_path, current_path);
    
    // Handle paths with directories
    char path_copy[PATH_MAX];
    strncpy(path_copy, path, PATH_MAX-1);
    path_copy[PATH_MAX-1] = '\0';
    
    // Extract parent directory path and new directory name
    char *dirname = path_copy;
    char *last_slash = strrchr(path_copy, '/');
    
    if (last_slash != NULL) {
        // Split path into directory and dirname parts
        *last_slash = '\0';
        dirname = last_slash + 1;
        
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
    
    // Check if directory already exists
    uint32_t entry_count;
    Fat16Entry* entries = read_directory(current_dir_cluster, &entry_count);
    if (!entries) {
        // Restore original directory
        current_dir_cluster = saved_cluster;
        strcpy(current_path, saved_path);
        return -ENOENT;
    }
    
    // Find the entry
    Fat16Entry* entry = find_entry_by_name(entries, entry_count, dirname);
    if (entry) {
        // Directory already exists
        free(entries);
        // Restore original directory
        current_dir_cluster = saved_cluster;
        strcpy(current_path, saved_path);
        return -EEXIST;
    }
    
    free(entries);
    
    // Find a free directory entry
    entries = read_directory(current_dir_cluster, &entry_count);
    int free_entry_idx = -1;
    for (uint32_t i = 0; i < entry_count; i++) {
        if (entries[i].filename[0] == 0x00 || entries[i].filename[0] == 0xE5) {
            free_entry_idx = i;
            break;
        }
    }
    
    if (free_entry_idx == -1) {
        // No free entry in directory
        free(entries);
        // Restore original directory
        current_dir_cluster = saved_cluster;
        strcpy(current_path, saved_path);
        return -ENOSPC;
    }
    
    // Find a free cluster for the new directory
    uint16_t total_clusters = (fs.bs.total_sectors_short ? fs.bs.total_sectors_short : fs.bs.total_sectors_int) 
                             / fs.bs.sectors_per_cluster;
    uint16_t dir_cluster = 0;
    for (uint16_t c = 2; c < total_clusters; c++) {
        if (get_fat_entry(c) == 0x0000) {
            dir_cluster = c;
            break;
        }
    }
    
    if (dir_cluster == 0) {
        // No free cluster
        free(entries);
        // Restore original directory
        current_dir_cluster = saved_cluster;
        strcpy(current_path, saved_path);
        return -ENOSPC;
    }
    
    // Create new directory entry
    Fat16Entry new_entry = {0};
    char fname[9], ext[4];
    format_to_fat_name(dirname, fname, ext);
    memcpy(new_entry.filename, fname, 8);
    memcpy(new_entry.ext, ext, 3);
    new_entry.attributes = 0x10;  // Directory attribute
    new_entry.starting_cluster = dir_cluster;
    new_entry.file_size = 0;  // Directories don't use file_size
    
    // Set date and time
    time_t now = time(NULL);
    struct tm *tm_now = localtime(&now);
    new_entry.modify_date = ((tm_now->tm_year - 80) << 9) | ((tm_now->tm_mon + 1) << 5) | tm_now->tm_mday;
    new_entry.modify_time = (tm_now->tm_hour << 11) | (tm_now->tm_min << 5) | (tm_now->tm_sec / 2);
    
    // Write entry to parent directory
    uint32_t dir_offset;
    if (current_dir_cluster == 0) {
        dir_offset = fs.root_dir_offset + free_entry_idx * sizeof(Fat16Entry);
    } else {
        dir_offset = fs.data_area_offset + (current_dir_cluster - 2) * fs.bs.sectors_per_cluster * fs.bs.sector_size 
                     + free_entry_idx * sizeof(Fat16Entry);
    }
    
    fseek(fs.fp, dir_offset, SEEK_SET);
    fwrite(&new_entry, sizeof(Fat16Entry), 1, fs.fp);
    fflush(fs.fp);
    
    // Mark directory cluster as end of chain in FAT
    uint32_t fat_offset = dir_cluster * 2;
    uint32_t fat_sector = fs.bs.reserved_sectors + (fat_offset / fs.bs.sector_size);
    uint32_t entry_offset = fat_offset % fs.bs.sector_size;
    
    for (int f = 0; f < fs.bs.number_of_fats; f++) {
        fseek(fs.fp, (fat_sector * fs.bs.sector_size) + fs.pt[0].start_sector * 512 
              + entry_offset + f * fs.bs.fat_size_sectors * fs.bs.sector_size, SEEK_SET);
        uint16_t val = 0xFFFF;  // End of chain
        fwrite(&val, 2, 1, fs.fp);
        fflush(fs.fp);
    }
    
    // Initialize the directory with "." and ".." entries
    uint32_t cluster_offset = fs.data_area_offset + (dir_cluster - 2) * fs.bs.sectors_per_cluster * fs.bs.sector_size;
    
    // "." entry (points to itself)
    Fat16Entry dot_entry = {0};
    memcpy(dot_entry.filename, ".       ", 8);
    memcpy(dot_entry.ext, "   ", 3);
    dot_entry.attributes = 0x10;  // Directory
    dot_entry.starting_cluster = dir_cluster;
    dot_entry.modify_date = new_entry.modify_date;
    dot_entry.modify_time = new_entry.modify_time;
    
    fseek(fs.fp, cluster_offset, SEEK_SET);
    fwrite(&dot_entry, sizeof(Fat16Entry), 1, fs.fp);
    
    // ".." entry (points to parent)
    Fat16Entry dotdot_entry = {0};
    memcpy(dotdot_entry.filename, "..      ", 8);
    memcpy(dotdot_entry.ext, "   ", 3);
    dotdot_entry.attributes = 0x10;  // Directory
    dotdot_entry.starting_cluster = current_dir_cluster;  // Point to parent directory
    dotdot_entry.modify_date = new_entry.modify_date;
    dotdot_entry.modify_time = new_entry.modify_time;
    
    fwrite(&dotdot_entry, sizeof(Fat16Entry), 1, fs.fp);
    
    // Initialize the rest of the directory entries to 0
    uint32_t remaining_entries = (fs.bs.sectors_per_cluster * fs.bs.sector_size / sizeof(Fat16Entry)) - 2;
    Fat16Entry empty_entry = {0};
    for (uint32_t i = 0; i < remaining_entries; i++) {
        fwrite(&empty_entry, sizeof(Fat16Entry), 1, fs.fp);
    }
    fflush(fs.fp);
    
    free(entries);
    
    // Restore original directory
    current_dir_cluster = saved_cluster;
    strcpy(current_path, saved_path);
    
    return 0;
}

// Removes a directory
static int fat_fuse_rmdir(const char *path)
{
    // Save current directory state to restore later
    uint16_t saved_cluster = current_dir_cluster;
    char saved_path[256];
    strcpy(saved_path, current_path);
    
    // Handle paths with directories
    char path_copy[PATH_MAX];
    strncpy(path_copy, path, PATH_MAX-1);
    path_copy[PATH_MAX-1] = '\0';
    
    // Extract parent directory path and directory name
    char *dirname = path_copy;
    char *last_slash = strrchr(path_copy, '/');
    
    if (last_slash != NULL) {
        // Split path into directory and dirname parts
        *last_slash = '\0';
        dirname = last_slash + 1;
        
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
    
    // Cannot remove root directory
    if (strlen(dirname) == 0 || strcmp(dirname, ".") == 0 || strcmp(dirname, "..") == 0) {
        // Restore original directory
        current_dir_cluster = saved_cluster;
        strcpy(current_path, saved_path);
        return -EINVAL;
    }
    
    // Read parent directory entries
    uint32_t entry_count;
    Fat16Entry* entries = read_directory(current_dir_cluster, &entry_count);
    if (!entries) {
        // Restore original directory
        current_dir_cluster = saved_cluster;
        strcpy(current_path, saved_path);
        return -ENOENT;
    }
    
    // Find the directory entry
    Fat16Entry* entry = find_entry_by_name(entries, entry_count, dirname);
    if (!entry) {
        free(entries);
        // Restore original directory
        current_dir_cluster = saved_cluster;
        strcpy(current_path, saved_path);
        return -ENOENT;
    }
    
    // Make sure it's a directory
    if (!(entry->attributes & 0x10)) {
        free(entries);
        // Restore original directory
        current_dir_cluster = saved_cluster;
        strcpy(current_path, saved_path);
        return -ENOTDIR;
    }
    
    // Get the directory cluster
    uint16_t dir_cluster = entry->starting_cluster;
    if (dir_cluster == 0) {
        // Empty directory entry or invalid cluster
        free(entries);
        // Restore original directory
        current_dir_cluster = saved_cluster;
        strcpy(current_path, saved_path);
        return -EINVAL;
    }
    
    // Check if directory is empty (except for "." and "..")
    uint32_t dir_entry_count;
    Fat16Entry* dir_entries = read_directory(dir_cluster, &dir_entry_count);
    if (!dir_entries) {
        free(entries);
        // Restore original directory
        current_dir_cluster = saved_cluster;
        strcpy(current_path, saved_path);
        return -ENOENT;
    }
    
    for (uint32_t i = 0; i < dir_entry_count; i++) {
        if (dir_entries[i].filename[0] == 0x00 || dir_entries[i].filename[0] == 0xE5) {
            continue; // Skip deleted or empty entries
        }
        
        // Skip "." and ".." entries
        if (dir_entries[i].filename[0] == '.' && 
            (dir_entries[i].filename[1] == ' ' || 
             (dir_entries[i].filename[1] == '.' && dir_entries[i].filename[2] == ' '))) {
            continue;
        }
        
        // If we get here, the directory is not empty
        free(dir_entries);
        free(entries);
        // Restore original directory
        current_dir_cluster = saved_cluster;
        strcpy(current_path, saved_path);
        return -ENOTEMPTY;
    }
    
    free(dir_entries);
    
    // Save the directory entry index for later
    uint32_t entry_idx = entry - entries;
    
    // Free the directory cluster in the FAT
    uint16_t cluster = dir_cluster;
    while (cluster >= 0x0002 && cluster < 0xFFF8) {
        uint16_t next_cluster = get_fat_entry(cluster);
        
        // Free this cluster in all FAT copies
        for (int f = 0; f < fs.bs.number_of_fats; f++) {
            uint32_t fat_offset = cluster * 2;
            uint32_t fat_sector = fs.bs.reserved_sectors + (fat_offset / fs.bs.sector_size);
            uint32_t entry_offset = fat_offset % fs.bs.sector_size;
            
            fseek(fs.fp, (fat_sector * fs.bs.sector_size) + fs.pt[0].start_sector * 512 
                  + entry_offset + f * fs.bs.fat_size_sectors * fs.bs.sector_size, SEEK_SET);
            uint16_t val = 0x0000;  // Free cluster
            fwrite(&val, 2, 1, fs.fp);
            fflush(fs.fp);
        }
        
        cluster = next_cluster;
    }
    
    // Mark the directory entry as deleted
    uint32_t dir_offset;
    if (current_dir_cluster == 0) {
        dir_offset = fs.root_dir_offset + entry_idx * sizeof(Fat16Entry);
    } else {
        dir_offset = fs.data_area_offset + (current_dir_cluster - 2) * fs.bs.sectors_per_cluster * fs.bs.sector_size 
                     + entry_idx * sizeof(Fat16Entry);
    }
    
    fseek(fs.fp, dir_offset, SEEK_SET);
    unsigned char deleted = 0xE5;
    fwrite(&deleted, 1, 1, fs.fp);  // Write 0xE5 in the first byte of the filename
    fflush(fs.fp);
    
    free(entries);
    
    // Restore original directory
    current_dir_cluster = saved_cluster;
    strcpy(current_path, saved_path);
    
    return 0;
}

// Removes a file from the FAT filesystem
static int fat_fuse_unlink(const char *path)
{
    // Save current directory state to restore later
    uint16_t saved_cluster = current_dir_cluster;
    char saved_path[256];
    strcpy(saved_path, current_path);
    
    // Handle paths with directories
    char path_copy[PATH_MAX];
    strncpy(path_copy, path, PATH_MAX-1);
    path_copy[PATH_MAX-1] = '\0';
    
    // Split path into directory and filename parts
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
    
    // Read directory entries
    uint32_t entry_count;
    Fat16Entry* entries = read_directory(current_dir_cluster, &entry_count);
    if (!entries) {
        // Restore original directory
        current_dir_cluster = saved_cluster;
        strcpy(current_path, saved_path);
        return -ENOENT;
    }
    
    // Find the file entry
    Fat16Entry* entry = find_entry_by_name(entries, entry_count, filename);
    if (!entry) {
        free(entries);
        // Restore original directory
        current_dir_cluster = saved_cluster;
        strcpy(current_path, saved_path);
        return -ENOENT;
    }
    
    // Make sure it's not a directory
    if (entry->attributes & 0x10) {
        free(entries);
        // Restore original directory
        current_dir_cluster = saved_cluster;
        strcpy(current_path, saved_path);
        return -EISDIR;
    }
    
    // Save the file entry index for later
    uint32_t entry_idx = entry - entries;
    
    // Free all file clusters in the FAT
    uint16_t cluster = entry->starting_cluster;
    while (cluster >= 0x0002 && cluster < 0xFFF8) {
        uint16_t next_cluster = get_fat_entry(cluster);
        
        // Free this cluster in all FAT copies
        for (int f = 0; f < fs.bs.number_of_fats; f++) {
            uint32_t fat_offset = cluster * 2;
            uint32_t fat_sector = fs.bs.reserved_sectors + (fat_offset / fs.bs.sector_size);
            uint32_t entry_offset = fat_offset % fs.bs.sector_size;
            
            fseek(fs.fp, (fat_sector * fs.bs.sector_size) + fs.pt[0].start_sector * 512 
                  + entry_offset + f * fs.bs.fat_size_sectors * fs.bs.sector_size, SEEK_SET);
            uint16_t val = 0x0000;  // Free cluster
            fwrite(&val, 2, 1, fs.fp);
            fflush(fs.fp);
        }
        
        cluster = next_cluster;
    }
    
    // Mark the file entry as deleted
    uint32_t dir_offset;
    if (current_dir_cluster == 0) {
        dir_offset = fs.root_dir_offset + entry_idx * sizeof(Fat16Entry);
    } else {
        dir_offset = fs.data_area_offset + (current_dir_cluster - 2) * fs.bs.sectors_per_cluster * fs.bs.sector_size 
                     + entry_idx * sizeof(Fat16Entry);
    }
    
    fseek(fs.fp, dir_offset, SEEK_SET);
    unsigned char deleted = 0xE5;
    fwrite(&deleted, 1, 1, fs.fp);  // Write 0xE5 in the first byte of the filename
    fflush(fs.fp);
    
    free(entries);
    
    // Restore original directory
    current_dir_cluster = saved_cluster;
    strcpy(current_path, saved_path);
    
    return 0;
}

// Check if file has a binary/media file extension
static int is_binary_file(const char* filename) {
    static const char* binary_extensions[] = {
        ".gif", ".GIF", ".jpg", ".JPG", ".jpeg", ".JPEG",
        ".png", ".PNG", ".bmp", ".BMP", ".tif", ".TIF",
        ".mp3", ".MP3", ".mp4", ".MP4", ".avi", ".AVI",
        ".mov", ".MOV", ".zip", ".ZIP", ".exe", ".EXE",
        ".pdf", ".PDF", ".doc", ".DOC", ".xls", ".XLS",
        NULL
    };
    
    for (int i = 0; binary_extensions[i] != NULL; i++) {
        if (strstr(filename, binary_extensions[i])) {
            return 1;
        }
    }
    
    return 0;
}

// Check if file is an animated GIF based on extension
static int is_animated_gif(const char* filename) {
    return (strstr(filename, ".gif") || strstr(filename, ".GIF"));
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
    .create     = fat_fuse_create,
    .write      = fat_fuse_write,
    .mkdir      = fat_fuse_mkdir,
    .rmdir      = fat_fuse_rmdir,
    .unlink     = fat_fuse_unlink,
};

int main(int argc, char *argv[])
{
    // Require at least 3 arguments: program name, image path, mount point
    if (argc < 3) {
        fprintf(stderr, "Usage: %s [FUSE options] <fat_image> <mountpoint>\n", argv[0]);
        return 1;
    }
    
    // Identify the positions of image path and mount point arguments
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
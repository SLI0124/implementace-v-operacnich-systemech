#include "fat_file_ops.h"
#include "fat_utils.h"
#include "fat_dir_ops.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

// Read file and optionally save it to disk
void readFile(FILE* in, Fat16BootSector* bs, char* filename, PartitionTable* pt, int save_to_file) {
  Fat16Entry entry;
  int i, found = 0;
  uint16_t cluster;
  uint16_t fat_entry, bytes_to_read;
  uint8_t* buffer;
  uint32_t entry_count;
  Fat16Entry* dir_entries;
  FILE* output_file = NULL;
  char path_copy[256];
  uint16_t temp_dir_cluster = current_dir_cluster;
  
  // Check if we have a path with directory components
  strcpy(path_copy, filename);
  if (strchr(path_copy, '/')) {
    char* file_part;
    
    // Extract the directory part and the filename part
    file_part = strrchr(path_copy, '/');
    *file_part = '\0'; // Split the path at the last slash
    file_part++; // Move past the slash to get the filename
    
    // Save current directory
    uint16_t saved_cluster = current_dir_cluster;
    char saved_path[256];
    strcpy(saved_path, current_path);
    
    // Change to the directory containing the file
    if (changeDir(path_copy) != 0) {
      printf("Directory %s not found\n", path_copy);
      return;
    }
    
    // Read the file
    readFile(in, bs, file_part, pt, save_to_file);
    
    // Restore the original directory
    current_dir_cluster = saved_cluster;
    strcpy(current_path, saved_path);
    
    return;
  }
  
  // Read from current directory
  dir_entries = readDirectory(current_dir_cluster, &entry_count);
  if (!dir_entries) {
    printf("Error reading directory\n");
    return;
  }
  
  // Try to find the file
  Fat16Entry* found_entry = findEntryByName(dir_entries, entry_count, filename);
  if (found_entry && !(found_entry->attributes & 0x10)) {
    entry = *found_entry;
    found = 1;
  }
  
  free(dir_entries);
  
  if (!found) {
    printf("File %s not found in directory %s\n", filename, current_path);
    return;
  }
  
  // Allocate buffer for reading clusters
  buffer = (uint8_t*)malloc(bs->sectors_per_cluster * bs->sector_size);
  if (!buffer) {
    printf("Memory allocation error\n");
    return;
  }
  
  // If saving to file, create the output file
  if (save_to_file) {
    char output_filename[512]; 
    char clean_name[256];
    
    // Create a clean filename
    cleanFatName(clean_name, entry.filename, entry.ext);
    
    // Use the original user-provided filename if possible
    if (strchr(filename, '.')) {
      snprintf(output_filename, sizeof(output_filename), "%s", filename);
    } else {
      snprintf(output_filename, sizeof(output_filename), "%s", clean_name);
    }
    
    output_file = fopen(output_filename, "wb");
    if (!output_file) {
      fprintf(stderr, "Error creating output file %s\n", output_filename);
      free(buffer);
      return;
    }
  }
  
  // File found, read its contents cluster by cluster
  cluster = entry.starting_cluster;
  uint32_t remaining_size = entry.file_size;
  uint32_t data_start = (bs->reserved_sectors + bs->number_of_fats * bs->fat_size_sectors + 
      (bs->root_dir_entries * 32 + bs->sector_size - 1) / bs->sector_size) * bs->sector_size + 
      pt[0].start_sector * 512;
  
  // Read all clusters
  while (cluster >= 0x0002 && cluster < 0xFFF8 && remaining_size > 0) {
    // Calculate the absolute position of the cluster
    fseek(in, data_start + (cluster - 2) * bs->sectors_per_cluster * bs->sector_size, SEEK_SET);
    
    // Read the cluster
    bytes_to_read = (remaining_size > bs->sectors_per_cluster * bs->sector_size) ? 
                    bs->sectors_per_cluster * bs->sector_size : remaining_size;
    fread(buffer, bytes_to_read, 1, in);
    
    // Output the data
    if (save_to_file) {
      fwrite(buffer, bytes_to_read, 1, output_file);
    } else {
      fwrite(buffer, bytes_to_read, 1, stdout);
    }
    
    remaining_size -= bytes_to_read;
    
    // Get next cluster from FAT
    cluster = getFatEntry(cluster);
  }
  
  free(buffer);
  
  if (save_to_file && output_file) {
    fclose(output_file);
  }
}

// Display file contents to console
void catFile(FILE* in, Fat16BootSector* bs, char* filename, PartitionTable* pt) {
  readFile(in, bs, filename, pt, 0); // 0 means don't save to file, just display
}

// Save file contents to disk
void saveFile(FILE* in, Fat16BootSector* bs, char* filename, PartitionTable* pt) {
  readFile(in, bs, filename, pt, 1); // 1 means save to file
}

// Write file from stdin or a file to FAT16 filesystem
void write(char *args) {
    // Support: write [-f linuxfile] fatfile
    char *src_file = NULL;
    char *fatfile = NULL;
    char *token;
    char argbuf[256];
    strncpy(argbuf, args, 255);
    argbuf[255] = '\0';
    token = strtok(argbuf, " ");
    if (token && strcmp(token, "-f") == 0) {
        src_file = strtok(NULL, " ");
        fatfile = strtok(NULL, ""); // rest is the FAT16 path
        if (!src_file || !fatfile) {
            printf("Usage: write [-f linuxfile] fatfile\n");
            return;
        }
    } else {
        fatfile = args;
        while (*fatfile == ' ') fatfile++;
    }
    FILE *input = stdin;
    if (src_file) {
        input = fopen(src_file, "rb");
        if (!input) {
            printf("Cannot open source file: %s\n", src_file);
            return;
        }
    }
    
    char path_copy[256];
    strcpy(path_copy, fatfile);
    char *last_slash = strrchr(path_copy, '/');
    char *file_part = fatfile;
    uint16_t saved_cluster = current_dir_cluster;
    char saved_path[256];
    strcpy(saved_path, current_path);
    if (last_slash) {
        *last_slash = '\0';
        file_part = last_slash + 1;
        if (changeDir(path_copy) != 0) {
            printf("Directory %s not found\n", path_copy);
            if (src_file) fclose(input);
            return;
        }
    }
    
    // Step 1: Find free directory entry
    uint32_t entry_count;
    Fat16Entry* entries = readDirectory(current_dir_cluster, &entry_count);
    int free_entry_idx = -1;
    for (uint32_t i = 0; i < entry_count; i++) {
        if (entries[i].filename[0] == 0x00 || entries[i].filename[0] == 0xE5) {
            free_entry_idx = i;
            break;
        }
    }
    if (free_entry_idx == -1) {
        printf("No free directory entry found (directory full).\n");
        free(entries);
        if (src_file) fclose(input);
        if (last_slash) {
            current_dir_cluster = saved_cluster;
            strcpy(current_path, saved_path);
        }
        return;
    }
    
    // Step 2: Find first free cluster in FAT
    uint16_t total_clusters = (fs.bs.total_sectors_short ? fs.bs.total_sectors_short : fs.bs.total_sectors_int)
        / fs.bs.sectors_per_cluster;
    uint16_t first_free_cluster = 0;
    for (uint16_t c = 2; c < total_clusters; c++) {
        if (getFatEntry(c) == 0x0000) {
            first_free_cluster = c;
            break;
        }
    }
    if (first_free_cluster == 0) {
        printf("No free cluster found.\n");
        free(entries);
        if (src_file) fclose(input);
        if (last_slash) {
            current_dir_cluster = saved_cluster;
            strcpy(current_path, saved_path);
        }
        return;
    }
    
    // Step 3: Prepare directory entry
    Fat16Entry new_entry = {0};
    char fname[9], ext[4];
    formatToFatName(file_part, fname, ext);
    memcpy(new_entry.filename, fname, 8);
    memcpy(new_entry.ext, ext, 3);
    new_entry.attributes = 0x20; // Archive
    new_entry.starting_cluster = first_free_cluster;
    new_entry.file_size = 0; // Will update later
    time_t now = time(NULL);
    struct tm *tm_now = localtime(&now);
    new_entry.modify_date = ((tm_now->tm_year - 80) << 9) | ((tm_now->tm_mon + 1) << 5) | tm_now->tm_mday;
    new_entry.modify_time = (tm_now->tm_hour << 11) | (tm_now->tm_min << 5) | (tm_now->tm_sec / 2);
    
    // Step 4: Write data from input to clusters
    uint16_t current_cluster = first_free_cluster;
    uint16_t prev_cluster = 0;
    uint32_t bytes_written = 0;
    uint32_t cluster_size = fs.bs.sectors_per_cluster * fs.bs.sector_size;
    uint8_t *buffer = malloc(cluster_size);
    if (!buffer) {
        printf("Memory allocation error\n");
        free(entries);
        if (src_file) fclose(input);
        if (last_slash) {
            current_dir_cluster = saved_cluster;
            strcpy(current_path, saved_path);
        }
        return;
    }
    
    int last = 0;
    while (!last) {
        size_t n = fread(buffer, 1, cluster_size, input);
        if (n == 0) {
            if (input == stdin && feof(input)) {
                clearerr(input); // Reset EOF for next shell command
                break;
            } else if (input != stdin && feof(input)) {
                break;
            } else {
                printf("Read error from input.\n");
                break;
            }
        }
        if (n < cluster_size) last = 1;
        
        // Write to cluster
        uint32_t cluster_offset = fs.data_area_offset + (current_cluster - 2) * cluster_size;
        fseek(fs.fp, cluster_offset, SEEK_SET);
        fwrite(buffer, 1, n, fs.fp);
        fflush(fs.fp);
        
        // Update FAT
        uint16_t next_cluster = 0;
        if (!last) {
            for (uint16_t c = current_cluster + 1; c < total_clusters; c++) {
                if (getFatEntry(c) == 0x0000) {
                    next_cluster = c;
                    break;
                }
            }
            if (next_cluster == 0) {
                printf("Disk full while writing.\n");
                break;
            }
        } else {
            next_cluster = 0xFFFF; // EOC
        }
        
        uint32_t fat_offset = current_cluster * 2;
        uint32_t fat_sector = fs.bs.reserved_sectors + (fat_offset / fs.bs.sector_size);
        uint32_t entry_offset = fat_offset % fs.bs.sector_size;
        
        // Write to all FAT copies
        for (int f = 0; f < fs.bs.number_of_fats; f++) {
            fseek(fs.fp, (fat_sector * fs.bs.sector_size) + fs.pt[0].start_sector * 512 + entry_offset + f * fs.bs.fat_size_sectors * fs.bs.sector_size, SEEK_SET);
            uint16_t val = next_cluster;
            fwrite(&val, 2, 1, fs.fp);
            fflush(fs.fp);
        }
        
        prev_cluster = current_cluster;
        current_cluster = next_cluster;
        bytes_written += n;
    }
    
    free(buffer);
    if (src_file) fclose(input);
    
    // Step 5: Update directory entry with file size
    new_entry.file_size = bytes_written;
    uint32_t dir_offset;
    if (current_dir_cluster == 0) {
        dir_offset = fs.root_dir_offset + free_entry_idx * sizeof(Fat16Entry);
    } else {
        dir_offset = fs.data_area_offset + (current_dir_cluster - 2) * fs.bs.sectors_per_cluster * fs.bs.sector_size + free_entry_idx * sizeof(Fat16Entry);
    }
    
    fseek(fs.fp, dir_offset, SEEK_SET);
    fwrite(&new_entry, sizeof(Fat16Entry), 1, fs.fp);
    fflush(fs.fp);
    free(entries);
    
    printf("File '%s' written (%u bytes).\n", fatfile, bytes_written);
    if (last_slash) {
        current_dir_cluster = saved_cluster;
        strcpy(current_path, saved_path);
    }
}

// Delete a file from the FAT16 filesystem
void rm(char *filename) {
    uint32_t entry_count;
    Fat16Entry* entries = readDirectory(current_dir_cluster, &entry_count);
    if (!entries) {
      printf("Error reading directory\n");
      return;
    }
    
    // Find file by name
    Fat16Entry* entry = findEntryByName(entries, entry_count, filename);
    if (!entry || (entry->attributes & 0x10)) {
      printf("File '%s' not found or is a directory.\n", filename);
      free(entries);
      return;
    }
    
    // Save the directory entry offset
    size_t entry_idx = entry - entries;
    
    // Free all clusters in the FAT
    uint16_t cluster = entry->starting_cluster;
    while (cluster >= 0x0002 && cluster < 0xFFF8) {
      uint16_t next = getFatEntry(cluster);
      
      // Write to all copies of the FAT table
      for (int f = 0; f < fs.bs.number_of_fats; f++) {
        uint32_t fat_offset = cluster * 2;
        uint32_t fat_sector = fs.bs.reserved_sectors + (fat_offset / fs.bs.sector_size);
        uint32_t entry_offset = fat_offset % fs.bs.sector_size;
        fseek(fs.fp, (fat_sector * fs.bs.sector_size) + fs.pt[0].start_sector * 512 + entry_offset + f * fs.bs.fat_size_sectors * fs.bs.sector_size, SEEK_SET);
        uint16_t zero = 0x0000;
        fwrite(&zero, 2, 1, fs.fp);
        fflush(fs.fp);
      }
      
      cluster = next;
    }
    
    // Mark the file as deleted by changing the first character to 0xE5
    uint32_t dir_offset;
    if (current_dir_cluster == 0) {
        dir_offset = fs.root_dir_offset + entry_idx * sizeof(Fat16Entry);
    } else {
        dir_offset = fs.data_area_offset + (current_dir_cluster - 2) * fs.bs.sectors_per_cluster * fs.bs.sector_size + entry_idx * sizeof(Fat16Entry);
    }
    
    fseek(fs.fp, dir_offset, SEEK_SET);
    unsigned char e5 = 0xE5;
    fwrite(&e5, 1, 1, fs.fp);
    fflush(fs.fp);
    
    printf("File '%s' deleted.\n", filename);
    free(entries);
}
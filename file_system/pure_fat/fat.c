#include "fat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <stdint.h>

// Global variables to track current directory
uint16_t current_dir_cluster = 0;
char current_path[256] = "/";

// Structure to represent file system parameters
typedef struct {
    FILE* fp;
    Fat16BootSector bs;
    PartitionTable pt[4];
    uint32_t root_dir_offset;
    uint32_t data_area_offset;
} FatFS;

FatFS fs;

// Forward declarations
Fat16Entry* readDirectory(uint16_t cluster, uint32_t* entryCount);
uint16_t getFatEntry(uint16_t cluster);

// Utility functions
void formatDateTime(uint16_t date, uint16_t time, char* buffer) {
  int day = date & 0x1F;
  int month = (date >> 5) & 0x0F;
  int year = ((date >> 9) & 0x7F) + 1980;
  
  int hours = (time >> 11) & 0x1F;
  int minutes = (time >> 5) & 0x3F;
  int seconds = (time & 0x1F) * 2;
  
  sprintf(buffer, "%02d/%02d/%04d %02d:%02d:%02d", day, month, year, hours, minutes, seconds);
}

// Clean a FAT filename to make it human-readable
void cleanFatName(char* dst, const unsigned char* filename, const unsigned char* ext) {
  int i, j = 0;
  // Copy filename (removing trailing spaces)
  for (i = 0; i < 8 && filename[i] != ' '; i++) {
    if (filename[i] == 0x00) break;
    dst[j++] = filename[i];
  }
  // Add extension if present and not all spaces
  if (ext[0] != ' ' || ext[1] != ' ' || ext[2] != ' ') {
    dst[j++] = '.';
    for (i = 0; i < 3 && ext[i] != ' '; i++) {
      if (ext[i] == 0x00) break;
      dst[j++] = ext[i];
    }
  }
  dst[j] = '\0';
}

// Format a name to FAT 8.3 format
void formatToFatName(const char* input, char* fname, char* ext) {
  int i;
  const char* dot_pos = strchr(input, '.');
  int name_len;
  
  // Fill with spaces initially
  memset(fname, ' ', 8);
  memset(ext, ' ', 3);
  fname[8] = ext[3] = '\0';
  
  // Split filename and extension
  if (dot_pos) {
    name_len = dot_pos - input;
    if (name_len > 8) name_len = 8;
    strncpy(fname, input, name_len);
    
    strncpy(ext, dot_pos + 1, 3);
  } else {
    name_len = strlen(input);
    if (name_len > 8) name_len = 8;
    strncpy(fname, input, name_len);
  }
  
  // Convert to uppercase
  for (i = 0; i < 8; i++) 
    if (fname[i] != ' ') fname[i] = toupper(fname[i]);
  for (i = 0; i < 3; i++) 
    if (ext[i] != ' ') ext[i] = toupper(ext[i]);
}

// FAT file system functions
uint16_t getFatEntry(uint16_t cluster) {
  uint16_t fat_entry = 0;
  uint32_t fat_offset = cluster * 2;
  uint32_t fat_sector = fs.bs.reserved_sectors + (fat_offset / fs.bs.sector_size);
  uint32_t entry_offset = fat_offset % fs.bs.sector_size;
  
  fseek(fs.fp, (fat_sector * fs.bs.sector_size) + fs.pt[0].start_sector * 512 + entry_offset, SEEK_SET);
  fread(&fat_entry, 2, 1, fs.fp);
  
  return fat_entry;
}

Fat16Entry* readDirectory(uint16_t cluster, uint32_t* entryCount) {
  Fat16Entry* entries = NULL;
  uint16_t current_cluster = cluster;
  uint32_t total_entries = 0;
  uint32_t max_entries_per_cluster = (fs.bs.sectors_per_cluster * fs.bs.sector_size) / sizeof(Fat16Entry);
  
  // Root directory is in a fixed location with fixed size
  if (cluster == 0) {
    entries = (Fat16Entry*)malloc(fs.bs.root_dir_entries * sizeof(Fat16Entry));
    if (!entries) {
      printf("Memory allocation error\n");
      return NULL;
    }
    
    fseek(fs.fp, fs.root_dir_offset, SEEK_SET);
    fread(entries, sizeof(Fat16Entry), fs.bs.root_dir_entries, fs.fp);
    *entryCount = fs.bs.root_dir_entries;
    return entries;
  }
  
  // For other directories, need to follow the FAT chain
  while (current_cluster >= 0x0002 && current_cluster < 0xFFF8) {
    Fat16Entry* temp_entries = (Fat16Entry*)realloc(entries, 
                                                    (total_entries + max_entries_per_cluster) * sizeof(Fat16Entry));
    if (!temp_entries) {
      free(entries);
      printf("Memory allocation error\n");
      return NULL;
    }
    entries = temp_entries;
    
    // Calculate location of this cluster
    uint32_t cluster_offset = fs.data_area_offset + (current_cluster - 2) * fs.bs.sectors_per_cluster * fs.bs.sector_size;
    
    // Read directory entries from this cluster
    fseek(fs.fp, cluster_offset, SEEK_SET);
    fread(&entries[total_entries], sizeof(Fat16Entry), max_entries_per_cluster, fs.fp);
    
    total_entries += max_entries_per_cluster;
    
    // Get next cluster from FAT
    current_cluster = getFatEntry(current_cluster);
  }
  
  *entryCount = total_entries;
  return entries;
}

// Find a directory entry by name
Fat16Entry* findEntry(Fat16Entry* dir_entries, uint32_t entry_count, const char* name) {
  char fname[9];
  char ext[4];
  int i;
  
  // Special case for "." and ".."
  if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
    for (i = 0; i < entry_count; i++) {
      if (dir_entries[i].filename[0] == '.' && 
          (dir_entries[i].filename[1] == ' ' || 
           (strcmp(name, "..") == 0 && dir_entries[i].filename[1] == '.' && dir_entries[i].filename[2] == ' '))) {
        return &dir_entries[i];
      }
    }
    return NULL;
  }
  
  formatToFatName(name, fname, ext);
  
  // Search for the file/directory in entries
  for (i = 0; i < entry_count; i++) {
    if (dir_entries[i].filename[0] != 0x00 && dir_entries[i].filename[0] != 0xE5) {
      // For directories, compare only the filename part (ignore extension)
      if ((dir_entries[i].attributes & 0x10) && 
          strncmp(dir_entries[i].filename, fname, 8) == 0) {
        return &dir_entries[i];
      }
      // For files, check both parts
      else if (!(dir_entries[i].attributes & 0x10) && 
               strncmp(dir_entries[i].filename, fname, 8) == 0 && 
               strncmp(dir_entries[i].ext, ext, 3) == 0) {
        return &dir_entries[i];
      }
    }
  }
  
  return NULL;
}

// Better version of findEntry that correctly handles FAT16 format
Fat16Entry* findEntryByName(Fat16Entry* entries, uint32_t entry_count, const char* name) {
  char cleaned_name[13];
  int i;
  
  // Handle special directories
  if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) {
    for (i = 0; i < entry_count; i++) {
      if (entries[i].filename[0] == '.' && 
          ((strcmp(name, ".") == 0 && entries[i].filename[1] == ' ') || 
           (strcmp(name, "..") == 0 && entries[i].filename[1] == '.' && entries[i].filename[2] == ' '))) {
        return &entries[i];
      }
    }
    return NULL;
  }
  
  // For each directory entry
  for (i = 0; i < entry_count; i++) {
    // Skip deleted or empty entries
    if (entries[i].filename[0] == 0xE5 || entries[i].filename[0] == 0x00) {
      continue;
    }
    
    // Create a cleaned name from this entry
    cleanFatName(cleaned_name, entries[i].filename, entries[i].ext);
    
    // Compare in case-insensitive manner 
    if (strcasecmp(cleaned_name, name) == 0) {
      return &entries[i];
    }
  }
  
  return NULL;
}

// Helper function to format file attributes into a human-readable string
void formatAttributes(uint8_t attributes, char* buffer) {
    buffer[0] = (attributes & 0x01) ? 'R' : '-'; // Read-only
    buffer[1] = (attributes & 0x02) ? 'H' : '-'; // Hidden
    buffer[2] = (attributes & 0x04) ? 'S' : '-'; // System
    buffer[3] = (attributes & 0x08) ? 'V' : '-'; // Volume label
    buffer[4] = (attributes & 0x10) ? 'D' : '-'; // Directory
    buffer[5] = (attributes & 0x20) ? 'A' : '-'; // Archive
    buffer[6] = '\0';
}

// User interface functions
void printDirectoryEntries(Fat16Entry* entries, uint32_t entry_count) {
  int i;
  int fileCount = 0;
  uint32_t totalSize = 0;
  printf(" %-20s  Attr       Size  Date       Time     Cluster\n", "Name");
  printf("--------------------  ------  -------  ---------  -------  -------\n");
  for (i = 0; i < entry_count; i++) {
    if (entries[i].filename[0] != 0x00 && entries[i].filename[0] != 0xE5) {
      char dateTimeStr[20];
      char attrStr[7];
      char name_buf[20];
      cleanFatName(name_buf, entries[i].filename, entries[i].ext);
      formatDateTime(entries[i].modify_date, entries[i].modify_time, dateTimeStr);
      formatAttributes(entries[i].attributes, attrStr);
      printf(" %-20s  %s  %7d  %s  %5d\n",
             name_buf, attrStr, entries[i].file_size,
             dateTimeStr, entries[i].starting_cluster);
      if (!(entries[i].attributes & 0x10) && !(entries[i].attributes & 0x08)) {
        fileCount++;
        totalSize += entries[i].file_size;
      }
    }
  }
  printf("--------------------  ------  -------  ---------  -------  -------\n");
  printf("   %d File(s)    %d bytes\n", fileCount, totalSize);
  printf("\nAttribute legend: R-Read-only, H-Hidden, S-System, V-Volume, D-Directory, A-Archive\n");
}

// Revised changeDir implementation
int changeDir(char* path) {
  uint32_t entry_count;
  Fat16Entry* entries;
  Fat16Entry* entry;
  char* token;
  char path_copy[256];
  char temp_path[256];
  uint16_t temp_dir_cluster = current_dir_cluster;
  
  // Save current state
  strcpy(temp_path, current_path);
  
  // Handle absolute path
  if (path[0] == '/') {
    strcpy(temp_path, "/");
    temp_dir_cluster = 0; // Root directory
    
    // If path is just "/", we're done
    if (strlen(path) == 1) {
      current_dir_cluster = temp_dir_cluster;
      strcpy(current_path, temp_path);
      return 0;
    }
    
    // Skip leading slash for tokenization
    path++;
  }
  
  strcpy(path_copy, path);
  token = strtok(path_copy, "/");
  
  while (token) {
    // Get current directory entries
    entries = readDirectory(temp_dir_cluster, &entry_count);
    if (!entries) {
      return -1;
    }
    
    // Handle ".." (parent directory)
    if (strcmp(token, "..") == 0) {
      // If we're already at root, nothing to do
      if (temp_dir_cluster == 0) {
        free(entries);
        token = strtok(NULL, "/");
        continue;
      }
      
      // Find the ".." entry
      entry = findEntryByName(entries, entry_count, "..");
      if (entry && entry->starting_cluster != 0) {
        temp_dir_cluster = entry->starting_cluster;
      } else {
        // If no ".." entry or it points to 0, go to root
        temp_dir_cluster = 0;
      }
      
      // Update the path
      char* last_slash = strrchr(temp_path, '/');
      if (last_slash && last_slash != temp_path) {
        *last_slash = '\0';
      } else {
        strcpy(temp_path, "/");
      }
      
      free(entries);
      token = strtok(NULL, "/");
      continue;
    }
    
    // Handle "." (current directory)
    if (strcmp(token, ".") == 0) {
      free(entries);
      token = strtok(NULL, "/");
      continue;
    }
    
    // Find the directory entry using our new function
    entry = findEntryByName(entries, entry_count, token);
    
    if (!entry) {
      printf("Directory '%s' not found\n", token);
      free(entries);
      return -1;
    }
    
    // Check if it's a directory
    if (!(entry->attributes & 0x10)) {
      printf("'%s' is not a directory\n", token);
      free(entries);
      return -1;
    }
    
    // Update current directory
    temp_dir_cluster = entry->starting_cluster;
    if (temp_dir_cluster == 0) {
      // Special case: subdirectory pointing to cluster 0 means root
      temp_dir_cluster = 0;
      strcpy(temp_path, "/");
    } else {
      // Update path
      if (strcmp(temp_path, "/") != 0) {
        strcat(temp_path, "/");
      }
      strcat(temp_path, token);
    }
    
    free(entries);
    token = strtok(NULL, "/");
  }
  
  // Apply changes only if successful
  current_dir_cluster = temp_dir_cluster;
  strcpy(current_path, temp_path);
  
  return 0;
}

void printTreeRecursive(uint16_t cluster, int level, const char* prefix) {
  uint32_t entry_count;
  Fat16Entry* entries = readDirectory(cluster, &entry_count);
  int i;
  char name_buf[13];
  
  if (!entries) return;
  
  for (i = 0; i < entry_count; i++) {
    if (entries[i].filename[0] != 0x00 && entries[i].filename[0] != 0xE5) {
      // Skip '.' and '..' entries
      if (entries[i].filename[0] == '.' && 
          (entries[i].filename[1] == ' ' || 
           (entries[i].filename[1] == '.' && entries[i].filename[2] == ' '))) {
        continue;
      }
      
      // Format name properly - ensure clean filename
      memset(name_buf, 0, sizeof(name_buf));
      cleanFatName(name_buf, entries[i].filename, entries[i].ext);
      
      // Print indentation and proper tree structure
      fprintf(stderr, "%s├── ", prefix);
      
      // Print filename with attributes
      if (entries[i].attributes & 0x10) {
        // It's a directory
        fprintf(stderr, "[%s] (dir)\n", name_buf);
        
        // Recursive call for subdirectory
        if (entries[i].starting_cluster != 0) {
          char new_prefix[256];
          sprintf(new_prefix, "%s│   ", prefix);
          printTreeRecursive(entries[i].starting_cluster, level + 1, new_prefix);
        }
      } else {
        // It's a file
        fprintf(stderr, "%s (%d bytes)\n", name_buf, entries[i].file_size);
      }
    }
  }
  
  free(entries);
}

// Print the directory tree starting from current directory
void printTree() {
  fprintf(stderr, "Directory Tree:\n");
  
  if (current_dir_cluster == 0) {
    fprintf(stderr, "[Root]\n");
  } else {
    fprintf(stderr, "[%s]\n", current_path);
  }
  
  printTreeRecursive(current_dir_cluster, 0, "");
}

// File operations
void readFile(FILE* in, Fat16BootSector* bs, char* filename, PartitionTable* pt, int save_to_file) {
  Fat16Entry entry;
  char fname[9], ext[4];
  int i, found = 0;
  uint16_t cluster;
  uint16_t fat_entry, bytes_to_read;
  uint8_t* buffer;
  uint32_t entry_count;
  Fat16Entry* dir_entries;
  FILE* output_file = NULL;
  char* path_part;
  char* file_part;
  char path_copy[256];
  uint16_t temp_dir_cluster = current_dir_cluster;
  
  // Check if we have a path with directory components
  strcpy(path_copy, filename);
  if (strchr(path_copy, '/')) {
    char* token;
    char dir_path[256] = "";
    
    // Extract the directory part and the filename part
    file_part = strrchr(path_copy, '/');
    *file_part = '\0'; // Split the path at the last slash
    file_part++; // Move past the slash to get the filename
    
    // Now path_copy contains the directory path, and file_part points to the filename
    
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
  
  // Try to find the file using our improved findEntryByName function first
  Fat16Entry* found_entry = findEntryByName(dir_entries, entry_count, filename);
  if (found_entry && !(found_entry->attributes & 0x10)) {
    entry = *found_entry;
    found = 1;
  } else {
    // Format name for FAT lookup as a fallback
    formatToFatName(filename, fname, ext);
    
    // Traditional search for the file in current directory
    for (i = 0; i < entry_count; i++) {
      if (dir_entries[i].filename[0] != 0x00 && dir_entries[i].filename[0] != 0xE5 && 
          !(dir_entries[i].attributes & 0x10)) {
        if (memcmp(dir_entries[i].filename, fname, 8) == 0 && 
            memcmp(dir_entries[i].ext, ext, 3) == 0) {
          entry = dir_entries[i];
          found = 1;
          break;
        }
      }
    }
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

// Wrapper functions
void catFile(FILE* in, Fat16BootSector* bs, char* filename, PartitionTable* pt) {
  readFile(in, bs, filename, pt, 0); // 0 means don't save to file, just display
}

void saveFile(FILE* in, Fat16BootSector* bs, char* filename, PartitionTable* pt) {
  readFile(in, bs, filename, pt, 1); // 1 means save to file
}

// Write file from stdin to disk image
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
    // ...existing code of write, but use 'input' instead of stdin for fread...
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
        // Zápis do všech kopií FAT tabulky (number_of_fats)
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
    // Find file by name (case-insensitive, respecting FAT 8.3 naming convention)
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
    printf("Soubor '%s' byl smazán.\n", filename);
    free(entries);
}

// Initialization function
int initFileSystem(const char* image_path) {
  int i;
  
  fs.fp = fopen(image_path, "rb+");
  
  if (!fs.fp) {
    printf("Error opening %s file\n", image_path);
    return -1;
  }

  fseek(fs.fp, 0x1BE, SEEK_SET);                // go to partition table start
  fread(fs.pt, sizeof(PartitionTable), 4, fs.fp);  // read all entries (4)

  printf("Partition table\n-----------------------\n");
  for (i = 0; i < 4; i++) {
    printf("Partition %d, type %02X, ", i, fs.pt[i].partition_type);
    printf("start sector %8d, length %8d sectors\n", fs.pt[i].start_sector, fs.pt[i].length_sectors);
  }

  printf("\nSeeking to first partition by %d sectors\n", fs.pt[0].start_sector);
  fseek(fs.fp, 512 * fs.pt[0].start_sector, SEEK_SET);
  fread(&fs.bs, sizeof(Fat16BootSector), 1, fs.fp);
  printf("Volume_label %.11s, %d sectors size\n", fs.bs.volume_label, fs.bs.sector_size);

  // Calculate important offsets
  fs.root_dir_offset = (fs.bs.reserved_sectors + fs.bs.fat_size_sectors * fs.bs.number_of_fats) * 
                  fs.bs.sector_size + fs.pt[0].start_sector * 512;
  
  fs.data_area_offset = fs.root_dir_offset + (fs.bs.root_dir_entries * sizeof(Fat16Entry));
  
  // Set initial directory to root
  current_dir_cluster = 0;
  strcpy(current_path, "/");
  
  return 0;
}

void printHelp() {
    printf("Available commands:\n");
    printf("  cd <dir>                Change directory\n");
    printf("  ls [dir]                List directory contents\n");
    printf("  cat <file>              Print file contents\n");
    printf("  save <file>             Save file from FAT16 to Linux\n");
    printf("  tree                    Print directory tree\n");
    printf("  write <file>            Write file from stdin to FAT16\n");
    printf("  write -f <src> <file>   Write file from Linux file <src> to FAT16\n");
    printf("  rm <file>               Delete file from FAT16\n");
    printf("  help                    Show this help message\n");
    printf("  exit, quit              Exit the shell\n");
}

// CLI command parser and executor
void executeCommand(char* cmd) {
  if (strncmp(cmd, "cd ", 3) == 0) {
    changeDir(cmd + 3);
  } 
  else if (strncmp(cmd, "ls ", 3) == 0) {
    // Handle ls with directory path
    char* dir_path = cmd + 3;
    
    // Save current directory
    uint16_t saved_cluster = current_dir_cluster;
    char saved_path[256];
    strcpy(saved_path, current_path);
    
    // Try to change to the specified directory
    if (changeDir(dir_path) == 0) {
      // If successful, list its contents
      uint32_t entry_count;
      Fat16Entry* entries = readDirectory(current_dir_cluster, &entry_count);
      
      printf("Directory of %s:\n\n", current_path);
      
      if (entries) {
        printDirectoryEntries(entries, entry_count);
        free(entries);
      }
      
      // Restore original directory
      current_dir_cluster = saved_cluster;
      strcpy(current_path, saved_path);
    } else {
      printf("Directory %s not found\n", dir_path);
    }
  }
  else if (strcmp(cmd, "ls") == 0) {
    // List current directory
    uint32_t entry_count;
    Fat16Entry* entries = readDirectory(current_dir_cluster, &entry_count);
    
    printf("Directory of %s:\n\n", current_path);
    
    if (entries) {
      printDirectoryEntries(entries, entry_count);
      free(entries);
    }
  }
  else if (strncmp(cmd, "cat ", 4) == 0) {
    catFile(fs.fp, &fs.bs, cmd + 4, fs.pt);
  }
  else if (strncmp(cmd, "save ", 5) == 0) {
    saveFile(fs.fp, &fs.bs, cmd + 5, fs.pt);
  }
  else if (strcmp(cmd, "tree") == 0) {
    printTree();
  }
  else if (strcmp(cmd, "exit") == 0 || strcmp(cmd, "quit") == 0) {
    // Handled in the main loop
  }
  else if (strncmp(cmd, "write ", 6) == 0) {
    write(cmd + 6); // now takes the whole arg string
  }
  else if (strncmp(cmd, "rm ", 3) == 0) {
    rm(cmd + 3);
  }
  else if (strcmp(cmd, "help") == 0) {
    printHelp();
  }
  else {
    printf("Unknown command: %s\n", cmd);
    printf("Available commands: cd, ls, cat, save, tree, write, exit\n");
  }
}

// Entry point
int main() {
  if (initFileSystem("sd.img") != 0) {
    return 1;
  }
  
  printf("\nEntering interactive mode. Type 'help' for commands, 'exit' to quit.\n");
  printHelp();
  
  char cmd[256];
  int running = 1;
  
  while (running) {
    printf("\nFAT16:%s> ", current_path);
    if (fgets(cmd, sizeof(cmd), stdin) == NULL) {
      if (feof(stdin)) {
        clearerr(stdin); // Reset EOF for next command
        printf("\n");
        continue;
      }
      break;
    }
    cmd[strcspn(cmd, "\n")] = '\0';  // Remove newline
    if (strcmp(cmd, "exit") == 0 || strcmp(cmd, "quit") == 0) {
      running = 0;
    } else if (strlen(cmd) > 0) {
      executeCommand(cmd);
    }
  }

  fclose(fs.fp);
  return 0;
}

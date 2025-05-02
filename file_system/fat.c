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
  int i;
  
  // Copy filename (removing trailing spaces)
  memcpy(dst, filename, 8);
  dst[8] = '\0';
  
  for (i = 7; i >= 0; i--) {
    if (dst[i] == ' ') {
      dst[i] = '\0';
    } else {
      break;
    }
  }
  
  // Add extension if present and not all spaces
  if (ext[0] != ' ' || ext[1] != ' ' || ext[2] != ' ') {
    strcat(dst, ".");
    
    // Copy extension without trailing spaces
    char ext_copy[4] = {0};
    memcpy(ext_copy, ext, 3);
    
    // Remove control characters if any
    for (i = 0; i < 3; i++) {
      if (ext_copy[i] < 32 || ext_copy[i] > 126) {
        ext_copy[i] = '_';  // Replace non-printable chars
      }
    }
    
    // Append clean extension
    for (i = 0; i < 3 && ext_copy[i] != ' '; i++) {
      dst[strlen(dst)] = ext_copy[i];
    }
  }
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
  
  // Debug printout - show what we're looking for
  printf("Looking for directory/file: '%s' as '%.8s'.'%.3s'\n", name, fname, ext);
  
  // Search for the file/directory in entries
  for (i = 0; i < entry_count; i++) {
    if (dir_entries[i].filename[0] != 0x00 && dir_entries[i].filename[0] != 0xE5) {
      // Debug printout - show what entries we have
      printf("Entry %d: '%.8s'.'%.3s' (attr: 0x%02X)\n", i, 
             dir_entries[i].filename, dir_entries[i].ext, dir_entries[i].attributes);
      
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

// User interface functions
void printDirectoryEntries(Fat16Entry* entries, uint32_t entry_count) {
  int i;
  int fileCount = 0;
  uint32_t totalSize = 0;
  
  printf(" Name        Ext  Attr       Size  Date       Time     Cluster\n");
  printf("----------  ---  ------  -------  ---------  -------  -------\n");
  
  for (i = 0; i < entry_count; i++) {
    if (entries[i].filename[0] != 0x00 && entries[i].filename[0] != 0xE5) {
      char dateTimeStr[20];
      formatDateTime(entries[i].modify_date, entries[i].modify_time, dateTimeStr);
      
      printf(" %-8.8s  %-3.3s  0x%02X  %7d  %s  %5d\n", 
             entries[i].filename, entries[i].ext, entries[i].attributes, entries[i].file_size, 
             dateTimeStr, entries[i].starting_cluster);
      
      // Not a directory and not volume ID
      if (!(entries[i].attributes & 0x10) && !(entries[i].attributes & 0x08)) { 
        fileCount++;
        totalSize += entries[i].file_size;
      }
    }
  }
  
  printf("----------  ---  ------  -------  ---------  -------  -------\n");
  printf("   %d File(s)    %d bytes\n", fileCount, totalSize);
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
    char output_filename[256];
    char clean_name[13];
    
    // Create a clean filename
    cleanFatName(clean_name, entry.filename, entry.ext);
    
    // Use the original user-provided filename instead of the FAT name
    // This preserves the case and format of the filename
    if (strchr(filename, '.')) {
      sprintf(output_filename, "%s", filename);
    } else {
      // If no extension in user input, use the clean name from FAT
      sprintf(output_filename, "%s", clean_name);
    }
    
    output_file = fopen(output_filename, "wb");
    if (!output_file) {
      fprintf(stderr, "Error creating output file %s\n", output_filename);
      free(buffer);
      return;
    }
    fprintf(stderr, "Saving file to: %s\n", output_filename);
  }
  
  // File found, read its contents cluster by cluster
  cluster = entry.starting_cluster;
  uint32_t remaining_size = entry.file_size;
  uint32_t data_start = (bs->reserved_sectors + bs->number_of_fats * bs->fat_size_sectors + 
      (bs->root_dir_entries * 32 + bs->sector_size - 1) / bs->sector_size) * bs->sector_size + 
      pt[0].start_sector * 512;
  
  fprintf(stderr, "\nReading file: %s, size: %d bytes\n", filename, entry.file_size);
  
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
    fprintf(stderr, "File saved successfully\n");
  }
}

// Wrapper functions
void catFile(FILE* in, Fat16BootSector* bs, char* filename, PartitionTable* pt) {
  readFile(in, bs, filename, pt, 0); // 0 means don't save to file, just display
}

void saveFile(FILE* in, Fat16BootSector* bs, char* filename, PartitionTable* pt) {
  readFile(in, bs, filename, pt, 1); // 1 means save to file
}

// Initialization function
int initFileSystem(const char* image_path) {
  int i;
  
  fs.fp = fopen(image_path, "rb");
  
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

// CLI command parser and executor
void executeCommand(char* cmd) {
  if (strncmp(cmd, "cd ", 3) == 0) {
    changeDir(cmd + 3);
  } 
  else if (strcmp(cmd, "ls") == 0) {
    uint32_t entry_count;
    Fat16Entry* entries = readDirectory(current_dir_cluster, &entry_count);
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
  else {
    printf("Unknown command: %s\n", cmd);
    printf("Available commands: cd, ls, cat, save, tree, exit\n");
  }
}

// Entry point
int main() {
  if (initFileSystem("sd.img") != 0) {
    return 1;
  }
  
  // 1. Print directory tree to stderr as requested in the assignment
  fprintf(stderr, "Directory Tree (using printTree function):\n");
  printTree();
  
  // 2. List root directory contents
  printf("\nFilesystem root directory listing\n-----------------------\n");
  printf(" Volume in drive: %.11s\n", fs.bs.volume_label);
  printf(" Directory of root:\n\n");
  
  uint32_t entry_count;
  Fat16Entry* entries = readDirectory(0, &entry_count);
  if (entries) {
    printDirectoryEntries(entries, entry_count);
    free(entries);
  }
  
  // 3. Change to ADR1 directory
  printf("\nChanging to ADR1 directory...\n");
  if (changeDir("ADR1") == 0) {
    printf("Successfully changed to directory: %s\n", current_path);
    
    // 4. List ADR1 contents
    printf("\nDirectory listing of %s\n-----------------------\n", current_path);
    entries = readDirectory(current_dir_cluster, &entry_count);
    if (entries) {
      printDirectoryEntries(entries, entry_count);
      
      // 5. Find the first file to display
      int i;
      char filename[13] = {0};
      for (i = 0; i < entry_count; i++) {
        if (entries[i].filename[0] != 0x00 && entries[i].filename[0] != 0xE5 && 
            !(entries[i].attributes & 0x10) && !(entries[i].attributes & 0x08)) {
          cleanFatName(filename, entries[i].filename, entries[i].ext);
          break;
        }
      }
      
      free(entries);
      
      // 6. Display the file if found
      if (filename[0] != 0) {
        printf("\nDisplaying content of file '%s' from %s:\n", filename, current_path);
        catFile(fs.fp, &fs.bs, filename, fs.pt);
      }
    }
  } else {
    printf("Failed to change to ADR1 directory\n");
  }
  
  // Optional interactive mode
  printf("\n\nEntering interactive mode. Type 'exit' to quit.\n");
  char cmd[256];
  int running = 1;
  
  while (running) {
    printf("\nFAT16:%s> ", current_path);
    
    fgets(cmd, sizeof(cmd), stdin);
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

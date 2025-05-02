#include "fat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <stdint.h>

// Global variables to track current directory
Fat16Entry* current_dir = NULL;
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

void formatDateTime(uint16_t date, uint16_t time, char* buffer) {
  int day = date & 0x1F;
  int month = (date >> 5) & 0x0F;
  int year = ((date >> 9) & 0x7F) + 1980;
  
  int hours = (time >> 11) & 0x1F;
  int minutes = (time >> 5) & 0x3F;
  int seconds = (time & 0x1F) * 2;
  
  sprintf(buffer, "%02d/%02d/%04d %02d:%02d:%02d", day, month, year, hours, minutes, seconds);
}

// Get the FAT entry for a specific cluster
uint16_t getFatEntry(uint16_t cluster) {
  uint16_t fat_entry = 0;
  uint32_t fat_offset = cluster * 2;
  uint32_t fat_sector = fs.bs.reserved_sectors + (fat_offset / fs.bs.sector_size);
  uint32_t entry_offset = fat_offset % fs.bs.sector_size;
  
  fseek(fs.fp, (fat_sector * fs.bs.sector_size) + fs.pt[0].start_sector * 512 + entry_offset, SEEK_SET);
  fread(&fat_entry, 2, 1, fs.fp);
  
  return fat_entry;
}

// Read the content of a directory given its starting cluster
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

// Find an entry in a directory by name
Fat16Entry* findEntry(Fat16Entry* dir_entries, uint32_t entry_count, const char* name) {
  char fname[9] = {0};
  char ext[4] = {0};
  const char* dot_pos = strchr(name, '.');
  int i, name_len;
  
  // Split filename and extension
  if (dot_pos) {
    name_len = dot_pos - name;
    if (name_len > 8) name_len = 8;
    strncpy(fname, name, name_len);
    
    strncpy(ext, dot_pos + 1, 3);
    ext[3] = '\0';
  } else {
    strncpy(fname, name, 8);
  }
  
  // Pad with spaces
  for (i = strlen(fname); i < 8; i++) fname[i] = ' ';
  for (i = strlen(ext); i < 3; i++) ext[i] = ' ';
  
  // Convert to uppercase
  for (i = 0; i < 8; i++) fname[i] = toupper(fname[i]);
  for (i = 0; i < 3; i++) ext[i] = toupper(ext[i]);
  
  // Search for the file/directory in entries
  for (i = 0; i < entry_count; i++) {
    if (dir_entries[i].filename[0] != 0x00 && dir_entries[i].filename[0] != 0xE5) {
      if (memcmp(dir_entries[i].filename, fname, 8) == 0 && 
          memcmp(dir_entries[i].ext, ext, 3) == 0) {
        return &dir_entries[i];
      }
    }
  }
  
  return NULL;
}

// Print information about directory entries
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

// Change current directory
int changeDir(char* path) {
  uint32_t entry_count;
  Fat16Entry* entries;
  Fat16Entry* entry;
  char* token;
  char path_copy[256];
  
  // Handle absolute path
  if (path[0] == '/') {
    strcpy(current_path, "/");
    current_dir_cluster = 0; // Root directory
    
    // If path is just "/", we're done
    if (strlen(path) == 1) {
      return 0;
    }
    
    // Skip leading slash for tokenization
    path++;
  }
  
  strcpy(path_copy, path);
  token = strtok(path_copy, "/");
  
  while (token) {
    // Get current directory entries
    entries = readDirectory(current_dir_cluster, &entry_count);
    if (!entries) {
      return -1;
    }
    
    // Handle ".." (parent directory)
    if (strcmp(token, "..") == 0) {
      // If we're already at root, nothing to do
      if (current_dir_cluster == 0) {
        free(entries);
        token = strtok(NULL, "/");
        continue;
      }
      
      // Remove the last directory from the path
      char* last_slash = strrchr(current_path, '/');
      if (last_slash && last_slash != current_path) {
        *last_slash = '\0';
      } else {
        strcpy(current_path, "/");
      }
      
      // TODO: We would need to navigate up the directory tree
      // This would require keeping track of parent directories
      // For simplicity, we'll reset to root for now
      current_dir_cluster = 0;
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
    
    // Find the requested directory entry
    entry = findEntry(entries, entry_count, token);
    
    if (!entry || !(entry->attributes & 0x10)) {
      printf("Directory '%s' not found\n", token);
      free(entries);
      return -1;
    }
    
    // Update current directory
    current_dir_cluster = entry->starting_cluster;
    
    // Update path
    if (strcmp(current_path, "/") != 0) {
      strcat(current_path, "/");
    }
    strcat(current_path, token);
    
    free(entries);
    token = strtok(NULL, "/");
  }
  
  printf("Current directory: %s\n", current_path);
  return 0;
}

// Print the directory tree recursively
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
      
      // Format name
      memcpy(name_buf, entries[i].filename, 8);
      name_buf[8] = '\0';
      // Remove trailing spaces
      for (int j = 7; j >= 0; j--) {
        if (name_buf[j] == ' ') {
          name_buf[j] = '\0';
        } else {
          break;
        }
      }
      
      // Add extension if present
      if (entries[i].ext[0] != ' ') {
        char ext_buf[5] = ".";
        memcpy(ext_buf + 1, entries[i].ext, 3);
        ext_buf[4] = '\0';
        // Remove trailing spaces from extension
        for (int j = 3; j >= 1; j--) {
          if (ext_buf[j] == ' ') {
            ext_buf[j] = '\0';
          } else {
            break;
          }
        }
        strcat(name_buf, ext_buf);
      }
      
      // Print indentation
      fprintf(stderr, "%s", prefix);
      
      // Print filename with attributes
      if (entries[i].attributes & 0x10) {
        // It's a directory
        fprintf(stderr, "[%s] (0x%02X)\n", name_buf, entries[i].attributes);
        
        // Prepare next level indentation
        char new_prefix[256];
        sprintf(new_prefix, "%s|   ", prefix);
        
        // Recursive call for subdirectory
        if (entries[i].starting_cluster != 0) {
          printTreeRecursive(entries[i].starting_cluster, level + 1, new_prefix);
        }
      } else {
        // It's a file
        fprintf(stderr, "%s (0x%02X, %d bytes)\n", name_buf, entries[i].attributes, entries[i].file_size);
      }
    }
  }
  
  free(entries);
}

// Wrapper to start directory tree printing
void printTree() {
  fprintf(stderr, "Directory Tree:\n");
  fprintf(stderr, "/\n");
  printTreeRecursive(0, 0, "|   ");
}

void read(FILE* in, Fat16BootSector* bs, char* filename, PartitionTable* pt, int save_to_file) {
  Fat16Entry entry;
  char fname[9], ext[4];
  int i, found = 0;
  uint16_t cluster;
  uint32_t fat_offset, fat_sector, entry_offset;
  uint16_t fat_entry, bytes_to_read;
  uint8_t* buffer;
  uint32_t dir_offset;
  uint32_t entry_count;
  Fat16Entry* dir_entries;
  FILE* output_file = NULL;
  
  // Read from current directory
  dir_entries = readDirectory(current_dir_cluster, &entry_count);
  if (!dir_entries) {
    printf("Error reading directory\n");
    return;
  }
  
  // Extract filename and extension (and convert to uppercase)
  for (i = 0; i < 8 && filename[i] != '.' && filename[i] != '\0'; i++) {
    fname[i] = toupper(filename[i]);
  }
  while (i < 8) fname[i++] = ' ';  // Pad with spaces
  fname[8] = '\0';
  
  if (strchr(filename, '.')) {
    char* extension = strchr(filename, '.') + 1;
    for (i = 0; i < 3 && extension[i] != '\0'; i++) {
      ext[i] = toupper(extension[i]);
    }
    while (i < 3) ext[i++] = ' ';  // Pad with spaces
  } else {
    for (i = 0; i < 3; i++) ext[i] = ' ';
  }
  ext[3] = '\0';
  
  // Search for the file in current directory
  for (i = 0; i < entry_count; i++) {
    if (dir_entries[i].filename[0] != 0x00 && dir_entries[i].filename[0] != 0xE5 && 
        !(dir_entries[i].attributes & 0x10)) {
      if (strncmp((char*)dir_entries[i].filename, fname, 8) == 0 && 
          strncmp((char*)dir_entries[i].ext, ext, 3) == 0) {
        entry = dir_entries[i];
        found = 1;
        break;
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
    
    // Create a clean filename (without FAT padding spaces)
    memcpy(clean_name, entry.filename, 8);
    clean_name[8] = '\0';
    
    // Remove trailing spaces
    for (i = 7; i >= 0; i--) {
      if (clean_name[i] == ' ') {
        clean_name[i] = '\0';
      } else {
        break;
      }
    }
    
    // Add extension if present
    if (entry.ext[0] != ' ') {
      strcat(clean_name, ".");
      for (i = 0; i < 3 && entry.ext[i] != ' '; i++) {
        clean_name[strlen(clean_name)] = entry.ext[i];
      }
    }
    
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
    fat_sector = bs->reserved_sectors + (cluster * 2) / bs->sector_size;
    entry_offset = (cluster * 2) % bs->sector_size;
    
    fseek(in, (fat_sector * bs->sector_size) + pt[0].start_sector * 512 + entry_offset, SEEK_SET);
    fread(&fat_entry, 2, 1, in);
    
    // Update cluster for next iteration
    cluster = fat_entry;
  }
  
  free(buffer);
  
  if (save_to_file && output_file) {
    fclose(output_file);
    fprintf(stderr, "File saved successfully\n");
  }
}

// Alias for read function with a more intuitive name
void cat(FILE* in, Fat16BootSector* bs, char* filename, PartitionTable* pt) {
  read(in, bs, filename, pt, 0); // 0 means don't save to file, just display
}

// Function to save file content to disk
void save(FILE* in, Fat16BootSector* bs, char* filename, PartitionTable* pt) {
  read(in, bs, filename, pt, 1); // 1 means save to file
}

int main() {
  int i;
  char choice[32];
  
  fs.fp = fopen("sd.img", "rb");
  
  if (!fs.fp) {
    printf("Error opening sd.img file\n");
    return 1;
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
  
  // Print directory tree to stderr
  printTree();
  
  // List root directory contents
  printf("\nFilesystem root directory listing\n-----------------------\n");
  printf(" Volume in drive: %.11s\n", fs.bs.volume_label);
  printf(" Directory of root:\n\n");
  
  uint32_t entry_count;
  Fat16Entry* entries = readDirectory(0, &entry_count);
  if (entries) {
    printDirectoryEntries(entries, entry_count);
    free(entries);
  }
  
  // Interactive mode
  int running = 1;
  while (running) {
    printf("\nCurrent directory: %s\n", current_path);
    printf("Commands: \n");
    printf("1. cd <path> - Change directory\n");
    printf("2. ls - List directory contents\n");
    printf("3. cat <filename> - Display file contents\n");
    printf("4. save <filename> - Save file contents to disk\n");
    printf("5. tree - Display directory tree\n");
    printf("6. exit - Exit program\n");
    printf("Enter command: ");
    
    char cmd[256];
    fgets(cmd, sizeof(cmd), stdin);
    cmd[strcspn(cmd, "\n")] = '\0';  // Remove newline
    
    if (strncmp(cmd, "cd ", 3) == 0) {
      changeDir(cmd + 3);
    } 
    else if (strcmp(cmd, "ls") == 0) {
      entries = readDirectory(current_dir_cluster, &entry_count);
      if (entries) {
        printDirectoryEntries(entries, entry_count);
        free(entries);
      }
    }
    else if (strncmp(cmd, "cat ", 4) == 0) {
      cat(fs.fp, &fs.bs, cmd + 4, fs.pt);
    }
    else if (strncmp(cmd, "save ", 5) == 0) {
      save(fs.fp, &fs.bs, cmd + 5, fs.pt);
    }
    else if (strcmp(cmd, "tree") == 0) {
      printTree();
    }
    else if (strcmp(cmd, "exit") == 0) {
      running = 0;
    }
    else {
      printf("Unknown command\n");
    }
  }

  fclose(fs.fp);
  return 0;
}

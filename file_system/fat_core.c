#include "fat_core.h"
#include "fat_utils.h"
#include <stdlib.h>
#include <string.h>

// Global variables definition
uint16_t current_dir_cluster = 0;
char current_path[256] = "/";
FatFS fs;

// Get FAT entry for a cluster
uint16_t getFatEntry(uint16_t cluster) {
  uint16_t fat_entry = 0;
  uint32_t fat_offset = cluster * 2;
  uint32_t fat_sector = fs.bs.reserved_sectors + (fat_offset / fs.bs.sector_size);
  uint32_t entry_offset = fat_offset % fs.bs.sector_size;
  
  fseek(fs.fp, (fat_sector * fs.bs.sector_size) + fs.pt[0].start_sector * 512 + entry_offset, SEEK_SET);
  fread(&fat_entry, 2, 1, fs.fp);
  
  return fat_entry;
}

// Read a directory (root or subdirectory)
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
    memset(cleaned_name, 0, sizeof(cleaned_name));
    cleanFatName(cleaned_name, entries[i].filename, entries[i].ext);
    
    // Compare in case-insensitive manner 
    if (strcasecmp(cleaned_name, name) == 0) {
      return &entries[i];
    }
  }
  
  return NULL;
}

// Initialize the filesystem
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
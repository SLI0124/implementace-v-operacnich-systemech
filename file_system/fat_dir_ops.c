#include "fat_dir_ops.h"
#include "fat_utils.h"
#include <stdlib.h>
#include <string.h>

// Change current directory
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
    
    // Find the directory entry
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

// Print directory entries in a formatted way
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

// Print directory tree recursively
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
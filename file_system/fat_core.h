#ifndef FAT_CORE_H
#define FAT_CORE_H

#include <stdio.h>
#include <stdint.h>
#include "fat.h" // Include original FAT16 structures

// Structure to represent file system parameters
typedef struct {
    FILE* fp;
    Fat16BootSector bs;
    PartitionTable pt[4];
    uint32_t root_dir_offset;
    uint32_t data_area_offset;
} FatFS;

// Global variables to track current directory
extern uint16_t current_dir_cluster;
extern char current_path[256];
extern FatFS fs;

// Core FAT functions
uint16_t getFatEntry(uint16_t cluster);
Fat16Entry* readDirectory(uint16_t cluster, uint32_t* entryCount);
Fat16Entry* findEntryByName(Fat16Entry* entries, uint32_t entry_count, const char* name);
int initFileSystem(const char* image_path);

#endif // FAT_CORE_H
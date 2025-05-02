#ifndef FAT_FILE_OPS_H
#define FAT_FILE_OPS_H

#include "fat_core.h"

// File operations
void readFile(FILE* in, Fat16BootSector* bs, char* filename, PartitionTable* pt, int save_to_file);
void catFile(FILE* in, Fat16BootSector* bs, char* filename, PartitionTable* pt);
void saveFile(FILE* in, Fat16BootSector* bs, char* filename, PartitionTable* pt);
void write(char *args);
void rm(char *filename);

#endif // FAT_FILE_OPS_H
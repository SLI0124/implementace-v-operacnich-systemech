#ifndef FAT_DIR_OPS_H
#define FAT_DIR_OPS_H

#include "fat_core.h"

// Directory operations
int changeDir(char* path);
void printDirectoryEntries(Fat16Entry* entries, uint32_t entry_count);
void printTreeRecursive(uint16_t cluster, int level, const char* prefix);
void printTree(void);

#endif // FAT_DIR_OPS_H
#ifndef FAT_UTILS_H
#define FAT_UTILS_H

#include <stdint.h>

// Utility functions for FAT16 filesystem
void formatDateTime(uint16_t date, uint16_t time, char* buffer);
void cleanFatName(char* dst, const unsigned char* filename, const unsigned char* ext);
void formatToFatName(const char* input, char* fname, char* ext);
void formatAttributes(uint8_t attributes, char* buffer);

#endif // FAT_UTILS_H
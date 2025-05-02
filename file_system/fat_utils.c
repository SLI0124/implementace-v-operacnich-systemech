#include "fat_utils.h"
#include "fat_core.h"
#include <ctype.h>
#include <string.h>
#include <stdio.h>

// Format FAT date/time fields into a human-readable string
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
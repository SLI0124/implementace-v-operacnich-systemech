#include "fat.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>
#include <stdint.h>

void formatDateTime(uint16_t date, uint16_t time, char* buffer) {
  int day = date & 0x1F;
  int month = (date >> 5) & 0x0F;
  int year = ((date >> 9) & 0x7F) + 1980;
  
  int hours = (time >> 11) & 0x1F;
  int minutes = (time >> 5) & 0x3F;
  int seconds = (time & 0x1F) * 2;
  
  sprintf(buffer, "%02d/%02d/%04d %02d:%02d:%02d", day, month, year, hours, minutes, seconds);
}

void read(FILE* in, Fat16BootSector* bs, char* filename, PartitionTable* pt) {
  Fat16Entry entry;
  char fname[9], ext[4];
  int i, found = 0;
  uint16_t cluster;
  uint32_t fat_offset, fat_sector, entry_offset;
  uint16_t fat_entry, bytes_to_read;
  uint8_t* buffer;
  
  // Extract filename and extension (and convert to uppercase)
  for (i = 0; i < 8 && filename[i] != '.' && filename[i] != '\0'; i++) {
    fname[i] = filename[i];
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
  
  // Go to the beginning of root directory
  fseek(in, (bs->reserved_sectors + bs->fat_size_sectors * bs->number_of_fats) * bs->sector_size + 
      pt[0].start_sector * 512, SEEK_SET);
  
  // Search for the file in root directory
  for (i = 0; i < bs->root_dir_entries; i++) {
    fread(&entry, sizeof(entry), 1, in);
    if (entry.filename[0] != 0x00 && entry.filename[0] != 0xE5 && !(entry.attributes & 0x10)) {
      if (strncmp((char*)entry.filename, fname, 8) == 0 && strncmp((char*)entry.ext, ext, 3) == 0) {
        found = 1;
        break;
      }
    }
  }
  
  if (!found) {
    printf("File %s not found\n", filename);
    return;
  }
  
  // Allocate buffer for reading clusters
  buffer = (uint8_t*)malloc(bs->sectors_per_cluster * bs->sector_size);
  if (!buffer) {
    printf("Memory allocation error\n");
    return;
  }
  
  // File found, read its contents cluster by cluster
  cluster = entry.starting_cluster;
  uint32_t remaining_size = entry.file_size;
  uint32_t data_start = (bs->reserved_sectors + bs->number_of_fats * bs->fat_size_sectors + 
      (bs->root_dir_entries * 32 + bs->sector_size - 1) / bs->sector_size) * bs->sector_size + 
      pt[0].start_sector * 512;
  
  printf("\nReading file: %s.%s, size: %d bytes\n", fname, ext, entry.file_size);
  
  // Read all clusters
  while (cluster >= 0x0002 && cluster < 0xFFF8 && remaining_size > 0) {
    // Calculate the absolute position of the cluster
    fseek(in, data_start + (cluster - 2) * bs->sectors_per_cluster * bs->sector_size, SEEK_SET);
    
    // Read the cluster
    bytes_to_read = (remaining_size > bs->sectors_per_cluster * bs->sector_size) ? 
                    bs->sectors_per_cluster * bs->sector_size : remaining_size;
    fread(buffer, bytes_to_read, 1, in);
    
    // Output the data to stdout
    fwrite(buffer, bytes_to_read, 1, stdout);
    
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
  printf("\n");
}

int main() {
  FILE* in = fopen("sd.img", "rb");
  int i;
  PartitionTable pt[4];
  Fat16BootSector bs;
  Fat16Entry entry;
  
  if (!in) {
    printf("Error opening sd.img file\n");
    return 1;
  }

  fseek(in, 0x1BE, SEEK_SET);                // go to partition table start, partitions start at offset 0x1BE, see http://www.cse.scu.edu/~tschwarz/coen252_07Fall/Lectures/HDPartitions.html
  fread(pt, sizeof(PartitionTable), 4, in);  // read all entries (4)

  printf("Partition table\n-----------------------\n");
  for (i = 0; i < 4; i++) { // for all partition entries print basic info
    printf("Partition %d, type %02X, ", i, pt[i].partition_type);
    printf("start sector %8d, length %8d sectors\n", pt[i].start_sector, pt[i].length_sectors);
  }

  printf("\nSeeking to first partition by %d sectors\n", pt[0].start_sector);
  fseek(in, 512 * pt[0].start_sector, SEEK_SET); // Boot sector starts here (seek in bytes)
  fread(&bs, sizeof(Fat16BootSector), 1, in);    // Read boot sector content, see http://www.tavi.co.uk/phobos/fat.html#boot_block
  printf("Volume_label %.11s, %d sectors size\n", bs.volume_label, bs.sector_size);

  // Seek to the beginning of root directory, it's position is fixed
  fseek(in, (bs.reserved_sectors - 1 + bs.fat_size_sectors * bs.number_of_fats) * bs.sector_size, SEEK_CUR);

  // Read all entries of root directory
  printf("\nFilesystem root directory listing\n-----------------------\n");
  printf(" Volume in drive: %.11s\n", bs.volume_label);
  printf(" Directory of root:\n\n");
  printf(" Name        Ext  Attr       Size  Date       Time     Cluster\n");
  printf("----------  ---  ------  -------  ---------  -------  -------\n");
  
  int fileCount = 0;
  uint32_t totalSize = 0;
  
  for (i = 0; i < bs.root_dir_entries; i++) {
    fread(&entry, sizeof(entry), 1, in);
    // Skip if filename was never used or deleted
    if (entry.filename[0] != 0x00 && entry.filename[0] != 0xE5) {
      char dateTimeStr[20];
      formatDateTime(entry.modify_date, entry.modify_time, dateTimeStr);
      
      // Display the attribute byte in hexadecimal
      printf(" %-8.8s  %-3.3s  0x%02X  %7d  %s  %5d\n", 
             entry.filename, entry.ext, entry.attributes, entry.file_size, 
             dateTimeStr, entry.starting_cluster);
      
      // Not a directory and not volume ID
      if (!(entry.attributes & 0x10) && !(entry.attributes & 0x08)) { 
        fileCount++;
        totalSize += entry.file_size;
      }
    }
  }
  
  printf("----------  ---  ------  -------  ---------  -------  -------\n");
  printf("   %d File(s)    %d bytes\n", fileCount, totalSize);
  
  // Reading example files
  char choice[32];
  printf("\nEnter filename to read (e.g., ABSTRAKT.TXT or FAT16.JPG): ");
  scanf("%s", choice);
  
  // Reset file pointer before reading
  fseek(in, 0, SEEK_SET);
  read(in, &bs, choice, pt);

  fclose(in);
  return 0;
}

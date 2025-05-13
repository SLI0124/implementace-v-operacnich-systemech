// FAT16 structures
// see http://www.tavi.co.uk/phobos/fat.html

#include <stdio.h>
#include <stdint.h>
#include <time.h>

typedef struct {
    unsigned char first_byte;
    unsigned char start_chs[3];
    unsigned char partition_type;
    unsigned char end_chs[3];
    unsigned int start_sector;
    unsigned int length_sectors;
} __attribute((packed)) PartitionTable;

typedef struct {
    unsigned char jmp[3];
    char oem[8];
    unsigned short sector_size;
    unsigned char sectors_per_cluster;
    unsigned short reserved_sectors;
    unsigned char number_of_fats;
    unsigned short root_dir_entries;
    unsigned short total_sectors_short; // if zero, later field is used
    unsigned char media_descriptor;
    unsigned short fat_size_sectors;
    unsigned short sectors_per_track;
    unsigned short number_of_heads;
    unsigned int hidden_sectors;
    unsigned int total_sectors_int;
    unsigned char drive_number;
    unsigned char current_head;
    unsigned char boot_signature;
    unsigned int volume_id;
    char volume_label[11];
    char fs_type[8];
    char boot_code[448];
    unsigned short boot_sector_signature;
} __attribute((packed)) Fat16BootSector;

typedef struct {
    unsigned char filename[8];
    unsigned char ext[3];
    unsigned char attributes;
    unsigned char reserved[10];
    unsigned short modify_time;
    unsigned short modify_date;
    unsigned short starting_cluster;
    unsigned int file_size;
} __attribute((packed)) Fat16Entry;

// External structure to represent file system parameters
typedef struct {
    FILE* fp;
    Fat16BootSector bs;
    PartitionTable pt[4];
    uint32_t root_dir_offset;
    uint32_t data_area_offset;
} FatFS;

// Global variables for use by the fat_fuse.c
extern FatFS fs;
extern uint16_t current_dir_cluster;
extern char current_path[256];

// Function declarations for functions used by fat_fuse.c
int init_file_system(const char* image_path);
Fat16Entry* read_directory(uint16_t cluster, uint32_t* entryCount);
Fat16Entry* find_entry_by_name(Fat16Entry* entries, uint32_t entry_count, const char* name);
void clean_fat_name(char* dst, const unsigned char* filename, const unsigned char* ext);
void format_to_fat_name(const char* input, char* fname, char* ext);
int change_dir(const char* path);
uint16_t get_fat_entry(uint16_t cluster);
time_t fat_date_time_to_unix(uint16_t date, uint16_t time);

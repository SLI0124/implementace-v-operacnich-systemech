#include "fat_shell.h"
#include "fat_core.h"
#include "fat_dir_ops.h"
#include "fat_file_ops.h"
#include <stdlib.h>
#include <string.h>

// Display help information
void printHelp() {
    printf("Available commands:\n");
    printf("  cd <dir>                Change directory\n");
    printf("  ls [dir]                List directory contents\n");
    printf("  cat <file>              Print file contents\n");
    printf("  save <file>             Save file from FAT16 to Linux\n");
    printf("  tree                    Print directory tree\n");
    printf("  write <file>            Write file from stdin to FAT16\n");
    printf("  write -f <src> <file>   Write file from Linux file <src> to FAT16\n");
    printf("  rm <file>               Delete file from FAT16\n");
    printf("  help                    Show this help message\n");
    printf("  exit, quit              Exit the shell\n");
}

// Process user commands
void executeCommand(char* cmd) {
  if (strncmp(cmd, "cd ", 3) == 0) {
    changeDir(cmd + 3);
  } 
  else if (strncmp(cmd, "ls ", 3) == 0) {
    // Handle ls with directory path
    char* dir_path = cmd + 3;
    
    // Save current directory
    uint16_t saved_cluster = current_dir_cluster;
    char saved_path[256];
    strcpy(saved_path, current_path);
    
    // Try to change to the specified directory
    if (changeDir(dir_path) == 0) {
      // If successful, list its contents
      uint32_t entry_count;
      Fat16Entry* entries = readDirectory(current_dir_cluster, &entry_count);
      
      printf("Directory of %s:\n\n", current_path);
      
      if (entries) {
        printDirectoryEntries(entries, entry_count);
        free(entries);
      }
      
      // Restore original directory
      current_dir_cluster = saved_cluster;
      strcpy(current_path, saved_path);
    } else {
      printf("Directory %s not found\n", dir_path);
    }
  }
  else if (strcmp(cmd, "ls") == 0) {
    // List current directory
    uint32_t entry_count;
    Fat16Entry* entries = readDirectory(current_dir_cluster, &entry_count);
    
    printf("Directory of %s:\n\n", current_path);
    
    if (entries) {
      printDirectoryEntries(entries, entry_count);
      free(entries);
    }
  }
  else if (strncmp(cmd, "cat ", 4) == 0) {
    catFile(fs.fp, &fs.bs, cmd + 4, fs.pt);
  }
  else if (strncmp(cmd, "save ", 5) == 0) {
    saveFile(fs.fp, &fs.bs, cmd + 5, fs.pt);
  }
  else if (strcmp(cmd, "tree") == 0) {
    printTree();
  }
  else if (strncmp(cmd, "write ", 6) == 0) {
    write(cmd + 6);
  }
  else if (strncmp(cmd, "rm ", 3) == 0) {
    rm(cmd + 3);
  }
  else if (strcmp(cmd, "help") == 0) {
    printHelp();
  }
  else if (strcmp(cmd, "exit") == 0 || strcmp(cmd, "quit") == 0) {
    // Handled in the main loop
  }
  else {
    printf("Unknown command: %s\n", cmd);
    printf("Type 'help' for available commands\n");
  }
}
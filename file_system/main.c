#include "fat_core.h"
#include "fat_shell.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main() {
  if (initFileSystem("sd.img") != 0) {
    return 1;
  }
  
  printf("\nEntering interactive mode. Type 'help' for commands, 'exit' to quit.\n");
  printHelp();
  
  char cmd[256];
  int running = 1;
  
  while (running) {
    printf("\nFAT16:%s> ", current_path);
    if (fgets(cmd, sizeof(cmd), stdin) == NULL) {
      if (feof(stdin)) {
        clearerr(stdin); // Reset EOF for next command
        printf("\n");
        continue;
      }
      break;
    }
    cmd[strcspn(cmd, "\n")] = '\0';  // Remove newline
    if (strcmp(cmd, "exit") == 0 || strcmp(cmd, "quit") == 0) {
      running = 0;
    } else if (strlen(cmd) > 0) {
      executeCommand(cmd);
    }
  }

  fclose(fs.fp);
  return 0;
}
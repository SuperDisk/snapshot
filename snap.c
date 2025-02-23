#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <ucontext.h>

#define USE_DL_PREFIX
#define ONLY_MSPACES 1
#include "dlmalloc.c"

#define HEAP_SIZE (10 * 1024 * 1024)  // 10 MB
#define MAP_ADDRESS 0x40000000          // 1GB mark
#define STACK_SIZE (64 * 1024)          // 64KB stack

struct program_state {
  ucontext_t context;
  mspace arena;
  int has_saved_context;

  char stack[STACK_SIZE];
  char heap[HEAP_SIZE];
};

static struct program_state* state;
static char* state_filename;

void dump_snapshot() {
  // Now we're on our managed stack, so getcontext will capture the right stack
  if (getcontext(&state->context) == -1) {
    perror("getcontext");
    exit(1);
  }

  if (state->has_saved_context) {
    puts("State restored successfully!");
    return;
  }

  state->has_saved_context = 1;

  FILE *fp = fopen(state_filename, "wb");
  if (!fp) {
    perror("fopen");
    exit(1);
  }

  if (fwrite((void*)MAP_ADDRESS, sizeof(struct program_state), 1, fp) != 1) {
    perror("fwrite");
    fclose(fp);
    exit(1);
  }

  fclose(fp);
  printf("State saved. Run with -r and same filename to restore.\n");
  exit(0);
}

void load_snapshot(void* buffer) {
  FILE *fp = fopen(state_filename, "rb");
  if (!fp) {
    perror("fopen");
    exit(1);
  }

  if (fread(buffer, sizeof(struct program_state), 1, fp) != 1) {
    perror("fread");
    fclose(fp);
    exit(1);
  }

  fclose(fp);
  puts("State loaded.");

  if (setcontext(&state->context) == -1) {
    perror("setcontext");
    exit(1);
  }

  puts("this should never happen");
  exit(1);
}

// This is the function that will run on our managed stack
void managed_func(int target_count) {
  int counter = 0;
  for (;;) {
    printf("Counter: %d\n", counter);
    counter++;

    char* junk = (char*)mspace_malloc(state->arena, 5);
    memcpy(junk, "hello", 5);
    printf("allocated up a string at %p\n", (void*)junk);

    if (counter == target_count) {
      printf("Saving state to file %s...\n", state_filename);
      dump_snapshot();
      puts("We came back...");
    }

    sleep(1);
  }
}

void print_usage(const char* program) {
  printf("Usage:\n");
  printf("  Fresh start: %s <count> <filename>\n", program);
  printf("  Restore: %s -r <filename>\n", program);
  exit(1);
}

int main(int argc, char *argv[]) {
  // Parse arguments
  if (argc < 2) print_usage(argv[0]);

  if (strcmp(argv[1], "-r") == 0) {
    if (argc != 3) print_usage(argv[0]);
    state_filename = argv[2];
  } else {
    if (argc != 3) print_usage(argv[0]);
    state_filename = argv[2];
  }

  // Map our memory region
  void *buffer = mmap((void*)MAP_ADDRESS, sizeof(struct program_state),
                      PROT_READ | PROT_WRITE,
                      MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
                      -1, 0);
  if (buffer == MAP_FAILED) {
    perror("mmap");
    return 1;
  }

  state = (struct program_state*)buffer;

  // Check if we're restoring
  if (strcmp(argv[1], "-r") == 0) {
    printf("Restoring from saved state in %s...\n", state_filename);
    load_snapshot(buffer);
  } else {
    // Fresh start
    memset(state, 0, sizeof(struct program_state));
    state->arena = create_mspace_with_base(state->heap, HEAP_SIZE, 1);

    // Initialize the context that will run on our managed stack
    if (getcontext(&state->context) == -1) {
      perror("getcontext");
      return 1;
    }

    // Set up the new context to use our managed stack
    state->context.uc_stack.ss_sp = state->stack;
    state->context.uc_stack.ss_size = STACK_SIZE;

    // Make the context start executing our managed function
    int target_count = atoi(argv[1]);
    makecontext(&state->context, (void (*)())managed_func, 1, target_count);

    printf("Fresh start. Memory mapped at: %p\n", buffer);

    // Switch to our managed stack
    if (setcontext(&state->context) == -1) {
      perror("setcontext");
      return 1;
    }

    puts("somehow we got back here.");
    return 1;
  }
}

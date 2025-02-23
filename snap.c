#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>
#include <ucontext.h>

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

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
  int needs_load;  // Flag to indicate we want to load a snapshot

  char stack[STACK_SIZE];
  char heap[HEAP_SIZE];
};

static struct program_state* state;
static char* state_filename;
static ucontext_t main_context;  // Store the main context

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

  state->has_saved_context = 0;
}

void load_snapshot(void* buffer) {
  puts("opening file...");
  FILE *fp = fopen(state_filename, "rb");
  if (!fp) {
    perror("fopen");
    exit(1);
  }

  puts("reading file...");
  if (fread(buffer, sizeof(struct program_state), 1, fp) != 1) {
    perror("fread");
    fclose(fp);
    exit(1);
  }

  puts("closing file");
  fclose(fp);
  puts("State loaded.");

  state->has_saved_context = 0;

  if (setcontext(&state->context) == -1) {
    perror("setcontext");
    exit(1);
  }

  puts("this should never happen");
  exit(1);
}

static void *lua_dlmalloc_alloc(void *ud, void *ptr, size_t osize, size_t nsize) {
  mspace arena = (mspace)ud;

  if (nsize == 0) {
    // Free memory
    mspace_free(arena, ptr);
    return NULL;
  } else if (ptr == NULL) {
    // Allocate new block
    return mspace_malloc(arena, nsize);
  } else {
    // Resize block
    return mspace_realloc(arena, ptr, nsize);
  }
}

void managed_func() {
  lua_State *L = lua_newstate(lua_dlmalloc_alloc, state->arena);
  luaL_openlibs(L);

  for (;;) {
    printf("Enter Lua code (or !dump / !load): ");

    char buffer[256];
    if (!fgets(buffer, sizeof(buffer), stdin)) {
      perror("fgets");
      continue;
    }

    // Remove newline character if present
    buffer[strcspn(buffer, "\n")] = 0;

    if (strcmp(buffer, "!dump") == 0) {
      printf("Saving state...\n");
      dump_snapshot();
      puts("We came back...");
    } else if (strcmp(buffer, "!load") == 0) {
      printf("Loading state...\n");
      // Set the flag and return to main context
      state->needs_load = 1;
      if (swapcontext(&state->context, &main_context) == -1) {
        perror("swapcontext");
        exit(1);
      }
      // If we get here, the load was successful
      puts("State restored!");
    } else {
      // Execute Lua code
      if (luaL_dostring(L, buffer) != LUA_OK) {
        fprintf(stderr, "Lua error: %s\n", lua_tostring(L, -1));
        lua_pop(L, 1);
      }
    }
  }

  lua_close(L); // Cleanup Lua state
}

void print_usage(const char* program) {
  printf("Usage:\n");
  printf("  Fresh start: %s <filename>\n", program);
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
    if (argc != 2) print_usage(argv[0]);
    state_filename = argv[1];
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
  }

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
  state->context.uc_link = &main_context;  // Return to main context when done

  // Make the context start executing our managed function
  makecontext(&state->context, (void (*)())managed_func, 0);

  printf("Fresh start. Memory mapped at: %p\n", buffer);

  // Main loop - switch between contexts as needed
  while (1) {
    if (swapcontext(&main_context, &state->context) == -1) {
      perror("swapcontext");
      return 1;
    }

    // If we get here, we've returned from the managed context
    if (state->needs_load) {
      load_snapshot(buffer);
      // load_snapshot doesn't return - it switches to the loaded context
    }
  }

  return 0;
}

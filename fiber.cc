// Pieces peeked from Seastar by Cloudius Systems, Ltd.
//
// clang++ -O1 -Wall -std=c++20 -g -fsanitize=address -fno-omit-frame-pointer -lfmt fiber.cc

#include <cassert>
#include <cstdint>
#include <fmt/core.h>
#include <iostream>
#include <memory>
#include <ucontext.h>
#include <vector>
#include <utility>
#include <setjmp.h>

using namespace std;
using namespace fmt;

struct stack_release {
  void operator()(char *ptr) const noexcept { free(ptr); }
};

using stack_ptr = std::unique_ptr<char[], stack_release>;

struct jmp_buf_link {
  jmp_buf jmpbuf;
  jmp_buf_link* link; // link to prev context

public:
  void begin(ucontext_t* initial_context, const void* stack_bottom, size_t stack_size);
  void enter();
  void leave();
  void end();
};

thread_local jmp_buf_link g_unthreaded_context;
thread_local jmp_buf_link* g_current_context;
thread_local jmp_buf_link* g_previous_context;

void init() {
    g_unthreaded_context.link = nullptr;
    g_current_context = &g_unthreaded_context;
}

inline void jmp_buf_link::begin(ucontext_t* initial_context, const void*, size_t) {
    auto prev = std::exchange(g_current_context, this);
    link = prev;
    if (setjmp(prev->jmpbuf) == 0) {
        setcontext(initial_context);
    }
}

inline void jmp_buf_link::enter() {
    auto prev = std::exchange(g_current_context, this);
    link = prev;
    if (setjmp(prev->jmpbuf) == 0) {
        longjmp(jmpbuf, 1);
    }
}

inline void jmp_buf_link::leave() {
    g_current_context = link;
    if (setjmp(jmpbuf) == 0) {
        longjmp(g_current_context->jmpbuf, 1);
    }
}

inline void jmp_buf_link::end() {
    g_current_context = link;
    longjmp(g_current_context->jmpbuf, 1);
}

inline
void throw_system_error_on(bool condition, const char* what_arg) {
  if (condition) {
    if ((errno == EBADF || errno == ENOTSOCK)) {
        abort();
    }
    throw std::system_error(errno, std::system_category(), what_arg);
  }
}

void* alligned_alloc(size_t size, size_t align) {
  void *ret;
  auto r = posix_memalign(&ret, align, size);
  if (r == ENOMEM) {
    throw std::bad_alloc();
  } else if (r == EINVAL) {
    throw std::runtime_error(format("Invalid alignment of {:d}; allocating {:d} bytes", align, size));
  } else {
    assert(r == 0);
    return ret;
  }
}

stack_ptr make_stack(size_t stack_size) {
  const size_t alignment = 16; // ABI requirement on x86_64
  void* mem = aligned_alloc(alignment, stack_size);
  if (mem == nullptr) {
      throw std::bad_alloc();
  }

  auto stack = stack_ptr(new (mem) char[stack_size]);

  // auto mp_status = mprotect(stack.get(), page_size, PROT_READ);
  // assert(mp_status != 0);

  return stack;
}

void t_main() {
#ifdef __x86_64__
    // There is no caller of main() in this context. We need to annotate this frame like this so that
    // unwinders don't try to trace back past this frame.
    // See https://github.com/scylladb/scylla/issues/1909.
    asm(".cfi_undefined rip");
#elif defined(__PPC__)
    asm(".cfi_undefined lr");
#elif defined(__aarch64__)
    asm(".cfi_undefined x30");
#elif defined(__s390x__)
    asm(".cfi_undefined %r14");
#else
    #warning "Backtracing from seastar threads may be broken"
#endif
    try {
        cout << "hi" << endl;
    } catch (...) {
    }
}

void s_main(void) {
  t_main();
}

void setup(jmp_buf_link *ctx, void *stack, size_t stack_size) {
  // use setcontext() for the initial jump, as it allows us
  // to set up a stack, but continue with longjmp() as it's
  // much faster.
  ucontext_t initial_context;

  auto main = reinterpret_cast<void (*)()>(&s_main);
  auto r = getcontext(&initial_context);
  throw_system_error_on(r == -1, "getcontext");

  initial_context.uc_stack.ss_sp = stack;
  initial_context.uc_stack.ss_size = stack_size;
  initial_context.uc_link = nullptr;

  makecontext(&initial_context, main, 0);

  ctx->begin(&initial_context, stack, stack_size);
}

int main() {
  const size_t stack_size = 4 * 4096;
  stack_ptr stack = make_stack(stack_size);
  jmp_buf_link jmp;

  init();
  setup(&jmp, stack.get(), stack_size);

  return 0;
}

#include "exec.h"
#include "prog.h"
#include "st.h"
#include "msg.h"
#include "parse.h"

#include <stdlib.h>
#include <assert.h>
#include <setjmp.h>
#include <string.h>

#define BIT16_LIMIT 65535

/* Execution error return location. */
static jmp_buf exec_env;

/* Errors (some of them are used more than once so
 * they are defined here to avoid different spelling
 * of the same error or something similar). */

#define STACK_UNDERFLOW_ERROR(pos) { \
  perr((pos), "stack underflow");    \
  longjmp(exec_env, EXEC_ERR);       \
}
#define POINTER_SEGMENT_ERROR(addr, pos) {        \
  perrf((pos), "can't access pointer segment at " \
       "`%lu` (max. index is 1)", (addr));        \
  longjmp(exec_env, EXEC_ERR);                    \
}
#define HEAP_ADDR_OVERFLOW_ERROR(instp, addr) { \
  INST_STR(inst_str_buf, (instp));         \
  perrf((instp)->pos, "address overflow: " \
        "`%s` tries to access heap at %lu", \
        inst_str_buf, (addr));             \
  longjmp(exec_env, EXEC_ERR);             \
}
#define STACK_ADDR_OVERFLOW_ERROR(instp, addr, max_addr) { \
  INST_STR(inst_str_buf, (instp));                         \
  perrf((instp)->pos, "stack address overflow: "           \
        "`%s` tries to access stack "                      \
       "at %lu (limit is at %lu)",                         \
        inst_str_buf, (addr), (max_addr));                 \
  longjmp(exec_env, EXEC_ERR);                             \
}
#define SEG_OVERFLOW_ERROR(instp, offset) {                 \
  INST_STR(inst_str_buf, (instp));                          \
  perrf((instp)->pos, "address overflow in `%s`: "          \
        "segment has %lu entries", inst_str_buf, (offset)); \
  longjmp(exec_env, EXEC_ERR);                              \
}
#define ADD_OVERFLOW_ERROR(x, y, sum, pos) {           \
  perrf((pos), "addition overflow: %d + %d = %d > %d", \
    (x), (y), (sum), BIT16_LIMIT);                     \
  longjmp(exec_env, EXEC_ERR);                         \
}
#define SUB_UNDERFLOW_ERROR(x, y, pos) {                  \
  int diff = (int) (x) - (int) (y);                       \
  perrf((pos), "subtraction underflow: %d - %d = %d < 0", \
    (x), (y), diff);                                      \
  longjmp(exec_env, EXEC_ERR);                            \
}
#define CTRL_FLOW_ERROR(ident, pos) {                 \
  if (strcmp((ident), "Sys.init") == 0) {             \
    perr((pos), "can't jump to function `Sys.init`; " \
    "Write it!");                                     \
  } else {                                            \
    perrf((pos), "can't jump to %s",                  \
      (ident));                                       \
  }                                                   \
  longjmp(exec_env, EXEC_ERR);                        \
}
#define NARGS_ERROR(nargs, sp, pos) {                           \
  perrf((pos), "given number of stack arguments (%d) is wrong." \
    " There are only %lu elements on the stack!",               \
    (nargs), (sp));                                             \
  longjmp(exec_env, EXEC_ERR);                                  \
}
#define READ_IO_ERROR(pos) {               \
  perr((pos), "system read failed."); \
  longjmp(exec_env, EXEC_ERR);             \
}
#define DEF_ERR(key, pos) {                        \
  perrf((pos), "can't jump to %s %s because it's " \
    "defined multiple times",                      \
    key_type_name((key).type), (key).ident);       \
  longjmp(exec_env, EXEC_ERR);                     \
}
#define READ_NUM_CHAR_ERROR(pos) {             \
  perr((pos), "invalid input, `Sys.read_num` " \
    "only accepts digits.");                   \
  longjmp(exec_env, EXEC_ERR);                 \
}
#define READ_NUM_OVERFLOW_ERROR(pos, num) {               \
  perrf((pos), "number %d read by `Sys.read_num` "        \
    "is too large. The limit is %d", (num), BIT16_LIMIT); \
  longjmp(exec_env, EXEC_ERR);                            \
}

void exec_pop(Inst inst, Stack* stack, Heap* heap, Memory* mem) {
  assert(stack != NULL);
  assert(mem != NULL);

  size_t offset = inst.mem.offset;

  switch(inst.mem.seg) {
    case ARG:
      if (
        offset < stack->arg_len &&
        // `offset < stack->arg_len` implicitly
        // means that `offset + stack->arg < stack->sp`
        // (same with loc).
        offset + stack->arg < stack->sp
      ) {
        Word arg_buf;
        if (!spop(stack, &arg_buf))
          STACK_UNDERFLOW_ERROR(inst.pos);
        stack->ops[offset + stack->arg] = arg_buf;
      } else {
        if (offset >= stack->arg_len) {
          SEG_OVERFLOW_ERROR(&inst, stack->arg_len);
        } else {
          STACK_ADDR_OVERFLOW_ERROR(&inst, offset + stack->arg, stack->sp);
        }
      }
      break;
    case LOC:
      if (
        offset < stack->lcl_len &&
        offset + stack->lcl < stack->sp
      ) {
        Word lcl_buf;
        if (!spop(stack, &lcl_buf))
          STACK_UNDERFLOW_ERROR(inst.pos);
        stack->ops[offset + stack->lcl] = lcl_buf;
      } else {
        if (offset >= stack->lcl_len) {
          SEG_OVERFLOW_ERROR(&inst, stack->lcl_len);
        } else {
          STACK_ADDR_OVERFLOW_ERROR(&inst, offset + stack->arg, stack->sp);
        }
      }
      break;
    case STAT:
      if (offset < MEM_STAT_SIZE) {
        if (!spop(stack, &mem->_static[offset]))
          STACK_UNDERFLOW_ERROR(inst.pos);
      } else {
        SEG_OVERFLOW_ERROR(&inst, MEM_STAT_SIZE);
      }
      break;
    case CONST: {
        // `pop`ping to constant deletes the value.
        Word val;
        if (!spop(stack, &val))
          STACK_UNDERFLOW_ERROR(inst.pos);
      }
      break;
    case THIS:
      if (offset + heap->_this <= MEM_HEAP_SIZE) {
        // If we land here, then `offset + heap->_this` fits
        // a `uint16_t`.
        Word val;
        if (!spop(stack, &val)) STACK_UNDERFLOW_ERROR(inst.pos);
        heap_set(*heap, (Addr)(offset + heap->_this), val);
      } else {
        HEAP_ADDR_OVERFLOW_ERROR(&inst, offset + heap->_this);
      }
      break;
    case THAT:
      if (offset + heap->that <= MEM_HEAP_SIZE) {
        Word val;
        if (!spop(stack, &val)) STACK_UNDERFLOW_ERROR(inst.pos);
        heap_set(*heap, (Addr)(offset + heap->that), val);
      } else {
        HEAP_ADDR_OVERFLOW_ERROR(&inst, offset + heap->that);
      }
      break;
    case PTR:
      if (offset == 0) {
        if (!spop(stack, (Word*) &heap->_this))
          STACK_UNDERFLOW_ERROR(inst.pos);
      } else if (offset == 1) {
        if (!spop(stack, (Word*) &heap->that))
          STACK_UNDERFLOW_ERROR(inst.pos);
      } else {
        POINTER_SEGMENT_ERROR(offset, inst.pos);
      }
      return;
    case TMP:
      if (offset < MEM_TEMP_SIZE) {
        if (!spop(stack, &mem->tmp[offset]))
          STACK_UNDERFLOW_ERROR(inst.pos);
      } else {
        SEG_OVERFLOW_ERROR(&inst, MEM_TEMP_SIZE)
      }
      break;
  }
}

void exec_push(Inst inst, Stack* stack, Heap* heap, Memory* mem) {
  assert(stack != NULL);
  assert(heap != NULL);
  assert(mem != NULL);
  
  size_t offset = inst.mem.offset;

  switch(inst.mem.seg) {
    case ARG:
      if (
        offset < stack->arg_len &&
        offset + stack->arg < stack->sp
      ) {
        spush(stack, stack->ops[offset + stack->arg]);
      } else {
        if (offset >= stack->arg_len) {
          SEG_OVERFLOW_ERROR(&inst, stack->arg_len);
        } else {
          STACK_ADDR_OVERFLOW_ERROR(&inst, offset + stack->arg, stack->sp);
        }
      }
      break;
    case LOC:
      if (
        offset < stack->lcl_len &&
        offset + stack->lcl < stack->sp
      ) {
        spush(stack, stack->ops[offset + stack->lcl]);
      } else {
        if (offset >= stack->lcl_len) {
          SEG_OVERFLOW_ERROR(&inst, stack->lcl_len);
        } else {
          STACK_ADDR_OVERFLOW_ERROR(&inst, offset + stack->arg, stack->sp);
        }
      }
      break;
    case STAT:
      if (offset < MEM_STAT_SIZE) {
        spush(stack, mem->_static[offset]);
      } else {
        SEG_OVERFLOW_ERROR(&inst, MEM_STAT_SIZE);
      }
      break;
    case CONST:
      // The `constant` segment is a pseudo segment
      // used to get the constant value of `offset`.
      spush(stack, (Word) inst.mem.offset);  // `Word` is `uint16_t`.
      return;
    case THIS:
      if (offset + heap->_this <= MEM_HEAP_SIZE) {
        spush(stack, heap_get(*heap, (Addr)(offset + heap->_this)));
      } else {
        HEAP_ADDR_OVERFLOW_ERROR(&inst, offset + heap->_this);        
      }
      break;
    case THAT:
      if (offset + heap->that <= MEM_HEAP_SIZE) {
        spush(stack, heap_get(*heap, (Addr)(offset + heap->that)));
      } else {
        HEAP_ADDR_OVERFLOW_ERROR(&inst, offset + heap->that);        
      }
      break;
    case PTR:
      // `pointer` isn't  really a segment but is instead
      // used to the the addresses of the `this` and `that`
      // segments.
      if (offset == 0) {
        assert(heap->_this <= MEM_HEAP_SIZE);
        spush(stack, (Word) heap->_this);
      } else if (offset == 1) {
        assert(heap->that <= MEM_HEAP_SIZE);
        spush(stack, (Word) heap->that);
      } else {
        POINTER_SEGMENT_ERROR(offset, inst.pos);
      }
      return;
    case TMP:
      if (offset < MEM_TEMP_SIZE) {
        spush(stack, mem->tmp[offset]);
      } else {
        SEG_OVERFLOW_ERROR(&inst, MEM_TEMP_SIZE)
      }
      break;
  }
}

// Extended word to allow buffering
// and checking if overflows occured
// on itermediate results.
typedef uint32_t Wordbuf;

static inline void exec_add(Stack* stack, Pos pos) {
  assert(stack != NULL);

  Word y;
  if (!spop(stack, &y))
    STACK_UNDERFLOW_ERROR(pos);
  Word x;
  if (!spop(stack, &x))
    STACK_UNDERFLOW_ERROR(pos);
  Wordbuf sum = (Wordbuf) x + (Wordbuf) y;

  if (sum <= BIT16_LIMIT) {
    spush(stack, (Word) sum);
  } else {
    // Since `spop` doesn't delete anything,
    // this resets the stack to the state
    // before attempting the add.
    stack->sp += 2;
    ADD_OVERFLOW_ERROR(x, y, sum, pos);
  }
}

static inline void exec_sub(Stack* stack, Pos pos) {
  assert(stack != NULL);

  Word y;
  if (!spop(stack, &y))
    STACK_UNDERFLOW_ERROR(pos);
  Word x;
  if (!spop(stack, &x))
    STACK_UNDERFLOW_ERROR(pos);

  if (x >= y) {
    spush(stack, x - y);
  } else {
    stack->sp += 2;  // Restore `x` and `y`.
    SUB_UNDERFLOW_ERROR(x, y, pos);
  }
}

static inline void exec_neg(Stack* stack, Pos pos) {
  assert(stack != NULL);

  Word y;
  if (!spop(stack, &y))
    STACK_UNDERFLOW_ERROR(pos);
  // Two's complement negation.
  y = ~y;
  y += 1;
  spush(stack, y);
}

static inline void exec_and(Stack* stack, Pos pos) {
  assert(stack != NULL);

  Word y;
  if (!spop(stack, &y))
    STACK_UNDERFLOW_ERROR(pos);
  Word x;
  if (!spop(stack, &x))
    STACK_UNDERFLOW_ERROR(pos);

  spush(stack, x & y);
}

static inline void exec_or(Stack* stack, Pos pos) {
  assert(stack != NULL);

  Word y;
  if (!spop(stack, &y))
    STACK_UNDERFLOW_ERROR(pos);
  Word x;
  if (!spop(stack, &x))
    STACK_UNDERFLOW_ERROR(pos);

  spush(stack, x | y);
}

static inline void exec_not(Stack* stack, Pos pos) {
  assert(stack != NULL);

  Word y;
  if (!spop(stack, &y))
    STACK_UNDERFLOW_ERROR(pos);
  spush(stack, ~y);
}

/* NOTE: Boolean operations return 0xFFFF (-1)
 * if the result is true. Otherwise they return
 * 0x0000 (false). Any value which is not 0x0000
 * is interpreted as being true.
 */

# define TRUE 0xFFFF
# define FALSE 0

static inline void exec_eq(Stack* stack, Pos pos) {
  assert(stack != NULL);

  Word y;
  if (!spop(stack, &y))
    STACK_UNDERFLOW_ERROR(pos);
  Word x;
  if (!spop(stack, &x))
    STACK_UNDERFLOW_ERROR(pos);

  spush(stack, x == y ? TRUE : FALSE);
}

static inline void exec_lt(Stack* stack, Pos pos) {
  assert(stack != NULL);

  Word y;
  if (!spop(stack, &y))
    STACK_UNDERFLOW_ERROR(pos);
  Word x;
  if (!spop(stack, &x))
    STACK_UNDERFLOW_ERROR(pos);

  spush(stack, x < y ? TRUE : FALSE);
}

static inline void exec_gt(Stack* stack, Pos pos) {
  assert(stack != NULL);

  Word y;
  if (!spop(stack, &y))
    STACK_UNDERFLOW_ERROR(pos);
  Word x;
  if (!spop(stack, &x))
    STACK_UNDERFLOW_ERROR(pos);

  spush(stack, x > y ? TRUE : FALSE);
}

/* Get the currently active file */
#define active_file(prog) (prog->files[prog->fi])

/* Get current instruction */
#define active_inst(prog) (prog->files[prog->fi].insts.cell[prog->files[prog->fi].ei])

#define JMP_OK 1
#define JMP_ERR 0
#define JMP_MULT_DEF -1

static int jump_to(Program* prog, SymKey key, SymVal* val) {
  assert(prog != NULL);
  assert(val != NULL);

  GetResult get_res = get_st(active_file(prog).st, &key, val);

  if (get_res == GTRES_OK) {
    /* Change instruction in active file. */
    active_file(prog).ei = val->inst_addr - 1;
    return JMP_OK;
  } else {
    unsigned int prev_fi = prog->fi;
    unsigned int next_ei_buf = 0;
    unsigned int next_fi_buf = 0;
    unsigned int ndefs = 0;
    for (unsigned int next_fi = 0; next_fi < prog->nfiles; next_fi++) {
      /* Don't re-check the active file. */
      if (prev_fi != next_fi) {
        if (get_st(prog->files[next_fi].st, &key, val) == GTRES_OK) {
          /* Copy new position values for later. */
          next_fi_buf = next_fi;
          next_ei_buf = val->inst_addr - 1;
          /* Continue if we just found the first definition
           * or break if there are multiple definitions. */
          ndefs++;
          if (ndefs > 1) break;
        }
      }
    }

    /* How many definitions did we find? */
    switch (ndefs) {
      case 0:
        /* The function doesn't exist anywhere. */
        return JMP_ERR;
      case 1:
        /* We found the one definition we need.
         * Go there! */
        prog->fi             = next_fi_buf;
        active_file(prog).ei = next_ei_buf;
        return JMP_OK;
      default:
        /* There is more than one definition. */
        return JMP_MULT_DEF;
    }
  }
}

static inline void exec_goto(Program* prog, Pos pos) {
  assert(prog != NULL);

  SymVal val;
  SymKey key = mk_key(
    active_file(prog).insts.cell[active_file(prog).ei].ident,
    SBT_LABEL
  );

  switch (jump_to(prog, key, &val)) {
    case JMP_ERR:
      CTRL_FLOW_ERROR(key.ident, pos);
      break;
    case JMP_MULT_DEF:
      DEF_ERR(key, pos);
      break;
    default:
      /* Else: everything went well. */
      break;
  }
}

static inline void exec_if_goto(Program* prog, Pos pos) {
  assert(prog != NULL);

  Word val;
  if (!spop(&prog->stack, &val))
    STACK_UNDERFLOW_ERROR(pos);

  /* Jump if topmost value is true. */

  if (val != FALSE) {
    SymVal val;
    SymKey key = mk_key(
      active_file(prog).insts.cell[active_file(prog).ei].ident,
      SBT_LABEL
    );

    /* `val` is restored on error. */
    switch (jump_to(prog, key, &val)) {
      case JMP_ERR:
        prog->stack.sp ++;
        CTRL_FLOW_ERROR(key.ident, pos);
        break;
      case JMP_MULT_DEF:
        prog->stack.sp ++;
        DEF_ERR(key, pos);
        break;
      default:
        /* Else: everything went well. */
        break;
    }
  }
}

static inline void exec_call(Program* prog, Pos pos) {
  assert(prog != NULL);

  const char* ident = active_file(prog).insts.cell[active_file(prog).ei].ident;
  Word nargs = active_file(prog).insts.cell[active_file(prog).ei].nargs;
  Stack* stack = &prog->stack;
  Heap* heap = &prog->heap;

  Addr ret_ei = active_file(prog).ei;
  Addr ret_fi = prog->fi;

  if (nargs > stack->sp)
    NARGS_ERROR(nargs, stack->sp, pos);

  SymVal val;
  SymKey key = mk_key(ident, SBT_FUNC);
  switch (jump_to(prog, key, &val)) {
    case JMP_ERR:
      CTRL_FLOW_ERROR(key.ident, pos);
      break;
    case JMP_MULT_DEF:
      DEF_ERR(key, pos);
      break;
    default:
      /* Else: everything went well. */
      break;
  }

  // Push return execution index on the stack.
  spush(stack, (Word) ret_ei);
  // Push the return file index on the stack.
  spush(stack, (Word) ret_fi);
  // Push caller's `LCL` on stack.
  spush(stack, (Word) stack->lcl);
  spush(stack, (Word) stack->lcl_len);
  // Push caller's `ARG` on stack.
  spush(stack, (Word) stack->arg);
  spush(stack, (Word) stack->arg_len);
  // Push caller's `THIS` on stack.
  spush(stack, (Word) heap->_this);
  // Push caller's `THAT` on stack.
  spush(stack, (Word) heap->that);

  /* Set `ARG` for new function.
   * `8` accounts for the return address etc. on stack.
   * `nargs` is the number of arguments assumed are
   * on stack right now (according to the `call` invocation).
   * `nargs` is checked at the beginning of this function to
   * avoid underflows here.
   */
  stack->arg = stack->sp - 8 - nargs;
  stack->arg_len = nargs;

  // Set `LCL` for new function and allocate locals.
  stack->lcl = stack->sp;
  stack->lcl_len = val.nlocals;
  for (size_t i = 0; i < stack->lcl_len; i++)
    spush(stack, 0);

  // Now we're ready to jump to the start of the function.
  active_file(prog).ei = val.inst_addr - 1;
}

void exec_ret(Program* prog, Pos pos) {
  assert(prog != NULL);

  Stack* stack = &prog->stack;
  Heap* heap = &prog->heap;

  // `LCL` always points to the stack position
  // right after all of the caller's segments
  // etc. have been pushed.
  Addr frame = stack->lcl;
  // Execution index and file index were pushed
  // first in this sequence.
  Addr ret_ei = stack->ops[frame - 8];
  Addr ret_fi = stack->ops[frame - 7];

  // `ARG` always points to the first argument
  // pushed on the stack by the caller. This is
  // where the caller will expect the return value.
  // We need to get the return value as a two step
  // operation because `spop` might change the location
  // of `stack->ops` which would result in a use after
  // free error if a pointer to a value on the stack
  // were passed to `spop` as `val`.
  Word ret_val;
  if (!spop(stack, &ret_val)) {
    STACK_UNDERFLOW_ERROR(pos);
  }
  // Insert the return value at the position
  // where the caller will expect it.
  stack->ops[stack->arg] = ret_val;

  stack->sp = stack->arg + 1;
  // Restore the rest of the registers which
  // have been pushed on stack.
  heap ->that    = stack->ops[frame - 1];
  heap ->_this   = stack->ops[frame - 2];
  stack->arg_len = stack->ops[frame - 3];
  stack->arg     = stack->ops[frame - 4];
  stack->lcl_len = stack->ops[frame - 5];
  stack->lcl     = stack->ops[frame - 6];

  /* Jump ! */

  prog->fi = ret_fi;
  // Don't subtract here (see `exec_call` and `exec_goto`)!
  prog->files[prog->fi].ei = ret_ei;
}

static inline void exec_builtin_print_char(Stack* stack, Pos pos) {
  assert(stack != NULL);

  Word val;
  if (!spop(stack, &val))
    STACK_UNDERFLOW_ERROR(pos);

  hvme_fprintf(stdout, "%c", (char) val);
}

static inline void exec_builtin_print_num(Stack* stack, Pos pos) {
  assert(stack != NULL);

  Word val;
  if (!spop(stack, &val))
    STACK_UNDERFLOW_ERROR(pos);

  hvme_fprintf(stdout, "%d", val);
}

static inline void exec_builtin_print_str(Program* prog, Pos pos) {
  assert(prog != NULL);

  Addr str_start;
  if (!spop(&prog->stack, (Word*) &str_start))
    STACK_UNDERFLOW_ERROR(pos);
  Word nchars;
  if (!spop(&prog->stack, &nchars))
    STACK_UNDERFLOW_ERROR(pos);

  for (Addr i = 0; i < nchars; i++) {
    hvme_fprintf(stdout, "%c",
      (char) heap_get(prog->heap, str_start + i));
  }
}

static inline void exec_builtin_read_char(Stack* stack) {
  assert(stack != NULL);

  Word ch = getchar();
  spush(stack, ch);
}

static inline void exec_builtin_read_num(Stack* stack, Pos pos) {
  assert(stack != NULL);

  unsigned int num_buf;
  int res = scanf("%u", &num_buf);
  if (res == EOF) {
    READ_IO_ERROR(pos);
  } else if (res == 0) {
    // Input was invalid and nothing was read.
    // This consumes the rest of the line.
    char c = fgetc(stdin);
    while (c != '\n') {
      c = fgetc(stdin);
    }
    READ_NUM_CHAR_ERROR(pos);
  }

  if (num_buf > BIT16_LIMIT) {
    READ_NUM_OVERFLOW_ERROR(pos, num_buf);
  } else {
    spush(stack, (Word) num_buf);
  }
}

static inline void exec_builtin_read_str(Program* prog, Pos pos) {
  assert(prog != NULL);

  Word heap_addr;
  if (!spop(&prog->stack, &heap_addr))
    STACK_UNDERFLOW_ERROR(pos);

  char* buf = NULL;
  size_t len = 0;
  ssize_t nread_buf = 0;

  if ((nread_buf = getline(&buf, &len, stdin)) == -1) {
    free(buf);
    READ_IO_ERROR(pos);
  }

  // Cast is OK because `-1` was checked.
  // `getline` always includes the delimiter ('\n')
  // which we want to remove. This means that while
  // the minimum number of characters will be one, we
  // have to decrease it by one.

  assert(nread_buf >= 1);
  size_t nread = nread_buf - 1;

  if (heap_addr + nread > MEM_HEAP_SIZE) {
    free(buf);
    HEAP_ADDR_OVERFLOW_ERROR(&active_inst(prog), heap_addr + nread);
  }

  /* `memcpy` doesn't work here because we read
   * `char`s which we must store as `Word`s. */
  for (unsigned int i = 0; i < nread; i++) {
    prog->heap.mem[heap_addr + i] = (Word) buf[i];
  }

  free(buf);

  spush(&prog->stack, (Word) nread);
}

int exec_prog(Program* prog) {
  assert(prog != NULL);

  int arrive = setjmp(exec_env);
  if (arrive == EXEC_ERR)
    return EXEC_ERR;

  /* Reaching the end of any file is enough to end execution.
   * `insts.idx` points to the next unused instruction field
   * in the instruction buffer from parsing. Thus it can be
   * used here as the number of instructions in the buffer. */

  for (; active_file(prog).ei < active_file(prog).insts.idx; active_file(prog).ei ++) {
    switch(active_inst(prog).code) {
      case POP:
        exec_pop(
          active_inst(prog),
          &prog->stack,
          &prog->heap,
          &active_file(prog).mem
        );
        break;
      case PUSH:
        exec_push(
          active_inst(prog),
          &prog->stack,
          &prog->heap,
          &active_file(prog).mem
        );
        break;
      case ADD:
        exec_add(&prog->stack, active_inst(prog).pos);
        break;
      case SUB:
        exec_sub(&prog->stack, active_inst(prog).pos);
        break;
      case NEG:
        exec_neg(&prog->stack, active_inst(prog).pos);
        break;
      case AND:
        exec_and(&prog->stack, active_inst(prog).pos);
        break;
      case OR:
        exec_or(&prog->stack, active_inst(prog).pos);
        break;
      case NOT:
        exec_not(&prog->stack, active_inst(prog).pos);
        break;
      case EQ:
        exec_eq(&prog->stack, active_inst(prog).pos);
        break;
      case LT:
        exec_lt(&prog->stack, active_inst(prog).pos);
        break;
      case GT:
        exec_gt(&prog->stack, active_inst(prog).pos);
        break;
      case GOTO:
        exec_goto(prog, active_inst(prog).pos);
        break;
      case IF_GOTO:
        exec_if_goto(prog, active_inst(prog).pos);
        break;
      case CALL:
        exec_call(prog, active_inst(prog).pos);
        break;
      case RET:
        exec_ret(prog, active_inst(prog).pos);
        break;
      case BUILTIN_PRINT_CHAR:
        exec_builtin_print_char(&prog->stack, active_inst(prog).pos);
        break;
      case BUILTIN_PRINT_NUM:
        exec_builtin_print_num(&prog->stack, active_inst(prog).pos);
        break;
      case BUILTIN_PRINT_STR:
        exec_builtin_print_str(prog, active_inst(prog).pos);
        break;
      case BUILTIN_READ_CHAR:
        exec_builtin_read_char(&prog->stack);
        break;
      case BUILTIN_READ_NUM:
        exec_builtin_read_num(&prog->stack, active_inst(prog).pos);
        break;
      case BUILTIN_READ_STR:
        exec_builtin_read_str(prog, active_inst(prog).pos);
        break;
      default: {
        INST_STR(str, &active_inst(prog));
        perrf(active_inst(prog).pos,
          "invalid inststruction `%s`; programmer mistake", str);
        return EXEC_ERR;
      }
    }
  }

  return 0;
}

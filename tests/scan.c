#define MUNIT_ENABLE_ASSERT_ALIASES
#include "munit.h"

#include <stdio.h>

#include "../src/scan.h"
#include "utils.h"

/* Internal scan function */
extern ssize_t scan_blk(Tokens* tokens, const char* blk, size_t len);

#define TEST_SCAN_TOKEN(name, lit, token)           \
TEST(name) {                                        \
  Tokens tokens = new_tokens(NULL);                    \
  char* blk = lit;                                  \
  ssize_t res = scan_blk(&tokens, blk, strlen(blk)); \
  assert_int(res, ==, 0);                           \
  assert_int(tokens.cell[0].t, ==, token);           \
  TOKEN_STR(str, &tokens.cell[0]);                    \
  assert_string_equal(str, blk);                    \
  del_tokens(tokens);                                 \
  return MUNIT_OK;                                  \
}

TEST_SCAN_TOKEN(scan_push, "push", TK_PUSH)
TEST_SCAN_TOKEN(scan_pop, "pop", TK_POP)
TEST_SCAN_TOKEN(scan_argument, "argument", TK_ARG)
TEST_SCAN_TOKEN(scan_local, "local", TK_LOC)
TEST_SCAN_TOKEN(scan_static, "static", TK_STAT)
TEST_SCAN_TOKEN(scan_constant, "constant", TK_CONST)
TEST_SCAN_TOKEN(scan_this, "this", TK_THIS)
TEST_SCAN_TOKEN(scan_that, "that", TK_THAT)
TEST_SCAN_TOKEN(scan_pointer, "pointer", TK_PTR)
TEST_SCAN_TOKEN(scan_temp, "temp", TK_TMP)
TEST_SCAN_TOKEN(scan_add, "add", TK_ADD)
TEST_SCAN_TOKEN(scan_sub, "sub", TK_SUB)
TEST_SCAN_TOKEN(scan_neg, "neg", TK_NEG)
TEST_SCAN_TOKEN(scan_eq, "eq", TK_EQ)
TEST_SCAN_TOKEN(scan_gt, "gt", TK_GT)
TEST_SCAN_TOKEN(scan_lt, "lt", TK_LT)
TEST_SCAN_TOKEN(scan_and, "and", TK_AND)
TEST_SCAN_TOKEN(scan_or, "or", TK_OR)
TEST_SCAN_TOKEN(scan_not, "not", TK_NOT)
TEST_SCAN_TOKEN(scan_label, "label", TK_LABEL)
TEST_SCAN_TOKEN(scan_goto, "goto", TK_GOTO)
TEST_SCAN_TOKEN(scan_if_goto, "if-goto", TK_IF_GOTO)
TEST_SCAN_TOKEN(scan_function, "function", TK_FUNC)
TEST_SCAN_TOKEN(scan_call, "call", TK_CALL)
TEST_SCAN_TOKEN(scan_return, "return", TK_RET)

TEST(scan_each_num) {
  // Accept all 65536 valid numbers  (0 - 65535).
  char blk[7];
  for (uint16_t i = 0; i < 65535; i++) {
    Tokens tokens = new_tokens(NULL);
    strncpy(blk, "      ", 7);  // Reset to whitespace.
    snprintf(blk, 7, "%d ", i);  // Write the current number to the string.
    ssize_t res = scan_blk(&tokens, blk, strlen(blk));
    assert_int(res, ==, 0);
    assert_int(tokens.cell[0].t, ==, TK_UINT);
    assert_int(tokens.cell[0].uilit, ==, i);
    del_tokens(tokens);
  }

  return MUNIT_OK;
}

TEST(truncate_idents) {
  //        This 25th character should be truncated.
  //                                               |
  char* label_blk = "label abstractachievedaccuracy1\n";
  Tokens tokens = new_tokens(NULL);
  ssize_t res =  scan_blk(&tokens, label_blk, strlen(label_blk));
  assert_int(res, ==, 0);  // Input should still be accepted.
  assert_string_equal(tokens.cell[1].ident, "abstractachievedaccuracy");
  assert_int(check_stream("`abstractachievedaccuracy1` is too long to be an identifier", 20, stderr), ==, 1);
  del_tokens(tokens);

  return MUNIT_OK;
}

TEST(eat_ws) {
  Tokens tokens = new_tokens(NULL);
  char* blk = " \t\n push \t \n pop  \n";
  ssize_t res = scan_blk(&tokens, blk, strlen(blk));
  assert_int(res, ==, 0);
  assert_int(tokens.cell[0].t, ==, TK_PUSH);
  assert_int(tokens.cell[1].t, ==, TK_POP);
  del_tokens(tokens);
  return MUNIT_OK;
}

TEST(eat_comments) {
  char* blk = 
    "// This is a test to see if comments work.\n"
    "// Here we have two lines, both of which are comments.\n"
    "push constant 1 // Wow this line is some real code!\n"
    "// More comments ...\n"
    "push constant 2 // <- More code.\n";

  Tokens tokens = new_tokens(NULL);
  ssize_t res = scan_blk(&tokens, blk, strlen(blk));
  assert_int(res, ==, 0);
  assert_int(tokens.cell[0].t, ==, TK_PUSH);
  assert_int(tokens.cell[1].t, ==, TK_CONST);
  assert_int(tokens.cell[2].t, ==, TK_UINT);
  assert_int(tokens.cell[3].t, ==, TK_PUSH);
  assert_int(tokens.cell[4].t, ==, TK_CONST);
  assert_int(tokens.cell[5].t, ==, TK_UINT);
  del_tokens(tokens);

  return MUNIT_OK;
}

TEST(find_num_remaining) {
  Tokens tokens = new_tokens(NULL);
  // `scan` should return `2` to signal that the last
  // two characters need to be copied to the start of the next block.
  char* blk = "push\npop\npop\npu";
  ssize_t res = scan_blk(&tokens, blk, strlen(blk));
  assert_int(res, ==, 2);
  assert_int(tokens.cell[0].t, ==, TK_PUSH);
  assert_int(tokens.cell[1].t, ==, TK_POP);
  assert_int(tokens.cell[2].t, ==, TK_POP);
  del_tokens(tokens);

  return MUNIT_OK;
}

TEST(scan_along_block_borders) {
  assert_int(SCAN_BLOCK_SIZE, ==, MAX_TOKEN_LEN);
  {  // Scanning normal tokens over borders works.
    char fn[] = "/tmp/XXXXXX";
    // Start of next block (`TK_SCAN_BLOCK_SIZE` is 8 for unit tests).
    //                           |
    setup_tmp(fn, "pop  \npush\npush\npop\n");
    Tokens tokens = new_tokens(fn);
    int scan_res = scan(&tokens);
    assert_int(scan_res, ==, SCAN_OK);
    assert_int(tokens.cell[0].t, ==, TK_POP);
    assert_int(tokens.cell[1].t, ==, TK_PUSH);
    assert_int(tokens.cell[2].t, ==, TK_PUSH);
    del_tokens(tokens);
  }
  {  // Newline warning aren't emitted if
     // the content ends with a newline.
    char fn[] = "/tmp/XXXXXX";
    setup_tmp(fn, "pop\n");
    Tokens tokens = new_tokens(fn);
    int scan_res = scan(&tokens);
    assert_int(scan_res, ==, SCAN_OK);
    del_tokens(tokens);
    // Check that `stderr` doesn't contain a warning.
    assert_int(check_stream("Warn:", 5, stderr), ==, 0);
  }
  {  // Scanning numbers over borders works.
    char fn[] = "/tmp/XXXXXX";
    setup_tmp(fn, "pop  \n48907\npush");
    Tokens tokens = new_tokens(fn);
    int scan_res = scan(&tokens);
    assert_int(scan_res, ==, SCAN_OK);
    assert_int(tokens.cell[0].t, ==, TK_POP);
    assert_int(tokens.cell[1].t, ==, TK_UINT);
    assert_int(tokens.cell[1].uilit,  ==, 48907);
    assert_int(tokens.cell[2].t, ==, TK_PUSH);
    del_tokens(tokens);
  }
  // Numbers without whitespace at end works.
  {
    char fn[] = "/tmp/XXXXXX";
    setup_tmp(fn, "pop  \npush\n48907");
    Tokens tokens = new_tokens(fn);
    int scan_res = scan(&tokens);
    assert_int(scan_res, ==, SCAN_OK);
    assert_int(tokens.cell[0].t, ==, TK_POP);
    assert_int(tokens.cell[1].t, ==, TK_PUSH);
    assert_int(tokens.cell[2].t, ==, TK_UINT);
    assert_int(tokens.cell[2].uilit,  ==, 48907);
    del_tokens(tokens);
  }
  {
    // This test case also check the behaviour of the
    // scanner when a newline is automatically inserted
    // so that an incompleted block (7 chars) becomes a
    // complete block (8 chars). If this happens the
    // scanner should still exit successfully.
    char fn[] = "/tmp/XXXXXX";
    setup_tmp(fn, "pop  \npush\n48");
    Tokens tokens = new_tokens(fn);
    int scan_res = scan(&tokens);
    assert_int(scan_res, ==, SCAN_OK);
    assert_int(tokens.cell[0].t, ==, TK_POP);
    assert_int(tokens.cell[1].t, ==, TK_PUSH);
    assert_int(tokens.cell[2].t, ==, TK_UINT);
    assert_int(tokens.cell[2].uilit,  ==, 48);
    del_tokens(tokens);
  }

  return MUNIT_OK;
}

TEST(eat_comments_with_blocks) {
  char fn[] = "/tmp/XXXXXX";
  setup_tmp(fn,
    "// This is a test to see if comments work.\n"
    "// Here we have two lines, both of which are comments.\n"
    "push constant 1 // Wow this line is some real code!\n"
    "// More comments ...\n"
    "push constant 2 // <- More code.\n");

  Tokens tokens = new_tokens(fn);
  int scan_res = scan(&tokens);
  assert_int(scan_res, ==, SCAN_OK);
  assert_int(tokens.cell[0].t, ==, TK_PUSH);
  assert_int(tokens.cell[1].t, ==, TK_CONST);
  assert_int(tokens.cell[2].t, ==, TK_UINT);
  assert_int(tokens.cell[3].t, ==, TK_PUSH);
  assert_int(tokens.cell[4].t, ==, TK_CONST);
  assert_int(tokens.cell[5].t, ==, TK_UINT);
  del_tokens(tokens);

  return MUNIT_OK;
}

TEST(realloc_tokens_array) {
  {  // Reallocate array of insufficient size.
    Tokens tokens = new_tokens(NULL);
    // Shrink the cell size.
    tokens.len = 2;
    tokens.cell = (Token*) realloc (tokens.cell, tokens.len * sizeof(Token));
    assert(tokens.cell != NULL);

    char* blk = "pop\npop\npop\npop";
    ssize_t res = scan_blk(&tokens, blk, strlen(blk));
    assert_int(res, ==, 0);
    // `ididx` is four instead of three (which would be
    // the expected zero-based index) because it already
    // points to the next index.
    assert_int(tokens.idx, ==, 4);
    assert_int(tokens.len, ==, TOKEN_BLOCK_SIZE + 2);
    for (size_t i = 0; i < tokens.idx; i++) {
      assert_int(tokens.cell[i].t, ==, TK_POP);
    }
    del_tokens(tokens);
  }
  {  // Reallocate empty array
    Tokens tokens = new_tokens(NULL);
    tokens.len = 0;
    tokens.cell = (Token*) realloc (tokens.cell, tokens.len * sizeof(Token));
    assert(tokens.cell == NULL);  // `NULL` since array is empty.

    char* blk = "pop\npop\npop\npop";
    ssize_t res = scan_blk(&tokens, blk, strlen(blk));
    assert_int(res, ==, 0);
    assert_int(tokens.idx, ==, 4);
    assert_int(tokens.len, ==, TOKEN_BLOCK_SIZE);
    for (size_t i = 0; i < tokens.idx; i++) {
      assert_int(tokens.cell[i].t, ==, TK_POP);
    }
    del_tokens(tokens);
  }

  return MUNIT_OK;
}

MunitTest scan_tests[] = {
  REG_TEST(scan_push),
  REG_TEST(scan_pop),
  REG_TEST(scan_argument),
  REG_TEST(scan_local),
  REG_TEST(scan_static),
  REG_TEST(scan_constant),
  REG_TEST(scan_this),
  REG_TEST(scan_that),
  REG_TEST(scan_pointer),
  REG_TEST(scan_temp),
  REG_TEST(scan_add),
  REG_TEST(scan_sub),
  REG_TEST(scan_neg),
  REG_TEST(scan_eq),
  REG_TEST(scan_gt),
  REG_TEST(scan_lt),
  REG_TEST(scan_and),
  REG_TEST(scan_or),
  REG_TEST(scan_not),
  REG_TEST(scan_label),
  REG_TEST(scan_goto),
  REG_TEST(scan_function),
  REG_TEST(scan_call),
  REG_TEST(scan_return),
  REG_TEST(scan_if_goto),
  REG_TEST(truncate_idents),
  REG_TEST(scan_each_num),
  REG_TEST(eat_ws),
  REG_TEST(eat_comments),
  REG_TEST(find_num_remaining),
  REG_TEST(scan_along_block_borders),
  REG_TEST(eat_comments_with_blocks),
  REG_TEST(realloc_tokens_array),
  { NULL, NULL, NULL, NULL, MUNIT_TEST_OPTION_NONE, NULL }
};

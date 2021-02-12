/*
 *
 * <apath> := [key] | [key] <apath>
 *
 * <value> := true | false | null | <int> | <float> | <string-literal>
 *            | <composite-value> | <actor>
 *
 * <actor> := d | ld | lld | f | lf | b | <size-specifier>? s | F(?)? | T(*)?
 *
 * <apath-value> := <apath> : <value>
 *
 * <composite-value> :=  { <apath-value>* } <existence-omission>?
 *                   | [ <value> ]  <existence-omission>?
 *
 * <existence-omission> := <size-specifier>? (E|O)
 *
 * <size-specifier> := . | .* | <integer>
 *
 *
 * examples:
 *
 * json_extractor(pos, size, "{ [key] : d"
 *                           "[key] : .*s }", &i)
 *
 * int ** list;
 * json_extractor(pos, size, "[ d ]", &list)*
 *
 *
 * json_injector(pos, size, "{  [key] : d"
 *                          "[key] : |abc| }", i);
 *
 *
 */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <ctype.h>
#include <stdarg.h>
#include "json-common.h"
#define N_PATH_MAX 8
#define KEY_MAX 128

#define JSMN_STATIC  // dont expose jsmn symbols
#define JSMN_PARENT_LINKS // add parent links to jsmn_tok, which are needed
#define JSMN_STRICT  // parse json in strict mode
#include "jsmn.h"
#include "ntl.h"

enum actor_tag {
  EXTRACTOR = 1,
  INJECTOR
};

/* 
 * the maximum levels of nested json object/array
 */
#define MAX_NESTED_LEVEL  16
#define MAX_ACTOR_NUMBERS   512

struct stack {
  unsigned char array[MAX_NESTED_LEVEL];
  int top;

  struct apath ** paths;
  struct apath * cur_path;

  struct value ** values;
  struct value * cur_value;
  enum actor_tag actor_tag;
};

#define PUSH(stack, c)  { stack->array[stack->top++] = c; }
#define TOP(stack)      (stack->array[stack->top-1])
#define POP(stack)      (stack->array[--stack->top])

struct apath {
  struct sized_buffer key;
  struct apath * next;
};

static void
print_apath (FILE * fp, struct apath * apath)
{
  fprintf(fp, "[%.*s]", apath->key.size, apath->key.start);
  if (apath->next)
    print_apath(fp, apath->next);
}

struct apath_value;

struct size_specifier {
  enum {
    UNKNOWN_SIZE = 0,
    FIXED_SIZE,
    PARAMETERIZED_SIZE,
    ZERO_SIZE
  } tag;
  union {
    size_t fixed_size;
    void * parameterized_size;
  } _;
};

struct actor {
  enum actor_tag tag;
  union {
    void *recipient; //must be a pointer, and it cannot be NULL
    void *provider; // this can be NULL or its value can be UNDEFINED
  } operand;
  struct size_specifier mem_size; // this designates the memory size of _;
  enum {
    BUILT_IN = 0,
    USER_DEF_ACCEPT_NON_NULL,
    USER_DEF_ACCEPT_NULL
  } action_tag;
  union {
    char built_in[10];
    int (*user_def)(char *, size_t, void *p);
  } action;
};

struct existence {
  struct size_specifier mem_size;
  bool has_this;
};

static void
print_actor (FILE * fp, struct actor * v)
{
  if (EXTRACTOR == v->tag)
    fprintf (fp, "[extractor]");
  else
    fprintf (fp, "[injector]");
  if (BUILT_IN == v->action_tag)
    fprintf(fp, "builtin(%s)\n", v->action.built_in);
  else
    fprintf(fp, "funptr(%p)\n", v->action.user_def);
}

struct value {
  enum {
    JSON_PRIMITIVE = 1,
    JSON_COMPOSITE_VALUE,
    JSON_ACTOR,
  } tag;
  union {
    struct sized_buffer primitve;
    struct composite_value * cv;
    struct actor actor;
  } _;
};

static void
print_composite_value (FILE * fp, struct composite_value * cv);

static void
print_value (FILE * fp, struct value * v) {
  fprintf(fp, "tag_%d ", v->tag);

  switch (v->tag) {
    case JSON_PRIMITIVE:
      fprintf(fp, "%.*s\n", v->_.primitve.size, v->_.primitve.start);
      break;
    case JSON_COMPOSITE_VALUE:
      print_composite_value(fp, v->_.cv);
      break;
    case JSON_ACTOR:
      print_actor(fp, &v->_.actor);
      break;
    default:
      break;
  }
}

struct apath_value {
  struct apath path;
  struct value value;
};

static void
print_apath_value (FILE * fp, struct apath_value *p)
{
  print_apath(fp, &p->path);
  fprintf(fp, " : ");
  print_value(fp, &p->value);
}

struct sized_apath_value {
  struct apath_value * pos;
  size_t size;
};

struct sized_value {
  struct value * pos;
  size_t size;
};

struct composite_value {
  enum {
    ARRAY = 1,
    OBJECT
  } tag;
  union {
    struct sized_value elements;
    struct sized_apath_value pairs;
  } _;
  struct existence E;
};

static void
print_composite_value (FILE * fp, struct composite_value * cv)
{
  if (cv->tag == ARRAY) {
    for (size_t i = 0; i < cv->_.elements.size; i++)
      print_value(fp, cv->_.elements.pos+i);
  }
  else {
    for (size_t i = 0; i < cv->_.pairs.size; i++)
      print_apath_value(fp, cv->_.pairs.pos+i);
  }
  if (cv->E.has_this) {
    fprintf(fp, "E ");
  }
}

static int
is_primitive (
  char * pos,
  size_t size,
  char ** next_pos_p)
{
  char * const end_pos = pos + size;
  unsigned char c;

  c = * pos;

  switch (c) {
    case 't': { // true
      if (pos + 3 < end_pos
          && 'r' == pos[1] && 'u' == pos[2] && 'e' == pos[3]) {
        pos += 4;
        goto return_true;
      }
      break;
    }
    case 'f': { // false
      if (pos + 4 < end_pos
          && 'a' == pos[1] && 'l' == pos[2] && 's' == pos[3] && 'e' == pos[4]) {
        pos += 5;
        goto return_true;
      }
      break;
    }
    case 'n': { // null
      if (pos + 3 < end_pos
          && 'u' == pos[1] && 'l' == pos[2] && 'l' == pos[3]) {
        pos += 4;
        goto return_true;
      }
      break;
    }
    case '"': { // a string literal
      pos ++;
      while (pos < end_pos) {
        c = *pos; pos ++;
        if ('"' == c)
          goto return_true;
      }
      break;
    }
    case '|': { // a propertiary string literal
      pos ++;
      while (pos < end_pos) {
        c = *pos; pos ++;
        if ('|' == c)
          goto return_true;
      }
      break;
    }
    default:
      if ('0' <= c && c <= '9') {
        pos++;
        while (pos < end_pos) {
          c = *pos;
          if (' ' == c || ',' == c) goto return_true;
          if ('.' == c || '0' <= c || c <= '9') pos++;
          else return 0;
        }
        goto return_true;
      }
      break;
  }
  return 0;


return_true:
  *next_pos_p = pos;
  return 1;
}

static int
parse_size_specifier (
  char * pos,
  size_t size,
  struct size_specifier * p,
  char **next_pos_p)
{
  char * const start_pos = pos, * const end_pos = pos + size, * x;
  long fixed_size = strtol(start_pos, &x, 10);

  if (x != start_pos) {
    if (fixed_size <= 0)
      ERR("size has to be a non-zero postive value %ld\n", fixed_size);

    p->tag = FIXED_SIZE;
    p->_.fixed_size = fixed_size;
    *next_pos_p = x; // jump to the end of number
    return 1;
  }
  else if (pos + 1 < end_pos && '.' == *pos && '*' == *(pos+1)) {
    p->tag = PARAMETERIZED_SIZE;
    *next_pos_p = pos + 2;
    return 1;
  }
  else if ('.' == *pos) {
    p->tag = ZERO_SIZE;
    *next_pos_p = pos + 1;
    return 1;
  }
  return 0;
}

static int
parse_value(
  struct stack * stack,
  char *pos, size_t size,
  struct value * p,
  char ** next_pos_p)
{
  char * const end_pos = pos + size;

  char *next_pos = NULL;
  if (is_primitive(pos, size, &next_pos)) {
    p->tag = JSON_PRIMITIVE;
    p->_.primitve.start = pos;
    p->_.primitve.size = next_pos - pos;
    *next_pos_p = next_pos;
    return 1;
  }
  struct actor * act = &p->_.actor;
  p->tag = JSON_ACTOR;
  act->tag = stack->actor_tag;
  int has_size_specifier = 0;

  if (parse_size_specifier(pos, end_pos - pos,
                           &act->mem_size, &next_pos)) {
    pos = next_pos;
    has_size_specifier = 1;
  }

  act->action_tag = BUILT_IN;
  switch(*pos)
  {
    case 'b':
      act->mem_size._.fixed_size = sizeof(bool);
      act->mem_size.tag = FIXED_SIZE;
      strcpy(act->action.built_in, "bool*");
      pos ++;
      goto return_true;
    case 'd':
      act->mem_size._.fixed_size = sizeof(int);
      act->mem_size.tag = FIXED_SIZE;
      strcpy(act->action.built_in, "int*");
      pos ++;
      goto return_true;
    case 'f':
      act->mem_size._.fixed_size = sizeof(float);
      act->mem_size.tag = FIXED_SIZE;
      strcpy(act->action.built_in, "float *");
      pos ++;
      goto return_true;
    case 'l': {
      if (STRNEQ(pos, "ld", 2)) {
        act->mem_size._.fixed_size = sizeof(long);
        act->mem_size.tag = FIXED_SIZE;
        strcpy(act->action.built_in, "long*");
        pos += 2;
        goto return_true;
      } else if (STRNEQ(pos, "lld", 3)) {
        act->mem_size._.fixed_size = sizeof(long long);
        act->mem_size.tag = FIXED_SIZE;
        strcpy(act->action.built_in, "long long *");
        pos += 3;
        goto return_true;
      } else if (STRNEQ(pos, "lf", 2)) {
        act->mem_size._.fixed_size = sizeof(double);
        act->mem_size.tag = FIXED_SIZE;
        strcpy(act->action.built_in, "double *");
        pos += 2;
        goto return_true;
      }
    }
    case 's':
      strcpy(act->action.built_in, "char*");
      pos ++;
      goto return_true;
    case 'L':
      strcpy(act->action.built_in, "array");
      pos ++;
      goto return_true;
    case 'A':
      strcpy(act->action.built_in, "array");
      pos ++;
      goto return_true;
    case 'F':
      act->action_tag = USER_DEF_ACCEPT_NON_NULL;
      pos ++;
      goto return_true;
    case 'T':
      strcpy(act->action.built_in, "token");
      pos ++;
      goto return_true;
    default:
      if (TOP(stack) == *pos) {
        if (has_size_specifier)
          ERR("size specifier '.' or '.*' should be followed by 's' \n");
        return 0;
      }
      else
        ERR("unexpected %c\n", *pos);
  }

return_true:
  *next_pos_p = pos;
  return 1;
}

static int
parse_existence(char *pos, size_t size,
                struct existence * p,
                char ** next_pos_p)
{
  if (size == 0)
    return 0;

  char * next_pos = NULL;
  if (parse_size_specifier(pos, size, &p->mem_size, &next_pos)) {
    pos = next_pos;
  }

  if (STRNEQ(pos, "E", 1)){
    p->has_this = true;
    pos ++;
    *next_pos_p = pos;
    return 1;
  }
  return 0;
}

static char * 
parse_composite_value(struct stack *, char *, size_t, struct composite_value *);

#define SKIP_SPACES(s, end)   { while (s < end && isspace(*s)) ++s; }

static char *
parse_apath_value(
  struct stack *stack,
  char *pos, size_t size, struct apath_value *av,
  struct apath *curr_path)
{
  // until find a ']' or '\0'
  char * const start_pos = pos, * const end_pos = pos + size,
    * next_pos = NULL;

  ASSERT_S('[' == *pos, "expecting '['");
  pos ++;
  while (*pos && pos < end_pos) {
    if (']' == *pos) break;
    ++pos;
  }

  ASSERT_S(*pos == ']', "A close bracket ']' is missing");

  int len = pos - start_pos - 1;
  ASSERT_S(len > 0, "Key is missing");

  curr_path->key.start = calloc(1, len); // @todo get memory from stack's pool
  curr_path->key.size = len;
  memcpy(curr_path->key.start, start_pos+1, len);

  ++pos; // eat up ']'
  SKIP_SPACES(pos, end_pos);
  switch (*pos)
  {
    case '[':
    {
      struct apath *next_path = calloc(1, sizeof(struct apath));
      curr_path->next = next_path;
      return parse_apath_value(stack, pos, end_pos - pos, av, next_path);
    }
    case ':':
    {
      ++pos; // eat up ':'
      SKIP_SPACES(pos, end_pos);
      if ('[' == *pos || '{' == *pos) {
        struct composite_value * cv = calloc(1, sizeof(struct composite_value));
        av->value._.cv = cv;
        av->value.tag = JSON_COMPOSITE_VALUE;
        pos = parse_composite_value(stack, pos, end_pos - pos, cv);
      }
      else if (parse_value(stack, pos, end_pos - pos, &av->value, &next_pos))
        pos = next_pos;
      else
        ERR("expecting a value after ':', %s does not have a legit value", pos);

      break;
    }
    default:
      ERR("expecting '[' or ':', but getting %c\n", *pos);
  }
  return pos;
}

static char *
parse_apath_value_list(
  struct stack * stack,
  char * pos,
  size_t size,
  struct sized_apath_value * pairs)
{
  char * const start_pos = pos, * const end_pos = pos + size;
  pairs->pos = calloc(20, sizeof(struct apath_value));

  size_t i = 0;
  while (pos < end_pos) {
    SKIP_SPACES(pos, end_pos);
    if ('[' == *pos) {
      pos = parse_apath_value(stack, pos, end_pos - pos,
                              pairs->pos + i, &pairs->pos[i].path);
      i++;
    }
    else if (TOP(stack) == *pos) {
      pairs->size = i;
      return pos;
    }
    else
      ERR("Expecting %c, but found %c in %s", TOP(stack), *pos, start_pos);
  }
  pairs->size = i;
  return pos;
}

static char *
parse_value_list (
  struct stack * stack,
  char * pos,
  size_t size,
  struct sized_value * elements)
{
  char * const end_pos = pos + size;
  elements->pos = calloc(20, sizeof(struct value));
  char * next_pos = NULL;

  size_t i = 0;
  while (pos < end_pos) {
    SKIP_SPACES(pos, end_pos);
    next_pos = NULL;
    if (parse_value(stack, pos, size, elements->pos+i, &next_pos)) {
      i++;
      pos = next_pos;
    }
    else if (TOP(stack) == *pos) {
      elements->size = i;
      return pos;
    }
    else {
      ERR("Unexpected %c in %s", *pos, pos);
    }
  }
  elements->size = i;
  return pos;
}

static struct stack stack = { .array = {0}, .top = 0, .actor_tag = INJECTOR };

static char *
parse_composite_value(
  struct stack *stack,
  char *pos,
  size_t size,
  struct composite_value *expr)
{
  char * const start_pos = pos, * const end_pos = pos + size;
  char * next_pos = NULL;

  SKIP_SPACES(pos, end_pos);
  switch(*pos)
  {
    case '{':
    {
      expr->tag = OBJECT;
      pos++;
      PUSH(stack, '}');
      pos = parse_apath_value_list(stack, pos, end_pos - pos, &expr->_.pairs);
      char c = POP(stack);
      if (c != *pos)
        ERR("Mismatched stack: expecting %c, but getting %c\n", c, *pos);
      pos++;
      SKIP_SPACES(pos, end_pos);
      if (parse_existence(pos, end_pos - pos, &expr->E, &next_pos))
        pos = next_pos;
      break;
    }
    case '[':
    {
      expr->tag = ARRAY;
      pos++;
      PUSH(stack, ']');
      pos = parse_value_list(stack, pos, end_pos - pos, &expr->_.elements);
      char c = POP(stack);
      if (c != *pos)
        ERR("Mismatched stack: expecting %c, but getting %c\n", c, *pos);
      pos++;
      SKIP_SPACES(pos, end_pos);
      if (parse_existence(pos, end_pos - pos, &expr->E, &next_pos))
        pos = next_pos;
      break;
    }
    default:
      ERR("unexpected %c in %s\n", *pos, start_pos);
  }
  return pos;
}

struct recipients {
  void * addrs[MAX_ACTOR_NUMBERS];
  size_t pos;
};

static void
collect_composite_value_recipients (struct composite_value *cv,
                                    struct recipients * rec);

static void
collect_value_recipients (struct value *v, struct recipients *rec)
{
  switch (v->tag) {
    case JSON_ACTOR: {
      struct actor *actor = &v->_.actor;
      switch (actor->action_tag) {
        case BUILT_IN:
          if (PARAMETERIZED_SIZE == actor->mem_size.tag) {
            rec->addrs[rec->pos] = (void *)&actor->mem_size._.parameterized_size;
            rec->pos ++;
          }
          rec->addrs[rec->pos] = (void *)&actor->operand.recipient;
          rec->pos ++;
          break;
        case USER_DEF_ACCEPT_NON_NULL:
          rec->addrs[rec->pos] = (void *)&actor->action.user_def;
          rec->pos ++;
          rec->addrs[rec->pos] = (void *)&actor->operand;
          rec->pos ++;
          break;
        case USER_DEF_ACCEPT_NULL:
          rec->addrs[rec->pos] = (void *)&actor->action.user_def;
          rec->pos ++;
          rec->addrs[rec->pos] = (void *)&actor->operand;
          rec->pos ++;
          break;
      }
      break;
    }
    case JSON_COMPOSITE_VALUE:
      collect_composite_value_recipients(v->_.cv, rec);
      break;
    case JSON_PRIMITIVE:
      break;
  }
}

static void
collect_composite_value_recipients (struct composite_value *cv, struct recipients * rec)
{
  switch(cv->tag)
  {
    case OBJECT: {
      struct apath_value *p;
      for (size_t i = 0; i < cv->_.pairs.size; i++) {
        p = cv->_.pairs.pos + i;
        collect_value_recipients(&p->value, rec);
      }
      break;
    }
    case ARRAY: {
      struct value * p;
      for (size_t i = 0; i < cv->_.elements.size; i++) {
        p = cv->_.elements.pos + i;
        collect_value_recipients(p, rec);
      }
      break;
    }
  }
}

int
json_injector(char * pos, size_t size, char * js_actor_spec, ...)
{
  struct stack stack = { .array = {0}, .top = 0, .actor_tag = INJECTOR };
  struct composite_value cv;
  memset(&cv, 0, sizeof(struct composite_value));
  char * next_pos =
    parse_composite_value(&stack, js_actor_spec, strlen(js_actor_spec), &cv);

  if (next_pos == pos)
    ERR("failed to parse %s\n", js_actor_spec);


  struct recipients  rec = { 0 };
  collect_composite_value_recipients(&cv, &rec);

  va_list ap;
  va_start(ap, js_actor_spec);
  for (size_t i = 0; i < rec.pos; i++)
    rec.addrs[i] = va_arg(ap, void *);
  va_end(ap);
  
}
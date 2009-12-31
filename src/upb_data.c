/*
 * upb - a minimalist implementation of protocol buffers.
 *
 * Copyright (c) 2009 Joshua Haberman.  See LICENSE for details.
 */

#include <stdlib.h>
#include "upb_data.h"
#include "upb_def.h"

static uint32_t round_up_to_pow2(uint32_t v)
{
  /* cf. http://graphics.stanford.edu/~seander/bithacks.html#RoundUpPowerOf2 */
  v--;
  v |= v >> 1;
  v |= v >> 2;
  v |= v >> 4;
  v |= v >> 8;
  v |= v >> 16;
  v++;
  return v;
}

/* upb_data *******************************************************************/

static void data_elem_unref(void *d, struct upb_fielddef *f) {
  if(f->type == UPB_TYPE(MESSAGE) || f->type == UPB_TYPE(GROUP)) {
    upb_msg_unref((upb_msg*)d, upb_downcast_msgdef(f->def));
  } else if(f->type == UPB_TYPE(STRING) || f->type == UPB_TYPE(BYTES)) {
    upb_string_unref((upb_string*)d);
  }
}

static void data_unref(void *d, struct upb_fielddef *f) {
  if(upb_isarray(f)) {
    upb_array_unref((upb_array*)d, f);
  } else {
    data_elem_unref(d, f);
  }
}

INLINE void data_init(upb_data *d, int flags) {
  d->v = flags;
}

static void check_not_frozen(upb_data *d) {
  // On one hand I am reluctant to put abort() calls in a low-level library
  // that are enabled in a production build.  On the other hand, this is a bug
  // in the client code that we cannot recover from, and it seems better to get
  // the error here than later.
  if(upb_data_hasflag(d, UPB_DATA_FROZEN)) abort();
}

static upb_strlen_t string_get_bytesize(upb_string *s) {
  if(upb_data_hasflag(&s->common.base, UPB_DATA_REFCOUNTED)) {
    return s->refcounted.byte_size;
  } else {
    return (s->norefcount.byte_size_and_flags & 0xFFFFFFF8) >> 3;
  }
}

static void string_set_bytesize(upb_string *s, upb_strlen_t newsize) {
  if(upb_data_hasflag(&s->common.base, UPB_DATA_REFCOUNTED)) {
    s->refcounted.byte_size = newsize;
  } else {
    s->norefcount.byte_size_and_flags &= 0x7;
    s->norefcount.byte_size_and_flags |= (newsize << 3);
  }
}


/* upb_string *******************************************************************/

upb_string *upb_string_new() {
  upb_string *s = malloc(sizeof(upb_refcounted_string));
  data_init(&s->common.base, UPB_DATA_HEAPALLOCATED | UPB_DATA_REFCOUNTED);
  s->refcounted.byte_size = 0;
  s->common.byte_len = 0;
  s->common.ptr = NULL;
  return s;
}

void _upb_string_free(upb_string *s)
{
  if(string_get_bytesize(s) != 0) free(s->common.ptr);
  free(s);
}

void upb_string_resize(upb_string *s, upb_strlen_t byte_len) {
  check_not_frozen(&s->common.base);
  if(string_get_bytesize(s) < byte_len) {
    // Need to resize.
    size_t new_byte_size = round_up_to_pow2(byte_len);
    s->common.ptr = realloc(s->common.ptr, new_byte_size);
    string_set_bytesize(s, new_byte_size);
  }
  s->common.byte_len = byte_len;
}

upb_string *upb_string_getref(upb_string *s, int ref_flags) {
  if(_upb_data_incref(&s->common.base, ref_flags)) return s;
  return upb_strdup(s);
}

upb_string *upb_strreadfile(const char *filename) {
  FILE *f = fopen(filename, "rb");
  if(!f) return false;
  if(fseek(f, 0, SEEK_END) != 0) goto error;
  long size = ftell(f);
  if(size < 0) goto error;
  if(fseek(f, 0, SEEK_SET) != 0) goto error;
  upb_string *s = upb_string_new();
  char *buf = upb_string_getrwbuf(s, size);
  if(fread(buf, size, 1, f) != 1) goto error;
  fclose(f);
  return s;

error:
  fclose(f);
  return NULL;
}

upb_string *upb_strdupc(const char *src) {
  upb_string *copy = upb_string_new();
  upb_strlen_t len = strlen(src);
  char *buf = upb_string_getrwbuf(copy, len);
  memcpy(buf, src, len);
  return copy;
}

void upb_strcat(upb_string *s, upb_string *append) {
  upb_strlen_t s_len = upb_strlen(s);
  upb_strlen_t append_len = upb_strlen(append);
  upb_strlen_t newlen = s_len + append_len;
  memcpy(upb_string_getrwbuf(s, newlen) + s_len,
         upb_string_getrobuf(append), append_len);
}

void upb_strcpy(upb_string *dest, upb_string *src) {
  upb_strlen_t src_len = upb_strlen(src);
  memcpy(upb_string_getrwbuf(dest, src_len), upb_string_getrobuf(src), src_len);
}

upb_string *upb_strslice(upb_string *s, int offset, int len) {
  upb_string *slice = upb_string_new();
  len = UPB_MIN((upb_strlen_t)len, upb_strlen(s) - (upb_strlen_t)offset);
  memcpy(upb_string_getrwbuf(slice, len), upb_string_getrobuf(s) + offset, len);
  return slice;
}

upb_string *upb_strdup(upb_string *s) {
  upb_string *copy = upb_string_new();
  upb_strcpy(copy, s);
  return copy;
}

int upb_strcmp(upb_string *s1, upb_string *s2) {
  upb_strlen_t common_length = UPB_MIN(upb_strlen(s1), upb_strlen(s2));
  int common_diff = memcmp(upb_string_getrobuf(s1), upb_string_getrobuf(s2),
                           common_length);
  return common_diff ==
      0 ? ((int)upb_strlen(s1) - (int)upb_strlen(s2)) : common_diff;
}


/* upb_array ******************************************************************/

// ONLY handles refcounted arrays for the moment.
void _upb_array_free(upb_array *a, struct upb_fielddef *f)
{
  if(upb_elem_ismm(f)) {
    for(upb_arraylen_t i = 0; i < a->refcounted.size; i++) {
      union upb_value_ptr p = _upb_array_getptr(a, f, i);
      data_elem_unref(p._void, f);
    }
  }
  if(a->refcounted.size != 0) free(a->common.elements._void);
  free(a);
}


/* upb_msg ********************************************************************/

upb_msg *upb_msg_new(struct upb_msgdef *md) {
  upb_msg *msg = malloc(md->size);
  memset(msg, 0, md->size);
  data_init(&msg->base, UPB_DATA_HEAPALLOCATED | UPB_DATA_REFCOUNTED);
  upb_def_ref(UPB_UPCAST(md));
  return msg;
}

// ONLY handles refcounted messages for the moment.
void _upb_msg_free(upb_msg *msg, struct upb_msgdef *md)
{
  for(int i = 0; i < md->num_fields; i++) {
    struct upb_fielddef *f = &md->fields[i];
    union upb_value_ptr p = _upb_msg_getptr(msg, f);
    if(!upb_field_ismm(f) || !p._void) continue;
    data_unref(p._void, f);
  }
  upb_def_unref(UPB_UPCAST(md));
  free(msg);
}


/* Parsing.  ******************************************************************/

struct upb_msgparser_frame {
  upb_msg *msg;
  struct upb_msgdef *md;
};

struct upb_msgparser {
  struct upb_cbparser *s;
  bool merge;
  struct upb_msgparser_frame stack[UPB_MAX_NESTING], *top;
};

/* Helper function that returns a pointer to where the next value for field "f"
 * should be stored, taking into account whether f is an array that may need to
 * be allocated or resized. */
static union upb_value_ptr get_value_ptr(struct upb_msg *msg,
                                         struct upb_fielddef *f)
{
  union upb_value_ptr p = upb_msg_getptr(msg, f);
  if(upb_isarray(f)) {
    if(!upb_msg_isset(msg, f)) {
      if(!*p.arr || !upb_mmhead_only(&((*p.arr)->mmhead))) {
        if(*p.arr)
          upb_array_unref(*p.arr);
        *p.arr = upb_array_new(f);
      }
      upb_array_truncate(*p.arr);
      upb_msg_set(msg, f);
    }
    p = upb_array_append(*p.arr);
  }
  return p;
}

/* Callbacks for the stream parser. */

static bool value_cb(void *udata, struct upb_msgdef *msgdef,
                     struct upb_fielddef *f, union upb_value val)
{
  (void)msgdef;
  struct upb_msgparser *mp = udata;
  struct upb_msg *msg = mp->top->msg;
  union upb_value_ptr p = get_value_ptr(msg, f);
  upb_msg_set(msg, f);
  upb_value_write(p, val, f->type);
  return true;
}

static bool str_cb(void *udata, struct upb_msgdef *msgdef,
                   struct upb_fielddef *f, uint8_t *str, size_t avail_len,
                   size_t total_len)
{
  (void)msgdef;
  struct upb_msgparser *mp = udata;
  struct upb_msg *msg = mp->top->msg;
  union upb_value_ptr p = get_value_ptr(msg, f);
  upb_msg_set(msg, f);
  if(avail_len != total_len) abort();  /* TODO: support streaming. */
  if(!*p.str || !upb_mmhead_only(&((*p.str)->mmhead))) {
    if(*p.str)
      upb_string_unref(*p.str);
    *p.str = upb_string_new();
  }
  upb_string_resize(*p.str, total_len);
  memcpy((*p.str)->ptr, str, avail_len);
  (*p.str)->byte_len = avail_len;
  return true;
}

static void start_cb(void *udata, struct upb_fielddef *f)
{
  struct upb_msgparser *mp = udata;
  struct upb_msg *oldmsg = mp->top->msg;
  union upb_value_ptr p = get_value_ptr(oldmsg, f);

  if(upb_isarray(f) || !upb_msg_isset(oldmsg, f)) {
    if(!*p.msg || !upb_mmhead_only(&((*p.msg)->mmhead))) {
      if(*p.msg)
        upb_msg_unref(*p.msg);
      *p.msg = upb_msg_new(upb_downcast_msgdef(f->def));
    }
    upb_msg_clear(*p.msg);
    upb_msg_set(oldmsg, f);
  }

  mp->top++;
  mp->top->msg = *p.msg;
}

static void end_cb(void *udata)
{
  struct upb_msgparser *mp = udata;
  mp->top--;
}

/* Externally-visible functions for the msg parser. */

struct upb_msgparser *upb_msgparser_new(struct upb_msgdef *def)
{
  struct upb_msgparser *mp = malloc(sizeof(struct upb_msgparser));
  mp->s = upb_cbparser_new(def, value_cb, str_cb, start_cb, end_cb);
  return mp;
}

void upb_msgparser_reset(struct upb_msgparser *s, struct upb_msg *msg, bool byref)
{
  upb_cbparser_reset(s->s, s);
  s->byref = byref;
  s->top = s->stack;
  s->top->msg = msg;
}

void upb_msgparser_free(struct upb_msgparser *s)
{
  upb_cbparser_free(s->s);
  free(s);
}

void upb_msg_parsestr(struct upb_msg *msg, void *buf, size_t len,
                      struct upb_status *status)
{
  struct upb_msgparser *mp = upb_msgparser_new(msg->def);
  upb_msgparser_reset(mp, msg, false);
  upb_msg_clear(msg);
  upb_msgparser_parse(mp, buf, len, status);
  upb_msgparser_free(mp);
}

size_t upb_msgparser_parse(struct upb_msgparser *s, upb_string *str,
                           struct upb_status *status)
{
  return upb_cbparser_parse(s->s, str, status);
}

/*
    This file is part of VK/KittenPHP-DB-Engine.

    VK/KittenPHP-DB-Engine is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 2 of the License, or
    (at your option) any later version.

    VK/KittenPHP-DB-Engine is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with VK/KittenPHP-DB-Engine.  If not, see <http://www.gnu.org/licenses/>.

    This program is released under the GPL with the additional exemption
    that compiling, linking, and/or using OpenSSL is allowed.
    You are free to remove this exemption from derived works.

    Copyright 2010-2013 Vkontakte Ltd
              2010-2013 Arseny Smirnov
              2010-2013 Aliaksei Levin
*/

#define _FILE_OFFSET_BITS 64

#include <stdarg.h>
#include <stddef.h>

#include "poll-data.h"
#include "poll-engine.h"

#include "dl.def"

#define ZERO_UID 1000000000
double max_delay = 0, sum_delay = 0;
long long cnt_delay = 0;

long long events_created, events_sent;

int my_verbosity = 0;
int __may_wait;

char debug_buff[1 << 24];
char *ds;
int debug_error;

void debug (char const *msg, ...) {
  if (ds - debug_buff < 8 * (1 << 20)) {
    va_list args;

    va_start (args, msg);
    ds += vsnprintf (ds, debug_buff + (1 << 24) - ds, msg, args);
    va_end (args);

    if (ds >= debug_buff + (1 << 24)) {
      debug_error = 1;
    }
  } else {
    debug_error = 1;
  }
}

#define MAX_BUFF (1 << 24)
char buff[MAX_BUFF];

extern long max_memory;

extern long long http_failed[4];
extern long long buff_overflow_cnt;
extern int http_sfd;

extern int engine_id, engine_n;

long events_memory, keys_memory, subscribers_memory, subscribers_cnt, str_memory, crypto_memory, queues_memory,
     time_keys_memory;
int events_cnt, keys_cnt, treap_cnt, treap_cnt_rev, queues_cnt;

long long send_changes_cnt,
  process_changes_cnt,
  changes_len_max,
  process_changes_total_len,
  changes_add_rev_cnt,
  changes_add_rev_len,
  changes_add_cnt,
  changes_add_len,
  changes_del_rev_cnt,
  changes_del_rev_len,
  changes_del_cnt,
  changes_del_len,
  to_add_overflow,
  to_del_overflow;

dl_crypto cr;

long long max_events_memory;

void encode_key (char *key, char *qname, unsigned int id, unsigned int ip, unsigned int secret);

int eq_n, eq_total;

#define MAXQ 5000000

int write_str (char *d, char *s, int sn) {
  char *st = d;
  int i;
  for (i = 0; i < sn; i++) {
    char c = s[i];
#define C(x, y) case x: *d++ = '\\', *d++ = y; break;
    switch (c) {
      C('"', '"');
      C('\\', '\\');
      C('/', '/');
      C('\b', 'b');
      C('\f', 'f');
      C('\n', 'n');
      C('\r', 'r');
      C('\t', 't');
      default:
        if ((unsigned char)c >= 32) {
          *d++ = c;
        } else {
          //TODO its TOTALLY WRONG
          d += sprintf (d,"\\u%04x", c);
        }
    }
#undef C
  }
  return d - st;
}


/***
  QUEUE CODE
 ***/
void clear_subscr (queue *q);

shmap_string_vptr h_queue, h_qkey;
void event_free (event *e);
void delete_qkey (qkey *k);

/***
  Secrets
 ***/

map_int_int secrets;

int upd_secret (int id) {
  int *v = map_int_int_add (&secrets, id);
  int nv = rand();
  if (nv == *v) {
    nv += 13;
  }
  return *v = nv;
}

int get_secret (int id) {
  int *v = map_int_int_get (&secrets, id);
  if (v == NULL) {
    return upd_secret (id);
  }
  return *v;
}

/***
  Timeouts
 ***/
#ifdef TEST_MODE
#  define TIME_TABLE_RATIO_EXP (0)
#  define TIME_TABLE_SIZE_EXP (8 - TIME_TABLE_RATIO_EXP)
#  define TIME_TABLE_SIZE (1 << TIME_TABLE_SIZE_EXP)
#  define TIME_TABLE_MASK (TIME_TABLE_SIZE - 1)
#  define GET_TIME_ID(x) (((unsigned int)(x) >> TIME_TABLE_RATIO_EXP) & TIME_TABLE_MASK)
#  define MAX_TIME_GAP ((15) >> TIME_TABLE_RATIO_EXP)
#else
#  define TIME_TABLE_RATIO_EXP (4)
#  define TIME_TABLE_SIZE_EXP (18 - TIME_TABLE_RATIO_EXP)
#  define TIME_TABLE_SIZE (1 << TIME_TABLE_SIZE_EXP)
#  define TIME_TABLE_MASK (TIME_TABLE_SIZE - 1)
#  define GET_TIME_ID(x) (((unsigned int)(x) >> TIME_TABLE_RATIO_EXP) & TIME_TABLE_MASK)
#  define MAX_TIME_GAP ((60 * 60) >> TIME_TABLE_RATIO_EXP)
#endif

int last_del_time;
qkey *time_st[TIME_TABLE_SIZE];

void del_entry_time (qkey *entry) {
  entry->next_time->prev_time = entry->prev_time;
  entry->prev_time->next_time = entry->next_time;
}

void add_entry_time (qkey *entry, int delay) {
  if (my_verbosity > 2) {
    fprintf (stderr, "upd timeout %s : %d\n", entry->name, entry->timeout);
  }

  qkey *f = time_st[GET_TIME_ID (entry->timeout + get_utime (CLOCK_MONOTONIC) + delay)];
  qkey *y = f->prev_time;

  entry->next_time = f;
  f->prev_time = entry;

  entry->prev_time = y;
  y->next_time = entry;
}

void free_by_time (int mx) {
  int cur_time = get_utime (CLOCK_MONOTONIC);
  int en = GET_TIME_ID(cur_time);
  qkey *st = time_st[last_del_time];

//  fprintf (stderr, "[%ld/%lld]\n", events_memory + keys_memory, max_events_memory);
  while (en - last_del_time > MAX_TIME_GAP || last_del_time - en > TIME_TABLE_SIZE - MAX_TIME_GAP ||
         (mx-- > 0 && last_del_time != en)) {
    if (st->next_time != st) {
      if (my_verbosity > 1) {
        fprintf (stderr, "del entry %p by time (key = %s) gap = %d, timeout = %d, qname = (%p), now = %d, now2 = %d\n", st->next_time, st->next_time->name, en - last_del_time, st->next_time->timeout, st->next_time->q, now, (int)get_utime (CLOCK_MONOTONIC));
        if (st->next_time->q != NULL) {
          queue *q = st->next_time->q;
          fprintf (stderr, "!%s!%d!", q->name, q->ref_cnt);
          if (q->ev_first != NULL) {
            fprintf (stderr, "%d\n", q->ev_first->ref_cnt);
          } else {
            fprintf (stderr, "NULL\n");
          }
        }
      }
      delete_qkey (st->next_time);
    } else {
      if (++last_del_time == TIME_TABLE_SIZE) {
        last_del_time = 0;
      }
      st = time_st[last_del_time];
    }
  }

  en = GET_TIME_ID(cur_time - (2 << TIME_TABLE_RATIO_EXP));
  int cur = last_del_time;
  st = time_st[cur];
  mx = 10000000;
  while (mx-- > 0 && cur != en && events_memory + keys_memory > max_events_memory) {
    qkey *i;
    for (i = time_st[cur]; i->next_time != time_st[cur] && mx > 0; mx--) {
      if (!i->next_time->lock) {
        fprintf (stderr, "del entry %p by memory limit (key = %s) [%ld / %lld]\n", i->next_time, i->next_time->name, events_memory + keys_memory, max_events_memory);
        delete_qkey (i->next_time);
      } else {
        i = i->next_time;
      }
    }
    if (++cur == TIME_TABLE_SIZE) {
      cur = 0;
    }
  }
}


/***
   Queue
 ***/
map_ll_vptr alias;

queue *get_queue_by_alias (ll id) {
  if (id == 0) {
    id = ZERO_UID;
  }

  queue **q = (queue **)map_ll_vptr_get (&alias, id);
  //fprintf (stderr, "search %lld\n", id);
  if (q == NULL) {
    return NULL;
  }
  return *q;
}


ll get_next_alias (void) {
  static ll st = -1;
  if (st == -1) {
    st = ((ll)get_utime(CLOCK_REALTIME) + 1001) * 1000000;
    st /= engine_n;
    st *= engine_n;
    st += engine_id;
  }
  st += engine_n;
  assert (st > 1000000000);
  return st;
}

qtype get_qtype (char *qname) {
  return Q_DEF;
}

queue *qs;
queue *queue_malloc (void) {
  queues_cnt++;
  if (qs == NULL) {
    queues_memory -= dl_get_memory_used();
    queue *res = dl_malloc0 (sizeof (queue));
    queues_memory += dl_get_memory_used();

    return res;
  }
  queue *res = qs;
  qs = (queue *)qs->ev_first;
  memset (res, 0, sizeof (queue));

  return res;
}
void queue_free (queue *q) {
  queues_cnt--;
  q->ev_first = (event *)qs;
  qs = q;
}

int process_user_query_queue (queue *q) {
  struct conn_query *Q;
  double wtime = precise_now; //get_utime (CLOCK_MONOTONIC);// + 0.05;

  for (Q = q->first_q; Q != (struct conn_query *)q; Q = Q->next) {
    if (Q->timer.wakeup_time > wtime || Q->timer.wakeup_time == 0) {
      Q->timer.wakeup_time = wtime;
      insert_event_timer (&Q->timer);
    }
  }

  return 0;
}

void queue_clear_subs (queue *q) {
  if (q != NULL && q->name != NULL && q->subs != NULL) {
    clear_subscr (q);

    queues_memory -= dl_get_memory_used();
    dl_free (q->subs, sizeof (treap));
    queues_memory += dl_get_memory_used();
    q->subs = NULL;
  }
}

// queue is deleted only if nobody listen to it
// in this case it has no events in it
inline void delete_queue (queue *q) {
  if (q->name != NULL) {
    //fprintf (stderr, "DEL queue [%s]\n", q->name);
    shmap_pair_string_vptr a;
    a.y = NULL;
    a.x = q->name;

    shmap_string_vptr_del (&h_queue, a);

    //TODO why process but not close?
    process_user_query_queue (q);

    queue_clear_subs (q);

    str_memory -= dl_get_memory_used();
    dl_strfree (q->name);
    str_memory += dl_get_memory_used();
    q->name = NULL;

    if (q->id == 0) {
      q->id = ZERO_UID;
    }
    map_ll_vptr_del (&alias, q->id);
    dbg ("del alias %lld\n", q->id);

    queue_free (q);
  }
}

inline queue *get_queue (char *name, int force) {
  shmap_pair_string_vptr a, *b;
  a.y = NULL;
  a.x = name;

  queue *q = NULL;
  if (force) {
    b = shmap_string_vptr_add (&h_queue, a);
    if (b->y == NULL) {
      str_memory -= dl_get_memory_used();
      b->x = dl_strdup (b->x);
      str_memory += dl_get_memory_used();

      q = b->y = queue_malloc();

      q->name = b->x;
      q->first_q = q->last_q = (struct conn_query *) q;

      q->ev_first = q->ev_last = NULL; // Yep. It will be list with 'if'

      dbg ("ALIAS for [%s] = %lld\n", name, q->id);
      ll tid = q->id;

      if (tid == 0) {
        tid = ZERO_UID;
      }

      dbg ("add alias : %lld\n", tid);
      queue **tq = (queue **)map_ll_vptr_add (&alias, tid);
      assert (*tq == NULL);
      *tq = q;
    } else {
      q = b->y;
    }
  } else {
    //fprintf (stderr, "GET queue [%s]\n", name);
    b = shmap_string_vptr_get (&h_queue, a);
    if (b != NULL) {
      q = b->y;
    }
  }

  return q;
}

inline void queue_add (queue *q, event *e) {
  assert (e->next == NULL);
  if (q->ev_first == NULL) {
    q->ev_first = q->ev_last = e;
  } else {
    q->ev_last->next = e;
    q->ev_last = e;
  }
}

void queue_fix (queue *q) {
  if (my_verbosity > 1) {
    fprintf (stderr, "in queue fix %p %d\n", q, q->ref_cnt);
  }
  if (q->ref_cnt == 0) {
    event *e = q->ev_first;
    while (e != NULL && e->ref_cnt == 0) {
      q->ev_first = e->next;

      event_free (e);

      e = q->ev_first;
    }
    if (e == NULL) {
      q->ev_last = NULL;
    }
  }

  // TODO: last condition should be enough
  if (/*q->ref_cnt == 0 && q->ev_first == NULL &&*/ q->keys_cnt == 0) {
    queue_clear_subs (q);
  }

  if (/*q->ref_cnt == 0 && q->ev_first == NULL &&*/ q->keys_cnt == 0 && q->subscr_cnt == 0) {
    delete_queue (q);
  }
  if (my_verbosity > 1) {
    fprintf (stderr, "exit queue fix\n");
  }
}

/***
  Event
 ***/

inline size_t get_event_size_d (int data_len) {
  return data_len + offsetof (event, data);
}
inline size_t get_event_size (event *e) {
  return get_event_size_d (e->data_len);
}

void event_free (event *e) {
  size_t l = get_event_size (e);

  events_cnt--;

  events_memory -= dl_get_memory_used();
  dl_free (e, l);
  events_memory += dl_get_memory_used();
}

event *event_malloc (int data_len) {
  size_t l = get_event_size_d (data_len);

  events_created++;
  events_cnt++;


  events_memory -= dl_get_memory_used();
  event *e = dl_malloc (l);
  events_memory += dl_get_memory_used();

  assert (e);

  e->data_len = data_len;
  e->ref_cnt = 0;
  e->next = NULL;

  e->created = get_utime (CLOCK_MONOTONIC);

  return e;
}

int add_event (queue *q, char *data, int data_len, int x, int y, int ttl) {
  if (q == NULL) {
    if (verbosity > 2) {
      fprintf (stderr, "Queue not found.\n");
    }
    return 0;
  }

  //fix
  //dbg ("ADD EVENT: queue name = [%s], data = [%s], ttl = [%d]\n", q->name, data, ttl);

  if (q->ref_cnt != 0 || q->ev_first != NULL) {
    if (verbosity > 2) {
      fprintf (stderr, "  queue = %p\n", q);
    }
    event *e = event_malloc (data_len + 1);

    memcpy (e->data, data, data_len + 1);

    queue_add (q, e);

    process_user_query_queue (q);
  }

  return 1;
}

/***
 QKeys
 ***/
qkey *qks;
qkey *qkey_malloc (void) {
  keys_cnt++;
  if (qks == NULL) {
    keys_memory -= dl_get_memory_used();
    qkey *res = dl_malloc0 (sizeof (qkey));
    keys_memory += dl_get_memory_used();

    return res;
  }
  qkey *res = qks;
  qks = (qkey *)qks->name;
  memset (res, 0, sizeof (qkey));

  return res;
}

void qkey_free (qkey *q) {
  keys_cnt--;

  keys_memory -= dl_get_memory_used();
  dl_free (q, sizeof (qkey));
  keys_memory += dl_get_memory_used();
  /*
  q->name = (char *)qks;
  qks = q;
  */
}


qkey *qkey_get (char *name, int force) {
  shmap_pair_string_vptr a, *b;
  a.y = NULL;
  a.x = name;

  qkey *k = NULL;
  if (force) {
    b = shmap_string_vptr_add (&h_qkey, a);
    if (b->y == NULL) {
      str_memory -= dl_get_memory_used();
      b->x = dl_strdup (b->x);
      str_memory += dl_get_memory_used();

      k = b->y = qkey_malloc();
      k->name = b->x;
    } else if (force == 1) {
      assert ("Keys collision" && 0);
    } else {
      k = b->y;
    }

    if (verbosity > 2) {
      fprintf (stderr, "Key created : [%s]\n", name);
    }
    // dbg ("Key created %p : [%s]:%p\n", k, k->name, k->name);
  } else {
//    fprintf (stderr, "get Key %p : ", name);
//    fprintf (stderr, "[%s]\n", name);
    b = shmap_string_vptr_get (&h_qkey, a);
    if (b != NULL) {
      k = b->y;
    }
  }

  return k;
}

void delete_qkey (qkey *k) {
  assert (k != NULL);

  del_entry_time (k);
  if (k->lock) {
    dl_log_add (LOG_DEF, 0, "trying to del locked key (%d)(key name = %s)(queue pointer = %p)(timeout = %d)!\n", k->lock, k->name, k->q, k->timeout);
    add_entry_time (k, 100);
    return;
  }
//  dl_log_add (LOG_DEF, 0, "trying to del key %p (%d)(key name = [%s]:%p)(queue pointer = %p)(timeout = %d)!\n", k, k->lock, k->name, k->name, k->q, k->timeout);

  assert (k->lock == 0);

  if (k->prev_st != NULL) {
    k->prev_st->ref_cnt--;
  }
  k->q->keys_cnt--;
  queue_fix (k->q);

  shmap_pair_string_vptr a;
  a.y = NULL;
  a.x = k->name;

  if (verbosity > 2) {
    fprintf (stderr, "Key deleted : [%s]\n", k->name);
  }
  dbg ("Key deleted : [%s]\n", k->name);
  shmap_string_vptr_del (&h_qkey, a);


  str_memory -= dl_get_memory_used();
  dl_strfree (k->name);
  str_memory += dl_get_memory_used();

  qkey_free (k);
}

void qkey_fix_timeout (qkey *k) {
  if (k->next_time != NULL) {
    del_entry_time (k);
  }
  add_entry_time (k, 0);
}

void qkey_clear_conn (char *kname) {
  qkey *k = qkey_get (kname, 0);
  if (k != NULL) {
    k->conn = NULL;
  }
}


inline char char_to_int (char c) {
  if ('A' <= c && c <= 'Z') {
    return c - 'A';
  }
  if ('a' <= c && c <= 'z') {
    return c - 'a' + 26;
  }
  if ('0' <= c && c <= '9') {
    return c - '0' + 26 + 26;
  }
  assert (0);
}


int check_qname (char *qname, qtype tp) {
  int i = 0;

  if (qname == NULL) {
    return 0;
  }

  if (tp != get_qtype (qname)) {
    return 0;
  }

  while (i <= MAX_QNAME && qname[i]) {
    if (!('A' <= qname[i] && qname[i] <= 'Z') && !('a' <= qname[i] && qname[i] <= 'z') &&
        !('0' <= qname[i] && qname[i] <= '9') && !(qname[i] == '_')) {
      return 0;
    }

    i++;
  }

  return 0 < i && i <= MAX_QNAME;
}


char *get_timestamp_key (char *qname, int id, int ip, int timeout, char *extra, qtype tp) {
  ip &= 0xFFFFF000;
  //TODO : failed codes
  if (!check_qname (qname, tp)) {
    return "{\"failed\":\"4\"}";
  }

  queue *q = get_queue (qname, 1);
  assert (q != NULL);

  static char tmp[KEY_LEN + 1];

  dbg ("Before encode_key\n");
  encode_key (tmp, qname, id, ip, get_secret (id));
  dbg ("After encode_key\n");

  tmp[KEY_LEN] = 0;
  qkey *k = qkey_get (tmp, 1);

  if (q->ev_last != NULL) {
    k->st = q->ev_last;
//    k->st = (event *)q;
  } else {
    k->st = (event *)q;
  }

  k->q = q;
  k->ts = rand() % 2000000000;
  k->lock = 0;
  k->st->ref_cnt++;
  k->q->keys_cnt++;

  k->timeout = timeout;
  dbg ("timeout = %d\n", timeout);
  k->next_time = k->prev_time = NULL;

  k->prev_ts = k->ts;
  k->prev_st = k->st;

  qkey_fix_timeout (k);

  ds = debug_buff;
  debug ("{\"ts\":\"%d\",\"key\":\"", k->ts);
  ds += write_str (ds, tmp, KEY_LEN);
  debug ("\"}");

  assert (k->st->next == NULL);

  return debug_buff;
}

int may_wait (char *s) {
  return __may_wait;
}

int quick_fix;
char *get_events_http (qkey *k) {
  if (!quick_fix) {
    ds = debug_buff;
  }

  qkey_fix_timeout (k);

  k->ts = k->prev_ts + 1;
  debug ("{\"ts\":\"%d\"", k->ts);

  int f = 0;
  debug (",\"events\":[");

  event *e = k->st;

  double cur_time = get_utime (CLOCK_MONOTONIC);
  while (e->next != NULL) {
    e = e->next;

    if (f) {
      debug (",");
    } else {
      __may_wait = 0;
      f = 1;
    }

    if (ds + e->data_len > debug_buff + (1 << 23)) {
      critical ("Queue %s overflow\n", k->q->name);
      debug_error = 1;
      break;
    }
    assert (ds + e->data_len < debug_buff + (1 << 24));
    assert (e->data_len > 0);
    if (e->data[0] == 2) {
      debug ("%s", e->data + 1);
    } else {
      debug ("\"");
      ds += write_str (ds, e->data, e->data_len - 1);
      debug ("\"");
    }

    double cur_delay = cur_time - max (e->created, k->subscribed);
    sum_delay += cur_delay;
    cnt_delay++;
    events_sent++;

    if (cur_delay > max_delay) {
      max_delay = cur_delay;
//      fprintf (stderr, "%lf\n", max_delay);
    }
  }
  k->st = e;

  debug ("]}");

  if (debug_error) {
    http_failed[3]++;
    buff_overflow_cnt++;
    debug_error = 0;
    ds = debug_buff;
    debug ("{\"failed\":3}");
  }

  queue_fix (k->q);

  return debug_buff;
}

char *get_events_http_group (qkey_group *k) {
  ds = debug_buff;
  __may_wait = 1;

  quick_fix = 1;
  if (k->n > 1) {
    debug ("[");
  }
  int i, http_ok = 0;
  for (i = 0; i < k->n; i++) {
    if (k->k[i] != NULL) {
      get_events_http (k->k[i]);
      http_ok++;
    } else {
      int err = (int)k->r[i];
      debug ("{\"failed\":2,\"err\":%d}", err);
      __may_wait = 0;
      http_failed[2 + err / 4]++;
    }
    if (debug_error) {
      quick_fix = 0;

      http_failed[3]++;
      buff_overflow_cnt++;

      debug_error = 0;
      ds = debug_buff;
      debug ("{\"failed\":3}");

      return debug_buff;
    }
    if (i + 1 != k->n) {
      debug (",");
    }
  }
  if (k->n > 1) {
    debug ("]");
  }
  if (!__may_wait) {
    http_failed[0] += http_ok;
  }

  quick_fix = 0;
  return debug_buff;
}


int hex_to_int (char *s, int *x) {
  int i;
  *x = 0;
  for (i = 0; i < 8; i++) {
    if ('0' <= s[i] && s[i] <= '9') {
      *x = (*x) * 16 + s[i] - '0';
    } else if ('a' <= s[i] && s[i] <= 'f') {
      *x = (*x) * 16 + s[i] - 'a' + 10;
    } else {
      return 0;
    }
  }
  return 1;
}

int decode_key (char *key, char *qname, int *id, int *ip, int *secret) {
  static char s[KEY_LEN + 1];
  int i, j;

  for (i = 0; i < KEY_LEN; i++) {
    if (!('A' <= key[i] && key[i] <= 'Z') && !('a' <= key[i] && key[i] <= 'z') &&
        !('0' <= key[i] && key[i] <= '9') && !(key[i] == '_')) {
      return 0;
    }
  }

  dl_crypto_decode (&cr, key, s);

  i = 0;
  while (i < MAX_QNAME && s[i] != '_') {
    i++;
  }

  if (i == MAX_QNAME) {
    return 0;
  }

  int len = MAX_QNAME - i;
  memcpy (qname, s + i + 1, len);
  qname[len] = 0;

  for (j = 0; j < i; j++) {
    if ((j * j + qname[j % len]) % 26 + 'a' != s[j]) {
      return 0;
    }
  }

  if (!hex_to_int (s + MAX_QNAME + 1, id)) {
    return 0;
  }

  if (!hex_to_int (s + MAX_QNAME + 1 + 8, ip)) {
    return 0;
  }

  if (!hex_to_int (s + MAX_QNAME + 1 + 8 + 8, secret)) {
    return 0;
  }

  return 1;
}

void encode_key (char *key, char *qname, unsigned int id, unsigned int ip, unsigned int secret) {
//  fprintf (stderr, "encode <%s|%d|%u|%d>\n", qname, id, ip, secret);
  dbg ("In encode_key %s %u %u %u\n", qname, id, ip, secret);
  static char s[KEY_LEN + 1];

  int len = strlen (qname);
  dbg ("After strlen\n");
  assert (0 < len && len <= MAX_QNAME);

  int i = MAX_QNAME - len, j;

  for (j = 0; j < i; j++) {
    s[j] = (j * j + qname[j % len]) % 26 + 'a';
  }
  s[i] = '_';
  memcpy (s + i + 1, qname, len);

  sprintf (s + MAX_QNAME + 1, "%08x", id);
  sprintf (s + MAX_QNAME + 1 + 8, "%08x", ip);
  sprintf (s + MAX_QNAME + 1 + 16, "%08x", secret);

  dbg ("Running dl_crypto_encode %s\n", s);
  dl_crypto_encode (&cr, s, key);
  key[KEY_LEN] = 0;
  dbg ("Leaving encode_key %s\n", key);
}

qkey *validate_key (char *key_name, int id, int ip, int req_ts, int a_release, char *err) {
  ip &= 0xFFFFF000;
  static char qname[MAX_QNAME + 1], kname[KEY_LEN + 1];
  int t_id, t_ip, t_secret;

  memcpy (kname, key_name, KEY_LEN);
  kname[KEY_LEN] = 0;

//  fprintf (stderr, "before decode\n");
  int secret = get_secret (id);
  if (!decode_key (key_name, qname, &t_id, &t_ip, &t_secret)) {
    if (now - start_time > 600) {
      dl_log_add (LOG_WARNINGS, 1, "Now = %.6lf. Failed : 2, a_release = %d, err = 1. Can't decode. id = %d, ip = %d, key_name = %s\n", precise_now, a_release, id, ip, kname);
    }
    *err = 1;
    return NULL;
  }

//  fprintf (stderr, "qname = %s\n", qname);
  if (my_verbosity > 1) {
    fprintf (stderr, "(%08x %08x) (%08x %08x) (%08x %08x)\n", t_id, id, t_ip, ip, t_secret, secret);
  }
  if (t_id != id || t_ip != ip || t_secret != secret) {
    if (t_secret == secret) {
      dl_log_add (LOG_WARNINGS, 2, "Now = %.6lf. Failed : 2, a_release = %d, err = 2. id or ip mismatch. t_id != id || t_ip != ip : %d != %d || %u != %u\n",
                                    precise_now, a_release, t_id, id, t_ip, ip);
    }
    *err = 2;
    return NULL;
  }

  qkey *k = qkey_get (kname, 0);
//  fprintf (stderr, "get k ok\n");

  if (k == NULL) {
    dl_log_add (LOG_WARNINGS, 3, "Now = %.6lf. Failed : 2, a_release = %d, err = 3. Key expired. Queue = %s\n", precise_now, a_release, qname);
    *err = 3;
    return NULL;
  }

  if (k->lock && !a_release) {
    dl_log_add (LOG_WARNINGS, 4, "Now = %.6lf. Failed : 2, a_release = 0, err = 4. Key locked. Queue = %s\n", precise_now, qname);
    *err = 4;
    return NULL;
  }

  //fprintf (stderr, "get q[%s]\n", qname);
  queue *q = get_queue (qname, 0);
  if (q == NULL) {
    assert (0);
    return NULL;
  }

//  fprintf (stderr, "get k\n");
  assert (q == k->q);
//  fprintf (stderr, "return %d %d %d\n", req_ts, k->prev_ts, k->ts);


  if (req_ts == k->prev_ts) {
    if (!a_release) {
      k->ts = k->prev_ts;
      k->st = k->prev_st;
    }

    return k;
  } else if (req_ts == k->ts) {
    if (!a_release) {
      k->prev_st->ref_cnt--;
      k->prev_st = k->st;
      k->prev_ts = k->ts;
      k->prev_st->ref_cnt++;
      queue_fix (k->q);
    }

    return k;
  }

  dl_log_add (LOG_WARNINGS, 5, "Now = %.6lf. Failed : 2, a_release = %d, err = 5. Mismatch ts. %d != %d && %d != %d. Queue = %s\n", precise_now, a_release, req_ts, k->prev_ts, req_ts, k->ts, qname);
  *err = 5;
  return NULL;
}

qkey_group *qkey_group_alloc (int n) {
  keys_memory -= dl_get_memory_used();

  qkey_group *r = dl_malloc (sizeof (qkey_group));
  r->n = n;
  r->k = dl_malloc (sizeof (qkey *) * n);
  r->r = dl_malloc0 (sizeof (char) * n);

  keys_memory += dl_get_memory_used();

  return r;
}

void qkey_group_free (qkey_group *k) {
  keys_memory -= dl_get_memory_used();
  dl_free (k->r, sizeof (char) * k->n);
  dl_free (k->k, sizeof (qkey *) * k->n);
  dl_free (k, sizeof (qkey_group));
  keys_memory += dl_get_memory_used();
}

qkey_group *validate_key_group (char *keys, int id, int ip, int *req_ts, int req_ts_n, int a_release) {
  int kn = strlen (keys);
  if (kn % KEY_LEN) {
    dl_log_add (LOG_WARNINGS, 1, "Failed : 2. kn %% KEY_LEN : %d %% %d\n", kn, KEY_LEN);
    return NULL;
  }
  kn /= KEY_LEN;
  if (kn != req_ts_n || kn <= 0) {
    dl_log_add (LOG_WARNINGS, 1, "Failed : 2. kn != req_ts_n || kn <= 0 : %d != %d || %d <= 0\n", kn, req_ts_n, kn);
    return NULL;
  }

  int i, j;
  for (i = 0; i < kn; i++) {
    for (j = i + 1; j < kn; j++) {
      if (!memcmp (keys + KEY_LEN * i, keys + KEY_LEN * j, KEY_LEN)) {
        dl_log_add (LOG_WARNINGS, 1, "Failed : 2. Key %d == key %d\n", i, j);
      }
    }
  }

  qkey_group *r = qkey_group_alloc (kn);
  for (i = 0; i < kn; i++) {
    r->k[i] = validate_key (keys + KEY_LEN * i, id, ip, req_ts[i], a_release, &r->r[i]);

    /*
    if (r->k[i] == NULL && !a_release) {
      qkey_group_free (r);
      return NULL;
    }
   */

    if (r->k[i] != NULL && !r->k[i]->lock) {
      r->k[i]->subscribed = get_utime (CLOCK_MONOTONIC);
    }
  }

  return r;
}

void release_key_group (qkey_group *k) {
  int i;
  for (i = 0; i < k->n; i++) {
    if (k->k[i] != NULL) {
      struct conn_query *Q = k->k[i]->conn;
      if (Q != NULL) {
        double wtime = precise_now; //get_utime (CLOCK_MONOTONIC);// + 0.01;

        if (Q->timer.wakeup_time > wtime || Q->timer.wakeup_time == 0) {
          Q->timer.wakeup_time = wtime;
          insert_event_timer (&Q->timer);
        }
      }
    }
  }
}

int get_queue_alias (char *s, ll *res) {
  if (!check_qname (s, get_qtype (s))) {
    return 0;
  }
  queue *q = get_queue (s, 1);
  *res = q->id;
  return 1;
}


#define MAX_DIFF 300000
pli to_add[MAX_DIFF],
    *gd_a, *gd_b,
    to_del[MAX_DIFF];
int to_add_n, to_del_n;

inline int insert_to_del (long long x) {
  if (to_del_n < MAX_DIFF) {
    to_del[to_del_n++].x = x;
    return 1;
  } else {
    to_del_overflow++;
    return 0;
  }
}

inline int insert_to_add (long long x, int y) {
  if (to_add_n < MAX_DIFF) {
    to_add[to_add_n].x = x;
    to_add[to_add_n].y = y;
    to_add_n++;
    return 1;
  } else {
    to_add_overflow++;
    return 0;
  }
}


void gen_diff_go (treap_node_ptr v) {
  if (v == NULL) {
    return;
  }
  gen_diff_go (v->l);

  while (gd_a != gd_b && gd_a->x > v->x) {
    insert_to_add (gd_a->x, gd_a->y);
    gd_a++;
  }
  if (gd_a == gd_b || gd_a->x != v->x) {
    insert_to_del (v->x);
  } else {
    if (gd_a->y != v->val) {
      insert_to_add (gd_a->x, gd_a->y);
    }
    gd_a++;
  }

  gen_diff_go (v->r);
}



void cnt_srt (pli *a, int n, pli *b) {
  static int was[10001] = {0},
             num[10001] = {0},
             st[10001] = {0},
             vn = 0, id = 0;

  assert (0 < engine_n && engine_n <= 10000);

  if (++id == 0) {
    memset (was, 0, sizeof (was));
    id = 1;
  }

  int i;
  vn = 0;
  st[0] = 0;
  for (i = 0; i < n; i++) {
    int x = dl_abs (a[i].x) % engine_n;
    if (was[x] != id) {
      num[x] = vn;
      was[x] = id;
      vn++;
      st[vn] = 0;
    }

    st[num[x] + 1]++;
  }
  for (i = 0; i < vn; i++) {
    st[i + 1] += st[i];
  }
  for (i = 0; i < n; i++) {
    b[st[num[dl_abs (a[i].x) % engine_n]]++] = a[i];
  }
}

#define MAX_SUBSCR 6000000
pli sb[MAX_SUBSCR], sb2[MAX_SUBSCR];


void process_changes (ll id, treap *t, pli *a, int n, int is_add, int is_rev) {
  if (n == 0) {
    return;
  }

  if (n > MAX_SUBSCR) {
    dl_log_add (LOG_DEF, 0, "Too many subscribers %d [id = %lld]\n", n, id);
    n = MAX_SUBSCR;
  }
  cnt_srt (a, n, sb);

  STAT (process_changes_cnt);
  STAT_MAX (changes_len_max, n);
  process_changes_total_len += n;

  if (is_add) {
    if (is_rev) {
       STAT (changes_add_rev_cnt);
       changes_add_rev_len += n;
    } else {
       STAT (changes_add_cnt);
       changes_add_len += n;
    }
  } else {
    if (is_rev) {
       STAT (changes_del_rev_cnt);
       changes_del_rev_len += n;
    } else {
       STAT (changes_del_cnt);
       changes_del_len += n;
    }
  }

  int i, j, f = 0;
  for (i = j = 0; i <= n; i++) {
    if (i < n) {
      //dbg ("CHNG TO SEND : %lld %s (%lld;%d)\n", id, is_add ? "add" : "del", sb[i].x, sb[i].y);

      if (t != NULL) {
        treap_cnt += t->size;
//          dbg ("add subsc_local %d->[%d,%d] (treap : %p %d)\n",
//                  id, sb[i].x, sb[i].y, t, t->size);
        if (is_add) {
          treap_add (t, sb[i].x, sb[i].y, rand());
        } else {
          treap_del (t, sb[i].x);
        }

        treap_cnt -= t->size;
      }
    }
    if (i == n || dl_abs (sb[i].x) % engine_n != dl_abs (sb[j].x) % engine_n) {
      f = 1;
      j = i;
    }
  }
  if (f) {
  }
}


void clear_subscr (queue *q) {
  dbg ("clear_subscr: (q = %p), (qname = %s)\n", q, q->name);
  if (-q->subs->size > MAX_DIFF) {
    to_del_overflow += -q->subs->size - MAX_DIFF;
  }
  to_del_n = treap_conv_to_array (q->subs->root, to_del, MAX_DIFF);
  process_changes (q->id, NULL, to_del, to_del_n, 0, 0);

  treap_cnt += q->subs->size;
  treap_free (q->subs);
  treap_cnt -= q->subs->size;
}




/***
 Subscribers
 ***/
#pragma pack(push,4)
typedef struct t_subscribers subscribers;
struct t_subscribers {
  ll id;
  treap s;
  queue *q;
};
#pragma pack(pop)

hset_llp h_subscribers;

subscribers *alloc_subscribers (void) {
  subscribers_cnt++;

  subscribers_memory -= dl_get_memory_used();
  subscribers *res = dl_malloc0 (sizeof (subscribers));
  subscribers_memory += dl_get_memory_used();

  return res;
}

void free_subscribers (subscribers *s) {
  assert (s->s.size == 0);
  hset_llp_del (&h_subscribers, &s->id);
  subscribers_cnt--;

  if (s->q != NULL) {
    s->q->subscr_cnt--;
    queue_fix (s->q);
  }

  subscribers_memory -= dl_get_memory_used();
  dl_free (s, sizeof (subscribers));
  subscribers_memory += dl_get_memory_used();
}

inline subscribers *get_subscribers (ll id, int force) {
  subscribers **b;

  // FAIL: TOO MUCH LOG DATA dbg ("get_subscribers %lld (force = %d)\n", id, force);

  if (force) {
    queue *q = NULL;

    if (!IS_UID (id)) {
      q = get_queue_by_alias (id);
      if (q == NULL) {
        return NULL;
      }
    }

    b = (subscribers **)hset_llp_add (&h_subscribers, &id);
//    fprintf (stderr, "%p ?? %p (%p;%p)\n", *b, (subscribers *)&id, &id, (int *)*b);
//    fprintf (stderr, "");
    if (*b == (subscribers *)&id) {
      subscribers *w = alloc_subscribers();
      if (!IS_UID (id)) {
        assert (q != NULL);
        w->q = q;
        q->subscr_cnt++;
      }

      w->id = id;
      treap_init (&w->s);

      if (my_verbosity > 1) {
        fprintf (stderr, "subcribers_created id = %lld\n", id);
      }

      *b = w;
    }
  } else {
    b = (subscribers **)hset_llp_get (&h_subscribers, &id);
    if (b == NULL) {
      return NULL;
    }
  }
  return *b;
}


int subscribers_add_new (ll id, pli *a, int n) {
  int i;
  for (i = 0; i < n; i++) {
    subscribers *s = get_subscribers (a[i].x, 1);
//    dbg ("add subscriber %d->%d[%d] %p\n", id, a[i].x, a[i].y, s);

    if (s != NULL) {
      treap_cnt_rev += s->s.size;
      treap_add (&s->s, id, a[i].y, rand());
      treap_cnt_rev -= s->s.size;

      //dbg ("add subscriber %lld->%lld[%d] (treap_size = %d)\n", id, a[i].x, a[i].y, s->s.size);
    }
  }

  return 1;
}

int subscribers_del_old (ll id, pli *a, int n) {
  int i;
  for (i = 0; i < n; i++) {
    subscribers *s = get_subscribers (a[i].x, 0);
    if (s != NULL) {
      treap_cnt_rev += s->s.size;
      treap_del (&s->s, id);
      treap_cnt_rev -= s->s.size;

      if (s->s.size == 0) {
        free_subscribers (s);
      }
      //dbg ("del subscriber %lld->%lld\n", id, a[i].x);
    }
  }

  return 1;
}

int subscribers_add_new_rev (ll id, pli *a, int n) {
  //TODO: force?
  subscribers *s = get_subscribers (id, 0);
  if (s != NULL) {
    treap_cnt_rev += s->s.size;

    int i;
    for (i = 0; i < n; i++) {
      treap_add (&s->s, a[i].x, a[i].y, rand());
      //dbg ("add subscriber(lazy) %lld->%lld\n", a[i].x, id);
    }
    treap_cnt_rev -= s->s.size;
  }

  return 1;
}

int subscribers_del_rev (ll id, pli *a, int n) {
  subscribers *s = get_subscribers (id, 0);
  if (s != NULL) {
    treap_cnt_rev += s->s.size;

    int i;
    for (i = 0; i < n; i++) {
      treap_del (&s->s, a[i].x);
      //dbg ("del subscriber(lazy) %lld->%lld\n", a[i].x, id);
    }
    treap_cnt_rev -= s->s.size;

    if (s->s.size == 0) {
      free_subscribers (s);
    }
  }

  return 1;
}



int do_add_event (char *qname, int qname_len, char *data, int data_len, int x, int y, int ttl) {
  if (verbosity > 2) {
    fprintf (stderr, "Queue name [%s], data %d:[%s] { & %d = %d} ttl = %d\n", qname, data_len, data, x, y, ttl);
  }

  //TODO check qname
  if (qname_len <= 0 || data_len <= 0 || qname_len > MAX_QNAME || data_len > 32768) {
    return 0;
  }

  return add_event (get_queue (qname, 0), data, data_len, x, y, ttl);
}

int init_all (void) {
  assert (sizeof (ll) == 2 * sizeof (int));
  assert (sizeof (ll) == sizeof (long long));
  assert (sizeof (pli) == sizeof (int) * 3);

  assert (offsetof (queue, ev_first) == offsetof (event, next));
  assert (offsetof (queue, ref_cnt) == offsetof (event, ref_cnt));
  assert (offsetof (queue, first_q) == offsetof (struct connection, first_query));
  assert (offsetof (queue, last_q) == offsetof (struct connection, last_query));
  assert (offsetof (queue, first_q) == offsetof (struct conn_query, next));
  assert (offsetof (queue, last_q) == offsetof (struct conn_query, prev));
  assert (offsetof (subscribers, id) == 0);

  assert (engine_n <= 10000);

  max_events_memory = max_memory / 2;

  shmap_string_vptr_init (&h_queue);
  shmap_string_vptr_init (&h_qkey);
  map_int_int_init (&secrets);
  map_ll_vptr_init (&alias);

  hset_llp_init (&h_subscribers);

  int i;
  for (i = 0; i < TIME_TABLE_SIZE; i++) {
    time_st[i] = qkey_malloc();
    time_st[i]->next_time = time_st[i]->prev_time = time_st[i];
  }
  time_keys_memory = keys_memory;

  keys_memory = 0;
  keys_cnt = 0;

  last_del_time = GET_TIME_ID (get_utime (CLOCK_MONOTONIC));

  crypto_memory -= dl_get_memory_used();
  dl_crypto_init (&cr, MAX_QNAME + 1 + 8 + 8 + 8, RSEED_LEN, 919998317, 999983, time (NULL));
  crypto_memory += dl_get_memory_used();

  return 1;
}

void free_all (void) {
  if (verbosity) {
    //TODO close all connections
    if (http_sfd != -1) {
      epoll_close (http_sfd);
      assert (close (http_sfd) >= 0);
    }

    keys_cnt += TIME_TABLE_SIZE;
    int i;
    for (i = 0; i < TIME_TABLE_SIZE; i++) {
      while (time_st[i]->next_time != time_st[i]) {
        if (time_st[i]->next_time->lock) {
          dbg ("Problem with key[%s] timeout = %d\n", time_st[i]->next_time->name,
               time_st[i]->next_time->timeout);
        }
        assert (!time_st[i]->next_time->lock);
//        fprintf (stderr, "delete qkey\n");
        delete_qkey (time_st[i]->next_time);
      }
      qkey_free (time_st[i]);
    }
    assert (keys_cnt == 0);

//    fprintf (stderr, "qs = %p %ld\n", qs, sizeof (queue));
    while (qs != NULL) {
      queue *q = qs;
      qs = (queue *)qs->ev_first;
      dl_free (q, sizeof (queue));
    }

    shmap_string_vptr_free (&h_queue);
    shmap_string_vptr_free (&h_qkey);
    map_int_int_free (&secrets);
    map_ll_vptr_free (&alias);

    hset_llp_free (&h_subscribers);

    dl_crypto_free (&cr);

    long long left_memory = dl_get_memory_used() - treap_get_memory();

    fprintf (stderr, "Memory left: %lld\n", left_memory);
    assert (left_memory == 0);
  }
}

long get_htbls_memory (void) {
  return shmap_string_vptr_get_memory_used (&h_queue) +
         shmap_string_vptr_get_memory_used (&h_qkey) +
         map_int_int_get_memory_used (&secrets) +
         map_ll_vptr_get_memory_used (&alias) +
         hset_llp_get_memory_used (&h_subscribers);
}

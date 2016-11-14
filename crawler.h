#ifndef __CRAWLER_H
#define __CRAWLER_H

#include <stdlib.h>
#include <pthread.h>
#include <semaphore.h>
#include <time.h>

typedef struct __node_t {
  char* src;
  char* content;
  struct __node_t *next;
} node_t;

void Node_Free(node_t *node);

// First node in queue has no link
typedef struct __queue_t {
  node_t *head;
  node_t *tail;
  pthread_mutex_t lock;
  sem_t empty; // max size of the queue - number of elements in queue
  sem_t full; // number of elements in queue
  size_t bounded; // Indicates if the queue has a max size and what the max size is.
  // Should this queue cause the program to exit when an error occurs.
  // also the exit value.
  char const * name;
  char const * err_msg;
  size_t panic_on_error;
  struct timespec timeout;
} queue_t;

int Queue_Init(queue_t *q, size_t size, size_t panic_on_error);
int Queue_Enqueue(queue_t *q, node_t* node);
int Queue_Dequeue(queue_t *q, node_t **node);
void Queue_Print(queue_t *q);

int crawl(
  char *start_url,
  int download_workers,
  int parse_workers,
  int queue_size,
  char * (*fetch_fn)(char *link),
  void (*edge_fn)(char *from, char *to)
);

node_t* ParseLinks(node_t* content);

typedef struct __hash_item {
  void* data;
  char* key;
  int present;
} hash_item;

typedef struct __hash_table {
  hash_item* table;
  size_t size;
  size_t current_size;
  pthread_mutex_t lock;
} hash_table;

// based on fletcher checksum
int hash(char* key);
int hash_init(hash_table* t, size_t size);
// returns * to item in hash table
hash_item hash_get(hash_table* hash_table, char* key);
void hash_insert(hash_table* t, hash_item item);
void hash_remove(hash_table* t, char* key);
void hash_print(hash_table* t);
void hash_free(hash_table* t);
#endif

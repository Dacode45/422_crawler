#include <stdlib.h>
#include <stdarg.h>
#include <unistd.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stdint.h>
#include <pthread.h>
#include <regex.h>

#include "crawler.h"

char * strcpy_alloc(char* str) {
  if (str == NULL) {
    return NULL;
  }
  char *tmp = (char*)calloc(strlen(str)+1, sizeof(char));
  if (tmp == NULL) {
    return NULL;
  }
  strcpy(tmp, str);
  return tmp;
}

int debugf(char const *fmt, ...) {
  int ret = 0;

  #ifdef DEBUG
    va_list myargs;
    va_start(myargs, fmt);
    ret = vprintf(fmt, myargs);
    va_end(myargs);
  #endif
  return ret;
}

regex_t regex;

typedef struct __crawler_arg {
  queue_t* link_q;
  queue_t* content_q;
  // Use our concurrent queue for when threads finish
  // src of a node is the thread_index
  // content of a node is the return value
  queue_t* finished_q;
  hash_table* seen_links;
  node_t* retv;
  char * (*fetch_fn)(char *link);
  void (*edge_fn)(char *from, char *to);
} crawler_arg;

// Whether there is content parsing.
// Prevents downloader threads from exiting while parsers run.
sem_t contents_parsing;
// similar to the above but parsers wait;
sem_t links_fetching;

void *downloader(void *raw_arg);
void *parser(void *raw_arg);

int crawl(char *start_url,
	  int download_workers,
	  int parse_workers,
	  int queue_size,
	  char * (*_fetch_fn)(char *url),
	  void (*_edge_fn)(char *from, char *to)) {

  int has_err = 0;

  //Compile regex.
  if(regcomp(&regex, "link:([a-z|0-9|\\/|:|\\.]+)", REG_EXTENDED | REG_ICASE)){
    perror("Could not complie regex\n");
    return(1);
  }
  sem_init(&contents_parsing, 0, 0);
  sem_init(&links_fetching, 0, 0);

  queue_t link_q;
  link_q.name = "links";
  queue_t content_q;
  content_q.name = "content";
  queue_t finished_q;
  finished_q.name = "finished";

  Queue_Init(&link_q, queue_size, 1);
  Queue_Init(&content_q, queue_size, 1);
  Queue_Init(&finished_q, (download_workers + parse_workers), 1);

  node_t *first_link_node = (node_t*)malloc(sizeof(node_t));
  first_link_node->src = NULL;
  first_link_node->content = strcpy_alloc(start_url);
  Queue_Enqueue(&link_q, first_link_node);

  hash_table seen_links;
  hash_init(&seen_links, 1);
  hash_item first_link;
  first_link.data = NULL;
  first_link.key = start_url;
  hash_insert(&seen_links, first_link);

  size_t should_cancel = 0;
  if (parse_workers <= 0) {
    parse_workers = 1;
  }
  if (download_workers <= 0) {
    download_workers = 1;
  }

  int *finished = (int*)malloc((parse_workers+download_workers)*sizeof(int));
  node_t* retvs = (node_t*)malloc((parse_workers+download_workers)*sizeof(node_t));
  crawler_arg *crawler_args = (crawler_arg*)malloc((parse_workers+download_workers)*sizeof(crawler_arg));
  pthread_t* parsers = (pthread_t *)malloc(parse_workers * sizeof(pthread_t));
  pthread_t* downloaders = (pthread_t *)malloc(download_workers * sizeof(pthread_t));

  // spawn threads parsers first;
  int i = 0;
  for (; i < parse_workers; i++ ) {
    crawler_arg args = crawler_args[i];
    node_t* retv = &retvs[i];

    // src of retv is index of thread. content is return value.
    retv->src = (char *)(long)i;
    args.link_q = &link_q;
    args.content_q = &content_q;
    args.finished_q = &finished_q;
    args.seen_links = &seen_links;
    args.retv = retv;
    args.fetch_fn = _fetch_fn;
    args.edge_fn = _edge_fn;

    pthread_create(&parsers[i], NULL, parser, &args);
    finished[i] = 1;
  }
  if (!should_cancel) {
    for (i = parse_workers; i < (parse_workers + download_workers); i++) {
      crawler_arg args = crawler_args[i];
      node_t* retv = &retvs[i];

      // src of retv is index of thread. content is return value.
      retv->src = (char *)(long)i;
      args.link_q = &link_q;
      args.content_q = &content_q;
      args.finished_q = &finished_q;
      args.seen_links = &seen_links;
      args.retv = retv;
      args.fetch_fn = _fetch_fn;
      args.edge_fn = _edge_fn;

      pthread_create(&downloaders[i-parse_workers], NULL, downloader, &args);
      finished[i] = 1;
    }
  }
  // cleanup
  if (!should_cancel) {
    for (i = 0; i < (parse_workers + download_workers); i++) {
      node_t* retv[1];
      Queue_Dequeue(&finished_q, retv);
      int thr_index = (int)(long)retv[0]->src;
      should_cancel = (int)(long)retv[0]->content;
      if (thr_index < parse_workers) {
        pthread_join(parsers[thr_index], NULL);
        finished[thr_index] = 2;
      } else {
        pthread_join(downloaders[thr_index-parse_workers], NULL);
        finished[thr_index] = 2;
      }
      debugf("freeing thread %i, returned %zu, %p\n", thr_index, should_cancel, retv[0]);
      if (should_cancel) {
        break;
      }
    }
  }

  if (should_cancel) {
    for (i = 0; i < (parse_workers + download_workers); i++) {
      if (finished[i] == 1) {
        if (i < parse_workers) {
          pthread_cancel(parsers[i]);
        } else {
          pthread_cancel(downloaders[i - parse_workers]);
        }
      }
    }
  }

  // cleanup.
  hash_free(&seen_links);
  free(finished);
  free(retvs);
  free(crawler_args);
  free(parsers);
  free(downloaders);
  return has_err;
}

void *downloader(void *raw_arg) {
  pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
  crawler_arg *arg = (crawler_arg *)raw_arg;
  node_t *current_link[1];
  current_link[0] = NULL;
  char *current_content = NULL;
  int link_q_size = 0;
  int content_q_size = 0;
  int c_parsing = 0;
  int l_fetching = 0;
  int double_tap = 0; // Checks semaphores twice before exiting.
  int has_err = 0;

  while(1) {
    sem_getvalue(&arg->link_q->full, &link_q_size);
    sem_getvalue(&arg->content_q->full, &content_q_size);
    sem_getvalue(&contents_parsing, &c_parsing);
    sem_getvalue(&links_fetching, &l_fetching);
    if (!link_q_size && !content_q_size) { // Both q's are empty.
      if (c_parsing) { // content is being parsed somewhere
        double_tap = 0;
        continue;
      }
      if (l_fetching) { // links are being fetched somewhere
        double_tap = 0;
        continue;
      }
      // queues are empty, and nothing is parsing or fetching
      if (double_tap == 2) {
        break;
      } else {
        double_tap++;
      }
    } else {
      double_tap = 0;
    }

    // debugf("In Downloader %lu, content q size:%i, link q size: %i\n",(long)arg->retv->src, content_q_size, link_q_size);
    sem_post(&links_fetching);
    if (link_q_size) {
      // get a link
      Queue_Dequeue(arg->link_q, current_link);
      // get content
      current_content = arg->fetch_fn((current_link[0])->content);
      if (current_content != NULL) {
        node_t *tmp = (node_t*)malloc(sizeof(node_t));
        tmp->src = strcpy_alloc(current_link[0]->content);
        tmp->content = current_content;
        tmp->next = NULL;
        Queue_Enqueue(arg->content_q, tmp);
        Node_Free(current_link[0]);
      }
    }
    sem_wait(&links_fetching);
  }
  arg->retv->content = (char *)(long)(has_err);
  debugf("Downloader %lu Exiting\n", (long)arg->retv->src);
  Queue_Enqueue(arg->finished_q, arg->retv);
  return NULL;
}

void *parser(void *raw_arg) {
  pthread_setcancelstate(PTHREAD_CANCEL_ENABLE, NULL);
  crawler_arg *arg = (crawler_arg *)raw_arg;
  node_t *first_link = NULL;
  node_t *current_content[1];
  int link_q_size = 0;
  int content_q_size = 0;
  int c_parsing = 0;
  int l_fetching = 0;
  int double_tap = 0; // Checks semaphores twice before exiting.
  int has_err = 0;

  while(1) {
    sem_getvalue(&arg->link_q->full, &link_q_size);
    sem_getvalue(&arg->content_q->full, &content_q_size);
    sem_getvalue(&contents_parsing, &c_parsing);
    sem_getvalue(&links_fetching, &l_fetching);
    if (!link_q_size && !content_q_size) { // Both q's are empty.
      if (c_parsing) { // content is being parsed somewhere
        double_tap = 0;
        continue;
      }
      if (l_fetching) { // links are being fetched somewhere
        double_tap = 0;
        continue;
      }
      // queues are empty, and nothing is parsing or fetching
      if (double_tap == 2) {
        break;
      } else {
        double_tap++;
      }
    } else {
      double_tap = 0;
    }

    debugf("In Parser %lu, content q size:%i, link q size: %i\n",(long)arg->retv->src, content_q_size, link_q_size);
    sem_post(&contents_parsing);
    if (content_q_size) {
      // get a link
      Queue_Dequeue(arg->content_q, current_content);
      // Parse content
      first_link = ParseLinks(current_content[0]);
      // Store link
      Node_Free(current_content[0]);
      while(first_link != NULL) {
        char *from = first_link->src;
        char *to = first_link->content;
        arg->edge_fn(from, to);
        hash_item link = hash_get(arg->seen_links, to);
        if (link.present) {
          node_t* tmp = first_link->next;
          Node_Free(first_link);
          first_link = tmp;
          continue;
        }
        node_t* tmp = first_link;
        first_link = first_link->next;
        link.key = to;
        link.data = NULL;
        hash_insert(arg->seen_links, link);
        Queue_Enqueue(arg->link_q, tmp);
      }
    }
    sem_wait(&contents_parsing);
  }
  arg->retv->content = (char *)(long)(has_err);
  debugf("Parser %lu Exiting\n", (long)arg->retv->src);
  Queue_Enqueue(arg->finished_q, arg->retv);
  return NULL;
}

node_t* ParseLinks(node_t* page) {
  debugf("Parsing link, src: %s, content: %s\n", page->src, page->content);
  char* text = page->content;
  if (text == NULL) {
    return NULL;
  }
  char* p = text;
  const int groups = 2;
  regmatch_t m[groups];
  node_t* start_node = NULL;
  node_t* end_node = NULL;
  while(1) {
    int nomatch = regexec(&regex, p, groups, m, 0);
    if (nomatch) {
      return start_node;
    }
    if (start_node == NULL) {
      start_node = end_node = (node_t *)malloc(sizeof(node_t));
    } else {
      node_t* prev = end_node;
      end_node = (node_t*)malloc(sizeof(node_t));
      prev->next = end_node;
    }
    int start = m[1].rm_so + (p-text);
    int finish = m[1].rm_eo + (p-text);

    end_node->src = strcpy_alloc(page->src);
    if (end_node->src == NULL) {
      perror("memory allocation failed");
      exit(1);
    }
    end_node->content = (char *)malloc((finish-start + 1)*sizeof(char));
    if (end_node->content == NULL) {
      perror("memory allocation failed");
      exit(1);
    }
    memcpy(end_node->content, text+start, finish-start);
    end_node->content[finish-start] = '\0';
    end_node->next = NULL;
    p += m[0].rm_eo;
  }
  return start_node;
}

void Node_Free(node_t *node){
  if (node != NULL) {
    if (node->src != NULL) {
      free(node->src);
    }
    if (node->content != NULL) {
      free(node->content);
    }
    free(node);
  }
}
const int QUEUE_DEFAULT_MAX_SIZE = 1000;
int Queue_Init(queue_t *q, size_t size, size_t panic_on_error){
  q->panic_on_error = panic_on_error;
  q->head = q->tail = NULL;
  if (size) {
    q->bounded = size;
  } else {
    q->bounded = QUEUE_DEFAULT_MAX_SIZE;
  }
  pthread_mutex_init(&q->lock, NULL);
  sem_init(&q->empty, 0, q->bounded);
  sem_init(&q->full, 0, 0);
  return 0;
}

int Queue_Enqueue(queue_t *q, node_t *node) {
  node->next = NULL;
  sem_wait(&q->empty);
  debugf("Queueu %s End Enqueueing\n", q->name);
  Queue_Print(q);
  pthread_mutex_lock(&q->lock);
  if(q->tail != NULL) {
    q->tail->next = node;
  } else {
    q->head = q->tail = node;
  }
  pthread_mutex_unlock(&q->lock);
  debugf("Queue %s End Enqueue\n", q->name);
  sem_post(&q->full);
  return 0;
}

int Queue_Dequeue(queue_t *q, node_t **node) {
  sem_wait(&q->full);
  debugf("Queueu %s Start Dequeueing\n", q->name);
  Queue_Print(q);
  pthread_mutex_lock(&q->lock);
  if (q->head == NULL) {
    q->head = q->tail;
  } if (q->head == NULL) {
    perror("queue dequeued when empty");
    pthread_mutex_unlock(&q->lock);
    return -1;
  }
  node_t *tmp = q->head;
  q->head = q->head->next;
  if (q->head == NULL) {
    q->head = q->tail = NULL;
  }
  node[0] = tmp;
  pthread_mutex_unlock(&q->lock);
  sem_post(&q->empty);
  debugf("Queue %s End Dequeue\n", q->name);
  return 0;
}

void Queue_Print(queue_t *q) {
  #ifdef DEBUG
  debugf("Printing contetns of q %s\n", q->name);
  node_t *node = q->head;
  while (node != NULL) {
    debugf("\tprinting NODE: %p\n", node);
    node = node->next;
  }
  #endif
  return;
}
// Implmentation of fletcher16 from https://en.wikipedia.org/wiki/Fletcher%27s_checksum
int hash(char* key) {
  if (key == NULL) {
    return 0;
  }
  uint16_t sum1 = 0xff, sum2 = 0xff;
  size_t tlen;
  size_t bytes = strlen(key);

  while (bytes) {
    tlen = ((bytes >= 20) ? 20 : bytes);
    bytes -= tlen;
    do {
      sum2 += sum1 += *(uint8_t*)key++;
      tlen--;
    } while (tlen);
    sum1 = (sum1 &0xff) + (sum1 >> 8);
    sum2 = (sum2 & 0xff) + (sum2 >> 8);
  }
  sum1 = (sum1 & 0xff) + (sum1 >> 8);
  sum2 = (sum2 & 0xff) + (sum2 >> 8);
  return (int)((sum2 << 8) | sum1);
}

int hash_init(hash_table* t, size_t size) {
  if (size <= 0) {
    size = 1;
  }
  hash_item* table = (hash_item*)calloc(size, sizeof(hash_item));
  if (table == NULL) {
    return -1;
  }
  t->table = table;
  t->size = size;
  t->current_size = 0;
  pthread_mutex_init(&t->lock, NULL);
  return 0;
}

hash_item hash_get(hash_table* t, char* key) {
  hash_item ret;
  ret.present = 0;
  if (key == NULL) {
    return ret;
  }
  int k = hash(key);
  debugf("hash table attempting to get item %s, %i\n", key, k);
  hash_print(t);
  int hashIndex = k % t->size;
  int looped = -1;
  int loopedIndex = hashIndex;
  pthread_mutex_lock(&t->lock);
  while(t->table[hashIndex].present) {
    if (hashIndex == loopedIndex) {
      if (++looped) {
        break;
      }
    }
    if(!strcmp(t->table[hashIndex].key, key)) {
      debugf("hash table found item %s\n", key);
      pthread_mutex_unlock(&t->lock);
      return t->table[hashIndex];
    }
    hashIndex++;
    hashIndex%= t->size;
  }

  pthread_mutex_unlock(&t->lock);
  debugf("hash table failed to find to get item %s\n", key);
  return ret;
}

void h_insert(hash_table* t, hash_item item){
  if (item.key == NULL) {
    return;
  }
  int k = hash(item.key);
  int hashIndex = k % t->size;
  if (t->current_size == t->size) {
    hash_item *old_table = t->table;
    hash_item *new_table = (hash_item *)calloc(2*t->size, sizeof(hash_item));
    int old_size = t->size;
    t->table = new_table;
    t->size = 2*old_size;
    t->current_size = 0;
    int i = 0;
    for(i = 0; i < old_size; i++) {
      if (old_table[i].present) {
        h_insert(t, old_table[i]);
      }
      if (old_table[i].key != NULL) {
        free(old_table[i].key);
      }
    }
    free(old_table);
  }
  while(t->table[hashIndex].present) {
    ++hashIndex;
    hashIndex %= t->size;
  }
  t->table[hashIndex].data = item.data;
  t->table[hashIndex].key = strcpy_alloc(item.key);
  t->table[hashIndex].present = 1;
  t->current_size++;
}

void hash_free(hash_table* t) {
  size_t i = 0;
  for(i = 0; i < t->size; i++) {
    if (t->table[i].key != NULL) {
      free(t->table[i].key);
    }
  }
  free(t->table);
}

void hash_insert(hash_table* t, hash_item item){
  debugf("hash table inserting item %s, %i\n", item.key, hash(item.key));
  pthread_mutex_lock(&t->lock);
  h_insert(t, item);
  pthread_mutex_unlock(&t->lock);
  debugf("hash table done inserting item %s, %i\n", item.key, hash(item.key));
  hash_print(t);
}

void hash_remove(hash_table* t, char* key) {
  if (key == NULL) {
    return;
  }
  int k = hash(key);
  debugf("hash table removing item %s, %i\n", key, k);
  hash_print(t);
  int hashIndex = k % t->size;
  int looped = -1;
  int loopedIndex = hashIndex;
  pthread_mutex_lock(&t->lock);
  while(t->table[hashIndex].present) {
    if (hashIndex == loopedIndex) {
      if (++looped) {
        break;
      }
    }
    if(!strcmp(t->table[hashIndex].key, key)) {
      t->table[hashIndex].present = 0;
      t->current_size--;
      pthread_mutex_unlock(&t->lock);
      return;
    }
    hashIndex++;
    hashIndex %= t->size;
  }
  pthread_mutex_unlock(&t->lock);
}

void hash_print(hash_table* t) {
  #ifdef DEBUG
  int i = 0;
  debugf("printing hash table\n");
  for (i = 0; i < t->size; i++) {
    debugf("\titem %i: %s, %i, %i\n", i, t->table[i].key, hash(t->table[i].key), t->table[i].present);
  }
  #endif
}

/**
 * The MapReduce coordinator.
 */

#ifndef H1_H__
#define H1_H__
#include "../rpc/rpc.h"
#include "../lib/lib.h"
#include "../app/app.h"
#include "job.h"
#include <glib.h>
#include <stdio.h>
#include <stdbool.h>
#include <time.h>
#include <sys/time.h>
#include <unistd.h>
#include <stdlib.h>
#include <rpc/pmap_clnt.h>
#include <string.h>
#include <memory.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <sys/stat.h>

typedef struct {
  get_task_reply* task;
  time_t start;
} task_t;

typedef struct {
  int cur_job_id;
  GHashTable *ht;
  int n_map;
  int n_reduce;
  GList* map_jobs;
  GList* reduce_jobs;
  GList* task_list;
} coordinator;

void coordinator_init(coordinator** coord_ptr);
#endif

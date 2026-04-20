/**
 * Logic for job and task management.
 *
 * You are not required to modify this file.
 */

 #include <stdlib.h>

#include "../rpc/rpc.h"
#ifndef JOB_H__
#define JOB_H__

/* You may add definitions here */

typedef struct job {
    int id;
    submit_job_request* req;
    int status;
    int total_map_num;
    int total_reduce_num;
    int maps_completed;
    int reduces_completed;
} job;

job* create_job(int id, submit_job_request* req);

// void add_job(job** head, job* to_add);

// //gets the next job that has status = 0
// job* get_job(job** head);

#endif

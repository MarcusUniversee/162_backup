/**
 * Logic for job and task management.
 *
 * You are not required to modify this file.
 */

#include "job.h"

job* create_job(int id, submit_job_request* req) {
    job* new_job = malloc(sizeof(job));
    if (new_job == NULL) {
        return NULL;
    }
    new_job->id = id;
    new_job->req = req;
    new_job->status = 0;
    new_job->total_map_num = req->files.files_len;
    new_job->total_reduce_num = req->n_reduce;
    new_job->maps_completed = 0;
    new_job->reduces_completed = 0;
    return new_job;
}

// void add_job(job** head, job* to_add) {
//     if (*head == NULL) {
//         *head = to_add;
//         return;
//     }
//     job* cur = *head;
//     job* prev = *head;
//     while (cur != NULL) {
//         prev = cur;
//         cur = cur->next;
//     }
//     prev->next = to_add;
// }

// job* get_job(job** head) {
//     if (*head == NULL) {
//         return NULL;
//     }
//     job* cur = *head;
//     while (cur != NULL) {
//         if (cur->status == 0) {
//             return cur;
//         }
//         cur = cur->next;
//     }
//     return NULL;
// }
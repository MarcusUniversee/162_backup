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

    //deep copy the submit job request so we always have it in the job
    submit_job_request* req_copy = malloc(sizeof(submit_job_request));
    if (req_copy == NULL) { free(new_job); return NULL; }

    req_copy->n_reduce = req->n_reduce;
    req_copy->output_dir = strdup(req->output_dir);
    req_copy->app = strdup(req->app);
    req_copy->args.args_len = req->args.args_len;
    req_copy->args.args_val = req->args.args_len ? strndup(req->args.args_val, req->args.args_len) : NULL;
    req_copy->files.files_len = req->files.files_len;
    req_copy->files.files_val = malloc(sizeof(path) * req->files.files_len);
    for (u_int i = 0; i < req->files.files_len; i++)
        req_copy->files.files_val[i] = strdup(req->files.files_val[i]);

    
    new_job->id = id;
    new_job->req = req_copy;
    new_job->status = 0;
    new_job->total_map_num = req->files.files_len;
    new_job->total_reduce_num = req->n_reduce;
    new_job->maps_completed = 0;
    new_job->reduces_completed = 0;
    return new_job;
}

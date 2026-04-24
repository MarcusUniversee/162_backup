/**
 * The MapReduce coordinator.
 */

#include "coordinator.h"

#ifndef SIG_PF
#define SIG_PF void (*)(int)
#endif

/* Global coordinator state. */
coordinator* state;

extern void coordinator_1(struct svc_req*, SVCXPRT*);

/* Set up and run RPC server. */
int main(int argc, char** argv) {
  register SVCXPRT* transp;

  pmap_unset(COORDINATOR, COORDINATOR_V1);

  transp = svcudp_create(RPC_ANYSOCK);
  if (transp == NULL) {
    fprintf(stderr, "%s", "cannot create udp service.");
    exit(1);
  }
  if (!svc_register(transp, COORDINATOR, COORDINATOR_V1, coordinator_1, IPPROTO_UDP)) {
    fprintf(stderr, "%s", "unable to register (COORDINATOR, COORDINATOR_V1, udp).");
    exit(1);
  }

  transp = svctcp_create(RPC_ANYSOCK, 0, 0);
  if (transp == NULL) {
    fprintf(stderr, "%s", "cannot create tcp service.");
    exit(1);
  }
  if (!svc_register(transp, COORDINATOR, COORDINATOR_V1, coordinator_1, IPPROTO_TCP)) {
    fprintf(stderr, "%s", "unable to register (COORDINATOR, COORDINATOR_V1, tcp).");
    exit(1);
  }

  coordinator_init(&state);

  svc_run();
  fprintf(stderr, "%s", "svc_run returned");
  exit(1);
  /* NOTREACHED */
}

/* EXAMPLE RPC implementation. */
int* example_1_svc(int* argp, struct svc_req* rqstp) {
  static int result;

  result = *argp + 1;

  return &result;
}

/* SUBMIT_JOB RPC implementation. */
/*
Assign and return a unique job ID (starting from 0 and increasing)
Keep track of the current order of jobs in the queue
Store job information in a manner that allows quick lookup by job ID
Validate the provided application name
*/
int* submit_job_1_svc(submit_job_request* argp, struct svc_req* rqstp) {
  static int result;

  printf("Received submit job request\n");

  /* TODO */
  int job_id = state->cur_job_id++;
  app app_ = get_app(argp->app);

  if (app_.name == NULL) {
    result = -(job_id + 1);
    return &result;
  }

  job* new_job = create_job(job_id, argp);
  if (new_job == NULL) {
    result = -(job_id + 1);
    return &result;
  }
  result = job_id;

  state->map_jobs = g_list_append(state->map_jobs, GINT_TO_POINTER(job_id));
  state->n_map += argp->files.files_len;

  GBytes *key = g_bytes_new(&job_id, sizeof(int)); 
  GBytes *value = g_bytes_new(&new_job, sizeof(job*));
  g_hash_table_replace(state->ht, key, value);
  

  // app app_ get_app(argp->app);
  // app_->map(argp->args);

  /* Do not modify the following code. */
  /* BEGIN */
  struct stat st;
  if (stat(argp->output_dir, &st) == -1) {
    mkdirp(argp->output_dir);
  }

  return &result;
  /* END */
}

/* POLL_JOB RPC implementation. */
poll_job_reply* poll_job_1_svc(int* argp, struct svc_req* rqstp) {
  static poll_job_reply result;

  result.done = false;
  result.failed = false;
  result.invalid_job_id = false;

  printf("Received poll job request\n");

  GBytes *key = g_bytes_new(argp, sizeof(int));
  GBytes *value = g_hash_table_lookup(state->ht, key);
  g_bytes_unref(key);

  if (value == NULL) {
    result.invalid_job_id = true;
    return &result;
  }

  const job **data_ptr = g_bytes_get_data(value, NULL);
  const job *data = *data_ptr;

  int status = data->status;

  switch (status) {
    case 1:
      result.done = true;
      break;
    case 2:
      result.failed = true;
      break;
    default:
      //do nothing
  }

  /* TODO */

  return &result;
}

/* GET_TASK RPC implementation. */
/*
Set task to a map task number between 0 and n_map-1 inclusive or a reduce task number between 0 and n_reduce-1 inclusive.
Set file to the file to process for map tasks.
Set wait to false if a task should be executed.
Set reduce to false if the assigned task is a map task.
Set job_id, output_dir, app, n_reduce, n_map, and args to the current job’s corresponding parameters.
Once you are done, sanity-check that tasks are being assigned correctly by inserting logging statements.*/
get_task_reply* get_task_1_svc(void* argp, struct svc_req* rqstp) {
  static get_task_reply result;

  printf("Received get task request\n");
  result.file = "";
  result.output_dir = "";
  result.app = "";
  result.wait = true;
  result.args.args_len = 0;

  //first check if we need to redo any task
  task_t* t;
  for (GList* elem = state->task_list; elem; elem = elem->next) {
    t = elem->data;
    if (time(NULL) - t->start > TASK_TIMEOUT_SECS) {
      t->start = time(NULL);
      return t->task;
    }
  }
  /* TODO */
  if (state->n_reduce > 0){
    printf("running reduce\n");
    state->n_reduce--;
    //remove the first element
    GList* next_job = g_list_first(state->reduce_jobs);
    int job_id = GPOINTER_TO_INT(next_job->data);

    GBytes *key = g_bytes_new(&job_id, sizeof(int));
    GBytes *value = g_hash_table_lookup(state->ht, key);
    g_bytes_unref(key);

    if (value == NULL) {
      return &result;
    }
    

    job **data_ptr = g_bytes_get_data(value, NULL);
    job *data = *data_ptr;
    task_t* t = malloc(sizeof(task_t));
    if (t == NULL) {
      return &result;
    }

    result.wait = false;
    result.reduce = true;
    result.job_id = job_id;
    result.output_dir = data->req->output_dir;
    result.app = data->req->app;
    data->req->n_reduce--;
    result.task = data->total_reduce_num - data->req->n_reduce - 1;
    if (data->req->n_reduce == 0) {
        state->reduce_jobs = g_list_delete_link(state->reduce_jobs, state->reduce_jobs);
    }
    result.n_reduce = data->total_reduce_num;
    result.n_map = data->total_map_num;
    result.args.args_len = data->req->args.args_len;
    result.args.args_val = data->req->args.args_val;

    get_task_reply* copy = malloc(sizeof(get_task_reply));
    if (copy == NULL) {
      free(t);
      return &result;
    }
    *copy = result;
    t->task = copy;
    t->start = time(NULL);
    state->task_list = g_list_append(state->task_list, t);

  } else if (state->n_map > 0) {
    printf("running map\n");
    state->n_map--;
    //remove the first element
    GList* next_job = g_list_first(state->map_jobs);
    int job_id = GPOINTER_TO_INT(next_job->data);

    GBytes *key = g_bytes_new(&job_id, sizeof(int));
    GBytes *value = g_hash_table_lookup(state->ht, key);
    g_bytes_unref(key);

    if (value == NULL) {
      return &result;
    }
    job **data_ptr = g_bytes_get_data(value, NULL);
    job *data = *data_ptr;

    task_t* t = malloc(sizeof(task_t));
    if (t == NULL) {
      return &result;
    }

    result.file = *data->req->files.files_val;
    data->req->files.files_val++;
    data->req->files.files_len--;
    result.task = data->total_map_num - data->req->files.files_len - 1;
    if (data->req->files.files_len == 0) {
      state->map_jobs = g_list_delete_link(state->map_jobs, state->map_jobs);
    }
    result.wait = false;
    result.reduce = false;
    result.job_id = job_id;
    result.output_dir = data->req->output_dir;
    result.app = data->req->app;
    result.n_reduce = data->total_reduce_num;
    result.n_map = data->total_map_num;
    result.args.args_len = data->req->args.args_len;
    result.args.args_val = data->req->args.args_val;

    get_task_reply* copy = malloc(sizeof(get_task_reply));
    if (copy == NULL) {
      free(t);
      return &result;
    }
    *copy = result;
    t->task = copy;
    t->start = time(NULL);
    state->task_list = g_list_append(state->task_list, t);
  }
  return &result;
}

/* FINISH_TASK RPC implementation. */
void* finish_task_1_svc(finish_task_request* argp, struct svc_req* rqstp) {
  static char* result;

  printf("Received finish task request\n");
  GBytes *key = g_bytes_new(&argp->job_id, sizeof(int));
  GBytes *value = g_hash_table_lookup(state->ht, key);
  g_bytes_unref(key);

  task_t* t;
  for (GList* elem = state->task_list; elem; elem = elem->next) {
    t = elem->data;
    if (t->task->job_id == argp->job_id && t->task->task == argp->task && t->task->reduce == argp->reduce) {
      state->task_list = g_list_delete_link(state->task_list, elem);
      free(t->task);
      free(t);
      break;
    }
  }

  if (value == NULL) {
    return (void*) &result;
  }
  job **data_ptr = g_bytes_get_data(value, NULL);
  job *data = *data_ptr;
  if (!argp->success) {
    data->status = 2;
    return (void*) &result;
  }
  /* TODO */
  if (argp->reduce) {
    //reduce
    if (data->reduces_completed >= data->total_reduce_num) {
      return (void*)&result;
    }
    data->reduces_completed++;
    if (data->reduces_completed == data->total_reduce_num) {
      data->status = 1;
    }
  } else {
    if (data->maps_completed >= data->total_map_num) {
      return (void*)&result;
    }
    data->maps_completed++;
    if (data->maps_completed == data->total_map_num) {
      state->reduce_jobs = g_list_append(state->reduce_jobs, GINT_TO_POINTER(argp->job_id));
      state->n_reduce += data->total_reduce_num;
    }
  }

  return (void*)&result;
}

/* Initialize coordinator state. */
void coordinator_init(coordinator** coord_ptr) {
  *coord_ptr = malloc(sizeof(coordinator));

  coordinator* coord = *coord_ptr;

  /* TODO */
  coord->cur_job_id = 0;
  coord->ht = g_hash_table_new(g_bytes_hash, g_bytes_equal);
  coord->map_jobs = NULL;
  coord->reduce_jobs = NULL;
  coord->n_map = 0;
  coord->n_reduce = 0;
  coord->task_list = NULL;
}

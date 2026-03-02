#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <signal.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

#include "tokenizer.h"

/* Convenience macro to silence compiler warnings about unused function parameters. */
#define unused __attribute__((unused))
#define MAX_BACKGROUND_PROCESSES 128
#define PATH_MAX 4096

/* Whether the shell is connected to an actual terminal or not. */
bool shell_is_interactive;

/* File descriptor for the shell input */
int shell_terminal;

/* Terminal mode settings for the shell */
struct termios shell_tmodes;

/* Process group id for the shell */
pid_t shell_pgid;

//keep track globally of all background
pid_t background_pids[MAX_BACKGROUND_PROCESSES];
int background_count = 0;

//most recently launched
pid_t current_gpid = -1;

int cmd_exit(struct tokens* tokens);
int cmd_help(struct tokens* tokens);
int cmd_pwd(struct tokens* tokens);
int cmd_cd(struct tokens* tokens);
int cmd_wait(struct tokens* tokens);
int cmd_fg(struct tokens* tokens);
int cmd_bg(struct tokens* tokens);

/* Built-in command functions take token array (see parse.h) and return int */
typedef int cmd_fun_t(struct tokens* tokens);

/* Built-in command struct and lookup table */
typedef struct fun_desc {
  cmd_fun_t* fun;
  char* cmd;
  char* doc;
} fun_desc_t;

fun_desc_t cmd_table[] = {
    {cmd_help, "?", "show this help menu"},
    {cmd_exit, "exit", "exit the command shell"},
    {cmd_pwd, "pwd", "prints the current working directory"},
    {cmd_cd, "cd", "changes the working directory to the listed directory"},
    {cmd_wait, "wait", "waits until all background jobs are finished before returning"},
    {cmd_fg, "fg", "move process with id pid to the foreground and resume"},
    {cmd_bg, "bg", "resume paused background process, or most recently launched"}
};

/* Prints a helpful description for the given command */
int cmd_help(unused struct tokens* tokens) {
  for (unsigned int i = 0; i < sizeof(cmd_table) / sizeof(fun_desc_t); i++)
    printf("%s - %s\n", cmd_table[i].cmd, cmd_table[i].doc);
  return 1;
}

/* Exits this shell */
int cmd_exit(unused struct tokens* tokens) { exit(0); }

int cmd_pwd(unused struct tokens* tokens) {
  char buf[PATH_MAX];
  getcwd(buf, PATH_MAX);
  printf("%s\n", buf);
  return 1;
}

int cmd_cd(struct tokens* tokens) {
  if (tokens_get_length(tokens) != 2) {
    printf("Error: cd takes in one required argument\n");
    return -1;
  }
  if (chdir(tokens_get_token(tokens, 1))) {
    printf("File path %s does not exist\n", tokens_get_token(tokens, 1));
  }
  return 1;
}

int cmd_wait(unused struct tokens* tokens) {
  int status;
  for (int i=0; i<background_count; i++) {
    waitpid(background_pids[i], &status, 0);
  }
  background_count = 0;
  return 1;
}

int cmd_fg(struct tokens* tokens) {
  size_t L = tokens_get_length(tokens);
  pid_t target_gpid;
  if (L == 1) {
    if (current_gpid == -1) {
      printf("Error: fg has no current job\n");
      return -1;
    }
    target_gpid = current_gpid;
  } else if (L == 2) {
    int pid = atoi(tokens_get_token(tokens, 1));
    target_gpid = getpgid(pid);
  } else {
    printf("Error: fg takes in zero or one arguments\n");
    return -1;
  }

  tcsetpgrp(shell_terminal, target_gpid);
  killpg(target_gpid, SIGCONT);
  int status;
  waitpid(-target_gpid, &status, WUNTRACED);
  tcsetpgrp(shell_terminal, shell_pgid);
  if (WIFSTOPPED(status)) {
    //handle the edge case where it can get restopped
    current_gpid = target_gpid;
  }
  return 1;
}

int cmd_bg(struct tokens* tokens) {
  size_t L = tokens_get_length(tokens);
  pid_t target_gpid;
  if (L == 1) {
    if (current_gpid == -1) {
      printf("Error: bg has no current job\n");
      return -1;
    }
    target_gpid = current_gpid;
  } else if (L == 2) {
    int pid = atoi(tokens_get_token(tokens, 1));
    target_gpid = getpgid(pid);
  } else {
    printf("Error: bg takes in zero or one arguments\n");
    return -1;
  }
  killpg(target_gpid, SIGCONT);
  //add to background_pids for wait
  background_pids[background_count++] = target_gpid;
  return 1;
}

/* Looks up the built-in command, if it exists. */
int lookup(char cmd[]) {
  for (unsigned int i = 0; i < sizeof(cmd_table) / sizeof(fun_desc_t); i++)
    if (cmd && (strcmp(cmd_table[i].cmd, cmd) == 0))
      return i;
  return -1;
}

//redirects stdout or stdin into/from a file
//args should be the tokens just from the running process segment
//seg_len should be the length of args[]
int redirect(char* args[], int seg_len) {
  for (int i = 0; i < seg_len; i++) {
    if (args[i] == NULL) {
      continue;
    }

    //check i+1 just in case because < and > need something after
    if (strcmp(args[i], "<") == 0 && i + 1 < seg_len) {
      int fd = open(args[i + 1], O_RDONLY);
      if (fd == -1) {
        fprintf(stderr, "Failed to open file\n");
        return 1;
      }
      dup2(fd, STDIN_FILENO);
      close(fd);
      //set arguments to NULL to ignore
      args[i] = NULL;
      args[i + 1] = NULL;
    } else if (strcmp(args[i], ">") == 0 && i + 1 < seg_len) {
      //WRONLY to open with write. CREAT to create if it doesn't exist
      //TRUNC to replace if file already exists. 0644 perms (general default)
      int fd = open(args[i + 1], O_WRONLY | O_CREAT | O_TRUNC, 0644);
      if (fd == -1) {
        fprintf(stderr, "Failed to open file\n");
        return 1;
      }
      dup2(fd, STDOUT_FILENO);
      close(fd);
      //set arguments to NULL to ignore
      args[i] = NULL;
      args[i + 1] = NULL;
    }
  }
  return 0;
}

size_t count_pipes(struct tokens* tokens) {
  int count = 0;
  size_t L = tokens_get_length(tokens);
  for (int i = 0; i < L; i++) {
    char *t = tokens_get_token(tokens, i);
    if (strcmp(t, "|") == 0) {
      count += 1;
    }
  }
  return count;
}

/* Intialization procedures for this shell */
void init_shell() {
  /* Our shell is connected to standard input. */
  shell_terminal = STDIN_FILENO;

  /* Check if we are running interactively */
  shell_is_interactive = isatty(shell_terminal);

  if (shell_is_interactive) {
    /* If the shell is not currently in the foreground, we must pause the shell until it becomes a
     * foreground process. We use SIGTTIN to pause the shell. When the shell gets moved to the
     * foreground, we'll receive a SIGCONT. */
    while (tcgetpgrp(shell_terminal) != (shell_pgid = getpgrp()))
      kill(-shell_pgid, SIGTTIN);

    /* Saves the shell's process id */
    shell_pgid = getpid();

    /* Take control of the terminal */
    tcsetpgrp(shell_terminal, shell_pgid);

    /* Save the current termios to a variable, so it can be restored later. */
    tcgetattr(shell_terminal, &shell_tmodes);
  }
}

int main(unused int argc, unused char* argv[]) {
  init_shell();

  static char line[4096];
  int line_num = 0;

  /* Please only print shell prompts when standard input is not a tty */
  if (shell_is_interactive) {
    fprintf(stdout, "%d: ", line_num);

    //ignore these signals if shell is in the foreground
    signal(SIGINT,  SIG_IGN); //ctrl c
    signal(SIGQUIT, SIG_IGN); //ctrl "\"
    signal(SIGTSTP, SIG_IGN); //ctrl z
    signal(SIGTTIN, SIG_IGN); //background input request
    signal(SIGTTOU, SIG_IGN); //background output request
  }

  while (fgets(line, 4096, stdin)) {
    /* Split our line into words. */
    struct tokens* tokens = tokenize(line);
    size_t L = tokens_get_length(tokens);

    bool background = false;
    if (L > 0 && strcmp(tokens_get_token(tokens, L-1), "&") == 0) background = true;

    //run a separate fork for every process
    size_t n = count_pipes(tokens) + 1; //number of programs
    int fundex = -1;
    if (n == 1) {
      fundex = lookup(tokens_get_token(tokens, 0));
    }
    //if its just a single built-in cmd, just directly run things
    if (fundex >= 0) {
      cmd_table[fundex].fun(tokens);
    } else {
      int pipe_arr[n - 1][2];
      for (int i=0; i<n-1; i++) {
        if (pipe(pipe_arr[i]) == -1) {
          fprintf(stderr, "Error attempting to pipe\n");
          return 1;
        }
      }
      pid_t pids[n];
      //need to keep track of where each process starts and ends inside
      //of the tokens to run full line
      int seg_start = 0;

      for (int i = 0; i < n; i++) {
        //get end of segment by searching for |
        int seg_end = seg_start;
        while (seg_end < L && strcmp(tokens_get_token(tokens, seg_end), "|") != 0)
          seg_end += 1;

        pids[i] = fork();
        if (pids[i] < 0) {
          fprintf(stderr, "Error while attempting to run program. Failed to create a child process\n");
          return 1;
        }
        if (pids[i] == 0) {
          //CHILD
          //set child to its own process group. duplicate of below because
          //we do not know if parent or child will run first
          setpgid(0, pids[0]);

          //enable all the signals, since it will only do it for the process group its in
          signal(SIGINT,  SIG_DFL);
          signal(SIGQUIT, SIG_DFL);
          signal(SIGTSTP, SIG_DFL);
          signal(SIGTTIN, SIG_DFL);
          signal(SIGTTOU, SIG_DFL);
          //pipe from previous stdin and pipe to next stdout
          if (i < n - 1) {
            dup2(pipe_arr[i][1], STDOUT_FILENO);
          }
          if (i > 0) {
            dup2(pipe_arr[i - 1][0], STDIN_FILENO);
          }
          //close everything
          for (int j = 0; j < n - 1; j++) {
            close(pipe_arr[j][0]);
            close(pipe_arr[j][1]);
          }

          //remove the & if it exists
          if (i == n-1 && background) {
            seg_end -= 1;
          }
          int seg_len = seg_end - seg_start; //# of total args
          
          char *args[seg_len + 1];
          for (int j = 0; j < seg_len; j++) {
            args[j] = tokens_get_token(tokens, seg_start + j);
          }
          args[seg_len] = NULL;

          int fundex = lookup(args[0]);
          if (fundex >= 0) {
            cmd_table[fundex].fun(tokens);
            exit(0);
          }

          //redirect in the case of > and <
          if (redirect(args, seg_len)) {
            exit(1);
          }

          char* path = getenv("PATH");
          char* executable = args[0];
          char* path_ptr = path;
          char full_path[PATH_MAX];
          char* dir;
          //attempt to execv, if it fails, it will continue
          //if not, this code should not run anymore
          while ((dir = strtok_r(path_ptr, ":", &path_ptr))) {
            snprintf(full_path, sizeof(full_path), "%s/%s", dir, executable);
            execv(full_path, args);
          }
          execv(executable, args);
          fprintf(stderr, "Failed to execv program\n");
          exit(1);
        }
        //PARENT
        //set child to its own process group. duplicate of above because
        //we do not know if parent or child will run first
        setpgid(pids[i], pids[0]);
        seg_start = seg_end + 1; //next segment, going past |
      }

      //PARENT
      int status;
      for (int i = 0; i < n - 1; i++) {
        close(pipe_arr[i][0]);
        close(pipe_arr[i][1]);
      }

      if (background) {
        //run in background
        //save all the child processes so that we can wait for it later
        for (int i=0; i<n; i++) {
          background_pids[background_count] = pids[i];
          background_count += 1;
        }
        current_gpid = pids[0];
      } else {
        //do not run in background
        //set the child processes group as the foreground
        if (shell_is_interactive) {
          tcsetpgrp(shell_terminal, pids[0]);
        }
        //wait for every single child process to finish
        for (int i = 0; i < n; i++) {
          waitpid(pids[i], &status, WUNTRACED);
        }
        if (WIFSTOPPED(status)) {
          current_gpid = pids[0];
        }
      }
    }
    if (shell_is_interactive) {
      /* Please only print shell prompts when standard input is not a tty */
      tcsetpgrp(shell_terminal, shell_pgid); //return foreground to old shell
      fprintf(stdout, "%d: ", ++line_num);
    }

    /* Clean up memory */
    tokens_destroy(tokens);
  }

  return 0;
}

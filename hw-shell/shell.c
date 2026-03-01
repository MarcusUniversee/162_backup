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
#define PATH_MAX 4096

/* Whether the shell is connected to an actual terminal or not. */
bool shell_is_interactive;

/* File descriptor for the shell input */
int shell_terminal;

/* Terminal mode settings for the shell */
struct termios shell_tmodes;

/* Process group id for the shell */
pid_t shell_pgid;

int cmd_exit(struct tokens* tokens);
int cmd_help(struct tokens* tokens);
int cmd_pwd(struct tokens* tokens);
int cmd_cd(struct tokens* tokens);

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

/* Looks up the built-in command, if it exists. */
int lookup(char cmd[]) {
  for (unsigned int i = 0; i < sizeof(cmd_table) / sizeof(fun_desc_t); i++)
    if (cmd && (strcmp(cmd_table[i].cmd, cmd) == 0))
      return i;
  return -1;
}

int redirect(struct tokens* tokens, char* args[]) {
  size_t L = tokens_get_length(tokens);
  for (int i = 0; i < L; i++) {
    char *t = tokens_get_token(tokens, i);
    if (strcmp(t, "<") == 0) {
      //stdin
      if (i + 1 < L) {
        char* redir_file = tokens_get_token(tokens, i + 1);
        //only allow reading
        int file_fd = open(redir_file, O_RDONLY);
        if (file_fd != -1) {
          dup2(file_fd, STDIN_FILENO);
          close(file_fd);
          args[i] = NULL;
        } else {
          //failure
          fprintf(stderr, "Failed to open file\n");
          return 1;
        }
      }
    } else if (strcmp(t, ">") == 0) {
      //stdout
      if (i + 1 < L) {
        char* redir_file = tokens_get_token(tokens, i + 1);
        //write only and create if does not exist, and replace old if exists
        int file_fd = open(redir_file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (file_fd != -1) {
          dup2(file_fd, STDOUT_FILENO);
          close(file_fd);
          args[i] = NULL;
        } else {
          //failure
          fprintf(stderr, "Failed to open file\n");
          return 1;
        }
      }
    }
  }
  return 0;
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
  if (shell_is_interactive)
    fprintf(stdout, "%d: ", line_num);

  while (fgets(line, 4096, stdin)) {
    /* Split our line into words. */
    struct tokens* tokens = tokenize(line);

    /* Find which built-in function to run. */
    int fundex = lookup(tokens_get_token(tokens, 0));

    if (fundex >= 0) {
      cmd_table[fundex].fun(tokens);
    } else {
      pid_t pid = fork();
      if (pid < 0) {
        fprintf(stdout, "Error while attempting to run program. Failed to create a child process\n");
      } else if (pid == 0) {
        size_t L = tokens_get_length(tokens);
        //child
        char *args[L + 1];
        for (int i = 0; i < L; i++) {
          args[i] = tokens_get_token(tokens, i);
        }
        args[L] = NULL;

        if (redirect(tokens, args)) {
          exit(1);
        }

        //first check path
        char* path = getenv("PATH");
        char* executable = tokens_get_token(tokens, 0);
        char* path_ptr = path;
        char full_path[PATH_MAX];
        char* dir;
        while ((dir = strtok_r(path_ptr, ":", &path_ptr))) {
          //need to add / after the dir
          snprintf(full_path, sizeof(full_path), "%s/%s", dir, executable);
          execv(full_path, args);
        }
        //then run directly
        execv(executable, args);
        fprintf(stderr, "Failed to execv program\n");
        exit(1);
      } else {
        //parent
        int status;
        waitpid(pid, &status, 0);
      }
    }

    if (shell_is_interactive)
      /* Please only print shell prompts when standard input is not a tty */
      fprintf(stdout, "%d: ", ++line_num);

    /* Clean up memory */
    tokens_destroy(tokens);
  }

  return 0;
}

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>

// function descriptions given above definitions below main()
int get_input(char in[1024], char* cmd_argv[512]); 
int parse_args(char* cmd_argv[512], int cmd_argc, char* home, int* exit_status); 
void reap_children();

// signal handler
static void sig_handle(int sig_num) {
  if (sig_num == SIGINT) {
    printf("terminated by signal %d\n", sig_num);
  }
}

int main() {
  char input[1024];             // input buffer where command-line input stored
  char* cmd_argv[512];          // array for pointing to args inside buffer
  char* home = getenv("HOME");  // store usr's HOME environment var here

  int exit_loop = 0;            // flag for main prompt loop: 1 exits loop
  int cmd_argc;                 // count of args in cmd_argv
  int exit_status = 0;          // set by parse_args(), for 'status' results

  struct sigaction action, default_action;

  action.sa_handler = sig_handle;
  action.sa_flags = 0;
  sigfillset(&(action.sa_mask));

  sigaction(SIGINT, &action, &default_action);  // block SIGINT's to main pgm

  // START prompt loop
  do {
    
    // get user input
    cmd_argc = get_input(input, cmd_argv);

    if (cmd_argc == 1) {        // no input
      continue;                 // so re-loop
    }

    // see if any child processes have ended
    reap_children();

    // parse arguments, bg_pid is set with child pid if a bg proc was created
    exit_loop = parse_args(cmd_argv, cmd_argc, home, &exit_status);

  }
  while (exit_loop == 0);
  // EXIT prompt loop

  return 0;
}

// This function takes 2 parameters, a 1024-byte char buffer for storing usr
// input into the command line, and an array of char-pointers. The function
// uses the buffer for storing input, and uses the pointer-array for
// partitioning that buffer into an entered command and its argruments. The
// function returns a count of command-line arguments, including a trailing
// null argument required by execvp.
int get_input(char in[1024], char* cmd_argv[512]) {
  int i = 0;
  int k;
  int cmd_argc = 1;     // count of args in arg-array 'cmd_argv'
  FILE *fd = stdin;

  in[0] = '\0';

  // flush output
  fflush(stdout);

  // print prompt
  if (isatty(0)) {      // only print if usr is a terminal: not in script
    printf(":");
  }

  fgets(in, 1024, fd);  // get input
  i = strlen(in) - 1;
  if (in[i] == '\n') {  // if last char input was newline, change it to \0
    in[i] = '\0';
  }
  if (feof(fd)) {       // if end-of-file reached, then we are in a script
    // http://stackoverflow.com/questions/23978444/c-redirect-stdin-to-keyboard
    if (!freopen("/dev/tty", "r", fd)) {        // re-open stdin as a terminal
      perror("/dev/tty");
      exit(1);
    }
  }

  cmd_argv[0] = &in[0]; // first arg should always start at 0 and be pgm-name
  k = strlen(in);

  for (i = 1; i < k; i++) {     // loop thru all of input buffer
    if (in[i] == ' ') {         // if a space is encountered...
      in[i] = '\0';             // null-terminate it
      if ((i + 1) < k) {        // don't go past length string
        if (in[i + 1] != ' ') { // ignore multiple spaces
          cmd_argv[cmd_argc] = &in[i + 1]; // assign pointer to this arg
          cmd_argc++;           // increase arg-count
        }
      }
    }
  }
  cmd_argv[cmd_argc] = NULL;    // last argument is null
  cmd_argc++;

  return cmd_argc;              // return count of arguments
}

// This function takes a variety parameters, primarily the char-ptr array of
// cmd-line args and the count of that array, to be used in forking the parent
// process and spawning a child based on the pgm in cmd_argv[0]. Built-in
// commands 'exit', 'cd', and 'status' are also handled inside of this
// function. Should always return 0, with the exception of the one case when
// cmd_argv[0] is 'exit'.
int parse_args(char* cmd_argv[512], int cmd_argc, char* home, int* exit_status) {
  char* read_ptr = NULL;
  char* write_ptr = NULL;
  int i, fd, wait_status;
  int exit_loop = 0;            // return value for function
  int continue_loop = 0;        // used to exit fn if cmd_argv[0] is a built-in
  int read_flag = 0;            // set if '<' arg encountered
  int write_flag = 0;           // set if '>' arg encountered
  int background_flag = 0;      // set if '&' is last usable arg

  pid_t spawn = -5;
  pid_t exit_pid;

  /*--------------------------------------------------------------------*/
  // built-in command processing
  // A) if input is 'exit', then exit the loop
  if (strcmp(cmd_argv[0], "exit") == 0) {
    exit_loop = 1;              // return value, tells main() to exit loop
    continue_loop = 1;
  }

  // B) if 'cd'
  else if (strcmp(cmd_argv[0], "cd") == 0) {
    if ((cmd_argv[1] == NULL) || (strcmp(cmd_argv[1], "~") == 0)) {
      // chdir to $HOME dir, which was stored in 'home' @ start of pgm
      if (chdir(home) != 0) {
        printf("Unable to change to home directory\n");
        *exit_status = 1;
      }
    }
    else {
      if (chdir(cmd_argv[1]) != 0) {
        printf("Unable to change directory\n");
        *exit_status = 1;
      }
    }
    continue_loop = 1;
  }

  // C) if 'status'
  else if (strcmp(cmd_argv[0], "status") == 0) {
    // print exit value of last-process that ran 
    printf("exit value %d\n", *exit_status);
    *exit_status = 0;
    continue_loop = 1;
  }

  else if ((cmd_argv[0][0] == '#') || (cmd_argv[0][0] == '\0')) {
    *exit_status = 0;
    continue_loop = 1;
  }

  if (continue_loop) {          // quit loop without forking
    return exit_loop;
  }
  /*------------------------------------------------------------------*/
  //
  // at this point, need to iterate thru cmd-line args, looking for <, > or & 
  //
  for (i = 1; i < cmd_argc - 1; i++) { 
    // < for reading process from follow-up argument's file 
    if (strcmp(cmd_argv[i], "<") == 0) { 
      // set input flag & store/open [i+1] as fd (assuming it exists) 
      read_flag = 1; 
      read_ptr = cmd_argv[i + 1]; // pts to name of input file
      cmd_argv[i] = NULL;     // this nullifies the array at "<" for execp 
      break;                      // break for loop
    } 
    // > for writing process to follow-up argument's file 
    if (strcmp(cmd_argv[i], ">") == 0) { 
      write_flag = 1; 
      write_ptr = cmd_argv[i + 1]; // pts to name of output file
      cmd_argv[i] = NULL;     // this nullifies the array at ">" for execvp 
      break;                       // break for loop
    } 
  } 
  // & for setting process in background 
  if (strcmp(cmd_argv[cmd_argc - 2], "&") == 0) {     // last legit argument 
    background_flag = 1; 
    cmd_argv[cmd_argc - 2] = '\0'; 

  } 

  // fork this process - parent will be the continuation of the shell, and 
  // child is a copy of the shell that needs to exec cmd_argv[0]. NOTE that 
  // fork will return child's pid to the parent shell process, and 0 to the 
  // child process (which is the one that execs cmd_argv[0]). 
  // else { 
  spawn = fork(); 
  if (spawn > 0) {    // PARENT here, so need to wait for child to exit(?) 
    if (background_flag) { 
      printf("background pid is %d\n", spawn);

    } else { 
      exit_pid = waitpid(spawn, &wait_status, 0); 

      if (WIFEXITED(wait_status)) {     // child process exited succesfully
        *exit_status = WEXITSTATUS(wait_status);
      }
    }
  }
  else if (spawn == 0) {      // CHILD here, so need to exec cmd_argv[0]
    // need to check flags, to see if process is reading or writing
    if (read_flag) {
      // open read_ptr for reading
      fd = open(read_ptr, O_RDONLY);
      if (fd == -1) {
        printf("smallsh: cannot open %s for input\n", read_ptr);
        exit (1);
      }
      if (fd != STDIN_FILENO) {
        dup2(fd, STDIN_FILENO);
      }
    }
    if (write_flag) {
      // open write_ptr for writing
      fd = open(write_ptr, O_WRONLY | O_CREAT | O_TRUNC, 0644);
      if (fd != STDOUT_FILENO) {
        dup2(fd, STDOUT_FILENO);
      }
    }
    if (background_flag) {
      // set up block on SIGINT for this child process
      struct sigaction block_action;
      block_action.sa_handler = SIG_IGN;
      block_action.sa_flags = 0;
      sigfillset(&(block_action.sa_mask));
      sigaction(SIGINT, &block_action, NULL);

    }
    execvp(cmd_argv[0], cmd_argv);    // executes pgm in 1st param
    perror(cmd_argv[0]);              // only returns on error
    return (1);
  }
  else {
    perror("Herror here.");
    exit(1);
  }

  return exit_loop;
}

// This function, called inside the prompt loop, deals with any extant
// child processes.
void reap_children() {
  int exit_pid = -5;
  int status, status_value;

  exit_pid = waitpid(-1, &status, WNOHANG);

  if (exit_pid != -1) {
    if (WIFEXITED(status)) {
      status_value = WEXITSTATUS(status);
      printf("background pid %d is done: exit value %d\n", exit_pid, status_value);
    }

    // works, but not immediately, because waitpid call is inside of function
    if (WIFSIGNALED(status)) {
      status_value = WTERMSIG(status);
      if (status_value == SIGTERM) {
        printf("background pid %d is done: terminated by signal %d\n", exit_pid, status);
      }
    }
  }
}


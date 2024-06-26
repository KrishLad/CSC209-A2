/* 
 * tsh - A tiny shell program with job control
 * 
 */
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>
#include <fcntl.h>

/* Misc manifest constants */
#define MAXLINE    1024   /* max line size */
#define MAXARGS     128   /* max args on a command line */
#define MAXJOBS      16   /* max jobs at any point in time */

/* Job states */
#define UNDEF 0 /* undefined */
#define FG 1    /* running in foreground */
#define BG 2    /* running in background */
#define ST 3    /* stopped */

/* 
 * Jobs states: FG (foreground), BG (background), ST (stopped)
 * Job state transitions and enabling actions:
 *     FG -> ST  : ctrl-z
 *     ST -> FG  : fg command
 *     ST -> BG  : bg command
 *     BG -> FG  : fg command
 * At most 1 job can be in the FG state.
 */

/* Global variables */
extern char **environ;      /* defined in libc */
char prompt[] = "tsh> ";    /* command line prompt (DO NOT CHANGE) */
int verbose = 0;            /* if true, print additional output */
char sbuf[MAXLINE];         /* for composing sprintf messages */

struct job_t {              /* Per-job data */
    pid_t pid;              /* job PID */
    int jid;                /* job ID [1, 2, ...] */
    int state;              /* UNDEF, FG, BG, or ST */
    char cmdline[MAXLINE];  /* command line */
};
struct job_t jobs[MAXJOBS]; /* The job list */

volatile sig_atomic_t ready; /* Is the newest child in its own process group? */

/* End global variables */


/* Function prototypes */

/* Here are the functions that you will implement */
void eval(char *cmdline);
int builtin_cmd(char **argv);
void do_bgfg(char **argv);
void waitfg(pid_t pid);
void sigchld_handler(int sig);
void sigint_handler(int sig);
void sigtstp_handler(int sig);

/* Here are helper routines that we've provided for you */
int parseline(const char *cmdline, char **argv); 
void sigquit_handler(int sig);
void sigusr1_handler(int sig);

void clearjob(struct job_t *job);
void initjobs(struct job_t *jobs);
int freejid(struct job_t *jobs); 
int addjob(struct job_t *jobs, pid_t pid, int state, char *cmdline);
int deletejob(struct job_t *jobs, pid_t pid); 
pid_t fgpid(struct job_t *jobs);
struct job_t *getjobpid(struct job_t *jobs, pid_t pid);
struct job_t *getjobjid(struct job_t *jobs, int jid); 
int pid2jid(pid_t pid); 
void listjobs(struct job_t *jobs);

void usage(void);
void unix_error(char *msg);
void app_error(char *msg);
typedef void handler_t(int);
handler_t *Signal(int signum, handler_t *handler);


/* Team Define Helpers*/
void setup_redirection(char **argv);
int has_piping(char **argv, int argc);
void my_pipe(char **argv, int argc, sigset_t *prev_mask, char *cmdline);

/*
 * main - The shell's main routine 
 */
int main(int argc, char **argv) {
    char c;
    char cmdline[MAXLINE];
    int emit_prompt = 1; /* emit prompt (default) */

    /* Redirect stderr to stdout (so that driver will get all output
     * on the pipe connected to stdout) */
    dup2(STDOUT_FILENO, STDERR_FILENO);

    /* Parse the command line */
    while ((c = getopt(argc, argv, "hvp")) != -1) {
        switch (c) {
            case 'h':             /* print help message */
                usage();
                break;
            case 'v':             /* emit additional diagnostic info */
                verbose = 1;
                break;
            case 'p':             /* don't print a prompt */
                emit_prompt = 0;  /* handy for automatic testing */
                break;
            default:
                usage();
        }
    }

    /* Install the signal handlers */

    Signal(SIGUSR1, sigusr1_handler); /* Child is ready */

    /* These are the ones you will need to implement */
    Signal(SIGINT,  sigint_handler);   /* ctrl-c */
    Signal(SIGTSTP, sigtstp_handler);  /* ctrl-z */
    Signal(SIGCHLD, sigchld_handler);  /* Terminated or stopped child */

    /* This one provides a clean way to kill the shell */
    Signal(SIGQUIT, sigquit_handler); 

    /* Initialize the job list */
    initjobs(jobs);

    /* Execute the shell's read/eval loop */
    while (1) {

        /* Read command line */
        if (emit_prompt) {
            printf("%s", prompt);
            fflush(stdout);
        }
        if ((fgets(cmdline, MAXLINE, stdin) == NULL) && ferror(stdin))
            app_error("fgets error");
        if (feof(stdin)) { /* End of file (ctrl-d) */
            fflush(stdout);
            exit(0);
        }

        /* Evaluate the command line */
        eval(cmdline);
        fflush(stdout);
    } 

    exit(0); /* control never reaches here */
}
  
/* 
 * eval - Evaluate the command line that the user has just typed in
 * 
 * If the user has requested a built-in command (quit, jobs, bg or fg)
 * then execute it immediately. Otherwise, fork a child process and
 * run the job in the context of the child. If the job is running in
 * the foreground, wait for it to terminate and then return.  Note:
 * each child process must have a unique process group ID so that our
 * background children don't receive SIGINT (SIGTSTP) from the kernel
 * when we type ctrl-c (ctrl-z) at the keyboard.  
*/
void eval(char *cmdline) {
    char **argv = malloc(sizeof(char *) * MAXARGS);
    int argc;
    pid_t pid;
    int jid;
    struct job_t *job;
    int err;
    sigset_t mask, prev_mask;

    argc = parseline(cmdline, argv);

    if (argc == 0 || argv[0] == NULL) {
        return;
    }
    else if (strcmp(argv[0], "quit") == 0 || strcmp(argv[0], "jobs") == 0 || strcmp(argv[0], "bg") == 0 || (strcmp(argv[0], "fg") == 0)) {
        err = builtin_cmd(argv);
        if (err != 0) {
            exit(err);
        }
    }
    else if (has_piping(argv, argc) == 1) { //need to put it in a separate if or else we have a fork within a fork, which is messy

        sigemptyset(&mask);
        sigaddset(&mask, SIGCHLD);
        sigaddset(&mask, SIGINT);
        sigaddset(&mask, SIGTSTP);
        sigprocmask(SIG_BLOCK, &mask, &prev_mask);

        my_pipe(argv, argc, &prev_mask, cmdline);
    }
    else {
        sigemptyset(&mask);
        sigaddset(&mask, SIGCHLD);
        sigaddset(&mask, SIGINT);
        sigaddset(&mask, SIGTSTP);
        sigprocmask(SIG_BLOCK, &mask, &prev_mask);
        
        pid = fork();

        if (pid < 0) {
            printf("Error forking");
            return;
        }
        //in child process
        if (pid == 0) {
            sigprocmask(SIG_SETMASK, &prev_mask, NULL);
            setpgid(0,0);
            Signal(SIGINT, SIG_DFL);
            Signal(SIGTSTP, SIG_DFL);

            //for input and output redirection
            setup_redirection(argv);

            //replace & with null terminator so that exec runs correctly
            if (strcmp(argv[argc-1], "&") == 0) { //background process
                argv[argc-1] = '\0';
            }

            err = execvp(argv[0], argv);
            if (err < 0) {
                printf("%s: Command not found\n", argv[0]);
                exit(err);
            }
        }

        //in parent process
        setpgid(pid, pid);
        Signal(SIGINT, sigint_handler);
        Signal(SIGTSTP, sigtstp_handler);

        if (strcmp(argv[argc-1], "&") == 0) { //background process
            addjob(jobs, pid, BG, cmdline);
            jid = pid2jid(pid);
            job = getjobpid(jobs, pid);
            printf("[%d] (%d) %s", jid, pid, job->cmdline);
            sigprocmask(SIG_SETMASK, &prev_mask, NULL);
        } else { //foreground process
            addjob(jobs, pid, FG, cmdline);
            sigprocmask(SIG_SETMASK, &prev_mask, NULL);
            waitfg(pid);
        }

    }
}

void setup_redirection(char **argv) {
    int i;
    for (i = 0; argv[i] != NULL; i++) {
        if (strcmp(argv[i], "<") == 0) { 
            int fd0 = open(argv[i+1], O_RDONLY, 0);
            dup2(fd0, STDIN_FILENO);
            close(fd0);
            argv[i] = NULL; // remove redirection from argv so it runs correctly
        }
        else if (strcmp(argv[i], ">") == 0) { 
            int fd1 = open(argv[i+1], O_WRONLY | O_CREAT, S_IRUSR | S_IWUSR);
            dup2(fd1, STDOUT_FILENO);
            close(fd1);
            argv[i] = NULL; 
        }
    }
}

int has_piping(char **argv, int argc) {
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "|") == 0) {
            return 1; // we found a pipe
        }
    }
    return 0; // we didn't find a pipe
}

void my_pipe(char **argv, int argc, sigset_t *prev_mask, char *cmdline) {
    int err;
    char *new_argv[MAXARGS][MAXARGS];
    int count = 0;
    int sec_count = -1;
    int pipefd[2];
    int prev_fd = -1;
    pid_t pid = 0;
    int jid;
    struct job_t *job;

    //parsing command line
    //split on the |, store in a 2d array of strings. for example:
    //ls | grep .txt -> [ [ls], [grep .txt] ]

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "|") == 0) {
            sec_count = -1;
            count ++;
        } else {
            sec_count ++;
            new_argv[count][sec_count] = argv[i];
        }
    }
    count ++; // this is necessary for some reason, it breaks without it.
    //loop over the 2d array
    for (int j = 0; j < count; j++) {

        err = pipe(pipefd);
        if (err != 0) {
            printf("Piping error");
            return;
        }

        pid = fork();
        if (pid == 0) { //child process
            sigprocmask(SIG_SETMASK, prev_mask, NULL);
            setpgid(0,0);
            Signal(SIGINT, SIG_DFL);
            Signal(SIGTSTP, SIG_DFL);

            //for input and output redirection
            setup_redirection(new_argv[j]);

            if (prev_fd != -1) {
                //if we are not at the first command, get input from the previous command. 
                //no else bc if we are at the first command, we just take input from STDIN
                dup2(prev_fd, STDIN_FILENO);
                close(prev_fd);
            }

            //if we are not at the last command, then redirect output to fd
            if (j < count - 1) {
                dup2(pipefd[1], STDOUT_FILENO);
            }
            
            err = execvp(new_argv[j][0], new_argv[j]);
            if (err == -1) {
                fprintf(stderr, "Failed to execute command: %s\n", new_argv[j][0]);
                return;
            }
        
            close(pipefd[0]);

        } else if (pid < 0) {
            perror("fork");
            return;
        } else { //parent process
            setpgid(pid, pid);
            Signal(SIGINT, sigint_handler);
            Signal(SIGTSTP, sigtstp_handler);

            if (prev_fd != -1) {
                close(prev_fd); // close the last reading end
            }
            close(pipefd[1]); //closing the writing end
            prev_fd = pipefd[0]; // save read end for the next command

            //exact same thing we do for a regular process
            if (strcmp(argv[argc-1], "&") == 0) { //background process
                addjob(jobs, pid, BG, cmdline);
                jid = pid2jid(pid);
                job = getjobpid(jobs, pid);
                printf("[%d] (%d) %s", jid, pid, job->cmdline);
                //pass in prev_mask instead of &prev_mask bc in this function its passed as an address
                sigprocmask(SIG_SETMASK, prev_mask, NULL);
            } else { //foreground process
                addjob(jobs, pid, FG, cmdline);
                //pass in prev_mask instead of &prev_mask bc in this function its passed as an address
                sigprocmask(SIG_SETMASK, prev_mask, NULL);
                waitfg(pid);
            }

        }
    }

}

/* 
 * parseline - Parse the command line and build the argv array.
 * 
 * Characters enclosed in single quotes are treated as a single
 * argument.  Return number of arguments parsed.
 */
int parseline(const char *cmdline, char **argv) {
    static char array[MAXLINE]; /* holds local copy of command line */
    char *buf = array;          /* ptr that traverses command line */
    char *delim;                /* points to space or quote delimiters */
    int argc;                   /* number of args */

    strcpy(buf, cmdline);
    buf[strlen(buf)-1] = ' ';  /* replace trailing '\n' with space */
    while (*buf && (*buf == ' ')) /* ignore leading spaces */
        buf++;

    /* Build the argv list */
    argc = 0;
    if (*buf == '\'') {
        buf++;
        delim = strchr(buf, '\'');
    }
    else {
        delim = strchr(buf, ' ');
    }

    while (delim) {
        argv[argc++] = buf;
        *delim = '\0';
        buf = delim + 1;
        while (*buf && (*buf == ' ')) /* ignore spaces */
            buf++;

        if (*buf == '\'') {
            buf++;
            delim = strchr(buf, '\'');
        }
        else {
            delim = strchr(buf, ' ');
        }
    }
    argv[argc] = NULL;
    
    return argc;
}

/* 
 * builtin_cmd - If the user has typed a built-in command then execute
 *    it immediately.  
 */
int builtin_cmd(char **argv) {
    if (strcmp(argv[0], "quit") == 0) {
        exit(0); // Exit the shell
    } else if (strcmp(argv[0], "jobs") == 0) {
        listjobs(jobs); // List all background jobs
        return 0;
    } else if (strcmp(argv[0], "bg") == 0 || strcmp(argv[0], "fg") == 0) {
        do_bgfg(argv); // Execute bg or fg command
        return 0;
    } else {
        return 1;
    }
}

/* 
 * do_bgfg - Execute the builtin bg and fg commands
 */
void do_bgfg(char **argv) {
    struct job_t *cur_job;
    int pid, jid;
    char *id = NULL;
    
    //ensuring we actually have an id argument
    if (argv[1] == NULL) {
        printf("%s command requires PID or %%jid argument\n", argv[0]);
        return;
    } else {
        id = argv[1];
    }

    if (id[0] == '%') {
        jid = atoi(&id[1]);
        if (jid == 0) {
            printf("%s: argument must be a PID or %%jid\n", argv[0]);
            return;
        }
        cur_job = getjobjid(jobs, jid);
        if (cur_job == NULL) {
            printf("%%%d: No such job\n", jid);
            return;
        }
    }
    else {
        pid = atoi(&id[0]);
        if (pid == 0) {
            printf("%s: argument must be a PID or %%jid\n", argv[0]);
            return;
        }
        cur_job = getjobpid(jobs, pid);
        if (cur_job == NULL) {
            printf("(%d): No such process\n", pid);
            return;
        }    
    }

    if (strcmp(argv[0], "bg") == 0 && cur_job->state == ST) {
        kill(-(cur_job->pid), SIGCONT);
        cur_job->state = BG;
        printf("[%d] (%d) %s", cur_job->jid, cur_job->pid, cur_job->cmdline);
    } 
    else if (strcmp(argv[0], "fg") == 0 && (cur_job->state == ST || cur_job->state == BG )) {
        kill(-(cur_job->pid), SIGCONT);
        cur_job->state = FG;
        waitfg(cur_job->pid);
    }

}

/* 
 * waitfg - Block until process pid is no longer the foreground process
 */
void waitfg(pid_t pid) {
    sigset_t mask, prev_mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGCHLD);
    sigprocmask(SIG_BLOCK, &mask, &prev_mask);

    // Suspend until the job is no longer in the foreground
    while (fgpid(jobs) == pid) {
        sigsuspend(&prev_mask);
    }

    sigprocmask(SIG_SETMASK, &prev_mask, NULL);
}


/*****************
 * Signal handlers
 *****************/

/* 
 * sigchld_handler - The kernel sends a SIGCHLD to the shell whenever
 *     a child job terminates (becomes a zombie), or stops because it
 *     received a SIGSTOP or SIGTSTP signal. The handler reaps all
 *     available zombie children, but doesn't wait for any other
 *     currently running children to terminate.  
 */
#include <unistd.h> // For write()

void sigchld_handler(int sig) {
    pid_t pid;
    int status;
    struct job_t *job;
    char buf[256]; // Buffer for messages
    int err;

    while ((pid = waitpid(-1, &status, WNOHANG | WUNTRACED)) > 0) {
        job = getjobpid(jobs, pid);

        if (WIFSTOPPED(status)) {
            // Update job state to stopped
            if (job != NULL) {
                job->state = ST;
            }
            int len = snprintf(buf, sizeof(buf), "Job [%d] (%d) stopped by signal %d\n", job->jid, job->pid, WSTOPSIG(status));
            if (len > 0) {
                err = write(STDOUT_FILENO, buf, len);
                if (err == -1) {
                    exit(1);
                }
            }
        } else if (WIFSIGNALED(status)) {
            // Job was terminated by a signal
            int len = snprintf(buf, sizeof(buf), "Job [%d] (%d) terminated by signal %d\n", job->jid, job->pid, WTERMSIG(status));
            if (len > 0) {
                err = write(STDOUT_FILENO, buf, len);
                if (err == -1) {
                    exit(1);
                }
            }
            deletejob(jobs, pid);
        } else if (WIFEXITED(status)) {
            // Job exited normally
            deletejob(jobs, pid);
        }
    }
}


/* 
 * sigint_handler - The kernel sends a SIGINT to the shell whenever the
 *    user types ctrl-c at the keyboard.  Catch it and send it along
 *    to the foreground job.  
 */
void sigint_handler(int sig) {

    pid_t pid = fgpid(jobs);

    if (pid != 0) {
        kill(-pid, SIGINT); // Send SIGINT to the entire foreground process group
    }
}

/*
 * sigtstp_handler - The kernel sends a SIGTSTP to the shell whenever
 *     the user types ctrl-z at the keyboard. Catch it and suspend the
 *     foreground job by sending it a SIGTSTP.  
 */
void sigtstp_handler(int sig) {

    pid_t pid = fgpid(jobs);
    struct job_t *job = getjobpid(jobs, pid);

    if (pid != 0) {
        kill(-pid, SIGTSTP); // Send SIGTSTP to the entire foreground process group
    }

    //setting job state to stopped
    if (job == NULL) {
        exit(1);
    } else {
        job->state = ST;
    }
}

/*
 * sigusr1_handler - child is ready
 */
void sigusr1_handler(int sig) {
    ready = 1;
}


/*********************
 * End signal handlers
 *********************/

/***********************************************
 * Helper routines that manipulate the job list
 **********************************************/

/* clearjob - Clear the entries in a job struct */
void clearjob(struct job_t *job) {
    job->pid = 0;
    job->jid = 0;
    job->state = UNDEF;
    job->cmdline[0] = '\0';
}

/* initjobs - Initialize the job list */
void initjobs(struct job_t *jobs) {
    int i;

    for (i = 0; i < MAXJOBS; i++)
        clearjob(&jobs[i]);
}

/* freejid - Returns smallest free job ID */
int freejid(struct job_t *jobs) {
    int i;
    int taken[MAXJOBS + 1] = {0};
    for (i = 0; i < MAXJOBS; i++)
        if (jobs[i].jid != 0) 
        taken[jobs[i].jid] = 1;
    for (i = 1; i <= MAXJOBS; i++)
        if (!taken[i])
            return i;
    return 0;
}

/* addjob - Add a job to the job list */
int addjob(struct job_t *jobs, pid_t pid, int state, char *cmdline) {
    int i;
    
    if (pid < 1)
        return 0;
    int free = freejid(jobs);
    if (!free) {
        printf("Tried to create too many jobs\n");
        return 0;
    }
    for (i = 0; i < MAXJOBS; i++) {
        if (jobs[i].pid == 0) {
            jobs[i].pid = pid;
            jobs[i].state = state;
            jobs[i].jid = free;
            strcpy(jobs[i].cmdline, cmdline);
            if(verbose){
                printf("Added job [%d] %d %s\n", jobs[i].jid, jobs[i].pid, jobs[i].cmdline);
            }
            return 1;
        }
    }
    return 0; /*suppress compiler warning*/
}

/* deletejob - Delete a job whose PID=pid from the job list */
int deletejob(struct job_t *jobs, pid_t pid) {
    int i;

    if (pid < 1)
        return 0;

    for (i = 0; i < MAXJOBS; i++) {
        if (jobs[i].pid == pid) {
            clearjob(&jobs[i]);
            return 1;
        }
    }
    return 0;
}

/* fgpid - Return PID of current foreground job, 0 if no such job */
pid_t fgpid(struct job_t *jobs) {
    int i;

    for (i = 0; i < MAXJOBS; i++)
        if (jobs[i].state == FG)
            return jobs[i].pid;
    return 0;
}

/* getjobpid  - Find a job (by PID) on the job list */
struct job_t *getjobpid(struct job_t *jobs, pid_t pid) {
    int i;

    if (pid < 1)
        return NULL;
    for (i = 0; i < MAXJOBS; i++)
        if (jobs[i].pid == pid)
            return &jobs[i];
    return NULL;
}

/* getjobjid  - Find a job (by JID) on the job list */
struct job_t *getjobjid(struct job_t *jobs, int jid) 
{
    int i;

    if (jid < 1)
        return NULL;
    for (i = 0; i < MAXJOBS; i++)
        if (jobs[i].jid == jid)
            return &jobs[i];
    return NULL;
}

/* pid2jid - Map process ID to job ID */
int pid2jid(pid_t pid) {
    int i;

    if (pid < 1)
        return 0;
    for (i = 0; i < MAXJOBS; i++)
        if (jobs[i].pid == pid) {
            return jobs[i].jid;
    }
    return 0;
}

/* listjobs - Print the job list */
void listjobs(struct job_t *jobs) {
    int i;
    
    for (i = 0; i < MAXJOBS; i++) {
        if (jobs[i].pid != 0) {
            printf("[%d] (%d) ", jobs[i].jid, jobs[i].pid);
            switch (jobs[i].state) {
                case BG: 
                    printf("Running ");
                    break;
                case FG: 
                    printf("Foreground ");
                    break;
                case ST: 
                    printf("Stopped ");
                    break;
                default:
                    printf("listjobs: Internal error: job[%d].state=%d ", 
                       i, jobs[i].state);
            }
            printf("%s", jobs[i].cmdline);
        }
    }
}
/******************************
 * end job list helper routines
 ******************************/


/***********************
 * Other helper routines
 ***********************/

/*
 * usage - print a help message and terminate
 */
void usage(void) {
    printf("Usage: shell [-hvp]\n");
    printf("   -h   print this message\n");
    printf("   -v   print additional diagnostic information\n");
    printf("   -p   do not emit a command prompt\n");
    exit(1);
}

/*
 * unix_error - unix-style error routine
 */
void unix_error(char *msg) {
    fprintf(stdout, "%s: %s\n", msg, strerror(errno));
    exit(1);
}

/*
 * app_error - application-style error routine
 */
void app_error(char *msg) {
    fprintf(stdout, "%s\n", msg);
    exit(1);
}

/*
 * Signal - wrapper for the sigaction function
 */
handler_t *Signal(int signum, handler_t *handler) {
    struct sigaction action, old_action;

    action.sa_handler = handler;  
    sigemptyset(&action.sa_mask); /* block sigs of type being handled */
    action.sa_flags = SA_RESTART; /* restart syscalls if possible */

    if (sigaction(signum, &action, &old_action) < 0)
        unix_error("Signal error");
    return (old_action.sa_handler);
}

/*
 * sigquit_handler - The driver program can gracefully terminate the
 *    child shell by sending it a SIGQUIT signal.
 */
void sigquit_handler(int sig) {
    printf("Terminating after receipt of SIGQUIT signal\n");
    exit(1);
}


/* 
 * tsh - A tiny shell program with job control
 * 
 * Mathieu Bordere
 */

/* TODO: safe writing in handlers */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <errno.h>

/* Misc manifest constants */
#define MAXLINE    1024   /* max line size */
#define MAXARGS     128   /* max args on a command line */
#define MAXJOBS      16   /* max jobs at any point in time */
#define MAXJID    1<<16   /* max job ID */

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
int nextjid = 1;            /* next job ID to allocate */
char sbuf[MAXLINE];         /* for composing sprintf messages */

struct job_t {              /* The job struct */
    pid_t pid;              /* job PID */
    int jid;                /* job ID [1, 2, ...] */
    int state;              /* UNDEF, BG, FG, or ST */
    char cmdline[MAXLINE];  /* command line */
};

struct job_t jobs[MAXJOBS]; /* The job list */
/* End global variables */

/* Function prototypes */

/* Here are the functions that you will implement */
void eval(char *cmdline);
int builtin_cmd(char **argv);
void do_bgfg(char **argv);
void waitfg(pid_t pid);

void sigchld_handler(int sig);
void sigtstp_handler(int sig);
void sigint_handler(int sig);

/* Here are helper routines that we've provided for you */
int parseline(const char *cmdline, char **argv); 
void sigquit_handler(int sig);

void clearjob(struct job_t *job);
void initjobs(struct job_t *jobs);
int maxjid(struct job_t *jobs); 
int addjob(struct job_t *jobs, pid_t pid, int state, char *cmdline);
int deletejob(struct job_t *jobs, pid_t pid); 
pid_t fgpid(struct job_t *jobs);
struct job_t *getjobpid(struct job_t *jobs, pid_t pid);
struct job_t *getjobjid(struct job_t *jobs, int jid); 
int pid2jid(pid_t pid); 
void listjobs(struct job_t *jobs);
void listbgjobs(struct job_t *jobs);
void listjob(struct job_t *job);

void usage(void);
void unix_error(char *msg);
void app_error(char *msg);
int isnumber(char *num);
typedef void handler_t(int);

handler_t *Signal(int signum, handler_t *handler);

/* Error handling wrappers around system calls */
int Kill(pid_t pid, int sig);
int Sigaddset(sigset_t *set, int signum);
int Sigemptyset(sigset_t *set);
int Sigfillset(sigset_t *set);
int Sigprocmask(int how, const sigset_t *set, sigset_t *oldset);

int main(int argc, char **argv) 
{
    char c;
    char cmdline[MAXLINE];
    int emit_prompt = 1; /* emit prompt (default) */

    /* Redirect stderr to stdout (so that driver will get all output
     * on the pipe connected to stdout) */
    dup2(1, 2);

    /* Parse the command line */
    while ((c = getopt(argc, argv, "hvp")) != EOF) {
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

    	if ((fgets(cmdline, MAXLINE, stdin) == NULL) && ferror(stdin)) {
    	    app_error("fgets error");
        }

    	if (feof(stdin)) { /* End of file (ctrl-d) */
    	    fflush(stdout);
    	    exit(0);
    	}

    	/* Evaluate the command line */
    	eval(cmdline);
    	fflush(stdout);
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
void eval(char *cmdline) 
{
    /* allocate storage for argv array */
    char *argv[MAXARGS];
    int bg;
    pid_t pid;
    sigset_t mask_all, mask_sigchld, prev_one;

    Sigfillset(&mask_all);
    Sigemptyset(&mask_sigchld);
    Sigaddset(&mask_sigchld, SIGCHLD);

    bg = parseline(cmdline, argv);
    
    /* return on empty command */
    if (bg == -1) {
        return; 
    }
    
    /* executes builtin_cmd directly in the logical test if the command
    is built-in. If not, executes the non-built-in command */
    if (!builtin_cmd(argv)) {
        pid = fork();

        if (pid < 0) {
            unix_error("fork failed.");
            exit(1);
        }

        /* Block SIGCHLD signals */
        Sigprocmask(SIG_BLOCK, &mask_sigchld, &prev_one);

        /* child */
        if (pid == 0) {

            /* put child in new processgroup with pgid = pid[child] */
            if(setpgid(0, 0) < 0) {
                unix_error("setpgid failed");
            }

            /* Unblock SIGCHLD */
            Sigprocmask(SIG_SETMASK, &prev_one, NULL);
            
            if (execve(argv[0], argv, environ) < 0) {
                unix_error("Command not found.");
                exit(0);
            }
        }

        /* parent */
        Sigprocmask(SIG_BLOCK, &mask_all, NULL);
        addjob(jobs, pid, bg+1, cmdline);
        if (bg) {
            listjob(getjobpid(jobs, pid));
        }
        Sigprocmask(SIG_SETMASK, &prev_one, NULL);

        if (!bg) {  
            waitfg(pid);
        }
    }  
    return;
}

/* 
 * parseline - Parse the command line and build the argv array.
 * 
 * Characters enclosed in single quotes are treated as a single
 * argument.  Return true if the user has requested a BG job, false if
 * the user has requested a FG job.  
 */
int parseline(const char *cmdline, char **argv) 
{
    static char array[MAXLINE]; /* holds local copy of command line */
    char *buf = array;          /* ptr that traverses command line */
    char *delim;                /* points to first space delimiter */
    int argc;                   /* number of args */
    int bg;                     /* background job? */

    strcpy(buf, cmdline);

    /* replace trailing '\n' with space */
    buf[strlen(buf)-1] = ' ';  
    
    /* ignore leading spaces */
    while (*buf && (*buf == ' ')) {
        buf++;
    }

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

        /* ignore spaces */
    	while (*buf && (*buf == ' ')) {
            buf++;       
        }
    	       
    	if (*buf == '\'') {
    	    buf++;
    	    delim = strchr(buf, '\'');
    	}

    	else {
    	    delim = strchr(buf, ' ');
    	}   
    }

    argv[argc] = NULL;
    
    /* ignore blank line */
    if (argc == 0) {
        return -1;
    }  

    /* should the job run in the background? */
    if ((bg = (*argv[argc-1] == '&')) != 0) {
        argv[--argc] = NULL;
    }

    return bg;
}

/* 
 * builtin_cmd - If the user has typed a built-in command then execute it immediately.
 * supported cmds: bg, fg, quit, jobs  
 * returns 0 if the command is not built-in
 */
int builtin_cmd(char **argv) 
{
    if (!strcmp(argv[0], "quit")) {
        exit(0);
        return 1;
    } else if (!strcmp(argv[0], "bg")) {
        do_bgfg(argv);
        return 1;
    } else if (!strcmp(argv[0], "fg")) {
        do_bgfg(argv);
        return 1;
    } else if (!strcmp(argv[0], "jobs")) {
        listbgjobs(jobs);
        fflush(stdout);
        return 1;
    } else {
        return 0;
    }
}

/* 
 * do_bgfg - Execute the builtin bg and fg commands
 */
void do_bgfg(char **argv) 
{
    sigset_t mask_sigchld, prev_one;
    struct job_t *job;
    int isbg = !strcmp(argv[0], "bg");

    if (argv[1] == NULL) {
        printf("%s command requires PID or %%jobid argument\n", argv[0]);
        return;
    }

    int isjid = '%' == argv[1][0];

    if (isjid) {
        char *jid = &argv[1][1];
        if (!isnumber(jid)) {
            printf("%s: argument must be a PID or %%jobid\n", argv[0]);
            return;
        }
        job = getjobjid(jobs, atoi(jid));
        if (job == NULL) {
            printf("no such job\n");
            return;
        }
    } else {
        if (!isnumber(argv[1])) {
            printf("%s: argument must be a PID or %%jobid\n", argv[0]);
            return;
        }
        job = getjobpid(jobs, atoi(argv[1]));
        if (job == NULL) {
            printf("no such process\n");
            return;
        }
    }

    Sigemptyset(&mask_sigchld);
    Sigaddset(&mask_sigchld, SIGCHLD);

    if (isbg && job->state != ST) {
        printf("bg error - Job is not STOPPED.\n");
        return;
    }

    if (!isbg && !(job->state == ST || job->state == BG)) {
        printf("fg error - Job is not STOPPED or in BACKGROUND.\n");
        return;
    }

    /* we want to mask SIGCHLD so that it isn't caught before state is adapted */
    Sigprocmask(SIG_BLOCK, &mask_sigchld, &prev_one);
    Kill(-(job->pid), SIGCONT);
    if (isbg) {
        listjob(job);
        job->state = BG;
    } else {
        job->state = FG;
    }
    Sigprocmask(SIG_SETMASK, &prev_one, NULL);

    if (!isbg) {
        waitfg(job->pid);
    }

    return;
}

/* 
 * waitfg - Block until process pid is no longer the foreground process
 */
void waitfg(pid_t pid)
{
    /* sleep indefinitely untill FG process is no longer the FG process */
    while(1) {
        sleep(1);
        if (fgpid(jobs) != pid) {
            break;
        }
    }
    return;
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
void sigchld_handler(int sig) 
{
    int olderrno = errno;
    int status;
    sigset_t mask_all, prev_all;
    pid_t pid;

    Sigfillset(&mask_all);
    while((pid = waitpid(-1, &status, WNOHANG | WUNTRACED)) > 0) {
        
        /* If we exited normally, we can safely delete the job */
        if (WIFEXITED(status)) {
            Sigprocmask(SIG_BLOCK, &mask_all, &prev_all);
            deletejob(jobs, pid);
            Sigprocmask(SIG_SETMASK, &prev_all, NULL);
        }

        if (WIFSTOPPED(status)) {
            printf("Job [%d] (%d) stopped by signal %d\n", pid2jid(pid), pid, WSTOPSIG(status));
        }

        if (WIFSIGNALED(status)) {
            printf("Job [%d] (%d) terminated by signal %d\n", pid2jid(pid), pid, WTERMSIG(status));
            Sigprocmask(SIG_BLOCK, &mask_all, &prev_all);
            deletejob(jobs, pid);
            Sigprocmask(SIG_SETMASK, &prev_all, NULL);
        }
    }

    errno = olderrno;
    return;
}

/* 
 * sigint_handler - The kernel sends a SIGINT to the shell whenver the
 *    user types ctrl-c at the keyboard.  Catch it and send it along
 *    to the foreground job.  
 */
void sigint_handler(int sig) 
{
    int olderrno = errno;
    pid_t pid = fgpid(jobs);
    
    if (pid == 0) {
        return;
    }

    Kill(-pid, SIGINT);
    errno = olderrno;
    return;
}

/*
 * sigtstp_handler - The kernel sends a SIGTSTP to the shell whenever
 *     the user types ctrl-z at the keyboard. Catch it and suspend the
 *     foreground job by sending it a SIGTSTP.  
 */
void sigtstp_handler(int sig) 
{
    int olderrno = errno;
    pid_t pid = fgpid(jobs);
    sigset_t mask_all, prev_all;

    Sigfillset(&mask_all);
    
    if (pid == 0) {
        return;
    }

    Kill(-pid, SIGTSTP);

    /* update foreground job state to ST */
    Sigprocmask(SIG_BLOCK, &mask_all, &prev_all);
    struct job_t *fgjob = getjobpid(jobs, pid);
    fgjob->state = ST;
    Sigprocmask(SIG_SETMASK, &prev_all, NULL);

    errno = olderrno;
    return;
}

/*
 * sigquit_handler - The driver program can gracefully terminate the
 *    child shell by sending it a SIGQUIT signal.
 */
void sigquit_handler(int sig) 
{
    printf("Terminating after receipt of SIGQUIT signal\n");
    exit(1);
}

/*********************
 * End signal handlers
 *********************/

/***********************************************
 * Helper routines that manipulate the job list
 **********************************************/

/* clearjob - Clear the entries in a job struct */
void clearjob(struct job_t *job) 
{
    job->pid = 0;
    job->jid = 0;
    job->state = UNDEF;
    job->cmdline[0] = '\0';
}

/* initjobs - Initialize the job list */
void initjobs(struct job_t *jobs) 
{
    int i;

    for (i = 0; i < MAXJOBS; i++) {
        clearjob(&jobs[i]);
    }
}

/* maxjid - Returns largest allocated job ID */
int maxjid(struct job_t *jobs) 
{
    int i, max=0;

    for (i = 0; i < MAXJOBS; i++) {
    	if (jobs[i].jid > max) {
    	    max = jobs[i].jid;
        }
    }

    return max;
}

/* addjob - Add a job to the job list */
int addjob(struct job_t *jobs, pid_t pid, int state, char *cmdline) 
{
    int i;
    
    if (pid < 1) {
        return 0;
    }

    for (i = 0; i < MAXJOBS; i++) {
    	if (jobs[i].pid == 0) {
    	    jobs[i].pid = pid;
    	    jobs[i].state = state;
    	    jobs[i].jid = nextjid++;
    	    if (nextjid > MAXJOBS) {
                nextjid = 1;
            }
    	    strcpy(jobs[i].cmdline, cmdline);
      	    if(verbose){
    	        printf("Added job [%d] %d %s\n", jobs[i].jid, jobs[i].pid, jobs[i].cmdline);
            }
            return 1;
    	}
    }
    printf("Tried to create too many jobs\n");
    return 0;
}

/* deletejob - Delete a job whose PID=pid from the job list */
int deletejob(struct job_t *jobs, pid_t pid) 
{
    int i;

    if (pid < 1) {
        return 0;
    }

    for (i = 0; i < MAXJOBS; i++) {
    	if (jobs[i].pid == pid) {
    	    clearjob(&jobs[i]);
    	    nextjid = maxjid(jobs)+1;
    	    return 1;
    	}
    }
    return 0;
}

/* fgpid - Return PID of current foreground job, 0 if no such job */
pid_t fgpid(struct job_t *jobs) 
{
    int i;

    for (i = 0; i < MAXJOBS; i++) {
        if (jobs[i].state == FG) {
            return jobs[i].pid;
        } 
    }
    	
    return 0;
}

/* getjobpid  - Find a job (by PID) on the job list */
struct job_t *getjobpid(struct job_t *jobs, pid_t pid) 
{
    int i;

    if (pid < 1) { 
        return NULL;
    }
	
    for (i = 0; i < MAXJOBS; i++) {
        if (jobs[i].pid == pid) {
            return &jobs[i];
        }        
    }
	
    return NULL;
}

/* getjobjid  - Find a job (by JID) on the job list */
struct job_t *getjobjid(struct job_t *jobs, int jid) 
{
    int i;

    if (jid < 1) { 
        return NULL;
    } 
	
    for (i = 0; i < MAXJOBS; i++) {
        if (jobs[i].jid == jid) {
            return &jobs[i];
        }      
    }
	
    return NULL;
}

/* pid2jid - Map process ID to job ID */
int pid2jid(pid_t pid) 
{
    int i;

    if (pid < 1) {
        return 0;
    }
	
    for (i = 0; i < MAXJOBS; i++) {
        if (jobs[i].pid == pid) {
            return jobs[i].jid;
        }  
    }
	
    return 0;
}

/* listjobs - Print the job list */
void listjobs(struct job_t *jobs) 
{
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
                    printf("listjobs: Internal error: job[%d].state=%d ", i, jobs[i].state);
    	    }
    	    printf("%s", jobs[i].cmdline);
    	}
    }
}

/* listbgjobs - Print the bg job list */
void listbgjobs(struct job_t *jobs) 
{
    int i;
    
    for (i = 0; i < MAXJOBS; i++) {
        if (jobs[i].pid != 0) {    
            switch (jobs[i].state) {
                case BG:
                    printf("[%d] (%d) ", jobs[i].jid, jobs[i].pid);
                    printf("Running ");
                    printf("%s", jobs[i].cmdline);
                    break;
                case ST:
                    printf("[%d] (%d) ", jobs[i].jid, jobs[i].pid);
                    printf("Stopped ");
                    printf("%s", jobs[i].cmdline);
                    break;
                default:
                    break;
            }
        }
    }
}

/******************************
 * end job list helper routines
 ******************************/


/***********************
 * Other helper routines
 ***********************/

void listjob(struct job_t *job)
{
    printf("[%d] (%d) %s", job->jid, job->pid, job->cmdline);
}

/*
 * usage - print a help message
 */
void usage(void) 
{
    printf("Usage: shell [-hvp]\n");
    printf("   -h   print this message\n");
    printf("   -v   print additional diagnostic information\n");
    printf("   -p   do not emit a command prompt\n");
    exit(1);
}

/*
 * unix_error - unix-style error routine
 */
void unix_error(char *msg)
{
    fprintf(stdout, "%s: %s\n", msg, strerror(errno));
    exit(1);
}

/*
 * app_error - application-style error routine
 */
void app_error(char *msg)
{
    fprintf(stdout, "%s\n", msg);
    exit(1);
}

/* return 1 if number 0 if not */
int isnumber(char *num) {
    int i = 0;
    while (num[i] != '\0') {
        if (!isdigit(num[i])) {
            return 0;
        }
        i++;
    }
    return 1;
}

/*
 * Signal - wrapper for the sigaction function
 */
handler_t *Signal(int signum, handler_t *handler) 
{
    struct sigaction action, old_action;

    action.sa_handler = handler;  
    sigemptyset(&action.sa_mask); /* block sigs of type being handled */
    action.sa_flags = SA_RESTART; /* restart syscalls if possible */

    if (sigaction(signum, &action, &old_action) < 0) {
        unix_error("Signal error");   
    }
	
    return (old_action.sa_handler);
}

/* Error handling wrappers around system calls */
int Kill(pid_t pid, int sig) 
{
    int res = kill(pid, sig);
    if (res < 0) {
        unix_error("kill failed");
    }
    return res;
}

int Sigaddset(sigset_t *set, int signum) 
{
    int res = sigaddset(set, signum);
    if (res < 0) {
        unix_error("sigaddset error");
    }
    return res;
}

int Sigemptyset(sigset_t *set)
{
    int res = sigemptyset(set);
    if (res < 0) {
        unix_error("sigemptyset error");
    }
    return res;
}

int Sigfillset(sigset_t *set)
{
    int res = sigfillset(set);
    if (res < 0) {
        unix_error("sigfillset error");
    }
    return res;
}

int Sigprocmask(int how, const sigset_t *set, sigset_t *oldset)
{
    int res = sigprocmask(how, set, oldset);
    if (res < 0) {
        unix_error("sigprocmask error");
    }
    return res;
}
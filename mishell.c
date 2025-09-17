#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/resource.h>
#include <sys/time.h>
#include <time.h>
#include <fcntl.h>
#include <errno.h>
#include <signal.h>
#include <limits.h>

#define PROMPT_FMT "mishell:%s$ "
#define MAX_LINE 4096
#define MAX_ARGS 256

volatile pid_t child_to_kill = 0;

void on_sigalrm(int sig) {
    if (child_to_kill > 0) {
        kill(child_to_kill, SIGKILL);
    }
}

char *trim(char *s) {
    if (!s) return s;
    while(*s && (*s==' '||*s=='\t'||*s=='\n' || *s=='\r')) s++;
    if (*s == 0) return s;
    char *end = s + strlen(s) - 1;
    while(end > s && (*end==' '||*end=='\t'||*end=='\n' || *end=='\r')) { *end = 0; end--; }
    return s;
}

char **split_args(char *cmd) {
    char **argv = calloc(MAX_ARGS, sizeof(char*));
    int argc = 0;
    char *p = cmd;
    while (*p) {
        while (*p && (*p==' ' || *p=='\t')) p++;
        if (!*p) break;
        char *start;
        if (*p == '\'' || *p == '"') {
            char quote = *p++;
            start = p;
            while (*p && *p != quote) p++;
        } else {
            start = p;
            while (*p && *p != ' ' && *p != '\t') p++;
        }
        size_t len = p - start;
        char *tok = malloc(len + 1);
        strncpy(tok, start, len);
        tok[len] = 0;
        argv[argc++] = tok;
        if (*p) p++;
    }
    argv[argc] = NULL;
    return argv;
}

void free_argv(char **argv) {
    if (!argv) return;
    for (int i=0; argv[i]; ++i) free(argv[i]);
    free(argv);
}

int execute_and_profile(char **argv, struct rusage *rusage_out, double *real_seconds, long *maxrss_out, int timeout_seconds, FILE *save_fp) {
    struct timespec t_start, t_end;
    pid_t pid;
    int status = -1;

    if (clock_gettime(CLOCK_MONOTONIC, &t_start) == -1) perror("clock_gettime");

    pid = fork();
    if (pid < 0) {
        perror("fork");
        return -1;
    } else if (pid == 0) {

        signal(SIGALRM, SIG_DFL);
        execvp(argv[0], argv);
 

        fprintf(stderr, "mishell: comando no encontrado o fallo exec: %s\n", argv[0]);
        _exit(127);
    } else {

        child_to_kill = pid;
        struct sigaction sa;
        sa.sa_handler = on_sigalrm;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_RESTART;
        sigaction(SIGALRM, &sa, NULL);

        if (timeout_seconds > 0) {
            alarm(timeout_seconds);
        }

        if (wait4(pid, &status, 0, rusage_out) == -1) {
            perror("wait4");
        }

        alarm(0);
        child_to_kill = 0;

        if (clock_gettime(CLOCK_MONOTONIC, &t_end) == -1) perror("clock_gettime");
        *real_seconds = (t_end.tv_sec - t_start.tv_sec) + (t_end.tv_nsec - t_start.tv_nsec)/1e9;

        if (rusage_out && maxrss_out) {
            *maxrss_out = rusage_out->ru_maxrss; 
        }

        if (save_fp) {

            fprintf(save_fp, "===== miprof result: command:");
            for (int i=0; argv[i]; ++i) fprintf(save_fp, " %s", argv[i]);
            fprintf(save_fp, "\nreal: %.6f s\nuser: %.6f s\nsys:  %.6f s\nmaxrss: %ld\n\n",
                *real_seconds,
                (double)rusage_out->ru_utime.tv_sec + rusage_out->ru_utime.tv_usec/1e6,
                (double)rusage_out->ru_stime.tv_sec + rusage_out->ru_stime.tv_usec/1e6,
                *maxrss_out);
            fflush(save_fp);
        }
    }
    return status;
}


void execute_pipeline(char **cmds, int ncmds) {
    int i;
    int in_fd = -1; 
    int pipefd[2];

    pid_t *pids = malloc(ncmds * sizeof(pid_t));
    for (i = 0; i < ncmds; ++i) {
        if (i < ncmds - 1) {
            if (pipe(pipefd) == -1) { perror("pipe"); return; }
        }

        char **argv = split_args(cmds[i]);
        if (!argv || !argv[0]) { free_argv(argv); if (i < ncmds - 1) { close(pipefd[0]); close(pipefd[1]); } continue; }

        pid_t pid = fork();
        if (pid < 0) { perror("fork"); free_argv(argv); return; }
        if (pid == 0) {
            /*hijo*/
            if (in_fd != -1) {
                dup2(in_fd, STDIN_FILENO);
                close(in_fd);
            }
            if (i < ncmds - 1) {
                close(pipefd[0]); 
                dup2(pipefd[1], STDOUT_FILENO);
                close(pipefd[1]);
            }
            
            execvp(argv[0], argv);
            fprintf(stderr, "mishell: comando no encontrado o fallo exec: %s\n", argv[0]);
            _exit(127);
        } else {
            /* padre */
            pids[i] = pid;
            if (in_fd != -1) close(in_fd);
            if (i < ncmds - 1) {
                close(pipefd[1]); 
                in_fd = pipefd[0]; 
            }
            free_argv(argv);
        }
    }

    /* se espera a los hijos */
    for (i=0;i<ncmds;++i) {
        int st;
        waitpid(pids[i], &st, 0);
    }
    free(pids);
}

void handle_miprof(char **argv) {
    if (!argv || !argv[0]) {
        fprintf(stderr, "uso: miprof [ejec | ejecsave archivo | maxtiempo segundos] comando args...\n");
        return;
    }

    if (strcmp(argv[0], "ejec") == 0) {
        if (!argv[1]) { fprintf(stderr, "miprof ejec: falta comando\n"); return; }
        
        char **cmd_argv = argv + 1;
        struct rusage r;
        double real;
        long maxrss;
        int st = execute_and_profile(cmd_argv, &r, &real, &maxrss, 0, NULL);
        /* Print:D */
        printf("real: %.6f s\nuser: %.6f s\nsys:  %.6f s\nmaxrss: %ld\n",
            real,
            (double)r.ru_utime.tv_sec + r.ru_utime.tv_usec/1e6,
            (double)r.ru_stime.tv_sec + r.ru_stime.tv_usec/1e6,
            maxrss);
    } else if (strcmp(argv[0], "ejecsave") == 0) {
        if (!argv[1]) { fprintf(stderr, "miprof ejecsave: falta archivo\n"); return; }
        char *filename = argv[1];
        if (!argv[2]) { fprintf(stderr, "miprof ejecsave: falta comando\n"); return; }
        char **cmd_argv = argv + 2;
        FILE *fp = fopen(filename, "a");
        if (!fp) { perror("fopen"); return; }
        struct rusage r;
        double real;
        long maxrss;
        int st = execute_and_profile(cmd_argv, &r, &real, &maxrss, 0, fp);
        
        

        printf("real: %.6f s\nuser: %.6f s\nsys:  %.6f s\nmaxrss: %ld\n",
            real,
            (double)r.ru_utime.tv_sec + r.ru_utime.tv_usec/1e6,
            (double)r.ru_stime.tv_sec + r.ru_stime.tv_usec/1e6,
            maxrss);
        fclose(fp);
    } else if (strcmp(argv[0], "maxtiempo") == 0) {
        if (!argv[1]) { fprintf(stderr, "miprof maxtiempo: falta segundos\n"); return; }
        char *endptr;
        long secs = strtol(argv[1], &endptr, 10);
        if (*endptr != '\0' || secs <= 0) { fprintf(stderr, "miprof maxtiempo: segundos inválidos\n"); return; }
        if (!argv[2]) { fprintf(stderr, "miprof maxtiempo: falta comando\n"); return; }
        char **cmd_argv = argv + 2;
        struct rusage r;
        double real;
        long maxrss;
       
        int st = execute_and_profile(cmd_argv, &r, &real, &maxrss, (int)secs, NULL);
        printf("real: %.6f s\nuser: %.6f s\nsys:  %.6f s\nmaxrss: %ld\n",
            real,
            (double)r.ru_utime.tv_sec + r.ru_utime.tv_usec/1e6,
            (double)r.ru_stime.tv_sec + r.ru_stime.tv_usec/1e6,
            maxrss);
    } else {
        fprintf(stderr, "miprof: opción desconocida '%s'\n", argv[0]);
    }
}

int main(int argc, char **argv) {
    char line[MAX_LINE];
    char cwd[PATH_MAX];

    
    struct sigaction sa_int;
    sa_int.sa_handler = SIG_IGN;
    sigemptyset(&sa_int.sa_mask);
    sa_int.sa_flags = 0;
    sigaction(SIGINT, &sa_int, NULL);

    struct sigaction sa_alrm;
    sa_alrm.sa_handler = on_sigalrm;
    sigemptyset(&sa_alrm.sa_mask);
    sa_alrm.sa_flags = SA_RESTART;
    sigaction(SIGALRM, &sa_alrm, NULL);

    while (1) {
        if (!getcwd(cwd, sizeof(cwd))) strcpy(cwd, "?");
        printf(PROMPT_FMT, cwd);
        fflush(stdout);

        if (!fgets(line, sizeof(line), stdin)) {

            printf("\n");
            break;
        }

        char *pline = trim(line);
        if (!pline || *pline == '\0') continue;


        if (strcmp(pline, "exit") == 0) break;


        char *saveptr = NULL;
        int ncmds = 0;
        char *cmds[128];
        char *token = strtok_r(pline, "|", &saveptr);
        while (token && ncmds < 128) {
            cmds[ncmds++] = trim(token);
            token = strtok_r(NULL, "|", &saveptr);
        }
        if (ncmds == 0) continue;

        
        if (ncmds == 1) {
            
            char *cmdcopy = strdup(cmds[0]);
            char **firstargv = split_args(cmdcopy);
            if (firstargv && firstargv[0]) {
                if (strcmp(firstargv[0], "miprof") == 0) {
                    
                    handle_miprof(firstargv+1);
                    free_argv(firstargv);
                    free(cmdcopy);
                    continue;
                }
                
                if (strcmp(firstargv[0], "cd") == 0) {
                    if (firstargv[1]) {
                        if (chdir(firstargv[1]) == -1) perror("cd");
                    } else {
                        fprintf(stderr, "cd: falta argumento\n");
                    }
                    free_argv(firstargv);
                    free(cmdcopy);
                    continue;
                }
            }
            free_argv(firstargv);
            free(cmdcopy);
        }

       
        if (ncmds == 1) {
            
            char **a = split_args(cmds[0]);
            if (!a || !a[0]) { free_argv(a); continue; }
            pid_t pid = fork();
            if (pid < 0) { perror("fork"); free_argv(a); continue; }
            if (pid == 0) {
                
                signal(SIGINT, SIG_DFL);
                execvp(a[0], a);
                fprintf(stderr, "mishell: comando no encontrado o fallo exec: %s\n", a[0]);
                _exit(127);
            } else {
                int st;
                waitpid(pid, &st, 0);
            }
            free_argv(a);
        } else {
            
            execute_pipeline(cmds, ncmds);
        }
    }

    return 0;
}

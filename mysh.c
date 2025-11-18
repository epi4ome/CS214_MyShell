#include <stdio.h>      
#include <stdlib.h>     
#include <string.h>     
#include <unistd.h>     
#include <sys/types.h>  
#include <sys/wait.h>   
#include <fcntl.h>      
#include <errno.h> 
#include <limits.h>

#define TOKEN 128

int is_space(char c) {
    return (c == ' ' || c == '\t' || c == '\n');
}

static int is_word(char c) {
    return !(c == '\0' || c == '\n' || c == '#' || is_space(c) || 
             c == '<' || c == '>' || c == '|');
}

char** tokenize(const char *line, int *token_count) {
    size_t cap = 16;
    char **tokens = malloc(cap * sizeof(char*));
    if (!tokens) {
        perror("malloc");
        *token_count = 0;
        return NULL;
    }
    *token_count = 0;
    const char *p = line;

    while (1) {
        if (*p == '\0' || *p == '\n' || *p == '#') break;

        if (is_space(*p)) { p++; continue; }

        if (*p == '<' || *p == '>' || *p == '|') {
            if (*token_count + 2 >= cap) {
                cap *= 2;
                char **nt = realloc(tokens, cap * sizeof(char*));
                if (!nt) { perror("realloc"); break; }
                tokens = nt;
            }
            char *t = malloc(2);
            if (!t) { perror("malloc"); break; }
            t[0] = *p; t[1] = '\0';
            tokens[(*token_count)++] = t;
            p++;
            continue;
        }

        const char *start = p;
        while (is_word(*p)) p++;
        size_t len = p - start;
        if (*token_count + 2 >= cap) {
            cap *= 2;
            char **nt = realloc(tokens, cap * sizeof(char*));
            if (!nt) { perror("realloc"); break; }
            tokens = nt;
        }
        char *word = malloc(len + 1);
        if (!word) { perror("malloc"); break; }
        strncpy(word, start, len);
        word[len] = '\0';
        tokens[(*token_count)++] = word;
    }

    tokens[*token_count] = NULL;
    return tokens;
}

void free_token(char **tokens, int count) {
    for (int i = 0; i < count; i++) free(tokens[i]);
    free(tokens);
}

//---
typedef struct{
    char **args;
    int arg_count;
    char *input_file;
    char *output_file;
} Command;

typedef struct{
    Command *commands; //array of commands
    int command_count; 
    int conditional;
} Job;

static int shell_terminate = 0; /* 0 = continue, 1 = exit, 2 = die */
static int shell_exit_code = EXIT_SUCCESS;

static int last_exit_status = 0;
static int shell_interactive = 1;

void free_job(Job *job);

char* resolve_program(const char *name) {
    if (strchr(name, '/') != NULL) {
        if (access(name, X_OK) == 0) {
            return strdup(name);
        }
        return NULL;
    }

    const char *search_dirs[] = {"/usr/local/bin", "/usr/bin", "/bin"};
    char full_path[PATH_MAX];

    for (int i = 0; i < 3; i++) {
        snprintf(full_path, sizeof(full_path), "%s/%s", search_dirs[i], name);
        if (access(full_path, X_OK) == 0) {
            return strdup(full_path);
        }
    }

    return NULL;

}

int builtin_cd(Command *cmd) {
    if (cmd->arg_count != 2) {
        fprintf(stderr, "cd: missing argument\n");
        return -1;
    }
    if (chdir(cmd->args[1]) != 0) {
        fprintf(stderr, "cd: No such file or directory\n");
        return -1;
    }
    return 0;
}

int builtin_pwd(Command *cmd) {
    if (cmd->arg_count != 1) {
        fprintf(stderr, "pwd: wrong number of arguments\n");
        return -1;
    }
    char cwd[1024];
    if (getcwd(cwd, sizeof(cwd)) == NULL) {
        perror("pwd");
        return -1;
    }

    printf("%s\n", cwd);
    return 0;
}

int builtin_which(Command *cmd) {
    if (cmd->arg_count != 2) {
        fprintf(stderr, "which: missing argument\n");
        return -1;
    }

    const char *name = cmd->args[1];

    if (strcmp(name, "cd") == 0 || strcmp(name, "pwd") == 0 || strcmp(name, "which") == 0 || strcmp(name, "exit") == 0 || strcmp(name, "die") == 0) {
        return -1;
    }

    char *path = resolve_program(name);
    if (path == NULL) {
        return -1;
    }
    printf("%s\n", path);
    free(path);
    return 0;
}

int builtin_exit(Command *cmd) {
    (void)cmd;
    shell_terminate = 1;
    shell_exit_code = EXIT_SUCCESS;
    return 0;
}

int builtin_die(Command *cmd){
    for (int i = 1; i < cmd->arg_count; i++){
        if (i > 1) printf(" ");
        printf("%s", cmd->args[i]);
    }
    if (cmd->arg_count > 1) printf("\n");
    shell_terminate = 2;
    shell_exit_code = EXIT_FAILURE;
    return -1;
}

int redirect_input(const char *filename) {
    int fd = open(filename, O_RDONLY);
    if (fd < 0) {
        perror("open input file");
        return -1;
    }
    if (dup2(fd, STDIN_FILENO) < 0) {
        perror("dup2 input");
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

int redirect_output(const char *filename) {
    int fd = open(filename, O_WRONLY | O_CREAT | O_TRUNC, 0640);
    if (fd < 0) {
        perror("open output file");
        return -1;
    }
    if (dup2(fd, 1) < 0) {
        perror("dup2 output");
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

int execute_pipeline(Job *job) {
    int num_commands = job->command_count;
    pid_t pids[num_commands];
    int pipes[num_commands - 1][2];

    for (int i = 0; i < num_commands - 1; i++) {
        if (pipe(pipes[i]) < 0) {
            perror("pipe");
            return -1;
        }
    }

    for (int i = 0; i < num_commands; i++) {
        pid_t pid = fork();

        if (pid < 0) {
            perror("fork");
            return -1;
        }

        if (pid == 0) {
            if (!shell_interactive && job->commands[i].input_file == NULL) {
                int nd = open("/dev/null", O_RDONLY);
                if (nd >= 0) {
                    if (dup2(nd, STDIN_FILENO) < 0) {
                        perror("dup2 /dev/null");
                        close(nd);
                        exit(1);
                    }
                    close(nd);
                }
            }

            if (i > 0) {
                if (dup2(pipes[i - 1][0], 0) < 0) {
                    perror("dup2 stdin");
                    exit(1);
                }
            }
            else if (job->commands[i].input_file != NULL) {
                if (redirect_input(job->commands[i].input_file) < 0) {
                    exit(1);
                }
            }

            if (i < num_commands - 1) {
                if (dup2(pipes[i][1], 1) < 0) {
                    perror("dup2 stdout");
                    exit(1);
                }
            }
            else if (job->commands[i].output_file != NULL) {
                if (redirect_output(job->commands[i].output_file) < 0) {
                    exit(1);
                }
            }

            for (int j = 0; j < num_commands - 1; j++) {
                close(pipes[j][0]);
                close(pipes[j][1]);
            }

            const char *name = job->commands[i].args[0];
            if (strcmp(name, "cd") == 0) {
                int r = builtin_cd(&job->commands[i]);
                _exit(r == 0 ? 0 : 1);
            } else if (strcmp(name, "pwd") == 0) {
                int r = builtin_pwd(&job->commands[i]);
                _exit(r == 0 ? 0 : 1);
            } else if (strcmp(name, "which") == 0) {
                int r = builtin_which(&job->commands[i]);
                _exit(r == 0 ? 0 : 1);
            } else if (strcmp(name, "exit") == 0) {
                _exit(0);
            } else if (strcmp(name, "die") == 0) {
                (void)builtin_die(&job->commands[i]);
                _exit(1);
            }

            char *path = resolve_program(job->commands[i].args[0]);
            if (path == NULL) {
                fprintf(stderr, "%s: command not found\n", job->commands[i].args[0]);
                _exit(1);
            }

            execv(path, job->commands[i].args);
            perror("execv");
            free(path);
            _exit(1);
        }
        else {
            pids[i] = pid;
        }
    }
    for (int i = 0; i < num_commands - 1; i++) {
        close(pipes[i][0]);
        close(pipes[i][1]);
    }

    int last_status = 0;
    for (int i = 0; i < num_commands; i++) {
        int status;
        waitpid(pids[i], &status, 0);
        if (i == num_commands - 1) {
            last_status = status;
        }
    }

    if (WIFEXITED(last_status)) {
        return WEXITSTATUS(last_status);
    }
    return -1;
}



void free_job(Job *job) {
    if (!job || job->commands == NULL) return;
    for (int i = 0; i < job->command_count; i++) {
        Command *c = &job->commands[i];
        if (c->args) {
            for (int j = 0; j < c->arg_count; j++) free(c->args[j]);
            free(c->args);
        }
        if (c->input_file) free(c->input_file);
        if (c->output_file) free(c->output_file);
    }
    free(job->commands);
    job->commands = NULL;
    job->command_count = 0;
}

int execute_job(Job *job, int last_exit_status) {
    if (job->conditional == 1) {  // "and"
        if (last_exit_status != 0) {
            return last_exit_status;
        }
    } else if (job->conditional == -1) {  // "or"
        if (last_exit_status == 0) {
            return last_exit_status;
        }
    }

    if (job->command_count == 1 && job->commands[0].arg_count > 0) {
        const char *cmd_name = job->commands[0].args[0];
        if (strcmp(cmd_name, "cd") == 0) {
            return builtin_cd(&job->commands[0]);
        } else if (strcmp(cmd_name, "exit") == 0) {
            shell_terminate = 1;
            shell_exit_code = EXIT_SUCCESS;
            return 0;
        } else if (strcmp(cmd_name, "die") == 0) {
            int r = builtin_die(&job->commands[0]);
            shell_terminate = 2;
            shell_exit_code = EXIT_FAILURE;
            return r;
        }
    }

    int exit_status = execute_pipeline(job);

    for (int i = 0; i < job->command_count; i++) {
        if (job->commands[i].arg_count > 0) {
            const char *nm = job->commands[i].args[0];
            if (strcmp(nm, "exit") == 0) {
                shell_terminate = 1;
                shell_exit_code = EXIT_SUCCESS;
            } else if (strcmp(nm, "die") == 0) {
                shell_terminate = 2;
                shell_exit_code = EXIT_FAILURE;
            }
        }
    }

    return exit_status;
}


void process_token(char **tokens, int token_count) {
    if (token_count == 0) return;

    int idx = 0;
    Job job;
    job.command_count = 0;
    job.conditional = 0;
    job.commands = NULL;

    if (idx < token_count) {
        if (strcmp(tokens[idx], "and") == 0) {
            job.conditional = 1;
            idx++;
        } else if (strcmp(tokens[idx], "or") == 0) {
            job.conditional = -1;
            idx++;
        }
    }

    if (idx >= token_count) {
        fprintf(stderr, "syntax error\n");
        return;
    }

    job.commands = malloc(sizeof(Command) * (token_count + 1));
    if (!job.commands) {
        perror("malloc");
        return;
    }

    int cmd_i = -1;
    char **arglist = NULL;
    int argcap = 0, arglen = 0;
    int saw_pipe = 0;
    char *cur_input = NULL;
    char *cur_output = NULL;

    while (idx < token_count) {
        char *t = tokens[idx];

        if (strcmp(t, "|") == 0) {
            if (arglen == 0) goto parse_error;
            arglist = realloc(arglist, sizeof(char*) * (arglen + 1));
            arglist[arglen] = NULL;
            cmd_i++;
            job.commands[cmd_i].args = arglist;
            job.commands[cmd_i].arg_count = arglen;
            job.commands[cmd_i].input_file = cur_input;
            job.commands[cmd_i].output_file = cur_output;
            cur_input = NULL; cur_output = NULL;
            arglist = NULL; argcap = 0; arglen = 0;
            saw_pipe = 1;
            idx++;
            continue;
        }

        if (strcmp(t, "<") == 0) {
            if (idx + 1 >= token_count) goto parse_error;
            if (saw_pipe) goto parse_error;
            if (cur_input) goto parse_error; /* duplicate */
            cur_input = strdup(tokens[idx + 1]);
            idx += 2;
            continue;
        }
        if (strcmp(t, ">") == 0) {
            if (idx + 1 >= token_count) goto parse_error;
            if (saw_pipe) goto parse_error;
            if (cur_output) goto parse_error;
            cur_output = strdup(tokens[idx + 1]);
            idx += 2;
            continue;
        }

        if (arglen + 1 >= argcap) {
            argcap = argcap ? argcap * 2 : 8;
            char **na = realloc(arglist, sizeof(char*) * argcap);
            if (!na) goto parse_error;
            arglist = na;
        }
        arglist[arglen++] = strdup(t);
        idx++;
    }

    if (arglen == 0) goto parse_error;
    arglist = realloc(arglist, sizeof(char*) * (arglen + 1));
    arglist[arglen] = NULL;
    cmd_i++;
    job.commands[cmd_i].args = arglist;
    job.commands[cmd_i].arg_count = arglen;
    job.commands[cmd_i].input_file = cur_input;
    job.commands[cmd_i].output_file = cur_output;
    cur_input = NULL; cur_output = NULL;
    job.command_count = cmd_i + 1;

    int status = execute_job(&job, last_exit_status);
    last_exit_status = status == 0 ? 0 : status;

    free_job(&job);
    return;

parse_error:
    if (arglist) {
        for (int i = 0; i < arglen; i++) free(arglist[i]);
        free(arglist);
    }
    if (cur_input) free(cur_input);
    if (cur_output) free(cur_output);
    if (job.commands) free(job.commands);
    fprintf(stderr, "syntax error\n");
    return;
}

int read_input(FILE *stream, char **line_buffer_ptr, size_t *buffer_size_ptr) {
    (void)buffer_size_ptr;
    int fd = fileno(stream);
    size_t cap = 128;
    size_t len = 0;
    char *buf = malloc(cap);
    if (buf == NULL) {
        perror("malloc");
        return -1;
    }

    while (1) {
        char c;
        ssize_t r = read(fd, &c, 1);
        if (r == 0) { /* EOF */
            if (len == 0) {
                free(buf);
                *line_buffer_ptr = NULL;
                return 0;
            }
            break;
        }
        if (r < 0) {
            if (errno == EINTR) continue;
            perror("read");
            free(buf);
            *line_buffer_ptr = NULL;
            return -1;
        }

        buf[len++] = c;
        if (len + 1 >= cap) {
            cap *= 2;
            char *nb = realloc(buf, cap);
            if (nb == NULL) {
                perror("realloc");
                free(buf);
                *line_buffer_ptr = NULL;
                return -1;
            }
            buf = nb;
        }

        if (c == '\n') break;
    }

    buf[len] = '\0';
    *line_buffer_ptr = buf;
    return 1;
}

int run_shell(FILE *stream, int i_mode) {
    char *line_buffer = NULL; 
    size_t buffer_size = 0; 
    int read_status;
    shell_interactive = i_mode;
    if (i_mode){
        write(1, "Welcome to my shell!\n", sizeof("Welcome to my shell!\n") - 1);
    }

    while (1) {
        if (i_mode){
            write(1, "mysh> ", sizeof("mysh> ") - 1);
        }
        read_status = read_input(stream, &line_buffer, &buffer_size); 
        if (read_status <= 0) {
            break; 
        }

        int token_count;
        char **tokens = tokenize(line_buffer, &token_count);

        if (token_count > 0){
            process_token(tokens, token_count);
        }
        free_token(tokens, token_count);

        if (shell_terminate) {
            break;
        }
    }

    if (i_mode){
        write(1, "exiting.\n", sizeof("exiting.\n") - 1);
    }
    if (line_buffer != NULL){
        free(line_buffer);
    }
    if (shell_terminate == 2) {
        return shell_exit_code;
    }
    return EXIT_SUCCESS;
}

int main(int argc, char *argv[]) {
    FILE *stream = stdin;
    int i_mode;

    if (argc > 2) {
        write(2, "Correct usage: ./mysh [batch_file]\n", 27);
        return 1;
    }
    if (argc == 2) {
        stream = fopen(argv[1], "r");
        if (stream == NULL) {
            perror("fopen");
            return 1;
        }
    }

    i_mode = isatty(fileno(stream));
    int rc = run_shell(stream, i_mode);

    if (argc == 2){
        fclose(stream);
    }

    if (rc != 0) return rc;
    return EXIT_SUCCESS;
}
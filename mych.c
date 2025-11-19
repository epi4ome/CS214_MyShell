#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <errno.h>

#define CAP 8

typedef enum {
    INTERACTIVE,
    BATCH 
} Mode;

static int sh_term = 0;
static int last_stat = 0;

static int is_whitespace(char c) {
    return (c == ' ' || c == '\t' || c == '\n');
}

static int is_word(char c) {
    return !(c == '\0' || c == '\n' || c == '#' || is_whitespace(c) ||
             c == '<' || c == '>' || c == '|');
}

char **tokenize(const char *line, size_t *token_count) {
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
        if (*p == '\0' || *p == '\n') break;
        if (*p == '#') break;

        if (is_whitespace(*p)) { p++; continue; }

        if (*p == '<' || *p == '>' || *p == '|') {
            if (*token_count + 1 >= cap) {
                cap *= 2;
                char **nt = realloc(tokens, cap * sizeof(char*));
                if (!nt) { perror("realloc"); break; }
                tokens = nt;
            }
            char *t = malloc(2);
            if (!t) { perror("malloc"); break; }
            t[0] = *p;
            t[1] = '\0';
            tokens[(*token_count)++] = t;
            p++;
            continue;
        }

        const char *start = p;
        while (is_word(*p)) p++;
        size_t len = p - start;
        if (*token_count + 1 >= cap) {
            cap *= 2;
            char **nt = realloc(tokens, cap * sizeof(char*));
            if (!nt) { perror("realloc"); break; }
            tokens = nt;
        }
        char *word = malloc(len + 1);
        if (!word) { 
            perror("malloc"); 
            break; 
        }
        memcpy(word, start, len);  
        word[len] = '\0';        
        tokens[(*token_count)++] = word;
    }

    if (*token_count + 1 >= cap) {
        char **nt = realloc(tokens, (*token_count + 1) * sizeof(char*));
        if (!nt) { perror("realloc"); free(tokens); *token_count = 0; return NULL; }
        tokens = nt;
    }
    tokens[*token_count] = NULL;
    return tokens;
}

void free_tokens(char **tokens, size_t count) {
    for (size_t i = 0; i < count; i++) {
        if (tokens[i] != NULL) free(tokens[i]);
    }
    free(tokens);
}

typedef struct {
    char **args;
    size_t arg_count; 
    char *input_file;
    char *output_file;
} Command;

typedef struct {
    Command *commands;
    size_t command_count; 
    int conditional;
} Job;

char *resolve(const char *name) {
    if (strchr(name, '/') != NULL) {
        if (access(name, X_OK) == 0) return strdup(name);
        return NULL;
    }

    if (strcmp(name, "cd") == 0 || strcmp(name, "pwd") == 0 ||
        strcmp(name, "which") == 0 || strcmp(name, "exit") == 0 ||
        strcmp(name, "die") == 0) {
        return NULL;
    }

    const char *search_dirs[] = {"/usr/local/bin", "/usr/bin", "/bin"};
    for (int i = 0; i < 3; i++) {
        size_t path_len = strlen(search_dirs[i]) + 1 + strlen(name) + 1;
        char *full_path = malloc(path_len);
        if (!full_path) continue;
        snprintf(full_path, path_len, "%s/%s", search_dirs[i], name);
        if (access(full_path, X_OK) == 0) return full_path;
        free(full_path);
    }

    return NULL;
}

int builtin_cd(Command *cmd) {
    if (cmd->arg_count != 2) {
        fprintf(stderr, "cd: missing argument\n");
        return 1;
    }
    if (chdir(cmd->args[1]) != 0) {
        fprintf(stderr, "cd: %s: %s\n", cmd->args[1], strerror(errno));
        return 1;
    }
    return 0;
}

int b_pwd(Command *cmd) {
    (void)cmd;
    size_t cap = 128;
    char *cwd = NULL;

    while (1) {
        char *tmp = realloc(cwd, cap);
        if (!tmp) {
            perror("realloc");
            free(cwd);
            return 1;
        }
        cwd = tmp;

        if (getcwd(cwd, cap)) {
            printf("%s\n", cwd);
            free(cwd);
            return 0;
        }

        if (errno != ERANGE) {
            perror("getcwd");
            free(cwd);
            return 1;
        }
        cap *= 2;
    }
}

int b_which(Command *cmd) {
    if (cmd->arg_count != 2) {
        fprintf(stderr, "which: missing argument\n");
        return 1;
    }
    const char *name = cmd->args[1];

    if (strcmp(name, "cd") == 0 || strcmp(name, "pwd") == 0 ||
        strcmp(name, "which") == 0 || strcmp(name, "exit") == 0 ||
        strcmp(name, "die") == 0) {
        return 1;
    }

    char *path = resolve(name);
    if (!path) {
        return 1;
    }
    printf("%s\n", path);
    free(path);
    return 0;
}

int b_exit(Command *cmd) {
    (void)cmd;
    sh_term = 1;
    return 0;
}

int b_die(Command *cmd) {
    for (size_t i = 1; i < cmd->arg_count; i++) { // Changed i from int to size_t
        if (i > 1) printf(" ");
        printf("%s", cmd->args[i]);
    }
    if (cmd->arg_count > 1) printf("\n");
    sh_term = 2;
    return 1;
}

int execute_builtin_in_child(Command *cmd) {
    if (cmd->arg_count == 0) return 0;

    const char *cmd_name = cmd->args[0];
    int result = 1;

    if (strcmp(cmd_name, "exit") == 0) {
        result = b_exit(cmd);
    } else if (strcmp(cmd_name, "die") == 0) {
        result = b_die(cmd);
    } else if (strcmp(cmd_name, "cd") == 0) {
        fprintf(stderr, "cd cannot participate in pipelines/redirection as the first command.\n");
        result = 1;
    } else if (strcmp(cmd_name, "pwd") == 0) {
        result = b_pwd(cmd);
    } else if (strcmp(cmd_name, "which") == 0) {
        result = b_which(cmd);
    } else {
        result = 1;
    }
    return result;
}

int redirect_input(const char *filename) {
    int fd = open(filename, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "%s: %s\n", filename, strerror(errno));
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
        fprintf(stderr, "%s: %s\n", filename, strerror(errno));
        return -1;
    }
    if (dup2(fd, STDOUT_FILENO) < 0) {
        perror("dup2 output");
        close(fd);
        return -1;
    }
    close(fd);
    return 0;
}

int execute_pipeline(Job *job, Mode mode) {
    size_t num_commands = job->command_count; // Changed from int to size_t
    pid_t pids[num_commands];
    int pipes[num_commands - 1][2];
    int last_exit_status = 0;

    Command *first_cmd = &job->commands[0];
    
    if (num_commands == 1) {
        if (strcmp(first_cmd->args[0], "cd") == 0) {
            return builtin_cd(first_cmd);
        } else if (strcmp(first_cmd->args[0], "exit") == 0) {
            return b_exit(first_cmd);
        } else if (strcmp(first_cmd->args[0], "die") == 0) {
            return b_die(first_cmd);
        }
    }

    for (size_t i = 0; i < num_commands - 1; i++) { // Changed i from int to size_t
        if (pipe(pipes[i]) < 0) {
            perror("pipe");
            return 1;
        }
    }

    for (size_t i = 0; i < num_commands; i++) { // Changed i from int to size_t
        Command *current_cmd = &job->commands[i];
        if (current_cmd->arg_count == 0) return 1;

        const char *cmd_name = current_cmd->args[0];
        pids[i] = fork();

        if (pids[i] < 0) {
            perror("fork");
            for (size_t j = 0; j < num_commands - 1; j++) { close(pipes[j][0]); close(pipes[j][1]); } // Changed j from int to size_t
            for (size_t j = 0; j < i; j++) { waitpid(pids[j], NULL, 0); } // Changed j from int to size_t
            return 1;
        }

        if (pids[i] == 0) { 
            if (i > 0) {
                if (dup2(pipes[i - 1][0], STDIN_FILENO) < 0) { perror("dup2 pipe in"); exit(1); }
            } else if (current_cmd->input_file) {
                if (redirect_input(current_cmd->input_file) < 0) exit(1);
            } else if (mode == BATCH) {
                int nd = open("/dev/null", O_RDONLY);
                if (nd >= 0) { dup2(nd, STDIN_FILENO); close(nd); }
            }

            if (i < num_commands - 1) {
                if (dup2(pipes[i][1], STDOUT_FILENO) < 0) { perror("dup2 pipe out"); exit(1); }
            } else if (current_cmd->output_file) {
                if (redirect_output(current_cmd->output_file) < 0) exit(1);
            }

            for (size_t j = 0; j < num_commands - 1; j++) { close(pipes[j][0]); close(pipes[j][1]); } // Changed j from int to size_t

            int is_builtin = (strcmp(cmd_name, "pwd") == 0 || strcmp(cmd_name, "which") == 0 ||
                              strcmp(cmd_name, "exit") == 0 || strcmp(cmd_name, "die") == 0);

            if (is_builtin) {
                int b_status = execute_builtin_in_child(current_cmd);
                exit(b_status);
            } else {
                char *path = resolve(cmd_name);
                if (!path) {
                    fprintf(stderr, "%s: command not found\n", cmd_name);
                    exit(127);
                }
                execv(path, current_cmd->args);
                perror("execv");
                free(path);
                exit(1);
            }
        }
    } 

    for (size_t i = 0; i < num_commands - 1; i++) { // Changed i from int to size_t
        close(pipes[i][0]);
        close(pipes[i][1]);
    }

    for (size_t i = 0; i < num_commands; i++) { // Changed i from int to size_t
        int status;
        waitpid(pids[i], &status, 0);
        if (i == num_commands - 1) last_exit_status = status; 
    }

    if (WIFEXITED(last_exit_status)) {
        return WEXITSTATUS(last_exit_status);
    } else if (WIFSIGNALED(last_exit_status)) {
        return 128 + WTERMSIG(last_exit_status);
    } else {
        return 1;
    }
}

void free_job(Job *job){
    if (!job || !job->commands) return;
    for (size_t i = 0; i < job->command_count; i++) { // Changed i from int to size_t
        Command *c = &job->commands[i];
        if (c->args) {
            free(c->args);
        }
        if (c->input_file) free(c->input_file);
        if (c->output_file) free(c->output_file);
    }
    free(job->commands); job->commands = NULL; job->command_count = 0;
}

int execute_job(Job *job, Mode mode) {
    if (job->conditional == 1 && last_stat != 0) return last_stat;
    if (job->conditional == -1 && last_stat == 0) return last_stat;

    if (job->command_count == 0 || job->commands[0].arg_count == 0) return 0;

    last_stat = execute_pipeline(job, mode); 
    return last_stat;
}

void process_tokens(char **tokens, size_t token_count, Mode mode) {
    if (token_count == 0) return;

    size_t idx = 0; // Changed from int to size_t
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

    if (idx >= token_count) return;

    job.commands = calloc(token_count / 2 + 1, sizeof(Command));
    if (!job.commands) { perror("malloc"); return; }

    ssize_t cmd_i = -1; // Changed from int to ssize_t (retaining -1 start)
    char **arglist = NULL;
    size_t argcap = 0, arglen = 0; // Changed from int to size_t
    char *cur_input = NULL, *cur_output = NULL;
    int syntax_error = 0;

    while (idx < token_count && !syntax_error) {
        char *t = tokens[idx];

        if (strcmp(t, "|") == 0) {
            if (arglen == 0 || cur_input || cur_output) { syntax_error = 1; break; }

            arglist = realloc(arglist, sizeof(char*) * (arglen + 1));
            arglist[arglen] = NULL;

            cmd_i++;
            job.commands[cmd_i].args = arglist;
            job.commands[cmd_i].arg_count = arglen;
            job.commands[cmd_i].input_file = cur_input;
            job.commands[cmd_i].output_file = cur_output;

            arglist = NULL; argcap = arglen = 0;
            cur_input = cur_output = NULL;
            idx++;
            continue;
        }

        if (strcmp(t, "<") == 0) {
            if (idx + 1 >= token_count || cur_input || strcmp(tokens[idx + 1], "|") == 0) {
                syntax_error = 1; break;
            }
            cur_input = tokens[idx + 1];
            tokens[idx + 1] = NULL;
            idx += 2;
            continue;
        }

        if (strcmp(t, ">") == 0) {
            if (idx + 1 >= token_count || cur_output || strcmp(tokens[idx + 1], "|") == 0) {
                syntax_error = 1; break;
            }
            cur_output = tokens[idx + 1];
            tokens[idx + 1] = NULL;
            idx += 2;
            continue;
        }

        if (arglen + 1 >= argcap) {
            if (argcap == 0) argcap = CAP;
            else argcap *= 2;
            char **na = realloc(arglist, sizeof(char*) * argcap);
            if (!na) { syntax_error = 1; break; }
            arglist = na;
        }

        arglist[arglen++] = tokens[idx];
        tokens[idx] = NULL; 
        idx++;
    }

    if (!syntax_error && arglen > 0) {
        arglist = realloc(arglist, sizeof(char*) * (arglen + 1));
        arglist[arglen] = NULL;

        cmd_i++;
        job.commands[cmd_i].args = arglist;
        job.commands[cmd_i].arg_count = arglen;
        job.commands[cmd_i].input_file = cur_input;
        job.commands[cmd_i].output_file = cur_output;
        job.command_count = (size_t)(cmd_i + 1); // Cast to size_t for consistency

        execute_job(&job, mode);

    } else {
        fprintf(stderr, "syntax error\n");
        last_stat = 1;

        if (arglist) free(arglist);
        if (cur_input) free(cur_input);
        if (cur_output) free(cur_output);
    }
    if (job.commands) free_job(&job);
}

char *read_line(int fd) {
    size_t size = 128, len = 0;
    char *buf = malloc(size);
    if (!buf) return NULL;

    while (1) {
        char c;
        ssize_t n = read(fd, &c, 1);

        if (n == 0) break;
        if (n < 0) {
            if (errno == EINTR) continue;
            free(buf);
            return NULL;
        }

        if (c == '\n') {
            buf[len] = '\0';
            return buf;
        }

        buf[len++] = c;
        if (len >= size - 1) {
            size *= 2;
            char *nb = realloc(buf, size);
            if (!nb) {
                free(buf);
                return NULL;
            }
            buf = nb;
        }
    }

    if (len == 0 && size > 0) {
        free(buf);
        return NULL;
    }

    buf[len] = '\0';
    return buf;
}

void run_shell(int fd, Mode mode) {
    if (mode == INTERACTIVE) {
        printf("Welcome to my shell!\n");
    }

    while (1) {
        if (sh_term) break;
        if (mode == INTERACTIVE) {
            if (write(STDOUT_FILENO, "mysh> ", 6) != 6) {
                perror("write prompt");
                break;
            }
        }

        char *line = read_line(fd);
        if (!line) break;

        size_t token_count = 0; // Changed from int to size_t
        char **tokens = tokenize(line, &token_count);
        free(line);

        if (token_count > 0) process_tokens(tokens, token_count, mode);
        free_tokens(tokens, token_count);
    }

    if (mode == INTERACTIVE && sh_term == 1) {
        printf("Exiting my shell.\n");
    }
}

int main(int argc, char *argv[]) {
    int fd;
    if (argc > 2) {
        write(STDERR_FILENO, "Usage: ./mysh [batch_file]\n", 28);
        return EXIT_FAILURE;
    }

    if (argc == 2) {
        fd = open(argv[1], O_RDONLY);
        if (fd < 0) {
            perror("open");
            return EXIT_FAILURE;
        }
    } else {
        fd = STDIN_FILENO;
    }

    Mode mode;
    if (isatty(fd)) {
        mode = INTERACTIVE;
    } else {
        mode = BATCH;
    }

    run_shell(fd, mode);

    if (argc == 2) close(fd);
    if (sh_term == 2) return EXIT_FAILURE;
    return EXIT_SUCCESS;
}
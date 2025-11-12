#include <stdio.h>      
#include <stdlib.h>     
#include <string.h>     
#include <unistd.h>     
#include <sys/types.h>  
#include <sys/wait.h>   
#include <fcntl.h>      
#include <errno.h> 

#define TOKEN 128

int is_space(char c) {
    return (c == ' ' || c == '\t' || c == '\n');
}

static int is_word(char c) {
    return !(c == '\0' || c == '\n' || c == '#' || is_space(c) || 
             c == '<' || c == '>' || c == '|');
}

char** tokenize(const char *line, int *token_count) {
    char **tokens = malloc(TOKEN * sizeof(char *));
    *token_count = 0;
    const char *p = line;

    while (1) {
        if (*p == '\0' || *p == '\n' || *p == '#') break;

        if (is_space(*p)) {
            p++;
            continue;
        }

        if (*p == '<' || *p == '>' || *p == '|') {
            char *t = malloc(2);
            t[0] = *p;
            t[1] = '\0';
            tokens[(*token_count)++] = t;
            p++;
            continue;
        }

        const char *start = p;
        while (is_word(*p)) {
            p++;
        }

        size_t len = p - start;
        char *word = malloc(len + 1);
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

void process_token(char **tokens, int token_count) {
    for (int i = 0; i < token_count; i++) {
        printf("[%s]\n", tokens[i]);
    }
}

int read_input(FILE *stream, char **line_buffer_ptr, size_t *buffer_size_ptr) {
    ssize_t nread;
    nread = getline(line_buffer_ptr, buffer_size_ptr, stream); 

    if (nread == -1) {
        if (feof(stream)) { 
            return 0; 
        } else {
            perror("getline");
            return -1; 
        }
    }
    if (*line_buffer_ptr == NULL) {
        return 1; 
    }
    return 1; 
}

void run_shell(FILE *stream, int i_mode) {
    char *line_buffer = NULL; 
    size_t buffer_size = 0; 
    int read_status;

    if (i_mode){
        write(1, "Welcome to my shell!\n", 21);
    }

    while (1) {
        if (i_mode){
            write(1, "mysh> ", 6);
        }
        read_status = read_input(stream, &line_buffer, &buffer_size); 
        if (read_status <= 0) {
            break; 
        }

        int token_count;
        char **tokens = tokenize(line_buffer, &token_count);

        if (token_count > 0){
            process_tokens(tokens, token_count);
        }
        free_tokens(tokens, token_count);
    }

    if (i_mode){
        write(1, "Exiting my shell.\n", 18);
    }
    if (line_buffer != NULL){
        free(line_buffer);
    }
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
    run_shell(stream, i_mode);

    if (argc == 2){
        fclose(stream);
    }

    return EXIT_SUCCESS;
}
all: mysh

mysh: mysh.c
	gcc -Wall -Wextra -g -o mysh mysh.c

clean:
	rm -f mysh

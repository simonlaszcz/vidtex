vidtex: main.c decoder.h decoder.c bedstead.h bedstead.c
	gcc -Wall -Werror -lncursesw -g -o vidtex main.c decoder.c bedstead.c

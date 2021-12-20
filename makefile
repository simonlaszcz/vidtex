SHELL=/bin/bash
CC=gcc
HEADERS=decoder.h bedstead.h telesoft.h
SRC=main.c decoder.c bedstead.c telesoft.c
FLAGS=-g

dev: vidtex tags

vidtex: $(HEADERS) $(SRC)
	$(CC) $(FLAGS) -Wall -Werror -lncursesw -o vidtex $(SRC)

tags: $(HEADERS) $(SRC)
	ctags $(HEADERS) $(SRC)

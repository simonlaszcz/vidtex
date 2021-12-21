SHELL=/bin/bash
CC=gcc
HEADERS=decoder.h bedstead.h galax.h telesoft.h
SRC=main.c decoder.c bedstead.c galax.c telesoft.c
FLAGS=-g

install: vidtex

dev: vidtex tags

vidtex: $(HEADERS) $(SRC)
	$(CC) $(FLAGS) -Wall -Werror -lncursesw -o vidtex $(SRC)

tags: $(HEADERS) $(SRC)
	ctags $(HEADERS) $(SRC)

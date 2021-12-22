SHELL=/bin/bash
CC=gcc
HEADERS=decoder.h bedstead.h galax.h telesoft.h
SRC=main.c decoder.c bedstead.c galax.c telesoft.c
FLAGS=-g
TARGET=vidtex

install: vidtex

dev: vidtex tags

vidtex: $(HEADERS) $(SRC)
	$(CC) $(FLAGS) -Wall -Werror -lncursesw -o $(TARGET) $(SRC)

tags: $(HEADERS) $(SRC)
	ctags $(HEADERS) $(SRC)

man: $(TARGET).1
	mkdir -p /usr/local/share/man/man2
	cp $(TARGET).1 /usr/local/share/man/man1
	gzip /usr/local/share/man/man1/$(TARGET).1
	mandb

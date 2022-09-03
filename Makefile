CC		= gcc
CFLAGS	= -Wall -Wextra -pedantic -std=c99
DEST	= /usr/local/bin
SOURCE	= kilo.c
PROGRAM	= kilo

kilo: $(SOURCE)
	$(CC) $(SOURCE) -o $(PROGRAM) $(CFLAGS)

install: $(PROGRAM)
	install -s $(PROGRAM) $(DEST)
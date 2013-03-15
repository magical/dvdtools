
all: lsdvd

lsdvd: lsdvd.c Makefile
	$(CC) -o lsdvd lsdvd.c -ldvdread -std=c99 -pedantic -Wall -Wextra -Wconversion -Wshadow -Wno-missing-field-initializers


CFLAGS=-std=c99 -pedantic -Wall -Wextra -Wconversion -Wshadow -Wno-missing-field-initializers

all: lsdvd catdvd dvdbreakpoints
test: bitreader_test
	./bitreader_test
lsdvd: lsdvd.c bitreader.c bitreader.h uint.h Makefile
	$(CC) $(CFLAGS) -o lsdvd lsdvd.c bitreader.c -ldvdread
catdvd: catdvd.c Makefile
	$(CC) $(CFLAGS) -o catdvd catdvd.c -ldvdread -ldvdcss
dvdbreakpoints: dvdbreakpoints.c bitreader.c bitreader.h uint.h Makefile
	$(CC) -std=c99 -O -Wall -Wconversion -Wshadow -Wno-unused -o $@ $< bitreader.c -ldvdread
bitreader_test: bitreader_test.c bitreader.c bitreader.h uint.h Makefile
	$(CC) $(CFLAGS) -o bitreader_test bitreader_test.c bitreader.c

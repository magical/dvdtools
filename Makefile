
CFLAGS=-std=c99 -pedantic -Wall -Wextra -Wconversion -Wshadow -Wno-missing-field-initializers

all: lsdvd catdvd layers dvdbreakpoints extractaudio
test: bitreader_test
	./bitreader_test

lsdvd: lsdvd.c bitreader.c bitreader.h uint.h Makefile
	$(CC) $(CFLAGS) -o lsdvd lsdvd.c bitreader.c -ldvdread
catdvd: catdvd.c Makefile
	$(CC) $(CFLAGS) -o catdvd catdvd.c -ldvdread -ldvdcss
layers: layers.c Makefile
	$(CC) $(CFLAGS) -o $@ $<
dvdbreakpoints: dvdbreakpoints.c bitreader.c bitreader.h uint.h Makefile
	$(CC) -std=c99 -O -Wall -Wconversion -Wshadow -Wno-unused -o $@ $< bitreader.c -ldvdread
extractaudio: extractaudio.c bitreader.c bitreader.h uint.h Makefile
	$(CC) $(CFLAGS) -o $@ $< bitreader.c -ldvdread

bitreader_test: bitreader_test.c bitreader.c bitreader.h uint.h Makefile
	$(CC) $(CFLAGS) -o bitreader_test bitreader_test.c bitreader.c

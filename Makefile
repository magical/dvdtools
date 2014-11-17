
CFLAGS=-O2 -std=c99 -pedantic -Wall -Wextra -Wconversion -Wshadow -Wno-missing-field-initializers

all: lsdvd catdvd layers dvdbreakpoints extractaudio
test: bitreader_test
	./bitreader_test

lsdvd: lsdvd.c bitreader.c bitreader.h uint.h Makefile
	$(CC) $(CFLAGS) -o lsdvd lsdvd.c bitreader.c -ldvdread
catdvd: catdvd.c Makefile
	$(CC) $(CFLAGS) -ggdb -I../libdvdcss/src -o catdvd catdvd.c -ldvdread ../libdvdcss/src/.libs/libdvdcss.a
layers: layers.c Makefile
	$(CC) $(CFLAGS) -o $@ $<
dvdbreakpoints: dvdbreakpoints.c bitreader.c bitreader.h uint.h Makefile
	$(CC) -std=c99 -O -Wall -Wconversion -Wshadow -Wno-unused -o $@ $< bitreader.c -ldvdread
extractaudio: extractaudio.c bitreader.c bitreader.h uint.h Makefile
	$(CC) $(CFLAGS) -o $@ $< bitreader.c -ldvdread
ac3strip: ac3strip.c ac3bits.c ac3tab.c ac3bits.h Makefile
	$(CC) $(CFLAGS) -o $@ ac3strip.c ac3bits.c

bitreader_test: bitreader_test.c bitreader.c bitreader.h uint.h Makefile
	$(CC) $(CFLAGS) -o bitreader_test bitreader_test.c bitreader.c

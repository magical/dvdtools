/* Big-endian bit reader.

Modeled after http://golang.org/src/pkg/compress/bzip2/bit_reader.go */

#include "bitreader.h"

#define UINT_BITS (sizeof(uint)*8)

void bitreader_init(struct bitreader *b, const u8 *buf, size_t len)
{
	b->buf = buf;
	b->len = len;
	b->pos = 0;
	b->bits = 0;
	b->count = 0;
	b->err = 0;
}

// Read and return the requested number of bits. If more bits are requested
// than fit in a uint, the earliest bits are discarded. In the event of EOF,
// b.err is set to -1 and the missing bits are filled with 0.
uint read_bits(struct bitreader *b, uint count)
{
	uint bits;
	while (b->count < count) {
		uint byte;
		if (b->pos < b->len) {
			byte = b->buf[b->pos];
			b->pos++;
		} else {
			byte = 0;
			b->err = -1;
		}
		b->bits = b->bits << 8 | byte;
		b->count += 8;
	}

	uint shift = b->count - count;
	uint mask = ~0U;
	if (count < UINT_BITS) {
		mask = (1U << count) - 1;
	}

	bits = (uint)(b->bits >> shift) & mask;
	b->count -= count;
	return bits;
}

void skip_bits(struct bitreader *b, uint count)
{
	read_bits(b, count); // lazy...
}

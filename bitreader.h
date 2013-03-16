#ifndef BITREADER_H
#define BITREADER_H

#include <stddef.h> // size_t
#include "uint.h"

struct bitreader {
	const u8 *buf;
	size_t len;
	size_t pos;
	u64 bits; // bit buffer
	uint count; // number of valid bits in buffer
	int err;
};

void bitreader_init(struct bitreader*, const u8*, size_t);
uint read_bits(struct bitreader*, uint);
void skip_bits(struct bitreader*, uint);

#endif

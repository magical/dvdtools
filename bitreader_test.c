#include "bitreader.h"
#include <assert.h>

int main(int argc, char *argv[])
{
	(void)argc;
	(void)argv;

	struct bitreader b;
	bitreader_init(&b, (const u8 *)"abcd", 4);

	assert(read_bits(&b, 8) == 'a');
	assert(read_bits(&b, 1) == 0);
	assert(read_bits(&b, 7) == 'b');
	assert(read_bits(&b, 16) == ('c' << 8 | 'd'));
	assert(b.err == 0);
	assert(read_bits(&b, 1) == 0);
	assert(b.err == -1);

	const u8 deadbeef[] = {0x0d, 0xea, 0xdb, 0xee, 0xf0};
	bitreader_init(&b, deadbeef, 5);
	skip_bits(&b, 4);
	assert(read_bits(&b, 16) == 0xdead);
	assert(read_bits(&b, 16) == 0xbeef);

	bitreader_init(&b, deadbeef, 5);
	skip_bits(&b, 4);
	assert(read_bits(&b, 32) == 0xdeadbeef);

	return 0;
}

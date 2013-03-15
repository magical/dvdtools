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

	return 0;
}

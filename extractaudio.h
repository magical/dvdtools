#include <stdio.h>
#ifdef __MINGW64__
#include <windows.h>
#endif

struct writer {
	int (*write)(struct writer* w, const unsigned char* buf, int size);
	int (*close)(struct writer* w);

	// Private fields used by various writers
	struct writer* writer;
	FILE* fp;
	int pid;
	unsigned char* buf;
	int depth;
	int channels;

	#ifdef __MINGW64__
	HANDLE hProcess;
	HANDLE hPipe;
	#endif
};

struct lpcm_info {
	int bitdepth;
	int sample_rate;
	int channels;
};


struct writer* open_flac(char *filename, struct lpcm_info info);

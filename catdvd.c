
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include "dvdcss/dvdcss.h"
#include <dvdread/dvd_reader.h>
#include <dvdread/dvd_udf.h>
#include <dvdread/ifo_read.h>

// We need to build a map of sectors in a title
// The top menu, each title menu, and each titleset have separate keys

enum {
	T_VOB,
	T_AOB,
	T_AUDIO_VOB,
};

struct vob {
	long start;
	long end;
	int type;
};
struct vob *vobs;
int voblen;
int vobcap;

uint8_t block[2048];

void add_vob(long start, long len, int type)
{
	while (voblen >= vobcap) {
		vobcap = vobcap * 2;
		if (vobcap == 0) {
			vobcap = 10;
		}
		vobs = realloc(vobs, (size_t)vobcap * sizeof(struct vob));
		if (vobs == NULL) {
			fputs("Couldn't allocate memory\n", stderr);
			exit(1);
		}
	}
	vobs[voblen].start = start;
	vobs[voblen].end = start + len - 1;
	vobs[voblen].type = type;
	voblen++;
}

int vob_cmp(const void *v0, const void *v1)
{
	const struct vob *a = v0, *b = v1;
	if (a->start < b->start)
		return -1;
	else if (a->start > b->start)
		return 1;
	return 0;
}

int load_video(dvd_reader_t *dvd) {
	ifo_handle_t *ifo0 = NULL;
	uint32_t size, start;
	int title, titlesets;

	ifo0 = ifoOpen(dvd, 0);
	if (ifo0 == NULL) {
		return 1;
	}
	titlesets = ifo0->vmgi_mat->vmg_nr_of_title_sets;
	ifoClose(ifo0);

	// Top menu
	start = UDFFindFile(dvd, "/VIDEO_TS/VIDEO_TS.VOB", &size);
	if (start == 0) {
		fprintf(stderr, "Couldn't open VIDEO_TS.VOB");
	}

	add_vob((long)start, (long)size/0x800, T_VOB);

	for (title = 1; title <= titlesets; title++) {
		char fn[] = "/VIDEO_TS/VTS_??_?.VOB";
		int64_t total_size;

		// Menu
		// note: C99 snprintf always NUL-terminates.
		snprintf(fn, sizeof(fn), "/VIDEO_TS/VTS_%02d_0.VOB", title);
		start = UDFFindFile(dvd, fn, &size);
		if (start == 0) {
			fprintf(stderr, "Couldn't find menu for titleset %d\n", title);
		} else {
			add_vob((long)start, (long)size/0x800, T_VOB);
		}

		snprintf(fn, sizeof(fn), "/VIDEO_TS/VTS_%02d_1.VOB", title);
		start = UDFFindFile(dvd, fn, &size);
		total_size = (int64_t)size;
		if (start == 0) {
			fprintf(stderr, "Couldn't find VOB for titleset %d\n", title);
		} else {
			fprintf(stderr, "%s: %ld\n", fn, (long)size);
			for (int j = 2; j < 10; j++) {
				uint32_t size1, start1;
				snprintf(fn, sizeof(fn), "/VIDEO_TS/VTS_%02d_%d.VOB", title, j);
				start1 = UDFFindFile(dvd, fn, &size1);
				if (start1 == 0) {
					break;
				}
				fprintf(stderr, "%s: %ld\n", fn, (long)size1);
				total_size += size1;
			}
			add_vob((long)start, (long)(total_size/0x800), T_VOB);
		}
	}
	return 0;
}

int load_audio(dvd_reader_t *dvd)
{
	uint32_t size, start;
	int title;

	// Top menu
	start = UDFFindFile(dvd, "/AUDIO_TS/AUDIO_TS.VOB", &size);
	if (start == 0) {
		// No audio layer.
		fprintf(stderr, "No audio layer\n");
		return 0;
	}
	add_vob((long)start, (long)size/0x800, T_AUDIO_VOB);

	start = UDFFindFile(dvd, "/AUDIO_TS/AUDIO_SV.VOB", &size);
	if (start != 0) {
		add_vob((long)start, (long)size/0x800, T_AUDIO_VOB);
	}

	for (title = 1; title <= 99; title++) {
		int64_t total_size = 0;
		char fn[] = "/AUDIO_TS/ATS_??_?.AOB";

		snprintf(fn, sizeof(fn), "/AUDIO_TS/ATS_%02d_1.AOB", title);
		start = UDFFindFile(dvd, fn, &size);
		total_size = (int64_t)size;
		if (start == 0) {
			fprintf(stderr, "Couldn't find AOB for titleset %d\n", title);
			break;
		}
		for (int j = 2; j < 10; j++) {
			uint32_t size1, start1;
			snprintf(fn, sizeof(fn), "/AUDIO_TS/ATS_%02d_%d.AOB", title, j);
			start1 = UDFFindFile(dvd, fn, &size1);
			if (start1 == 0) {
				break;
			}
			fprintf(stderr, "%s: %ld\n", fn, (long)size1);
			total_size += (int64_t)size1;
		}
		//fprintf(stderr, "size: %lld\n", total_size);
		add_vob((long)start, (long)(total_size/0x800), T_AOB);
	}
	return 0;

}

int main(int argc, char *argv[])
{
	dvd_reader_t *dvd = NULL;
	dvdcss_t dvdcss = NULL;
	char *dvd_filename = NULL;
	uint32_t mkb_start, mkb_size;
	uint8_t *mkb;
	int err, errors, flags;

	if (argc == 2) {
		dvd_filename = argv[1];
	} else if (argc == 1) {
		dvd_filename = "/dev/dvd";
	} else {
		return 1;
	}

	dvd = DVDOpen(dvd_filename);
	if (dvd == NULL) {
		return 1;
	}

	mkb_start = UDFFindFile(dvd, "/AUDIO_TS/DVDAUDIO.MKB", &mkb_size);

	load_video(dvd);
	load_audio(dvd);

	if (dvd != NULL) {
		DVDClose(dvd);
	}

	qsort(vobs, (size_t)voblen, sizeof(*vobs), vob_cmp);

	for (int i = 0; i < voblen; i++) {
		const char* type = "VOB";
		switch (vobs[i].type) {
		case T_AOB:     type = "AOB"; break;
		case T_AUDIO_VOB: type = "AUDIO_TS/VOB"; break;
		}
		fprintf(stderr, "%s: %ld,%ld\n", type, vobs[i].start, vobs[i].end);
	}

	dvdcss = dvdcss_open(dvd_filename);
	if (dvdcss == NULL) {
		fputs("Couldn't open dvd with dvdcss\n", stderr);
		return 1;
	}

	// Try to read DVD-Audio key.
	if (mkb_start) {
		mkb = malloc(mkb_size);
		err = dvdcss_seek(dvdcss, (int)mkb_start, DVDCSS_NOFLAGS);
		if (err < 0) {
			fprintf(stderr, "Seek error: %s\n", dvdcss_error(dvdcss));
			return 1;
		}
		err = dvdcss_read(dvdcss, mkb, (int)(mkb_size/0x800), DVDCSS_NOFLAGS);
		if (err < 1) {
			fprintf(stderr, "Error reading DVDAUDIO.MKB: %s\n", dvdcss_error(dvdcss));
			return 1;
		}
		err = dvdcss_init_cppm(dvdcss, mkb, mkb_size);
		if (err < 0) {
			fprintf(stderr, "Error processing DVDAUDIO.MKB: %s\n", dvdcss_error(dvdcss));
			return 1;
		}

		err = dvdcss_seek(dvdcss, 0, DVDCSS_NOFLAGS);
		if (err < 0) {
			fprintf(stderr, "Error seeking to beginning of DVD.\n");
			return 1;
		}
	}

	struct vob *vob, *lastvob;
	bool invob = false;
	int lb = 0;
	vob = &vobs[0];
	lastvob = &vobs[voblen-1];
	dvdcss_seek(dvdcss, lb, 0);
	errors = 0;
	for (; ; lb++) {
		while (vob->end < lb && vob < lastvob) {
			vob++;
		}
		invob = (vob->start <= lb && lb <= vob->end);
		if (vob->start == lb && vob->type == T_VOB) {
			err = dvdcss_seek(dvdcss, (int)lb, DVDCSS_SEEK_KEY);
			if (err < 0) {
				fprintf(stderr, "Seek error: %s\n", dvdcss_error(dvdcss));
				return 1;
			}
		}
		if (invob && vob->type == T_VOB) {
			flags = DVDCSS_READ_DECRYPT;
		} else if (invob && (vob->type == T_AOB || vob->type == T_AUDIO_VOB)) {
			flags = DVDCSS_READ_DECRYPT_CPPM;
		} else {
			flags = DVDCSS_NOFLAGS;
		}
		err = dvdcss_read(dvdcss, block, 1, flags);
		if (err < 0) {
			fprintf(stderr, "Error reading dvd: %d\n", -err);
			return 1;
		} else if (err == 0) {
			break; // EOF?
		}

		if (fwrite(block, sizeof(block), 1, stdout) != 1) {
			return 1;
		}

		// Debugging
		if (block[0] == 0 && block[1] == 0 && block[2] == 1 && block[3] == 0xba) {
			//uint8_t *b = block;
			//b = b + 14 + (b[13] & 7);
			if (!invob) {
				fprintf(stderr, "Block %d: Found MPEG data outside a VOB\n", lb);
				errors++;
			}
		} else {
			if (invob && vob->type != T_AUDIO_VOB) {
				fprintf(stderr, "Block %d: Found non-MPEG data inside a VOB\n", lb);
				errors++;
			}
		}

		if (errors > 30) {
			fprintf(stderr, "Too many errors.\n");
			return 1;
		}
	}

	return 0;
}

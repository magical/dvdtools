
#include <stdlib.h>
#include <stdio.h>
#include <dvdcss/dvdcss.h>
#include <dvdread/dvd_reader.h>
#include <dvdread/dvd_udf.h>
#include <dvdread/ifo_read.h>

// We need to build a map of sectors in a title
// The top menu, each title menu, and each titleset have separate keys

struct vob {
	size_t start;
	size_t end;
};
struct vob *vobs;
size_t voblen;
size_t vobcap;

uint8_t block[2048];

void add_vob(size_t start, size_t end)
{
	while (voblen >= vobcap) {
		vobcap = vobcap * 2;
		if (vobcap == 0) {
			vobcap = 10;
		}
		vobs = realloc(vobs, vobcap * sizeof(struct vob));
		if (vobs == NULL) {
			fputs("Couldn't allocate memory\n", stderr);
			exit(1);
		}
	}
	vobs[voblen].start = start;
	vobs[voblen].end = end;
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

int main(int argc, char *argv[])
{
	dvd_reader_t *dvd = NULL;
	ifo_handle_t *ifo0 = NULL;
	dvdcss_t dvdcss = NULL;
	char *dvd_filename = NULL;
	uint32_t size, start;
	int titlesets;

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
	ifo0 = ifoOpen(dvd, 0);
	if (ifo0 == NULL) {
		DVDClose(dvd);
		return 1;
	}

	titlesets = ifo0->vmgi_mat->vmg_nr_of_title_sets;

	ifoClose(ifo0);

	// Top menu
	start = UDFFindFile(dvd, "/VIDEO_TS/VIDEO_TS.VOB", &size);
	if (start == 0) {
		fprintf(stderr, "Couldn't open VIDEO_TS.VOB");
	}

	//add_vob(
	//	vmg_start + ifo0->vmgi_mat->vbgm_vobs,
	//	vmg_start + ifo0->vmgi_mat->vmg_last_sector - vmgi_last_sector
	//);
	add_vob(start, start + (size - 1)/0x800);

	for (int i = 0; i < titlesets; i++) {
		char fn[] = "/VIDEO_TS/VTS_??_?.VOB";

		// Menu
		// note: C99 snprintf always NUL-terminates.
		snprintf(fn, sizeof(fn), "/VIDEO_TS/VTS_%02d_0.VOB", i+1);
		start = UDFFindFile(dvd, fn, &size);
		if (start == 0) {
			fprintf(stderr, "Couldn't find menu for titleset %d\n", i+1);
		} else {
			add_vob(start, start + (size - 1)/0x800);
		}

		snprintf(fn, sizeof(fn), "/VIDEO_TS/VTS_%02d_1.VOB", i+1);
		start = UDFFindFile(dvd, fn, &size);
		if (start == 0) {
			fprintf(stderr, "Couldn't find vob for titleset %d\n", i+1);
		} else {
			for (int j = 2; j < 10; j++) {
				uint32_t size1, start1;
				snprintf(fn, sizeof(fn), "/VIDEO_TS/VTS_%02d_%d.VOB", i+1, j);
				start1 = UDFFindFile(dvd, fn, &size1);
				if (start1 == 0) {
					break;
				}
				size += size1;
			}
			add_vob(start, start + (size - 1)/0x800);
		}
	}

	if (dvd != NULL)
		DVDClose(dvd);

	qsort(vobs, voblen, sizeof(*vobs), vob_cmp);

	dvdcss = dvdcss_open(dvd_filename);
	if (dvdcss == NULL) {
		fputs("Couldn't open dvd with dvdcss\n", stderr);
		return 1;
	}

	for (size_t i = 0; i < voblen; i++) {
		fprintf(stderr, "%d,%d\n", vobs[i].start, vobs[i].end);
	}

	struct vob *vob = NULL;
	size_t vobi = 0;
	for (size_t lb = 0; ; lb++) {
		int err, flags;
		if (vob == NULL && vobi < voblen) {
			vob = &vobs[vobi];
		}
		if (vob != NULL && vob->start == lb) {
			err = dvdcss_seek(dvdcss, (int)lb, DVDCSS_SEEK_KEY);
			if (err < 0) {
				fprintf(stderr, "Seek error: %s\n", dvdcss_error(dvdcss));
				return 1;
			}
		}
		if (vob != NULL && vob->start <= lb && lb <= vob->end) {
			flags = DVDCSS_READ_DECRYPT;
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

		if (vob != NULL && vob->end <= lb) {
			vob = NULL;
			vobi += 1;
		}

		// Debugging
		if (block[0] == 0 && block[1] == 0 && block[2] == 1 && block[3] == 0xba) {
			//uint8_t *b = block;
			//b = b + 14 + (b[13] & 7);
			if (flags != DVDCSS_READ_DECRYPT) {
				fprintf(stderr, "Found MPEG data outside a VOB\n");
				return 1;
			}
		} else {
			if (flags != DVDCSS_NOFLAGS) {
				fprintf(stderr, "Found non-MPEG data inside a VOB\n");
				return 1;
			}
		}

		if (fwrite(block, sizeof(block), 1, stdout) != 1) {
			return 1;
		}
	}

	return 0;
}

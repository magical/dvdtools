#include <stdlib.h>
#include <stdio.h>
#include <dvdread/dvd_reader.h>
#include <dvdread/ifo_read.h>

typedef unsigned int uint;
typedef unsigned char u8;
typedef unsigned long long u64;

typedef u8 sectorbuf[DVD_VIDEO_LB_LEN];

void die(char *s) {
	fputs(s, stderr);
	fputc('\n', stderr);
	exit(1);
}

static const char* audio_formats[] =
	{"ac3", "0x1", "mpeg1", "mpeg2ext", "lpcm", "0x5", "dts", "0x7"};
static const int bitrates[] = {16, 20, 24, -1};
static const char* sample_rates[] = {"48Hz", "96Hz"};
static const char* channels[] = {"mono", "2ch", "3ch", "4ch", "5ch", "5.1ch", "7ch", "8ch"};
static const int stream_ids[] = {0x80, 0, 0xc0, 0xc0, 0xa0, 0, 0x88, 0};

int print_audio(audio_attr_t *a, int n)
{
	if (a->audio_format == 4) {
		return printf("%d: %s %s %d %s %#02x\n", n,
			audio_formats[a->audio_format],
			channels[a->channels],
			bitrates[a->quantization],
			sample_rates[a->sample_frequency],
			stream_ids[a->audio_format]+n);
	} else {
		return printf("%d: %s %s %s %#02x\n", n,
			audio_formats[a->audio_format],
			channels[a->channels],
			sample_rates[a->sample_frequency],
			stream_ids[a->audio_format]+n);
	}
}

struct time {
	uint hour;
	uint min;
	uint sec;
	uint nano;
};

struct time time_from_scr(u64 scr) {
	struct time t;
	uint frac = scr % 27000000;
	uint sec = scr / 27000000;
	t.nano = frac * (1e9 / 27e6);
	t.sec = sec % 60;
	t.min = (sec / 60) % 60;
	t.hour = sec / 3600;
	return t;
}

int get_scr(dvd_reader_t *dvd, int title, int sector, u64 *scrp)
{
	dvd_file_t *vob = DVDOpenFile(dvd, title, DVD_READ_TITLE_VOBS);
	if (vob == NULL) { return -1; }
	sectorbuf b;
	if (DVDReadBlocks(vob, sector, 1, b) < 0) { goto error; }
	if (b[0] != 0 || b[1] != 0 || b[2] != 1 || b[3] != 0xba) {
		printf("get_scr: %d,%d: %02x%02x%02x%02x\n", title, sector, b[0], b[1], b[2], b[3]);
		goto error;
	}
	u64 scr = ((b[4] & 0x38) >> 3 << 30) |
	          ((b[4] & 3) << 28) |
	          (b[5] << 20) |
	          (b[6] >> 3 << 15) |
	          ((b[6] & 3) << 13) |
	          (b[7] << 5) |
	          (b[8] >> 3);
	uint scr_ext = (((uint)b[8] & 3) << 7) |
	               ((uint)b[9] >> 1);
	scr = scr * 300 + scr_ext;
	*scrp = scr;
	DVDCloseFile(vob);
	return 0;
error:
	DVDCloseFile(vob);
	return -1;
}

int pack_stream_id(sectorbuf b)
{
	// Theoretically, each pack only contains a single stream, so we
	// shouldn't have to look beyond the first packet.
	if (b[0] != 0 || b[1] != 0 || b[2] != 1 || b[3] != 0xba) {
		return -1; // not a PACK
	}
	u8 *p = b + 0xe + (b[0xd] & 7); // stuffing
	if (p[0] != 0 || p[1] != 0 || p[2] != 1) {
		return -1; // not a packet
	}
	if (p[3] == 0xbd) {
		// private stream
		return p[9+p[8]];
	}
	return p[3];
}

// Return the sector of the first audio packet for the given audio index.
// The sector argument should be the sector of a NAV packet.
int get_audio_sector(dvd_file_t *vob, int sector, int index)
{
	sectorbuf b;
	if (DVDReadBlocks(vob, sector, 1, b) < 1) { return -1; }
	if (pack_stream_id(b) != 0xbb) {
		// Not a NAV
		printf("not a nav: %#x\n", pack_stream_id(b));
		return -1;
	}
	// Read the a_async value for the requested stream.
	// Should probably just use nav_read.c.
	u8 *p = b + 0x599 + index*2;
	int n = (p[0] & 0x7f) << 8 | p[1];
	if (p[0] & 0x80) {
		return sector - n;
	}
	return sector + n;
}

#define MAX_TRIES 10

// Scan for the first packet with the given stream id, starting at sector.
// Returns -1 if the stream is not found within the maximum number of tries,
// or DVDReadBlock fails.
int find_stream(dvd_file_t *vob, int sector, int stream)
{
	sectorbuf b;
	for (int try = 0; try < MAX_TRIES; try++) {
		if (DVDReadBlocks(vob, sector+try, 1, b) < 1) return -1;
		if (pack_stream_id(b) == stream) {
			return try;
		}
	}
	return -1;
}

int get_pts(dvd_reader_t *dvd, int title, int sector, u64 *ptsp)
{
	dvd_file_t *vob = DVDOpenFile(dvd, title, DVD_READ_TITLE_VOBS);
	if (vob == NULL) { return -1; }

	sectorbuf b;
	for (int try = 0; try < 150; try++) {
		if (DVDReadBlocks(vob, sector+try, 1, b) < 1) { goto error; }
		if (b[0] != 0 || b[1] != 0 || b[2] != 1 || b[3] != 0xba) {
			printf("get_pts: %d,%d: %02x%02x%02x%02x\n", title, sector+try, b[0], b[1], b[2], b[3]);
			goto error;
		}
		if (b[14] != 0 || b[15] != 0 || b[16] != 1 || b[17] != 0xbd ||
		    (b[0x14] & 0x80) != 0x80) {
			continue;
		}
		int code = b[0x17+b[0x16]];
		if (!((0x80 <= code && code < 0x90) || (0xa0 <= code && code < 0xa8))) {
			continue;
		}
		printf("found %#02x at %d+%d\n", code, sector, try);
		u64 pts = ((u64)(b[23] & 0x0e) >> 1 << 30) |
		          ((u64)b[24] << 22) |
		          ((u64)b[25] >> 1 << 15) |
		          ((u64)b[26] << 7) |
		          ((u64)b[27] >> 1);
		*ptsp = pts;
		goto success;
	}
	goto error;

error:
	DVDCloseFile(vob);
	return -1;
success:
	DVDCloseFile(vob);
	return 0;
}


int main(int argc, char *argv[])
{
	if (argc != 2) {
		return 1;
	}
	dvd_reader_t *dvd = DVDOpen(argv[1]);
	if (dvd == NULL) { die("Unable to open dvd"); }
	ifo_handle_t *ifo0 = ifoOpen(dvd, 0);
	if (ifo0 == NULL) { die("Unable to open VIDEO_TS.IFO"); }
	int titles = ifo0->tt_srpt->nr_of_srpts;
	//int titles = ifo0->vmgi_mat->vmg_nr_of_title_sets;

	for (int i = 0; i < titles; i++) {
		int title = ifo0->tt_srpt->title[i].title_set_nr;
		int chapters = ifo0->tt_srpt->title[i].nr_of_ptts;
		printf("title %d\n", title);
		printf("chapters: %d\n", chapters);
		printf("vts_ttn %d\n", ifo0->tt_srpt->title[i].vts_ttn);
		ifo_handle_t *ifo = ifoOpen(dvd, title);
		if (ifo == NULL) {
			continue;
		}
		dvd_file_t *vob = DVDOpenFile(dvd, title, DVD_READ_TITLE_VOBS);
		if (vob == NULL) {
			ifoClose(ifo);
			continue;
		}

		vtsi_mat_t *v = ifo->vtsi_mat;
		for (int j = 0; j < v->nr_of_vts_audio_streams; j++) {
			print_audio(&v->vts_audio_attr[j], j);
		}

		u64 last_scr = 0, last_pts = 0;
		for (int j = 0; j < ifo->vts_pgcit->nr_of_pgci_srp; j++) {
			pgc_t *pgc = ifo->vts_pgcit->pgci_srp[j].pgc;
			printf("%d: programs: %d\n", j, pgc->nr_of_programs);
			for (int k = 0; k < pgc->nr_of_cells; k++) {
				cell_playback_t *pb = &pgc->cell_playback[k];
				/*printf("%d,%d: %d\n", j, k,
					1 + pb->last_sector - pb->first_sector);
				*/
				int audio_sector = get_audio_sector(vob, pb->first_sector, 0);
				if (audio_sector < 0) continue;
				u64 scr, pts0, pts;
				if (get_scr(dvd, title, pb->first_sector, &scr) >= 0 &&
				    get_pts(dvd, title, pb->first_sector, &pts0) >= 0 &&
				    get_pts(dvd, title, audio_sector, &pts) >= 0) {
					struct time t = time_from_scr(scr);
					struct time len = time_from_scr(scr - last_scr);
					printf("%d,%d: %llu ", j, k, scr);
					printf("%f ", (scr-last_scr) * 96 / 27e3);
					printf("%f ", (pts-last_pts) * 96 / 90.0);
					printf("%llu ", pts - pts0);
					printf("%u:%02u:%06.3f ", t.hour, t.min, t.sec + t.nano / 1e9);
					printf("%u:%02u:%06.3f\n", len.hour, len.min, len.sec + len.nano / 1e9);
					last_scr = scr;
					last_pts = pts;
				} else {
					//printf("%d,%d\n", j, k);
				}
			}
		}
		DVDCloseFile(vob);
		ifoClose(ifo);
	}
	ifoClose(ifo0);

	DVDClose(dvd);
}

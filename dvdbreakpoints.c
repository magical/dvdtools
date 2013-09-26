#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdbool.h>
#include <inttypes.h>
#include <string.h>
#include <getopt.h>
#include <dvdread/dvd_reader.h>
#include <dvdread/ifo_read.h>
#include "uint.h"
#include "bitreader.h"

typedef u8 sectorbuf[DVD_VIDEO_LB_LEN];

static bool streq(const char *a, const char *b) {
	return strcmp(a, b) == 0;
}

void die(char *fmt, ...) {
	va_list va;
	va_start(va, fmt);
	vfprintf(stderr, fmt, va);
	fputc('\n', stderr);
	va_end(va);
	exit(1);
}

static const char* audio_formats[] =
	{"ac3", "0x1", "mpeg1", "mpeg2ext", "lpcm", "0x5", "dts", "0x7"};
static const uint audio_sample_sizes[] = {16, 20, 24, 0};
static const uint audio_sample_rates[] = {48, 96};
static const char* sample_rates[] = {"48kHz", "96kHz"};
static const char* channels[] = {"mono", "2ch", "3ch", "4ch", "5ch", "5.1ch", "7ch", "8ch"};
static const uint stream_ids[] = {0x80, 0, 0xc0, 0xc0, 0xa0, 0, 0x88, 0};

static const int dts_channels[] = {1, 2, 2, 2, 2, 3, 3, 4, 4, 5, 6, 6, 6, 7, 8, 8};
static const int dts_source_bitdepth[] = {16, 16, 20, 20, 0, 24, 24};
static const char* dts_sample_rates[] = {
	"?", "8kHz", "16kHz", "32kHz", "?", "?", "11.025kHz", "22.050kHz",
	"44.1kHz", "?", "?", "12kHz", "24kHz", "48kHz", "96kHz", "192kHz"
};
static const int dts_target_bitrate[] = {
	32000, 56000, 64000, 96000, 112000, 128000, 192000, 224000,
	256000, 320000, 384000, 448000, 512000, 576000, 640000, 768000,
	896000, 1024000, 1152000, 1280000, 1344000, 1408000, 1411200, 1472000,
	1536000, 1920000, 2048000, 3072000, 3840000,
	1/*open*/, 2/*variable*/, 3/*lossless*/
};

static const char* ac3_sample_rates[] = {"48kHz", "44.1kHz", "32kHz", "???Hz"};
static const int ac3_bitrates[] = {
	32, 32, 40, 40, 48, 48, 56, 56, 64, 64, 70, 70, 96, 96, 112, 112,
	128, 128, 160, 160, 192, 192, 224, 224, 256, 256, 320, 320, 384, 384, 448, 448,
	512, 512, 576, 576, 640, 640
};
static const int ac3_channels[] = {2, 1, 2, 3, 3, 4, 4, 5};

int print_audio(audio_attr_t *a, int n)
{
	if (a->audio_format == 4) {
		return printf("audio %d: %s %s %d %s %#02x\n", n,
			audio_formats[a->audio_format],
			channels[a->channels],
			audio_sample_sizes[a->quantization],
			sample_rates[a->sample_frequency],
			stream_ids[a->audio_format]+(uint)n);
	} else {
		return printf("audio %d: %s %s %s %#02x\n", n,
			audio_formats[a->audio_format],
			channels[a->channels],
			sample_rates[a->sample_frequency],
			stream_ids[a->audio_format]+(uint)n);
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
	uint frac = (uint)(scr % 27000000);
	uint sec = (uint)(scr / 27000000);
	t.nano = (uint)(frac * (1e9 / 27e6));
	t.sec = sec % 60;
	t.min = (sec / 60) % 60;
	t.hour = sec / 3600;
	return t;
}

struct time time_from_pts(u64 pts) {
	return time_from_scr(pts*300);
}

// Return the stream id associated with a pack. If private is true and the
// packet is private stream 1, return the stream id of the private stream.
int pack_stream_id(sectorbuf b, bool private)
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
	if (private && p[3] == 0xbd) {
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
	if (pack_stream_id(b, true) != 0xbb) {
		// Not a NAV
		printf("not a nav: %#x\n", pack_stream_id(b, true));
		return -1;
	}
	// Read the a_async value for the requested stream.
	// Should probably just use nav_read.c.
	u8 *p = b + 0x599 + index*2;
	int n = (p[0] & 0x7f) << 8 | p[1];
	if (n == 0 || n == 0x3fff) {
		return -1;
	}
	if (p[0] & 0x80) {
		return sector - n;
	}
	return sector + n;
}

#define MAX_TRIES 150

// Scan for the first packet with the given stream id, starting at sector.
// Returns the sector number and reads the sector into b.
// Returns -1 if the stream is not found within the maximum number of tries,
// or if DVDReadBlocks fails.
int find_stream(dvd_file_t *vob, int sector, int stream, sectorbuf b)
{
	for (int try = 0; try < MAX_TRIES; try++) {
		if (DVDReadBlocks(vob, sector+try, 1, b) < 1) return -1;
		if (pack_stream_id(b, true) == stream) {
			return try;
		}
	}
	return -1;
}

// Scan for the first audio stream after sector.
int find_audio_stream(dvd_file_t *vob, int sector, sectorbuf b)
{
	int stream;
	for (int try = 0; try < MAX_TRIES; try++) {
		if (DVDReadBlocks(vob, sector+try, 1, b) < 1) return -1;
		stream = pack_stream_id(b, false);
		if (0xc0 <= stream && stream < 0xe0) {
			return sector+try;
		}
		stream = pack_stream_id(b, true);
		if ((0x80 <= stream && stream < 0x90) ||
		    (0xa0 <= stream && stream < 0xa8)) {
			return sector+try;
		}
	}
	return -1;
}

int get_scr(sectorbuf b, u64 *scrp)
{
	struct bitreader br;
	u64 scr;
	uint scr_ext;

	if (pack_stream_id(b, false) == -1) {
		return -1;
	}
	bitreader_init(&br, b+4, sizeof(sectorbuf)-4);
	skip_bits(&br, 2);
	scr = (u64)read_bits(&br, 3) << 30;
	skip_bits(&br, 1);
	scr |= (u64)read_bits(&br, 15) << 15;
	skip_bits(&br, 1);
	scr |= (u64)read_bits(&br, 15);
	skip_bits(&br, 1);
	scr_ext = read_bits(&br, 9);
	scr = scr*300 + scr_ext;
	if (br.err != 0) {
		return -1;
	}
	*scrp = scr;
	return 0;
}

/* Returns -1 if the buf is not a valid packet or if no PTS is present. */
int get_pts(sectorbuf b, u64 *ptsp)
{
	struct bitreader br;
	u64 pts;
	if (pack_stream_id(b, false) != 0xbd) {
		printf("get_pts: not a valid packet: %#x\n", pack_stream_id(b, false));
		return -1;
	}
	if ((b[0x14] & 0x80) != 0x80) {
		// no pts present
		return -1;
	}
	bitreader_init(&br, b+23, sizeof(sectorbuf)-23);
	skip_bits(&br, 4);
	pts = (u64)read_bits(&br, 3) << 30;
	skip_bits(&br, 1);
	pts |= (u64)read_bits(&br, 15) << 15;
	skip_bits(&br, 1);
	pts |= (u64)read_bits(&br, 15);
	if (br.err != 0) {
		return -1;
	}
	*ptsp = pts;
	return 0;
}

struct lpcm_info {
	int bitdepth;
	int sample_rate;
	int channels;
};

struct lpcm_info read_lpcm_header(sectorbuf b)
{
	struct lpcm_info info = {0};

	int stream = pack_stream_id(b, true);
	if (!(0xa0 <= stream && stream < 0xa8)) {
		goto error;
	}
	u8 *p = b + 0xe + (b[0xd] & 7); // points to PES
	p = p + 9 + p[8]; // points to PES payload
	p++; // points to audio substream header

	info.bitdepth = p[4] >> 6;
	info.sample_rate = (p[4] >> 4) & 3;
	info.channels = p[4] & 3;

error:
	return info;
}

struct ac3_info {
	uint sample_rate;
	uint frame_size;
	uint channels;
	uint surround;
	bool lfe;
};

struct ac3_info read_ac3_header(sectorbuf b, int *err)
{
	struct ac3_info info = {0};
	// AC3 packets do not line up with frames like DTS does, so we'll just have to hope our packet doesn't end prematurely

	int stream = pack_stream_id(b, true);
	if (!(0x80 <= stream && stream <= 0x88)) {
		goto error;
	}

	u8 *p = b + 0xe + (b[0xd] & 7); // points to PES
	u8 *end = p + 6 + (p[4]<<8 | p[5]);
	p = p + 9 + p[8]; // points to PES payload
	if (p[2] == 0 && p[3] == 0) {
		goto error;
	}
	p = p + 3 + (p[2]<<8 | p[3]); // points to "first" AC3 packet

	struct bitreader br;
	uint acmod;
	bitreader_init(&br, p, (size_t)(end - p));

	skip_bits(&br, 16); // syncword
	skip_bits(&br, 16); // crc
	info.sample_rate = read_bits(&br, 2); // sample rate
	info.frame_size = read_bits(&br, 6); // frame size
	skip_bits(&br, 5); // bsid
	skip_bits(&br, 3); // bsmod
	acmod = read_bits(&br, 3); // acmod
	info.channels = acmod;
	if ((acmod & 1) && acmod != 1) { skip_bits(&br, 2); } // cmixlev
	if (acmod & 4) { skip_bits(&br, 2); } // surmixlev
	if (acmod == 2) { skip_bits(&br, 2); } // dsurmod
	info.lfe = read_bits(&br, 2); // lfeon
	// etc

	if (br.err != 0) {
		goto error;
	}

	return info;
error:
	if (err != NULL) {
		*err = -1;
	}
	return info;
}

struct dts_info {
	int target_bitrate;
	int sample_rate;
	int channels;
	int lfe;
	int source_bitdepth;
	int frame_size;
	int ext_audio_id;
};

struct dts_info read_dts_header(sectorbuf b)
{
	struct dts_info info = {0};

	int stream = pack_stream_id(b, true);
	if (!(0x88 <= stream && stream < 0x90)) {
		goto error;
	}

	u8 *p = b + 0xe + (b[0xd] & 7); // points to PES
	p = p + 9 + p[8]; // points to PES payload
	p += 4; // points to DTS header
	// DTS packets do not cross sectors, so the header is always at the top

	// note: 750 frames per second
	info.frame_size = (p[5] & 3) << 12 | p[6] << 4 | p[7] >> 4;
	info.channels = (p[7] & 0xf) << 2 | p[8] >> 6;
	info.sample_rate = (p[8] & 0x3c) >> 2;
	info.target_bitrate = (p[8] & 3) << 3 | p[9] >> 5;
	info.ext_audio_id = p[10] >> 5;
	info.lfe = (p[10] & 6) >> 2;
	info.source_bitdepth = (p[11] & 1) << 2 | p[12] >> 6;

error:
	return info;
}

enum {
	DISPLAY_TIME,
	DISPLAY_FRAMES, // cdda mm:ss:ff
	DISPLAY_SAMPLES,
	DISPLAY_BYTES
};

enum {
	TRACK_OFFSET,
	TRACK_LENGTH
};

void usage(void) {
	printf("usage: dvdsplitpoints [-t title] [-a audio] [-d mode] [-l] [/dev/dvd]\n");
}

int main(int argc, char *argv[])
{
	char *dvd_filename = "/dev/dvd";
	uint title = 1;
	uint audio = 0;
	int display_mode = DISPLAY_TIME;
	int track_mode = TRACK_LENGTH;
	int opt;
	while ((opt = getopt(argc, argv, "t:a:d:l")) != -1)
	switch (opt) {
	case 't':
		title = (uint)atoi(optarg);
		break;
	case 'a':
		audio = (uint)atoi(optarg);
		break;
	case 'd':
		if (streq(optarg, "time")) {
			display_mode = DISPLAY_TIME;
		} else if (streq(optarg, "frames")) {
			display_mode = DISPLAY_FRAMES;
		} else if (streq(optarg, "samples")) {
			display_mode = DISPLAY_SAMPLES;
		} else if (streq(optarg, "bytes")) {
			display_mode = DISPLAY_BYTES;
		} else {
			printf("error: unknown display mode");
			usage();
			return 1;
		}
		break;
	case 'l':
		track_mode = TRACK_OFFSET;
		break;
	default:
		usage();
		return 1;
	}
	// TODO: args
	if (optind < argc) {
		dvd_filename = argv[optind];
		optind++;
	}
	if (optind < argc) {
		usage();
		return 1;
	}
	dvd_reader_t *dvd = DVDOpen(dvd_filename);
	if (dvd == NULL) { die("Couldn't open %s", dvd_filename); }
	ifo_handle_t *ifo0 = ifoOpen(dvd, 0);
	if (ifo0 == NULL) { die("Unable to open VIDEO_TS.IFO"); }

	if (!(1 <= title && title <= ifo0->tt_srpt->nr_of_srpts)) {
		die("Title out of range");
	}

	title_info_t tt = ifo0->tt_srpt->title[title-1];

	ifoClose(ifo0);

	ifo_handle_t *ifo = ifoOpen(dvd, tt.title_set_nr);
	if (ifo == NULL) {
		DVDClose(dvd);
		die("Couldn't open IFO for titleset %d", tt.title_set_nr);
	}
	dvd_file_t *vob = DVDOpenFile(dvd, tt.title_set_nr, DVD_READ_TITLE_VOBS);
	if (vob == NULL) {
		ifoClose(ifo);
		DVDClose(dvd);
		die("Couldn't open VOB for titleset %d", tt.title_set_nr);
	}

	if (!(tt.vts_ttn - 1 < ifo->vts_ptt_srpt->nr_of_srpts)) {
		die("Corrupt IFO: vts_ttn out of range");
	}
	ttu_t vtt = ifo->vts_ptt_srpt->title[tt.vts_ttn - 1];

	if (audio >= ifo->vtsi_mat->nr_of_vts_audio_streams) {
		die("Audio stream out of range");
	}
	audio_attr_t a = ifo->vtsi_mat->vts_audio_attr[audio];
	if (stream_ids[a.audio_format] == 0) {
		die("Unknown audio format");
	}
	if (a.sample_frequency == 1 && a.audio_format != 4) {
		die("Unknown sample rate");
	}
	uint stream = stream_ids[a.audio_format] + audio;
	uint sample_rate = audio_sample_rates[a.sample_frequency];
	uint sample_size = audio_sample_sizes[a.quantization];
	uint chan = a.channels + 1U;

	if (sample_size == 0) {
		die("Unknown sample size");
	}

	u64 first_pts = 0, last_pts = 0;
	for (int i = 0; i < vtt.nr_of_ptts; i++) {
		uint pgcn = vtt.ptt[i].pgcn - 1U;
		uint pgn = vtt.ptt[i].pgn - 1U;
		pgc_t *pgc = ifo->vts_pgcit->pgci_srp[pgcn].pgc;
		uint cell = pgc->program_map[pgn];
		cell_playback_t *pb = &pgc->cell_playback[cell-1U];
		sectorbuf b;
		if (1) {
			int audio_sector = get_audio_sector(vob, (int)pb->first_sector, (int)audio);
			if (audio_sector < 0) {
				die("Couldn't get audio sector");
			}
			if (DVDReadBlocks(vob, audio_sector, 1, b) < 1) {
				die("Couldn't read audio packet");
			}
		} else {
			if (find_stream(vob, (int)pb->first_sector, (int)stream, b) < 0) {
				die("Couldn't find audio packet");
			}
		}
		u64 pts;
		if (get_pts(b, &pts) < 0) {
			die("Couldn't read PTS");
		}
		//printf("DEBUG: %"PRIu64"\n", pts);
		if (i == 0) {
			first_pts = pts;
			last_pts = pts;
			continue;
		}
		u64 this_pts = pts;
		if (track_mode == TRACK_OFFSET) {
			pts -= first_pts;
		} else {
			pts -= last_pts;
		}
		if (display_mode == DISPLAY_BYTES) {
			u64 bytes = pts * sample_rate / 90 * (sample_size/8) * chan;
			printf("%"PRIu64"\n", bytes);
		} else if (display_mode == DISPLAY_SAMPLES) {
			printf("%"PRIu64"\n", pts*sample_rate/90);
		} else if (display_mode == DISPLAY_FRAMES) {
			struct time t = time_from_pts(pts);
			printf("%.2u:%.2u:%.2u\n", t.hour*60 + t.min, t.sec, t.nano / 1000 * 75 / (uint)1e6);
		} else {
			struct time t = time_from_pts(pts);
			printf("%.2u:%.2u:%06.3f\n", t.hour, t.min, t.sec + t.nano / 1e9);
		}
		last_pts = this_pts;
	}

	if (track_mode == TRACK_LENGTH) {
		uint i = vtt.nr_of_ptts - 1U;
		uint pgcn = vtt.ptt[i].pgcn - 1U;
		uint pgn = vtt.ptt[i].pgn - 1U;
		pgc_t *pgc = ifo->vts_pgcit->pgci_srp[pgcn].pgc;
		uint cell = pgc->program_map[pgn];
		cell_playback_t *pb = &pgc->cell_playback[cell-1];
		sectorbuf b;

		if (find_stream(vob, (int)pb->last_sector, (int)stream, b) < 0) {
			die("Couldn't find last audio packet");
		}
		u64 pts;
		if (get_pts(b, &pts) < 0) {
			die("Couldn't read PTS");
		}
		pts -= last_pts;
		if (display_mode == DISPLAY_BYTES) {
			u64 bytes = pts * sample_rate / 90 * (sample_size/8) * chan;
			printf("%"PRIu64"\n", bytes);
		} else if (display_mode == DISPLAY_SAMPLES) {
			printf("%"PRIu64"\n", pts*sample_rate/90);
		} else if (display_mode == DISPLAY_FRAMES) {
			struct time t = time_from_pts(pts);
			printf("%.2u:%.2u:%.2u\n", t.hour*60 + t.min, t.sec, t.nano / 1000 * 75 / (uint)1e6);
		} else {
			struct time t = time_from_pts(pts);
			printf("%.2u:%.2u:%06.3f\n", t.hour, t.min, t.sec + t.nano / 1e9);
		}
	}

	/*
	switch (stream & 0xf8) {
	case 0x80: { // AC3
		int err;
		struct ac3_info info = read_ac3_header(b2, &err);
		printf("%d,%d,%d: ", j, k, a);
		if (err) {
			printf("error reading ac3 header\n");
			break;
		}
		printf("ac3 %d%sch %s %dkb/s\n",
			ac3_channels[info.channels],
			info.lfe ? ".1" : "",
			ac3_sample_rates[info.sample_rate],
			ac3_bitrates[info.frame_size]
		);
		break;
	}
	case 0x88: { // DTS
		//printf("%d,%d,%d: dts %\n");
		struct dts_info info = read_dts_header(b2);
		printf("%d,%d,%d: dts %d%sch %dbit %s %.1fkbps (actual: %.1fkbps)\n",
			j, k, a,
			dts_channels[info.channels],
			info.lfe ? ".1" : "",
			dts_source_bitdepth[info.source_bitdepth],
			info.ext_audio_id == 2 ? "96kHz" : dts_sample_rates[info.sample_rate],
			dts_target_bitrate[info.target_bitrate] / 1000.0,
			(info.frame_size+1) * 750 / 1000.0
		);
		break;
	}
	case 0xa0: {// LPCM
		struct lpcm_info info = read_lpcm_header(b2);
		printf("%d,%d,%d: lpcm %s %dbit %s\n",
			j, k, a,
			channels[info.channels],
			audio_sample_sizes[info.bitdepth],
			sample_rates[info.sample_rate]);
		break;
	}
	}
	*/

	DVDCloseFile(vob);
	ifoClose(ifo);

	DVDClose(dvd);
}

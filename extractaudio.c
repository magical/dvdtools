#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdbool.h>
#include <inttypes.h>
#include <string.h>
#include <getopt.h>
#include <unistd.h>
#include <dvdread/dvd_reader.h>
#include <dvdread/ifo_read.h>
#include "uint.h"
#include "bitreader.h"
#include "extractaudio.h"

typedef u8 sectorbuf[DVD_VIDEO_LB_LEN];

static int debug = 0;

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
static const char* dvd_channels[] = {"mono", "2ch", "3ch", "4ch", "5ch", "5.1ch", "7ch", "8ch"};
static const uint stream_ids[] = {0x80, 0, 0xc0, 0xc0, 0xa0, 0, 0x88, 0};

static const int dts_channels[] = {1, 2, 2, 2, 2, 3, 3, 4, 4, 5, 6, 6, 6, 7, 8, 8};
static const int dts_source_bitdepth[] = {16, 16, 20, 20, 0, 24, 24};
/*static const char* dts_sample_rates[] = {
	"?", "8kHz", "16kHz", "32kHz", "?", "?", "11.025kHz", "22.050kHz",
	"44.1kHz", "?", "?", "12kHz", "24kHz", "48kHz", "96kHz", "192kHz"
};*/
static const int dts_target_bitrate[] = {
	32000, 56000, 64000, 96000, 112000, 128000, 192000, 224000,
	256000, 320000, 384000, 448000, 512000, 576000, 640000, 768000,
	896000, 1024000, 1152000, 1280000, 1344000, 1408000, 1411200, 1472000,
	1536000, 1920000, 2048000, 3072000, 3840000,
	1/*open*/, 2/*variable*/, 3/*lossless*/
};

/*static const char* ac3_sample_rates[] = {"48kHz", "44.1kHz", "32kHz", "???Hz"};*/
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
			dvd_channels[a->channels],
			audio_sample_sizes[a->quantization],
			sample_rates[a->sample_frequency],
			stream_ids[a->audio_format]+(uint)n);
	} else {
		return printf("audio %d: %s %s %s %#02x\n", n,
			audio_formats[a->audio_format],
			dvd_channels[a->channels],
			sample_rates[a->sample_frequency],
			stream_ids[a->audio_format]+(uint)n);
	}
}

// Skip the MPEG header. Returns a pointer to the PES header.
u8 *skip_mpeg_header(u8 *b)
{
	return b + 0xe + (b[0xd] & 7);
}

// Skip the MPEG and PES headers. Returns a pointer to the substream header.
u8* skip_pes_header(u8 *b)
{
	u8 *p = b + 0xe + (b[0xd] & 7); // points to PES
	p = p + 9 + p[8]; // points to PES payload
	return p;
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
	u8 *p = skip_mpeg_header(b);
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
// Returns -1 on error.
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

struct lpcm_info read_lpcm_header(sectorbuf b)
{
	struct lpcm_info info = {0};
	static const int lpcm_bitdepths[] = {16, 20, 24, 0};
	static const int lpcm_sample_rates[] = {48000, 96000};

	int stream = pack_stream_id(b, true);
	if (!(0xa0 <= stream && stream < 0xa8)) {
		goto error;
	}
	// get pointer to audio substream header
	u8 *p = skip_pes_header(b) + 1;

	info.bitdepth = lpcm_bitdepths[p[4] >> 6];
	info.sample_rate = lpcm_sample_rates[(p[4] >> 4) & 3];
	info.channels = (p[4] & 3) + 1;

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

	u8 *p = skip_mpeg_header(b); // points to PES
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

// MPEG Header
// 00-03: 00 00 01 BA (4 bytes)
// 04-09: system clock reference (6 bytes)
// 0A-0C: mux rate
// 0D: stuffing length
// <stuffing>
//
// PES HEADER
// 00 00 01 <stream id>
// packet length
// XX XX <header length>
//
// AUDIO HEADER
// 1 byte: stream id
// 2 bytes: offset to first packet
// format-specific extra stuff

struct stream_info {
	// stream is the audio substream id.
	int stream;
	// data_offset is the offset to the start of the audio stream data.
	// all offsets are relative to the start of the sector.
	int data_offset;
	// first_frame_offset is the offset to the frame corresponding to the
	// packet's PTS (FirstAccUnit).
	// if there is no first frame, it is -1.
	int first_frame_offset;
	// end_offset is the offset to the byte after the packet data. in other
	// words, it is the size of the packet.
	int end_offset;
};

int get_stream_info(sectorbuf b, struct stream_info *info)
{
	int data_offset, first_frame_offset, size, stream;
	u8 *p = b;

	if (p[0] != 0 || p[1] != 0 || p[2] != 1 || p[3] != 0xba) {
		return -1;
	}

	p = skip_mpeg_header(b);
	if (p[0] != 0 || p[1] != 0 || p[2] != 1) {
		return -1;
	}
	if (p[3] != 0xbd) {
		return -1;
	}
	size = p[4]<<8 | p[5]; // + p+6 = end, so, length of the remaining data
	size += p+6 - b;

	p = skip_pes_header(b);
	stream = p[0];
	first_frame_offset = p[2]<<8 | p[3]; // relative to &p[3]
	if (first_frame_offset == 0) {
		// no first frame
		first_frame_offset = -1;
	} else {
		first_frame_offset += p+3 - b;
	}

	data_offset = p+4 - b;
	if ((stream & ~7) == 0xA0) {
		// LPCM streams have a 3-byte header
		data_offset += 3;
	}

	info->stream = stream;
	info->end_offset = size;
	info->data_offset = data_offset;
	info->first_frame_offset = first_frame_offset;
	return 0;
}

// Write audio packets to a file.
// Last_sector should be the first audio packet of the next chapter.
int dump_audio(struct writer* w, dvd_file_t *vob, int stream, int first_sector, int last_sector)
{
	sectorbuf b;
	int sector;
	int start, end;
	struct stream_info info;

	// First packet: dump packet starting at FirstAccUnit
	// Intermediate packets: dump complete packet contents
	// Last packet: dump packet until FirstAccUnit

	for (sector = first_sector; sector <= last_sector; sector++) {
		if (DVDReadBlocks(vob, sector, 1, b) < 1) {
			printf("sector %d: DVDReadBlocks failed\n", sector);
			return -1;
		}
		if (pack_stream_id(b, false) != 0xbd) {
			continue;
		}
		if (get_stream_info(b, &info) < 0) {
			printf("get_stream_info failed\n");
			return -1;
		}
		if (info.stream != stream) {
			if (sector == first_sector) {
				printf("sector %d: expected stream %#x, found %#x\n", sector, stream, info.stream);
				return -1;
			} else {
				continue;
			}
		}
		if (sector == first_sector || sector == last_sector) {
			if (info.first_frame_offset < 0) {
				printf("sector %d: no first frame\n", sector);
				return -1;
			}
			if (info.first_frame_offset >= info.end_offset) {
				printf("sector %d: invalid frame offset %#x\n",
					sector, info.first_frame_offset);
				return -1;
			}
		}
		if (sector == first_sector) {
			printf("sector %d: frame offset %#x\n", sector, info.first_frame_offset);
			start = info.first_frame_offset;
			end = info.end_offset;
		} else if (sector == last_sector) {
			start = info.data_offset;
			end = info.first_frame_offset;
		} else {
			start = info.data_offset;
			end = info.end_offset;
		}
		if (start == end) {
			continue;
		}
		if (w->write(w, b+start, end-start) < 0) {
			return -1;
		}
	}
	return 0;
}

static int file_write(struct writer* w, const u8* buf, int size);
static int file_close(struct writer* w);
static int repack_write(struct writer* w, const u8* buf, int size);
static int repack_close(struct writer* w);

static struct writer* open_file(const char* filename)
{
	struct writer* w = malloc(sizeof *w);
	if (w == NULL) {
		return NULL;
	}
	w->fp = fopen(filename, "wb");
	if (w->fp == NULL) {
		perror("fopen");
		free(w);
		return NULL;
	}
	w->write = file_write;
	w->close = file_close;
	return w;
}

static int file_write(struct writer* w, const u8* buf, int size)
{
	if (size < 0) {
		return -1;
	}
	if (fwrite(buf, 1, (size_t)size, w->fp) != (size_t)size) {
		perror("fwrite");
		return -1;
	}
	return 0;
}

static int file_close(struct writer* w)
{
	int err = fclose(w->fp);
	free(w);
	return err;
}

// Copy src to dst, repacking DVD PCM to little-endian PCM. Reads no more than
// srcsize bytes and writes no more than dstsize bytes. Stores the number of
// bytes written and read in wn and rn.
static void repack(u8 *dst, const u8 *src, int dstsize, int srcsize, int depth, int channels, int* wn, int* rn)
{
	int si, di, ch, j;
	uint b;
	int srcblocksize = 2*depth/8 * channels;
	int dstblocksize = 2*((depth+7)/8) * channels;
	if (debug) {
		fprintf(stderr, "dstsize=%d, dstblocksize=%d\n", dstsize, dstblocksize);
		fprintf(stderr, "srcsize=%d, srcblocksize=%d\n", srcsize, srcblocksize);
		fprintf(stderr, "depth=%d, channels=%d\n", depth, channels);
	}
	for (si = 0, di = 0; si+srcblocksize <= srcsize && di+dstblocksize <= dstsize; ) {
		//fprintf(stderr, "si=%d, di=%d\n", si, di);
		if (depth == 16) {
			for (ch = 0; ch < channels * 2; ch++) {
				dst[di + ch*2 + 1] = src[si + ch*2 + 0];
				dst[di + ch*2 + 0] = src[si + ch*2 + 1];
			}
		} else {
			for (ch = 0; ch < channels * 2; ch++) {
				dst[di + ch*3 + 2] = src[si + ch*2 + 0];
				dst[di + ch*3 + 1] = src[si + ch*2 + 1];
			}
		}
		si += channels * 2 * 2;
		if (depth == 16) {
			di += channels * 2 * 2;
		}
		else if (depth == 20) {
			// Low nibbles packed two per byte
			for (j = 0; j < channels; j++) {
				b = src[si + j];
				ch = j*2;
				dst[di + (ch+0)*3 + 0] = b & 0xf0;
				dst[di + (ch+1)*3 + 0] = (b & 0x0f) << 4;
			}
			si += channels;
			di += channels * 2 * 3;
		}
		else if (depth == 24) {
			for (ch = 0; ch < channels * 2; ch++) {
				dst[di + ch*3 + 0] = src[si + ch];
			}
			si += channels * 2;
			di += channels * 2 * 3;
		}
	}
	*wn = di;
	*rn = si;
	return;
}

enum { repack_bufsize = 8192 };

static int repack_write(struct writer* w, const u8* buf, int size)
{
	int wn, rn, err;

	while (size > 0) {
		repack(w->buf, buf, repack_bufsize, size, w->depth, w->channels, &wn, &rn);
		if (wn > 0) {
			err = w->writer->write(w->writer, w->buf, wn);
			if (err < 0) {
				return err;
			}
		}
		if (rn > size) {
			if (debug) {
				abort(); // uh-oh
			}
			rn = size;
		}
		buf += rn;
		size -= rn;
	}
	return 0;
}

static int repack_close(struct writer* w)
{
	int err = w->writer->close(w->writer);
	free(w->buf);
	free(w);
	return err;
}

struct writer* open_repack(struct writer* writer, struct lpcm_info info)
{
	struct writer* w;
	u8 *buf;

	w = malloc(sizeof *w);
	if (w == NULL) {
		return NULL;
	}

	buf = malloc((size_t)repack_bufsize);
	if (buf == NULL) {
		perror("malloc");
		free(w);
		return NULL;
	}

	w->write = repack_write;
	w->close = repack_close;
	w->writer = writer;
	w->buf = buf;
	w->depth = info.bitdepth;
	w->channels = info.channels;
	return w;
}


// Parse a decimal number and place it in *out.
// Returns a pointer to the unparsed portion of the string.
const char* parse_int(const char* s, int* out)
{
	int n = 0;
	const char *p;
	for (p = s; '0' <= *p && *p <= '9'; p++) {
		n *= 10;
		n += *p - '0';
	}
	if (p != s) {
		*out = n;
	}
	return p;
}

int parse_range(const char* s, int* startp, int* endp)
{
	// Valid inputs:
	//     N-M
	//      -N
	//     N-
	//      N

	const char* p = s;
	*startp = 1;
	*endp = -1;

	if (s[0] == '-' && s[1] == '\0') {
		return -1;
	}

	p = parse_int(p, startp);
	if (*p == '-') {
		p = parse_int(p+1, endp);
	} else if (p != s) {
		*endp = *startp;
	}
	if (*p != '\0' || p == s) {
		return -1;
	}

	return 0;
}

void usage(void)
{
	printf("usage: extractaudio [-d /dev/dvd] [-t title] [-a audio] [range]\n");
}

int main(int argc, char *argv[])
{
	char *dvd_filename = "/dev/dvd";
	uint title = 1;
	uint audio = 0;
	int chapterstart = 0, chapterend = -1;
	char* chapterrange = "1-";
	enum {
		FORMAT_RAW,
		FORMAT_FLAC,
	} format = FORMAT_RAW;
	int opt;
	while ((opt = getopt(argc, argv, "a:d:t:f")) != -1)
	switch (opt) {
	case 'a':
		audio = (uint)atoi(optarg);
		break;
	case 'd':
		dvd_filename = optarg;
		break;
	case 't':
		title = (uint)atoi(optarg);
		break;
	case 'f':
		format = FORMAT_FLAC;
		break;
	default:
		usage();
		return 0;
	}
	if (optind < argc) {
		chapterrange = argv[optind];
		optind++;
	}
	if (optind < argc) {
		usage();
		return 0;
	}

	if (parse_range(chapterrange, &chapterstart, &chapterend) < 0) {
		die("Couldn't parse chapter range");
	}
	printf("Ripping tracks %d through %d\n", chapterstart, chapterend);

	dvd_reader_t *dvd = DVDOpen(dvd_filename);
	if (dvd == NULL) { die("Couldn't open %s", dvd_filename); }
	ifo_handle_t *ifo0 = ifoOpen(dvd, 0);
	if (ifo0 == NULL) { die("Unable to open VIDEO_TS.IFO"); }

	if (!(1 <= title && title <= ifo0->tt_srpt->nr_of_srpts)) {
		die("Title out of range");
	}

	title_info_t tt = ifo0->tt_srpt->title[title-1];

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
	if (a.sample_frequency == 1 && a.audio_format != 4 && a.audio_format != 6) {
		die("Unknown sample rate");
	}
	print_audio(&a, (int)audio);
	int stream = (int)stream_ids[a.audio_format] + (int)audio;

	int chapters = vtt.nr_of_ptts;
	int audio_sector[chapters+1];

	for (int i = 0; i < chapters; i++) {
		uint pgcn = vtt.ptt[i].pgcn - 1U;
		uint pgn = vtt.ptt[i].pgn - 1U;
		pgc_t *pgc = ifo->vts_pgcit->pgci_srp[pgcn].pgc;
		uint cell = pgc->program_map[pgn];
		cell_playback_t *pb = &pgc->cell_playback[cell-1U];

		audio_sector[i] = get_audio_sector(vob, (int)pb->first_sector, (int)audio);
		if (audio_sector[i] < 0) {
			if (i != chapters - 1) {
				if (i < chapterstart-1) {
					fprintf(stderr, "warning: chapter %d: Couldn't get audio sector\n", i);
				} else {
					die("chapter %d: Couldn't get audio sector", i);
				}
			}
			audio_sector[i] = (int)pb->first_sector;
			chapters = i;
			break;
		}
		if (i == chapters - 1) {
			audio_sector[chapters] = (int)pb->last_sector;
		}
	}

	char *ext = ".bin";
	struct lpcm_info lpcm_info;
	if (format == FORMAT_RAW) {
		switch (stream & ~7) {
		case 0x80:
			ext = ".ac3";
			break;
		case 0x88:
			ext = ".dts";
			break;
		case 0xa0:
			ext = ".pcm";
			break;
		}
	} else if (format == FORMAT_FLAC) {
		if ((stream & ~7) != 0xa0) {
			printf("error: -f option can only be used with lpcm audio streams\n");
			return 1;
		}
		sectorbuf b;
		if (DVDReadBlocks(vob, audio_sector[0], 1, b) < 1) {
			printf("error: couldn't read audio sector\n");
			return 1;
		}
		lpcm_info = read_lpcm_header(b);
		//printf("%d: %d %d %d\n", audio_sector[0], lpcm_info.bitdepth, lpcm_info.sample_rate, lpcm_info.channels);
		ext = ".flac";
	}

	if (chapterstart > 0) {
		// Switch to zero-indexing
		chapterstart--;
	}
	if (chapterend < 0) {
		chapterend = chapters;
	}

	char filename[5+10+4+1];
	struct writer* w;
	for (int i = chapterstart; i < chapterend; i++) {
		snprintf(filename, sizeof filename, "track%02d%s", i+1, ext);
		if (format == FORMAT_RAW) {
			w = open_file(filename);
			if (w == NULL) {
				return 1;
			}
		} else if (format == FORMAT_FLAC) {
			struct writer* w0 = open_flac(filename, lpcm_info);
			if (w0 == NULL) {
				return 1;
			}
			w = open_repack(w0, lpcm_info);
			if (w == NULL) {
				w0->close(w0);
				return 1;
			}
		}
		dump_audio(w, vob, stream, audio_sector[i], audio_sector[i+1]);
		w->close(w);
	}

	DVDCloseFile(vob);
	ifoClose(ifo);

	ifoClose(ifo0);
	DVDClose(dvd);
}

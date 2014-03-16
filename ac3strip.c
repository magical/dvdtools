#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <errno.h>
//#include "bitreader.h"

struct bitreader {
	FILE *f;
	int nread; // number of bytes read
	int err;

	uint64_t res; // bit reservoir
	int n; // number of available bits
};

struct bitwriter {
	uint8_t *buf;
	int len;
	int cap;
	int err;

	uint64_t res;
	int n;
};

int      getnbitsread(struct bitreader*);
uint64_t readbits(struct bitreader*, int);
void     writebits(struct bitwriter*, uint64_t, int);
void grow(struct bitwriter*, int);

// Get the total number of bits read.
int
getnbitsread(struct bitreader *br) {
	return br->nread * 8 - br->n;
}

// Read n bits from br.
uint64_t
readbits(struct bitreader *br, int n)
{
	uint64_t ret;
	int c;
	while (br->n < n) {
		c = getc(br->f);
		if (c < 0) {
			br->err = errno;
			return 0;
		}
		br->res <<= 8;
		br->res |= (uint64_t)c;
		br->n += 8;
		br->nread += 1;
	}
	ret = br->res >> (br->n - n);
	ret &= ((uint64_t)1 << n) - (uint64_t)1;
	return ret;
}

// Write n bits to bw.
void
writebits(struct bitwriter *bw, uint64_t bits, int n)
{
	if (n <= 0) {
		return;
	}
	if (n < 64) {
		bits &= (1ULL<<n)-1ULL;
	}
	bw->res <<= n;
	bw->res |= bits;
	grow(bw, bw->len + bw->n/8);
	if (bw->err) {
		return;
	}
	while (bw->n >= 8 && bw->len < bw->cap) {
		bw->buf[bw->len] = (uint8_t)(bw->res >> (bw->n - 8));
		bw->n -= 8;
		bw->len += 1;
	}
}

void
grow(struct bitwriter *bw, int to)
{
	void *buf;
	if (to < bw->cap) {
		return;
	}
	if (to < bw->cap*2) {
		to = bw->cap*2;
	}
	if (to < 1024) {
		to = 1024;
	}
	buf = realloc(bw->buf, (size_t)to);
	if (buf == NULL) {
		// uh-oh
		bw->err = ENOMEM;
		return;
	}
	bw->buf = buf;
	bw->cap = to;
}

// Step 1: Round-trip

#define SYNCWORD 0x00B7

struct ac3 {
	struct bitwriter *bw;
	struct bitreader *br;

	int acmod; // channel mode
	int lfeon; // LFE channel present
	int fscod; // frequency code. 0=48kHz; 1=44.1kHz; 2=32kHz
	int frmsizecod; // frame size code
	int cplinu; // channel coupling in use flag

	int bap[256]; // bit allocation
};

int ac3(struct bitwriter*, struct bitreader*);
static int syncframe(struct ac3*);
static void audblk(struct ac3*);

static int nfchanstab[] = {2, 1, 2, 3, 3, 4, 4, 5};
static int expgrptab[] = {0, 3, 6, 12};
static int frmsizetab[3][38] = {{
	96, 96, 120, 120, 144, 144, 168, 168, 192, 192, 240, 240, 288, 288,
	336, 336, 384, 384, 480, 480, 576, 576, 672, 672, 768, 768, 960, 960,
	1152, 1152, 1344, 1344, 1536, 1536, 1728, 1728, 1920, 1920,
}, {
	69, 70, 87, 88, 104, 105, 121, 122, 139, 140, 174, 175, 208, 209, 243,
	244, 278, 279, 348, 349, 417, 418, 487, 488, 557, 558, 696, 697, 835,
	836, 975, 976, 1114, 1115, 1253, 1254, 1393, 1394,
}, {
	64, 64, 80, 80, 96, 96, 112, 112, 128, 128, 160, 160, 192, 192, 224,
	224, 256, 256, 320, 320, 384, 384, 448, 448, 512, 512, 640, 640, 768,
	768, 896, 896, 1024, 1024, 1152, 1152, 1280, 1280,
}};

// Copy and return n bits from a->br to a->bw.
int
copy(struct ac3 *a, int n)
{
	uint64_t bits = readbits(a->br, n);
	if (a->br->err) {
		return 0;
	}
	writebits(a->bw, bits, n);
	return (int)bits;
}

int
ac3(struct bitwriter *bw, struct bitreader *br)
{
	struct ac3 a;
	a.bw = bw;
	a.br = br;
	for (;;) {
		syncframe(&a);
	}
}

/* Appologies for the terrible variable names. They're straight from the spec. */

static int
syncframe(struct ac3 *a)
{
	int i, blk;
	int syncword, crcrsv, crc2;
	int addbsil;
	int nauxbits;

	syncword = (int)readbits(a->br, 16);
	if (syncword != SYNCWORD) {
		return -1;
	}
	writebits(a->bw, (uint64_t)syncword, 16);
	copy(a, 16); // crc1
	a->fscod      = copy(a, 2); // fscod
	a->frmsizecod = copy(a, 6); // frmsizecod

	// We'll come back later to fix the CRC.

	// Bit stream information
	//
	copy(a, 5); // bsid
	copy(a, 3); // bsmod
	a->acmod = copy(a, 3); // acmod
	if ((a->acmod & 1) && a->acmod != 1) {
		copy(a, 2); // cmixlev
	}
	if (a->acmod & 4) {
		copy(a, 2); // surmixlev
	}
	if (a->acmod == 2) {
		copy(a, 2); // dsurmod
	}
	a->lfeon = copy(a, 1); // lfeon
	copy(a, 5); // dialnorm
	if (copy(a, 1)) { // compre
		copy(a, 8); // compr
	}
	if (copy(a, 1)) { // langcode
		copy(a, 8); // langcod
	}
	if (copy(a, 1)) { // audprodie
		copy(a, 5); // mixlevel
		copy(a, 2); // roomtyp
	}
	if (a->acmod == 0) {
		copy(a, 5); // dialnorm2
		if (copy(a, 1)) { // compr2e
			copy(a, 8); // compr2
		}
		if (copy(a, 1)) { // langcod2e
			copy(a, 8); // langcod2
		}
		if (copy(a, 1)) { // audprodi2e
			copy(a, 5); // mixlevel2
			copy(a, 2); // roomtyp2
		}
	}
	copy(a, 1); // copyrightb
	copy(a, 1); // origbe
	if (copy(a, 1)) { // timecod1e
		copy(a, 14); // timecod1
	}
	if (copy(a, 1)) { // timecod2e
		copy(a, 14); // timecod2
	}
	if (copy(a, 1)) { // addbsie
		addbsil = copy(a, 6); // addbsil
		for (i = 0; i < addbsil + 1; i++) {
			copy(a, 8); // addbsi
		}
	}

	// Audio blocks
	//
	for (blk = 0; blk < 6; blk++) {
		audblk(a);
	}

	// Auxilliary bits
	//
	nauxbits = frmsizetab[a->fscod][a->frmsizecod] * 16;
	nauxbits -= getnbitsread(a->br);
	while (nauxbits >= 16) {
		copy(a, 16);
		nauxbits -= 16;
	}
	copy(a, nauxbits);
	copy(a, 14); // auxdatal
	copy(a, 1); // auxdatae

	// Final CRC
	//
	crcrsv = copy(a, 1); // crcrsv
	crc2 = copy(a, 16); // crc2

	(void)crcrsv;
	(void)crc2;

	return 0;
}

static void
audblk(struct ac3 *a)
{
	int i, ch, bin, seg;
	int nfchans;

	nfchans = nfchanstab[a->acmod];

	copy(a, nfchans); // blksw[ch]
	copy(a, nfchans); // dlithflag[ch]

	if (copy(a, 1)) { // dynrange
		copy(a, 8); // dynrang
	}

	if (a->acmod == 0) {
		if (copy(a, 1)) { // dynrang2e
			copy(a, 8); // dynrang2
		}
	}

	// Coupling
	//
	int cplinu, chincpl, cplcoe[5];
	int cplbegf, cplendf, cplbegmant, cplendmant;
	int bnd, ncplsubnd, ncplbnd;
	int phsflginu;
	cplinu = 0;
	chincpl = 0;
	phsflginu = 0;
	ncplbnd = 0;
	if (copy(a, 1)) { // cplstre
		cplinu = copy(a, 1); // cplinu
		if (cplinu) {
			chincpl = copy(a, nfchans); // chincpl
			if (a->acmod == 2) {
				phsflginu = copy(a, 1); // phsflginu
			}
			cplbegf = copy(a, 4); // cplbegf
			cplendf = copy(a, 4) + 3; // cplendf
			cplbegmant = cplbegf*12 + 37;
			cplendmant = cplendf*12 + 37;
			ncplsubnd = cplendf - cplbegf;
			ncplbnd = ncplsubnd;
			for (bnd = 1; bnd < ncplsubnd; bnd++) {
				ncplbnd += copy(a, 1); // ncplbnd
			}
		}
	}
	if (cplinu) {
		for (ch = 0; ch < nfchans; ch++) {
			if (chincpl & (1<<ch)) {
				cplcoe[ch] = copy(a, 1); // cplcoe[ch]
				if (cplcoe[ch]) {
					copy(a, 2); // mstrcplco[ch]
					for (bnd = 0; bnd < ncplbnd; bnd++) {
						copy(a, 4); // cplcoexp[ch][bnd]
						copy(a, 4); // cplcomant[ch][bnd]
					}
				}
			}
		}
		if (a->acmod == 2 && phsflginu && (cplcoe[0] || cplcoe[1])) {
			copy(a, ncplbnd); // phsflg
		}
	}
	if (a->acmod == 2) {
		if (copy(a, 1)) { // rematstr
			if (cplbegf == 0 && cplinu) {
				copy(a, 2); // rematflg
			} else if (cplbegf <= 2 && cplinu) {
				copy(a, 3); // rematflg
			} else {
				copy(a, 4); // rematflg
			}
		}
	}

	// Exponents
	//
	int cplexpstr, chexpstr[5], lfeexpstr;
	int chbwcod[5];
	int grp, ncplgrps, nchgrps;
	int tmp;
	cplexpstr = 0;
	if (cplinu) {
		cplexpstr = copy(a, 2); // cplexpstr
	}
	for (ch = 0; ch < nfchans; ch++) {
		chexpstr[ch] = copy(a, 2); // chexpstr[ch]
	}
	if (a->lfeon) {
		lfeexpstr = copy(a, 1); // lfeexpstr
	}
	for (ch = 0; ch < nfchans; ch++) {
		if (chexpstr[ch] != 0 && !(chincpl & (1<<ch))) {
			chbwcod[ch] = copy(a, 6); // chbwcod[ch]
		}
	}
	if (cplinu) {
		if (cplexpstr != 0) {
			ncplgrps = (cplendmant - cplbegmant) / expgrptab[cplexpstr];
			copy(a, 4); // cplabsexp
			for (grp = 0; grp < ncplgrps; grp++) {
				copy(a, 7); // cplexps
			}
		}
	}
	for (ch = 0; ch < nfchans; ch++) {
		if (chexpstr[ch] != 0) {
			copy(a, 4); // exps[ch][0]
			tmp = chexpstr[ch];
			nchgrps = (chbwcod[ch]*3 + 72 + expgrptab[tmp]-3) / expgrptab[tmp];
			for (grp = 0; grp < nchgrps; grp++) {
				copy(a, 7); // exps[ch][grp]
			}
		}
	}
	if (a->lfeon) {
		if (lfeexpstr != 0) {
			copy(a, 4); // lfeexps[0]
			copy(a, 7); // lfeexps[1]
			copy(a, 7); // lfeexps[2]
		}
	}

	// Bit allocation
	//
	int cpldeltbae, deltbae[5], deltnseg;
	if (copy(a, 1)) { // baie
		copy(a, 2); // sdcycod
		copy(a, 2); // fdcycod
		copy(a, 2); // sgaincod
		copy(a, 2); // dbpbcod
		copy(a, 2); // floorcod
	}
	if (copy(a, 1)) { // snroffste
		copy(a, 6); // csnroffst
		if (cplinu) {
			copy(a, 4); // cplfsnroffst
			copy(a, 3); // cplfgaincod
		}
		for (ch = 0; ch < nfchans; ch++) {
			copy(a, 4); // fsnroffst
			copy(a, 3); // fgaincod
		}
		if (a->lfeon) {
			copy(a, 4); // lfefsnroffst
			copy(a, 3); // lfefgaincod
		}
	}
	if (cplinu) {
		if (copy(a, 1)) { // cplleake
			copy(a, 3); // cplfleak
			copy(a, 3); // cplsleak
		}
	}
	if (copy(a, 1)) { // deltbaie
		if (cplinu) {
			cpldeltbae = copy(a, 2); // cpldeltdae
		}
		for (ch = 0; ch < nfchans; ch++) {
			deltbae[ch] = copy(a, 2); // deltdae[ch]
		}
		if (cplinu) {
			if (cpldeltbae == 1) {
				deltnseg = copy(a, 3); // deltnseg
				for (seg = 0; seg < deltnseg; seg++) {
					copy(a, 5); // deltoffst
					copy(a, 4); // deltlen
					copy(a, 3); // deltba
				}
			}
		}
		for (ch = 0; ch < nfchans; ch++) {
			if (deltbae[ch] == 1) {
				deltnseg = copy(a, 3); // deltnseg
				for (seg = 0; seg < deltnseg; seg++) {
					copy(a, 5); // deltoffst
					copy(a, 4); // deltlen
					copy(a, 3); // deltba
				}
			}
		}
	}

	// Skip bytes
	//
	int skipl;
	if (copy(a, 1)) { // skiple
		skipl = copy(a, 9); // skipl
		for (i = 0; i < skipl; i++) {
			copy(a, 8);
		}
	}

	// Mantissas
	//
	// nchmant[ch] from chbwcod[ch] or cplbegf
	//
	int nchmant[5];
	int ncplmant;
	int got_cplchan;
	got_cplchan = 0;
	for (ch = 0; ch < nfchans; ch++) {
		for (bin = 0; bin < nchmant[ch]; bin++) {
			copy(a, x); // chmant[ch][bin]
			if (cplinu && (chincpl & (1<<ch)) && !got_cplchan) {
				ncplmant = 12 * ncplsubnd;
				for (bin = 0; bin < ncplmant; bin++) {
					// cplmant[bin]
				}
				got_cplchan = 1;
			}
		}
	}
	if (a->lfeon) {
		for (bin = 0; bin < 7; bin++) {
			copy(a, x); // lfemant[bin]
		}
	}
}

int main()
{
	struct bitwriter bw;
	struct bitreader br;
	ac3(&bw, &br);
	return 0;
}

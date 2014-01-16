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
int      copyint(struct bitwriter*, struct bitreader*, int);
void grow(struct bitwriter*, int);

int
getnbitsread(struct bitreader *br) {
	return br->nread * 8 - br->n;
}

// Read N bits from B.
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

int
copyint(struct bitwriter *bw, struct bitreader *br, int n)
{
	uint64_t bits = readbits(br, n);
	if (br->err) {
		return 0;
	}
	writebits(bw, bits, n);
	return (int)bits;
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
static int syncframe(struct ac3*, struct bitwriter*, struct bitreader*);
static void audblk(struct ac3*, struct bitwriter*, struct bitreader*);

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
		syncframe(&a, bw, br);
	}
}

/* Appologies for the terrible variable names. They're straight from the spec. */

static int
syncframe(struct ac3 *a, struct bitwriter *bw, struct bitreader *br)
{
	int i, blk;
	int syncword, crcrsv, crc2;
	int addbsil;
	int nauxbits;

	syncword = (int)readbits(br, 16);
	if (syncword != SYNCWORD) {
		return -1;
	}
	writebits(bw, (uint64_t)syncword, 16);
	copyint(bw, br, 16); // crc1
	a->fscod      = copyint(bw, br, 2); // fscod
	a->frmsizecod = copyint(bw, br, 6); // frmsizecod

	// We'll come back later to fix the CRC.

	// Bit stream information
	//
	copyint(bw, br, 5); // bsid
	copyint(bw, br, 3); // bsmod
	a->acmod = copyint(bw, br, 3); // acmod
	if ((a->acmod & 1) && a->acmod != 1) {
		copyint(bw, br, 2); // cmixlev
	}
	if (a->acmod & 4) {
		copyint(bw, br, 2); // surmixlev
	}
	if (a->acmod == 2) {
		copyint(bw, br, 2); // dsurmod
	}
	a->lfeon = copyint(bw, br, 1); // lfeon
	copyint(bw, br, 5); // dialnorm
	if (copyint(bw, br, 1)) { // compre
		copyint(bw, br, 8); // compr
	}
	if (copyint(bw, br, 1)) { // langcode
		copyint(bw, br, 8); // langcod
	}
	if (copyint(bw, br, 1)) { // audprodie
		copyint(bw, br, 5); // mixlevel
		copyint(bw, br, 2); // roomtyp
	}
	if (a->acmod == 0) {
		copyint(bw, br, 5); // dialnorm2
		if (copyint(bw, br, 1)) { // compr2e
			copyint(bw, br, 8); // compr2
		}
		if (copyint(bw, br, 1)) { // langcod2e
			copyint(bw, br, 8); // langcod2
		}
		if (copyint(bw, br, 1)) { // audprodi2e
			copyint(bw, br, 5); // mixlevel2
			copyint(bw, br, 2); // roomtyp2
		}
	}
	copyint(bw, br, 1); // copyrightb
	copyint(bw, br, 1); // origbe
	if (copyint(bw, br, 1)) { // timecod1e
		copyint(bw, br, 14); // timecod1
	}
	if (copyint(bw, br, 1)) { // timecod2e
		copyint(bw, br, 14); // timecod2
	}
	if (copyint(bw, br, 1)) { // addbsie
		addbsil = copyint(bw, br, 6); // addbsil
		for (i = 0; i < addbsil + 1; i++) {
			copyint(bw, br, 8); // addbsi
		}
	}

	// Audio blocks
	//
	for (blk = 0; blk < 6; blk++) {
		audblk(a, bw, br);
	}

	// Auxilliary bits
	//
	nauxbits = frmsizetab[a->fscod][a->frmsizecod] * 16;
	nauxbits -= getnbitsread(br);
	while (nauxbits >= 16) {
		copyint(bw, br, 16);
		nauxbits -= 16;
	}
	copyint(bw, br, nauxbits);
	copyint(bw, br, 14); // auxdatal
	copyint(bw, br, 1); // auxdatae

	// Final CRC
	//
	crcrsv = copyint(bw, br, 1); // crcrsv
	crc2 = copyint(bw, br, 16); // crc2

	(void)crcrsv;
	(void)crc2;

	return 0;
}

static void
audblk(struct ac3 *a, struct bitwriter *bw, struct bitreader *br)
{
	int i, ch, bin, seg;
	int nfchans;
	int tmp;

	nfchans = nfchanstab[a->acmod];

	copyint(bw, br, nfchans); // blksw[ch]
	copyint(bw, br, nfchans); // dlithflag[ch]

	if (copyint(bw, br, 1)) { // dynrange
		copyint(bw, br, 8); // dynrang
	}

	if (a->acmod == 0) {
		if (copyint(bw, br, 1)) { // dynrang2e
			copyint(bw, br, 8); // dynrang2
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
	if (copyint(bw, br, 1)) { // cplstre
		cplinu = copyint(bw, br, 1); // cplinu
		if (cplinu) {
			chincpl = copyint(bw, br, nfchans); // chincpl
			if (a->acmod == 2) {
				phsflginu = copyint(bw, br, 1); // phsflginu
			}
			cplbegf = copyint(bw, br, 4); // cplbegf
			cplendf = copyint(bw, br, 4) + 3; // cplendf
			cplbegmant = cplbegf*12 + 37;
			cplendmant = cplendf*12 + 37;
			ncplsubnd = cplendf - cplbegf;
			ncplbnd = ncplsubnd;
			for (bnd = 1; bnd < ncplsubnd; bnd++) {
				ncplbnd += copyint(bw, br, 1); // ncplbnd
			}
		}
	}
	if (cplinu) {
		for (ch = 0; ch < nfchans; ch++) {
			if (chincpl & (1<<ch)) {
				cplcoe[ch] = copyint(bw, br, 1); // cplcoe[ch]
				if (cplcoe[ch]) {
					copyint(bw, br, 2); // mstrcplco[ch]
					for (bnd = 0; bnd < ncplbnd; bnd++) {
						copyint(bw, br, 4); // cplcoexp[ch][bnd]
						copyint(bw, br, 4); // cplcomant[ch][bnd]
					}
				}
			}
		}
		if (a->acmod == 2 && phsflginu && (cplcoe[0] || cplcoe[1])) {
			copyint(bw, br, ncplbnd); // phsflg
		}
	}
	if (a->acmod == 2) {
		if (copyint(bw, br, 1)) { // rematstr
			if (cplbegf == 0 && cplinu) {
				copyint(bw, br, 2); // rematflg
			} else if (cplbegf <= 2 && cplinu) {
				copyint(bw, br, 3); // rematflg
			} else {
				copyint(bw, br, 4); // rematflg
			}
		}
	}

	// Exponents
	//
	int cplexpstr, chexpstr[5], lfeexpstr;
	int chbwcod[5];
	int grp, ncplgrps, nchgrps;
	cplexpstr = 0;
	if (cplinu) {
		cplexpstr = copyint(bw, br, 2); // cplexpstr
	}
	for (ch = 0; ch < nfchans; ch++) {
		chexpstr[ch] = copyint(bw, br, 2); // chexpstr[ch]
	}
	if (a->lfeon) {
		lfeexpstr = copyint(bw, br, 1); // lfeexpstr
	}
	for (ch = 0; ch < nfchans; ch++) {
		if (chexpstr[ch] != 0 && !(chincpl & (1<<ch))) {
			chbwcod[ch] = copyint(bw, br, 6); // chbwcod[ch]
		}
	}
	if (cplinu) {
		if (cplexpstr != 0) {
			ncplgrps = (cplendmant - cplbegmant) / expgrptab[cplexpstr];
			copyint(bw, br, 4); // cplabsexp
			for (grp = 0; grp < ncplgrps; grp++) {
				copyint(bw, br, 7); // cplexps
			}
		}
	}
	for (ch = 0; ch < nfchans; ch++) {
		if (chexpstr[ch] != 0) {
			copyint(bw, br, 4); // exps[ch][0]
			tmp = chexpstr[ch];
			nchgrps = (chbwcod[ch]*3 + 72 + expgrptab[tmp]-3) / expgrptab[tmp];
			for (grp = 0; grp < nchgrps; grp++) {
				copyint(bw, br, 7); // exps[ch][grp]
			}
		}
	}
	if (a->lfeon) {
		if (lfeexpstr != 0) {
			copyint(bw, br, 4); // lfeexps[0]
			copyint(bw, br, 7); // lfeexps[1]
			copyint(bw, br, 7); // lfeexps[2]
		}
	}

	// Bit allocation
	//
	int cpldeltbae, deltbae[5], deltnseg;
	if (copyint(bw, br, 1)) { // baie
		copyint(bw, br, 2); // sdcycod
		copyint(bw, br, 2); // fdcycod
		copyint(bw, br, 2); // sgaincod
		copyint(bw, br, 2); // dbpbcod
		copyint(bw, br, 2); // floorcod
	}
	if (copyint(bw, br, 1)) { // snroffste
		copyint(bw, br, 6); // csnroffst
		if (cplinu) {
			copyint(bw, br, 4); // cplfsnroffst
			copyint(bw, br, 3); // cplfgaincod
		}
		for (ch = 0; ch < nfchans; ch++) {
			copyint(bw, br, 4); // fsnroffst
			copyint(bw, br, 3); // fgaincod
		}
		if (a->lfeon) {
			copyint(bw, br, 4); // lfefsnroffst
			copyint(bw, br, 3); // lfefgaincod
		}
	}
	if (cplinu) {
		if (copyint(bw, br, 1)) { // cplleake
			copyint(bw, br, 3); // cplfleak
			copyint(bw, br, 3); // cplsleak
		}
	}
	if (copyint(bw, br, 1)) { // deltbaie
		if (cplinu) {
			cpldeltbae = copyint(bw, br, 2); // cpldeltdae
		}
		for (ch = 0; ch < nfchans; ch++) {
			deltbae[ch] = copyint(bw, br, 2); // deltdae[ch]
		}
		if (cplinu) {
			if (cpldeltbae == 1) {
				deltnseg = copyint(bw, br, 3); // deltnseg
				for (seg = 0; seg < deltnseg; seg++) {
					copyint(bw, br, 5); // deltoffst
					copyint(bw, br, 4); // deltlen
					copyint(bw, br, 3); // deltba
				}
			}
		}
		for (ch = 0; ch < nfchans; ch++) {
			if (deltbae[ch] == 1) {
				deltnseg = copyint(bw, br, 3); // deltnseg
				for (seg = 0; seg < deltnseg; seg++) {
					copyint(bw, br, 5); // deltoffst
					copyint(bw, br, 4); // deltlen
					copyint(bw, br, 3); // deltba
				}
			}
		}
	}

	// Skip bytes
	//
	int skipl;
	if (copyint(bw, br, 1)) { // skiple
		skipl = copyint(bw, br, 9); // skipl
		for (i = 0; i < skipl; i++) {
			copyint(bw, br, 8);
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
			copyint(bw, br, x); // chmant[ch][bin]
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
			copyint(bw, br, x); // lfemant[bin]
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

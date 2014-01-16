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

uint64_t readint(struct bitreader*, int);
void     writeint(struct bitwriter*, uint64_t, int);
uint64_t copyint(struct bitwriter*, struct bitreader*, int);
void grow(struct bitwriter*, int);

// Read N bits from B.
uint64_t
readint(struct bitreader *br, int n)
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
		br->res |= c;
		br->n += 8;
		br->nread += 1;
	}
	ret = br->res >> (br->n - n);
	ret &= ((uint64_t)1 << n) - (uint64_t)1;
	return ret;
}

void
writeint(struct bitwriter *bw, uint64_t bits, int n)
{
	bw->res <<= n;
	bw->res |= bits;
	grow(bw, bw->len + bw->n/8);
	if (bw->err) {
		return;
	}
	while (bw->n >= 8 && bw->len < bw->cap) {
		bw->buf[bw->len] = bw->res >> (bw->n - 8);
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
	buf = realloc(bw->buf, to);
	if (buf == NULL) {
		// uh-oh
		bw->err = ENOMEM;
		return;
	}
	bw->buf = buf;
	bw->cap = to;
}

// Copy and return n bits from br to bw.
uint64_t
copyint(struct bitwriter *bw, struct bitreader *br, int n)
{
	uint64_t bits = readint(br, n);
	if (br->err) {
		return 0;
	}
	writeint(bw, bits, n);
	return bits;
}

// Step 1: Round-trip

#define SYNCWORD 0x00B7

int ac3(struct bitwriter*, struct bitreader*);
int syncframe(struct bitwriter*, struct bitreader*);
void bsi(struct bitwriter*, struct bitreader*);
void audblk(struct bitwriter*, struct bitreader*);
void auxdata(void);

static int nfchanstab[] = {2, 1, 2, 3, 3, 4, 4, 5};
static int cplexpgrptab[] = {0, 3, 6, 12};

struct ac3 {
	struct bitwriter *bw;
	struct bitreader *br;
};

int
ac3(struct bitwriter *bw, struct bitreader *br)
{
	for (;;) {
		syncframe(bw, br);
	}
}

int
syncframe(struct bitwriter *bw, struct bitreader *br)
{
	int blk;
	int crcrsv, crc2;

	int syncword = readint(br, 16);
	if (syncword != SYNCWORD) {
		return -1;
	}
	writeint(bw, syncword, 16);
	copyint(bw, br, 16); // crc1
	copyint(bw, br, 2); // fscod
	copyint(bw, br, 6); // frmsizecod

	// We'll come back and fix up the CRC later.

	bsi(bw, br);
	for (blk = 0; blk < 6; blk++) {
		audblk(bw, br);
	}
	auxdata();

	crcrsv = copyint(bw, br, 1);
	crc2 = copyint(bw, br, 16);

	(void)crcrsv;
	(void)crc2;

	return 0;
}

void
bsi(struct bitwriter *bw, struct bitreader *br)
{
	int acmod, addbsil;
	int i;

	copyint(bw, br, 5); // bsid
	copyint(bw, br, 3); // bsmod
	acmod = copyint(bw, br, 3);
	if ((acmod & 1) && acmod != 1) {
		copyint(bw, br, 2); // cmixlev
	}
	if (acmod & 4) {
		copyint(bw, br, 2); // surmixlev
	}
	if (acmod == 2) {
		copyint(bw, br, 2); // dsurmod
	}
	copyint(bw, br, 1); // lfeon
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
	if (acmod == 0) {
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
		addbsil = copyint(bw, br, 6);
		for (i = 0; i < addbsil + 1; i++) {
			copyint(bw, br, 8); // addbsi
		}
	}
}

void
audblock(struct bitwriter *bw, struct bitreader *br)
{
	int acmod, lfeon;
	int ch, nfchans;
	nfchans = nfchanstab[acmod];

	int i, bin, seg;

	copyint(bw, br, nfchans); // blksw[ch]
	copyint(bw, br, nfchans); // dlithflag[ch]

	if (copyint(bw, br, 1)) { // dynrange
		copyint(bw, br, 8); // dynrang
	}

	if (acmod == 0) {
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
		cplinu = copyint(bw, br, 1);
		if (cplinu) {
			chincpl = copyint(bw, br, nfchans);
			if (acmod == 2) {
				phsflginu = copyint(bw, br, 1);
			}
			cplbegf = copyint(bw, br, 4);
			cplendf = copyint(bw, br, 4) + 3;
			cplbegmant = cplbegf*12 + 37;
			cplendmant = cplendf*12 + 37;
			ncplsubnd = cplendf - cplbegf;
			ncplbnd = ncplsubnd;
			for (bnd = 1; bnd < ncplsubnd; bnd++) {
				ncplbnd += copyint(bw, br, 1);
			}
		}
	}
	if (cplinu) {
		for (ch = 0; ch < nfchans; ch++) {
			if (chincpl & (1<<ch)) {
				cplcoe[ch] = copyint(bw, br, 1);
				if (cplcoe[ch]) {
					copyint(bw, br, 2); // mstrcplco[ch]
					for (bnd = 0; bnd < ncplbnd; bnd++) {
						copyint(bw, br, 4); // cplcoexp[ch][bnd]
						copyint(bw, br, 4); // cplcomant[ch][bnd]
					}
				}
			}
		}
		if (acmod == 2 && phsflginu && (cplcoe[0] || cplcoe[1])) {
			copyint(bw, br, ncplbnd); // phsflg
		}
	}
	if (acmod == 2) {
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
	int grp, ncplgrps, nchgrps;
	cplexpstr = 0;
	if (cplinu) {
		cplexpstr = copyint(bw, br, 2);

	}
	for (ch = 0; ch < nfchans; ch++) {
		chexpstr[ch] = copyint(bw, br, 2);
	}
	if (lfeon) {
		lfeexpstr = copyint(bw, br, 1);
	}
	for (ch = 0; ch < nfchans; ch++) {
		if (chexpstr[ch] != 0 && !(chincpl & (1<<ch))) {
			copyint(bw, br, 6); // chbwcod
		}
	}
	if (cplinu) {
		if (cplexpstr != 0) {
			ncplgrps = (cplendmant - cplbegmant) / cplexpgrptab[cplexpstr];
			copyint(bw, br, 4); // cplabsexp
			for (grp = 0; grp < ncplgrps; grp++) {
				copyint(bw, br, 7); // cplexps
			}
		}
	}
	for (ch = 0; ch < nfchans; ch++) {
		if (chexpstr[ch] != 0) {
			copyint(bw, br, 4); // exps[ch][0]
			nchgrps = 0; // XXX
			for (grp = 0; grp < nchgrps; grp++) {
				copyint(bw, br, 7); // exps[ch][grp]
			}
		}
	}
	if (lfeon) {
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
		if (lfeon) {
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
			cpldeltbae = copyint(bw, br, 2);
		}
		for (ch = 0; ch < nfchans; ch++) {
			deltbae[ch] = copyint(bw, br, 2);
		}
		if (cplinu) {
			if (cpldeltbae == 1) {
				deltnseg = copyint(bw, br, 3);
				for (seg = 0; seg < deltnseg; seg++) {
					copyint(bw, br, 5); // deltoffst
					copyint(bw, br, 4); // deltlen
					copyint(bw, br, 3); // deltba
				}
			}
		}
		for (ch = 0; ch < nfchans; ch++) {
			if (deltbae[ch] == 1) {
				deltnseg = copyint(bw, br, 3);
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
	if (lfeon) {
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

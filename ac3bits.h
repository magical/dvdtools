// Struct balloc contains bit allocation parameters.
struct balloc {
	int deltbae, deltnseg;
	int deltoffst[8], deltlen[8], deltba[8];
	int fgain, fsnroffst;
	int fleak, sleak;
};

int bit_allocation(
	int *bapout,
	struct balloc *ba, int fscod,
	int *exp,
	int start, int end,
	int csnroffst,
	int sdecay, int fdecay, int sgain, int dbknee, int floor
);

int
decode_exponents(
	int *exp,
	int* gexp, int ngrps, int absexp, int grpsize
);

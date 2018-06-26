#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>

FILE *fdopen(int, const char*);
static int flac_write(struct writer* w, const u8* buf, int size);
static int flac_close(struct writer* w);

char *itoa(int n) {
	char *a = malloc(11);
	int err = snprintf(a, 11, "%d", n);
	if (err < 0) {
		perror("snprintf");
		free(a);
		return NULL;
	} else if (err >= 11) {
		free(a);
		return NULL;
	}
	return a;
}


struct writer* open_flac(char *filename, struct lpcm_info info)
{
	char *bps = NULL, *samplerate = NULL, *chans = NULL;
	int pid, err;
	int fd[2];
	FILE *fp = NULL;
	struct writer* w = NULL;

	if (filename == NULL) {
		return NULL;
	}

	w = malloc(sizeof *w);
	if (w == NULL) {
		return NULL;
	}

	err = pipe(fd);
	if (err < 0) {
		perror("pipe");
		goto cleanup;
	}

	fp = fdopen(fd[1], "w");
	if (fp == NULL) {
		goto cleanup;
	}

	// It might seem like it would be better to allocate these in the
	// child, but we'd be in big trouble if they failed.
	bps = itoa(info.bitdepth);
	samplerate = itoa(info.sample_rate);
	chans = itoa(info.channels);
	if (bps == NULL || samplerate == NULL || chans == NULL) {
		goto cleanup;
	}

	pid = fork();
	if (pid < 0) {
		perror("fork");
		goto cleanup;
	} else if (pid == 0) {
		close(fd[1]);
		dup2(fd[0], 0);
		execlp("flac",
			"flac",
			"--silent",
			"-o", filename,
			"--force-raw-format",
			"--sign=signed",
			"--endian=little",
			"--bps", bps,
			"--sample-rate", samplerate,
			"--channels", chans,
			"-",
			NULL);
		// If we get here, execlp failed
		perror("execlp");
		_exit(1);
	}

	close(fd[0]);
	free(bps);
	free(samplerate);
	free(chans);

	w->fp = fp;
	w->pid = pid;
	w->write = flac_write;
	w->close = flac_close;

	return w;

cleanup:
	free(w);
	free(bps);
	free(samplerate);
	free(chans);
	return NULL;
}

static int flac_write(struct writer* w, const u8* buf, int size)
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


static int flac_close(struct writer* w)
{
	int err = fclose(w->fp);
	waitpid(w->pid, NULL, 0);
	free(w);
	return err;
}

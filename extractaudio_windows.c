#include <stdlib.h>
#include <stdio.h>
#include <windows.h>
#include <strsafe.h>
#include "extractaudio.h"

#define FLAC_PATH "C:\\Users\\Andrew\\Downloads\\flac-1.3.1-win\\win64\\flac.exe"

static int flac_write(struct writer* w, const unsigned char* buf, int size);
static int flac_close(struct writer* w);

// https://docs.microsoft.com/en-us/windows/desktop/ProcThread/creating-a-child-process-with-redirected-input-and-output
struct writer* open_flac(char *filename, struct lpcm_info info)
{
	struct writer* w = NULL;
	HANDLE prd, pwr;
	PROCESS_INFORMATION piProcInfo = {0};
	STARTUPINFO siStartInfo = {0};
	SECURITY_ATTRIBUTES saAttr = {0};
	char* cmdline = NULL;

	saAttr.nLength = sizeof(SECURITY_ATTRIBUTES);
	saAttr.bInheritHandle = TRUE;
	saAttr.lpSecurityDescriptor = NULL;

	if (filename == NULL) {
		return NULL;
	}

	w = malloc(sizeof *w);
	if (w == NULL) {
		return NULL;
	}

	if (!CreatePipe(&prd, &pwr, &saAttr, 0)) {
		printf("CreatePipe failed: %lu\n", GetLastError());
		goto cleanup;
	}

	// Ensure the write handle to the pipe for STDIN is not inherited.
    if (!SetHandleInformation(pwr, HANDLE_FLAG_INHERIT, 0)) {
		printf("SetHandleInformation failed: %lu\n", GetLastError());
		goto cleanup;
	}

	size_t cmdlinesize = 150 + sizeof(FLAC_PATH) + strlen(filename) + 9+9+9;
	cmdline = malloc(cmdlinesize);
	StringCbPrintf(
		cmdline,
		cmdlinesize,
		"\"%s\" --silent"
			" -o \"%s\""
			" --force-raw-format"
			" --sign=signed"
			" --endian=little"
			" --bps %d"
			" --sample-rate %d"
			" --channels %d"
			" -",
		FLAC_PATH,
		filename,
		info.bitdepth,
		info.sample_rate,
		info.channels
	);

	if (!CreateProcess(NULL,
		cmdline,
		NULL, // security attributes
		NULL, // thread security attributes
		TRUE, // inherit handles
		0, // creation flags
		NULL, // inherit environment
		NULL, // inherit working directory
		&siStartInfo,
		&piProcInfo
	)) {
		printf("CreateProcess failed: %lu\n", GetLastError());
		return -1;
	}

	w->hProcess = piProcInfo.hProcess;
	w->hPipe = pwr;
	w->write = flac_write;
	w->close = flac_close;

	free(cmdline);
	return w;

cleanup:
	free(w);
	free(cmdline);
	return NULL;
}

static int flac_write(struct writer* w, const unsigned char* buf, int size)
{
	DWORD dwWritten;
	if (!WriteFile(w->hPipe, buf, size, &dwWritten, NULL)) {
		return -1;
	}
	return dwWritten;
}

static int flac_close(struct writer* w)
{
	int success = CloseHandle(w->hPipe);
	WaitForSingleObject( w->hProcess, INFINITE );
	free(w);
	if (!success) {
		return 1;
	}
	return 0;
}

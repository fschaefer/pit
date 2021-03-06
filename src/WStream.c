#include "WStream.h"
#include "common.h"

#include <stdlib.h>
#include <stdio.h>
#include <inttypes.h>
#include <fcntl.h>
#include <glob.h> /* нужно чтобы просто обмануть нетбинс чтобы он думал что size_t задефайнен */
#include <errno.h>
#include <unistd.h>
#include <string.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <limits.h>
#include <dirent.h>

static void WStream__createChunk(struct WStream *ws);
static void WStream__acquireWriterLock(struct WStream *ws);
static void WStream__write(struct WStream *ws, const char *buf, ssize_t len, char writeInOneChunk);
static void WStream__findLastTimemicro(struct WStream *ws);
static void WStream__mayCloseChunk(struct WStream *ws);
static void WStream__needChunk(struct WStream *ws);
static void WStream__closeChunk(struct WStream *ws);

void WStream_init(struct WStream *ws, const char *rootDir, ssize_t chunkSize) {
	ws->rootDir = rootDir;

	ws->chunkFd = -1;
	ws->writerLockFd = -1;
	ws->timestampChunkNumber = 0;
	ws->chunkMaxSize = chunkSize;
	ws->lineBuffer = NULL;
	ws->lineBufferSize = 0;
	ws->lineBufferMaxSize = 0;
	ws->lastChunkTimemicro = 0;
	ws->denyChunkClose = 0;
	ws->chunkCloseScheduled = 0;
	ws->lastCreatedChunkTimemicro = 0;

	ws->pid = (unsigned long)getpid();
	ws->startTime = (uint32_t)time(NULL);

	if(mkdir(rootDir, 0755) == -1) {
		if(errno != EEXIST)
			error("mkdir('%s')", rootDir);
	}

	WStream__acquireWriterLock(ws);
	WStream__findLastTimemicro(ws);
	/*WStream__createNextChunk(ws);*/
}

void WStream_destroy(struct WStream *ws) {
	if(ws->chunkFd >= 0)
		close(ws->chunkFd);

	if(ws->lineBuffer) {
		free(ws->lineBuffer);
		ws->lineBuffer = NULL;
		ws->lineBufferSize = 0;
		ws->lineBufferMaxSize = 0;
	}

	ws->chunkFd = -1;

	if(ws->writerLockFd >= 0)
		close(ws->writerLockFd);

	ws->writerLockFd = -1;
}

void WStream_write(struct WStream *ws, const char *buf, ssize_t len) {
	ws->denyChunkClose = 1;

	WStream__write(ws, buf, len, 0);

	ws->denyChunkClose = 0;
}

void WStream_flush(struct WStream *ws) {
	if(ws->lineBuffer && ws->lineBufferSize) {
		debug("flush line tail");
		WStream__write(ws, ws->lineBuffer, ws->lineBufferSize, 1);
		ws->lineBufferSize = 0;
	}
}

/**
 * В отличии от WStream_write() эта функция гарантирует, что строка будет
 * целиком записана в один чанк без разбивки.
 * Допускается передавать неполные строки. В этом случае функцию бедуте ждать
 * окончания строки перед фактической записью
 *
 * @param ws
 * @param buf
 * @param len
 */
void WStream_writeLines(struct WStream *ws, const char *buf, ssize_t len) {
	char *lastLineEnd;
	ssize_t toWrite;
	ssize_t toBuffer;

	ws->denyChunkClose = 1;

	if(!ws->lineBuffer) {
		ws->lineBuffer = malloc(WSTREAM_LINE_MAX_LENGTH);
		ws->lineBufferMaxSize = WSTREAM_LINE_MAX_LENGTH;
		ws->lineBufferSize = 0;
	}

	lastLineEnd = memrchr(buf, '\n', (size_t)len);
	toWrite = lastLineEnd ? (ssize_t)(lastLineEnd - buf) + 1 : 0;
	toBuffer = len - toWrite;

	if(toWrite) {
		if(ws->lineBufferSize) {
			WStream__write(ws, ws->lineBuffer, ws->lineBufferSize, 1);
			ws->lineBufferSize = 0;
		}

		WStream__write(ws, buf, toWrite, 1);
	}

	if(toBuffer) {
		if(ws->lineBufferSize + toBuffer <= ws->lineBufferMaxSize) {
			memcpy(ws->lineBuffer + ws->lineBufferSize, buf + toWrite, (size_t)toBuffer);
			ws->lineBufferSize += toBuffer;
		} else {
			/*
			 * бида-бида, строка не помещается в буффер
			 * просто флашим весь кусок, не допуская сплита. Пока не встретится
			 * символ '\n' мы будем писать в тот же самый чанк
			 *
			 * Можно обойтись без буферизации и писать сразу в файл, просто запрещая сплит,
			 * но на буферизации мы сэкономим на одном вызове WStream__write() со
			 * сложной логикой и несколько io-вызовов на каждый вызов WStream_writeLines()
			 */
			warning("line is too long, flushing with split supression '\\n'");

			WStream__write(ws, ws->lineBuffer, ws->lineBufferSize, 1);
			ws->lineBufferSize = 0;

			WStream__write(ws, buf + toWrite, toBuffer, 1);
		}
	}

	ws->denyChunkClose = 0;

	WStream__mayCloseChunk(ws);
}

/**
 * @param ws
 * @param buf
 * @param len
 * @param disableSplit Если != 0, то весь буфер будет гарантированно записан в один чанк
 */
static void WStream__write(struct WStream *ws, const char *buf, ssize_t len, char disableSplit) {
	ssize_t written = 0;

	WStream__needChunk(ws);

	if(!disableSplit && ws->chunkSize >= ws->chunkMaxSize) {
		WStream__closeChunk(ws);
		WStream__createChunk(ws);
	}

	/* хак, позволяющий сделать проверку на новый чанк, не записывая ничего */
	if(!buf)
		return;

	do {
		while(written != len && (disableSplit || ws->chunkSize < ws->chunkMaxSize)) {
			ssize_t toWriteInThisChunk = len - written;
			ssize_t wr;
			int fd = ws->chunkFd;
			int fdMustBeClosed = 0;

			if(!disableSplit && toWriteInThisChunk >= ws->chunkMaxSize - ws->chunkSize) {
				/*
				 * чанк закончится на этой записи, заранее создаём новый,
				 * чтобы читателю гарантированно было куда переключиться
				 */

				toWriteInThisChunk = ws->chunkMaxSize - ws->chunkSize;

				debug("chunk size overflow (%llu bytes)", (unsigned long long)ws->chunkMaxSize);

				WStream__createChunk(ws);
				fdMustBeClosed = 1;
			} else {
				ws->chunkSize += toWriteInThisChunk;
			}

			/* пишем этот кусок с учётом возможных прерываний по сигналам */
			do {
				wr = write(fd, buf + written, (size_t)toWriteInThisChunk);

				if(wr <= 0) {
					if(errno != EINTR)
						error("write(#%d)", fd);
				}

				written += wr;
				toWriteInThisChunk -= wr;
			} while(toWriteInThisChunk);

			if(fdMustBeClosed)
				close(fd);
		}
	} while(written < len);
}

void WStream_scheduleCloseChunk(struct WStream *ws) {
	ws->chunkCloseScheduled = 1;
	WStream__mayCloseChunk(ws);
}

static void WStream__needChunk(struct WStream *ws) {
	if(ws->chunkFd == -1)
		WStream__createChunk(ws);
}

static void WStream__mayCloseChunk(struct WStream *ws) {
	if(ws->chunkCloseScheduled)
		WStream__closeChunk(ws);
}

static void WStream__closeChunk(struct WStream *ws) {
	if(ws->chunkFd != -1) {
		debug("chunk closed");
		close(ws->chunkFd);
		ws->chunkFd = -1;
	}

	ws->chunkCloseScheduled = 0;
}

static void WStream__acquireWriterLock(struct WStream *ws) {
	char path[PATH_MAX];

	snprintf(path, sizeof(path), "%s/.writer.lock", ws->rootDir);

	debug("acquiring writer lock: %s", path);
	ws->writerLockFd = open(path, O_CREAT | O_WRONLY, 0644);
	if(ws->writerLockFd == -1)
		error("unable to open/create lock-file '%s'", path);

	if(flock(ws->writerLockFd, LOCK_SH | LOCK_NB) == -1)
		error("unable to acquire writer lock on '%s'. Maybe this stream currenly used", ws->rootDir);
}

static void WStream__createChunk(struct WStream *ws) {
	char tmpPathBuf[PATH_MAX + 64];
	char pathBuf[PATH_MAX + 64];

	int fd;
	uint64_t currentTimemicro = timemicro();

	if(currentTimemicro == ws->lastChunkTimemicro) {
		ws->timestampChunkNumber++;
	} else {
		ws->timestampChunkNumber = 0;
		ws->lastChunkTimemicro = currentTimemicro;
	}

	snprintf(
		pathBuf,
		sizeof(pathBuf),
		"%s/%016" PRIu64 ".%03lu.%05lu-%08" PRIx32 ".chunk",
		ws->rootDir,
		ws->lastChunkTimemicro,
		ws->timestampChunkNumber,
		ws->pid & 0xffffl,
		ws->startTime
	);

	snprintf(tmpPathBuf, sizeof(tmpPathBuf), "%s.tmp", pathBuf);

	debug("creating new chunk: %s -> %s", tmpPathBuf, pathBuf);

	fd = open(tmpPathBuf, O_CREAT | O_WRONLY | O_EXCL, 0644);
	if(fd < 0)
		error("open('%s')", tmpPathBuf);

	if(!flockRangeNB(fd, 1, 1, F_WRLCK))
		error("file '%s' already locked", tmpPathBuf);

	if(rename(tmpPathBuf, pathBuf) == -1)
		error("rename('%s', '%s')", tmpPathBuf, pathBuf);

	ws->chunkFd = fd;
	ws->chunkSize = 0;

	ws->chunkCloseScheduled = 0;
	ws->lastCreatedChunkTimemicro = timemicro();
}

static void WStream__findLastTimemicro(struct WStream *ws) {
	/* копипаста из RStream__findFirstChunk */
	DIR *d;
	struct dirent *e;
	uint64_t mts;

	debug("Scanning '%s' for last chunk", ws->rootDir);

	d = opendir(ws->rootDir);
	if(!d)
		error("opendir(%s)", ws->rootDir);

	while((e = readdir(d))) {
		if(e->d_name[0] == '.' || strstr(e->d_name, ".chunk") != e->d_name + (strlen(e->d_name) - 6))
			continue;

		if(!sscanf(e->d_name, "%" PRIu64, &mts)) {
			warning("unable to parse chunk filename: %s", e->d_name);
			continue;
		}

		if(mts > ws->lastChunkTimemicro)
			ws->lastChunkTimemicro = mts;
	}

	debug("Last microtimestamp: %" PRIu64, ws->lastChunkTimemicro);

	closedir(d);
}

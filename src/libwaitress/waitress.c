/*
Copyright (c) 2009-2011
	Lars-Dominik Braun <lars@6xq.net>

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in
all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
THE SOFTWARE.
*/

#ifndef __FreeBSD__
#define _POSIX_C_SOURCE 1 /* required by getaddrinfo() */
#define _BSD_SOURCE /* snprintf() */
#define _DARWIN_C_SOURCE /* snprintf() on OS X */
#endif

#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <string.h>
#include <unistd.h>
#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <fcntl.h>
#include <poll.h>
#include <errno.h>
#include <assert.h>

#include "config.h"
#include "waitress.h"

#define streq(a,b) (strcmp(a,b) == 0)
#define WAITRESS_HTTP_VERSION "1.1"

typedef struct {
	char *data;
	size_t pos;
} WaitressFetchBufCbBuffer_t;

void WaitressInit (WaitressHandle_t *waith) {
	memset (waith, 0, sizeof (*waith));
	waith->socktimeout = 30000;
}

void WaitressFree (WaitressHandle_t *waith) {
	free (waith->url.url);
	free (waith->proxy.url);
	memset (waith, 0, sizeof (*waith));
}

/*	Proxy set up?
 *	@param Waitress handle
 *	@return true|false
 */
bool WaitressProxyEnabled (const WaitressHandle_t *waith) {
	return waith->proxy.host != NULL;
}

/*	urlencode post-data
 *	@param encode this
 *	@return malloc'ed encoded string, don't forget to free it
 */
char *WaitressUrlEncode (const char *in) {
	size_t inLen = strlen (in);
	/* worst case: encode all characters */
	char *out = calloc (inLen * 3 + 1, sizeof (*in));
	const char *inPos = in;
	char *outPos = out;

	while (inPos - in < inLen) {
		if (!isalnum (*inPos) && *inPos != '_' && *inPos != '-' && *inPos != '.') {
			*outPos++ = '%';
			snprintf (outPos, 3, "%02x", *inPos & 0xff);
			outPos += 2;
		} else {
			/* copy character */
			*outPos++ = *inPos;
		}
		++inPos;
	}

	return out;
}

/*	base64 encode data
 *	@param encode this
 *	@return malloc'ed string
 */
static char *WaitressBase64Encode (const char *in) {
	assert (in != NULL);

	size_t inLen = strlen (in);
	char *out, *outPos;
	const char *inPos;
	static const char *alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
			"abcdefghijklmnopqrstuvwxyz0123456789+/";
	const size_t alphabetLen = strlen (alphabet);

	/* worst case is 1.333 */
	out = malloc ((inLen * 2 + 1) * sizeof (*out));
	if (out == NULL) {
		return NULL;
	}
	outPos = out;
	inPos = in;

	while (inLen >= 3) {
		uint8_t idx;

		idx = ((*inPos) >> 2) & 0x3f;
		assert (idx < alphabetLen);
		*outPos = alphabet[idx];
		++outPos;

		idx = ((*inPos) & 0x3) << 4;
		++inPos;
		idx |= ((*inPos) >> 4) & 0xf;
		assert (idx < alphabetLen);
		*outPos = alphabet[idx];
		++outPos;

		idx = ((*inPos) & 0xf) << 2;
		++inPos;
		idx |= ((*inPos) >> 6) & 0x3;
		assert (idx < alphabetLen);
		*outPos = alphabet[idx];
		++outPos;

		idx = (*inPos) & 0x3f;
		++inPos;
		assert (idx < alphabetLen);
		*outPos = alphabet[idx];
		++outPos;

		inLen -= 3;
	}

	switch (inLen) {
		case 2: {
			uint8_t idx;

			idx = ((*inPos) >> 2) & 0x3f;
			assert (idx < alphabetLen);
			*outPos = alphabet[idx];
			++outPos;

			idx = ((*inPos) & 0x3) << 4;
			++inPos;
			idx |= ((*inPos) >> 4) & 0xf;
			assert (idx < alphabetLen);
			*outPos = alphabet[idx];
			++outPos;

			idx = ((*inPos) & 0xf) << 2;
			assert (idx < alphabetLen);
			*outPos = alphabet[idx];
			++outPos;

			*outPos = '=';
			++outPos;
			break;
		}

		case 1: {
			uint8_t idx;

			idx = ((*inPos) >> 2) & 0x3f;
			assert (idx < alphabetLen);
			*outPos = alphabet[idx];
			++outPos;

			idx = ((*inPos) & 0x3) << 4;
			assert (idx < alphabetLen);
			*outPos = alphabet[idx];
			++outPos;

			*outPos = '=';
			++outPos;

			*outPos = '=';
			++outPos;
			break;
		}
	}
	*outPos = '\0';

	return out;
}

/*	Split http url into host, port and path
 *	@param url
 *	@param returned url struct
 *	@return url is a http url? does not say anything about its validity!
 */
static bool WaitressSplitUrl (const char *inurl, WaitressUrl_t *retUrl) {
	assert (inurl != NULL);
	assert (retUrl != NULL);

	static const char *httpPrefix = "http://";
	
	/* is http url? */
	if (strncmp (httpPrefix, inurl, strlen (httpPrefix)) == 0) {
		enum {FIND_USER, FIND_PASS, FIND_HOST, FIND_PORT, FIND_PATH, DONE}
				state = FIND_USER, newState = FIND_USER;
		char *url, *urlPos, *assignStart;
		const char **assign = NULL;

		url = strdup (inurl);
		retUrl->url = url;

		urlPos = url + strlen (httpPrefix);
		assignStart = urlPos;

		if (*urlPos == '\0') {
			state = DONE;
		}

		while (state != DONE) {
			const char c = *urlPos;

			switch (state) {
				case FIND_USER: {
					if (c == ':') {
						assign = &retUrl->user;
						newState = FIND_PASS;
					} else if (c == '@') {
						assign = &retUrl->user;
						newState = FIND_HOST;
					} else if (c == '/') {
						/* not a user */
						assign = &retUrl->host;
						newState = FIND_PATH;
					} else if (c == '\0') {
						assign = &retUrl->host;
						newState = DONE;
					}
					break;
				}

				case FIND_PASS: {
					if (c == '@') {
						assign = &retUrl->password;
						newState = FIND_HOST;
					} else if (c == '/') {
						/* not a password */
						assign = &retUrl->port;
						newState = FIND_PATH;
					} else if (c == '\0') {
						assign = &retUrl->port;
						newState = DONE;
					}
					break;
				}

				case FIND_HOST: {
					if (c == ':') {
						assign = &retUrl->host;
						newState = FIND_PORT;
					} else if (c == '/') {
						assign = &retUrl->host;
						newState = FIND_PATH;
					} else if (c == '\0') {
						assign = &retUrl->host;
						newState = DONE;
					}
					break;
				}

				case FIND_PORT: {
					if (c == '/') {
						assign = &retUrl->port;
						newState = FIND_PATH;
					} else if (c == '\0') {
						assign = &retUrl->port;
						newState = DONE;
					}
					break;
				}

				case FIND_PATH: {
					if (c == '\0') {
						assign = &retUrl->path;
						newState = DONE;
					}
					break;
				}

				case DONE:
					break;
			} /* end switch */

			if (assign != NULL) {
				*assign = assignStart;
				*urlPos = '\0';
				assignStart = urlPos+1;

				state = newState;
				assign = NULL;
			}

			++urlPos;
		} /* end while */

		/* fixes for our state machine logic */
		if (retUrl->user != NULL && retUrl->host == NULL && retUrl->port != NULL) {
			retUrl->host = retUrl->user;
			retUrl->user = NULL;
		}
		return true;
	} /* end if strncmp */

	return false;
}

/*	Parse url and set host, port, path
 *	@param Waitress handle
 *	@param url: protocol://host:port/path
 */
bool WaitressSetUrl (WaitressHandle_t *waith, const char *url) {
	return WaitressSplitUrl (url, &waith->url);
}

/*	Set http proxy
 *	@param waitress handle
 *  @param url, e.g. http://proxy:80/
 */
bool WaitressSetProxy (WaitressHandle_t *waith, const char *url) {
	return WaitressSplitUrl (url, &waith->proxy);
}

/*	Callback for WaitressFetchBuf, appends received data to \0-terminated
 *	buffer
 *	@param received data
 *	@param data size
 *	@param buffer structure
 */
static WaitressCbReturn_t WaitressFetchBufCb (void *recvData, size_t recvDataSize,
		void *extraData) {
	char *recvBytes = recvData;
	WaitressFetchBufCbBuffer_t *buffer = extraData;

	if (buffer->data == NULL) {
		if ((buffer->data = malloc (sizeof (*buffer->data) *
				(recvDataSize + 1))) == NULL) {
			return WAITRESS_CB_RET_ERR;
		}
	} else {
		char *newbuf;
		if ((newbuf = realloc (buffer->data,
				sizeof (*buffer->data) *
				(buffer->pos + recvDataSize + 1))) == NULL) {
			free (buffer->data);
			return WAITRESS_CB_RET_ERR;
		}
		buffer->data = newbuf;
	}
	memcpy (buffer->data + buffer->pos, recvBytes, recvDataSize);
	buffer->pos += recvDataSize;
	buffer->data[buffer->pos] = '\0';

	return WAITRESS_CB_RET_OK;
}

/*	Fetch string. Beware! This overwrites your waith->data pointer
 *	@param waitress handle
 *	@param \0-terminated result buffer, malloced (don't forget to free it
 *			yourself)
 */
WaitressReturn_t WaitressFetchBuf (WaitressHandle_t *waith, char **retBuffer) {
	WaitressFetchBufCbBuffer_t buffer;
	WaitressReturn_t wRet;

	memset (&buffer, 0, sizeof (buffer));

	waith->data = &buffer;
	waith->callback = WaitressFetchBufCb;

	wRet = WaitressFetchCall (waith);
	*retBuffer = buffer.data;
	return wRet;
}

/*	poll wrapper that retries after signal interrupts, required for socksify
 *	wrapper
 */
static int WaitressPollLoop (struct pollfd *fds, nfds_t nfds, int timeout) {
	int pollres = -1;
	int pollerr = 0;

	do {
		pollres = poll (fds, nfds, timeout);
		pollerr = errno;
		errno = 0;
	} while (pollerr == EINTR || pollerr == EINPROGRESS || pollerr == EAGAIN);

	return pollres;
}

/*	write () wrapper with poll () timeout
 *	@param socket fd
 *	@param write buffer
 *	@param write count bytes
 *	@param reuse existing pollfd structure
 *	@param timeout (microseconds)
 *	@return WAITRESS_RET_OK, WAITRESS_RET_TIMEOUT or WAITRESS_RET_ERR
 */
static WaitressReturn_t WaitressPollWrite (int sockfd, const char *buf, size_t count,
		struct pollfd *sockpoll, int timeout) {
	int pollres = -1;

	sockpoll->events = POLLOUT;
	pollres = WaitressPollLoop (sockpoll, 1, timeout);
	if (pollres == 0) {
		return WAITRESS_RET_TIMEOUT;
	} else if (pollres == -1) {
		return WAITRESS_RET_ERR;
	}
	if (write (sockfd, buf, count) == -1) {
		return WAITRESS_RET_ERR;
	}
	return WAITRESS_RET_OK;
}

/*	read () wrapper with poll () timeout
 *	@param socket fd
 *	@param write to this buf, not NULL terminated
 *	@param buffer size
 *	@param reuse existing pollfd struct
 *	@param timeout (in microseconds)
 *	@param read () return value/written bytes
 *	@return WAITRESS_RET_OK, WAITRESS_RET_TIMEOUT, WAITRESS_RET_ERR
 */
static WaitressReturn_t WaitressPollRead (int sockfd, char *buf, size_t count,
		struct pollfd *sockpoll, int timeout, ssize_t *retSize) {
	int pollres = -1;

	sockpoll->events = POLLIN;
	pollres = WaitressPollLoop (sockpoll, 1, timeout);
	if (pollres == 0) {
		return WAITRESS_RET_TIMEOUT;
	} else if (pollres == -1) {
		return WAITRESS_RET_ERR;
	}
	if ((*retSize = read (sockfd, buf, count)) == -1) {
		return WAITRESS_RET_READ_ERR;
	}
	return WAITRESS_RET_OK;
}

/*	send basic http authorization
 *	@param waitress handle
 *	@param url containing user/password
 *	@param header name prefix
 */
static bool WaitressFormatAuthorization (WaitressHandle_t *waith,
		WaitressUrl_t *url, const char *prefix, char *writeBuf,
		const size_t writeBufSize) {
	if (url->user != NULL) {
		char userPass[1024], *encodedUserPass;
		snprintf (userPass, sizeof (userPass), "%s:%s", url->user,
				(url->password != NULL) ? url->password : "");
		encodedUserPass = WaitressBase64Encode (userPass);
		snprintf (writeBuf, writeBufSize, "%sAuthorization: Basic %s\r\n",
				prefix, encodedUserPass);
		free (encodedUserPass);
		return true;
	}
	return false;
}

/*	get default http port if none was given
 */
static const char *WaitressDefaultPort (WaitressUrl_t *url) {
	return url->port == NULL ? "80" : url->port;
}

/*	get line from string
 *	@param string beginning/return value of last call
 *	@return start of _next_ line or NULL if there is no next line
 */
static char *WaitressGetline (char * const str) {
	char *eol;

	eol = strchr (str, '\n');
	if (eol == NULL) {
		return NULL;
	}

	/* make lines parseable by string routines */
	*eol = '\0';
	if (eol-1 >= str && *(eol-1) == '\r') {
		*(eol-1) = '\0';
	}
	/* skip \0 */
	++eol;

	return eol;
}

/*	identity encoding handler
 */
static WaitressCbReturn_t WaitressHandleIdentity (WaitressHandle_t *waith,
		char *buf, size_t size) {
	waith->request.contentReceived += size;
	return waith->callback (buf, size, waith->data);
}

/*	chunked encoding handler. buf must be \0-terminated, size does not include
 *	trailing \0.
 */
static WaitressCbReturn_t WaitressHandleChunked (WaitressHandle_t *waith,
		char *buf, size_t size) {
	char *content = buf, *nextContent;

	while (1) {
		if (waith->request.chunkSize > 0) {
			size_t remaining = size-(content-buf);

			if (remaining >= waith->request.chunkSize) {
				WaitressHandleIdentity (waith, content, waith->request.chunkSize);
				/* FIXME: skip trailing \r\n */
				content += waith->request.chunkSize+2;
				waith->request.chunkSize = 0;
			} else {
				WaitressHandleIdentity (waith, content, remaining);
				waith->request.chunkSize -= remaining;
				return WAITRESS_CB_RET_OK;
			}
		}

		if ((nextContent = WaitressGetline (content)) != NULL) {
			long int chunkSize = strtol (content, NULL, 16);
			if (chunkSize == 0) {
				return WAITRESS_CB_RET_OK;
			} else if (chunkSize < 0) {
				return WAITRESS_CB_RET_ERR;
			} else {
				waith->request.chunkSize = chunkSize;
				content = nextContent;
			}
		} else {
			return WAITRESS_CB_RET_OK;
		}
	}

	return WAITRESS_CB_RET_OK;
}

/*	handle http header
 */
static void WaitressHandleHeader (WaitressHandle_t *waith, const char * const key,
		const char * const value) {
	if (streq (key, "Content-Length")) {
		waith->request.contentLength = atol (value);
	} else if (streq (key, "Transfer-Encoding")) {
		if (streq (value, "chunked")) {
			waith->request.dataHandler = WaitressHandleChunked;
		}
	}
}

/*	parse http status line and return status code
 */
static int WaitressParseStatusline (const char * const line) {
	char status[4] = "000";

	if (sscanf (line, "HTTP/1.%*1[0-9] %3[0-9] ", status) == 1) {
		return atoi (status);
	}
	return -1;
}

/*	Receive data from host and call *callback ()
 *	@param waitress handle
 *	@return WaitressReturn_t
 */
WaitressReturn_t WaitressFetchCall (WaitressHandle_t *waith) {
/* FIXME: compiler macros are ugly... */
#define FINISH(ret) wRet = ret; goto finish;
#define WRITE_RET(buf, count) \
		if ((wRet = WaitressPollWrite (sockfd, buf, count, \
				&sockpoll, waith->socktimeout)) != WAITRESS_RET_OK) { \
			FINISH (wRet); \
		}
#define READ_RET(buf, count, size) \
		if ((wRet = WaitressPollRead (sockfd, buf, count, \
				&sockpoll, waith->socktimeout, size)) != WAITRESS_RET_OK) { \
			FINISH (wRet); \
		}

	struct addrinfo hints, *res;
	int sockfd;
	char *buf = NULL;
	ssize_t recvSize = 0;
	WaitressReturn_t wRet = WAITRESS_RET_OK;
	struct pollfd sockpoll;
	int pollres;
	/* header parser vars */
	char *nextLine = NULL, *thisLine = NULL;
	enum {HDRM_HEAD, HDRM_LINES, HDRM_FINISHED} hdrParseMode = HDRM_HEAD;
	unsigned int bufFilled = 0;

	/* initialize */
	memset (&waith->request, 0, sizeof (waith->request));
	waith->request.dataHandler = WaitressHandleIdentity;
	memset (&hints, 0, sizeof hints);

	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;

	/* Use proxy? */
	if (WaitressProxyEnabled (waith)) {
		if (getaddrinfo (waith->proxy.host,
				WaitressDefaultPort (&waith->proxy), &hints, &res) != 0) {
			return WAITRESS_RET_GETADDR_ERR;
		}
	} else {
		if (getaddrinfo (waith->url.host,
				WaitressDefaultPort (&waith->url), &hints, &res) != 0) {
			return WAITRESS_RET_GETADDR_ERR;
		}
	}

	if ((sockfd = socket (res->ai_family, res->ai_socktype, res->ai_protocol)) == -1) {
		freeaddrinfo (res);
		return WAITRESS_RET_SOCK_ERR;
	}
	sockpoll.fd = sockfd;

	/* we need shorter timeouts for connect() */
	fcntl (sockfd, F_SETFL, O_NONBLOCK);

	/* increase socket receive buffer */
	const int sockopt = 256*1024;
	setsockopt (sockfd, SOL_SOCKET, SO_RCVBUF, &sockopt, sizeof (sockopt));

	/* non-blocking connect will return immediately */
	connect (sockfd, res->ai_addr, res->ai_addrlen);

	sockpoll.events = POLLOUT;
	pollres = WaitressPollLoop (&sockpoll, 1, waith->socktimeout);
	freeaddrinfo (res);
	if (pollres == 0) {
		return WAITRESS_RET_TIMEOUT;
	} else if (pollres == -1) {
		return WAITRESS_RET_ERR;
	}
	/* check connect () return value */
	socklen_t pollresSize = sizeof (pollres);
	getsockopt (sockfd, SOL_SOCKET, SO_ERROR, &pollres, &pollresSize);
	if (pollres != 0) {
		return WAITRESS_RET_CONNECT_REFUSED;
	}

	const char *path = waith->url.path;
	if (waith->url.path == NULL) {
		/* avoid NULL pointer deref */
		path = "";
	} else if (waith->url.path[0] == '/') {
		/* most servers don't like "//" */
		++path;
	}

	buf = malloc (WAITRESS_BUFFER_SIZE * sizeof (*buf));
	/* send request */
	if (WaitressProxyEnabled (waith)) {
		snprintf (buf, WAITRESS_BUFFER_SIZE,
			"%s http://%s:%s/%s HTTP/" WAITRESS_HTTP_VERSION "\r\n",
			(waith->method == WAITRESS_METHOD_GET ? "GET" : "POST"),
			waith->url.host,
			WaitressDefaultPort (&waith->url), path);
	} else {
		snprintf (buf, WAITRESS_BUFFER_SIZE,
			"%s /%s HTTP/" WAITRESS_HTTP_VERSION "\r\n",
			(waith->method == WAITRESS_METHOD_GET ? "GET" : "POST"),
			path);
	}
	WRITE_RET (buf, strlen (buf));

	snprintf (buf, WAITRESS_BUFFER_SIZE,
			"Host: %s\r\nUser-Agent: " PACKAGE "\r\nConnection: Close\r\n",
			waith->url.host);
	WRITE_RET (buf, strlen (buf));

	if (waith->method == WAITRESS_METHOD_POST && waith->postData != NULL) {
		snprintf (buf, WAITRESS_BUFFER_SIZE, "Content-Length: %zu\r\n",
				strlen (waith->postData));
		WRITE_RET (buf, strlen (buf));
	}

	/* write authorization headers */
	if (WaitressFormatAuthorization (waith, &waith->url, "", buf,
			WAITRESS_BUFFER_SIZE)) {
		WRITE_RET (buf, strlen (buf));
	}
	if (WaitressFormatAuthorization (waith, &waith->proxy, "Proxy-",
			buf, WAITRESS_BUFFER_SIZE)) {
		WRITE_RET (buf, strlen (buf));
	}
	
	if (waith->extraHeaders != NULL) {
		WRITE_RET (waith->extraHeaders, strlen (waith->extraHeaders));
	}
	
	WRITE_RET ("\r\n", 2);

	if (waith->method == WAITRESS_METHOD_POST && waith->postData != NULL) {
		WRITE_RET (waith->postData, strlen (waith->postData));
	}

	/* receive answer */
	nextLine = buf;
	while (hdrParseMode != HDRM_FINISHED) {
		READ_RET (buf+bufFilled, WAITRESS_BUFFER_SIZE-1 - bufFilled, &recvSize);
		if (recvSize == 0) {
			/* connection closed too early */
			FINISH (WAITRESS_RET_CONNECTION_CLOSED);
		}
		bufFilled += recvSize;
		buf[bufFilled] = '\0';
		thisLine = buf;

		/* split */
		while (hdrParseMode != HDRM_FINISHED &&
				(nextLine = WaitressGetline (thisLine)) != NULL) {
			switch (hdrParseMode) {
				/* Status code */
				case HDRM_HEAD:
					switch (WaitressParseStatusline (thisLine)) {
						case 200:
						case 206:
							hdrParseMode = HDRM_LINES;
							break;

						case 403:
							FINISH(WAITRESS_RET_FORBIDDEN);
							break;

						case 404:
							FINISH(WAITRESS_RET_NOTFOUND);
							break;

						case -1:
							/* ignore invalid line */
							break;

						default:
							FINISH (WAITRESS_RET_STATUS_UNKNOWN);
							break;
					}
					break;

				/* Everything else, except status code */
				case HDRM_LINES:
					/* empty line => content starts here */
					if (*thisLine == '\0') {
						hdrParseMode = HDRM_FINISHED;
					} else {
						/* parse header: "key: value", ignore invalid lines */
						char *key = thisLine, *val;

						val = strchr (thisLine, ':');
						if (val != NULL) {
							*val++ = '\0';
							while (*val != '\0' && isspace ((unsigned char) *val)) {
								++val;
							}
							WaitressHandleHeader (waith, key, val);
						}
					}
					break;

				default:
					break;
			} /* end switch */
			thisLine = nextLine;
		} /* end while strchr */
		memmove (buf, thisLine, bufFilled-(thisLine-buf));
		bufFilled -= (thisLine-buf);
	} /* end while hdrParseMode */

	/* push remaining bytes */
	if (bufFilled > 0) {
		/* data must be \0-terminated for chunked handler */
		buf[bufFilled] = '\0';
		if (waith->request.dataHandler (waith, buf, bufFilled) ==
				WAITRESS_CB_RET_ERR) {
			FINISH (WAITRESS_RET_CB_ABORT);
		}
	}

	/* receive content */
	do {
		READ_RET (buf, WAITRESS_BUFFER_SIZE-1, &recvSize);
		buf[recvSize] = '\0';
		if (recvSize > 0) {
			if (waith->request.dataHandler (waith, buf, recvSize) ==
					WAITRESS_CB_RET_ERR) {
				wRet = WAITRESS_RET_CB_ABORT;
				break;
			}
		}
	} while (recvSize > 0);

finish:
	close (sockfd);
	free (buf);

	if (wRet == WAITRESS_RET_OK &&
			waith->request.contentReceived < waith->request.contentLength) {
		return WAITRESS_RET_PARTIAL_FILE;
	}
	return wRet;

#undef FINISH
#undef WRITE_RET
#undef READ_RET
}

const char *WaitressErrorToStr (WaitressReturn_t wRet) {
	switch (wRet) {
		case WAITRESS_RET_OK:
			return "Everything's fine :)";
			break;

		case WAITRESS_RET_ERR:
			return "Unknown.";
			break;

		case WAITRESS_RET_STATUS_UNKNOWN:
			return "Unknown HTTP status code.";
			break;

		case WAITRESS_RET_NOTFOUND:
			return "File not found.";
			break;
		
		case WAITRESS_RET_FORBIDDEN:
			return "Forbidden.";
			break;

		case WAITRESS_RET_CONNECT_REFUSED:
			return "Connection refused.";
			break;

		case WAITRESS_RET_SOCK_ERR:
			return "Socket error.";
			break;

		case WAITRESS_RET_GETADDR_ERR:
			return "getaddr failed.";
			break;

		case WAITRESS_RET_CB_ABORT:
			return "Callback aborted request.";
			break;

		case WAITRESS_RET_PARTIAL_FILE:
			return "Partial file.";
			break;
	
		case WAITRESS_RET_TIMEOUT:
			return "Timeout.";
			break;

		case WAITRESS_RET_READ_ERR:
			return "Read error.";
			break;

		case WAITRESS_RET_CONNECTION_CLOSED:
			return "Connection closed by remote host.";
			break;

		default:
			return "No error message available.";
			break;
	}
}

#ifdef TEST
/* test cases for libwaitress */

#include <stdio.h>
#include <stdbool.h>
#include <string.h>
#include "waitress.h"

/*	string equality test (memory location or content)
 */
static bool streqtest (const char *x, const char *y) {
	return (x == y) || (x != NULL && y != NULL && streq (x, y));
}

/*	test WaitressSplitUrl
 *	@param tested url
 *	@param expected user
 *	@param expected password
 *	@param expected host
 *	@param expected port
 *	@param expected path
 */
static void compareUrl (const char *url, const char *user,
		const char *password, const char *host, const char *port,
		const char *path) {
	WaitressUrl_t splitUrl;

	memset (&splitUrl, 0, sizeof (splitUrl));

	WaitressSplitUrl (url, &splitUrl);

	bool userTest, passwordTest, hostTest, portTest, pathTest, overallTest;

	userTest = streqtest (splitUrl.user, user);
	passwordTest = streqtest (splitUrl.password, password);
	hostTest = streqtest (splitUrl.host, host);
	portTest = streqtest (splitUrl.port, port);
	pathTest = streqtest (splitUrl.path, path);

	overallTest = userTest && passwordTest && hostTest && portTest && pathTest;

	if (!overallTest) {
		printf ("FAILED test(s) for %s\n", url);
		if (!userTest) {
			printf ("user: %s vs %s\n", splitUrl.user, user);
		}
		if (!passwordTest) {
			printf ("password: %s vs %s\n", splitUrl.password, password);
		}
		if (!hostTest) {
			printf ("host: %s vs %s\n", splitUrl.host, host);
		}
		if (!portTest) {
			printf ("port: %s vs %s\n", splitUrl.port, port);
		}
		if (!pathTest) {
			printf ("path: %s vs %s\n", splitUrl.path, path);
		}
	} else {
		printf ("OK for %s\n", url);
	}
}

/*	compare two strings
 */
void compareStr (const char *result, const char *expected) {
	if (!streq (result, expected)) {
		printf ("FAIL for %s, result was %s\n", expected, result);
	} else {
		printf ("OK for %s\n", expected);
	}
}

/*	test entry point
 */
int main () {
	/* WaitressSplitUrl tests */
	compareUrl ("http://www.example.com/", NULL, NULL, "www.example.com", NULL,
			"");
	compareUrl ("http://www.example.com", NULL, NULL, "www.example.com", NULL,
			NULL);
	compareUrl ("http://www.example.com:80/", NULL, NULL, "www.example.com",
			"80", "");
	compareUrl ("http://www.example.com:/", NULL, NULL, "www.example.com", "",
			"");
	compareUrl ("http://:80/", NULL, NULL, "", "80", "");
	compareUrl ("http://www.example.com/foobar/barbaz", NULL, NULL,
			"www.example.com", NULL, "foobar/barbaz");
	compareUrl ("http://www.example.com:80/foobar/barbaz", NULL, NULL,
			"www.example.com", "80", "foobar/barbaz");
	compareUrl ("http://foo:bar@www.example.com:80/foobar/barbaz", "foo", "bar",
			"www.example.com", "80", "foobar/barbaz");
	compareUrl ("http://foo:@www.example.com:80/foobar/barbaz", "foo", "",
			"www.example.com", "80", "foobar/barbaz");
	compareUrl ("http://foo@www.example.com:80/foobar/barbaz", "foo", NULL,
			"www.example.com", "80", "foobar/barbaz");
	compareUrl ("http://:foo@www.example.com:80/foobar/barbaz", "", "foo",
			"www.example.com", "80", "foobar/barbaz");
	compareUrl ("http://:@:80", "", "", "", "80", NULL);
	compareUrl ("http://", NULL, NULL, NULL, NULL, NULL);
	compareUrl ("http:///", NULL, NULL, "", NULL, "");
	compareUrl ("http://foo:bar@", "foo", "bar", "", NULL, NULL);

	/* WaitressBase64Encode tests */
	compareStr (WaitressBase64Encode ("M"), "TQ==");
	compareStr (WaitressBase64Encode ("Ma"), "TWE=");
	compareStr (WaitressBase64Encode ("Man"), "TWFu");
	compareStr (WaitressBase64Encode ("The quick brown fox jumped over the lazy dog."),
			"VGhlIHF1aWNrIGJyb3duIGZveCBqdW1wZWQgb3ZlciB0aGUgbGF6eSBkb2cu");
	compareStr (WaitressBase64Encode ("The quick brown fox jumped over the lazy dog"),
			"VGhlIHF1aWNrIGJyb3duIGZveCBqdW1wZWQgb3ZlciB0aGUgbGF6eSBkb2c=");
	compareStr (WaitressBase64Encode ("The quick brown fox jumped over the lazy do"),
			"VGhlIHF1aWNrIGJyb3duIGZveCBqdW1wZWQgb3ZlciB0aGUgbGF6eSBkbw==");

	return EXIT_SUCCESS;
}
#endif /* TEST */


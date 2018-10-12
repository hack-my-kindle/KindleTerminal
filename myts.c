/*

Copyright (c) 2010 Luigi Rizzo. All rights reserved.

Redistribution and use in source and binary forms, with or without
modification, are permitted provided that the following conditions
are met:
1. Redistributions of source code must retain the above copyright
   notice, this list of conditions and the following disclaimer.
2. Redistributions in binary form must reproduce the above copyright
   notice, this list of conditions and the following disclaimer in the
   documentation and/or other materials provided with the distribution.

THIS SOFTWARE IS PROVIDED BY THE AUTHOR AND CONTRIBUTORS ``AS IS'' AND
ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
ARE DISCLAIMED.  IN NO EVENT SHALL THE AUTHOR OR CONTRIBUTORS BE LIABLE
FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
SUCH DAMAGE.

 */
/*

 * $Id: myts.c 7657 2010-11-05 04:38:48Z luigi $


Backend for ajaxterm

The main instance of a program keeps a list of current sessions,
and for each of them handles a child which runs a shell and
talks through a pty pair.

In standalone mode, the process runs as a web server and so it
can suspend requests for some time until they are handled.

In slave1 mode, it talks through a single unix pipe so it cannot suspend.

In slave2 mode it uses multiple unix pipes.

 */

#include <stdio.h>
#include <stdlib.h>
#include <memory.h>
#include <signal.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <fcntl.h>
#ifdef linux
#include <string.h>	/* strcasestr */
/* strcasestr prototype is problematic */
char *strcasestr(const char *haystack, const char *pneedle);
#include <pty.h>
#else
#include <libutil.h>	/* forkpty */
#endif
#include <sys/time.h>	/* gettimeofday */
#include <errno.h>
#include <sys/socket.h>
#include <sys/mman.h>	/* PROT_READ and mmap */
#include <netinet/in.h>
#include <netdb.h>	/* gethostbyname */
#include <ctype.h>	/* isalnum */
#include <arpa/inet.h>	/* inet_aton */

#define KMAX	256	/* keyboard queue */
#define SMAX	256	/* keyboard queue */
#define	ROWS	25
#define	COLS	80
#define	INBUFSZ	4096	/* GET/POST queries */
#define	OUTBUFSZ (5*ROWS*COLS)	/* output buffer */

/* supported mime types -- suffix space mime. Default is text/plain.
 * processed by getmime(filename)
 */
static const char *mime_types[] = {
    "html text/html",
    "htm text/html",
    "js text/javascript",
    "css text/css",
    NULL
};
/*
 * struct my_sock contains support for talking to the browser.
 * It contains a buffer for receiving the incoming request,
 * hold a copy of the response, and support for an mmapped file.
 * Initially, reply = 0, len = sizeof(inbuf) and pos = 0,
 * we accumulate data into inbuf and call parse_msg() to tell
 * whether we are done.
 * During the reply phase, inbuf contains the response hdr,
 * possibly map contains the body, len = header length, body_len = body_len
 * and pos = 0. We first send the header, then toggle len = -body_len,
 * and then send the body. When done, detach the buffer.
 * If filep is set, then map is a mapped file, otherwise points to
 * some other buffer and does not need to be freed.
 */
struct my_sock {
	struct my_sock *next;
	int socket;		/* the network socket */
	int reply;		/* set when replying */
	int pos, len;		/* read position and length */
	struct sockaddr_in sa;	/* not used */
	char inbuf[INBUFSZ];	/* I/O buffer */
	char outbuf[OUTBUFSZ];	/* I/O buffer */

	/* memory mapped file */
	int filep;
	int body_len;
	char *map;
};

/*
 * struct my_sess describes a shell session to which we talk.
 */
struct my_sess {
	struct my_sess *next;
	char *name;	/* session name */
	int pid;	/* pid of the child */
	int master;	/* master tty */

	/* screen/keyboard buf have len *pos. *pos is the next byte to send */
	int kseq;	// need a sequence number for kb input ?
	int klen;	/* pending input for keyboard */
	char keys[KMAX];
	int slen;	/* pending input for screen */
	char sbuf[SMAX];

	int rows, cols;	/* geometry */
	int cur;
	char *page;	/* dump of the screen */
	char *oldpage;	/* dump of the screen */
};

/*
 * my_args contains all the arguments for the program
 */
struct my_args {
	struct sockaddr_in sa;
	char *cmd;	/* command to run */
	int nsess;	/* number of sessions */
	int lfd;	/* listener fd */
	struct my_sock *socks;
	struct my_sess *sess;
	int cycles;
	int unsafe;	/* allow read all file systems */
	int verbose;	/* allow read all file systems */
};

int myerr(const char *s)
{
    fprintf(stderr, "error: %s\n", s);
    exit(2);
}

/* convert the html encoding back to plain ascii
 * XXX must be fixed to handle UTF8
 */
char *unescape(char *s)
{
    char *src, *dst, c, *hex = "0123456789abcdef0123456789ABCDEF";
    for (src = dst = s; *src; ) {
	c = *src++;
	if (c == '+') c = ' ';
	else if (c == '%') {
	    c = '\0';
	    if (*src && index(hex, *src)) c = c*16 + ((index(hex, *src++)-hex)&0xf);
	    if (*src && index(hex, *src)) c = c*16 + ((index(hex, *src++)-hex)&0xf);
	}
	*dst++ = c;
    }
    *dst++ = '\0';
    return s;
}

/*
 * append a string to a page, interpreting ANSI sequences
 */
char *page_append(struct my_sess *sh, char *s)
{
    int pagelen = sh->rows * sh->cols;
    int curcol;

    for (; *s; s++) {
	char c = *s;
	if (sh->cur >= pagelen) {
	    // fprintf(stderr, "+++ scroll at %d / %d +++\n", sh->cur, pagelen);
	    sh->cur = pagelen - sh->cols;
	    memcpy(sh->page, sh->page + sh->cols, sh->cur);
	    memset(sh->page + pagelen - sh->cols, ' ', sh->cols);
	}
	curcol = sh->cur % sh->cols;
	/* now should map actions */
	if (c == '\r') {
	    sh->cur -= curcol;
	} else if (c == '\n') {
	    sh->cur += sh->cols;
	    if (sh->cur >= pagelen) { // XXX not sure if needed
		// fprintf(stderr, "+++ scroll2 at %d / %d +++\n", sh->cur, pagelen);
		sh->cur -= sh->cols;
		memcpy(sh->page, sh->page + sh->cols, sh->cur);
		memset(sh->page + pagelen - sh->cols, ' ', sh->cols);
	    }
	} else if (c == '\t') {
	    if (curcol >= pagelen - 8)
		sh->cur += (sh->cols - 1 - curcol);
	    else
		sh->cur += 8 - (sh->cur % 8);
	} else if (c == '\b') { // backspace
	    if (curcol > 0)
		    sh->cur--;
	    sh->page[sh->cur] = ' ';
	} else if (c == '\033') { /* escape */
	    if (!s[1])
		break;	// process later
	    if (s[1] == '[' ) { // CSI found
		/* see http://en.wikipedia.org/wiki/ANSI_escape_code */
		char *parm, *base = s + 2, cmd, mark=' ';
		int a1= 1, a2= 1, a3 = 1;
		// fprintf(stderr, "+++ CSI FOUND ESC-%s\n", s+1);
		if (*base == '?')
		    mark = *base++;
		if (!*base)
		    break; // process later
		// skip parameters
		for (parm = base; *parm && index("0123456789;", *parm); parm++) ;
		// fprintf(stderr, "+++ now PARM %s\n", parm);
		cmd = parm[0];
		if (!cmd)
		    return s; // process later
		s = parm;
		sscanf(base, "%d;%d;%d", &a1, &a2, &a3);
		if (cmd == 'A') { // up
		    while (a1-- > 0) {
			    if (sh->cur >= sh->cols) sh->cur -= sh->cols;
		    }
		} else if (cmd == 'B') { // down
		    while (a1-- > 0) {
			    if (sh->cur < pagelen -sh->cols) sh->cur += sh->cols;
		    }
		} else if (cmd == 'C') { // right
		    if (a1 >= sh->cols - curcol) a1 = sh->cols - curcol - 1;
		    sh->cur += a1;
		} else if (cmd == 'D') { // left
		    if (a1 > curcol) a1 = curcol;
		    sh->cur -= a1;
		} else if (cmd == 'H' || cmd == 'f') { // position
		    if (a1 > sh->rows) a1 = sh->rows;
		    if (a2 > sh->cols) a2 = sh->cols;
		    sh->cur = (a1 - 1)*sh->cols + a2 - 1;
		} else if (cmd == 'J') { /* clear part of screen */
		    if (base == parm || a1 == 0) {
			a1 = pagelen - sh->cols;
			memset(sh->page + sh->cur, ' ', a1);
		    } else if (a1 == 1) {
			memset(sh->page, ' ', sh->cur);
		    } else if (a1 == 2) {
			memset(sh->page, ' ', pagelen);
			sh->cur = 0; // msdos ansy.sys
		    } else {
			goto notfound;
		    }
		    
		} else if (cmd == 'K') { /* clear */
		    if (base == parm || a1 == 0) {
			a1 = sh->cols - curcol;
			memset(sh->page + sh->cur, ' ', a1);
		    } else if (a1 == 1) {
			goto notfound;
		    } else if (a1 == 2) {
			goto notfound;
		    } else {
			goto notfound;
		    }
		} else if (mark == '?' && cmd == 'l') { /* hide cursor */
			goto notfound;
		} else if (mark == '?' && cmd == 'h') { /* show cursor */
			goto notfound;
		} else {
notfound:
		    fprintf(stderr, "ANSI sequence %d %d %d ( ESC-[%c%.*s)\n",
			a1, a2, a3, mark, (parm+1 - base), base);	
		}
	    }
	} else {
	    sh->page[sh->cur] = *s;
	    if (curcol != sh->cols -1) sh->cur++;
	}
	if (sh->cur >= pagelen)
	    fprintf(stderr,"--- ouch, overflow on c %d\n", c);
    }
    if (*s) {
	fprintf(stderr, "----- leftover stuff ESC [%s]\n", s+1);
    }
    return s;
}

int forkchild(struct my_sess *s, char *cmd)
{
    struct winsize ws;

    bzero(&ws, sizeof(ws));
    ws.ws_row = s->rows;
    ws.ws_col = s->cols;
    s->pid = forkpty(&s->master, NULL, NULL, &ws);
    // fprintf(stderr, "forkpty gives pid %d pty %d\n", s->pid, s->master);
    if (s->pid < 0) {
	fprintf(stderr, "forkpty failed\n");
	return 1;
    }
    if (s->pid == 0) {	/* execvp the shell */
	char *av[] = { cmd, NULL};
	execvp(av[0], av);
	exit(1);
    }
    return 0;
}

int u_mode(struct my_args *me, struct my_sock *ss, char *body)
{
	/* ajaxterm parameters */
	char *s = NULL, *w = NULL, *h = NULL, *c = NULL, *k = NULL;
	char *cur, *p, *p2;
	struct timeval t;
	char tmp[ROWS*COLS+1];
	char *src, *dst;
	char done;
	int i, l, rows = 0, cols = 0;
	struct my_sess *sh = NULL;

	for (p = body; (cur = strsep(&p, "&")); ) {
	    if (!*cur) continue;
	    p2 = strsep(&cur, "=");
	    if (!strcmp(p2, "s")) s = cur;
	    if (!strcmp(p2, "w")) w = cur;
	    if (!strcmp(p2, "h")) h = cur;
	    if (!strcmp(p2, "c")) c = cur;
	    if (!strcmp(p2, "k")) k = cur;
	}
	if (!s || !*s)
	    goto error;
	if (w) cols = atoi(w);
	if (cols < 10 || cols > 150) cols = 80;
	if (h) rows = atoi(h);
	if (rows < 4 || rows > 80) rows = 25;

	for (i = 0, sh = me->sess; sh; sh = sh->next, i++) {
	    // fprintf(stderr, "at %p have %s\n", sh, sh->name);
	    if (!strcmp(s, sh->name))
		break;
	}
	if (!sh) {
	    // fprintf(stderr, "--- session %s not found %d\n", s, i);
	    int l1 = rows * cols + 1;
	    int l2 = strlen(s) + 1;
	    int pagelen = rows*cols;
	    sh = calloc(1, sizeof(*sh) + l1*2 + l2);
	    if (!sh)
		goto error;
	    sh->rows = rows;
	    sh->cols = cols;
	    sh->cur = 0;

	    sh->name = (char *)(sh + 1);
	    sh->page = sh->name + l2;
	    sh->oldpage = sh->page + l1;
	    memset(sh->page, ' ', pagelen);
	    strcpy(sh->name, s);
	    sh->next = me->sess;
	    me->sess = sh;
	    if (forkchild(sh, me->cmd)) {
		ss->len = sprintf(ss->outbuf,
		    "HTTP/1.1 400 fork failed\r\n\r\n");
		return 0;
	    }
	}
	unescape(k);
	strncat(sh->keys + sh->klen, k, sizeof(sh->keys) - 1 - sh->klen);
	sh->klen = strlen(sh->keys);

	rows = sh->rows;
	cols = sh->cols;
	src = sh->page;
	sh->page[rows*cols] = '\0';	// ensure it is terminated XXX bug in cursor handling
	if (!strcmp(sh->page, sh->oldpage)) {
	    /* no modifications, compact version */
same:
	    ss->len = sprintf(ss->outbuf,
		"HTTP/1.1 200 OK\r\n"
		"Content-Type: text/xml\r\n\r\n"
		"<?xml version=\"1.0\" ?>"
		"<idem></idem>");
	    if (me->verbose) fprintf(stderr, "response %s\n", ss->outbuf);
	    return 0;
	} else {
	    strcpy(sh->oldpage, sh->page);
	}
	goto good;

error:
	goto same;
	rows = ROWS;
	cols = COLS;
	gettimeofday(&t, NULL);
	l = sprintf(tmp, "session %p at %d.%06d pressed %s",
		ss, (int)t.tv_sec, (int)(t.tv_usec), k);
	src = tmp;

good:
	ss->len = sprintf(ss->outbuf,
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/xml\r\n\r\n"
	    "<?xml version=\"1.0\" encoding=\"UTF-8\" ?>"
	    "<pre class=\"term kindle\">");
	done = '\0';

	for (i=0, dst = ss->outbuf + ss->len; i < rows*cols;) {
	    char cc = done ? done : src[i];
	    if (!cc) done = cc = ' ';
	    if (sh && src == sh->page && i == sh->cur)
		dst += sprintf(dst, "<span class=\"b1\">");
	    if (isalnum(cc) || cc == ' ') // XXX
		    *dst++ = cc;
	    else
		dst += sprintf(dst, "%%%02x", cc);
	    if (sh && src == sh->page && i == sh->cur)
		dst += sprintf(dst, "</span>");
	    if (++i % cols == 0)
		*dst++ = '\n';
	}
	l = sprintf(dst, "</pre>");
	ss->len = dst + l - ss->outbuf;
	if (me->verbose) fprintf(stderr, "response %s\n", ss->outbuf);
	return 0;
}

/*
 * HTTP support
 */
int opensock(struct sockaddr_in sa)
{
    int fd;
    int i;

    fd = socket(sa.sin_family, SOCK_STREAM, 0);
    if (fd < 0) {
	perror(" cannot create socket");
	return -1;
    }
    fcntl(fd, F_SETFD, 1 );	// close on exec
    i = 1;
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &i, sizeof(i) ) < 0 ) {
	perror(" cannot reuseaddr");
	return -1;
    }
    if (bind(fd, (struct sockaddr *)&sa, sizeof(struct sockaddr_in))) {
        perror( "bind" );
        return -1;
    }

    if (listen(fd, 20) < 0 ) {
        perror( "listen" );
        return -1;
    }
    return fd;
}

int handle_listen(struct my_args *me)
{
    int fd;
    unsigned int l;
    struct sockaddr_in sa;
    struct my_sock *s;

    if (me->verbose) fprintf(stderr, "listening socket\n");
    bzero(&sa, sizeof(sa));
    l = sizeof(sa);
    fd = accept(me->lfd, (struct sockaddr *)&sa, &l);
    if (fd < 0) {
	fprintf(stderr, "listen failed\n");
	return -1;
    }
    s = calloc(1, sizeof(*s));
    if (!s) {
	close(fd);
	fprintf(stderr, "alloc failed\n");
	return -1;
    }
    s->sa = sa;
    s->socket = fd;
    s->filep = -1;	/* no file */
    s->pos = 0;
    s->len = sizeof(s->inbuf);
    s->next = me->socks;
    me->socks = s;
    return 0;
}

/* support function to return mime types. */
const char *getmime(const char *res)
{
    const char **p, *suffix;
    int lres = strlen(res);

    res += lres;	/* move to the end of the resource */
    for (p = mime_types; (suffix = *p); p++) {
	int lsuff = strcspn(suffix, " ");
	if (lsuff > lres)
	    continue;
	if (!bcmp(res - lsuff, suffix, lsuff))
	    return suffix + lsuff + 1;
    }
    return "text/plain";	/* default */
}

/*
 * A stripped down parser for http, which also interprets what
 * we need to do.
 */
int parse_msg(struct my_args *me, struct my_sock *s)
{
    char *a, *b, *c = s->inbuf;	/* strsep support */
    char *body, *method = NULL, *resource = NULL;
    int row, tok;
    int clen = -1;
    char *err = "generic error";

    if (s->pos == s->len) { // buffer full, we are done
	fprintf(stderr, "--- XXX input buffer full\n");
	s->len = 0; // complete
    }
    /* locate the end of the header. If not found, just return */
    body = strstr(s->inbuf, "\n\r\n") + 3;
    if (body < s->inbuf) body = strstr(s->inbuf, "\n\n") + 2;
    if (body < s->inbuf && s->len) return 0;
    /* quick search for content length */
    a = strcasestr(s->inbuf, "Content-length:");
    if (a && a < body) {
	sscanf(a + strlen("Content-length:") + 1, "%d", &clen);
	if (me->verbose) fprintf(stderr, "content length = %d, body len %d\n",
		clen, s->pos - (body - s->inbuf));
	if (s->pos - (body - s->inbuf) != clen)
		return 0;
    }
    /* no content length, hope body is complete */
    /* XXX maybe do a multipass */

    /* now parse the header */
    for (row=0; (b = strsep(&c, "\r\n"));) {
	if (*b == '\0') continue;
	if (b > body) {
	    body = b;
	    break;
	}
	row++;
        for (tok=0; (a = strsep(&b, " \t"));) {
	    if (*a == '\0') continue;
	    tok++;
	    if (row == 1) {
		if (tok == 1) method = a;
		if (tok == 2) resource = a;
	    }
	}
    }
    s->reply = 1; /* body found, we move to reply mode. */
    if (me->verbose) fprintf(stderr, "%s %s\n", method, resource);
    if (me->verbose) fprintf(stderr, "+++ request body [%s]\n", body);
    s->pos = 0;
    if (!strcmp(method, "POST") && !strcmp(resource, "/u")) {
	/* this is the ajax request */
	return u_mode(me, s, body);
    } else if (!strcmp(method, "GET") && !strncmp(resource, "/u?", 3)) {
	/* same ajax request using GET */
	return u_mode(me, s, resource+3);
    } else {	/* request for a file, map and serve it */
	struct stat sb;

	err = "invalid pathname";
	if (!me->unsafe && resource[1] == '/')
	    goto error;	/* avoid absolute pathnames */
	for (a = resource+1; *a; a++) { /* avoid back pointers */
	    if (*a == '.' && a[1] == '.')
		goto error;
	}
	if (!strcmp(resource, "/"))
	    resource = "/ajaxterm.html";
	s->filep = open(resource+1, O_RDONLY);
	err = "open failed";
	if (s->filep < 0 || fstat(s->filep, &sb))
	    goto error;
	err = "mmap failed";
	/* linux wants MAP_PRIVATE or MAP_SHARED, not 0 */
	s->map = mmap(NULL, (int)sb.st_size, PROT_READ, MAP_PRIVATE, s->filep, (off_t)0);
	if (s->map == MAP_FAILED)
	    goto error;
	s->body_len = sb.st_size;
        s->len = sprintf(s->outbuf,
	    "HTTP/1.1 200 OK\r\n"
	    "Content-Type: %s\r\nContent-Length: %d\r\n\r\n",
		getmime(resource+1), (int)sb.st_size);
	return 0;
    }
error:
    if (s->filep >= 0)
	close(s->filep);
    s->len = sprintf(s->outbuf,
	"HTTP/1.1 200 OK\r\n"
	"Content-Type: text/plain\r\n\r\nResource %s : %s\n", resource, err);
    return 0;
}

/*
 * Handle I/O on the socket talking to the browser.
 * We always use it in half duplex
 */
int sock_io(struct my_args *me, struct my_sock *s)
{
    int l;
    
    if (s->reply) {
	/* first write the header, then set s->len negative and
	 * write the mapped file
	 */
	if (s->len > 0) {
	    l = write(s->socket, s->outbuf + s->pos, s->len - s->pos);
	} else {
	    l = write(s->socket, s->map + s->pos, s->body_len - s->pos);
	}
	if (l <= 0)
	    goto write_done;
	s->pos += l;
        if (me->verbose) fprintf(stderr, "written1 %d/%d\n", s->pos, s->len);
	if (s->pos == s->len) { /* header sent, move to the body */
	    s->len = -s->body_len;
	    s->pos = 0;
	}
        if (me->verbose) fprintf(stderr, "written2 %d/%d\n", s->pos, -s->len);
	if (s->pos == -s->len) { /* body sent, close */
write_done:
	    if (me->verbose) fprintf(stderr, "reply complete\n");
	    /* the kindle wants shutdown before close */
	    shutdown(s->socket, SHUT_RDWR);
	    close(s->socket);
	    s->len = 0;
	    if (s->filep) {
		if (s->map) munmap(s->map, s->body_len);
		close(s->filep);
	    }
	}
    } else { /* accumulate request */
	l = read(s->socket, s->inbuf + s->pos, s->len - s->pos);
	if (me->verbose) fprintf(stderr, "read %p returns %d %s\n", s, l, s->inbuf);
	if (l <= 0) {
	    fprintf(stderr, "buf [%s]\n", s->inbuf);
	    s->len = 0; // mark done with read
	} else {
	    s->pos += l;
	}
	parse_msg(me, s); /* check if msg is complete */
    }
    return 0;
}

int shell_keyboard(struct my_args *me, struct my_sess *sh)
{
    int l;
    l = write(sh->master, sh->keys, sh->klen);
    if (l <= 0)
	return 1;
    strcpy(sh->keys, sh->keys + l);
    sh->klen -= l;
    return 0;
}

/* process screen output from the shell */
int shell_screen(struct my_args *me, struct my_sess *p)
{
    int l, spos;
    char *s;

    spos = strlen(p->sbuf);
    l = read(p->master, p->sbuf + spos, sizeof(p->sbuf) - 1 - spos);
    if (l <= 0) {
        fprintf(stderr, "--- screen gives %d\n", l);
	p->master = -1;
	return 1;
    }
    spos += l;
    p->sbuf[spos] = '\0';
    s = page_append(p, p->sbuf); /* returns unprocessed pointer */
    strcpy(p->sbuf, s);
    return 0;
}

/*
 * Main loop implementing web server and connection handling
 */
int mainloop(struct my_args *me)
{
    fprintf(stderr, "listen on %s:%d\n",
	inet_ntoa(me->sa.sin_addr), ntohs(me->sa.sin_port));
    me->lfd = opensock(me->sa);

    for (;;) {
	int n, nmax;
	struct my_sock *s, *nexts, **ps;
	struct my_sess *p, *nextp, **pp;
	fd_set r, w;
	struct timeval to = { 5, 0 };

	FD_ZERO(&r);
	FD_ZERO(&w);
	FD_SET(me->lfd, &r);
	nmax = me->lfd;
	for (s = me->socks; s; s = s->next) { /* handle sockets */
	    FD_SET(s->socket, s->reply ? &w : &r);
	    if (nmax < s->socket)
		nmax = s->socket;
	}
	for (p = me->sess; p; p = p->next) {	/* handle terminals */
	    FD_SET(p->master, &r);
	    if (nmax < p->master)
		nmax = p->master;
	    if (p->klen)	/* have bytes to send to keyboard */
		FD_SET(p->master, &w);
	}
	n = select(nmax + 1, &r, &w, NULL, &to);
	if (n == 0) {
	    fprintf(stderr, "select returns %d\n", n);
	    continue;
	}
	if (FD_ISSET(me->lfd, &r))
	    handle_listen(me);
	for (ps = &me->socks, s = *ps; s; s = nexts) { /* scan sockets */
	    nexts = s->next;
	    if (FD_ISSET(s->socket, s->reply ? &w : &r))
		sock_io(me, s);
	    if (s->len != 0) { /* socket still active */
		ps = &s->next;
	    } else { /* socket dead, unlink */
		*ps = s->next;
		free(s);
	    }
	}
	for (pp = &me->sess, p = *pp; p ; p = nextp) { /* scan shells */
	    nextp = p->next;
	    if (FD_ISSET(p->master, &w))
		shell_keyboard(me, p);
	    if (p->master >= 0 && FD_ISSET(p->master, &r))
		shell_screen(me, p);
	    if (p->master >= 0) { /* session still active */
		pp = &p->next;
	    } else { /* dead session, unlink */
		*pp = p->next;
		fprintf(stderr, "-- free session %p ---\n", p);
		free(p);
	    }
	}
    }
    return 0;
}

int main(int argc, char *argv[])
{
    struct my_args me;

    bzero(&me, sizeof(me));
    me.sa.sin_family = PF_INET;
    me.sa.sin_port = htons(8022);
    inet_aton("127.0.0.1", &me.sa.sin_addr);
    me.cmd = "login";
    for ( ; argc > 1 ; argc--, argv++) {
	if (!strcmp(argv[1], "--unsafe")) {
	    me.unsafe = 1;
	    continue;
	}
	if (!strcmp(argv[1], "--verbose")) {
	    me.verbose = 1;
	    continue;
	}
	if (argc < 3)
	    break;
	if (!strcmp(argv[1], "--cmd")) {
    	    me.cmd = argv[2];
	    argc--; argv++; continue;
	}
	if (!strcmp(argv[1], "--port")) {
    	    me.sa.sin_port = htons(atoi(argv[2]));
	    argc--; argv++; continue;
	}
	if (!strcmp(argv[1], "--addr")) {
	    struct hostent *h = gethostbyname(argv[2]);
	    if (h) {
		me.sa.sin_addr = *(struct in_addr *)(h->h_addr);
	    } else if (!inet_aton(argv[1], &me.sa.sin_addr)) {
		perror("cannot parse address");
		exit(1);
	    }
	    argc--; argv++; continue;
	}
	break;
    }
    mainloop(&me);
    return 0;
}

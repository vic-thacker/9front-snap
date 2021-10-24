#include <u.h>
#include <libc.h>
#include <bio.h>
#include <thread.h>
#include <regexp.h>

#include "mail.h"

static int
procrd(Biobufhdr *f, void *buf, long len)
{
	return ioread(f->aux, f->fid, buf, len);
}

static int
procwr(Biobufhdr *f, void *buf, long len)
{
	return iowrite(f->aux, f->fid, buf, len);
}

/*
 * NB: this function consumes integers with
 * a trailing space, as generated by acme;
 * it's not a general purpose number parsing
 * function.
 */
static int
evgetnum(Biobuf *f)
{
	int c, n;

	n = 0;
	while('0'<=(c=Bgetc(f)) && c<='9')
		n = n*10+(c-'0');
	if(c != ' '){
		werrstr("event number syntax: %c", c);
		return -1;
	}
	return n;
}

static int
evgetdata(Biobuf *f, Event *e)
{
	int i, n, o;
	Rune r;

	o = 0;
	n = evgetnum(f);
	for(i = 0; i < n; i++){
		if((r = Bgetrune(f)) == -1)
			break;
		o += runetochar(e->text + o, &r);
	}
	e->text[o] = 0;
	return o;
}

int
winevent(Win *w, Event *e)
{
	int flags;

	flags = 0;
Again:
	e->action = Bgetc(w->event);
	e->type = Bgetc(w->event);
	e->q0 = evgetnum(w->event);
	e->q1 = evgetnum(w->event);
	e->flags = evgetnum(w->event);
	e->ntext = evgetdata(w->event, e);
	if(Bgetc(w->event) != '\n'){
		werrstr("unterminated message");
		return -1;
	}
//fprint(2, "event: %c%c %d %d %x %.*s\n", e->action, e->type, e->q0, e->q1, e->flags, e->ntext, e->text);
	if(e->flags & 0x2){
		e->p0 = e->q0;
		flags = e->flags;
		goto Again;
	}
	e->flags |= flags;
	return e->action;
}

void
winreturn(Win *w, Event *e)
{
	if(e->flags & 0x2)
		fprint(w->revent, "%c%c%d %d\n", e->action, e->type, e->p0, e->p0);
	else
		fprint(w->revent, "%c%c%d %d\n", e->action, e->type, e->q0, e->q1);
}

int
winopen(Win *w, char *f, int mode)
{
	char buf[128];
	int fd;

	snprint(buf, sizeof(buf), "/mnt/wsys/%d/%s", w->id, f);
	if((fd = open(buf, mode|OCEXEC)) == -1)
		sysfatal("open %s: %r", buf);
	return fd;
}

Biobuf*
bwinopen(Win *w, char *f, int mode)
{
	char buf[128];
	Biobuf *bfd;

	snprint(buf, sizeof(buf), "/mnt/wsys/%d/%s", w->id, f);
	if((bfd = Bopen(buf, mode|OCEXEC)) == nil)
		sysfatal("open %s: %r", buf);
	bfd->aux = w->io;
	Biofn(bfd, (mode == OREAD)?procrd:procwr);
	return bfd;
}

Biobuf*
bwindata(Win *w, int mode)
{
	int fd;

	if((fd = dup(w->data, -1)) == -1)
		sysfatal("dup: %r");
	return Bfdopen(fd, mode);
}

void
wininit(Win *w, char *name)
{
	char buf[12];

	w->ctl = open("/mnt/wsys/new/ctl", ORDWR|OCEXEC);
	if(w->ctl < 0)
		sysfatal("winopen: %r");
	if(read(w->ctl, buf, 12)!=12)
		sysfatal("read ctl: %r");
	if(fprint(w->ctl, "name %s\n", name) == -1)
		sysfatal("write ctl: %r");
	if(fprint(w->ctl, "noscroll\n") == -1)
		sysfatal("write ctl: %r");
	w->io = ioproc();
	w->id = atoi(buf);
	w->event = bwinopen(w, "event", OREAD);
	w->revent = winopen(w, "event", OWRITE);
	w->addr = winopen(w, "addr", ORDWR);
	w->data = winopen(w, "data", ORDWR);
	w->open = 1;
}

void
winclose(Win *w)
{
	if(w->open)
		fprint(w->ctl, "delete\n");
	if(w->data != -1)
		close(w->data);
	if(w->addr != -1)
		close(w->addr);
	if(w->event != nil)
		Bterm(w->event);
	if(w->revent != -1)
		close(w->revent);
	if(w->io)
		closeioproc(w->io);
	if(w->ctl != -1)
		close(w->ctl);
	w->ctl = -1;
	w->data = -1;
	w->addr = -1;
	w->event = nil;
	w->revent = -1;
	w->io = nil;
}

void
wintagwrite(Win *w, char *s)
{
	int fd, n;

	n = strlen(s);
	fd = winopen(w, "tag", OWRITE);
	if(write(fd, s, n) != n)
		sysfatal("tag write: %r");
	close(fd);
}

int
winread(Win *w, int q0, int q1, char *data, int ndata)
{
	int m, n, nr;
	char *buf;

	m = q0;
	buf = emalloc(Bufsz);
	while(m < q1){
		n = sprint(buf, "#%d", m);
		if(write(w->addr, buf, n) != n){
			fprint(2, "error writing addr: %r");
			goto err;
		}
		n = read(w->data, buf, Bufsz);
		if(n <= 0){
			fprint(2, "reading data: %r");
			goto err;
		}
		nr = utfnlen(buf, n);
		while(m+nr >q1){
			do; while(n>0 && (buf[--n]&0xC0)==0x80);
			--nr;
		}
		if(n == 0 || n > ndata)
			break;
		memmove(data, buf, n);
		ndata -= n;
		data += n;
		*data = 0;
		m += nr;
	}
	free(buf);
	return 0;
err:
	free(buf);
	return -1;
}

char*
winreadsel(Win *w)
{
	int n, q0, q1;
	char *r;

	wingetsel(w, &q0, &q1);
	n = UTFmax*(q1-q0);
	r = emalloc(n + 1);
	if(winread(w, q0, q1, r, n) == -1){
		free(r);
		return nil;
	}
	return r;
}

void
wingetsel(Win *w, int *q0, int *q1)
{
	char *e, buf[25];

	fprint(w->ctl, "addr=dot");
	if(pread(w->addr, buf, 24, 0) != 24)
		sysfatal("read addr: %r");
	buf[24] = 0;
	*q0 = strtol(buf, &e, 10);
	*q1 = strtol(e, nil, 10);
}

void
winsetsel(Win *w, int q0, int q1)
{
	fprint(w->addr, "#%d,#%d", q0, q1);
	fprint(w->ctl, "dot=addr");
}

int
matchmesg(Win *, char *text)
{
	Resub m[3];

	memset(m, 0, sizeof(m));
	if(strncmp(text, mbox.path, strlen(mbox.path)) == 0)
		return strstr(text + strlen(mbox.path), "/body") == nil;
	else if(regexec(mesgpat, text, m, nelem(m))){
		if(*m[1].ep == 0 || *m[1].ep == '/'){
			*m[1].ep = 0;
			return 1;
		}
	}
	return 0;
}

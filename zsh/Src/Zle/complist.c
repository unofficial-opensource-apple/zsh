/*
 * complist.c - completion listing enhancements
 *
 * This file is part of zsh, the Z shell.
 *
 * Copyright (c) 1999 Sven Wischnowsky
 * All rights reserved.
 *
 * Permission is hereby granted, without written agreement and without
 * license or royalty fees, to use, copy, modify, and distribute this
 * software and to distribute modified versions of this software for any
 * purpose, provided that the above copyright notice and the following
 * two paragraphs appear in all copies of this software.
 *
 * In no event shall Sven Wischnowsky or the Zsh Development Group be liable
 * to any party for direct, indirect, special, incidental, or consequential
 * damages arising out of the use of this software and its documentation,
 * even if Sven Wischnowsky and the Zsh Development Group have been advised of
 * the possibility of such damage.
 *
 * Sven Wischnowsky and the Zsh Development Group specifically disclaim any
 * warranties, including, but not limited to, the implied warranties of
 * merchantability and fitness for a particular purpose.  The software
 * provided hereunder is on an "as is" basis, and Sven Wischnowsky and the
 * Zsh Development Group have no obligation to provide maintenance,
 * support, updates, enhancements, or modifications.
 *
 */

#include "complist.mdh"
#include "complist.pro"


/* We use the parameters ZLS_COLORS and ZLS_COLOURS in the same way as
 * the color ls does. It's just that we don't support the `or' file
 * type. */


static Widget w_menuselect;
static Keymap mskeymap, lskeymap;

/* Indixes into the terminal string arrays. */

#define COL_NO  0
#define COL_FI  1
#define COL_DI  2
#define COL_LN  3
#define COL_PI  4
#define COL_SO  5
#define COL_BD  6
#define COL_CD  7
#define COL_EX  8
#define COL_MI  9
#define COL_LC 10
#define COL_RC 11
#define COL_EC 12
#define COL_TC 13
#define COL_SP 14
#define COL_MA 15
#define COL_HI 16
#define COL_DU 17

#define NUM_COLS 18

/* Maximum number of in-string colours supported. */

#define MAX_POS 11

/* Names of the terminal strings. */

static char *colnames[] = {
    "no", "fi", "di", "ln", "pi", "so", "bd", "cd", "ex", "mi",
    "lc", "rc", "ec", "tc", "sp", "ma", "hi", "du", NULL
};

/* Default values. */

static char *defcols[] = {
    "0", "0", "1;31", "1;36", "33", "1;35", "1;33", "1;33", "1;32", NULL,
    "\033[", "m", NULL, "0", "0", "7", NULL, NULL
};

/* This describes a terminal string for a file type. */

typedef struct filecol *Filecol;

struct filecol {
    Patprog prog;		/* group pattern */
    char *col;			/* color string */
    Filecol next;		/* next one */
};

/* This describes a terminal string for a pattern. */

typedef struct patcol *Patcol;

struct patcol {
    Patprog prog;
    Patprog pat;		/* pattern for match */
    char *cols[MAX_POS + 1];
    Patcol next;
};

/* This describes a terminal string for a filename extension. */

typedef struct extcol *Extcol;

struct extcol {
    Patprog prog;		/* group pattern or NULL */
    char *ext;			/* the extension */
    char *col;			/* the terminal color string */
    Extcol next;		/* the next one in the list */
};

/* This holds all terminal strings. */

typedef struct listcols *Listcols;

struct listcols {
    Filecol files[NUM_COLS];	/* strings for file types */
    Patcol pats;		/* strings for patterns */
    Extcol exts;		/* strings for extensions */
};

/* Combined length of LC and RC, maximum length of capability strings. */

static int lr_caplen, max_caplen;

/* This parses the value of a definition (the part after the `=').
 * The return value is a pointer to the character after it. */

static char *
getcolval(char *s, int multi)
{
    char *p, *o = s;

    for (p = s; *s && *s != ':' && (!multi || *s != '='); p++, s++) {
	if (*s == '\\' && s[1]) {
	    switch (*++s) {
	    case 'a': *p = '\007'; break;
	    case 'n': *p = '\n'; break;
	    case 'b': *p = '\b'; break;
	    case 't': *p = '\t'; break;
	    case 'v': *p = '\v'; break;
	    case 'f': *p = '\f'; break;
	    case 'r': *p = '\r'; break;
	    case 'e': *p = '\033'; break;
	    case '_': *p = ' '; break;
	    case '?': *p = '\177'; break;
	    default:
		if (*s >= '0' && *s <= '7') {
		    int i = STOUC(*s);

		    if (*++s >= '0' && *s <= '7') {
			i = (i * 8) + STOUC(*s);
			if (*++s >= '0' && *s <= '7')
			    i = (i * 8) + STOUC(*s);
		    }
		    *p = (char) i;
		} else
		    *p = *s;
	    }
	} else if (*s == '^') {
	    if ((s[1] >= '@' && s[1] <= '_') ||
		(s[1] >= 'a' && s[1] <= 'z'))
		*p = (char) (STOUC(*s) & ~0x60);
	    else if (s[1] == '?')
		*p = '\177';
	    else {
		*p++ = *s;
		*p = s[1];
	    }
	    s++;
	} else
	    *p = *s;
    }
    if (p != s)
	*p = '\0';
    if ((s - o) > max_caplen)
	max_caplen = s - o;
    return s;
}

/* This parses one definition. Return value is a pointer to the
 * character after it. */

static char *
getcoldef(Listcols c, char *s)
{
    Patprog gprog = NULL;

    if (*s == '(') {
	char *p;
	int l = 0;

	for (p = s + 1, l = 0; *p && (*p != ')' || l); p++)
	    if (*p == '\\' && p[1])
		p++;
	    else if (*p == '(')
		l++;
	    else if (*p == ')')
		l--;

	if (*p == ')') {
	    char sav = p[1];

	    p[1] = '\0';
	    tokenize(s);
	    gprog = patcompile(s, 0, NULL);
	    p[1]  =sav;

	    s = p + 1;
	}
    }
    if (*s == '*') {
	Extcol ec, eo;
	char *n, *p;

	/* This is for an extension. */

	n = ++s;
	while (*s && *s != '=')
	    s++;
	if (!*s)
	    return s;
	*s++ = '\0';
	p = getcolval(s, 0);
	ec = (Extcol) zhalloc(sizeof(*ec));
	ec->prog = gprog;
	ec->ext = n;
	ec->col = s;
	ec->next = NULL;
	if ((eo = c->exts)) {
	    while (eo->next)
		eo = eo->next;
	    eo->next = ec;
	} else
	    c->exts = ec;
	if (*p)
	    *p++ = '\0';
	return p;
    } else if (*s == '=') {
	char *p = ++s, *t, *cols[MAX_POS];
	int ncols = 0;
	Patprog prog;

	/* This is for a pattern. */

	while (*s && *s != '=')
	    s++;
	if (!*s)
	    return s;
	*s++ = '\0';
	while (1) {
	    t = getcolval(s, 1);
	    if (ncols < MAX_POS)
		cols[ncols++] = s;
	    s = t;
	    if (*s != '=')
		break;
	    *s++ = '\0';
	}
	tokenize(p);
	if ((prog = patcompile(p, 0, NULL))) {
	    Patcol pc, po;
	    int i;

	    pc = (Patcol) zhalloc(sizeof(*pc));
	    pc->prog = gprog;
	    pc->pat = prog;
	    for (i = 0; i < ncols; i++)
		pc->cols[i] = cols[i];
	    pc->cols[i] = NULL;
	    pc->next = NULL;
	    if ((po = c->pats)) {
		while (po->next)
		    po = po->next;
		po->next = pc;
	    } else
		c->pats = pc;
	}
	if (*t)
	    *t++ = '\0';
	return t;
    } else {
	char *n = s, *p, **nn;
	int i;

	/* This is for a file type. */

	while (*s && *s != '=')
	    s++;
	if (!*s)
	    return s;
	*s++ = '\0';
	for (i = 0, nn = colnames; *nn; i++, nn++)
	    if (!strcmp(n, *nn))
		break;
	p = getcolval(s, 0);
	if (*nn) {
	    Filecol fc, fo;

	    fc = (Filecol) zhalloc(sizeof(*fc));
	    fc->prog = (i == COL_EC || i == COL_LC || i == COL_RC ?
			NULL : gprog);
	    fc->col = s;
	    fc->next = NULL;
	    if ((fo = c->files[i])) {
		while (fo->next)
		    fo = fo->next;
		fo->next = fc;
	    } else
		c->files[i] = fc;
	}
	if (*p)
	    *p++ = '\0';
	return p;
    }
}

static Filecol
filecol(char *col)
{
    Filecol fc;

    fc = (Filecol) zhalloc(sizeof(*fc));
    fc->prog = NULL;
    fc->col = col;
    fc->next = NULL;

    return fc;
}

/* This initializes the given terminal color structure. */

static void
getcols(Listcols c)
{
    char *s;
    int i, l;

    max_caplen = lr_caplen = 0;
    queue_signals();
    if (!(s = getsparam("ZLS_COLORS")) &&
	!(s = getsparam("ZLS_COLOURS"))) {
	for (i = 0; i < NUM_COLS; i++)
	    c->files[i] = filecol("");
	c->pats = NULL;
	c->exts = NULL;
	
	if ((s = tcstr[TCSTANDOUTBEG]) && s[0]) {
	    c->files[COL_MA] = filecol(s);
	    c->files[COL_EC] = filecol(tcstr[TCSTANDOUTEND]);
	} else
	    c->files[COL_MA] = filecol(defcols[COL_MA]);
	lr_caplen = 0;
	if ((max_caplen = strlen(c->files[COL_MA]->col)) <
	    (l = strlen(c->files[COL_EC]->col)))
	    max_caplen = l;
	unqueue_signals();
	return;
    }
    /* We have one of the parameters, use it. */
    memset(c, 0, sizeof(*c));
    s = dupstring(s);
    while (*s)
	if (*s == ':')
	    s++;
	else
	    s = getcoldef(c, s);
    unqueue_signals();

    /* Use default values for those that aren't set explicitly. */
    for (i = 0; i < NUM_COLS; i++) {
	if (!c->files[i] || !c->files[i]->col)
	    c->files[i] = filecol(defcols[i]);
	if (c->files[i] && c->files[i]->col &&
	    (l = strlen(c->files[i]->col)) > max_caplen)
	    max_caplen = l;
    }
    lr_caplen = strlen(c->files[COL_LC]->col) + strlen(c->files[COL_RC]->col);

    /* Default for missing files. */
    if (!c->files[COL_MI] || !c->files[COL_MI]->col)
	c->files[COL_MI] = c->files[COL_FI];

    return;
}

/* Information about the list shown. */

static int noselect, mselect, inselect, mcol, mline, mcols, mlines, mmlen;
static int selected, mlbeg = -1, mlend = 9999999, mscroll, mrestlines;
static int mnew, mlastcols, mlastlines, mhasstat, mfirstl, mlastm;
static int mlprinted, molbeg = -2, mocol = 0, moline = 0, mstatprinted;
static char *mstatus, *mlistp;
static Cmatch **mtab, **mmtabp;
static int mtab_been_reallocated;
static Cmgroup *mgtab, *mgtabp;
static struct listcols mcolors;

/* Used in mtab/mgtab, for explanations. */

#define MMARK       ((unsigned long) 1)
#define mmarked(v)  (((unsigned long) (v)) & MMARK)
#define mtmark(v)   ((Cmatch *) (((unsigned long) (v)) | MMARK))
#define mtunmark(v) ((Cmatch *) (((unsigned long) (v)) & ~MMARK))
#define mgmark(v)   ((Cmgroup)  (((unsigned long) (v)) | MMARK))
#define mgunmark(v) ((Cmgroup)  (((unsigned long) (v)) & ~MMARK))

/* Information for in-string colours. */

static int nrefs;
static int begpos[MAX_POS], curisbeg;
static int endpos[MAX_POS];
static int sendpos[MAX_POS], curissend; /* sorted end positions */
static char **patcols, *curiscols[MAX_POS];
static int curiscol;

/* The last color used. */

static char *last_cap;

static void
zlrputs(Listcols c, char *cap)
{
    if (!*last_cap || strcmp(last_cap, cap)) {
	VARARR(char, buf, lr_caplen + max_caplen + 1);

	strcpy(buf, c->files[COL_LC]->col);
	strcat(buf, cap);
	strcat(buf, c->files[COL_RC]->col);

	tputs(buf, 1, putshout);

	strcpy(last_cap, cap);
    }
}

static void
zcputs(Listcols c, char *group, int colour)
{
    Filecol fc;

    for (fc = c->files[colour]; fc; fc = fc->next)
	if (fc->col &&
	    (!fc->prog || !group || pattry(fc->prog, group))) {
	    zlrputs(c, fc->col);

	    return;
	}
    zlrputs(c, "0");
}

/* Turn off colouring. */

static void
zcoff(void)
{
    if (mcolors.files[COL_EC] && mcolors.files[COL_EC]->col) {
	tputs(mcolors.files[COL_EC]->col, 1, putshout);
	*last_cap = '\0';
    } else
	zcputs(&mcolors, NULL, COL_NO);
}


static void
initiscol(Listcols c)
{
    int i;

    zlrputs(c, patcols[0]);

    curiscols[curiscol = 0] = *patcols++;

    curisbeg = curissend = 0;

    for (i = 0; i < nrefs; i++)
	sendpos[i] = 0xfffffff;
    for (; i < MAX_POS; i++)
	begpos[i] = endpos[i] = sendpos[i] = 0xfffffff;
}

static void
doiscol(Listcols c, int pos)
{
    int fi;

    while (pos > sendpos[curissend]) {
	curissend++;
	if (curiscol) {
	    zcputs(c, NULL, COL_NO);
	    zlrputs(c, curiscols[--curiscol]);
	}
    }
    while (((fi = (endpos[curisbeg] < begpos[curisbeg] || 
		  begpos[curisbeg] == -1)) ||
	    pos == begpos[curisbeg]) && *patcols) {
	if (!fi) {
	    int i, j, e = endpos[curisbeg];
	    
	    /* insert e in sendpos */
	    for (i = curissend; sendpos[i] <= e; ++i)
		;
	    for (j = MAX_POS - 1; j > i; --j)
		sendpos[j] = sendpos[j-1];
	    sendpos[i] = e;
	    
	    zcputs(c, NULL, COL_NO);
	    zlrputs(c, *patcols);
	    curiscols[++curiscol] = *patcols;
	}
	++patcols;
	++curisbeg;
    }
}

/* Stripped-down version of printfmt(). But can do in-string colouring. */

static int
clprintfmt(Listcols c, char *p, int ml)
{
    int cc = 0, i = 0, ask, beg;

    initiscol(c);

    for (; *p; p++) {
	doiscol(c, i++);
	cc++;
	if (*p == '\n') {
	    if (mlbeg >= 0 && tccan(TCCLEAREOL))
		tcout(TCCLEAREOL);
	    cc = 0;
	}
	if (ml == mlend - 1 && (cc % columns) == columns - 1)
	    return 0;

	putc(*p, shout);
	if ((beg = !(cc % columns)))
	    ml++;
	if (mscroll && !(cc % columns) &&
	    !--mrestlines && (ask = asklistscroll(ml)))
	    return ask;
    }
    if (mlbeg >= 0 && tccan(TCCLEAREOL))
	tcout(TCCLEAREOL);
    return 0;
}

/* Local version of nicezputs() with in-string colouring. */

static int
clnicezputs(Listcols c, char *s, int ml)
{
    int cc, i = 0, col = 0, ask, oml = ml;
    char *t;

    initiscol(c);

    while ((cc = *s++)) {
	doiscol(c, i++);
	if (itok(cc)) {
	    if (cc <= Comma)
		cc = ztokens[cc - Pound];
	    else 
		continue;
	}
	if (cc == Meta)
	    cc = *s++ ^ 32;

	for (t = nicechar(cc); *t; t++) {
	    if (ml == mlend - 1 && col == columns - 1) {
		mlprinted = ml - oml;
		return 0;
	    }
	    putc(*t, shout);
	    if (++col == columns) {
		ml++;
		if (mscroll && !--mrestlines && (ask = asklistscroll(ml))) {
		    mlprinted = ml - oml;
		    return ask;
		}
		col = 0;
                fputs(" \010", shout);
	    }
	}
    }
    mlprinted = ml - oml;
    return 0;
}

/* Get the terminal color string for the given match. */

static int
putmatchcol(Listcols c, char *group, char *n)
{
    Patcol pc;

    nrefs = MAX_POS - 1;

    for (pc = c->pats; pc; pc = pc->next)
	if ((!pc->prog || !group || pattry(pc->prog, group)) &&
	    pattryrefs(pc->pat, n, &nrefs, begpos, endpos)) {
	    if (pc->cols[1]) {
		patcols = pc->cols;

		return 1;
	    }
	    zlrputs(c, pc->cols[0]);

	    return 0;
	}

    zcputs(c, group, COL_NO);

    return 0;
}

/* Get the terminal color string for the file with the given name and
 * file modes. */

static int
putfilecol(Listcols c, char *group, char *n, mode_t m)
{
    int colour;
    Extcol ec;
    Patcol pc;

    for (ec = c->exts; ec; ec = ec->next)
	if (strsfx(ec->ext, n) &&
	    (!ec->prog || !group || pattry(ec->prog, group))) {
	    zlrputs(c, ec->col);

	    return 0;
	}

    nrefs = MAX_POS - 1;

    for (pc = c->pats; pc; pc = pc->next)
	if ((!pc->prog || !group || pattry(pc->prog, group)) &&
	    pattryrefs(pc->pat, n, &nrefs, begpos, endpos)) {
	    if (pc->cols[1]) {
		patcols = pc->cols;

		return 1;
	    }
	    zlrputs(c, pc->cols[0]);

	    return 0;
	}

    if (S_ISDIR(m))
	colour = COL_DI;
    else if (S_ISLNK(m))
	colour = COL_LN;
    else if (S_ISFIFO(m))
	colour = COL_PI;
    else if (S_ISSOCK(m))
	colour = COL_SO;
    else if (S_ISBLK(m))
	colour = COL_BD;
    else if (S_ISCHR(m))
	colour = COL_CD;
    else if (S_ISREG(m) && (m & S_IXUGO))
	colour = COL_EX;
    else
	colour = COL_FI;

    zcputs(c, group, colour);

    return 0;
}

static Cmgroup last_group;

/**/
static int
asklistscroll(int ml)
{
    Thingy cmd;
    int i, ret = 0;

    compprintfmt(NULL, 1, 1, 1, ml, NULL);

    fflush(shout);
    zsetterm();
    selectlocalmap(lskeymap);
    if (!(cmd = getkeycmd()) || cmd == Th(z_sendbreak))
	ret = 1;
    else if (cmd == Th(z_acceptline) ||
	     cmd == Th(z_downhistory) ||
	     cmd == Th(z_downlineorhistory) ||
	     cmd == Th(z_downlineorsearch) ||
	     cmd == Th(z_vidownlineorhistory))
	mrestlines = 1;
    else if (cmd == Th(z_completeword) ||
		   cmd == Th(z_expandorcomplete) ||
		   cmd == Th(z_expandorcompleteprefix) ||
		   cmd == Th(z_menucomplete) ||
		   cmd == Th(z_menuexpandorcomplete) ||
		   !strcmp(cmd->nam, "menu-select") ||
		   !strcmp(cmd->nam, "complete-word") ||
		   !strcmp(cmd->nam, "expand-or-complete") ||
		   !strcmp(cmd->nam, "expand-or-complete-prefix") ||
		   !strcmp(cmd->nam, "menu-complete") ||
	     !strcmp(cmd->nam, "menu-expand-or-complete"))
	mrestlines = lines - 1;
    else {
	ungetkeycmd();
	ret = 1;
    }
    selectlocalmap(NULL);
    settyinfo(&shttyinfo);
    putc('\r', shout);
    for (i = columns - 1; i--; )
	putc(' ', shout);
    putc('\r', shout);

    return ret;
}

#define dolist(X)   ((X) >= mlbeg && (X) < mlend)
#define dolistcl(X) ((X) >= mlbeg && (X) < mlend + 1)
#define dolistnl(X) ((X) >= mlbeg && (X) < mlend - 1)

/**/
static int
compprintnl(int ml)
{
    int ask;

    if (mlbeg >= 0 && tccan(TCCLEAREOL))
	tcout(TCCLEAREOL);
    putc('\n', shout);

    if (mscroll && !--mrestlines && (ask = asklistscroll(ml)))
	return ask;

    return 0;
}

/* This is used to print the strings (e.g. explanations). *
 * It returns the number of lines printed.       */

/**/
static int
compprintfmt(char *fmt, int n, int dopr, int doesc, int ml, int *stop)
{
    char *p, nc[2*DIGBUFSIZE + 12], nbuf[2*DIGBUFSIZE + 12];
    int l = 0, cc = 0, b = 0, s = 0, u = 0, m, ask, beg, stat;

    if ((stat = !fmt)) {
	if (mlbeg >= 0) {
	    if (!(fmt = mstatus)) {
		mlprinted = 0;
		return 0;
	    }
	    cc = -1;
	} else
	    fmt = mlistp;
    }
    for (p = fmt; *p; p++) {
	if (doesc && *p == '%') {
	    if (*++p) {
		m = 0;
		switch (*p) {
		case '%':
		    if (dopr == 1)
			putc('%', shout);
		    cc++;
		    break;
		case 'n':
		    if (!stat) {
			sprintf(nc, "%d", n);
			if (dopr == 1)
			    fputs(nc, shout);
			cc += strlen(nc);
		    }
		    break;
		case 'B':
		    b = 1;
		    if (dopr)
			tcout(TCBOLDFACEBEG);
		    break;
		case 'b':
		    b = 0; m = 1;
		    if (dopr)
			tcout(TCALLATTRSOFF);
		    break;
		case 'S':
		    s = 1;
		    if (dopr)
			tcout(TCSTANDOUTBEG);
		    break;
		case 's':
		    s = 0; m = 1;
		    if (dopr)
			tcout(TCSTANDOUTEND);
		    break;
		case 'U':
		    u = 1;
		    if (dopr)
			tcout(TCUNDERLINEBEG);
		    break;
		case 'u':
		    u = 0; m = 1;
		    if (dopr)
			tcout(TCUNDERLINEEND);
		    break;
		case '{':
		    for (p++; *p && (*p != '%' || p[1] != '}'); p++)
			if (dopr)
			    putc(*p, shout);
		    if (*p)
			p++;
		    else
			p--;
		    break;
		case 'm':
		    if (stat) {
			sprintf(nc, "%d/%d", (n ? mlastm : mselect),
				listdat.nlist);
			m = 2;
		    }
		    break;
		case 'M':
		    if (stat) {
			sprintf(nbuf, "%d/%d", (n ? mlastm : mselect),
				listdat.nlist);
			sprintf(nc, "%-9s", nbuf);
			m = 2;
		    }
		    break;
		case 'l':
		    if (stat) {
			sprintf(nc, "%d/%d", ml + 1, listdat.nlines);
			m = 2;
		    }
		    break;
		case 'L':
		    if (stat) {
			sprintf(nbuf, "%d/%d", ml + 1, listdat.nlines);
			sprintf(nc, "%-9s", nbuf);
			m = 2;
		    }
		    break;
		case 'p':
		    if (stat) {
			if (ml == listdat.nlines - 1)
			    strcpy(nc, "Bottom");
			else if (n ? mfirstl : (mlbeg > 0 || ml != mfirstl))
			    sprintf(nc, "%d%%",
				    ((ml + 1) * 100) / listdat.nlines);
			else
			    strcpy(nc, "Top");
			m = 2;
		    }
		    break;
		case 'P':
		    if (stat) {
			if (ml == listdat.nlines - 1)
			    strcpy(nc, "Bottom");
			else if (n ? mfirstl : (mlbeg > 0 || ml != mfirstl))
			    sprintf(nc, "%2d%%   ",
				    ((ml + 1) * 100) / listdat.nlines);
			else
			    strcpy(nc, "Top   ");
			m = 2;
		    }
		    break;
		}
		if (m == 2 && dopr == 1) {
		    int l = strlen(nc);

		    if (l + cc > columns - 2)
			nc[l -= l + cc - (columns - 2)] = '\0';
		    fputs(nc, shout);
		    cc += l;
		} else if (dopr && m == 1) {
		    if (b)
			tcout(TCBOLDFACEBEG);
		    if (s)
			tcout(TCSTANDOUTBEG);
		    if (u)
			tcout(TCUNDERLINEBEG);
		}
	    } else
		break;
	} else {
	    if ((++cc == columns - 2 || *p == '\n') && stat)
		dopr = 2;
	    if (*p == '\n') {
		if (dopr == 1 && mlbeg >= 0 && tccan(TCCLEAREOL))
		    tcout(TCCLEAREOL);
		l += 1 + ((cc - 1) / columns);
		cc = 0;
	    }
	    if (dopr == 1) {
		if (ml == mlend - 1 && (cc % columns) == columns - 1) {
		    dopr = 0;
		    continue;
		}
		putc(*p, shout);
		if ((beg = !(cc % columns)) && !stat) {
		    ml++;
                    fputs(" \010", shout);
                }
		if (mscroll && beg && !--mrestlines && (ask = asklistscroll(ml))) {
		    *stop = 1;
		    if (stat && n)
			mfirstl = -1;
		    return (mlprinted = l + (cc / columns));
		}
	    }
	}
    }
    if (dopr) {
        if (!(cc % columns))
            fputs(" \010", shout);
        if (mlbeg >= 0 && tccan(TCCLEAREOL))
            tcout(TCCLEAREOL);
    }
    if (stat && n)
	mfirstl = -1;

    return (mlprinted = l + (cc / columns));
}

/* This is like zputs(), but allows scrolling. */

/**/
static int
compzputs(char const *s, int ml)
{
    int c, col = 0, ask;

    while (*s) {
	if (*s == Meta)
	    c = *++s ^ 32;
	else if(itok(*s)) {
	    s++;
	    continue;
	} else
	    c = *s;
	s++;
	putc(c, shout);
	if (c == '\n' && mlbeg >= 0 && tccan(TCCLEAREOL))
	    tcout(TCCLEAREOL);
	if (mscroll && (++col == columns || c == '\n')) {
	    ml++;
	    if (!--mrestlines && (ask = asklistscroll(ml)))
		return ask;

	    col = 0;
	}
    }
    return 0;
}

/* This is like nicezputs(), but allows scrolling. */

/**/
static int
compnicezputs(char *s, int ml)
{
    int c, col = 0, ask, oml = ml;
    char *t;

    while ((c = *s++)) {
	if (itok(c)) {
	    if (c <= Comma)
		c = ztokens[c - Pound];
	    else 
		continue;
	}
	if (c == Meta)
	    c = *s++ ^ 32;

	for (t = nicechar(c); *t; t++) {
	    if (ml == mlend - 1 && col == columns - 1) {
		mlprinted = ml - oml;
		return 0;
	    }
	    putc(*t, shout);
	    if (++col == columns) {
		ml++;
		if (mscroll && !--mrestlines && (ask = asklistscroll(ml))) {
		    mlprinted = ml - oml;
		    return ask;
		}
		col = 0;
	    }
	}
    }
    mlprinted = ml - oml;
    return 0;
}

/**/
static int
compprintlist(int showall)
{
    static int lasttype = 0, lastbeg = 0, lastml = 0, lastinvcount = -1;
    static int lastn = 0, lastnl = 0, lastnlnct = -1;
    static Cmgroup lastg = NULL;
    static Cmatch *lastp = NULL;
    static Cexpl *lastexpl = NULL;

    Cmgroup g;
    Cmatch *p, m;
    Cexpl *e;
    int pnl = 0, cl, mc = 0, ml = 0, printed = 0, stop = 0, asked = 1;
    int lastused = 0;

    mfirstl = -1;
    if (mnew || lastinvcount != invcount || lastbeg != mlbeg || mlbeg < 0) {
	lasttype = 0;
	lastg = NULL;
	lastexpl = NULL;
	lastml = 0;
	lastnlnct = -1;
    }
    cl = (listdat.nlines > lines - nlnct - mhasstat ?
	  lines - nlnct - mhasstat : listdat.nlines) - (lastnlnct > nlnct);
    lastnlnct = nlnct;
    mrestlines = lines - 1;
    lastinvcount = invcount;

    if (cl < 2) {
	cl = -1;
	if (tccan(TCCLEAREOD))
	    tcout(TCCLEAREOD);
    } else if (mlbeg >= 0 && !tccan(TCCLEAREOL) && tccan(TCCLEAREOD))
	tcout(TCCLEAREOD);

    g = ((lasttype && lastg) ? lastg : amatches);
    while (g) {
	char **pp = g->ylist;

	if ((e = g->expls)) {
	    int l;

	    if (!lastused && lasttype == 1) {
		e = lastexpl;
		ml = lastml;
		lastused = 1;
	    }
	    while (*e) {
		if (((*e)->count || (*e)->always) &&
		    (!listdat.onlyexpl ||
		     (listdat.onlyexpl & ((*e)->always > 0 ? 2 : 1)))) {
		    if (pnl) {
			if (dolistnl(ml) && compprintnl(ml))
			    goto end;
			pnl = 0;
			ml++;
			if (dolistcl(ml) && cl >= 0 && --cl <= 1) {
			    cl = -1;
			    if (tccan(TCCLEAREOD))
				tcout(TCCLEAREOD);
			}
		    }
		    if (mlbeg < 0 && mfirstl < 0)
			mfirstl = ml;
		    l = compprintfmt((*e)->str,
                                     ((*e)->always ? -1 : (*e)->count),
                                     dolist(ml), 1, ml, &stop);
		    if (mselect >= 0) {
			int mm = (mcols * ml), i;

			for (i = mcols; i--; ) {
			    mtab[mm + i] = mtmark(NULL);
			    mgtab[mm + i] = mgmark(NULL);
			}
		    }
		    if (stop)
			goto end;
		    if (!lasttype && ml >= mlbeg) {
			lasttype = 1;
			lastg = g;
			lastbeg = mlbeg;
			lastml = ml;
			lastexpl = e;
			lastp = NULL;
			lastused = 1;
		    }
		    ml += mlprinted;
		    if (dolistcl(ml) && cl >= 0 && (cl -= mlprinted) <= 1) {
			cl = -1;
			if (tccan(TCCLEAREOD))
			    tcout(TCCLEAREOD);
		    }
		    pnl = 1;
		}
		e++;
		if (!mnew && ml > mlend)
		    goto end;
	    }
	}
	if (!listdat.onlyexpl && mlbeg < 0 && pp && *pp) {
	    if (pnl) {
		if (dolistnl(ml) && compprintnl(ml))
		    goto end;
		pnl = 0;
		ml++;
		if (cl >= 0 && --cl <= 1) {
		    cl = -1;
		    if (tccan(TCCLEAREOD))
			tcout(TCCLEAREOD);
		}
	    }
	    if (mlbeg < 0 && mfirstl < 0)
		mfirstl = ml;
	    if (g->flags & CGF_LINES) {
		while (*pp) {
		    if (compzputs(*pp, ml))
			goto end;
		    if (*++pp && compprintnl(ml))
			goto end;
		}
	    } else {
		int n = g->lcount, nl, nc, i, a;
		char **pq;

		nl = nc = g->lins;

		while (n && nl--) {
		    i = g->cols;
		    mc = 0;
		    pq = pp;
		    while (n && i--) {
			if (pq - g->ylist >= g->lcount)
			    break;
			if (compzputs(*pq, mscroll))
			    goto end;
			if (i) {
			    a = (g->widths ? g->widths[mc] : g->width) -
				strlen(*pq);
			    while (a--)
				putc(' ', shout);
			}
			pq += ((g->flags & CGF_ROWS) ? 1 : nc);
			mc++;
			n--;
		    }
		    if (n) {
			if (compprintnl(ml))
			    goto end;
			ml++;
			if (cl >= 0 && --cl <= 1) {
			    cl = -1;
			    if (tccan(TCCLEAREOD))
				tcout(TCCLEAREOD);
			}
		    }
		    pp += ((g->flags & CGF_ROWS) ? g->cols : 1);
		}
	    }
	} else if (!listdat.onlyexpl &&
		   (g->lcount || (showall && g->mcount))) {
	    int n = g->dcount, nl, nc, i, j, wid;
	    Cmatch *q;

	    nl = nc = g->lins;

	    if ((g->flags & CGF_HASDL) &&
		(lastused || !lasttype || lasttype == 2)) {
		if (!lastused && lasttype == 2) {
		    p = lastp;
		    ml = lastml;
		    n = lastn;
		    nl = lastnl;
		    lastused = 1;
		    pnl = 0;
		} else
		    p = g->matches;

		for (; (m = *p); p++) {
		    if (m->disp && (m->flags & CMF_DISPLINE) &&
                        (showall || !(m->flags & (CMF_HIDE|CMF_NOLIST)))) {
			if (pnl) {
			    if (dolistnl(ml) && compprintnl(ml))
				goto end;
			    pnl = 0;
			    ml++;
			    if (dolistcl(ml) && cl >= 0 && --cl <= 1) {
				cl = -1;
				if (tccan(TCCLEAREOD))
				    tcout(TCCLEAREOD);
			    }
			}
			if (!lasttype && ml >= mlbeg) {
			    lasttype = 2;
			    lastg = g;
			    lastbeg = mlbeg;
			    lastml = ml;
			    lastp = p;
			    lastn = n;
			    lastnl = nl;
			    lastused = 1;
			}
			if (mfirstl < 0)
			    mfirstl = ml;
			if (dolist(ml))
			    printed++;
			if (clprintm(g, p, 0, ml, 1, 0))
			    goto end;
			ml += mlprinted;
			if (dolistcl(ml) && (cl -= mlprinted) <= 1) {
			    cl = -1;
			    if (tccan(TCCLEAREOD))
				tcout(TCCLEAREOD);
			}
			pnl = 1;
		    }
		    if (!mnew && ml > mlend)
			goto end;
		}
	    }
	    if (n && pnl) {
		if (dolistnl(ml) && compprintnl(ml))
		    goto end;
		pnl = 0;
		ml++;
		if (dolistcl(ml) && cl >= 0 && --cl <= 1) {
		    cl = -1;
		    if (tccan(TCCLEAREOD))
			tcout(TCCLEAREOD);
		}
	    }
	    if (!lastused && lasttype == 3) {
		p = lastp;
		n = lastn;
		nl = lastnl;
		ml = lastml;
		lastused = 1;
	    } else
		p = skipnolist(g->matches, showall);

	    while (n && nl--) {
		if (!lasttype && ml >= mlbeg) {
		    lasttype = 3;
		    lastg = g;
		    lastbeg = mlbeg;
		    lastml = ml;
		    lastp = p;
		    lastn = n;
		    lastnl = nl + 1;
		    lastused = 1;
		}
		i = g->cols;
		mc = 0;
		q = p;
		while (n && i--) {
		    wid = (g->widths ? g->widths[mc] : g->width);
		    if (!(m = *q)) {
			if (clprintm(g, NULL, mc, ml, (!i), wid))
			    goto end;
			break;
		    }
                    if (clprintm(g, q, mc, ml, (!i), wid))
                        goto end;

		    if (dolist(ml))
			printed++;
		    ml += mlprinted;
		    if (dolistcl(ml) && (cl -= mlprinted) < 1) {
			cl = -1;
			if (tccan(TCCLEAREOD))
			    tcout(TCCLEAREOD);
		    }
		    if (mfirstl < 0)
			mfirstl = ml;

		    if (--n)
			for (j = ((g->flags & CGF_ROWS) ? 1 : nc);
			     j && *q; j--)
			    q = skipnolist(q + 1, showall);
		    mc++;
		}
		while (i-- > 0) {
		    if (clprintm(g, NULL, mc, ml, (!i),
				 (g->widths ? g->widths[mc] : g->width)))
			goto end;
		    mc++;
		}
		if (n) {
		    if (dolistnl(ml) && compprintnl(ml))
			goto end;
		    ml++;
		    if (dolistcl(ml) && cl >= 0 && --cl <= 1) {
			cl = -1;
			if (tccan(TCCLEAREOD))
			    tcout(TCCLEAREOD);
		    }
		    if (nl)
			for (j = ((g->flags & CGF_ROWS) ? g->cols : 1);
			     j && *p; j--)
			    p = skipnolist(p + 1, showall);
		}
		if (!mnew && ml > mlend)
		    goto end;
	    }
	}
	if (g->lcount || (showall && g->mcount))
	    pnl = 1;
	g = g->next;
    }
    asked = 0;
 end:
    mstatprinted = 0;
    lastlistlen = 0;
    if (nlnct <= 1)
	mscroll = 0;
    if (clearflag) {
	int nl;

	/* Move the cursor up to the prompt, if always_last_prompt *
	 * is set and all that...                                  */
	if (mlbeg >= 0) {
	    if ((nl = listdat.nlines + nlnct) >= lines) {
		if (mhasstat) {
		    putc('\n', shout);
		    compprintfmt(NULL, 0, 1, 1, mline, NULL);
                    mstatprinted = 1;
		}
		nl = lines - 1;
	    } else
		nl--;
	    tcmultout(TCUP, TCMULTUP, nl);
	    showinglist = -1;

	    lastlistlen = listdat.nlines;
	} else if ((nl = listdat.nlines + nlnct - 1) < lines) {
	    if (mlbeg >= 0 && tccan(TCCLEAREOL))
		tcout(TCCLEAREOL);
	    tcmultout(TCUP, TCMULTUP, nl);
	    showinglist = -1;

	    lastlistlen = listdat.nlines;
	} else {
	    clearflag = 0;
	    if (!asked) {
		mrestlines = (ml + nlnct > lines);
		compprintnl(ml);
	    }
	}
    } else if (!asked) {
	mrestlines = (ml + nlnct > lines);
	compprintnl(ml);
    }
    listshown = (clearflag ? 1 : -1);
    mnew = 0;

    return printed;
}

/**/
static int
clprintm(Cmgroup g, Cmatch *mp, int mc, int ml, int lastc, int width)
{
    Cmatch m;
    int len, subcols = 0, stop = 0, ret = 0;

    if (g != last_group)
        *last_cap = '\0';

    last_group = g;

    if (!mp) {
	if (dolist(ml)) {
	    zcputs(&mcolors, g->name, COL_SP);
	    len = width - 2;
	    while (len-- > 0)
		putc(' ', shout);
	    zcoff();
	}
	mlprinted = 0;
	return 0;
    }
    m = *mp;

    if ((m->flags & CMF_ALL) && (!m->disp || !m->disp[0]))
	bld_all_str(m);

    mlastm = m->gnum;
    if (m->disp && (m->flags & CMF_DISPLINE)) {
	if (mselect >= 0) {
	    int mm = (mcols * ml), i;

            if (m->flags & CMF_DUMMY) {
                for (i = mcols; i--; ) {
                    mtab[mm + i] = mtmark(mp);
                    mgtab[mm + i] = mgmark(g);
                }
            } else {
                for (i = mcols; i--; ) {
                    mtab[mm + i] = mp;
                    mgtab[mm + i] = g;
                }
            }
	}
	if (!dolist(ml)) {
	    mlprinted = printfmt(m->disp, 0, 0, 0) / columns;
	    return 0;
	}
	if (m->gnum == mselect) {
	    int mm = (mcols * ml);
	    mline = ml;
	    mcol = 0;
	    mmtabp = mtab + mm;
	    mgtabp = mgtab + mm;
	    mmlen = mcols;
	    zcputs(&mcolors, g->name, COL_MA);
	} else if ((m->flags & CMF_NOLIST) &&
                   mcolors.files[COL_HI] && mcolors.files[COL_HI]->col)
	    zcputs(&mcolors, g->name, COL_HI);
	else if (mselect >= 0 && (m->flags & (CMF_MULT | CMF_FMULT)) &&
                 mcolors.files[COL_DU] && mcolors.files[COL_DU]->col)
	    zcputs(&mcolors, g->name, COL_DU);
	else
	    subcols = putmatchcol(&mcolors, g->name, m->disp);
	if (subcols)
	    ret = clprintfmt(&mcolors, m->disp, ml);
	else {
	    compprintfmt(m->disp, 0, 1, 0, ml, &stop);
	    if (stop)
		ret = 1;
	}
	zcoff();
    } else {
	int mx;

	if (g->widths) {
	    int i;

	    for (i = mx = 0; i < mc; i++)
		mx += g->widths[i];
	} else
	    mx = mc * g->width;

	if (mselect >= 0) {
	    int mm = mcols * ml, i;

            if (m->flags & CMF_DUMMY) {
                for (i = (width ? width : mcols); i--; ) {
                    mtab[mx + mm + i] = mtmark(mp);
                    mgtab[mx + mm + i] = mgmark(g);
                }
            } else {
                for (i = (width ? width : mcols); i--; ) {
                    mtab[mx + mm + i] = mp;
                    mgtab[mx + mm + i] = g;
                }
            }
	}
	if (!dolist(ml)) {
	    mlprinted = niceztrlen(m->disp ? m->disp : m->str) / columns;
	    return 0;
	}
	if (m->gnum == mselect) {
	    int mm = mcols * ml;

	    mcol = mx;
	    mline = ml;
	    mmtabp = mtab + mx + mm;
	    mgtabp = mgtab + mx + mm;
	    mmlen = width;
	    zcputs(&mcolors, g->name, COL_MA);
	} else if (m->flags & CMF_NOLIST)
	    zcputs(&mcolors, g->name, COL_HI);
	else if (mselect >= 0 && (m->flags & (CMF_MULT | CMF_FMULT)))
	    zcputs(&mcolors, g->name, COL_DU);
	else if (m->mode)
	    subcols = putfilecol(&mcolors, g->name, m->str, m->mode);
	else
	    subcols = putmatchcol(&mcolors, g->name, (m->disp ? m->disp : m->str));

	if (subcols)
	    ret = clnicezputs(&mcolors, (m->disp ? m->disp : m->str), ml);
	else
	    ret = compnicezputs((m->disp ? m->disp : m->str), ml);
	if (ret) {
	    zcoff();
	    return 1;
	}
	len = niceztrlen(m->disp ? m->disp : m->str);
	mlprinted = len / columns;

	if ((g->flags & CGF_FILES) && m->modec) {
	    if (m->gnum != mselect) {
		zcoff();
		zcputs(&mcolors, g->name, COL_TC);
	    }
	    putc(m->modec, shout);
	    len++;
        }
	if ((len = width - len - 2) > 0) {
	    if (m->gnum != mselect) {
		zcoff();
		zcputs(&mcolors, g->name, COL_SP);
	    }
	    while (len-- > 0)
		putc(' ', shout);
	}
	zcoff();
	if (!lastc) {
	    zcputs(&mcolors, g->name, COL_SP);
	    fputs("  ", shout);
	    zcoff();
	}
    }
    return ret;
}

static int
singlecalc(int *cp, int l, int *lcp)
{
    int c = *cp, n, j, first = 1;
    Cmatch **p, *op, *mp = mtab[l * columns + c];

    for (n = 0, j = c, p = mtab + l * columns + c, op = NULL; j >= 0; j--, p--) {
        if (*p == mp)
            c = j;
        if (!first && *p != op)
            n++;
        op = *p;
        first = 0;
    }
    *cp = c;
    *lcp = 1;
    for (p = mtab + l * columns + c; c < columns; c++, p++)
        if (*p && mp != *p)
            *lcp = 0;

    return n;
}

static void
singledraw()
{
    Cmgroup g;
    int mc1, mc2, ml1, ml2, md1, md2, mcc1, mcc2, lc1, lc2, t1, t2;

    t1 = mline - mlbeg;
    t2 = moline - molbeg;

    if (t2 < t1) {
        mc1 = mocol; ml1 = moline; md1 = t2;
        mc2 = mcol; ml2 = mline; md2 = t1;
    } else {
        mc1 = mcol; ml1 = mline; md1 = t1;
        mc2 = mocol; ml2 = moline; md2 = t2;
    }
    mcc1 = singlecalc(&mc1, ml1, &lc1);
    mcc2 = singlecalc(&mc2, ml2, &lc2);

    if (md1)
        tc_downcurs(md1);
    if (mc1)
        tcmultout(TCRIGHT, TCMULTRIGHT, mc1);
    g = mgtab[ml1 * columns + mc1];
    clprintm(g, mtab[ml1 * columns + mc1], mcc1, ml1, lc1,
             (g->widths ? g->widths[mcc1] : g->width));
    putc('\r', shout);

    if (md2 != md1)
        tc_downcurs(md2 - md1);
    if (mc2)
        tcmultout(TCRIGHT, TCMULTRIGHT, mc2);
    g = mgtab[ml2 * columns + mc2];
    clprintm(g, mtab[ml2 * columns + mc2], mcc2, ml2, lc2,
             (g->widths ? g->widths[mcc2] : g->width));
    putc('\r', shout);

    if (mstatprinted) {
        int i = lines - md2 - nlnct;

        tc_downcurs(i - 1);
        compprintfmt(NULL, 0, 1, 1, mline, NULL);
        tcmultout(TCUP, TCMULTUP, lines - 1);
    } else
        tcmultout(TCUP, TCMULTUP, md2 + nlnct);

    showinglist = -1;
    listshown = 1;
}

static int
complistmatches(Hookdef dummy, Chdata dat)
{
    static int onlnct = -1;

    Cmgroup oamatches = amatches;

    amatches = dat->matches;

    noselect = 0;

    if ((minfo.asked == 2 && mselect < 0) || nlnct >= lines) {
	showinglist = 0;
	amatches = oamatches;
	return (noselect = 1);
    }
    getcols(&mcolors);

    mnew = ((calclist(mselect >= 0) || mlastcols != columns ||
	     mlastlines != listdat.nlines) && mselect >= 0);

    if (!listdat.nlines || (mselect >= 0 &&
			    !(isset(USEZLE) && !termflags &&
			      complastprompt && *complastprompt))) {
	showinglist = listshown = 0;
	noselect = 1;
	amatches = oamatches;
	return 1;
    }
    if (inselect || mlbeg >= 0)
	clearflag = 0;

    mscroll = 0;
    mlistp = NULL;

    queue_signals();
    if (mselect >= 0 || mlbeg >= 0 ||
	(mlistp = dupstring(getsparam("LISTPROMPT")))) {
	unqueue_signals();
	if (mlistp && !*mlistp)
	    mlistp = "%SAt %p: Hit TAB for more, or the character to insert%s";
	trashzle();
	showinglist = listshown = 0;

	lastlistlen = 0;

	if (mlistp) {
	    clearflag = (isset(USEZLE) && !termflags && dolastprompt);
	    mscroll = 1;
	} else {
	    clearflag = 1;
	    minfo.asked = (listdat.nlines + nlnct <= lines);
	}
    } else {
	unqueue_signals();
	mlistp = NULL;
	if (asklist()) {
	    amatches = oamatches;
	    return (noselect = 1);
	}
    }
    if (mlbeg >= 0) {
	mlend = mlbeg + lines - nlnct - mhasstat;
	while (mline >= mlend)
	    mlbeg++, mlend++;
    } else
	mlend = 9999999;

    if (mnew) {
	int i;

    	mtab_been_reallocated = 1;

	i = columns * listdat.nlines;
	free(mtab);
	mtab = (Cmatch **) zalloc(i * sizeof(Cmatch **));
	memset(mtab, 0, i * sizeof(Cmatch **));
	free(mgtab);
	mgtab = (Cmgroup *) zalloc(i * sizeof(Cmgroup));
	memset(mgtab, 0, i * sizeof(Cmgroup));
	mlastcols = mcols = columns;
	mlastlines = mlines = listdat.nlines;
    }
    last_cap = (char *) zhalloc(max_caplen + 1);
    *last_cap = '\0';

    if (!mnew && inselect && onlnct == nlnct && mlbeg >= 0 && mlbeg == molbeg)
        singledraw();
    else if (!compprintlist(mselect >= 0) || !clearflag)
	noselect = 1;

    onlnct = nlnct;
    molbeg = mlbeg;
    mocol = mcol;
    moline = mline;

    amatches = oamatches;

    return noselect;
}

static int
adjust_mcol(int wish, Cmatch ***tabp, Cmgroup **grp)
{
    Cmatch **tab = *tabp;
    int p, n, c;

    tab -= mcol;

    for (p = wish; p >= 0 && (!tab[p] || mmarked(tab[p])); p--);
    for (n = wish; n < mcols && (!tab[n] || mmarked(tab[n])); n++);
    if (n == mcols)
	n = -1;

    if (p < 0) {
	if (n < 0)
	    return 1;
	c = n;
    } else if (n < 0)
	c = p;
    else
	c = ((mcol - p) < (n - mcol) ? p : n);

    *tabp = tab + c;
    if (grp)
	*grp = *grp + c - mcol;

    mcol = c;
    
    return 0;
}

typedef struct menustack *Menustack;

struct menustack {
    Menustack prev;
    char *line;
    Brinfo brbeg;
    Brinfo brend;
    int nbrbeg, nbrend;
    int cs, acc, nmatches, mline, mlbeg, nolist;
    struct menuinfo info;
    Cmgroup amatches, pmatches, lastmatches, lastlmatches;
    char *origline;
    int origcs, origll;
    char *status;
    int mode;
};

typedef struct menusearch *Menusearch;

struct menusearch {
    Menusearch prev;
    char *str;
    int line;
    int col;
    int back;
    int state;
    Cmatch **ptr;
};

#define MS_OK       0
#define MS_FAILED   1
#define MS_WRAPPED  2

#define MAX_STATUS 128

static char *
setmstatus(char *status, char *sline, int sll, int scs,
           int *csp, int *llp, int *lenp)
{
    char *p, *s, *ret = NULL;
    int pl, sl, max;

    if (csp) {
        *csp = cs;
        *llp = ll;
        *lenp = lastend - wb;

        ret = dupstring((char *) line);

        p = (char *) zhalloc(cs - wb + 1);
        strncpy(p, (char *) line + wb, cs - wb);
        p[cs - wb] = '\0';
        if (lastend < cs)
            s = "";
        else {
            s = (char *) zhalloc(lastend - cs + 1);
            strncpy(s, (char *) line + cs, lastend - cs);
            s[lastend - cs] = '\0';
        }
        cs = 0;
        foredel(ll);
        spaceinline(sll);
        memcpy(line, sline, sll);
        cs = scs;
    } else {
        p = complastprefix;
        s = complastsuffix;
    }
    pl = strlen(p);
    sl = strlen(s);
    max = (columns < MAX_STATUS ? columns : MAX_STATUS) - 14;

    if (max > 12) {
        int h = (max - 2) >> 1;

        strcpy(status, "interactive: ");
        if (pl > h - 3) {
            strcat(status, "...");
            strcat(status, p + pl - h - 3);
        } else
            strcat(status, p);

        strcat(status, "[]");
        if (sl > h - 3) {
            strncat(status, s, h - 3);
            strcat(status, "...");
        } else
            strcat(status, s);
    }
    return ret;
}

static Menusearch msearchstack;
static char *msearchstr = NULL;
static int msearchstate;

static void
msearchpush(Cmatch **p, int back)
{
    Menusearch s = (Menusearch) zhalloc(sizeof(struct menusearch));

    s->prev = msearchstack;
    msearchstack = s;
    s->str = dupstring(msearchstr);
    s->line = mline;
    s->col = mcol;
    s->back = back;
    s->state = msearchstate;
    s->ptr = p;
}

static Cmatch **
msearchpop(int *backp)
{
    Menusearch s = msearchstack;

    if (s->prev)
        msearchstack = s->prev;

    msearchstr = s->str;
    mline = s->line;
    mcol = s->col;
    msearchstate = s->state;

    *backp = s->back;

    return s->ptr;
}

static Cmatch **
msearch(Cmatch **ptr, int ins, int back, int rep, int *wrapp)
{
    char s[2];
    Cmatch **p, *l = NULL, m;
    int x = mcol, y = mline;
    int ex, ey, wrap = 0, owrap = (msearchstate & MS_WRAPPED);

    msearchpush(ptr, back);

    if (ins) {
        s[0] = c;
        s[1] = '\0';

        msearchstr = dyncat(msearchstr, s);
    }
    if (back) {
        ex = mcols - 1;
        ey = -1;
    } else {
        ex = 0;
        ey = listdat.nlines;
    }
    p = mtab + (mline * mcols) + mcol;
    if (rep)
        l = *p;
    while (1) {
        if (!rep && mtunmark(*p) && *p != l) {
            l = *p;
            m = *mtunmark(*p);

            if (strstr((m->disp ? m->disp : m->str), msearchstr)) {
                mcol = x;
                mline = y;

                return p;
            }
        }
        rep = 0;

        if (back) {
            p--;
            if (--x < 0) {
                x = mcols - 1;
                y--;
            }
        } else {
            p++;
            if (++x == mcols) {
                x = 0;
                y++;
            }
        }
        if (x == ex && y == ey) {
            if (wrap) {
                msearchstate = MS_FAILED | owrap;
                break;
            }
            msearchstate |= MS_WRAPPED;

            if (back) {
                x = mcols - 1;
                y = listdat.nlines - 1;
                p = mtab + (y * mcols) + x;
            } else {
                x = y = 0;
                p = mtab;
            }
            ex = mcol;
            ey = mline;
            wrap = 1;
            *wrapp = 1;
        }
    }
    return NULL;
}

#define MM_INTER   1
#define MM_FSEARCH 2
#define MM_BSEARCH 3

static int
domenuselect(Hookdef dummy, Chdata dat)
{
    static Chdata fdat = NULL;
    static char *lastsearch = NULL;
    Cmatch **p;
    Cmgroup *pg;
    Thingy cmd;
    int     do_last_key = 0;
    Menustack u = NULL;
    int i = 0, acc = 0, wishcol = 0, setwish = 0, oe = onlyexpl, wasnext = 0;
    int space, lbeg = 0, step = 1, wrap, pl = nlnct, broken = 0, first = 1;
    int nolist = 0, mode = 0, modecs, modell, modelen;
    char *s;
    char status[MAX_STATUS], *modeline;

    msearchstack = NULL;
    msearchstr = "";
    msearchstate = MS_OK;

    status[0] = '\0';
    queue_signals();
    if (fdat || (dummy && (!(s = getsparam("MENUSELECT")) ||
			   (dat && dat->num < atoi(s))))) {
	if (fdat) {
	    fdat->matches = dat->matches;
	    fdat->num = dat->num;
	    fdat->nmesg = dat->nmesg;
	}
	unqueue_signals();
	return 0;
    }
    if ((s = getsparam("MENUSCROLL"))) {
	if (!(step = mathevali(s)))
	    step = (lines - nlnct) >> 1;
	else if (step < 0)
	    if ((step += lines - nlnct) < 0)
		step = 1;
    }
    if ((s = getsparam("MENUMODE"))) {
        if (!strcmp(s, "interactive")) {
            int l = strlen(origline);

            mode = MM_INTER;
            cs = 0;
            foredel(ll);
            spaceinline(l);
            strncpy((char *) line, origline, l);
            cs = origcs;
            setmstatus(status, NULL, 0 , 0, NULL, NULL, NULL);
        } else if (strpfx("search", s)) {
            mode = (strstr(s, "back") ? MM_BSEARCH : MM_FSEARCH);
        }
    }
    if ((mstatus = dupstring(getsparam("MENUPROMPT"))) && !*mstatus)
	mstatus = "%SScrolling active: current selection at %p%s";
    unqueue_signals();
    mhasstat = (mstatus && *mstatus);
    fdat = dat;
    selectlocalmap(mskeymap);
    noselect = 1;
    while ((menuacc &&
	    !hasbrpsfx(*(minfo.cur), minfo.prebr, minfo.postbr)) ||
	   ((*minfo.cur)->flags & CMF_DUMMY) ||
	   (((*minfo.cur)->flags & (CMF_NOLIST | CMF_MULT)) &&
	    (!(*minfo.cur)->str || !*(*minfo.cur)->str)))
	do_menucmp(0);

    mselect = (*(minfo.cur))->gnum;
    mline = 0;
    mlines = 999999;
    mlbeg = 0;
    molbeg = -42;
    for (;;) {
    	mtab_been_reallocated = 0;
	if (mline < 0) {
	    int x, y;
	    Cmatch **p = mtab;

	    for (y = 0; y < mlines; y++) {
		for (x = mcols; x; x--, p++)
		    if (*p && !mmarked(*p) && **p && mselect == (**p)->gnum)
			break;
		if (x) {
                    mcol = mcols - x;
		    break;
                }
	    }
	    if (y < mlines)
		mline = y;
	}
	while (mline < mlbeg)
	    if ((mlbeg -= step) < 0)
		mlbeg = 0;

	if (mlbeg && lbeg != mlbeg) {
	    Cmatch **p = mtab + ((mlbeg - 1) * columns), **q;
	    int c;

	    while (mlbeg) {
		for (q = p, c = columns; c; q++, c--)
		    if (*q && !mmarked(*q))
			break;
		if (c)
		    break;
		p -= columns;
		mlbeg--;
	    }
	}
	if ((space = lines - pl - mhasstat))
	    while (mline >= mlbeg + space)
		if ((mlbeg += step) + space > mlines)
		    mlbeg = mlines - space;
	if (lbeg != mlbeg) {
	    Cmatch **p = mtab + (mlbeg * columns), **q;
	    int c;

	    while (mlbeg < mlines) {
		for (q = p, c = columns; c; q++, c--)
		    if (*q)
			break;
		if (c)
		    break;
		p += columns;
		mlbeg++;
	    }
	}
	lbeg = mlbeg;
        onlyexpl = 0;
        showinglist = -2;
        if (first && !listshown && isset(LISTBEEP))
            zbeep();
        if (first) {
            modeline = dyncat(complastprefix, complastsuffix);
            modecs = cs;
            modell = ll;
            modelen = minfo.len;
        }
        first = 0;
        if (mode == MM_INTER) {
            statusline = status;
            statusll = strlen(status);
        } else if (mode) {
            int l = sprintf(status, "%s%sisearch%s: ",
                            ((msearchstate & MS_FAILED) ? "failed " : ""),
                            ((msearchstate & MS_WRAPPED) ? "wrapped " : ""),
                            (mode == MM_FSEARCH ? "" : " backward"));

            strncat(status, msearchstr, MAX_STATUS - l - 1);

            statusline = status;
            statusll = strlen(status);
        } else {
            statusline = NULL;
            statusll = 0;
        }
        zrefresh();
        statusline = NULL;
        statusll = 0;
        inselect = 1;
        if (noselect) {
            broken = 1;
            break;
        }
	selected = 1;
	if (!i) {
	    i = mcols * mlines;
	    while (i--)
		if (mtab[i])
		    break;
	    if (!i)
		break;
	    i = 1;
	}
	p = mmtabp;
	pg = mgtabp;
	minfo.cur = *p;
	minfo.group = *pg;
	if (setwish)
	    wishcol = mcol;
	else if (mcol > wishcol) {
	    while (mcol > 0 && p[-1] == minfo.cur)
		mcol--, p--, pg--;
	} else if (mcol < wishcol) {
	    while (mcol < mcols - 1 && p[1] == minfo.cur)
		mcol++, p++, pg++;
	}
	setwish = wasnext = 0;

    getk:

    	if (!do_last_key) {
	    cmd = getkeycmd();
	    if (mtab_been_reallocated) {
		do_last_key = 1;
		continue;
	    }
    	}
	do_last_key = 0;

	if (!cmd || cmd == Th(z_sendbreak)) {
	    zbeep();
            molbeg = -1;
	    break;
	} else if (nolist && cmd != Th(z_undo) &&
                   (!mode || (cmd != Th(z_backwarddeletechar) &&
                              cmd != Th(z_selfinsert) &&
                              cmd != Th(z_selfinsertunmeta)))) {
	    ungetkeycmd();
	    break;
	} else if (cmd == Th(z_acceptline)) {
            if (mode == MM_FSEARCH || mode == MM_BSEARCH) {
                mode = 0;
                continue;
            }
	    acc = 1;
	    break;
        } else if (cmd == Th(z_viinsert)) {
            if (mode == MM_INTER)
                mode = 0;
            else {
                int l = strlen(origline);

                mode = MM_INTER;
                cs = 0;
                foredel(ll);
                spaceinline(l);
                strncpy((char *) line, origline, l);
                cs = origcs;
                setmstatus(status, NULL, 0, 0, NULL, NULL, NULL);

                continue;
            }
	} else if (cmd == Th(z_acceptandinfernexthistory) ||
                   (mode == MM_INTER && (cmd == Th(z_selfinsert) ||
                                         cmd == Th(z_selfinsertunmeta)))) {
            char *saveline = NULL;
            int savell;
            int savecs;
	    Menustack s = (Menustack) zhalloc(sizeof(*s));

	    s->prev = u;
	    u = s;
	    s->line = dupstring((char *) line);
	    s->cs = cs;
	    s->mline = mline;
	    s->mlbeg = mlbeg;
	    memcpy(&(s->info), &minfo, sizeof(struct menuinfo));
	    s->amatches = amatches;
	    s->pmatches = pmatches;
	    s->lastmatches = lastmatches;
	    s->lastlmatches = lastlmatches;
            s->nolist = nolist;
	    s->acc = menuacc;
	    s->brbeg = dupbrinfo(brbeg, NULL, 1);
	    s->brend = dupbrinfo(brend, NULL, 1);
	    s->nbrbeg = nbrbeg;
	    s->nbrend = nbrend;
	    s->nmatches = nmatches;
	    s->origline = origline;
	    s->origcs = origcs;
	    s->origll = origll;
            s->status = dupstring(status);
            s->mode = mode;
	    menucmp = menuacc = hasoldlist = 0;
	    minfo.cur = NULL;
	    fixsuffix();
	    handleundo();
	    validlist = 0;
	    amatches = pmatches = lastmatches = NULL;
	    invalidate_list();
	    iforcemenu = 1;
	    comprecursive = 1;
            if (cmd != Th(z_acceptandinfernexthistory)) {
                int l = strlen(origline);

                cs = 0;
                foredel(ll);
                spaceinline(l);
                strncpy((char *) line, origline, l);
                cs = origcs;

                if (cmd == Th(z_selfinsert))
                    selfinsert(zlenoargs);
                else
                    selfinsertunmeta(zlenoargs);

                saveline = (char *) zhalloc(ll);
                memcpy(saveline, line, ll);
                savell = ll;
                savecs = cs;
                iforcemenu = -1;
            } else
                mode = 0;
	    menucomplete(zlenoargs);
	    iforcemenu = 0;

            if (cmd != Th(z_acceptandinfernexthistory))
                modeline = setmstatus(status, saveline, savell, savecs,
                                      &modecs, &modell, &modelen);

	    if (nmatches < 1 || !minfo.cur || !*(minfo.cur)) {
		nolist = 1;
                if (mode == MM_INTER) {
                    statusline = status;
                    statusll = strlen(status);
                }
		if (nmessages) {
		    showinglist = -2;
		    zrefresh();
		} else {
		    trashzle();
		    zsetterm();
		    if (tccan(TCCLEAREOD))
			tcout(TCCLEAREOD);
		    fputs("no matches\r", shout);
		    fflush(shout);
		    tcmultout(TCUP, TCMULTUP, nlnct);
		    showinglist = clearlist = 0;
		    clearflag = 1;
		    zrefresh();
		    showinglist = clearlist = 0;
		}
                statusline = NULL;
                statusll = 0;

		goto getk;
	    }
	    clearlist = listshown = 1;
	    mselect = (*(minfo.cur))->gnum;
	    setwish = wasnext = 1;
	    mline = 0;
            molbeg = -42;
	    continue;
	} else if (cmd == Th(z_acceptandhold) ||
		   cmd == Th(z_acceptandmenucomplete)) {
	    Menustack s = (Menustack) zhalloc(sizeof(*s));
	    int ol;

            mode = 0;
	    s->prev = u;
	    u = s;
	    s->line = dupstring((char *) line);
	    s->cs = cs;
	    s->mline = mline;
	    s->mlbeg = mlbeg;
	    memcpy(&(s->info), &minfo, sizeof(struct menuinfo));
	    s->amatches = s->pmatches =
		s->lastmatches = s->lastlmatches = NULL;
            s->nolist = nolist;
	    s->acc = menuacc;
	    s->brbeg = dupbrinfo(brbeg, NULL, 1);
	    s->brend = dupbrinfo(brend, NULL, 1);
	    s->nbrbeg = nbrbeg;
	    s->nbrend = nbrend;
	    s->nmatches = nmatches;
	    s->origline = origline;
	    s->origcs = origcs;
	    s->origll = origll;
            s->status = dupstring(status);
            s->mode = mode;
	    accept_last();
	    handleundo();
	    comprecursive = 1;
	    do_menucmp(0);
	    mselect = (*(minfo.cur))->gnum;

	    p -= mcol;
	    mcol = 0;
	    ol = mline;
	    do {
		for (mcol = 0; mcol < mcols; mcol++, p++)
		    if (*p == minfo.cur)
			break;
		if (mcol != mcols)
		    break;
		if (++mline == mlines) {
		    mline = 0;
		    p -= mlines * mcols;
		}
	    } while (mline != ol);
	    if (*p != minfo.cur) {
		noselect = clearlist = listshown = 1;
		onlyexpl = 0;
		zrefresh();
		break;
	    }
	    setwish = 1;
	    continue;
	} else if (cmd == Th(z_undo) ||
                   (mode == MM_INTER && cmd == Th(z_backwarddeletechar))) {
	    int l;

	    if (!u)
		break;

	    handleundo();
	    cs = 0;
	    foredel(ll);
	    spaceinline(l = strlen(u->line));
	    strncpy((char *) line, u->line, l);
	    cs = u->cs;
	    menuacc = u->acc;
	    memcpy(&minfo, &(u->info), sizeof(struct menuinfo));
	    p = &(minfo.cur);
	    mline = u->mline;
	    mlbeg = u->mlbeg;
	    if (u->lastmatches && lastmatches != u->lastmatches) {
		if (lastmatches)
		    freematches(lastmatches, 0);
		amatches = u->amatches;
		pmatches = u->pmatches;
		lastmatches = u->lastmatches;
		lastlmatches = u->lastlmatches;
		nmatches = u->nmatches;
		hasoldlist = validlist = 1;
	    }
	    freebrinfo(brbeg);
	    freebrinfo(brend);
	    brbeg = dupbrinfo(u->brbeg, &lastbrbeg, 0);
	    brend = dupbrinfo(u->brend, &lastbrend, 0);
	    nbrbeg = u->nbrbeg;
	    nbrend = u->nbrend;
	    origline = u->origline;
	    origcs = u->origcs;
	    origll = u->origll;
            strcpy(status, u->status);
            mode = u->mode;
            nolist = u->nolist;

	    u = u->prev;
	    clearlist = 1;
	    setwish = 1;
	    listdat.valid = 0;
            molbeg = -42;

            if (nolist) {
                if (mode == MM_INTER) {
                    statusline = status;
                    statusll = strlen(status);
                }
                zrefresh();
                statusline = NULL;
                statusll = 0;
                goto getk;
            }
            if (mode)
                continue;
	} else if (cmd == Th(z_redisplay)) {
	    redisplay(zlenoargs);
            molbeg = -42;
	    continue;
	} else if (cmd == Th(z_clearscreen)) {
	    clearscreen(zlenoargs);
            molbeg = -42;
	    continue;
	} else if (cmd == Th(z_downhistory) ||
		   cmd == Th(z_downlineorhistory) ||
		   cmd == Th(z_downlineorsearch) ||
		   cmd == Th(z_vidownlineorhistory)) {
	    int omline;
	    Cmatch **op;

            mode = 0;
	    wrap = 0;

	down:

	    omline = mline;
	    op = p;

	    do {
		if (mline == mlines - 1) {
		    if (wrap & 2) {
			mline = omline; 
			p = op;
			break;
		    }
		    p -= mline * mcols;
		    mline = 0;
		    wrap |= 1;
		} else {
		    mline++;
		    p += mcols;
		}
		if (adjust_mcol(wishcol, &p, NULL))
		    continue;
	    } while (!*p || mmarked(*p));

	    if (wrap == 1)
		goto right;
	} else if (cmd == Th(z_uphistory) ||
		   cmd == Th(z_uplineorhistory) ||
		   cmd == Th(z_uplineorsearch) ||
		   cmd == Th(z_viuplineorhistory)) {
	    int omline;
	    Cmatch **op;

            mode = 0;
	    wrap = 0;

	up:

	    omline = mline;
	    op = p;

	    do {
		if (!mline) {
		    if (wrap & 2) {
			mline = omline; 
			p = op;
			break;
		    }
		    mline = mlines - 1;
		    p += mline * mcols;
		    wrap |= 1;
		} else {
		    mline--;
		    p -= mcols;
		}
		if (adjust_mcol(wishcol, &p, NULL))
		    continue;
	    } while (!*p || mmarked(*p));

	    if (wrap == 1) {
		if (mcol == wishcol)
		    goto left;

		wishcol = mcol;
	    }
	} else if (cmd == Th(z_emacsforwardword) ||
		   cmd == Th(z_viforwardword) ||
		   cmd == Th(z_viforwardwordend) ||
		   cmd == Th(z_forwardword)) {
	    int i = lines - pl - 1, oi = i, ll = 0;
	    Cmatch **lp = NULL;

            mode = 0;
	    if (mline == mlines - 1)
		goto top;
	    while (i > 0) {
		if (mline == mlines - 1) {
		    if (i != oi && lp)
			break;
		    goto top;
		} else {
		    mline++;
		    p += mcols;
		}
		if (adjust_mcol(wishcol, &p, NULL))
		    continue;
		if (*p && !mmarked(*p)) {
		    i--;
		    lp = p;
		    ll = mline;
		}
	    }
	    p = lp;
	    mline = ll;
	} else if (cmd == Th(z_emacsbackwardword) ||
		   cmd == Th(z_vibackwardword) ||
		   cmd == Th(z_backwardword)) {
	    int i = lines - pl - 1, oi = i, ll = 0;
	    Cmatch **lp = NULL;

            mode = 0;
	    if (!mline)
		goto bottom;
	    while (i > 0) {
		if (!mline) {
		    if (i != oi && lp)
			break;
		    goto bottom;
		} else {
		    mline--;
		    p -= mcols;
		}
		if (adjust_mcol(wishcol, &p, NULL))
		    continue;
		if (*p || !mmarked(*p)) {
		    i--;
		    lp = p;
		    ll = mline;
		}
	    }
	    p = lp;
	    mline = ll;
	} else if (cmd == Th(z_beginningofhistory)) {
	    int ll;
	    Cmatch **lp;

            mode = 0;

	top:

	    ll = mline;
	    lp = p;
	    while (mline) {
		mline--;
		p -= mcols;
		if (adjust_mcol(wishcol, &p, NULL))
		    continue;
		if (*p && !mmarked(*p)) {
		    lp = p;
		    ll = mline;
		}
	    }
	    mline = ll;
	    p = lp;
	} else if (cmd == Th(z_endofhistory)) {
	    int ll;
	    Cmatch **lp;

            mode = 0;

	bottom:

	    ll = mline;
	    lp = p;
	    while (mline < mlines - 1) {
		mline++;
		p += mcols;
		if (adjust_mcol(wishcol, &p, NULL))
		    continue;
		if (*p && !mmarked(*p)) {
		    lp = p;
		    ll = mline;
		}
	    }
	    mline = ll;
	    p = lp;
	} else if (cmd == Th(z_forwardchar) || cmd == Th(z_viforwardchar)) {
	    int omcol;
	    Cmatch **op;

            mode = 0;
	    wrap = 0;

	right:

	    omcol = mcol;
	    op = p;

	    do {
		if (mcol == mcols - 1) {
		    if (wrap & 1) {
			p = op;
			mcol = omcol;
			break;
		    }
		    p -= mcol;
		    mcol = 0;
		    wrap |= 2;
		} else {
		    mcol++;
		    p++;
		}
	    } while (!*p || mmarked(*p) || (mcol != omcol && *p == *op));
	    wishcol = mcol;

	    if (wrap == 2)
		goto down;
	} else if (cmd == Th(z_backwardchar) || cmd == Th(z_vibackwardchar)) {
	    int omcol;
	    Cmatch **op;

            mode = 0;
	    wrap = 0;

	left:

	    omcol = mcol;
	    op = p;

	    do {
		if (!mcol) {
		    if (wrap & 1) {
			p = op;
			mcol = omcol;
			break;
		    }
		    mcol = mcols - 1;
		    p += mcol;
		    wrap |= 2;
		} else {
		    mcol--;
		    p--;
		}
	    } while (!*p || mmarked(*p) || (mcol != omcol && *p == *op));
	    wishcol = mcol;

	    if (wrap == 2) {
		p += mcols - 1 - mcol;
		wishcol = mcol = mcols - 1;
		adjust_mcol(wishcol, &p, NULL);
		goto up;
	    }
	} else if (cmd == Th(z_beginningofbufferorhistory) ||
		   cmd == Th(z_beginningofline) ||
		   cmd == Th(z_beginningoflinehist) ||
		   cmd == Th(z_vibeginningofline)) {
            mode = 0;
	    p -= mcol;
	    mcol = 0;
	    while (!*p || mmarked(*p)) {
		mcol++;
		p++;
	    }
	    wishcol = 0;
	} else if (cmd == Th(z_endofbufferorhistory) ||
		   cmd == Th(z_endofline) ||
		   cmd == Th(z_endoflinehist) ||
		   cmd == Th(z_viendofline)) {
            mode = 0;
	    p += mcols - mcol - 1;
	    mcol = mcols - 1;
	    while (!*p || mmarked(*p)) {
		mcol--;
		p--;
	    }
	    wishcol = mcols - 1;
	} else if (cmd == Th(z_viforwardblankword) ||
		   cmd == Th(z_viforwardblankwordend)) {
	    Cmgroup g = *pg;
	    int ol = mline;

            mode = 0;
	    do {
		if (mline == mlines - 1) {
		    p -= mline * mcols;
		    pg -= mline * mcols;
		    mline = 0;
		} else {
		    mline++;
		    p += mcols;
		    pg += mcols;
		}
		if (adjust_mcol(wishcol, &p, &pg))
		    continue;
	    } while (ol != mline && (*pg == g || !*pg || mmarked(*pg)));
	} else if (cmd == Th(z_vibackwardblankword)) {
	    Cmgroup g = *pg;
	    int ol = mline;

            mode = 0;
	    do {
		if (!mline) {
		    mline = mlines - 1;
		    p += mline * mcols;
		    pg += mline * mcols;
		} else {
		    mline--;
		    p -= mcols;
		    pg -= mcols;
		}
		if (adjust_mcol(wishcol, &p, &pg))
		    continue;
	    } while (ol != mline && (*pg == g || !*pg || mmarked(*pg)));
	} else if (cmd == Th(z_completeword) ||
		   cmd == Th(z_expandorcomplete) ||
		   cmd == Th(z_expandorcompleteprefix) ||
		   cmd == Th(z_menucomplete) ||
		   cmd == Th(z_menuexpandorcomplete) ||
		   !strcmp(cmd->nam, "menu-select") ||
		   !strcmp(cmd->nam, "complete-word") ||
		   !strcmp(cmd->nam, "expand-or-complete") ||
		   !strcmp(cmd->nam, "expand-or-complete-prefix") ||
		   !strcmp(cmd->nam, "menu-complete") ||
		   !strcmp(cmd->nam, "menu-expand-or-complete")) {
            if (mode == MM_INTER) {
                origline = modeline;
                origcs = modecs;
                origll = modell;
                cs = 0;
                foredel(ll);
                spaceinline(origll);
                strncpy((char *) line, origline, origll);
                cs = origcs;
                minfo.len = modelen;
            } else {
                mode = 0;
                comprecursive = 1;
                do_menucmp(0);
                mselect = (*(minfo.cur))->gnum;
                setwish = 1;
                mline = -1;
            }
	    continue;
	} else if (cmd == Th(z_reversemenucomplete) ||
		   !strcmp(cmd->nam, "reverse-menu-complete")) {
            mode = 0;
	    comprecursive = 1;
	    reversemenucomplete(zlenoargs);
	    mselect = (*(minfo.cur))->gnum;
	    setwish = 1;
	    mline = -1;
	    continue;
        } else if (cmd == Th(z_historyincrementalsearchforward) ||
                   cmd == Th(z_historyincrementalsearchbackward) ||
                   ((mode == MM_FSEARCH || mode == MM_BSEARCH) &&
                    (cmd == Th(z_selfinsert) ||
                     cmd == Th(z_selfinsertunmeta)))) {
            Cmatch **np, **op = p;
            int was = (mode == MM_FSEARCH || mode == MM_BSEARCH);
            int ins = (cmd == Th(z_selfinsert) || cmd == Th(z_selfinsertunmeta));
            int back = (cmd == Th(z_historyincrementalsearchbackward));
            int wrap;

            do {
                if (was) {
                    p += wishcol - mcol;
                    mcol = wishcol;
                }
                if (!ins) {
                    if (was) {
                        if (!*msearchstr && lastsearch) {
                            msearchstr = dupstring(lastsearch);
                            mode = 0;
                        }
                    } else {
                        msearchstr = "";
                        msearchstack = NULL;
                    }
                }
                if (cmd == Th(z_selfinsertunmeta)) {
                    c &= 0x7f;
                    if (c == '\r')
                        c = '\n';
                }
                wrap = 0;
                np = msearch(p, ins, (ins ? (mode == MM_BSEARCH) : back),
                             (was && !ins), &wrap);

                if (!ins)
                    mode = (back ? MM_BSEARCH : MM_FSEARCH);

                if (*msearchstr) {
                    zsfree(lastsearch);
                    lastsearch = ztrdup(msearchstr);
                }
                if (np) {
                    wishcol = mcol;
                    p = np;
                }
                adjust_mcol(wishcol, &p, NULL);

            } while ((back || cmd == Th(z_historyincrementalsearchforward)) &&
                     np && !wrap && was && **p == **op);

        } else if ((mode == MM_FSEARCH || mode == MM_BSEARCH) &&
                   cmd == Th(z_backwarddeletechar)) {
            int back;
            Cmatch **np = msearchpop(&back);

            mode = (back ? MM_BSEARCH : MM_FSEARCH);
            wishcol = mcol;
            if (np) {
                p = np;
                adjust_mcol(wishcol, &p, NULL);
            }
	} else if (cmd == Th(z_undefinedkey)) {
            mode = 0;
	    continue;
	} else {
	    ungetkeycmd();
	    if (cmd->widget && (cmd->widget->flags & WIDGET_NCOMP)) {
		acc = 0;
		broken = 2;
	    } else
		acc = 1;
	    break;
	}
	do_single(**p);
	mselect = (**p)->gnum;
    }
    if (u)
	for (; u; u = u->prev)
	    if (u->lastmatches != lastmatches)
		freematches(u->lastmatches, 0);

    selectlocalmap(NULL);
    mselect = mlastcols = mlastlines = -1;
    mstatus = NULL;
    inselect = mhasstat = 0;
    if (nolist)
        clearlist = listshown = 1;
    if (acc && validlist && minfo.cur) {
	menucmp = lastambig = hasoldlist = 0;
	do_single(*(minfo.cur));
    }
    if (wasnext || broken) {
	menucmp = 2;
	showinglist = ((validlist && !nolist) ? -2 : 0);
	minfo.asked = 0;
	if (!noselect) {
	    int nos = noselect;

	    zrefresh();
	    noselect = nos;
	}
    }
    if (!noselect && (!dat || acc)) {
	showinglist = ((validlist && !nolist) ? -2 : 0);
	onlyexpl = oe;
	if (!smatches)
	    clearlist = listshown = 1;
	zrefresh();
    }
    mlbeg = -1;
    fdat = NULL;

    return (broken == 2 ? 3 :
	    ((dat && !broken) ? (acc ? 1 : 2) : (!noselect ^ acc)));
}

/* The widget function. */

static int
menuselect(char **args)
{
    int d = 0;

    if (!minfo.cur) {
	selected = 0;
	menucomplete(args);
	if ((minfo.cur && minfo.asked == 2) || selected)
	    return 0;
	d = 1;
    }
    if (minfo.cur && (minfo.asked == 2 || domenuselect(NULL, NULL)) && !d)
	menucomplete(args);

    return 0;
}

/**/
int
setup_(Module m)
{
    return 0;
}

/**/
int
boot_(Module m)
{
    mtab = NULL;
    mgtab = NULL;
    mselect = -1;
    inselect = 0;

    w_menuselect = addzlefunction("menu-select", menuselect,
                                    ZLE_MENUCMP|ZLE_KEEPSUFFIX|ZLE_ISCOMP);
    if (!w_menuselect) {
	zwarnnam(m->nam, "name clash when adding ZLE function `menu-select'",
		 NULL, 0);
	return -1;
    }
    addhookfunc("comp_list_matches", (Hookfn) complistmatches);
    addhookfunc("menu_start", (Hookfn) domenuselect);
    mskeymap = newkeymap(NULL, "menuselect");
    linkkeymap(mskeymap, "menuselect", 1);
    bindkey(mskeymap, "\t", refthingy(t_completeword), NULL);
    bindkey(mskeymap, "\n", refthingy(t_acceptline), NULL);
    bindkey(mskeymap, "\r", refthingy(t_acceptline), NULL);
    bindkey(mskeymap, "\33[A",  refthingy(t_uplineorhistory), NULL);
    bindkey(mskeymap, "\33[B",  refthingy(t_downlineorhistory), NULL);
    bindkey(mskeymap, "\33[C",  refthingy(t_forwardchar), NULL);
    bindkey(mskeymap, "\33[D",  refthingy(t_backwardchar), NULL);
    bindkey(mskeymap, "\33OA",  refthingy(t_uplineorhistory), NULL);
    bindkey(mskeymap, "\33OB",  refthingy(t_downlineorhistory), NULL);
    bindkey(mskeymap, "\33OC",  refthingy(t_forwardchar), NULL);
    bindkey(mskeymap, "\33OD",  refthingy(t_backwardchar), NULL);
    lskeymap = newkeymap(NULL, "listscroll");
    linkkeymap(lskeymap, "listscroll", 1);
    bindkey(lskeymap, "\t", refthingy(t_completeword), NULL);
    bindkey(lskeymap, " ", refthingy(t_completeword), NULL);
    bindkey(lskeymap, "\n", refthingy(t_acceptline), NULL);
    bindkey(lskeymap, "\r", refthingy(t_acceptline), NULL);
    bindkey(lskeymap, "\33[B",  refthingy(t_downlineorhistory), NULL);
    bindkey(lskeymap, "\33OB",  refthingy(t_downlineorhistory), NULL);
    return 0;
}

/**/
int
cleanup_(Module m)
{
    free(mtab);
    free(mgtab);

    deletezlefunction(w_menuselect);
    deletehookfunc("comp_list_matches", (Hookfn) complistmatches);
    deletehookfunc("menu_start", (Hookfn) domenuselect);
    unlinkkeymap("menuselect", 1);
    unlinkkeymap("listscroll", 1);
    return 0;
}

/**/
int
finish_(Module m)
{
    return 0;
}

/* linenoise.cpp -- guerrilla line editing library against the idea that a
 * line editing lib needs to be 20,000 lines of C code.
 *
 * You can find the latest source code at:
 * 
 *   http://github.com/antirez/linenoise
 */
#include <string>
#include <termios.h>
#include <assert.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "string_fmt.h"
#include "linenoise.h"

using namespace std;

#define LINENOISE_DEFAULT_HISTORY_MAX_LEN 100
#define LINENOISE_MAX_LINE 4096
static const char *unsupported_term[] = {"dumb","cons25","emacs",NULL};
static linenoiseCompletionCallback *completionCallback = NULL;

static struct termios orig_termios; /* In order to restore at exit.*/
static int rawmode = 0; /* For atexit() function to check if restore is needed*/
static int mlmode = 0;  /* Multi line mode. Default is single line. */
static int atexit_registered = 0; /* Register atexit just 1 time. */
static int history_max_len = LINENOISE_DEFAULT_HISTORY_MAX_LEN;
static int history_len = 0;
static char **history = NULL;

/* The linenoiseState structure represents the state during line editing.
 * We pass this state to functions implementing specific editing
 * functionalities. */
struct linenoiseState {
    int ifd;            /* Terminal stdin file descriptor. */
    int ofd;            /* Terminal stdout file descriptor. */
    char *buf;          /* Edited line buffer. */
    size_t buflen;      /* Edited line buffer size. */
    const char *prompt; /* Prompt to display. */
    size_t plen;        /* Prompt length. */
    size_t pos;         /* Current cursor position. */
    size_t oldpos;      /* Previous refresh cursor position. */
    size_t len;         /* Current edited line length. */
    size_t cols;        /* Number of columns in terminal. */
    size_t maxrows;     /* Maximum num of rows used so far (multiline mode) */
    int history_index;  /* The history index we are currently editing. */
};

enum KEY_ACTION{
	KEY_NULL = 0,	    /* NULL */
	CTRL_A = 1,         /* Ctrl+a */
	CTRL_B = 2,         /* Ctrl-b */
	CTRL_C = 3,         /* Ctrl-c */
	CTRL_D = 4,         /* Ctrl-d */
	CTRL_E = 5,         /* Ctrl-e */
	CTRL_F = 6,         /* Ctrl-f */
	CTRL_H = 8,         /* Ctrl-h */
	TAB = 9,            /* Tab */
	CTRL_K = 11,        /* Ctrl+k */
	CTRL_L = 12,        /* Ctrl+l */
	ENTER = 13,         /* Enter */
	CTRL_N = 14,        /* Ctrl-n */
	CTRL_P = 16,        /* Ctrl-p */
	CTRL_T = 20,        /* Ctrl-t */
	CTRL_U = 21,        /* Ctrl+u */
	CTRL_W = 23,        /* Ctrl+w */
	ESC = 27,           /* Escape */
	BACKSPACE =  127    /* Backspace */
};

static void linenoiseAtExit(void);
int linenoiseHistoryAdd(const char *line);
static void refreshLine(struct linenoiseState *l);
static void refreshSingleLine(struct linenoiseState *l);

/* Debugging macro. */
#if 0
FILE *lndebug_fp = NULL;
#define lndebug(...) \
    do { \
        if (lndebug_fp == NULL) { \
            lndebug_fp = fopen("/tmp/lndebug.txt","a"); \
            fprintf(lndebug_fp, \
            "[%d %d %d] p: %d, rows: %d, rpos: %d, max: %d, oldmax: %d\n", \
            (int)l->len,(int)l->pos,(int)l->oldpos,plen,rows,rpos, \
            (int)l->maxrows,old_rows); \
        } \
        fprintf(lndebug_fp, ", " __VA_ARGS__); \
        fflush(lndebug_fp); \
    } while (0)
#else
#define lndebug(fmt, ...)
#endif

/* ======================= Low level terminal handling ====================== */

/* Set if to use or not the multi line mode. */
void
linenoiseSetMultiLine (int ml)
{
    mlmode = ml;
}

/* Return true if the terminal name is in the list of terminals we know are
 * not able to understand basic escape sequences. */
static int
isUnsupportedTerm (void)
{
    char *term = getenv("TERM");
    int j;

    if (term == NULL) return 0;
    for (j = 0; unsupported_term[j]; j++)
        if (!strcasecmp(term, unsupported_term[j])) return 1;
    return 0;
}

/* Raw mode: 1960 magic shit. */
static int
enableRawMode(int fd)
{
    struct termios raw;

    if (!isatty(STDIN_FILENO)) goto fatal;
    if (!atexit_registered) {
        atexit(linenoiseAtExit);
        atexit_registered = 1;
    }
    if (tcgetattr(fd,&orig_termios) == -1) goto fatal;

    raw = orig_termios;  /* modify the original mode */
    /* input modes: no break, no CR to NL, no parity check, no strip char,
     * no start/stop output control. */
    raw.c_iflag &= ~(BRKINT | ICRNL | INPCK | ISTRIP | IXON);
    /* output modes - disable post processing */
    raw.c_oflag &= ~(OPOST);
    /* control modes - set 8 bit chars */
    raw.c_cflag |= (CS8);
    /* local modes - choing off, canonical off, no extended functions,
     * no signal chars (^Z,^C) */
    raw.c_lflag &= ~(ECHO | ICANON | IEXTEN | ISIG);
    /* control chars - set return condition: min number of bytes and timer.
     * We want read to return every single byte, without timeout. */
    raw.c_cc[VMIN] = 1; raw.c_cc[VTIME] = 0; /* 1 byte, no timer */

    /* put terminal in raw mode after flushing */
    if (tcsetattr(fd,TCSAFLUSH,&raw) < 0) goto fatal;
    rawmode = 1;
    return 0;

fatal:
    errno = ENOTTY;
    return -1;
}

static void
disableRawMode (int fd)
{
    /* Don't even check the return value as it's too late. */
    if (rawmode && tcsetattr(fd,TCSAFLUSH,&orig_termios) != -1)
        rawmode = 0;
}

/* Use the ESC [6n escape sequence to query the horizontal cursor position
 * and return it. On error -1 is returned, on success the position of the
 * cursor. */
static int
getCursorPosition (int ifd, int ofd)
{
    char buf[32];
    int cols, rows;
    unsigned int i = 0;

    /* Report cursor location */
    if (write(ofd, "\x1b[6n", 4) != 4) return -1;

    /* Read the response: ESC [ rows ; cols R */
    while (i < sizeof(buf)-1) {
        if (read(ifd,buf+i,1) != 1) break;
        if (buf[i] == 'R') break;
        i++;
    }
    buf[i] = '\0';

    /* Parse it. */
    if (buf[0] != ESC || buf[1] != '[') return -1;
    if (sscanf(buf+2,"%d;%d",&rows,&cols) != 2) return -1;
    return cols;
}

static void
get_col_row (unsigned int &cols, unsigned int &rows)
{
    struct winsize ws;

    if (ioctl(1, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
	/* handle this case later */
	assert(0);
    }

    cols = ws.ws_col;
    rows = ws.ws_row;
}

/* Try to get the number of columns in the current terminal, or assume 80
 * if it fails. */
static int
getColumns (int ifd, int ofd)
{
    struct winsize ws;

    if (ioctl(1, TIOCGWINSZ, &ws) == -1 || ws.ws_col == 0) {
        /* ioctl() failed. Try to query the terminal itself. */
        int start, cols;

        /* Get the initial position so we can restore it later. */
        start = getCursorPosition(ifd,ofd);
        if (start == -1) goto failed;

        /* Go to right margin and get position. */
        if (write(ofd,"\x1b[999C",6) != 6) goto failed;
        cols = getCursorPosition(ifd,ofd);
        if (cols == -1) goto failed;

        /* Restore position. */
        if (cols > start) {
	    string_fmt_c s;

	    s.format("\x1b[%dD",cols-start);
            if (write(ofd, s.c_str(), s.size()) == -1) {
                /* Can't recover... */
            }
        }
        return cols;
    } else {
        return ws.ws_col;
    }

failed:
    return 80;
}

/* Clear the screen. Used to handle ctrl+l */
void
linenoiseClearScreen (void)
{
    if (write(STDOUT_FILENO,"\x1b[H\x1b[2J",7) <= 0) {
        /* nothing to do, just to avoid warning. */
    }
}

/* Beep, used for completion when there is nothing to complete or when all
 * the choices were already shown. */
static void
linenoiseBeep(void)
{
    fprintf(stderr, "\x7");
    fflush(stderr);
}

/* ============================== Completion ================================ */

/* Free a list of completion option populated by linenoiseAddCompletion(). */
static void
freeCompletions (linenoiseCompletions *lc)
{
    size_t i;
    for (i = 0; i < lc->len; i++)
        free(lc->cvec[i]);
    if (lc->cvec != NULL)
        free(lc->cvec);
    for (i = 0; i < lc->len; i++) {
	if (lc->hvec[i]) free(lc->hvec[i]);
    }
   if (lc->hvec != NULL)
        free(lc->hvec);
 }

static int
helpLine (struct linenoiseState *ls)
{
    unsigned int max_cols, max_rows;
    
    get_col_row(max_cols, max_rows);

    linenoiseCompletions lc = { 0, NULL, NULL };

    completionCallback(ls->buf,&lc);

    if (lc.len == 0) {
	printf("\r\n *no-match*");
    } else {
        unsigned int i = 0;

	// maximum rows to display is the screen rows less
	// 1 for prompt and 1 for the '... more ...' msg
	if (max_rows > 1) max_rows -= 2;
			      
	for (i = 0; i < lc.len && i < max_rows; i++) {
	    printf("\r\n %-20s %s", lc.cvec[i], lc.hvec[i]);
        }
	if (lc.len >= max_rows) {
	    printf("\r\n      ... more ...");
	}
    }

    printf("\r\n");
    refreshSingleLine(ls);
    fflush(stdout);
    freeCompletions(&lc);
    return 0;
}

/* This is an helper function for linenoiseEdit() and is called when the
 * user types the <tab> key in order to complete the string currently in the
 * input.
 * 
 * The state of the editing is encapsulated into the pointed linenoiseState
 * structure as described in the structure definition. */
static int
completeLine (struct linenoiseState *ls)
{
    linenoiseCompletions lc = { 0, NULL, NULL };
    int nread, nwritten;
    char c = 0;

    completionCallback(ls->buf,&lc);
    if (lc.len == 0) {
        linenoiseBeep();
    } else {
        size_t stop = 0, i = 0;

        while(!stop) {
            /* Show completion or original buffer */
            if (i < lc.len) {
                struct linenoiseState saved = *ls;

                ls->len = ls->pos = strlen(lc.cvec[i]);
                ls->buf = lc.cvec[i];
                refreshLine(ls);
                ls->len = saved.len;
                ls->pos = saved.pos;
                ls->buf = saved.buf;
            } else {
                refreshLine(ls);
            }

            nread = read(ls->ifd,&c,1);
            if (nread <= 0) {
                freeCompletions(&lc);
                return -1;
            }

            switch(c) {
                case TAB:
                    i = (i+1) % (lc.len+1);
                    if (i == lc.len) linenoiseBeep();
                    break;
                case ESC:
                    /* Re-show original buffer */
                    if (i < lc.len) refreshLine(ls);
                    stop = 1;
                    break;
                default:
                    /* Update buffer and return */
                    if (i < lc.len) {
                        nwritten = snprintf(ls->buf,ls->buflen,"%s",lc.cvec[i]);
                        ls->len = ls->pos = nwritten;
                    }
                    stop = 1;
                    break;
            }
        }
    }

    freeCompletions(&lc);
    return c; /* Return last read character */
}

/* Register a callback function to be called for tab-completion. */
void
linenoiseSetCompletionCallback (linenoiseCompletionCallback *fn)
{
    completionCallback = fn;
}

/* This function is used by the callback function registered by the user
 * in order to add completion options given the input string when the
 * user typed <tab>. See the example.c source code for a very easy to
 * understand example. */
void
linenoiseAddCompletion (linenoiseCompletions *lc,
			const char *str, const char *help)
{
    char **cvec, **hvec;

    cvec = (char **) realloc(lc->cvec,sizeof(char*)*(lc->len+1));
    if (cvec == NULL) return;
    lc->cvec = cvec;
    lc->cvec[lc->len] = strdup(str);

    hvec = (char **) realloc(lc->hvec,sizeof(char*)*(lc->len+1));
    if (hvec == NULL) return;
    lc->hvec = hvec;
    lc->hvec[lc->len] = strdup(help);

    lc->len++;

}

/* =========================== Line editing ================================= */


/* Single line low level line refresh.
 *
 * Rewrite the currently edited line accordingly to the buffer content,
 * cursor position, and number of columns of the terminal. */
static void
refreshSingleLine (struct linenoiseState *l)
{
    size_t plen = strlen(l->prompt);
    int fd = l->ofd;
    char *buf = l->buf;
    size_t len = l->len;
    size_t pos = l->pos;
    string_fmt_c ab;
    
    
    while((plen+pos) >= l->cols) {
        buf++;
        len--;
        pos--;
    }
    while (plen+len > l->cols) {
        len--;
    }

    /* Cursor to left edge */
    ab = "\x1b[0G";

    /* Write the prompt and the current buffer content */
    ab += l->prompt;
    ab += buf;

    /* Erase to right */
    ab += "\x1b[0K";

    /* Move cursor to original position. */
    ab.append("\x1b[0G\x1b[%dC", (int) (pos + plen));

    if (write(fd, ab.c_str(), ab.size()) == -1) {} /* Can't recover from write error. */
}

/* Multi line low level line refresh.
 *
 * Rewrite the currently edited line accordingly to the buffer content,
 * cursor position, and number of columns of the terminal. */
static void
refreshMultiLine(struct linenoiseState *l)
{
    int plen = strlen(l->prompt);
    int rows = (plen+l->len+l->cols-1)/l->cols; /* rows used by current buf. */
    int rpos = (plen+l->oldpos+l->cols)/l->cols; /* cursor relative row. */
    int rpos2; /* rpos after refresh. */
    int old_rows = l->maxrows;
    int fd = l->ofd, j;
    string_fmt_c ab;

    /* Update maxrows if needed. */
    if (rows > (int)l->maxrows) l->maxrows = rows;

    /* First step: clear all the lines used before. To do so start by
     * going to the last row. */

    if (old_rows-rpos > 0) {
        lndebug("go down %d", old_rows-rpos);
        ab.append("\x1b[%dB", old_rows-rpos);
    }

    /* Now for every row clear it, go up. */
    for (j = 0; j < old_rows-1; j++) {
        lndebug("clear+up");
        ab += "\x1b[0G\x1b[0K\x1b[1A";
    }

    /* Clean the top line. */
    lndebug("clear");
    ab += "\x1b[0G\x1b[0K";
    
    /* Write the prompt and the current buffer content */
    ab += l->prompt;
    ab += l->buf;


    /* If we are at the very end of the screen with our prompt, we need to
     * emit a newline and move the prompt to the first column. */
    if (l->pos &&
        l->pos == l->len &&
        (l->pos+plen) % l->cols == 0)
    {
        lndebug("<newline>");
	ab += "\n";
	ab += "\x1b[0G";
        rows++;
        if (rows > (int)l->maxrows) l->maxrows = rows;
    }

    /* Move cursor to right position. */
    rpos2 = (plen+l->pos+l->cols)/l->cols; /* current cursor relative row. */
    lndebug("rpos2 %d", rpos2);

    /* Go up till we reach the expected positon. */
    if (rows-rpos2 > 0) {
        lndebug("go-up %d", rows-rpos2);
        ab.append("\x1b[%dA", rows-rpos2);
    }

    /* Set column. */
    lndebug("set col %d", 1+((plen+(int)l->pos) % (int)l->cols));
    ab.append("\x1b[%dG", 1+((plen+(int)l->pos) % (int)l->cols));

    lndebug("\n");
    l->oldpos = l->pos;

    if (write(fd, ab.c_str(), ab.size()) == -1) {} /* Can't recover from write error. */
}

/* Calls the two low level functions refreshSingleLine() or
 * refreshMultiLine() according to the selected mode. */
static void
refreshLine(struct linenoiseState *l)
{
    if (mlmode)
        refreshMultiLine(l);
    else
        refreshSingleLine(l);
}

/* Insert the character 'c' at cursor current position.
 *
 * On error writing to the terminal -1 is returned, otherwise 0. */
int
linenoiseEditInsert (struct linenoiseState *l, char c)
{
    if (l->len < l->buflen) {
        if (l->len == l->pos) {
            l->buf[l->pos] = c;
            l->pos++;
            l->len++;
            l->buf[l->len] = '\0';
            if ((!mlmode && l->plen+l->len < l->cols) /* || mlmode */) {
                /* Avoid a full update of the line in the
                 * trivial case. */
                if (write(l->ofd,&c,1) == -1) return -1;
            } else {
                refreshLine(l);
            }
        } else {
            memmove(l->buf+l->pos+1,l->buf+l->pos,l->len-l->pos);
            l->buf[l->pos] = c;
            l->len++;
            l->pos++;
            l->buf[l->len] = '\0';
            refreshLine(l);
        }
    }
    return 0;
}

/* Move cursor on the left. */
void
linenoiseEditMoveLeft (struct linenoiseState *l)
{
    if (l->pos > 0) {
        l->pos--;
        refreshLine(l);
    }
}

/* Move cursor on the right. */
void
linenoiseEditMoveRight (struct linenoiseState *l)
{
    if (l->pos != l->len) {
        l->pos++;
        refreshLine(l);
    }
}

static int
isWordSep (int ch)
{
    return !isalnum(ch);
}

void
linenoiseEditMoveLeftWord (struct linenoiseState *l)
{
    if (l->pos == 0) return;

    l->pos--;

    while (l->pos > 0 && isWordSep(l->buf[l->pos]))
	l->pos--;

    while (l->pos > 0 && !isWordSep(l->buf[l->pos-1]))
	l->pos--;

    refreshLine(l);
}

/* Move word to the right */
void
linenoiseEditMoveRightWord (struct linenoiseState *l)
{
    if (l->pos == l->len) return;

    l->pos++;

    while (l->pos != l->len && isWordSep(l->buf[l->pos]))
	l->pos++;

    while (l->pos != l->len && !isWordSep(l->buf[l->pos]))
	l->pos++;

    refreshLine(l);
}


/* Move cursor to the start of the line. */
void
linenoiseEditMoveHome (struct linenoiseState *l)
{
    if (l->pos != 0) {
        l->pos = 0;
        refreshLine(l);
    }
}

/* Move cursor to the end of the line. */
void
linenoiseEditMoveEnd(struct linenoiseState *l)
{
    if (l->pos != l->len) {
        l->pos = l->len;
        refreshLine(l);
    }
}

/* Substitute the currently edited line with the next or previous history
 * entry as specified by 'dir'. */
#define LINENOISE_HISTORY_NEXT 0
#define LINENOISE_HISTORY_PREV 1
void
linenoiseEditHistoryNext (struct linenoiseState *l, int dir)
{
    if (history_len > 1) {
        /* Update the current history entry before to
         * overwrite it with the next one. */
        free(history[history_len - 1 - l->history_index]);
        history[history_len - 1 - l->history_index] = strdup(l->buf);
        /* Show the new entry */
        l->history_index += (dir == LINENOISE_HISTORY_PREV) ? 1 : -1;
        if (l->history_index < 0) {
            l->history_index = 0;
            return;
        } else if (l->history_index >= history_len) {
            l->history_index = history_len-1;
            return;
        }
        strncpy(l->buf, history[history_len - 1 - l->history_index], l->buflen);
        l->buf[l->buflen-1] = '\0';
        l->len = l->pos = strlen(l->buf);
        refreshLine(l);
    }
}

/* Delete the character at the right of the cursor without altering the cursor
 * position. Basically this is what happens with the "Delete" keyboard key. */
void
linenoiseEditDelete (struct linenoiseState *l)
{
    if (l->len > 0 && l->pos < l->len) {
        memmove(l->buf+l->pos,l->buf+l->pos+1,l->len-l->pos-1);
        l->len--;
        l->buf[l->len] = '\0';
        refreshLine(l);
    }
}

/* Backspace implementation. */
void
linenoiseEditBackspace (struct linenoiseState *l)
{
    if (l->pos > 0 && l->len > 0) {
        memmove(l->buf+l->pos-1,l->buf+l->pos,l->len-l->pos);
        l->pos--;
        l->len--;
        l->buf[l->len] = '\0';
        refreshLine(l);
    }
}

/* Delete the previous word, maintaining the cursor at the start of the
 * current word. */
void
linenoiseEditDeletePrevWord (struct linenoiseState *l)
{
    size_t old_pos = l->pos;
    size_t diff;

    while (l->pos > 0 && l->buf[l->pos-1] == ' ')
        l->pos--;
    while (l->pos > 0 && l->buf[l->pos-1] != ' ')
        l->pos--;
    diff = old_pos - l->pos;
    memmove(l->buf+l->pos,l->buf+old_pos,l->len-old_pos+1);
    l->len -= diff;
    refreshLine(l);
}

/* Delete the next word, maintaining the cursor at the start of the
 * current word. */
void
linenoiseEditDeleteNextWord (struct linenoiseState *l)
{
    size_t old_pos = l->pos;
    size_t diff;

    while (l->pos != l->len && isWordSep(l->buf[l->pos]))
        l->pos++;

    while (l->pos != l->len && !isWordSep(l->buf[l->pos]))
	l->pos++;

    diff = l->pos - old_pos;
    memmove(l->buf+old_pos,l->buf+l->pos,l->len-l->pos+1);
    l->len -= diff;
    l->pos = old_pos;

    refreshLine(l);
}

/* This function is the core of the line editing capability of linenoise.
 * It expects 'fd' to be already in "raw mode" so that every key pressed
 * will be returned ASAP to read().
 *
 * The resulting string is put into 'buf' when the user type enter, or
 * when ctrl+d is typed.
 *
 * The function returns the length of the current buffer. */
static int
linenoiseEdit (int stdin_fd, int stdout_fd,
	       char *buf, size_t buflen, const char *prompt)
{
    struct linenoiseState l;

    /* Populate the linenoise state that we pass to functions implementing
     * specific editing functionalities. */
    l.ifd = stdin_fd;
    l.ofd = stdout_fd;
    l.buf = buf;
    l.buflen = buflen;
    l.prompt = prompt;
    l.plen = strlen(prompt);
    l.oldpos = l.pos = 0;
    l.len = 0;
    l.cols = getColumns(stdin_fd, stdout_fd);
    l.maxrows = 0;
    l.history_index = 0;

    /* Buffer starts empty. */
    l.buf[0] = '\0';
    l.buflen--; /* Make sure there is always space for the nulterm */

    /* The latest history entry is always our current buffer, that
     * initially is just an empty string. */
    linenoiseHistoryAdd("");
    
    if (write(l.ofd,prompt,l.plen) == -1) return -1;
    while(1) {
        char c;
        int nread;
        char seq[3];

        nread = read(l.ifd,&c,1);
        if (nread <= 0) return l.len;

        /* Only autocomplete when the callback is set. It returns < 0 when
         * there was an error reading from fd. Otherwise it will return the
         * character that should be handled next. */
	if (completionCallback) {
	    if (c == TAB) {
		c = completeLine(&l);
		/* Return on errors */
		if (c < 0) return l.len;
		/* Read next character when 0 */
		if (c == 0) continue;
	    } else if (c == '?') {
		c = helpLine(&l);
		continue;
	    }
	    
        }

        switch(c) {
        case ENTER:
            history_len--;
            free(history[history_len]);
            return (int)l.len;
        case CTRL_C:
            errno = EAGAIN;
            return -1;
        case BACKSPACE:
        case CTRL_H:
            linenoiseEditBackspace(&l);
            break;
        case CTRL_D:     /* ctrl-d, remove char at right of cursor, or of the
                       line is empty, act as end-of-file. */
            if (l.len > 0) {
                linenoiseEditDelete(&l);
            } else {
                history_len--;
                free(history[history_len]);
                return -1;
            }
            break;
        case CTRL_T:    /* ctrl-t, swaps current character with previous. */
            if (l.pos > 0 && l.pos < l.len) {
                int aux = buf[l.pos-1];
                buf[l.pos-1] = buf[l.pos];
                buf[l.pos] = aux;
                if (l.pos != l.len-1) l.pos++;
                refreshLine(&l);
            }
            break;
        case CTRL_B:     /* ctrl-b */
            linenoiseEditMoveLeft(&l);
            break;
        case CTRL_F:     /* ctrl-f */
            linenoiseEditMoveRight(&l);
            break;
        case CTRL_P:    /* ctrl-p */
            linenoiseEditHistoryNext(&l, LINENOISE_HISTORY_PREV);
            break;
        case CTRL_N:    /* ctrl-n */
            linenoiseEditHistoryNext(&l, LINENOISE_HISTORY_NEXT);
            break;
        case ESC:    /* escape sequence */
	    /* Use first byte following ESC to determine if additional
	     * reads are necessary */
            if (read(l.ifd,seq,1) == -1) break;

            /* ESC [ sequences. Additional 2 or 3 bytes */
            if (seq[0] == '[') {
		if (read(l.ifd,seq+1,1) == -1) break;
                if (seq[1] >= '0' && seq[1] <= '9') {
                    /* Extended escape, read additional byte. */
                    if (read(l.ifd,seq+2,1) == -1) break;
                    if (seq[2] == '~') {
                        switch(seq[1]) {
                        case '3': /* Delete key. */
                            linenoiseEditDelete(&l);
                            break;
                        }
                    }
                } else {
                    switch(seq[1]) {
                    case 'A': /* Up */
                        linenoiseEditHistoryNext(&l, LINENOISE_HISTORY_PREV);
                        break;
                    case 'B': /* Down */
                        linenoiseEditHistoryNext(&l, LINENOISE_HISTORY_NEXT);
                        break;
                    case 'C': /* Right */
                        linenoiseEditMoveRight(&l);
                        break;
                    case 'D': /* Left */
                        linenoiseEditMoveLeft(&l);
                        break;
                    case 'H': /* Home */
                        linenoiseEditMoveHome(&l);
                        break;
                    case 'F': /* End*/
                        linenoiseEditMoveEnd(&l);
                        break;
                    }
                }
            }

            /* ESC O sequences. 2 bytes */
            else if (seq[0] == 'O') {
		if (read(l.ifd,seq+1,1) == -1) break;
                switch(seq[1]) {
                case 'H': /* Home */
                    linenoiseEditMoveHome(&l);
                    break;
                case 'F': /* End*/
                    linenoiseEditMoveEnd(&l);
                    break;
                }
            }

	    /* ESC single byte */
	    else {
		switch(seq[0]) {
		case 'b':
		    linenoiseEditMoveLeftWord(&l);
		    break;
		case 'f':
		    linenoiseEditMoveRightWord(&l);
		    break;
		case BACKSPACE:
		case 'h':
		    linenoiseEditDeletePrevWord(&l);
		    break;
		case 'd':
		    linenoiseEditDeleteNextWord(&l);
		    break;

		default:
		    /* not handled */
		    break;
		}
	    }

            break;
        default:
            if (linenoiseEditInsert(&l,c)) return -1;
            break;
        case CTRL_U: /* Ctrl+u, delete the whole line. */
            buf[0] = '\0';
            l.pos = l.len = 0;
            refreshLine(&l);
            break;
        case CTRL_K: /* Ctrl+k, delete from current to end of line. */
            buf[l.pos] = '\0';
            l.len = l.pos;
            refreshLine(&l);
            break;
        case CTRL_A: /* Ctrl+a, go to the start of the line */
            linenoiseEditMoveHome(&l);
            break;
        case CTRL_E: /* ctrl+e, go to the end of the line */
            linenoiseEditMoveEnd(&l);
            break;
        case CTRL_L: /* ctrl+l, clear screen */
            linenoiseClearScreen();
            refreshLine(&l);
            break;
        case CTRL_W: /* ctrl+w, delete previous word */
            linenoiseEditDeletePrevWord(&l);
            break;
        }
    }
    return l.len;
}

/* This special mode is used by linenoise in order to print scan codes
 * on screen for debugging / development purposes. It is implemented
 * by the linenoise_example program using the --keycodes option. */
void
linenoisePrintKeyCodes (void)
{
    char quit[4];

    printf("Linenoise key codes debugging mode.\n"
            "Press keys to see scan codes. Type 'quit' at any time to exit.\n");
    if (enableRawMode(STDIN_FILENO) == -1) return;
    memset(quit,' ',4);
    while(1) {
        char c;
        int nread;

        nread = read(STDIN_FILENO,&c,1);
        if (nread <= 0) continue;
        memmove(quit, quit+1, sizeof(quit)-1); /* shift string to left. */
        quit[sizeof(quit)-1] = c; /* Insert current char on the right. */
        if (memcmp(quit,"quit",sizeof(quit)) == 0) break;

        printf("'%c' %02x (%d) (type quit to exit)\n",
            isprint(c) ? c : '?', (int)c, (int)c);
        printf("\x1b[0G"); /* Go left edge manually, we are in raw mode. */
        fflush(stdout);
    }
    disableRawMode(STDIN_FILENO);
}

/* This function calls the line editing function linenoiseEdit() using
 * the STDIN file descriptor set in raw mode. */
static int
linenoiseRaw (char *buf, size_t buflen, const char *prompt)
{
    int count;

    if (buflen == 0) {
        errno = EINVAL;
        return -1;
    }
    if (!isatty(STDIN_FILENO)) {
        /* Not a tty: read from file / pipe. */
        if (fgets(buf, buflen, stdin) == NULL) return -1;
        count = strlen(buf);
        if (count && buf[count-1] == '\n') {
            count--;
            buf[count] = '\0';
        }
    } else {
        /* Interactive editing. */
        if (enableRawMode(STDIN_FILENO) == -1) return -1;
        count = linenoiseEdit(STDIN_FILENO, STDOUT_FILENO, buf, buflen, prompt);
        disableRawMode(STDIN_FILENO);
        printf("\n");
    }
    return count;
}

/* The high level function that is the main API of the linenoise library.
 * This function checks if the terminal has basic capabilities, just checking
 * for a blacklist of stupid terminals, and later either calls the line
 * editing function or uses dummy fgets() so that you will be able to type
 * something even in the most desperate of the conditions. */
char *
linenoise (const char *prompt)
{
    char buf[LINENOISE_MAX_LINE];
    int count;

    if (isUnsupportedTerm()) {
        size_t len;

        printf("%s",prompt);
        fflush(stdout);
        if (fgets(buf,LINENOISE_MAX_LINE,stdin) == NULL) return NULL;
        len = strlen(buf);
        while(len && (buf[len-1] == '\n' || buf[len-1] == '\r')) {
            len--;
            buf[len] = '\0';
        }
        return strdup(buf);
    } else {
        count = linenoiseRaw(buf,LINENOISE_MAX_LINE,prompt);
        if (count == -1) return NULL;
        return strdup(buf);
    }
}

/* ================================ History ================================= */

/* Free the history, but does not reset it. Only used when we have to
 * exit() to avoid memory leaks are reported by valgrind & co. */
static void
freeHistory (void)
{
    if (history) {
        int j;

        for (j = 0; j < history_len; j++)
            free(history[j]);
        free(history);
    }
}

/* At exit we'll try to fix the terminal to the initial conditions. */
static void
linenoiseAtExit (void)
{
    disableRawMode(STDIN_FILENO);
    freeHistory();
}

/* This is the API call to add a new entry in the linenoise history.
 * It uses a fixed array of char pointers that are shifted (memmoved)
 * when the history max length is reached in order to remove the older
 * entry and make room for the new one, so it is not exactly suitable for huge
 * histories, but will work well for a few hundred of entries.
 *
 * Using a circular buffer is smarter, but a bit more complex to handle. */
int
linenoiseHistoryAdd (const char *line)
{
    char *linecopy;

    if (history_max_len == 0) return 0;

    /* Initialization on first call. */
    if (history == NULL) {
        history = (char **) malloc(sizeof(char*)*history_max_len);
        if (history == NULL) return 0;
        memset(history,0,(sizeof(char*)*history_max_len));
    }

    /* Don't add duplicated lines. */
    if (history_len && !strcmp(history[history_len-1], line)) return 0;

    /* Add an heap allocated copy of the line in the history.
     * If we reached the max length, remove the older line. */
    linecopy = strdup(line);
    if (!linecopy) return 0;
    if (history_len == history_max_len) {
        free(history[0]);
        memmove(history,history+1,sizeof(char*)*(history_max_len-1));
        history_len--;
    }
    history[history_len] = linecopy;
    history_len++;
    return 1;
}

/* Set the maximum length for the history. This function can be called even
 * if there is already some history, the function will make sure to retain
 * just the latest 'len' elements if the new history length value is smaller
 * than the amount of items already inside the history. */
int
linenoiseHistorySetMaxLen (int len)
{
    char **n;

    if (len < 1) return 0;
    if (history) {
        int tocopy = history_len;

        n = (char **) malloc(sizeof(char*)*len);
        if (n == NULL) return 0;

        /* If we can't copy everything, free the elements we'll not use. */
        if (len < tocopy) {
            int j;

            for (j = 0; j < tocopy-len; j++) free(history[j]);
            tocopy = len;
        }
        memset(n,0,sizeof(char*)*len);
        memcpy(n,history+(history_len-tocopy), sizeof(char*)*tocopy);
        free(history);
        history = n;
    }
    history_max_len = len;
    if (history_len > history_max_len)
        history_len = history_max_len;
    return 1;
}

/* Save the history in the specified file. On success 0 is returned
 * otherwise -1 is returned. */
int
linenoiseHistorySave (const char *filename)
{
    FILE *fp = fopen(filename,"w");
    int j;
    
    if (fp == NULL) return -1;
    for (j = 0; j < history_len; j++)
        fprintf(fp,"%s\n",history[j]);
    fclose(fp);
    return 0;
}

/* Load the history from the specified file. If the file does not exist
 * zero is returned and no operation is performed.
 *
 * If the file exists and the operation succeeded 0 is returned, otherwise
 * on error -1 is returned. */
int
linenoiseHistoryLoad (const char *filename)
{
    FILE *fp = fopen(filename,"r");
    char buf[LINENOISE_MAX_LINE];
    
    if (fp == NULL) return -1;

    while (fgets(buf,LINENOISE_MAX_LINE,fp) != NULL) {
        char *p;
        
        p = strchr(buf,'\r');
        if (!p) p = strchr(buf,'\n');
        if (p) *p = '\0';
        linenoiseHistoryAdd(buf);
    }
    fclose(fp);
    return 0;
}
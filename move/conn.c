#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <sys/time.h>
#include <sys/types.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>

#include "global.h"
#include "object.h"
#include "scanner.h"
#include "conn.h"

#if 0
#define DEBUG
#endif

PRIVATE OVECTOR mkfileconn(int fd, int blocking) {
  OVECTOR c = newovector(CO_MAXSLOTINDEX, T_CONNECTION);

  if (!blocking)
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);

  c->finalize = 1;
  ATPUT(c, CO_TYPE, MKNUM(CONN_FILE));
  ATPUT(c, CO_UNGETC, MKNUM(-1));
  ATPUT(c, CO_INFO, NULL);
  ATPUT(c, CO_HANDLE, MKNUM(fd));

  return c;
}

PUBLIC OVECTOR newfileconn(int fd) {
  return mkfileconn(fd, 0);
}

PUBLIC OVECTOR newfileconn_blocking(int fd) {
  return mkfileconn(fd, 1);
}

PUBLIC OVECTOR newstringconn(BVECTOR data) {
  OVECTOR c = newovector(CO_MAXSLOTINDEX, T_CONNECTION);

  c->finalize = 1;
  ATPUT(c, CO_TYPE, MKNUM(CONN_STRING));
  ATPUT(c, CO_UNGETC, MKNUM(-1));
  ATPUT(c, CO_INFO, MKNUM(0));
  ATPUT(c, CO_HANDLE, (OBJ) data);

  return c;
}

PRIVATE void conn_ungetc(OVECTOR conn, int ch) {
  ATPUT(conn, CO_UNGETC, MKNUM(ch));
}

PRIVATE int stringconn_getter(OVECTOR conn) {
  BVECTOR data = (BVECTOR) AT(conn, CO_HANDLE);
  int pos = NUM(AT(conn, CO_INFO));

  if (conn_closed(conn))
    return -1;

  if (NUM(AT(conn, CO_UNGETC)) != -1) {
    int u = NUM(AT(conn, CO_UNGETC));
    ATPUT(conn, CO_UNGETC, MKNUM(-1));
    return u;
  }

  if (pos >= data->_.length) {
    conn_close(conn);
    return -1;
  }

  ATPUT(conn, CO_INFO, MKNUM(pos+1));
  return AT(data, pos);
}

PRIVATE int fileconn_getter(OVECTOR conn) {
  char buf;

  if (conn_closed(conn))
    return -1;

  if (NUM(AT(conn, CO_UNGETC)) >= 0) {
    int u = NUM(AT(conn, CO_UNGETC));
    ATPUT(conn, CO_UNGETC, MKNUM(-1));
    return u;
  }

  while (1) {
    switch (read(NUM(AT(conn, CO_HANDLE)), &buf, 1)) {
      case -1:
	if (errno == EAGAIN) {
	  fd_set fds;
	  int the_fd = NUM(AT(conn, CO_HANDLE));

	  FD_ZERO(&fds);
	  FD_SET(the_fd, &fds);

	  if (select(NR_OPEN, &fds, NULL, NULL, NULL) >= 0 || errno == EINTR) {
	    if (FD_ISSET(the_fd, &fds))
	      continue;	/* try the read again. */
	  }

	  /* otherwise fall through */
	}

	/* FALL THROUGH */

      case 0:
	conn_close(conn);
	return -1;

      default:	/* Read was successful. */
	if (NUM(AT(conn, CO_UNGETC)) == -2 && buf == '\n') {	/* "eat next nl." */
	  ATPUT(conn, CO_UNGETC, MKNUM(-1));
	  continue;	/* read another char. */
	}

	if (buf == '\r') {
	  buf = '\n';
	  ATPUT(conn, CO_UNGETC, MKNUM(-2));	/* "eat next nl." */
	}

	break;
    }
    break;
  }

  return buf;
}

PRIVATE int nullconn_getter(void *arg) {
  return -1;
}

#define CONN_GETS_GETC(conn) ((NUM(AT(conn, CO_TYPE)) == CONN_FILE) ? fileconn_getter(conn) : -1)

PUBLIC char *conn_gets(char *s, int size, OVECTOR conn) {
  char *org = s;
  int c;

  if (conn_closed(conn))
    return NULL;

  if (NUM(AT(conn, CO_TYPE)) != CONN_FILE)
    return NULL;

  while (size > 1) {
    c = CONN_GETS_GETC(conn);

    if (c == -1)
      return NULL;

    if (c == '\r') {
      int c2 = CONN_GETS_GETC(conn);
      if (c2 != '\n')
	conn_ungetc(conn, c2);
      break;
    }

    if (c == '\n')
      break;

    *s = c;
    s++;
    size--;
  }

  *s = '\0';
  return org;
}

PUBLIC int conn_write(const char *buf, int size, OVECTOR conn) {
  if (NUM(AT(conn, CO_TYPE)) != CONN_FILE)
    return -1;

  return write(NUM(AT(conn, CO_HANDLE)), buf, size);
}

PUBLIC int conn_puts(const char *s, OVECTOR conn) {
  if (NUM(AT(conn, CO_TYPE)) != CONN_FILE)
    return -1;

  return write(NUM(AT(conn, CO_HANDLE)), s, strlen(s));
}

PUBLIC void conn_close(OVECTOR conn) {
#ifdef DEBUG
  printf("(%d call close %p)\n", pthread_self(), conn);
#endif
  switch (NUM(AT(conn, CO_TYPE))) {
    case CONN_FILE: {
      int fd = NUM(AT(conn, CO_HANDLE));
#ifdef DEBUG
      printf(" closing %d ", fd); fflush(stdout);
#endif
      if (fd > 2)	/* don't close stdin, stdout, or stderr */
	close(fd);
      break;
    }

    default:
      break;
  }

  ATPUT(conn, CO_TYPE, MKNUM(CONN_NONE));
  ATPUT(conn, CO_HANDLE, NULL);
  ATPUT(conn, CO_INFO, NULL);
}

PUBLIC void fill_scaninst(SCANINST si, OVECTOR conn) {
  si->cache = -1;
  si->linenum = 0;
  si->yylval = NULL;

  switch (NUM(AT(conn, CO_TYPE))) {
    case CONN_NONE:
      si->getter = (stream_fn) nullconn_getter;
      si->arg = NULL;
      break;

    case CONN_FILE:
      si->getter = (stream_fn) fileconn_getter;
      si->arg = conn;
      break;

    case CONN_STRING:
      si->getter = (stream_fn) stringconn_getter;
      si->arg = conn;
      break;

    default:
      fprintf(stderr, "connection %p had funny type %d\n", conn, NUM(AT(conn, CO_TYPE)));
      exit(3);
  }
}

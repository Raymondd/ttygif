/*
 * Copyright (c) 2000 Satoru Takabayashi <satoru@namazu.org>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *  This product includes software developed by the University of
 *  California, Berkeley and its contributors.
 * 4. Neither the name of the University nor the names of its contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE REGENTS AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 */

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <unistd.h>
#include <termios.h>
#include <sys/time.h>
#include <string.h>

#include "ttyrec.h"
#include "io.h"
#include "string_builder.h"

typedef int    (*ReadFunc)    (FILE *fp, Header *h, char **buf);
typedef void   (*WriteFunc)   (char *buf, int len);
typedef void   (*ProcessFunc) (FILE *fp, ReadFunc read_func);

struct timeval
timeval_diff (struct timeval tv1, struct timeval tv2)
{
    struct timeval diff;

    diff.tv_sec = tv2.tv_sec - tv1.tv_sec;
    diff.tv_usec = tv2.tv_usec - tv1.tv_usec;

    if (diff.tv_usec < 0) {
        diff.tv_sec--;
        diff.tv_usec += 1000000;
    }

    return diff;
}

int
ttydelay (struct timeval prev, struct timeval cur)
{
    struct timeval diff = timeval_diff(prev, cur);
    if (diff.tv_sec < 0) {
      diff.tv_sec = diff.tv_usec = 0;
    }
    return (diff.tv_sec * 1000) + (diff.tv_usec / 1000);
}

int
ttyread (FILE *fp, Header *h, char **buf)
{
    if (read_header(fp, h) == 0) {
        return 0;
    }

    *buf = malloc(h->len);
    if (*buf == NULL) {
        perror("malloc");
    }
  
    if (fread(*buf, 1, h->len, fp) == 0) {
        perror("fread");
    }
    return 1;
}

void
ttywrite (char *buf, int len)
{
    fwrite(buf, 1, len, stdout);
    fflush(stdout);
}

void
clear_screen (void) {
    printf("\e[1;1H\e[2J");
}

int
take_snapshot(const char *fname, char* window_id, StringBuilder *command)
{
  static char cmd [256];

  // ensure text has been written before taking screenshot
  usleep(50000);
  if (sprintf(cmd, "xwd -id %s -out %s", window_id, fname) < 0) {
      return -1;
  }

  if (system(cmd) != 0) {
      return -1;
  }

  return 0;
}

void
ttyplay (FILE *fp, ReadFunc read_func, WriteFunc write_func)
{
    int index = 0;
    int delay = 0;
    struct timeval prev;

    clear_screen();

    setbuf(stdout, NULL);
    setbuf(fp, NULL);

    char* wid = getenv("WINDOWID");

    if (wid == NULL || !strlen(wid)) {
        printf("Error: WINDOWID environment variable was empty.");
        exit(EXIT_FAILURE);
    }

    StringBuilder *sb = StringBuilder_new();
    StringBuilder_write(sb, "convert -loop 0 ");

    int nskipped = 0;
    int skip = 0;

    char dir_template[] = "/tmp/tmpgif.XXXXXX";
    char *dir_name = mkdtemp(dir_template);
    if (dir_name == NULL) {
      perror("Failed to create tmp directory");
      exit(EXIT_FAILURE);
    }

    while (1) {

        char *buf;
        Header h;
        static char fname[256];
        static char arg_buffer[256];

        if (read_func(fp, &h, &buf) == 0) {
            break;
        }

        write_func(buf, h.len);

        if (index != 0) {
            delay = ttydelay(prev, h.tv);
        }

        if (delay == 0) {
          skip = 1;
          nskipped++;
        } else {
          skip = 0;
        }

        if (skip && nskipped > 5) {
          nskipped = 0;
          skip = 0;
        }

        if (!skip && index != 0) {
          if (sprintf(arg_buffer, " -delay %f %s", delay * 0.1, fname) < 0) {
              perror("command error");
              break;
          }
          StringBuilder_write(sb, arg_buffer);
        }
        if (sprintf(fname, "%s/%d.xwd", dir_name, index) < 0) {
            perror("filename error");
            break;
        }
        if (take_snapshot(fname, wid, sb) != 0) {
            perror("snapshot");
            break;
        }

        index++;
        prev = h.tv;
        free(buf);
    }

    StringBuilder_write(sb, " -layers Optimize tty.gif");

    printf("Creating Animated GIF\n");
    if (system(sb->s) != 0) {
      perror("convert failed");
    }

    StringBuilder_free(sb);
}

void ttyplayback (FILE *fp, ReadFunc read_func)
{
    ttyplay(fp, ttyread, ttywrite);
}

void
usage (void)
{
    printf("Usage: ttygif [FILE]\n");
    exit(EXIT_FAILURE);
}

int 
main (int argc, char **argv)
{
    ReadFunc read_func  = ttyread;
    ProcessFunc process = ttyplayback;
    FILE *input = NULL;
    struct termios old, new;

    if (argc < 2) {
      usage();
    }

    set_progname(argv[0]);
    input = efopen(argv[1], "r");
    
    assert(input != NULL);

    tcgetattr(0, &old); /* Get current terminal state */
    new = old;          /* Make a copy */
    new.c_lflag &= ~(ICANON | ECHO | ECHONL); /* unbuffered, no echo */
    tcsetattr(0, TCSANOW, &new); /* Make it current */

    process(input, read_func);
    tcsetattr(0, TCSANOW, &old);  /* Return terminal state */

    return 0;
}

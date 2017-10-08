/* `time' utility to display resource usage of processes, main source file.
   Copyright (C) 1990, 91, 92, 93, 96 Free Software Foundation, Inc.

   Originally written by David Keppel <pardo@cs.washington.edu>.
   Heavily modified by David MacKenzie <djm@gnu.ai.mit.edu>.
   Heavily modified (again) by Assaf Gordon <assafgordon@gmail.com>.

   This file is part of GNU Time.

   GNU Time is free software: you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation, either version 3 of the License, or
   (at your option) any later version.

   GNU Time is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with GNU Time.  If not, see <http://www.gnu.org/licenses/>.
*/ 


#include "config.h"

#include <sys/wait.h>
#include <sys/resource.h>
#include <sys/capability.h>
#include <sys/types.h>
#include <sys/prctl.h>
#include <stdio.h>
#include <signal.h>
#include <getopt.h>
#include <inttypes.h>
#include <errno.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <limits.h>
#include <unistd.h>
#include <grp.h> 
#include <pwd.h>
#include <sched.h>
#include <sys/mount.h>

#include "progname.h"
#include "error.h"
#define Version VERSION
#include "version-etc.h"

#include "resuse.h"
#include "rusage-kb.h"



/* For now, no gettext support */
#define _(x) (x)


/* Exit statuses for programs like 'env' that exec other programs.
   Copied from coreutils' system.h */
enum
{
  EXIT_CANCELED = 125, /* Internal error prior to exec attempt.  */
  EXIT_CANNOT_INVOKE = 126, /* Program located, but not usable.  */
  EXIT_ENOENT = 127 /* Could not find program to exec.  */
};

/* If the inferior program exited abnormally (e.g. signalled)
   add this offset to time's exit code: this is what sh
   returns for signaled processes. */
#define SIGNALLED_OFFSET 128

#define AUTHORS \
    "David Keppel",                 \
    "David MacKenzie",              \
    "Assaf Gordon"


/* A Pointer to a signal handler.  */
typedef RETSIGTYPE (*sighandler) ();

/* msec = milliseconds = 1/1,000 (1*10e-3) second.
   usec = microseconds = 1/1,000,000 (1*10e-6) second.  */

/* Systems known to fill in the average resident set size fields:
   SunOS 4.1.3 (m68k and sparc)
   Mt. Xinu 4.3BSD on HP9000/300 (m68k)
   Ultrix 4.4 (mips)
   IBM ACIS 4.3BSD (rt)
   Sony NEWS-OS 4.1C (m68k)

   Systems known to not fill them in:
   OSF/1 1.3 (alpha)
   BSD/386 1.1 (anything derived from NET-2)
   NetBSD 1.0 (4.4BSD-derived)
   Irix 5.2 (R4000)
   Solaris 2.3
   Linux 1.0

   It doesn't matter how many clock ticks/second there are on
   systems that don't fill in those fields.

   If the avgresident (%t) we print is less than a power of 2 away from
   the maxresident (%M), then we likely are using the right number.
   Another good check is comparing the average text size with the
   output of `size' on the executable.

   According to the SunOS manual, there are 50 ticks/sec on the sun3
   and 100 on the sun4.

   Some manuals have an apparent error, claiming that units for average
   sizes are kb*sec.  Judging by the contents of `struct rusage', it
   looks like it should be kb*ticks, like on SunOS.  Ticks/sec seems
   to be (empirically):
   50 Mt. Xinu
   250 Ultrix (mips)
   50 ACIS
   100 NEWS-OS

   sysconf(_SC_CLK_TCK) is *unrelated*.  */

#if defined(sun3) || defined(hp300) || defined(ibm032)
#define TICKS_PER_SEC 50
#endif
#if defined(mips)
#define TICKS_PER_SEC 250
#endif
#ifndef TICKS_PER_SEC
#define TICKS_PER_SEC 100
#endif

/* The number of milliseconds in one `tick' used by the `rusage' structure.  */
#define MSEC_PER_TICK (1000 / TICKS_PER_SEC)

/* Return the number of clock ticks that occur in M milliseconds.  */
#define MSEC_TO_TICKS(m) ((m) / MSEC_PER_TICK)


/* The default output format.  */
static const char *const default_format =
"%Uuser %Ssystem %Eelapsed %PCPU (%Xavgtext+%Davgdata %Mmaxresident)k\n\
%Iinputs+%Ooutputs (%Fmajor+%Rminor)pagefaults %Wswaps";

/* The output format for the -p option .*/
static const char *const posix_format = "real %e\nuser %U\nsys %S";

/* Format string for printing all statistics verbosely.
   Keep this output to 24 lines so users on terminals can see it all.

   The format string is used two ways: as a format string, and in
   verbose mode, to document all the possible formatting possiblities.
   When `longstats' is used as a format string, it has to be put into
   one contiguous string (e.g., into a `char[]').  We could alternatively
   store it as a `char *' and convert it into a `*char[]' when we need
   it as documentation, but most compilers choke on very long strings.  */

static const char *const longstats[] =
{
  "\tCommand being timed: \"%C\"\n",
  "\tUser time (seconds): %U\n",
  "\tSystem time (seconds): %S\n",
  "\tPercent of CPU this job got: %P\n",
  "\tElapsed (wall clock) time (h:mm:ss or m:ss): %E\n",
  "\tAverage shared text size (kbytes): %X\n",
  "\tAverage unshared data size (kbytes): %D\n",
  "\tAverage stack size (kbytes): %p\n",
  "\tAverage total size (kbytes): %K\n",
  "\tMaximum resident set size (kbytes): %M\n",
  "\tAverage resident set size (kbytes): %t\n",
  "\tMajor (requiring I/O) page faults: %F\n",
  "\tMinor (reclaiming a frame) page faults: %R\n",
  "\tVoluntary context switches: %w\n",
  "\tInvoluntary context switches: %c\n",
  "\tSwaps: %W\n",
  "\tFile system inputs: %I\n",
  "\tFile system outputs: %O\n",
  "\tSocket messages sent: %s\n",
  "\tSocket messages received: %r\n",
  "\tSignals delivered: %k\n",
  "\tPage size (bytes): %Z\n",
  "\tExit status: %x",
  NULL
};

/* If true, show an English description next to each statistic.  */
static bool verbose;

/* Name of output file.  Only used if -o option is given.  */
static const char *outfile;

/* Output stream, stderr by default.  */
static FILE *outfp;

/* If true, append to `outfile' rather than truncating it.  */
static bool append;

/* The output format string.  */
static const char *output_format;

/* Quiet mode: do not print info about abnormal terminations */
static bool quiet;

/* Cap drops: select caps to drop from spawned process */
static int *cap_drops = NULL;
static int cap_drops_length = 0;
static bool cap_drop_setpcap = false;
static bool cap_drop_setuid = false;
static bool cap_drop_setgid = false;

/* set user */
static bool set_user = false;
static uid_t set_uid;
static gid_t set_gid;
static gid_t *set_groups;
static int set_groups_length = 0;

/* pid namespace */
static bool pid_namesapce = false;

/* pid file */
static FILE *pidfp = NULL;

static struct option longopts[] =
{
  {"append", no_argument, NULL, 'a'},
  {"format", required_argument, NULL, 'f'},
  {"help", no_argument, NULL, 'h'},
  {"output-file", required_argument, NULL, 'o'},
  {"cap-drop", required_argument, NULL, 'c'},
  {"user", required_argument, NULL, 'u'},
  {"pid-namespace", no_argument, NULL, 'n'},
  {"pid-file", required_argument, NULL, 'i'},
  {"portability", no_argument, NULL, 'p'},
  {"quiet", no_argument, NULL, 'q'},
  {"verbose", no_argument, NULL, 'v'},
  {"version", no_argument, NULL, 'V'},
  {NULL, no_argument, NULL, 0}
};

# define PROGRAM_NAME "time"


static void
usage (int status)
{
  if (status != EXIT_SUCCESS)
    {
      fprintf (stderr, _("Try '%s --help' for more information.\n"),
               program_name);
      exit (status);
    }

  /* If normal exit status, print usage info on stdout */

  printf (_("\
Usage: %s [OPTIONS] COMMAND [ARG]...\n\
"),program_name);
  fputs (_("\
Run COMMAND, then print system resource usage.\n\
\n\
"), stdout);

  /*
    printf ("\
Usage: %s [-apvV] [-f format] [-o file] [--append] [--verbose]\n\
       [--portability] [--format=format] [--output=file] [--version]\n\
       [--help] command [arg...]\n",
          program_name);
  */

  fputs (_("\
  -a, --append              with -o FILE, append instead of overwriting\n\
"), stdout);
  fputs (_("\
  -f, --format=FORMAT       use the specified FORMAT instead of the default\n\
"), stdout);
  fputs (_("\
  -o, --output=FILE         write to FILE instead of STDOUT\n"), stdout);
  fputs (_("\
  -c, --cap-drop=cap1[,cap2...]    drop these capabilities\n"), stdout);
  fputs (_("\
  -n, --pid-namespace       enable pid namesapce\n"), stdout);
  fputs (_("\
  -u, --user=USER_NAME      execute with specific user's uid and pid\n"), stdout);
  fputs (_("\
  -p, --portability         print POSIX standard 1003.2 conformant string:\n\
                              real %%e\n\
                              user %%U\n\
                              sys %%S\n\
"), stdout);
  fputs (_("\
  -q, --quiet               do not print information about abnormal program\n\
                            termination (non-zero exit codes or signals)\n\
"), stdout);
  fputs (_("\
  -v, --verbose             print all resource usage information instead of\n\
                            the default format\n\
"), stdout);


  fputs (_("\
  -h,  --help               display this help and exit\n"), stdout);
  fputs (_("\
  -V,  --version            output version information and exit\n"), stdout);

  /* Commonly used variables */
  fputs (_("\nCommonly usaged format sequences for -f/--format:\n"), stdout);
  fputs (_("(see documentation for full list)\n"), stdout);

  fputs (_("  %%   a literal '%'\n"), stdout);
  fputs (_("  %C   command line and arguments\n"), stdout);
  fputs (_("  %c   involuntary context switches\n"), stdout);
  fputs (_("  %E   elapsed real time (wall clock) in [hour:]min:sec\n"), stdout);
  fputs (_("  %e   elapsed real time (wall clock) in seconds\n"), stdout);
  fputs (_("  %F   major page faults\n"), stdout);
  fputs (_("  %M   maximum resident set size in KB\n"), stdout);
  fputs (_("  %P   percent of CPU this job got\n"), stdout);
  fputs (_("  %R   minor page faults\n"), stdout);
  fputs (_("  %S   system (kernel) time in seconds\n"), stdout);
  fputs (_("  %U   user time in seconds\n"), stdout);
  fputs (_("  %w   voluntary context switches\n"), stdout);
  fputs (_("  %x   exit status of command\n"), stdout);

  /* Default output format */
  fputs (_("\nDefault output format:\n"), stdout);
  fputs (default_format, stdout);
  fputc ('\n',stdout);

  /* Warning about shell's built-in 'time', copied from
     coreutils' */
  printf ("\n\
NOTE: your shell may have its own version of %s, which usually supersedes\n\
the version described here.  Please refer to your shell's documentation\n\
for details about the options it supports.\n",
          PROGRAM_NAME);

  /* General help information, copied from coreutils' emit_ancillary_info */
  printf (_("\n%s website: <%s>\n"), PACKAGE_NAME, PACKAGE_URL);
  printf (_("Full documentation at: <%smanual>\n"), PACKAGE_URL);
  printf (_("E-mail bug reports to: %s\n"), PACKAGE_BUGREPORT);

  exit (EXIT_SUCCESS);
}


/* Print ARGV to FP, with each entry in ARGV separated by FILLER.  */

static void
fprintargv (fp, argv, filler)
     FILE *fp;
     const char *const *argv;
     const char *filler;
{
  const char *const *av;

  av = argv;
  fputs (*av, fp);
  while (*++av)
    {
      fputs (filler, fp);
      fputs (*av, fp);
    }
  if (ferror (fp))
    error (1, errno, "write error");
}

/* Return a null-terminated string containing the concatenation,
   in order, of all of the elements of ARGV.
   The '\0' at the end of each ARGV-element is not copied.
   Example:	char *argv[] = {"12", "ab", ".,"};
 		linear_argv(argv) == "12ab.,"
   Print a message and return NULL if memory allocation failed.  */

static char *
linear_argv (argv)
     const char *const *argv;
{
  const char *const *s;		/* Each string in ARGV.  */
  char *new;			/* Allocated space.  */
  char *dp;			/* Copy in to destination.  */
  const char *sp;		/* Copy from source.  */
  int size;

  /* Find length of ARGV and allocate.  */
  size = 1;
  for (s = argv; *s; ++s)
    size += strlen (*s);
  new = (char *) malloc (size);
  if (new == NULL)
    {
      fprintf (stderr, "%s: virtual memory exhausted\n", program_name);
      return NULL;
    }

  /* Copy each string in ARGV to the new string.  At the end of
     each string copy, back up `dp' so that on the next string,
     the `\0' will be overwritten.  */
  for (s = argv, sp = *s, dp = new; *s; ++s)
    {
      sp = *s;
      while ((*dp++ = *sp++) != '\0')
	/* Do nothing.  */ ;
      --dp;
    }

  return new;
}



/* summarize: Report on the system use of a command.

   Copy the FMT argument to FP except that `%' sequences
   have special meaning, and `\n' and `\t' are translated into
   newline and tab, respectively, and `\\' is translated into `\'.

   The character following a `%' can be:
   (* means the tcsh time builtin also recognizes it)
   % == a literal `%'
   C == command name and arguments
*  D == average unshared data size in K (ru_idrss+ru_isrss)
*  E == elapsed real (wall clock) time in [hour:]min:sec
*  F == major page faults (required physical I/O) (ru_majflt)
*  I == file system inputs (ru_inblock)
*  K == average total mem usage (ru_idrss+ru_isrss+ru_ixrss)
*  M == maximum resident set size in K (ru_maxrss)
*  O == file system outputs (ru_oublock)
*  P == percent of CPU this job got (total cpu time / elapsed time)
*  R == minor page faults (reclaims; no physical I/O involved) (ru_minflt)
*  S == system (kernel) time (seconds) (ru_stime)
*  U == user time (seconds) (ru_utime)
*  W == times swapped out (ru_nswap)
*  X == average amount of shared text in K (ru_ixrss)
   Z == page size
*  c == involuntary context switches (ru_nivcsw)
   e == elapsed real time in seconds
*  k == signals delivered (ru_nsignals)
   p == average unshared stack size in K (ru_isrss)
*  r == socket messages received (ru_msgrcv)
*  s == socket messages sent (ru_msgsnd)
   t == average resident set size in K (ru_idrss)
*  w == voluntary context switches (ru_nvcsw)
   x == exit status of command

   Various memory usages are found by converting from page-seconds
   to kbytes by multiplying by the page size, dividing by 1024,
   and dividing by elapsed real time.

   FP is the stream to print to.
   FMT is the format string, interpreted as described above.
   COMMAND is the command and args that are being summarized.
   RESP is resource information on the command.  */

static void
summarize (fp, fmt, command, resp)
     FILE *fp;
     const char *fmt;
     const char **command;
     RESUSE *resp;
{
  unsigned long r;		/* Elapsed real milliseconds.  */
  unsigned long v;		/* Elapsed virtual (CPU) milliseconds.  */

  if (!quiet)
    {
      if (WIFSTOPPED (resp->waitstatus))
        fprintf (fp, "Command stopped by signal %d\n",
                 WSTOPSIG (resp->waitstatus));
      else if (WIFSIGNALED (resp->waitstatus))
        fprintf (fp, "Command terminated by signal %d\n",
                 WTERMSIG (resp->waitstatus));
      else if (WIFEXITED (resp->waitstatus) && WEXITSTATUS (resp->waitstatus))
        fprintf (fp, "Command exited with non-zero status %d\n",
                 WEXITSTATUS (resp->waitstatus));
    }

  /* Convert all times to milliseconds.  Occasionally, one of these values
     comes out as zero.  Dividing by zero causes problems, so we first
     check the time value.  If it is zero, then we take `evasive action'
     instead of calculating a value.  */

  r = resp->elapsed.tv_sec * 1000 + resp->elapsed.tv_usec / 1000;

  v = resp->ru.ru_utime.tv_sec * 1000 + resp->ru.ru_utime.TV_MSEC +
    resp->ru.ru_stime.tv_sec * 1000 + resp->ru.ru_stime.TV_MSEC;

  while (*fmt)
    {
      switch (*fmt)
	{
	case '%':
	  switch (*++fmt)
	    {
	    case '%':		/* Literal '%'.  */
	      putc ('%', fp);
	      break;
	    case 'C':		/* The command that got timed.  */
	      fprintargv (fp, command, " ");
	      break;
        case 'D': /* Average unshared data size.  */
          fprintf (fp, "%" PRIuMAX,
                   MSEC_TO_TICKS (v) == 0 ? 0 :
                   get_rusage_idrss_kb (&resp->ru) / MSEC_TO_TICKS (v) +
                   get_rusage_isrss_kb (&resp->ru) / MSEC_TO_TICKS (v));
          break;
	    case 'E':		/* Elapsed real (wall clock) time.  */
	      if (resp->elapsed.tv_sec >= 3600)	/* One hour -> h:m:s.  */
		fprintf (fp, "%ld:%02ld:%02ld",
			 (long int)(resp->elapsed.tv_sec / 3600),
			 (long int)((resp->elapsed.tv_sec % 3600) / 60),
			 (long int)(resp->elapsed.tv_sec % 60));
	      else
		fprintf (fp, "%ld:%02ld.%02ld",	/* -> m:s.  */
			 (long int)(resp->elapsed.tv_sec / 60),
			 (long int)(resp->elapsed.tv_sec % 60),
			 (long int)(resp->elapsed.tv_usec / 10000));
	      break;
	    case 'F':		/* Major page faults.  */
	      fprintf (fp, "%ld", resp->ru.ru_majflt);
	      break;
	    case 'I':		/* Inputs.  */
	      fprintf (fp, "%ld", resp->ru.ru_inblock);
	      break;

        case 'K': /* Average mem usage == data+stack+text.  */
          fprintf (fp, "%lu",
                   MSEC_TO_TICKS (v) == 0 ? 0 :
		   (long unsigned int)
                   (get_rusage_idrss_kb (&resp->ru) / MSEC_TO_TICKS (v) +
                    get_rusage_isrss_kb (&resp->ru) / MSEC_TO_TICKS (v) +
                    get_rusage_ixrss_kb (&resp->ru) / MSEC_TO_TICKS (v)));
          break;
        case 'M': /* Maximum resident set size.  */
          fprintf (fp, "%" PRIuMAX, get_rusage_maxrss_kb (&resp->ru));
          break;

	    case 'O':		/* Outputs.  */
	      fprintf (fp, "%ld", resp->ru.ru_oublock);
	      break;
	    case 'P':		/* Percent of CPU this job got.  */
	      /* % cpu is (total cpu time)/(elapsed time).  */
	      if (r > 0)
		fprintf (fp, "%lu%%", (v * 100 / r));
	      else
		fprintf (fp, "?%%");
	      break;
	    case 'R':		/* Minor page faults (reclaims).  */
	      fprintf (fp, "%ld", resp->ru.ru_minflt);
	      break;
	    case 'S':		/* System time.  */
	      fprintf (fp, "%ld.%02ld",
		       (long int)resp->ru.ru_stime.tv_sec,
		       (long int)(resp->ru.ru_stime.TV_MSEC / 10));
	      break;
	    case 'U':		/* User time.  */
	      fprintf (fp, "%ld.%02ld",
		       (long int)(resp->ru.ru_utime.tv_sec),
		       (long int)(resp->ru.ru_utime.TV_MSEC / 10));
	      break;
	    case 'W':		/* Times swapped out.  */
	      fprintf (fp, "%ld", resp->ru.ru_nswap);
	      break;

        case 'X': /* Average shared text size.  */
          fprintf (fp, "%" PRIuMAX,
                   MSEC_TO_TICKS (v) == 0 ? 0 :
                   get_rusage_ixrss_kb (&resp->ru) / MSEC_TO_TICKS (v));
          break;

	    case 'Z':		/* Page size.  */
	      fprintf (fp, "%d", getpagesize ());
	      break;
	    case 'c':		/* Involuntary context switches.  */
	      fprintf (fp, "%ld", resp->ru.ru_nivcsw);
	      break;
	    case 'e':		/* Elapsed real time in seconds.  */
	      fprintf (fp, "%ld.%02ld",
		       (long int)resp->elapsed.tv_sec,
		       (long int)(resp->elapsed.tv_usec / 10000));
	      break;
	    case 'k':		/* Signals delivered.  */
	      fprintf (fp, "%ld", resp->ru.ru_nsignals);
	      break;

        case 'p': /* Average stack segment.  */
          fprintf (fp, "%"PRIuMAX,
                   MSEC_TO_TICKS (v) == 0 ? 0 :
                   get_rusage_isrss_kb (&resp->ru) / MSEC_TO_TICKS (v));
          break;

	    case 'r':		/* Incoming socket messages received.  */
	      fprintf (fp, "%ld", resp->ru.ru_msgrcv);
	      break;
	    case 's':		/* Outgoing socket messages sent.  */
	      fprintf (fp, "%ld", resp->ru.ru_msgsnd);
	      break;

        case 't': /* Average resident set size.  */
          fprintf (fp, "%" PRIuMAX,
                   MSEC_TO_TICKS (v) == 0 ? 0 :
                   get_rusage_idrss_kb (&resp->ru) / MSEC_TO_TICKS (v));
          break;

	    case 'w':		/* Voluntary context switches.  */
	      fprintf (fp, "%ld", resp->ru.ru_nvcsw);
	      break;
	    case 'x':		/* Exit status.  */
	      fprintf (fp, "%d", WEXITSTATUS (resp->waitstatus));
	      break;
	    case '\0':
	      putc ('?', fp);
	      return;
	    default:
	      putc ('?', fp);
	      putc (*fmt, fp);
	    }
	  ++fmt;
	  break;

	case '\\':		/* Format escape.  */
	  switch (*++fmt)
	    {
	    case 't':
	      putc ('\t', fp);
	      break;
	    case 'n':
	      putc ('\n', fp);
	      break;
	    case '\\':
	      putc ('\\', fp);
	      break;
	    default:
	      putc ('?', fp);
	      putc ('\\', fp);
	      putc (*fmt, fp);
	    }
	  ++fmt;
	  break;

	default:
	  putc (*fmt++, fp);
	}

      if (ferror (fp))
	error (1, errno, "write error");
    }
  putc ('\n', fp);

  if (ferror (fp))
    error (1, errno, "write error");
}

/* Initialize the options and parse the command line arguments.
   Also note the position in ARGV where the command to time starts.

   By default, output is to stderr.

   ARGV is the array of command line arguments.
   ARGC is the number of command line arguments.

   Return the command line to run and gather statistics on.  */

static const char **
getargs (argc, argv)
     int argc;
     char **argv;
{
  int optc;
  char *format;			/* Format found in environment.  */

  /* Initialize the option flags.  */
  verbose = false;
  outfile = NULL;
  outfp = stderr;
  append = false;
  output_format = default_format;
  /* for parse caps */
  char *newstr;
  const char * const delim = ",";
  char *sepstr;
  char *substr;
  int *new_cap_drops;
  cap_value_t cap;
  /* for user option */
  struct passwd *pwd;
  
  /* Set the format string from the environment.  Do this before checking
     the args so that we won't clobber a user-specified format.  */
  format = getenv ("TIME");
  if (format)
    output_format = format;

  while ((optc = getopt_long (argc, argv, "+af:o:pqvc:u:i:nV", longopts, (int *) 0))
	 != EOF)
    {
      switch (optc)
	{
	case 'a':
	  append = true;
	  break;
	case 'f':
	  output_format = optarg;
	  break;
	case 'h':
	  usage (EXIT_SUCCESS);
	case 'o':
	  outfile = optarg;
	  break;
	case 'p':
	  output_format = posix_format;
	  break;
    case 'q':
      quiet = true;
      break;
	case 'v':
	  verbose = true;
	  break;
	case 'c':
	  newstr = strdup(optarg);
	  sepstr = newstr;
	  
    while (true) {
        substr = strsep(&sepstr, delim);
        if (!substr) {
          break;
        }
        
        if (cap_from_name(substr, &cap)) {
          fprintf(stderr, "unknown capability %s \n", substr);
          exit(EXIT_CANCELED);
        }
        
        if (cap == CAP_SETPCAP) {
          cap_drop_setpcap = true;
          continue;
        }
        
        if (cap == CAP_SETUID) {
          cap_drop_setuid = true;
          continue;
        }
        
        if (cap == CAP_SETGID) {
          cap_drop_setgid = true;
          continue;
        }
        
        new_cap_drops = malloc((cap_drops_length + 1) * sizeof *cap_drops);
        if (cap_drops_length != 0) {
          memcpy(new_cap_drops, cap_drops, cap_drops_length * sizeof *cap_drops);
        }
        free(cap_drops);
        cap_drops = new_cap_drops;
        cap_drops_length++;
        
	      cap_drops[cap_drops_length - 1] = cap;
	      
        // printf("#%d sub string: %s (@%p), cap id: %u\n", count++, substr, substr, cap);
    }
    
    // for (int i = 0; i < cap_drops_length; i++) {
    //   printf("%d, ", cap_drops[i]);
    // }
    // printf("\n");
    
	  free(newstr);
    break;
  case 'u':
    set_user = true;
    pwd = getpwnam(optarg);
    if (pwd == NULL) {
      fprintf(stderr, "unknown user %s \n", optarg);
      exit(EXIT_CANCELED);
    }
    set_uid = pwd->pw_uid;
    set_gid = pwd->pw_gid;
    
    set_groups = malloc(0);
    getgrouplist(optarg, set_gid, set_groups, &set_groups_length);
    free(set_groups);
    set_groups = malloc(set_groups_length * sizeof *set_groups);
    
    if(getgrouplist(optarg, set_gid, set_groups, &set_groups_length) == -1) {
      fprintf(stderr, "fail to get supplement groups\n");
      exit(EXIT_CANCELED);
    }
    break;
  case 'n':
    pid_namesapce = true;
    break;
  case 'i':
    pidfp = fopen (optarg, "w");
    if (pidfp == NULL) {
      error (EXIT_CANCELED, errno, "fail to create pid file: %s", outfile);
    }
    break;
	case 'V':
      version_etc (stdout, PROGRAM_NAME, PACKAGE_NAME, Version, AUTHORS,
                   (char *) NULL);
      exit (EXIT_SUCCESS);
	default:
	  usage (EXIT_CANCELED);
	}
    }

  if (optind == argc)
    {
      error (0, 0, _("missing program to run"));
      usage (EXIT_CANCELED);
    }

  if (outfile)
    {
      if (append)
	outfp = fopen (outfile, "a");
      else
	outfp = fopen (outfile, "w");
      if (outfp == NULL)
	error (EXIT_CANCELED, errno, "%s", outfile);
    }

  /* If the user specified verbose output, we need to convert
     `longstats' to a `char *'.  */
  if (verbose)
    {
      output_format = (const char *) linear_argv (longstats);
      if (output_format == NULL)
	exit (EXIT_CANCELED);		/* Out of memory.  */
    }

  return (const char **) &argv[optind];
}

static int 
modify_cap(capability, flag, setting)
     int capability;
     cap_flag_t flag;
     cap_flag_value_t setting;
{
    cap_t caps;
    cap_value_t capList[1];
    
    /* Retrieve caller's current capabilities */
    
    // printf("get caps\n");
    
    caps = cap_get_proc();
    if (caps == NULL) {
        perror("cannot get cap");
        return -1;
    }
    /* Change setting of 'capability' in the effective set of 'caps'. The
    third argument, 1, is the number of items in the array 'capList'. */
    
    // printf("set caps\n");
    
    capList[0] = capability;
    if (cap_set_flag(caps, flag, 1, capList, setting)
    ) {
        cap_free(caps);
        perror("cannot set cap");
        return -1;
    }
    /*
    if (prctl(PR_CAPBSET_DROP, capability, 0, 0, 0)) {
        perror("cannot drop cap");
        return -1;
    }
    */
    /* Push modified capability sets back to kernel, to change
    caller's capabilities */
    
    // printf("set caps proc\n");
    
    if (cap_set_proc(caps) == -1) {
        cap_free(caps);
        perror("cannot set cap to process");
        return -1;
    }
    
    /* Free the structure that was allocated by libcap */
    
    // printf("free caps\n");
    
    if (cap_free(caps) == -1) {
        perror("cannot free cap");
        return -1;
    }
    
    /* check caller's current capabilities */
    
    // printf("get caps again\n");
    
    caps = cap_get_proc();
    if (caps == NULL) {
        perror("cannot get cap");
        return -1;
    }
    
    // printf("print caps\n");
    
    cap_flag_value_t cap_flags_value;
    int failed = 0;
    
    char *cap_name = cap_to_name(capability);
    printf("%-15s", cap_name);
    cap_free(cap_name);
    
    cap_get_flag(caps, capability, flag, &cap_flags_value);
    if (flag == CAP_EFFECTIVE)
      printf(" EFFECTIVE %-4s\n", (cap_flags_value == CAP_SET) ? "OK" : "NOK");
    if (flag == CAP_PERMITTED)
      printf(" PERMITTED %-4s\n", (cap_flags_value == CAP_SET) ? "OK" : "NOK");
    if (flag == CAP_INHERITABLE)
      printf(" INHERITABLE %-4s\n", (cap_flags_value == CAP_SET) ? "OK" : "NOK");
    if (cap_flags_value != setting) failed = 1;
    /*
    cap_get_flag(caps, capability, CAP_EFFECTIVE, &cap_flags_value);
    printf(" EFFECTIVE %-4s ", (cap_flags_value == CAP_SET) ? "OK" : "NOK");
    if (cap_flags_value != setting) failed = 1;

    cap_get_flag(caps, capability, CAP_PERMITTED, &cap_flags_value);
    printf(" PERMITTED %-4s ", (cap_flags_value == CAP_SET) ? "OK" : "NOK");
    if (cap_flags_value != setting) failed = 1;

    cap_get_flag(caps, capability, CAP_INHERITABLE, &cap_flags_value);
    printf(" INHERITABLE %-4s ", (cap_flags_value == CAP_SET) ? "OK" : "NOK");
    if (cap_flags_value != setting) failed = 1;
    
    int in_bounding = prctl(PR_CAPBSET_READ, capability, 0, 0, 0);
    printf(" Bounding_Set %-4s ", in_bounding ? "OK" : "NOK");
    */
    
    if (failed) {
        perror("set cap failed");
        return -1;
    }
    
    /* Free the structure that was allocated by libcap */
    
    // printf("free caps again\n");
    
    if (cap_free(caps) == -1) {
        perror("cannot free cap");
        return -1;
    }
    
    return 0;
}

static int
modify_caps(capabilities, flag, count, setting)
  int *capabilities;
  cap_flag_t flag;
  int count;
  cap_flag_value_t setting;
{
  for (int i = 0; i < count; i++) {
    if (modify_cap(capabilities[i], flag, setting)) {
      return -1;
    }
  }
  
  return 0;
}


static int
drop_bounding_cap(capability)
  int capability;
{
    if (prctl(PR_CAPBSET_DROP, capability, 0, 0, 0)) {
        perror("cannot drop bounding cap");
        return -1;
    }
    
    char *cap_name = cap_to_name(capability);
    printf("%-15s", cap_name);
    
    bool in_bounding = prctl(PR_CAPBSET_READ, capability, 0, 0, 0);
    printf(" Bounding_Set %-4s\n", in_bounding ? "OK" : "NOK");
    return 0;
}

static int
drop_bounding_caps(capabilities, count)
  int *capabilities;
  int count;
{
  int capability;
  
  for (int i = 0; i < count; i++) {
    capability = capabilities[i];
    if (drop_bounding_cap(capability)) {
      return 1;
    }
  }
  
  return 0;
}

static int
clear_effective_and_permitted_caps_except_setpcap()
{
    cap_t caps = cap_get_proc();
    if (caps == NULL) {
        perror("cannot get cap");
        return -1;
    }
    
    cap_value_t capList[1];
    capList[0] = CAP_SETPCAP;
    
    if (
        cap_set_flag(caps, CAP_EFFECTIVE, 1, capList, CAP_SET) ||
        cap_set_flag(caps, CAP_PERMITTED, 1, capList, CAP_SET)
    ) {
        cap_free(caps);
        perror("cannot set cap");
        return -1;
    }
    
    if (cap_free(caps) == -1) {
        perror("cannot free cap");
        return -1;
    }
    
    return 0;
}


/* Run command CMD and return statistics on it.
   Put the statistics in *RESP.  */

static void
run_command (cmd, resp)
     char *const *cmd;
     RESUSE *resp;
{
  pid_t pid;			/* Pid of child.  */
  sighandler interrupt_signal, quit_signal;
  int saved_errno;

  resuse_start (resp);
  
  if (pid_namesapce) {
    unshare(CLONE_NEWUTS | CLONE_NEWIPC | CLONE_NEWPID | CLONE_NEWNS);
  }
  
  pid = fork ();		/* Run CMD as child process.  */
  if (pid < 0)
    error (EXIT_CANCELED, errno, "cannot fork");
  else if (pid == 0)
    {
      if (pid_namesapce) {
        if (mount("none", "/proc", NULL, MS_PRIVATE|MS_REC, NULL)) {
          perror("Cannot umount proc! errno");
          _exit (EXIT_CANNOT_INVOKE);
        }
        
        if (mount("proc", "/proc", "proc", MS_NOSUID|MS_NOEXEC|MS_NODEV, NULL)) {
          perror("Cannot mount proc! errno=");
          _exit (EXIT_CANNOT_INVOKE);
        }
      }
      
      if (
        modify_caps(cap_drops, CAP_INHERITABLE, cap_drops_length, CAP_CLEAR) ||
        modify_caps(cap_drops, CAP_EFFECTIVE, cap_drops_length, CAP_CLEAR) ||
        modify_caps(cap_drops, CAP_PERMITTED , cap_drops_length, CAP_CLEAR) ||
        drop_bounding_caps(cap_drops, cap_drops_length)
      ) {
        perror("cannot modify cap");
        _exit (EXIT_CANNOT_INVOKE);
      }
      
      prctl(PR_SET_KEEPCAPS, 1, 0, 0, 0);
      
      if (set_user) {
        if (
          setgroups(set_groups_length, set_groups) == -1 ||
          setgid(set_gid) ||
          setuid(set_uid)
        ) {
          perror("cannot set user");
          _exit (EXIT_CANNOT_INVOKE);
        }
      }
      
      prctl(PR_SET_KEEPCAPS, 0, 0, 0, 0);
      modify_cap(CAP_SETPCAP, CAP_EFFECTIVE, CAP_SET);
      
      if (cap_drop_setuid) {
        if (
          modify_cap(CAP_SETUID, CAP_INHERITABLE, CAP_CLEAR) ||
          modify_cap(CAP_SETUID, CAP_EFFECTIVE, CAP_CLEAR) ||
          modify_cap(CAP_SETUID, CAP_PERMITTED, CAP_CLEAR) ||
          drop_bounding_cap(CAP_SETUID)
        ) {
          perror("cannot modify cap");
          _exit (EXIT_CANNOT_INVOKE);
        }
      }
      
      if (cap_drop_setgid) {
        if (
          modify_cap(CAP_SETGID, CAP_INHERITABLE, CAP_CLEAR) ||
          modify_cap(CAP_SETGID, CAP_EFFECTIVE, CAP_CLEAR) ||
          modify_cap(CAP_SETGID, CAP_PERMITTED, CAP_CLEAR) ||
          drop_bounding_cap(CAP_SETGID)
        ) {
          perror("cannot modify cap");
          _exit (EXIT_CANNOT_INVOKE);
        }
      }
      
      if (set_user && set_uid != 0) {
        clear_effective_and_permitted_caps_except_setpcap();
      }
      
      if (cap_drop_setpcap) {
        if (
          // modify_cap(CAP_SETPCAP, CAP_INHERITABLE, CAP_CLEAR) ||
          // modify_cap(CAP_SETPCAP, CAP_EFFECTIVE, CAP_CLEAR) ||
          // modify_cap(CAP_SETPCAP, CAP_PERMITTED, CAP_CLEAR) ||
          drop_bounding_cap(CAP_SETPCAP)
        ) {
          perror("cannot modify cap");
          _exit (EXIT_CANNOT_INVOKE);
        }
      } else if (set_user && set_uid != 0) {
        if (
          modify_cap(CAP_SETPCAP, CAP_INHERITABLE, CAP_CLEAR) ||
          modify_cap(CAP_SETPCAP, CAP_EFFECTIVE, CAP_CLEAR) ||
          modify_cap(CAP_SETPCAP, CAP_PERMITTED, CAP_CLEAR)
        ) {
          perror("cannot modify cap");
          _exit (EXIT_CANNOT_INVOKE);
        }
      }
      
      /* If child.  */
      /* Don't cast execvp arguments; that causes errors on some systems,
	 versus merely warnings if the cast is left off.  */
      execvp (cmd[0], cmd);
      saved_errno = errno;
      error (0, errno, "cannot run %s", cmd[0]);
      _exit (saved_errno == ENOENT ? EXIT_ENOENT : EXIT_CANNOT_INVOKE);
    }
  
  if (pidfp != NULL) {
    fprintf (pidfp, "%lu\n", (unsigned long)pid);
    fclose(pidfp);
    pidfp = NULL;
  }
  
  /* Have signals kill the child but not self (if possible).  */
  interrupt_signal = signal (SIGINT, SIG_IGN);
  quit_signal = signal (SIGQUIT, SIG_IGN);

  if (resuse_end (pid, resp) == 0)
    error (1, errno, "error waiting for child process");

  /* Re-enable signals.  */
  signal (SIGINT, interrupt_signal);
  signal (SIGQUIT, quit_signal);
}

int
main (argc, argv)
     int argc;
     char **argv;
{
  const char **command_line;
  RESUSE res;
  int status;

  set_program_name (argv[0]);
  command_line = getargs (argc, argv);
  run_command (command_line, &res);
  summarize (outfp, output_format, command_line, &res);
  fflush (outfp);

  if (WIFSTOPPED (res.waitstatus))
    status = WSTOPSIG (res.waitstatus) + SIGNALLED_OFFSET;
  else if (WIFSIGNALED (res.waitstatus))
    status = WTERMSIG (res.waitstatus) + SIGNALLED_OFFSET;
  else if (WIFEXITED (res.waitstatus))
    status = WEXITSTATUS (res.waitstatus);
  else
    {
      /* shouldn't happen.  */
      error (0, 0, _("unknown status from command (%d)"), res.waitstatus);
      status = EXIT_FAILURE;
    }

  return status;
}

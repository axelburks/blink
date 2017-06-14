/*-
 * Copyright (c) 1988, 1993, 1994
 *	The Regents of the University of California.  All rights reserved.
 *
 * This code is derived from software contributed to Berkeley by
 * David Hitz of Auspex Systems Inc.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
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

#if 0
#ifndef lint
static char const copyright[] =
"@(#) Copyright (c) 1988, 1993, 1994\n\r\
	The Regents of the University of California.  All rights reserved.\n\r";
#endif /* not lint */

#ifndef lint
static char sccsid[] = "@(#)cp.c	8.2 (Berkeley) 4/1/94";
#endif /* not lint */
#endif
#include <sys/cdefs.h>
__FBSDID("$FreeBSD: src/bin/cp/cp.c,v 1.52 2005/09/05 04:36:08 csjp Exp $");

/*
 * Cp copies source files to target files.
 *
 * The global PATH_T structure "to" always contains the path to the
 * current target file.  Since fts(3) does not change directories,
 * this path can be either absolute or dot-relative.
 *
 * The basic algorithm is to initialize "to" and use fts(3) to traverse
 * the file hierarchy rooted in the argument list.  A trivial case is the
 * case of 'cp file1 file2'.  The more interesting case is the case of
 * 'cp file1 file2 ... fileN dir' where the hierarchy is traversed and the
 * path (relative to the root of the traversal) is appended to dir (stored
 * in "to") to form the final target path.
 */

#include <sys/types.h>
#include <sys/stat.h>

#include <err.h>
#include <errno.h>
#include <fts.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifdef __APPLE__
#include <copyfile.h>
// #include <get_compat.h>
// #else /* !__APPLE__ */
#define COMPAT_MODE(a,b) (1)
#endif /* __APPLE__ */

#include "extern.h"
extern void cp_usage(void);

#define	STRIP_TRAILING_SLASH(p) {					\
        while ((p).p_end > (p).p_path + 1 && (p).p_end[-1] == '/')	\
                *--(p).p_end = 0;					\
}

static char emptystring[] = "";

PATH_T to = { to.p_path, emptystring, "" };

int fflag, iflag, nflag, pflag, vflag;
#ifdef __APPLE__
int Xflag;
#endif /* __APPLE__ */
static int Rflag, rflag;
volatile sig_atomic_t info;

enum op { FILE_TO_FILE, FILE_TO_DIR, DIR_TO_DNE };

static int copy(char *[], enum op, int);
static void siginfo(int __unused);

int
cp_main(int argc, char *argv[])
{
	struct stat to_stat, tmp_stat;
	enum op type;
	int Hflag, Lflag, Pflag, ch, fts_options, r, have_trailing_slash;
	char *target;

    fflag = iflag = nflag = pflag = vflag = 0;
#ifdef __APPLE__
    Xflag = 0;
#endif /* __APPLE__ */
    Rflag = rflag = 0;

	Hflag = Lflag = Pflag = 0;
	while ((ch = getopt(argc, argv, "HLPRXafinprv")) != -1)
		switch (ch) {
		case 'H':
			Hflag = 1;
			Lflag = Pflag = 0;
			break;
		case 'L':
			Lflag = 1;
			Hflag = Pflag = 0;
			break;
		case 'P':
			Pflag = 1;
			Hflag = Lflag = 0;
			break;
		case 'R':
			Rflag = 1;
			break;
		case 'X':
			Xflag = 1;
			break;
		case 'f':
			fflag = 1;
			/* Determine if the STD is SUSv3 or Legacy */
			if (COMPAT_MODE("bin/cp", "unix2003"))
				nflag = 0;	/* reset nflag, but not iflag */
			else
				iflag = nflag = 0;	/* reset both */
			break;
		case 'i':
			iflag = 1;
			if (COMPAT_MODE("bin/cp", "unix2003"))
				nflag = 0;	/* reset nflag, but not fflag */
			else
				fflag = nflag = 0;
			break;
		case 'n':
			nflag = 1;
			fflag = iflag = 0;
			break;
		case 'p':
			pflag = 1;
			break;
		case 'r':
			rflag = 1;
			break;
		case 'v':
			vflag = 1;
			break;
		case 'a':
			pflag = 1;
			Pflag = 1;
			Rflag = 1;
			break;
		default:
			cp_usage();
            return 0;
			break;
		}
	argc -= optind;
	argv += optind;

    if (argc < 2) {
		cp_usage();
        return 0;
    }

	fts_options = FTS_NOCHDIR | FTS_PHYSICAL;
	if (rflag) {
		if (Rflag)
			errx(1,
		    "the -R and -r options may not be specified together.");
		if (Hflag || Lflag || Pflag)
			errx(1,
	"the -H, -L, and -P options may not be specified with the -r option.");
		fts_options &= ~FTS_PHYSICAL;
		fts_options |= FTS_LOGICAL;
	}
	if (Rflag) {
		if (Hflag)
			fts_options |= FTS_COMFOLLOW;
		if (Lflag) {
			fts_options &= ~FTS_PHYSICAL;
			fts_options |= FTS_LOGICAL;
		}
	} else {
		fts_options &= ~FTS_PHYSICAL;
		fts_options |= FTS_LOGICAL | FTS_COMFOLLOW;
	}
	(void)signal(SIGINFO, siginfo);

	/* Save the target base in "to". */
	target = argv[--argc];
	if (strlcpy(to.p_path, target, sizeof(to.p_path)) >= sizeof(to.p_path))
		errx(1, "%s: name too long", target);
	to.p_end = to.p_path + strlen(to.p_path);
        if (to.p_path == to.p_end) {
		*to.p_end++ = '.';
		*to.p_end = 0;
	}
	have_trailing_slash = (to.p_end[-1] == '/');
	if (have_trailing_slash)
		STRIP_TRAILING_SLASH(to);
	to.target_end = to.p_end;

	/* Set end of argument list for fts(3). */
	argv[argc] = NULL;

	/*
	 * Cp has two distinct cases:
	 *
	 * cp [-R] source target
	 * cp [-R] source1 ... sourceN directory
	 *
	 * In both cases, source can be either a file or a directory.
	 *
	 * In (1), the target becomes a copy of the source. That is, if the
	 * source is a file, the target will be a file, and likewise for
	 * directories.
	 *
	 * In (2), the real target is not directory, but "directory/source".
	 */
	r = stat(to.p_path, &to_stat);
	if (r == -1 && errno != ENOENT)
		err(1, "%s", to.p_path);
	if (r == -1 || !S_ISDIR(to_stat.st_mode)) {
		/*
		 * Case (1).  Target is not a directory.
		 */
		if (argc > 1) {
			cp_usage();
			// exit(1);
            return 1;
		}
		/*
		 * Need to detect the case:
		 *	cp -R dir foo
		 * Where dir is a directory and foo does not exist, where
		 * we want pathname concatenations turned on but not for
		 * the initial mkdir().
		 */
		if (r == -1) {
			if (rflag || (Rflag && (Lflag || Hflag)))
				stat(*argv, &tmp_stat);
			else
				lstat(*argv, &tmp_stat);

			if (S_ISDIR(tmp_stat.st_mode) && (Rflag || rflag))
				type = DIR_TO_DNE;
			else
				type = FILE_TO_FILE;
		} else
			type = FILE_TO_FILE;

		if (have_trailing_slash && type == FILE_TO_FILE) {
			if (r == -1)
				errx(1, "directory %s does not exist",
				     to.p_path);
			else
				errx(1, "%s is not a directory", to.p_path);
		}
	} else
		/*
		 * Case (2).  Target is a directory.
		 */
		type = FILE_TO_DIR;

    return copy(argv, type, fts_options);
	// exit (copy(argv, type, fts_options));
}

static int
copy(char *argv[], enum op type, int fts_options)
{
	struct stat to_stat;
	FTS *ftsp;
	FTSENT *curr;
	int base = 0, dne, badcp, rval;
	size_t nlen;
	char *p, *target_mid;
	mode_t mask, mode;

	/*
	 * Keep an inverted copy of the umask, for use in correcting
	 * permissions on created directories when not using -p.
	 */
	mask = ~umask(0777);
	umask(~mask);

	if ((ftsp = fts_open(argv, fts_options, NULL)) == NULL)
		err(1, "fts_open");
	for (badcp = rval = 0; (curr = fts_read(ftsp)) != NULL; badcp = 0) {
		switch (curr->fts_info) {
		case FTS_NS:
		case FTS_DNR:
		case FTS_ERR:
			warnx("%s: %s",
			    curr->fts_path, strerror(curr->fts_errno));
                fprintf(stderr, "\r");
			rval = 1;
			continue;
		case FTS_DC:			/* Warn, continue. */
			warnx("%s: directory causes a cycle", curr->fts_path);
            fprintf(stderr, "\r");
			rval = 1;
			continue;
		default:
			;
		}
#ifdef __APPLE__

#ifdef __clang__
#pragma clang diagnostic push
/* clang doesn't like fts_name[1], but we know better... */
#pragma clang diagnostic ignored "-Warray-bounds"
#endif
		/* Skip ._<file> when using copyfile and <file> exists */
		if ((pflag || !Xflag) && (curr->fts_level != FTS_ROOTLEVEL) &&
		    (curr->fts_namelen > 2) && /* ._\0 is not AppleDouble */
		    (curr->fts_name[0] == '.') && (curr->fts_name[1] == '_')) {
#ifdef __clang__
#pragma clang diagnostic pop
#endif
			struct stat statbuf;
			char path[PATH_MAX];
			char *p = strrchr(curr->fts_path, '/');
			if (p) {
				size_t s = p + 2 - curr->fts_path;
				if (s > sizeof(path)) s = sizeof(path);
				strlcpy(path, curr->fts_path, s);
				strlcat(path, curr->fts_name+2, sizeof(path));
			} else {
				strlcpy(path, curr->fts_name+2, sizeof(path));
			}
			if (!lstat(path, &statbuf)) {
				continue;
			}
		}
#endif /* __APPLE__ */
		/*
		 * If we are in case (2) or (3) above, we need to append the
                 * source name to the target name.
                 */
		if (type != FILE_TO_FILE) {
			/*
			 * Need to remember the roots of traversals to create
			 * correct pathnames.  If there's a directory being
			 * copied to a non-existent directory, e.g.
			 *	cp -R a/dir noexist
			 * the resulting path name should be noexist/foo, not
			 * noexist/dir/foo (where foo is a file in dir), which
			 * is the case where the target exists.
			 *
			 * Also, check for "..".  This is for correct path
			 * concatenation for paths ending in "..", e.g.
			 *	cp -R .. /tmp
			 * Paths ending in ".." are changed to ".".  This is
			 * tricky, but seems the easiest way to fix the problem.
			 *
			 * XXX
			 * Since the first level MUST be FTS_ROOTLEVEL, base
			 * is always initialized.
			 */
			if (curr->fts_level == FTS_ROOTLEVEL) {
				if (type != DIR_TO_DNE) {
					p = strrchr(curr->fts_path, '/');
					base = (p == NULL) ? 0 :
					    (int)(p - curr->fts_path + 1);

					if (!strcmp(&curr->fts_path[base],
					    ".."))
						base += 1;
				} else
					base = curr->fts_pathlen;
			}

			p = &curr->fts_path[base];
			nlen = curr->fts_pathlen - base;
			target_mid = to.target_end;
			if (*p != '/' && target_mid[-1] != '/')
				*target_mid++ = '/';
			*target_mid = 0;
			if (target_mid - to.p_path + nlen >= PATH_MAX) {
				warnx("%s%s: name too long (not copied)",
				    to.p_path, p);
                fprintf(stderr, "\r");
				rval = 1;
				continue;
			}
			(void)strncat(target_mid, p, nlen);
			to.p_end = target_mid + nlen;
			*to.p_end = 0;
			STRIP_TRAILING_SLASH(to);
		}

		if (curr->fts_info == FTS_DP) {
			/*
			 * We are nearly finished with this directory.  If we
			 * didn't actually copy it, or otherwise don't need to
			 * change its attributes, then we are done.
			 */
			if (!curr->fts_number)
				continue;
			/*
			 * If -p is in effect, set all the attributes.
			 * Otherwise, set the correct permissions, limited
			 * by the umask.  Optimise by avoiding a chmod()
			 * if possible (which is usually the case if we
			 * made the directory).  Note that mkdir() does not
			 * honour setuid, setgid and sticky bits, but we
			 * normally want to preserve them on directories.
			 */
			if (pflag) {
				if (setfile(curr->fts_statp, -1))
					rval = 1;
#ifdef __APPLE__
				/* setfile will fail if writeattr is denied */
                if (copyfile(curr->fts_path, to.p_path, NULL, COPYFILE_ACL)<0) {
					warn("%s: unable to copy ACL to %s", curr->fts_path, to.p_path);
                    fprintf(stderr, "\r");
                }
#else  /* !__APPLE__ */
				if (preserve_dir_acls(curr->fts_statp,
				    curr->fts_accpath, to.p_path) != 0)
					rval = 1;
#endif /* __APPLE__ */
			} else {
				mode = curr->fts_statp->st_mode;
				if ((mode & (S_ISUID | S_ISGID | S_ISTXT)) ||
				    ((mode | S_IRWXU) & mask) != (mode & mask))
					if (chmod(to.p_path, mode & mask) != 0){
						warn("chmod: %s", to.p_path);
                        fprintf(stderr, "\r");
						rval = 1;
					}
			}
			continue;
		}

		/* Not an error but need to remember it happened */
		if (stat(to.p_path, &to_stat) == -1)
			dne = 1;
		else {
			if (to_stat.st_dev == curr->fts_statp->st_dev &&
			    to_stat.st_ino == curr->fts_statp->st_ino) {
				warnx("%s and %s are identical (not copied).",
				    to.p_path, curr->fts_path);
                fprintf(stderr, "\r");
				rval = 1;
				if (S_ISDIR(curr->fts_statp->st_mode))
					(void)fts_set(ftsp, curr, FTS_SKIP);
				continue;
			}
			if (!S_ISDIR(curr->fts_statp->st_mode) &&
			    S_ISDIR(to_stat.st_mode)) {
				warnx("cannot overwrite directory %s with "
				    "non-directory %s",
				    to.p_path, curr->fts_path);
                fprintf(stderr, "\r");
				rval = 1;
				continue;
			}
			dne = 0;
		}

		switch (curr->fts_statp->st_mode & S_IFMT) {
		case S_IFLNK:
			/* Catch special case of a non-dangling symlink */
			if ((fts_options & FTS_LOGICAL) ||
			    ((fts_options & FTS_COMFOLLOW) &&
			    curr->fts_level == 0)) {
				if (copy_file(curr, dne))
					badcp = rval = 1;
			} else {	
				if (copy_link(curr, !dne))
					badcp = rval = 1;
			}
			break;
		case S_IFDIR:
			if (!Rflag && !rflag) {
				warnx("%s is a directory (not copied).",
				    curr->fts_path);
                fprintf(stderr, "\r");
				(void)fts_set(ftsp, curr, FTS_SKIP);
				badcp = rval = 1;
				break;
			}
			/*
			 * If the directory doesn't exist, create the new
			 * one with the from file mode plus owner RWX bits,
			 * modified by the umask.  Trade-off between being
			 * able to write the directory (if from directory is
			 * 555) and not causing a permissions race.  If the
			 * umask blocks owner writes, we fail..
			 */
			if (dne) {
				if (mkdir(to.p_path,
					  curr->fts_statp->st_mode | S_IRWXU) < 0) {
					if (COMPAT_MODE("bin/cp", "unix2003")) {
						warn("%s", to.p_path);
                        fprintf(stderr, "\r");
					} else {
						err(1, "%s", to.p_path);
					}
				}
			} else if (!S_ISDIR(to_stat.st_mode)) {
				errno = ENOTDIR;
				if (COMPAT_MODE("bin/cp", "unix2003")) {
					warn("%s", to.p_path);
                    fprintf(stderr, "\r");
				} else {
					err(1, "%s", to.p_path);
				}
			}
			/*
			 * Arrange to correct directory attributes later
			 * (in the post-order phase) if this is a new
			 * directory, or if the -p flag is in effect.
			 */
			curr->fts_number = pflag || dne;
#ifdef __APPLE__
			if (!Xflag) {
                if (copyfile(curr->fts_path, to.p_path, NULL, COPYFILE_XATTR) < 0) {
					warn("%s: unable to copy extended attributes to %s", curr->fts_path, to.p_path);
				/* ACL and mtime set in postorder traversal */
                    fprintf(stderr, "\r");
                }
			}
#endif /* __APPLE__ */
			break;
		case S_IFBLK:
		case S_IFCHR:
			if (Rflag) {
				if (copy_special(curr->fts_statp, !dne))
					badcp = rval = 1;
			} else {
				if (copy_file(curr, dne))
					badcp = rval = 1;
			}
			break;
		case S_IFIFO:
			if (Rflag) {
				if (copy_fifo(curr->fts_statp, !dne))
					badcp = rval = 1;
			} else {
				if (copy_file(curr, dne))
					badcp = rval = 1;
			}
			break;
		default:
			if (copy_file(curr, dne))
				badcp = rval = 1;
			break;
		}
		if (vflag && !badcp)
			(void)printf("%s -> %s\n\r", curr->fts_path, to.p_path);
	}
	if (errno)
		err(1, "fts_read");
	fts_close(ftsp);
	return (rval);
}

static void
siginfo(int sig __unused)
{

	info = 1;
}

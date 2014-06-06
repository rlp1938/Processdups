/*
 * processdups.c
 *
 * Copyright 2014 Bob Parker <rlp1938@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 * MA 02110-1301, USA.
 *
 *
 */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <getopt.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>

#include "config.h"

char *pathend = "!*END*!";

struct filedata {
    char *from; // start of file content
    char *to;   // last byte of data + 1
};

struct hashrecord {
	char thesum[33];
	char ino[21]; // An ASCII formatted inode up to 20 chars on 64 bit.
	char ftyp;
	char path[PATH_MAX];
	char *line;
};

static void help_print(int forced);
static FILE *dofopen(const char *path, const char *mode);
static char *dostrdup(const char *s);
static char *domd5sum(const char *pathname);
static struct hashrecord parse_line(char *line);
static void clipeol(char *line);
static char *thepathname(char *line);
static char *getcluster(char *path, int depth);
static struct filedata *readfile(char *path, int fatal);
static struct filedata *mkstructdata(char *from, char *to);
static void dofwrite(char *filename, char *from, char *to);

void *memmem(const void *haystack, size_t haystacklen,
             const void *needle, size_t needlelen);




static char *helptext = "\n\tUsage: processdups [option] duplicates-list\n"
  "\n\tOptions:\n"
  "\t-h outputs this help message.\n"
  "\t-l causes deleted paths to be re-instated as hard links.\n"
  "\tNB this an interactive program which tells you what it wants\n"
  "\t\ton the way.\n"
  ;
static int linkthem;


int main(int argc, char **argv)
{
	int opt, doquit;
	char choice[10];
	struct stat sb;
	struct filedata *fdat;
	char *dupsfile;
	char *from, *to, *line, *writefrom;
	struct hashrecord hrlist[30];
	char *destroy =
	"Enter the path number to preserve, will DELETE others: ";
	char *destroyAndLink =
	"Enter the path number to preserve, will LINK others to that: ";
	char *print_this;

	// set defaults
	linkthem = 0;

	while((opt = getopt(argc, argv, ":hl")) != -1) {
		switch(opt){
		case 'h':
			help_print(0);
		break;
		case 'l':
			linkthem = 1;
		break;
		case ':':
			fprintf(stderr, "Option %c requires an argument\n",optopt);
			help_print(1);
		break;
		case '?':
			fprintf(stderr, "Illegal option: %c\n",optopt);
			help_print(1);
		break;
		} //switch()
	}//while()
	// now process the non-option arguments

	// 1.Check that argv[???] exists.
	if (!(argv[optind])) {
		fprintf(stderr, "No duplicates file provided\n");
		help_print(1);
	}

	// 2. Check that the file exists.
	if ((stat(argv[optind], &sb)) == -1){
		perror(argv[optind]);
		help_print(EXIT_FAILURE);
	}

	// 3. Check that this is a file
	if (!(S_ISREG(sb.st_mode))) {
		fprintf(stderr, "Not a file: %s\n", argv[optind]);
		help_print(EXIT_FAILURE);
	}

	// read file into memory
	dupsfile = argv[optind];
	fdat = readfile(dupsfile, 1);
	from = fdat->from;
	to = fdat->to;
	writefrom = from;
	// turn the entire amorphous mess into an array of C strings
	{
		char *cp = from;
		while (cp < to) {
			if (*cp == '\n') *cp = '\0';
			cp++;
		}
	}

	line = writefrom;
	while (line < to) {
		int hrindex, hrtotal, chosen;
		char currenthash[33];
		char *line2;
		char *preservepath = NULL;
		struct hashrecord hr;
		// give user chance to quit here.
		hrindex = hrtotal = 0;
		hr = parse_line(line);
		if (!strlen(hr.path)) break;
		strcpy(currenthash, hr.thesum);
		hrlist[hrindex] = hr;
		hrindex++;
		line2 = line + strlen(line) +1;	// at beginning next line.
		hr = parse_line(line2);
		while (strcmp((char *)currenthash, (char *)hr.thesum) == 0) {
			hrlist[hrindex] = hr;
			hrindex++;
			// get next line
			line2 = line2 + strlen(line2) +1;	// line beginning
			if (strlen(line2)) {
				hr = parse_line(line2);
				if (!(strlen(hr.path))) break;
			} else {
				break;
			}
		}
		hrtotal = hrindex;
		// display list of dups and get delete criteria
		fprintf(stdout, "\n\tMD5SUM: %s\n", currenthash);
		for (hrindex=0; hrindex<hrtotal; hrindex++){
			fprintf(stdout, " %d%s  %s %c\n", hrindex,
				hrlist[hrindex].path, hrlist[hrindex].ino,
				hrlist[hrindex].ftyp);
		}
		if (linkthem) {
			print_this = destroyAndLink;
		} else {
			print_this = destroy;
		}
		fprintf(stdout, "%s%s", print_this,
		"\n(Enter -1 to ignore this group.)"
		"\n(Enter -5 to delete all of this group.)"
		"\n(Enter any other number to quit.)" );
		if (!(fgets(choice, 10, stdin))) {
			perror(choice);	// stop gcc bitching
		}
		chosen = atoi(choice);
		// Range check on chosen.
		doquit = 1;
		if (chosen == -1) goto reinit;
		if (chosen == -5) {
			doquit = 0;
		}
		if (chosen >= 0 && chosen < hrtotal) doquit = 0;
		if (doquit) break;
		// Subject to some deletions having been made, this will rewrite
		// the dups file with last bundle of entries still in the file.
		for (hrindex=0; hrindex<hrtotal; hrindex++){
			// unlink the losers
			if (hrindex != chosen){
				if (unlink(hrlist[hrindex].path) == -1){
					perror(hrlist[hrindex].path);
				}
			} else {
				preservepath = hrlist[hrindex].path;
			}
		}
		sync();
		if (!(linkthem)) goto reinit;
		for (hrindex=0; hrindex<hrtotal; hrindex++){
			// relink the losers
			if (hrindex == chosen) continue;
			if (preservepath) {
				if (link(preservepath, hrlist[hrindex].path) == -1){
					perror(hrlist[hrindex].path);
				}
			}
		}
reinit:
		// re-initialise line
		line = line2;	// hr will be done again at top.
		writefrom = line;	// when I quit I'll rewrite the dups file
							// from writefrom
	}
	if (writefrom != from) {
		FILE *fpo = dofopen(dupsfile, "w");
		line = writefrom;
		while(line < to) {
			fprintf(fpo, "%s\n", line);
			line += strlen(line);
			line++;	// looking at next line
		}
		fclose(fpo);
	}
	return 0;
}

void help_print(int forced){
    fputs(helptext, stderr);
    exit(forced);
} // help_print()

struct filedata *mkstructdata(char *from, char *to)
{
	// create and initialise this struct in 1 place only
	struct filedata *dp = malloc(sizeof(struct filedata));
	if (!(dp)){
		perror("malloc failure making 'struct filedata'");
		exit(EXIT_FAILURE);
	}
	dp->from = from;
	dp->to = to;
	return dp;
} // mkstructdata()

struct filedata *readfile(char *path, int fatal)
{
    /*
     * open a file and read contents into memory.
     * if fatal is 0 and the file does not exist return NULL
     * but if it's non-zero abort and do that for all other
     * errors.
    */
    struct stat sb;
    FILE *fpi;
    off_t bytes;
    char *from, *to;

    if ((stat(path, &sb)== -1)){
        if (!(fatal)) return NULL;
			perror(path);
			exit(EXIT_FAILURE);
    }
    if (!(S_ISREG(sb.st_mode) || S_ISLNK(sb.st_mode))) {
        perror(path);
        exit(EXIT_FAILURE);
    }
    fpi = fopen(path, "r");
    if (!(fpi)) {
		perror(path);
        exit(EXIT_FAILURE);
    }
    from = malloc(sb.st_size);
    if(!(from)) {
        perror(path);
        exit(EXIT_FAILURE);
    }
    bytes = fread(from, 1, sb.st_size, fpi);
    if (bytes != sb.st_size){
        perror(path);
        exit(EXIT_FAILURE);
    }
    to = from + bytes;
    fclose(fpi);
    return mkstructdata(from, to);
} // readfile()

struct hashrecord parse_line(char *line)
{
	// <path> <MD5> <inode> <f|s>
	char *cp, *bcp;
	struct hashrecord hr;
	char buf[PATH_MAX];

	strcpy(buf, line);
	cp = strstr(buf, pathend);
	if (cp) {
		*cp = '\0';
	} else {
		hr.path[0] = '\0';
		return hr;
	}

	strcpy(hr.path, buf);
	cp += strlen(pathend);
	cp++;	// looking at input MD5sum.
	strncpy(hr.thesum, cp, 32);
	hr.thesum[32] = '\0';
	cp += 32;
	cp++;	// looking at input inode, as string.
	bcp = cp;
	while (*bcp != ' ') bcp++;
	*bcp = '\0';
	strcpy(hr.ino, cp);
	cp = bcp + 1;	// looking at a char [f|s]
	hr.ftyp = *cp;

	hr.line = line;	// not sure that I need this at all.

	return hr;
} // parse_line()

void dofwrite(char *filename, char *from, char *to)
{
	FILE *fpo;
	size_t written;

	fpo = dofopen(filename, "w");
	written = fwrite(from, 1, to-from, fpo);
	if (written != to-from) {
		fprintf(stderr, "For file: %s\n expected to write: %lu bytes"
			"but only wrote %lu bytes.\n", filename, to-from, written);
	}
} // dofwrite()

FILE *dofopen(const char *path, const char *mode)
{
	// fopen with error handling
	FILE *fp = fopen(path, mode);
	if(!(fp)){
		perror(path);
		exit(EXIT_FAILURE);
	}
	return fp;
} // dofopen()


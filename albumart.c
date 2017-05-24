/* MiniDLNA media server
 * Copyright (C) 2008  Justin Maggard
 *
 * This file is part of MiniDLNA.
 *
 * MiniDLNA is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 as
 * published by the Free Software Foundation.
 *
 * MiniDLNA is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with MiniDLNA. If not, see <http://www.gnu.org/licenses/>.
 */
#include "config.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/param.h>
#include <limits.h>
#include <libgen.h>
#include <setjmp.h>
#include <errno.h>
#include <ftw.h>

#include <jpeglib.h>

#include "upnpglobalvars.h"
#include "albumart.h"
#include "sql.h"
#include "utils.h"
#include "image_utils.h"
#include "libav.h"
#include "video_thumb.h"
#include "log.h"

image_size_type_t image_size_types[] = {
	{ JPEG_TN,  "jpeg_tn",   160,  160 },
	{ JPEG_SM,  "jpeg_sm",   640,  480 },
	{ JPEG_MED, "jpeg_med", 1024,  768 },
	{ JPEG_LRG, "jpeg_lrg", 4096, 4096 },
	{ JPEG_INV, "",            0,    0 }
};

const image_size_type_t*
get_image_size_type(image_size_type_enum size_type)
{
	if (size_type < JPEG_TN || size_type > JPEG_LRG) size_type = JPEG_INV;
	return &image_size_types[size_type];
}

int
art_cache_path(const image_size_type_t *image_size, const char* postfix, const char *orig_path, char **cache_file)
{
	unsigned int hash = DJBHash((uint8_t*)orig_path, strlen(orig_path));
	#ifdef DEBUG
	const char *fname = strrchr(orig_path, '/');
	if(!fname) fname = orig_path; else ++fname;
	#endif
	if(xasprintf(
		cache_file,
		"%s/art_cache/%08x"
		#ifdef DEBUG
		" (%s)"
		#endif
		"%s%s",
		db_path,
		hash,
		#ifdef DEBUG
		fname,
		#endif
		image_size ? "/XXXXxYYYY" : "", // holds the image size, 10 chars max. I.e. '.4096x4096'
		postfix
	) < 0 )  return 0;

	if(image_size)
	{
		// '/' is not part of a valid filename, but we added it there.
		// This is how we know what to replace, and why we can safely do so.
		char* replace = strrchr(*cache_file, '/');
		sprintf(replace, ".%dx%d%s", image_size->width, image_size->height, postfix);
	}

	return 1;
}

int
art_cache_exists(const image_size_type_t *image_size_type, const char* postfix, const char *orig_path, char **cache_file)
{
	if(!art_cache_path(image_size_type, postfix, orig_path, cache_file))
		return 0;
	return (!access(*cache_file, F_OK));
}

void
art_cache_cleanup(const char* path)
{
	char* cache_file = NULL;

	image_size_type_t* image_size = image_size_types;
	do {

	#ifdef ENABLE_VIDEO_THUMB
		/* Remove video thumbnails */
		if(art_cache_exists(image_size, ".jpg", path, &cache_file))
		{
			if(!remove(cache_file))
				DPRINTF(E_DEBUG, L_INOTIFY, "Removed video thumbnail (%s).\n", cache_file);
			free(cache_file);
		}
	#endif

		if(art_cache_exists(image_size, ".mta", path, &cache_file))
		{
			sql_exec(db, "DELETE from MTA where PATH = '%q'", cache_file);
			if(!remove(cache_file))
				DPRINTF(E_DEBUG, L_INOTIFY, "Removed MTA data (%s).\n", cache_file);
			free(cache_file);
		}

	} while(image_size++); // one call to art_cache_exists() with NULL
}

const char* art_cache_rename_nftw_renamer_old_path;
int art_cache_rename_nftw_renamer(
	const char *fpath,
	const struct stat *sb,
	int typeflag,
	struct FTW *ftwbuf
) {
	if( typeflag & FTW_DP ) { // directory
		return 0;
	}

	// If ftwbuf->level is 0, it means that we were passed file, instead of a
	// directory. Therefor we don't need to 'construct' the old filename, it's
	// already there.
	// Note that in the case of a directory rename all file names remain the
	// same (so we can construct the old name from the old path), but in a the
	// case of renamed file, the file itself has a different name.
	const char* old_fpath = NULL;
	char buffer[PATH_MAX] = {};
	if(ftwbuf->level) {
		const char* fname = fpath + ftwbuf->base;
		snprintf(buffer, sizeof(buffer), "%s/%s", art_cache_rename_nftw_renamer_old_path, fname);
		old_fpath = buffer;
	}
	else {
		old_fpath = art_cache_rename_nftw_renamer_old_path;
	}

	char* old_cache_file = NULL;
	if(!art_cache_path(NULL, ".jpg", old_fpath, &old_cache_file)) {
		return 0;
	}

	char* new_cache_file = NULL;
	if(!art_cache_path(NULL, ".jpg", fpath, &new_cache_file)) {
		free(old_cache_file);
		return 0;
	}

	DPRINTF(E_DEBUG, L_GENERAL, "rename for\n '%s' ('%s') -->\n '%s' ('%s')\n", old_fpath, old_cache_file, fpath, new_cache_file);
	if(rename(old_cache_file, new_cache_file)) {
		DPRINTF(E_DEBUG, L_GENERAL, "rename failed: %s (%d)\n", strerror(errno), errno);
	}
	else {
		// Update database, too
		int ret = 0;

		ret = sql_exec(db, "UPDATE ALBUM_ART SET PATH = '%q' WHERE PATH = '%q'", new_cache_file, old_cache_file);
		if(SQLITE_OK != ret)
		{
			DPRINTF(E_WARN, L_METADATA, "Error renaming ALBUM_ART date base entry for '%s': %d\n", old_cache_file, ret);
		}

		ret = sql_exec(db, "UPDATE DETAILS SET PATH = '%q' WHERE PATH = '%q'", fpath, old_fpath);
		if(SQLITE_OK != ret)
		{
			DPRINTF(E_WARN, L_METADATA, "Error renaming DETAILS date base entry for '%s': %d\n", old_fpath, ret);
		}
	}

	free(old_cache_file);
	free(new_cache_file);

	return 0;
}

int
art_cache_rename(const char * oldpath, const char * newpath)
{
	// i.e. not thread safe
	art_cache_rename_nftw_renamer_old_path = oldpath;
	// StackOverflow says 15 is a good number of file descriptors:
	// http://stackoverflow.com/questions/8436841/how-to-recursively-list-directories-in-c-on-linux
	return nftw(newpath, art_cache_rename_nftw_renamer, 15, 0);
}

static int
save_resized_album_art_from_imsrc_to(const image_s *imsrc, const char *src_file, const char *dst_file, const image_size_type_t *image_size_type)
{
	int dstw, dsth;
	char *result;

	if (!imsrc || !image_size_type)
		return -1;

	if (imsrc->width > imsrc->height)
	{
		dstw = image_size_type->width;
		dsth = (imsrc->height << 8) / ((imsrc->width << 8) / dstw);
	}
	else
	{
		dsth = image_size_type->height;
		dstw = (imsrc->width << 8) / ((imsrc->height << 8) / dsth);
	}

	if (dstw > imsrc->width && dsth > imsrc->height)
	{
		/* if requested dimensions are bigger than image, don't upsize but
		 * link file or save as-is if linking fails */
		int ret = link_file(src_file, dst_file);
		result = (ret == 0) ? (char*)dst_file : image_save_to_jpeg_file(imsrc, dst_file);
	}
	else
	{
		image_s *imdst = image_resize(imsrc, dstw, dsth);
		result = image_save_to_jpeg_file(imdst, dst_file);
		image_free(imdst);
	}

	if (result == NULL)
	{
		DPRINTF(E_WARN, L_ARTWORK, "Failed to create albumart cache of '%s' to '%s' [%s]\n", src_file, dst_file, strerror(errno));
		return -1;
	}

	return 0;
}

static char *
save_resized_album_art_from_imsrc(const image_s *imsrc, const char *path, const image_size_type_t *image_size_type)
{
	char *cache_file;
	if (!image_size_type)
		return NULL;

	if(!art_cache_path(image_size_type, ".jpg", path, &cache_file))
		return NULL;

	int ret = save_resized_album_art_from_imsrc_to(imsrc, path, cache_file, image_size_type);
	if (ret != 0)
	{
		free(cache_file);
		cache_file = NULL;
	}

	return cache_file;
}

int
save_resized_album_art_from_file_to_file(const char *path, const char *dst_file, const image_size_type_t *image_size_type)
{
	image_s *imsrc = image_new_from_jpeg(path, 1, NULL, 0, 1, ROTATE_NONE);
	int ret = save_resized_album_art_from_imsrc_to(imsrc, path, dst_file, image_size_type);
	image_free(imsrc);
	return ret;
}

/* And our main album art functions */
void
update_if_album_art(const char *path)
{
	char *dir;
	char *match;
	char file[MAXPATHLEN];
	char fpath[MAXPATHLEN];
	char dpath[MAXPATHLEN];
	int ncmp = 0;
	int album_art;
	DIR *dh;
	struct dirent *dp;
	enum file_types type = TYPE_UNKNOWN;
	int64_t art_id = 0;
	int ret;

	strncpyt(fpath, path, sizeof(fpath));
	match = basename(fpath);
	/* Check if this file name matches a specific audio or video file */
	if( ends_with(match, ".cover.jpg") )
	{
		ncmp = strlen(match)-10;
	}
	else
	{
		ncmp = strrchr(match, '.') - match;
	}
	/* Check if this file name matches one of the default album art names */
	album_art = is_album_art(match);

	strncpyt(dpath, path, sizeof(dpath));
	dir = dirname(dpath);
	dh = opendir(dir);
	if( !dh )
		return;
	while ((dp = readdir(dh)) != NULL)
	{
		if (is_reg(dp) == 1)
		{
			type = TYPE_FILE;
		}
		else if (is_dir(dp) == 1)
		{
			type = TYPE_DIR;
		}
		else
		{
			snprintf(file, sizeof(file), "%s/%s", dir, dp->d_name);
			type = resolve_unknown_type(file, ALL_MEDIA);
		}
		if( type != TYPE_FILE )
			continue;
		if( (dp->d_name[0] != '.') &&
		    (is_video(dp->d_name) || is_audio(dp->d_name)) &&
		    (album_art || strncmp(dp->d_name, match, ncmp) == 0) )
		{
			DPRINTF(E_DEBUG, L_METADATA, "New file %s looks like cover art for %s\n", path, dp->d_name);
			snprintf(file, sizeof(file), "%s/%s", dir, dp->d_name);
			art_id = find_album_art(file, NULL, 0);
			ret = sql_exec(db, "UPDATE DETAILS set ALBUM_ART = %lld where PATH = '%q'", (long long)art_id, file);
			if( ret != SQLITE_OK )
				DPRINTF(E_WARN, L_METADATA, "Error setting %s as cover art for %s\n", match, dp->d_name);
		}
	}
	closedir(dh);
}

char *
check_embedded_art(const char *path, uint8_t *image_data, int image_size)
{
	char *art_path = NULL, *thumb_art_path = NULL;
	image_s *imsrc;
	static char last_path[PATH_MAX];
	static unsigned int last_hash = 0;
	static int last_success = 0;
	unsigned int hash;

	if( !image_data || !image_size || !path )
	{
		return NULL;
	}

	imsrc = image_new_from_jpeg(NULL, 0, image_data, image_size, 1, ROTATE_NONE);
	if( !imsrc )
	{
		last_success = 0;
		return NULL;
	}

	/* If the embedded image matches the embedded image from the last file we
	 * checked, just make a link. Better than storing it on the disk twice.
	 *
	 * Daniel:
	 * Is this really worth the complexity? We don't seem to bother with this
	 * for resized images at all...
	 */
	hash = DJBHash(image_data, image_size);
	if(hash == last_hash && last_success)
	{
		if(art_cache_exists(NULL, ".jpg", path, &art_path))
		{
			if(!link_file(last_path, art_path))
			{
				// Linking failed, try to save a new copy instead (below)
				free(art_path);
				art_path = NULL;
			}
		}
		else
		{
			// File did not exist yet, somehow, try again (below)
			free(art_path);
			art_path = NULL;
		}
	}

	// New file, save an original copy
	if(!art_path) {
		last_hash = hash;
		if(!art_cache_path(NULL, ".jpg", path, &art_path))
		{
			// This time it's fatal...
			return NULL;
		}
		image_save_to_jpeg_file(imsrc, art_path);
	}

	/* add a thumbnail version anticipiating a bit for the most likely access.
	 * The webservice will generate other thumbs on the fly if not available */
	thumb_art_path = save_resized_album_art_from_imsrc(imsrc, path, get_image_size_type(JPEG_TN));
	free(thumb_art_path);
	image_free(imsrc);

	if( !art_path )
	{
		DPRINTF(E_WARN, L_ARTWORK, "Invalid embedded album art in %s\n", path);
		last_success = 0;
		return NULL;
	}
	DPRINTF(E_DEBUG, L_ARTWORK, "Found new embedded album art in %s\n", path);
	last_success = 1;
	strcpy(last_path, art_path);

	return(art_path);
}

static char *
check_for_album_file(const char *path)
{
	char file[MAXPATHLEN];
	char mypath[MAXPATHLEN];
	struct album_art_name_s *album_art_name;
	char *p;
	const char *dir;
	struct stat st;
	int ret;

	if( stat(path, &st) != 0 )
		return NULL;

	if( S_ISDIR(st.st_mode) )
	{
		dir = path;
		goto check_dir;
	}
	strncpyt(mypath, path, sizeof(mypath));
	dir = dirname(mypath);

	/* First look for file-specific cover art */
	snprintf(file, sizeof(file), "%s.cover.jpg", path);
	ret = access(file, R_OK);
	if( ret != 0 )
	{
		strncpyt(file, path, sizeof(file));
		p = strrchr(file, '.');
		if( p )
		{
			strcpy(p, ".jpg");
			ret = access(file, R_OK);
		}
		if( ret != 0 )
		{
			p = strrchr(file, '/');
			if( p )
			{
				memmove(p+2, p+1, file+MAXPATHLEN-p-2);
				p[1] = '.';
				ret = access(file, R_OK);
			}
		}
	}
	if (ret == 0) goto add_cached_image;

check_dir:
	/* Then fall back to possible generic cover art file names */
	for (album_art_name = album_art_names; album_art_name; album_art_name = album_art_name->next)
	{
		snprintf(file, sizeof(file), "%s/%s", dir, album_art_name->name);
		if (access(file, R_OK) == 0)
add_cached_image:
		{
			char *cache_file, *thumb;

			DPRINTF(E_DEBUG, L_ARTWORK, "Found album art in %s\n", file);
			if (art_cache_exists(NULL, ".jpg", file, &cache_file))
				return cache_file;

			int ret = copy_file(file, cache_file);
			/* add a thumbnail version anticipiating a bit for the most likely access.
			* The webservice will generate other thumbs on the fly if not available */
			image_s *imsrc = image_new_from_jpeg(file, 1, NULL, 0, 1, ROTATE_NONE);
			if (!imsrc) break;

			thumb = save_resized_album_art_from_imsrc(imsrc, file, get_image_size_type(JPEG_TN));
			image_free(imsrc);
			free(thumb);
			return ret == 0 ? cache_file : NULL;
		}
	}

	return NULL;
}

#ifdef ENABLE_VIDEO_THUMB
char *
generate_thumbnail(const char * path)
{
	char *tfile = NULL;
	char cache_dir[MAXPATHLEN];

	if( art_cache_exists(NULL, ".jpg", path, &tfile) )
		return tfile;

	memset(&cache_dir, 0, sizeof(cache_dir));

	if ( is_video(path) )
	{
		strncpyt(cache_dir, tfile, sizeof(cache_dir)-1);
		if ( !make_dir(dirname(cache_dir), S_IRWXU|S_IRGRP|S_IXGRP|S_IROTH|S_IXOTH) &&
			!video_thumb_generate_tofile(path, tfile, 20, runtime_vars.thumb_width))
			return tfile;
	}
	free(tfile);

	return 0;
}
#endif

int64_t
find_album_art(const char *path, uint8_t *image_data, int image_size)
{

	char *album_art = check_embedded_art(path, image_data, image_size);
	if (album_art == NULL) album_art = check_for_album_file(path);
#ifdef ENABLE_VIDEO_THUMB
	if (album_art == NULL && GETFLAG(THUMB_MASK))
		album_art = generate_thumbnail(path);
#endif
	if (album_art == NULL) return 0;


	int64_t ret = sql_get_int_field(db, "SELECT ID from ALBUM_ART where PATH = '%q'", album_art);
	if (ret == 0)
	{
		if (sql_exec(db, "INSERT into ALBUM_ART (PATH) VALUES ('%q')", album_art) == SQLITE_OK)
		{
			ret = sqlite3_last_insert_rowid(db);
		}
		else
		{
			DPRINTF(E_WARN, L_METADATA, "Error setting %s as cover art for %s\n", album_art, path);
			ret = 0;
		}
	}

	free(album_art);
	return ret;
}

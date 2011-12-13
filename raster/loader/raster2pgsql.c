/*
 * $Id$
 *
 * PostGIS Raster loader
 * http://trac.osgeo.org/postgis/wiki/WKTRaster
 *
 * Copyright 2001-2003 Refractions Research Inc.
 * Copyright 2009 Paul Ramsey <pramsey@cleverelephant.ca>
 * Copyright 2009 Mark Cave-Ayland <mark.cave-ayland@siriusit.co.uk>
 * Copyright (C) 2011 Regents of the University of California
 *   <bkpark@ucdavis.edu>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#include "raster2pgsql.h"
#include "gdal_vrt.h"
#include <assert.h>

/* This is needed by liblwgeom */
void lwgeom_init_allocators(void) {
	lwgeom_install_default_allocators();
}

void rt_init_allocators(void) {
	rt_install_default_allocators();
}

static void
raster_destroy(rt_raster raster) {
	uint16_t i;
	uint16_t nbands = rt_raster_get_num_bands(raster);
	for (i = 0; i < nbands; i++) {
		rt_band band = rt_raster_get_band(raster, i);
		void *mem = rt_band_get_data(band);
		if (mem) free(mem);
		rt_band_destroy(band);
	}
	rt_raster_destroy(raster);
}

/* string replacement function taken from
 * http://ubuntuforums.org/showthread.php?s=aa6f015109fd7e4c7e30d2fd8b717497&t=141670&page=3
 */
/* ---------------------------------------------------------------------------
  Name       : replace - Search & replace a substring by another one.
  Creation   : Thierry Husson, Sept 2010
  Parameters :
      str    : Big string where we search
      oldstr : Substring we are looking for
      newstr : Substring we want to replace with
      count  : Optional pointer to int (input / output value). NULL to ignore.
               Input:  Maximum replacements to be done. NULL or < 1 to do all.
               Output: Number of replacements done or -1 if not enough memory.
  Returns    : Pointer to the new string or NULL if error.
  Notes      :
     - Case sensitive - Otherwise, replace functions "strstr" by "strcasestr"
     - Always allocates memory for the result.
--------------------------------------------------------------------------- */
static char*
strreplace(
	const char *str,
	const char *oldstr, const char *newstr,
	int *count
) {
	const char *tmp = str;
	char *result;
	int found = 0;
	int length, reslen;
	int oldlen = strlen(oldstr);
	int newlen = strlen(newstr);
	int limit = (count != NULL && *count > 0) ? *count : -1;

	tmp = str;
	while ((tmp = strstr(tmp, oldstr)) != NULL && found != limit)
		found++, tmp += oldlen;

	length = strlen(str) + found * (newlen - oldlen);
	if ((result = (char *) rtalloc(length + 1)) == NULL) {
		fprintf(stderr, _("Not enough memory\n"));
		found = -1;
	}
	else {
		tmp = str;
		limit = found; /* Countdown */
		reslen = 0; /* length of current result */

		/* Replace each old string found with new string  */
		while ((limit-- > 0) && (tmp = strstr(tmp, oldstr)) != NULL) {
			length = (tmp - str); /* Number of chars to keep intouched */
			strncpy(result + reslen, str, length); /* Original part keeped */
			strcpy(result + (reslen += length), newstr); /* Insert new string */

			reslen += newlen;
			tmp += oldlen;
			str = tmp;
		}
		strcpy(result + reslen, str); /* Copies last part and ending null char */
	}

	if (count != NULL) *count = found;
	return result;
}

static char *
strtolower(char * str) {
	int j;

	for (j = strlen(str) - 1; j >= 0; j--)
		str[j] = tolower(str[j]);

	return str;
}

/* split a string based on a delimiter */
static char**
strsplit(const char *str, const char *delimiter, int *n) {
	char *tmp = NULL;
	char **rtn = NULL;
	char *token = NULL;

	*n = 0;
	if (!str)
		return NULL;

	/* copy str to tmp as strtok will mangle the string */
	tmp = rtalloc(sizeof(char) * (strlen(str) + 1));
	if (NULL == tmp) {
		fprintf(stderr, _("Not enough memory\n"));
		return NULL;
	}
	strcpy(tmp, str);

	if (!strlen(tmp) || !delimiter || !strlen(delimiter)) {
		*n = 1;
		rtn = (char **) rtalloc(*n * sizeof(char *));
		if (NULL == rtn) {
			fprintf(stderr, _("Not enough memory\n"));
			return NULL;
		}
		rtn[0] = (char *) rtalloc(sizeof(char) * (strlen(tmp) + 1));
		if (NULL == rtn[0]) {
			fprintf(stderr, _("Not enough memory\n"));
			return NULL;
		}
		strcpy(rtn[0], tmp);
		rtdealloc(tmp);
		return rtn;
	}

	token = strtok(tmp, delimiter);
	while (token != NULL) {
		if (*n < 1) {
			rtn = (char **) rtalloc(sizeof(char *));
		}
		else {
			rtn = (char **) rtrealloc(rtn, (*n + 1) * sizeof(char *));
		}
		if (NULL == rtn) {
			fprintf(stderr, _("Not enough memory\n"));
			return NULL;
		}

		rtn[*n] = NULL;
		rtn[*n] = (char *) rtalloc(sizeof(char) * (strlen(token) + 1));
		if (NULL == rtn[*n]) {
			fprintf(stderr, _("Not enough memory\n"));
			return NULL;
		}

		strcpy(rtn[*n], token);
		*n = *n + 1;

		token = strtok(NULL, delimiter);
	}

	rtdealloc(tmp);
	return rtn;
}

static char*
trim(const char *input) {
	char *rtn;
	char *ptr;

	if (!input)
		return NULL;
	else if (!*input)
		return (char *) input;

	/* trim left */
	while (isspace(*input))
		input++;

	/* trim right */
	ptr = ((char *) input) + strlen(input);
	while (isspace(*--ptr));
	*(++ptr) = '\0';

	rtn = rtalloc(sizeof(char) * (strlen(input) + 1));
	if (NULL == rtn) {
		fprintf(stderr, _("Not enough memory\n"));
		return NULL;
	}
	strcpy(rtn, input);

	return rtn;
}

static char*
chartrim(const char *input, char *remove) {
	char *rtn = NULL;
	char *ptr = NULL;

	if (!input)
		return NULL;
	else if (!*input)
		return (char *) input;

	/* trim left */
	while (strchr(remove, *input) != NULL)
		input++;

	/* trim right */
	ptr = ((char *) input) + strlen(input);
	while (strchr(remove, *--ptr) != NULL);
	*(++ptr) = '\0';

	rtn = rtalloc(sizeof(char) * (strlen(input) + 1));
	if (NULL == rtn) {
		fprintf(stderr, _("Not enough memory\n"));
		return NULL;
	}
	strcpy(rtn, input);

	return rtn;
}

static void
usage() {
	printf(_("RELEASE: %s GDAL_VERSION=%d (%s)\n"), POSTGIS_VERSION, POSTGIS_GDAL_VERSION, RCSID );
	printf(_(
		"USAGE: raster2pgsql [<options>] <raster>[ <raster>[ ...]] [[<schema>.]<table>]\n"
		"  Multiple rasters can also be specified using wildcards (*,?).\n"
		"\n"
		"OPTIONS:\n"
	));
	printf(_(
		"  -s <srid> Set the raster's SRID. Defaults to %d.\n"
	), SRID_UNKNOWN);
	printf(_(
		"  -b <band> Index (1-based) of band to extract from raster.  For more\n"
		"      than one band index, separate with comma (,).  If unspecified,\n"
		"      all bands of raster will be extracted.\n"
	));
	printf(_(
		"  -t <tile size> Cut raster into tiles to be inserted one per\n"
		"      table row.  <tile size> is expressed as WIDTHxHEIGHT.\n"
	));
	printf(_(
		"  -R  Register the raster as an out-of-db (filesystem) raster.  Provided\n"
		"      raster should have absolute path to the file\n"
	));
	printf(_(
		" (-d|a|c|p) These are mutually exclusive options:\n"
		"     -d  Drops the table, then recreates it and populates\n"
		"         it with current raster data.\n"
		"     -a  Appends raster into current table, must be\n"
		"         exactly the same table schema.\n"
		"     -c  Creates a new table and populates it, this is the\n"
		"         default if you do not specify any options.\n"
		"     -p  Prepare mode, only creates the table.\n"
	));
	printf(_(
		"  -f <column> Specify the name of the raster column\n"
	));
	printf(_(
		"  -F  Add a column with the filename of the raster.\n"
	));
	printf(_(
		"  -l <overview factor> Create overview of the raster.  For more than\n"
		"      one factor, separate with comma(,).  Overview table name follows\n" 
		"      the pattern o_<overview factor>_<table>.  Created overview is\n"
		"      stored in the database and is not affected by -R.\n"
	));
	printf(_(
		"  -q  Wrap PostgreSQL identifiers in quotes.\n"
	));
	printf(_(
		"  -I  Create a GIST spatial index on the raster column.  The ANALYZE\n"
		"      command will automatically be issued for the created index.\n"
	));
	printf(_(
		"  -M  Run VACUUM ANALYZE on the table of the raster column.  Most\n"
		"      useful when appending raster to existing table with -a.\n"
	));
	printf(_(
		"  -C  Set the standard set of constraints on the raster\n"
		"      column after the rasters are loaded.  Some constraints may fail\n"
		"      if one or more rasters violate the constraint.\n"
		"  -x  Disable setting the max extent constraint.  Only applied if\n"
		"      -C flag is also used.\n"
		"  -r  Set the regular blocking constraint.  Only applied if -C flag is\n"
		"      also used.\n"
	));
	printf(_(
		"  -T <tablespace> Specify the tablespace for the new table.\n"
		"      Note that indices (including the primary key) will still use\n"
		"      the default tablespace unless the -X flag is also used.\n"
	));
	printf(_(
		"  -X <tablespace> Specify the tablespace for the table's new index.\n"
		"      This applies to the primary key and the spatial index if\n"
		"      the -I flag is used.\n"
	));
	printf(_(
		"  -N <nodata> NODATA value to use on bands without a NODATA value.\n"
	));
	printf(_(
		"  -E <endian> Control endianness of generated binary output of\n"
		"      raster.  Use 0 for XDR and 1 for NDR (default).  Only NDR\n"
		"      is supported at this time.\n"
	));
	printf(_(
		"  -V <version> Specify version of output format.  Default\n"
		"      is 0.  Only 0 is supported at this time.\n"
	));
	printf(_(
		"  -e  Execute each statement individually, do not use a transaction.\n"
	));
	printf(_(
		"  -Y  Use COPY statements instead of INSERT statements.\n"
	));
	printf(_(
		"  -?  Display this help screen.\n"
	));
}

static void
init_rastinfo(RASTERINFO *info) {
	info->srs = NULL;
	memset(info->dim, 0, sizeof(double) * 2);
	info->nband_count = 0;
	info->nband = NULL;
	info->gdalbandtype = NULL;
	info->bandtype = NULL;
	info->hasnodata = NULL;
	info->nodataval = NULL;
	memset(info->gt, 0, sizeof(double) * 6);
	memset(info->tile_size, 0, sizeof(int) * 2);
}

static void
rtdealloc_rastinfo(RASTERINFO *info) {
	if (info->srs != NULL)
		rtdealloc(info->srs);
	if (info->nband_count > 0 && info->nband != NULL)
		rtdealloc(info->nband);
	if (info->gdalbandtype != NULL)
		rtdealloc(info->gdalbandtype);
	if (info->bandtype != NULL)
		rtdealloc(info->bandtype);
	if (info->hasnodata != NULL)
		rtdealloc(info->hasnodata);
	if (info->nodataval != NULL)
		rtdealloc(info->nodataval);
}

static void
init_config(RTLOADERCFG *config) {
	config->rt_file_count = 0;
	config->rt_file = NULL;
	config->rt_filename = NULL;
	config->schema = NULL;
	config->table = NULL;
	config->raster_column = NULL;
	config->file_column = 0;
	config->overview_count = 0;
	config->overview = NULL;
	config->overview_table = NULL;
	config->quoteident = 0;
	config->srid = SRID_UNKNOWN;
	config->nband = NULL;
	config->nband_count = 0;
	memset(config->tile_size, 0, sizeof(int) * 2);
	config->outdb = 0;
	config->opt = 'c';
	config->idx = 0;
	config->maintenance = 0;
	config->constraints = 0;
	config->max_extent = 1;
	config->regular_blocking = 0;
	config->tablespace = NULL;
	config->idx_tablespace = NULL;
	config->hasnodata = 0;
	config->nodataval = 0;
	config->endian = 1;
	config->version = 0;
	config->transaction = 1;
	config->copy_statements = 0;
}

static void
rtdealloc_config(RTLOADERCFG *config) {
	int i = 0;
	if (config->rt_file_count) {
		for (i = config->rt_file_count - 1; i >= 0; i--) {
			rtdealloc(config->rt_file[i]);
			if (config->rt_filename)
				rtdealloc(config->rt_filename[i]);
		}
		rtdealloc(config->rt_file);
		if (config->rt_filename)
			rtdealloc(config->rt_filename);
	}
	if (config->schema != NULL)
		rtdealloc(config->schema);
	if (config->table != NULL)
		rtdealloc(config->table);
	if (config->raster_column != NULL)
		rtdealloc(config->raster_column);
	if (config->overview_count > 0) {
		if (config->overview != NULL)
			rtdealloc(config->overview);
		if (config->overview_table != NULL) {
			for (i = config->overview_count - 1; i >= 0; i--)
				rtdealloc(config->overview_table[i]);
			rtdealloc(config->overview_table);
		}
	}
	if (config->nband_count > 0 && config->nband != NULL)
		rtdealloc(config->nband);
	if (config->tablespace != NULL)
		rtdealloc(config->tablespace);
	if (config->idx_tablespace != NULL)
		rtdealloc(config->idx_tablespace);

	rtdealloc(config);
}

static void
init_stringbuffer(STRINGBUFFER *buffer) {
	buffer->line = NULL;
	buffer->length = 0;
}

static void
rtdealloc_stringbuffer(STRINGBUFFER *buffer, int freebuffer) {
	if (buffer->length) {
		uint32_t i = 0;
		for (i = 0; i < buffer->length; i++) {
			if (buffer->line[i] != NULL)
				rtdealloc(buffer->line[i]);
		}
		rtdealloc(buffer->line);
	}
	buffer->line = NULL;
	buffer->length = 0;

	if (freebuffer)
		rtdealloc(buffer);
}

static void
dump_stringbuffer(STRINGBUFFER *buffer) {
	int i = 0;

	for (i = 0; i < buffer->length; i++) {
		printf("%s\n", buffer->line[i]);
	}
}

static void
flush_stringbuffer(STRINGBUFFER *buffer) {
	dump_stringbuffer(buffer);
	rtdealloc_stringbuffer(buffer, 0);
}

static int
append_stringbuffer(STRINGBUFFER *buffer, const char *str) {
	buffer->length++;

	buffer->line = rtrealloc(buffer->line, sizeof(char *) * buffer->length);
	if (buffer->line == NULL) {
		fprintf(stderr, _("Could not allocate memory for appending string to buffer\n"));
		return 0;
	}

	buffer->line[buffer->length - 1] = NULL;
	buffer->line[buffer->length - 1] = rtalloc(sizeof(char) * (strlen(str) + 1));
	if (buffer->line[buffer->length - 1] == NULL) {
		fprintf(stderr, _("Could not allocate memory for appending string to buffer\n"));
		return 0;
	}
	strcpy(buffer->line[buffer->length - 1], str);

	return 1;
}

static int
build_overviews(int idx, RTLOADERCFG *config, RASTERINFO *info, STRINGBUFFER *ovset) {
	GDALDatasetH hdsSrc;
	VRTDatasetH hdsOv;
	VRTSourcedRasterBandH hbandOv;
	double gtOv[6] = {0.};
	int dimOv[2] = {0};
	int factor = 0;

	int i = 0;
	int j = 0;

	VRTDatasetH hdsDst;
	VRTSourcedRasterBandH hbandDst;
	int tile_size[2] = {0};
	int ntiles[2] = {1, 1};
	int xtile = 0;
	int ytile = 0;
	double gt[6] = {0.};

	rt_raster rast = NULL;
	char *hex;
	uint32_t hexlen = 0;

	hdsSrc = GDALOpenShared(config->rt_file[idx], GA_ReadOnly);
	if (hdsSrc == NULL) {
		fprintf(stderr, _("Cannot open raster: %s\n"), config->rt_file[idx]);
		return 0;
	}

	/* working copy of geotransform matrix */
	memcpy(gtOv, info->gt, sizeof(double) * 6);

	/* loop over each overview factor */
	for (i = 0; i < config->overview_count; i++) {
		factor = config->overview[i];
		if (factor < 2) continue;

		dimOv[0] = (int) (info->dim[0] + (factor / 2)) / factor;
		dimOv[1] = (int) (info->dim[1] + (factor / 2)) / factor;

		/* create VRT dataset */
		hdsOv = VRTCreate(dimOv[0], dimOv[1]);
		/*
    GDALSetDescription(hdsOv, "/tmp/ov.vrt");
		*/
		GDALSetProjection(hdsOv, info->srs);

		/* adjust scale */
		gtOv[1] *= factor;
		gtOv[5] *= factor;

		GDALSetGeoTransform(hdsOv, gtOv);

		/* add bands as simple sources */
		for (j = 0; j < info->nband_count; j++) {
			GDALAddBand(hdsOv, info->gdalbandtype[j], NULL);
			hbandOv = (VRTSourcedRasterBandH) GDALGetRasterBand(hdsOv, j + 1);

			if (info->hasnodata[j])
				GDALSetRasterNoDataValue(hbandOv, info->nodataval[j]);

			VRTAddSimpleSource(
				hbandOv, GDALGetRasterBand(hdsSrc, info->nband[j]),
				0, 0,
				info->dim[0], info->dim[1],
				0, 0,
				dimOv[0], dimOv[1],
				"near", VRT_NODATA_UNSET
			);
		}

		/* make sure VRT reflects all changes */
		VRTFlushCache(hdsOv);

		/* decide on tile size */
		if (!config->tile_size[0])
			tile_size[0] = dimOv[0];
		else
			tile_size[0] = config->tile_size[0];
		if (!config->tile_size[1])
			tile_size[1] = dimOv[1];
		else
			tile_size[1] = config->tile_size[1];

		/* number of tiles */
		if (
			tile_size[0] != dimOv[0] &&
			tile_size[1] != dimOv[1]
		) {
			ntiles[0] = (dimOv[0] + tile_size[0] -  1) / tile_size[0];
			ntiles[1] = (dimOv[1] + tile_size[1]  - 1) / tile_size[1];
		}

		/* working copy of geotransform matrix */
		memcpy(gt, gtOv, sizeof(double) * 6);

		/* tile overview */
		/* each tile is a VRT with constraints set for just the data required for the tile */
		for (ytile = 0; ytile < ntiles[1]; ytile++) {
			for (xtile = 0; xtile < ntiles[0]; xtile++) {
				/*
				char fn[100];
				sprintf(fn, "/tmp/tile%d.vrt", (ytile * ntiles[0]) + xtile);
				*/

				/* compute tile's upper-left corner */
				GDALApplyGeoTransform(
					gtOv,
					xtile * tile_size[0], ytile * tile_size[1],
					&(gt[0]), &(gt[3])
				);

				/* create VRT dataset */
				hdsDst = VRTCreate(tile_size[0], tile_size[1]);
				/*
    		GDALSetDescription(hdsDst, fn);
				*/
				GDALSetProjection(hdsDst, info->srs);
				GDALSetGeoTransform(hdsDst, gt);

				/* add bands as simple sources */
				for (j = 0; j < info->nband_count; j++) {
					GDALAddBand(hdsDst, info->gdalbandtype[j], NULL);
					hbandDst = (VRTSourcedRasterBandH) GDALGetRasterBand(hdsDst, j + 1);

					if (info->hasnodata[j])
						GDALSetRasterNoDataValue(hbandDst, info->nodataval[j]);

					VRTAddSimpleSource(
						hbandDst, GDALGetRasterBand(hdsOv, j + 1),
						xtile * tile_size[0], ytile * tile_size[1],
						tile_size[0], tile_size[1],
						0, 0,
						tile_size[0], tile_size[1],
						"near", VRT_NODATA_UNSET
					);
				}

				/* make sure VRT reflects all changes */
				VRTFlushCache(hdsDst);

				/* convert VRT dataset to rt_raster */
				rast = rt_raster_from_gdal_dataset(hdsDst);

				/* set srid if provided */
				rt_raster_set_srid(rast, config->srid);

				/* convert rt_raster to hexwkb */
				hex = rt_raster_to_hexwkb(rast, &hexlen);
				raster_destroy(rast);

				/* add hexwkb to tileset */
				append_stringbuffer(&(ovset[i]), hex);

				rtdealloc(hex);
				GDALClose(hdsDst);
			}
		}

		GDALClose(hdsOv);
	}

	GDALClose(hdsSrc);
	return 1;
}

static int
convert_raster(int idx, RTLOADERCFG *config, RASTERINFO *info, STRINGBUFFER *tileset) {
	GDALDatasetH hdsSrc;
	GDALRasterBandH hbandSrc;
	int nband = 0;
	int i = 0;
	int ntiles[2] = {1, 1};
	int xtile = 0;
	int ytile = 0;
	double gt[6] = {0.};

	rt_raster rast = NULL;
	char *hex;
	uint32_t hexlen = 0;

	hdsSrc = GDALOpenShared(config->rt_file[idx], GA_ReadOnly);
	if (hdsSrc == NULL) {
		fprintf(stderr, _("Cannot open raster: %s\n"), config->rt_file[idx]);
		return 0;
	}

	nband = GDALGetRasterCount(hdsSrc);
	if (!nband) {
		fprintf(stderr, _("No bands found in raster: %s\n"), config->rt_file[idx]);
		GDALClose(hdsSrc);
		return 0;
	}

	/* check that bands specified are available */
	for (i = 0; i < config->nband_count; i++) {
		if (config->nband[i] > nband) {
			fprintf(stderr, _("Band %d not found in raster: %s\n"), config->nband[i], config->rt_file[idx]);
			GDALClose(hdsSrc);
			return 0;
		}
	}

	/* record srs */
	if (GDALGetProjectionRef(hdsSrc) != NULL) {
		info->srs = rtalloc(sizeof(char) * (strlen(GDALGetProjectionRef(hdsSrc)) + 1));
		if (info->srs == NULL) {
			fprintf(stderr, _("Could not allocate memory for storing SRS\n"));
			GDALClose(hdsSrc);
			return 0;
		}
		strcpy(info->srs, GDALGetProjectionRef(hdsSrc));
	}

	/* record geotransform matrix */
	if (GDALGetGeoTransform(hdsSrc, info->gt) != CE_None) {
		fprintf(stderr, _("Cannot get geotransform matrix from raster: %s\n"), config->rt_file[idx]);
		GDALClose(hdsSrc);
		return 0;
	}
	memcpy(gt, info->gt, sizeof(double) * 6);

	/* record # of bands */
	/* user-specified bands */
	if (config->nband_count > 0) {
		info->nband_count = config->nband_count;
		info->nband = rtalloc(sizeof(int) * info->nband_count);
		if (info->nband == NULL) {
			fprintf(stderr, _("Could not allocate memory for storing band indices\n"));
			GDALClose(hdsSrc);
			return 0;
		}
		memcpy(info->nband, config->nband, sizeof(int) * info->nband_count);
	}
	/* all bands */
	else {
		info->nband_count = nband;
		info->nband = rtalloc(sizeof(int) * info->nband_count);
		if (info->nband == NULL) {
			fprintf(stderr, _("Could not allocate memory for storing band indices\n"));
			GDALClose(hdsSrc);
			return 0;
		}
		for (i = 0; i < info->nband_count; i++)
			info->nband[i] = i + 1;
	}

	/* initialize parameters dependent on nband */
	info->gdalbandtype = rtalloc(sizeof(GDALDataType) * info->nband_count);
	if (info->gdalbandtype == NULL) {
		fprintf(stderr, _("Could not allocate memory for storing GDAL data type\n"));
		GDALClose(hdsSrc);
		return 0;
	}
	info->bandtype = rtalloc(sizeof(rt_pixtype) * info->nband_count);
	if (info->bandtype == NULL) {
		fprintf(stderr, _("Could not allocate memory for storing pixel type\n"));
		GDALClose(hdsSrc);
		return 0;
	}
	info->hasnodata = rtalloc(sizeof(int) * info->nband_count);
	if (info->hasnodata == NULL) {
		fprintf(stderr, _("Could not allocate memory for storing hasnodata flag\n"));
		GDALClose(hdsSrc);
		return 0;
	}
	info->nodataval = rtalloc(sizeof(double) * info->nband_count);
	if (info->nodataval == NULL) {
		fprintf(stderr, _("Could not allocate memory for storing nodata value\n"));
		GDALClose(hdsSrc);
		return 0;
	}
	memset(info->gdalbandtype, GDT_Unknown, sizeof(GDALDataType) * info->nband_count);
	memset(info->bandtype, PT_END, sizeof(rt_pixtype) * info->nband_count);
	memset(info->hasnodata, 0, sizeof(int) * info->nband_count);
	memset(info->nodataval, 0, sizeof(double) * info->nband_count);

	/* dimensions of raster */
	info->dim[0] = GDALGetRasterXSize(hdsSrc);
	info->dim[1] = GDALGetRasterYSize(hdsSrc);

	/* decide on tile size */
	if (!config->tile_size[0])
		info->tile_size[0] = info->dim[0];
	else
		info->tile_size[0] = config->tile_size[0];
	if (!config->tile_size[1])
		info->tile_size[1] = info->dim[1];
	else
		info->tile_size[1] = config->tile_size[1];

	/* number of tiles */
	if (
		info->tile_size[0] != info->dim[0] &&
		info->tile_size[1] != info->dim[1]
	) {
		ntiles[0] = (info->dim[0] + info->tile_size[0]  - 1) / info->tile_size[0];
		ntiles[1] = (info->dim[1] + info->tile_size[1]  - 1) / info->tile_size[1];
	}

	/* go through bands for attributes */
	for (i = 0; i < info->nband_count; i++) {
		hbandSrc = GDALGetRasterBand(hdsSrc, info->nband[i]);

		/* datatype */
		info->gdalbandtype[i] = GDALGetRasterDataType(hbandSrc);

		/* complex data type? */
		if (GDALDataTypeIsComplex(info->gdalbandtype[i])) {
			fprintf(stderr, _("The pixel type of band %d is a complex data type.  PostGIS Raster does not support complex data types\n"), i + 1);
			GDALClose(hdsSrc);
			return 0;
		}

		/* convert data type to that of postgis raster */
		info->bandtype[i] = rt_util_gdal_datatype_to_pixtype(info->gdalbandtype[i]);

		/* hasnodata and nodataval */
		info->nodataval[i] = GDALGetRasterNoDataValue(hbandSrc, &(info->hasnodata[i]));
		if (!info->hasnodata[i]) {
			/* does NOT have nodata value, but user-specified */
			if (config->hasnodata) {
				info->hasnodata[i] = 1;
				info->nodataval[i] = config->nodataval;
			}
			else
				info->nodataval[i] = 0;
		}
	}

	/* out-db raster */
	if (config->outdb) {
		rt_band band = NULL;

		GDALClose(hdsSrc);

		/* each tile is a raster */
		for (ytile = 0; ytile < ntiles[1]; ytile++) {
			for (xtile = 0; xtile < ntiles[0]; xtile++) {
				
				/* compute tile's upper-left corner */
				GDALApplyGeoTransform(
					info->gt,
					xtile * info->tile_size[0], ytile * info->tile_size[1],
					&(gt[0]), &(gt[3])
				);

				/* create raster object */
				rast = rt_raster_new(info->tile_size[0], info->tile_size[1]);
				if (rast == NULL) {
					fprintf(stderr, _("Could not create raster\n"));
					return 0;
				}

				/* set raster attributes */
				rt_raster_set_srid(rast, config->srid);
				rt_raster_set_geotransform_matrix(rast, gt);

				/* add bands */
				for (i = 0; i < info->nband_count; i++) {
					band = rt_band_new_offline(
						info->tile_size[0], info->tile_size[1],
						info->bandtype[i],
						info->hasnodata[i], info->nodataval[i],
						info->nband[i] - 1,
						config->rt_file[idx]
					);
					if (band == NULL) {
						fprintf(stderr, _("Could not create offline band\n"));
						raster_destroy(rast);
						return 0;
					}

					/* add band to raster */
					if (rt_raster_add_band(rast, band, rt_raster_get_num_bands(rast)) == -1) {
						fprintf(stderr, _("Could not add offlineband to raster\n"));
						rt_band_destroy(band);
						raster_destroy(rast);
						return 0;
					}
				}

				/* convert rt_raster to hexwkb */
				hex = rt_raster_to_hexwkb(rast, &hexlen);
				raster_destroy(rast);

				/* add hexwkb to tileset */
				append_stringbuffer(tileset, hex);

				rtdealloc(hex);
			}
		}
	}
	/* in-db raster */
	else {
		VRTDatasetH hdsDst;
		VRTSourcedRasterBandH hbandDst;

		/* each tile is a VRT with constraints set for just the data required for the tile */
		for (ytile = 0; ytile < ntiles[1]; ytile++) {
			for (xtile = 0; xtile < ntiles[0]; xtile++) {

				/* compute tile's upper-left corner */
				GDALApplyGeoTransform(
					info->gt,
					xtile * info->tile_size[0], ytile * info->tile_size[1],
					&(gt[0]), &(gt[3])
				);

				/* create VRT dataset */
				hdsDst = VRTCreate(info->tile_size[0], info->tile_size[1]);
				GDALSetProjection(hdsDst, info->srs);
				GDALSetGeoTransform(hdsDst, gt);

				/* add bands as simple sources */
				for (i = 0; i < info->nband_count; i++) {
					GDALAddBand(hdsDst, info->gdalbandtype[i], NULL);
					hbandDst = (VRTSourcedRasterBandH) GDALGetRasterBand(hdsDst, i + 1);

					if (info->hasnodata[i])
						GDALSetRasterNoDataValue(hbandDst, info->nodataval[i]);

					VRTAddSimpleSource(
						hbandDst, GDALGetRasterBand(hdsSrc, info->nband[i]),
						xtile * info->tile_size[0], ytile * info->tile_size[1],
						info->tile_size[0], info->tile_size[1],
						0, 0,
						info->tile_size[0], info->tile_size[1],
						"near", VRT_NODATA_UNSET
					);
				}

				/* make sure VRT reflects all changes */
				VRTFlushCache(hdsDst);

				/* convert VRT dataset to rt_raster */
				rast = rt_raster_from_gdal_dataset(hdsDst);

				/* set srid if provided */
				rt_raster_set_srid(rast, config->srid);

				/* convert rt_raster to hexwkb */
				hex = rt_raster_to_hexwkb(rast, &hexlen);
				raster_destroy(rast);

				/* add hexwkb to tileset */
				append_stringbuffer(tileset, hex);

				rtdealloc(hex);
				GDALClose(hdsDst);
			}
		}

		GDALClose(hdsSrc);
	}

	return 1;
}

static int
insert_records(
	const char *schema, const char *table, const char *column,
	const char *filename, int copy_statements,
	STRINGBUFFER *tileset, STRINGBUFFER *buffer
) {
	char *fn = NULL;
	uint32_t len = 0;
	char *sql = NULL;
	uint32_t x = 0;

	assert(table != NULL);
	assert(column != NULL);

	append_stringbuffer(buffer, "");

	/* COPY statements */
	if (copy_statements) {

		/* COPY */
		len = strlen("COPY  () FROM stdin;") + 1;
		if (schema != NULL)
			len += strlen(schema);
		len += strlen(table);
		len += strlen(column);
		if (filename != NULL)
			len += strlen(",\"filename\"");

		sql = rtalloc(sizeof(char) * len);
		if (sql == NULL) {
			fprintf(stderr, _("Could not allocate memory for COPY statement\n"));
			return 0;
		}
		sprintf(sql, "COPY %s%s (%s%s) FROM stdin;",
			(schema != NULL ? schema : ""),
			table,
			column,
			(filename != NULL ? ",\"filename\"" : "")
		);

		append_stringbuffer(buffer, sql);
		rtdealloc(sql);
		sql = NULL;

		/* escape tabs in filename */
		if (filename != NULL)
			fn = strreplace(filename, "\t", "\\t", NULL);

		/* rows */
		for (x = 0; x < tileset->length; x++) {
			len = strlen(tileset->line[x]) + 1;

			if (filename != NULL)
				len += strlen(fn) + 1;

			sql = rtalloc(sizeof(char) * len);
			if (sql == NULL) {
				fprintf(stderr, _("Could not allocate memory for COPY statement\n"));
				return 0;
			}
			sprintf(sql, "%s%s%s",
				tileset->line[x],
				(filename != NULL ? "\t" : ""),
				(filename != NULL ? fn : "")
			);

			append_stringbuffer(buffer, sql);
			rtdealloc(sql);
			sql = NULL;
		}

		/* end of data */
		append_stringbuffer(buffer, "\\.");
	}
	/* INSERT statements */
	else {
		len = strlen("INSERT INTO  () VALUES (''::raster);") + 1;
		if (schema != NULL)
			len += strlen(schema);
		len += strlen(table);
		len += strlen(column);
		if (filename != NULL)
			len += strlen(",\"filename\"");

		/* escape single-quotes in filename */
		if (filename != NULL)
			fn = strreplace(filename, "'", "''", NULL);

		for (x = 0; x < tileset->length; x++) {
			int sqllen = len;

			sqllen += strlen(tileset->line[x]);
			if (filename != NULL)
				sqllen += strlen(",''") + strlen(fn);

			sql = rtalloc(sizeof(char) * sqllen);
			if (sql == NULL) {
				fprintf(stderr, _("Could not allocate memory for INSERT statement\n"));
				return 0;
			}
			sprintf(sql, "INSERT INTO %s%s (%s%s) VALUES ('%s'::raster%s%s%s);",
				(schema != NULL ? schema : ""),
				table,
				column,
				(filename != NULL ? ",\"filename\"" : ""),
				tileset->line[x],
				(filename != NULL ? ",'" : ""),
				(filename != NULL ? fn : ""),
				(filename != NULL ? "'" : "")
			);

			append_stringbuffer(buffer, sql);
			rtdealloc(sql);
			sql = NULL;
		}
	}

	append_stringbuffer(buffer, "");

	if (fn != NULL) rtdealloc(fn);
	return 1;
}

static int
drop_table(const char *schema, const char *table, STRINGBUFFER *buffer) {
	char *sql = NULL;
	uint32_t len = 0;

	len = strlen("DROP TABLE IF EXISTS ;") + 1;
	if (schema != NULL)
		len += strlen(schema);
	len += strlen(table);

	sql = rtalloc(sizeof(char) * len);
	if (sql == NULL) {
		fprintf(stderr, _("Could not allocate memory for DROP TABLE statement\n"));
		return 0;
	}
	sprintf(sql, "DROP TABLE IF EXISTS %s%s;",
		(schema != NULL ? schema : ""),
		table
	);

	append_stringbuffer(buffer, sql);
	rtdealloc(sql);

	return 1;
}

static int
create_table(
	const char *schema, const char *table, const char *column,
	const int file_column,
	const char *tablespace, const char *idx_tablespace,
	STRINGBUFFER *buffer
) {
	char *sql = NULL;
	uint32_t len = 0;

	assert(table != NULL);
	assert(column != NULL);

	len = strlen("CREATE TABLE  (\"rid\" serial PRIMARY KEY, raster);") + 1;
	if (schema != NULL)
		len += strlen(schema);
	len += strlen(table);
	len += strlen(column);
	if (file_column)
		len += strlen(",\"filename\" text");
	if (tablespace != NULL)
		len += strlen(" TABLESPACE ") + strlen(tablespace);
	if (idx_tablespace != NULL)
		len += strlen(" USING INDEX TABLESPACE ") + strlen(idx_tablespace);

	sql = rtalloc(sizeof(char) * len);
	if (sql == NULL) {
		fprintf(stderr, _("Could not allocate memory for CREATE TABLE statement\n"));
		return 0;
	}
	sprintf(sql, "CREATE TABLE %s%s (\"rid\" serial PRIMARY KEY,%s raster%s)%s%s%s%s;",
		(schema != NULL ? schema : ""),
		table,
		column,
		(file_column ? ",\"filename\" text" : ""),
		(tablespace != NULL ? " TABLESPACE " : ""),
		(tablespace != NULL ? tablespace : ""),
		(idx_tablespace != NULL ? " USING INDEX TABLESPACE " : ""),
		(idx_tablespace != NULL ? idx_tablespace : "")
	);

	append_stringbuffer(buffer, sql);
	rtdealloc(sql);

	return 1;
}

static int
create_index(
	const char *schema, const char *table, const char *column,
	const char *tablespace,
	STRINGBUFFER *buffer
) {
	char *sql = NULL;
	uint32_t len = 0;

	assert(table != NULL);
	assert(column != NULL);

	/* create index */
	len = strlen("CREATE INDEX ON  USING gist (st_convexhull());") + 1;
	if (schema != NULL)
		len += strlen(schema);
	len += strlen(table);
	len += strlen(column);
	if (tablespace != NULL)
		len += strlen(" TABLESPACE ") + strlen(tablespace);

	sql = rtalloc(sizeof(char) * len);
	if (sql == NULL) {
		fprintf(stderr, _("Could not allocate memory for CREATE INDEX statement\n"));
		return 0;
	}
	sprintf(sql, "CREATE INDEX ON %s%s USING gist (st_convexhull(%s))%s%s;",
		(schema != NULL ? schema : ""),
		table,
		column,
		(tablespace != NULL ? " TABLESPACE " : ""),
		(tablespace != NULL ? tablespace : "")
	);

	append_stringbuffer(buffer, sql);
	rtdealloc(sql);

	return 1;
}

static int
analyze_table(
	const char *schema, const char *table,
	STRINGBUFFER *buffer
) {
	char *sql = NULL;
	uint32_t len = 0;

	assert(table != NULL);

	len = strlen("ANALYZE ;") + 1;
	if (schema != NULL)
		len += strlen(schema);
	len += strlen(table);

	sql = rtalloc(sizeof(char) * len);
	if (sql == NULL) {
		fprintf(stderr, _("Could not allocate memory for ANALYZE TABLE statement\n"));
		return 0;
	}
	sprintf(sql, "ANALYZE %s%s;",
		(schema != NULL ? schema : ""),
		table
	);

	append_stringbuffer(buffer, sql);
	rtdealloc(sql);

	return 1;
}

static int
vacuum_table(
	const char *schema, const char *table,
	STRINGBUFFER *buffer
) {
	char *sql = NULL;
	uint32_t len = 0;

	assert(table != NULL);

	len = strlen("VACUUM ANALYZE ;") + 1;
	if (schema != NULL)
		len += strlen(schema);
	len += strlen(table);

	sql = rtalloc(sizeof(char) * len);
	if (sql == NULL) {
		fprintf(stderr, _("Could not allocate memory for VACUUM statement\n"));
		return 0;
	}
	sprintf(sql, "VACUUM ANALYZE %s%s;",
		(schema != NULL ? schema : ""),
		table
	);

	append_stringbuffer(buffer, sql);
	rtdealloc(sql);

	return 1;
}

static int
add_raster_constraints(
	const char *schema, const char *table, const char *column,
	int regular_blocking, int max_extent,
	STRINGBUFFER *buffer
) {
	char *sql = NULL;
	uint32_t len = 0;

	char *_schema = NULL;
	char *_table = NULL;
	char *_column = NULL;

	assert(table != NULL);
	assert(column != NULL);

	if (schema != NULL) {
		char *tmp = chartrim(schema, ".");
		_schema = chartrim(tmp, "\"");
		rtdealloc(tmp);
	}
	_table = chartrim(table, "\"");
	_column = chartrim(column, "\"");

	len = strlen("SELECT AddRasterConstraints('','','',TRUE,TRUE,TRUE,TRUE,TRUE,TRUE,FALSE,TRUE,TRUE,TRUE,FALSE);") + 1;
	if (_schema != NULL)
		len += strlen(_schema);
	len += strlen(_table);
	len += strlen(_column);

	sql = rtalloc(sizeof(char) * len);
	if (sql == NULL) {
		fprintf(stderr, _("Could not allocate memory for AddRasterConstraints statement\n"));
		return 0;
	}
	sprintf(sql, "SELECT AddRasterConstraints('%s','%s','%s',TRUE,TRUE,TRUE,TRUE,TRUE,TRUE,%s,TRUE,TRUE,TRUE,%s);",
		(_schema != NULL ? _schema : ""),
		_table,
		_column,
		(regular_blocking ? "TRUE" : "FALSE"),
		(max_extent ? "TRUE" : "FALSE")
	);
	
	if (_schema != NULL)
		rtdealloc(_schema);
	rtdealloc(_table);
	rtdealloc(_column);

	append_stringbuffer(buffer, sql);
	rtdealloc(sql);

	return 1;
}

static int
add_overview_constraints(
	const char *ovschema, const char *ovtable, const char *ovcolumn,
	const char *schema, const char *table, const char *column,
	const int factor,
	STRINGBUFFER *buffer
) {
	char *sql = NULL;
	uint32_t len = 0;

	char *_ovschema = NULL;
	char *_ovtable = NULL;
	char *_ovcolumn = NULL;

	char *_schema = NULL;
	char *_table = NULL;
	char *_column = NULL;

	assert(ovtable != NULL);
	assert(ovcolumn != NULL);
	assert(table != NULL);
	assert(column != NULL);
	assert(factor >= MINOVFACTOR && factor <= MAXOVFACTOR);

	if (ovschema != NULL) {
		char *tmp = chartrim(ovschema, ".");
		_ovschema = chartrim(tmp, "\"");
		rtdealloc(tmp);
	}
	_ovtable = chartrim(ovtable, "\"");
	_ovcolumn = chartrim(ovcolumn, "\"");

	if (schema != NULL) {
		char *tmp = chartrim(schema, ".");
		_schema = chartrim(tmp, "\"");
		rtdealloc(tmp);
	}
	_table = chartrim(table, "\"");
	_column = chartrim(column, "\"");

	len = strlen("SELECT AddOverviewConstraints('','','','','','',);") + 5;
	if (_ovschema != NULL)
		len += strlen(_ovschema);
	len += strlen(_ovtable);
	len += strlen(_ovcolumn);
	if (_schema != NULL)
		len += strlen(_schema);
	len += strlen(_table);
	len += strlen(_column);

	sql = rtalloc(sizeof(char) * len);
	if (sql == NULL) {
		fprintf(stderr, _("Could not allocate memory for AddOverviewConstraints statement\n"));
		return 0;
	}
	sprintf(sql, "SELECT AddOverviewConstraints('%s','%s','%s','%s','%s','%s',%d);",
		(_ovschema != NULL ? _ovschema : ""),
		_ovtable,
		_ovcolumn,
		(_schema != NULL ? _schema : ""),
		_table,
		_column,
		factor
	);
	
	if (_ovschema != NULL)
		rtdealloc(_ovschema);
	rtdealloc(_ovtable);
	rtdealloc(_ovcolumn);

	if (_schema != NULL)
		rtdealloc(_schema);
	rtdealloc(_table);
	rtdealloc(_column);

	append_stringbuffer(buffer, sql);
	rtdealloc(sql);

	return 1;
}

static int
process_rasters(RTLOADERCFG *config, STRINGBUFFER *buffer) {
	int i = 0;

	assert(config != NULL);
	assert(config->table != NULL);
	assert(config->raster_column != NULL);

	if (config->transaction) {
		if (!append_stringbuffer(buffer, "BEGIN;")) {
			fprintf(stderr, _("Cannot add BEGIN statement to string buffer\n"));
			return 0;
		}
	}

	/* drop table */
	if (config->opt == 'd') {
		if (!drop_table(config->schema, config->table, buffer)) {
			fprintf(stderr, _("Cannot add DROP TABLE statement to string buffer\n"));
			return 0;
		}

		if (config->overview_count) {
			for (i = 0; i < config->overview_count; i++) {
				if (!drop_table(config->schema, config->overview_table[i], buffer)) {
					fprintf(stderr, _("Cannot add an overview's DROP TABLE statement to string buffer\n"));
					return 0;
				}
			}
		}
	}

	/* create table */
	if (config->opt != 'a') {
		if (!create_table(
			config->schema, config->table, config->raster_column,
			config->file_column,
			config->tablespace, config->idx_tablespace,
			buffer
		)) {
			fprintf(stderr, _("Cannot add CREATE TABLE statement to string buffer\n"));
			return 0;
		}

		if (config->overview_count) {
			for (i = 0; i < config->overview_count; i++) {
				if (!create_table(
					config->schema, config->overview_table[i], config->raster_column,
					0,
					config->tablespace, config->idx_tablespace,
					buffer
				)) {
					fprintf(stderr, _("Cannot add an overview's CREATE TABLE statement to string buffer\n"));
					return 0;
				}
			}
		}
	}

	/* no need to run if opt is 'p' */
	if (config->opt != 'p') {
		/* register GDAL drivers */
		GDALAllRegister();

		/* process each raster */
		for (i = 0; i < config->rt_file_count; i++) {
			RASTERINFO rastinfo;
			STRINGBUFFER tileset;

			fprintf(stderr, _("Processing %d/%d: %s\n"), i + 1, config->rt_file_count, config->rt_file[i]);

			init_rastinfo(&rastinfo);
			init_stringbuffer(&tileset);

			/* convert raster */
			if (!convert_raster(i, config, &rastinfo, &tileset)) {
				fprintf(stderr, _("Cannot process raster %s\n"), config->rt_file[i]);
				rtdealloc_rastinfo(&rastinfo);
				rtdealloc_stringbuffer(&tileset, 0);
				return 0;
			}

			/* process raster tiles into COPY or INSERT statements */
			if (!insert_records(
				config->schema, config->table, config->raster_column,
				(config->file_column ? config->rt_filename[i] : NULL), config->copy_statements,
				&tileset, buffer
			)) {
				fprintf(stderr, _("Cannot convert raster tiles into INSERT or COPY statements\n"));
				rtdealloc_rastinfo(&rastinfo);
				rtdealloc_stringbuffer(&tileset, 0);
				return 0;
			}

			rtdealloc_stringbuffer(&tileset, 0);

			/* flush buffer after every raster */
			flush_stringbuffer(buffer);

			/* overviews */
			if (config->overview_count) {
				int j = 0;
				STRINGBUFFER *ovset = NULL;

				/* build appropriate # of ovset */
				ovset = (STRINGBUFFER *) rtalloc(sizeof(struct stringbuffer_t) * config->overview_count);
				if (ovset == NULL) {
					fprintf(stderr, _("Cannot allocate memory for overview tiles\n"));
					rtdealloc_rastinfo(&rastinfo);
					return 0;
				}
				for (j = 0; j < config->overview_count; j++)
					init_stringbuffer(&(ovset[j]));

				if (!build_overviews(i, config, &rastinfo, ovset)) {
					fprintf(stderr, _("Cannot create overviews for raster %s\n"), config->rt_file[i]);

					for (j = 0; j < config->overview_count; j++)
						rtdealloc_stringbuffer(&(ovset[j]), 0);
					rtdealloc(ovset);

					rtdealloc_rastinfo(&rastinfo);
					return 0;
				}

				/* process overview tiles */
				for (j = 0; j < config->overview_count; j++) {
					if (!insert_records(
						config->schema, config->overview_table[j], config->raster_column,
						NULL, config->copy_statements,
						&(ovset[j]), buffer
					)) {
						fprintf(stderr, _("Cannot convert overview tiles into INSERT or COPY statements\n"));

						for (j = 0; j < config->overview_count; j++)
							rtdealloc_stringbuffer(&(ovset[j]), 0);
						rtdealloc(ovset);

						rtdealloc_rastinfo(&rastinfo);
						return 0;
					}

					/* flush buffer after every raster */
					flush_stringbuffer(buffer);
				}

				/* free ovset */
				for (j = 0; j < config->overview_count; j++)
					rtdealloc_stringbuffer(&(ovset[j]), 0);
				rtdealloc(ovset);
			}

			rtdealloc_rastinfo(&rastinfo);
		}
	}

	/* index */
	if (config->idx) {
		/* create index */
		if (!create_index(
			config->schema, config->table, config->raster_column,
			config->idx_tablespace,
			buffer
		)) {
			fprintf(stderr, _("Cannot add CREATE INDEX statement to string buffer\n"));
			return 0;
		}

		/* analyze */
		if (config->opt != 'p') {
			if (!analyze_table(
				config->schema, config->table,
				buffer
			)) {
				fprintf(stderr, _("Cannot add ANALYZE statement to string buffer\n"));
				return 0;
			}
		}

		if (config->overview_count) {
			for (i = 0; i < config->overview_count; i++) {
				/* create index */
				if (!create_index(
					config->schema, config->overview_table[i], config->raster_column,
					config->idx_tablespace,
					buffer
				)) {
					fprintf(stderr, _("Cannot add an overview's CREATE INDEX statement to string buffer\n"));
					return 0;
				}

				/* analyze */
				if (config->opt != 'p') {
					if (!analyze_table(
						config->schema, config->overview_table[i],
						buffer
					)) {
						fprintf(stderr, _("Cannot add an overview's ANALYZE statement to string buffer\n"));
						return 0;
					}
				}
			}
		}
	}

	/* add constraints */
	if (config->constraints) {
		if (!add_raster_constraints(
			config->schema, config->table, config->raster_column,
			config->regular_blocking, config->max_extent,
			buffer
		)) {
			fprintf(stderr, _("Cannot add AddRasterConstraints statement to string buffer\n"));
			return 0;
		}

		if (config->overview_count) {
			for (i = 0; i < config->overview_count; i++) {
				if (!add_raster_constraints(
					config->schema, config->overview_table[i], config->raster_column,
					config->regular_blocking, config->max_extent,
					buffer
				)) {
					fprintf(stderr, _("Cannot add an overview's AddRasterConstraints statement to string buffer\n"));
					return 0;
				}

				if (!add_overview_constraints(
					config->schema, config->overview_table[i], config->raster_column,
					config->schema, config->table, config->raster_column,
					config->overview[i],
					buffer
				)) {
					fprintf(stderr, _("Cannot add an overview's AddOverviewConstraints statement to string buffer\n"));
					return 0;
				}
			}
		}
	}

	if (config->transaction) {
		if (!append_stringbuffer(buffer, "END;")) {
			fprintf(stderr, _("Cannot add END statement to string buffer\n"));
			return 0;
		}
	}

	/* maintenance */
	if (config->opt != 'p' && config->maintenance) {
		if (!vacuum_table(
			config->schema, config->table,
			buffer
		)) {
			fprintf(stderr, _("Cannot add VACUUM statement to string buffer\n"));
			return 0;
		}

		if (config->overview_count) {
			for (i = 0; i < config->overview_count; i++) {
				if (!vacuum_table(
					config->schema, config->overview_table[i],
					buffer
				)) {
					fprintf(stderr, _("Cannot add an overview's VACUUM statement to string buffer\n"));
					return 0;
				}
			}
		}

	}

	return 1;
}

int
main(int argc, char **argv) {
	RTLOADERCFG *config = NULL;
	STRINGBUFFER *buffer = NULL;
	int i = 0;
	int j = 0;
	char **elements = NULL;
	int n = 0;
	FILE *fp = NULL;
	char *tmp = NULL;

#ifdef USE_NLS
	setlocale (LC_ALL, "");
	bindtextdomain (PACKAGE, LOCALEDIR);
	textdomain (PACKAGE);
#endif

	/* no args, show usage */
	if (argc == 1) {
		usage();
		exit(0);
	}

	/* initialize config */
	config = rtalloc(sizeof(RTLOADERCFG));
	if (config == NULL) {
		fprintf(stderr, _("Could not allocate memory for loader configuration\n"));
		exit(1);
	}
	init_config(config);

	/****************************************************************************
	* parse arguments
	****************************************************************************/

	for (i = 1; i < argc; i++) {
		/* srid */
		if (CSEQUAL(argv[i], "-s") && i < argc - 1) {
			config->srid = atoi(argv[++i]);
		}
		/* band index */
		else if (CSEQUAL(argv[i], "-b") && i < argc - 1) {
			elements = strsplit(argv[++i], ",", &n);
			if (n < 1) {
				fprintf(stderr, _("Cannot process -b.\n"));
				rtdealloc_config(config);
				exit(1);
			}

			config->nband_count = n;
			config->nband = rtalloc(sizeof(int) * n);
			if (config->nband == NULL) {
				fprintf(stderr, _("Could not allocate memory for storing band indices\n"));
				rtdealloc_config(config);
				exit(1);
			}
			for (j = 0; j < n; j++) {
				char *t = trim(elements[j]);
				config->nband[j] = atoi(t);
				rtdealloc(t);
				rtdealloc(elements[j]);
			}
			rtdealloc(elements);
			elements = NULL;
			n = 0;

			for (j = 0; j < config->nband_count; j++) {
				if (config->nband[j] < 1) {
					fprintf(stderr, _("Band index %d must be greater than 0.\n"), config->nband[j]);
					rtdealloc_config(config);
					exit(1);
				}
			}
		}
		/* tile size */
		else if (CSEQUAL(argv[i], "-t") && i < argc - 1) {
			elements = strsplit(argv[++i], "x", &n);
			if (n != 2) {
				fprintf(stderr, _("Cannot process -t.\n"));
				rtdealloc_config(config);
				exit(1);
			}

			for (j = 0; j < n; j++) {
				char *t = trim(elements[j]);
				config->tile_size[j] = atoi(t);
				rtdealloc(t);
				rtdealloc(elements[j]);
			}
			rtdealloc(elements);
			elements = NULL;
			n = 0;

			for (j = 0; j < 2; j++) {
				if (config->tile_size[j] < 1) {
					fprintf(stderr, _("Tile size must be greater than 0x0.\n"));
					rtdealloc_config(config);
					exit(1);
				}
			}

		}
		/* out-of-db raster */
		else if (CSEQUAL(argv[i], "-R")) {
			config->outdb = 1;
		}
		/* drop table and recreate */
		else if (CSEQUAL(argv[i], "-d")) {
			config->opt = 'd';
		}
		/* append to table */
		else if (CSEQUAL(argv[i], "-a")) {
			config->opt = 'a';
		}
		/* create new table */
		else if (CSEQUAL(argv[i], "-c")) {
			config->opt = 'c';
		}
		/* prepare only */
		else if (CSEQUAL(argv[i], "-p")) {
			config->opt = 'p';
		}
		/* raster column name */
		else if (CSEQUAL(argv[i], "-f") && i < argc - 1) {
			config->raster_column = rtalloc(sizeof(char) * (strlen(argv[++i]) + 1));
			if (config->raster_column == NULL) {
				fprintf(stderr, _("Could not allocate memory for storing raster column name\n"));
				rtdealloc_config(config);
				exit(1);
			}
			strncpy(config->raster_column, argv[i], strlen(argv[i]) + 1);
		}
		/* filename column */
		else if (CSEQUAL(argv[i], "-F")) {
			config->file_column = 1;
		}
		/* overview factors */
		else if (CSEQUAL(argv[i], "-l") && i < argc - 1) {
			elements = strsplit(argv[++i], ",", &n);
			if (n < 1) {
				fprintf(stderr, _("Cannot process -l.\n"));
				rtdealloc_config(config);
				exit(1);
			}

			config->overview_count = n;
			config->overview = rtalloc(sizeof(int) * n);
			if (config->overview == NULL) {
				fprintf(stderr, _("Could not allocate memory for storing overview factors\n"));
				rtdealloc_config(config);
				exit(1);
			}
			for (j = 0; j < n; j++) {
				char *t = trim(elements[j]);
				config->overview[j] = atoi(t);
				rtdealloc(t);
				rtdealloc(elements[j]);
			}
			rtdealloc(elements);
			elements = NULL;
			n = 0;

			for (j = 0; j < config->overview_count; j++) {
				if (config->overview[j] < MINOVFACTOR || config->overview[j] > MAXOVFACTOR) {
					fprintf(stderr, _("Overview factor %d is not between %d and %d.\n"), config->overview[j], MINOVFACTOR, MAXOVFACTOR);
					rtdealloc_config(config);
					exit(1);
				}
			}
		}
		/* quote identifiers */
		else if (CSEQUAL(argv[i], "-q")) {
			config->quoteident = 1;
		}
		/* create index */
		else if (CSEQUAL(argv[i], "-I")) {
			config->idx = 1;
		}
		/* maintenance */
		else if (CSEQUAL(argv[i], "-M")) {
			config->maintenance = 1;
		}
		/* set constraints */
		else if (CSEQUAL(argv[i], "-C")) {
			config->constraints = 1;
		}
		/* disable extent constraint */
		else if (CSEQUAL(argv[i], "-x")) {
			config->max_extent = 0;
		}
		/* enable regular_blocking */
		else if (CSEQUAL(argv[i], "-r")) {
			config->regular_blocking = 1;
		}
		/* tablespace of new table */
		else if (CSEQUAL(argv[i], "-T") && i < argc - 1) {
			config->tablespace = rtalloc(sizeof(char) * (strlen(argv[++i]) + 1));
			if (config->tablespace == NULL) {
				fprintf(stderr, _("Could not allocate memory for storing tablespace of new table\n"));
				rtdealloc_config(config);
				exit(1);
			}
			strncpy(config->tablespace, argv[i], strlen(argv[i]) + 1);
		}
		/* tablespace of new index */
		else if (CSEQUAL(argv[i], "-X") && i < argc - 1) {
			config->idx_tablespace = rtalloc(sizeof(char) * (strlen(argv[++i]) + 1));
			if (config->idx_tablespace == NULL) {
				fprintf(stderr, _("Could not allocate memory for storing tablespace of new indices\n"));
				rtdealloc_config(config);
				exit(1);
			}
			strncpy(config->idx_tablespace, argv[i], strlen(argv[i]) + 1);
		}
		/* nodata value */
		else if (CSEQUAL(argv[i], "-N") && i < argc - 1) {
			config->hasnodata = 1;
			config->nodataval = atof(argv[++i]);
		}
		/* endianness */
		else if (CSEQUAL(argv[i], "-E") && i < argc - 1) {
			config->endian = atoi(argv[++i]);
			config->endian = 1;
		}
		/* version */
		else if (CSEQUAL(argv[i], "-V") && i < argc - 1) {
			config->version = atoi(argv[++i]);
			config->version = 0;
		}
		/* transaction */
		else if (CSEQUAL(argv[i], "-e")) {
			config->transaction = 0;
		}
		/* COPY statements */
		else if (CSEQUAL(argv[i], "-Y")) {
			config->copy_statements = 1;
		}
		/* help */
		else if (CSEQUAL(argv[i], "-?")) {
			usage();
			rtdealloc_config(config);
			exit(0);
		}
		else {
			config->rt_file_count++;
			config->rt_file = (char **) rtrealloc(config->rt_file, sizeof(char *) * config->rt_file_count);
			if (config->rt_file == NULL) {
				fprintf(stderr, _("Could not allocate memory for storing raster files\n"));
				rtdealloc_config(config);
				exit(1);
			}

			config->rt_file[config->rt_file_count - 1] = rtalloc(sizeof(char) * (strlen(argv[i]) + 1));
			if (config->rt_file[config->rt_file_count - 1] == NULL) {
				fprintf(stderr, _("Could not allocate memory for storing raster filename\n"));
				rtdealloc_config(config);
				exit(1);
			}
			strncpy(config->rt_file[config->rt_file_count - 1], argv[i], strlen(argv[i]) + 1);
		}
	}

	/* no files provided */
	if (!config->rt_file_count) {
		fprintf(stderr, _("No raster provided.\n"));
		rtdealloc_config(config);
		exit(1);
	}
	/* at least two files, see if last is table */
	else if (config->rt_file_count > 1) {
		fp = fopen(config->rt_file[config->rt_file_count - 1], "rb");

		/* unable to access file, assume table */
		if (fp == NULL) {
			char *ptr;
			ptr = strchr(config->rt_file[config->rt_file_count - 1], '.');

			/* schema.table */
			if (ptr) {
				config->schema = rtalloc(sizeof(char) * (strlen(config->rt_file[config->rt_file_count - 1]) + 1));
				if (config->schema == NULL) {
					fprintf(stderr, _("Could not allocate memory for storing schema name\n"));
					rtdealloc_config(config);
					exit(1);
				}
				snprintf(config->schema, ptr - config->rt_file[config->rt_file_count - 1] + 1, "%s", config->rt_file[config->rt_file_count - 1]);

				config->table = rtalloc(sizeof(char) * strlen(config->rt_file[config->rt_file_count - 1]));
				if (config->table == NULL) {
					fprintf(stderr, _("Could not allocate memory for storing table name\n"));
					rtdealloc_config(config);
					exit(1);
				}
				snprintf(config->table, strlen(config->rt_file[config->rt_file_count - 1]) - strlen(config->schema), "%s", ptr + 1);
			}
			else {
				config->table = rtalloc(sizeof(char) * strlen(config->rt_file[config->rt_file_count - 1]) + 1);
				if (config->table == NULL) {
					fprintf(stderr, _("Could not allocate memory for storing table name\n"));
					rtdealloc_config(config);
					exit(1);
				}
				strncpy(config->table, config->rt_file[config->rt_file_count - 1], strlen(config->rt_file[config->rt_file_count - 1]) + 1);
			}

			rtdealloc(config->rt_file[--(config->rt_file_count)]);
			config->rt_file = (char **) rtrealloc(config->rt_file, sizeof(char *) * config->rt_file_count);
			if (config->rt_file == NULL) {
				fprintf(stderr, _("Could not reallocate the memory holding raster names\n"));
				rtdealloc_config(config);
				exit(1);
			}
		}
		else {
			fclose(fp);
			fp = NULL;
		}
	}

	/****************************************************************************
	* validate raster files
	****************************************************************************/

	/* check that all files are touchable */
	for (i = 0; i < config->rt_file_count; i++) {
		fp = fopen(config->rt_file[i], "rb");

		if (fp == NULL) {
			fprintf(stderr, _("Unable to read raster file: %s\n"), config->rt_file[i]);
			rtdealloc_config(config);
			exit(1);
		}

		fclose(fp);
		fp = NULL;
	}

	/* process each file for just the filename */
	config->rt_filename = (char **) rtalloc(sizeof(char *) * config->rt_file_count);
	if (config->rt_filename == NULL) {
		fprintf(stderr, _("Could not allocate memory for cleaned raster filenames\n"));
		rtdealloc_config(config);
		exit(1);
	}
	for (i = 0; i < config->rt_file_count; i++) {
		char *file;
		char *ptr;

		file = rtalloc(sizeof(char) * (strlen(config->rt_file[i]) + 1));
		if (file == NULL) {
			fprintf(stderr, _("Could not allocate memory for cleaned raster filename\n"));
			rtdealloc_config(config);
			exit(1);
		}
		strcpy(file, config->rt_file[i]);

		for (ptr = file + strlen(file); ptr > file; ptr--) {
			if (*ptr == '/' || *ptr == '\\') {
				ptr++;
				break;
			}
		}

		config->rt_filename[i] = rtalloc(sizeof(char) * (strlen(ptr) + 1));
		if (config->rt_filename[i] == NULL) {
			fprintf(stderr, _("Could not allocate memory for cleaned raster filename\n"));
			rtdealloc_config(config);
			exit(1);
		}
		strcpy(config->rt_filename[i], ptr);
		rtdealloc(file);
	}

	/****************************************************************************
	* defaults for table and column names
	****************************************************************************/

	/* first file as proxy table name */
	if (config->table == NULL) {
		char *file;
		char *ptr;

		file = rtalloc(sizeof(char) * (strlen(config->rt_filename[0]) + 1));
		if (file == NULL) {
			fprintf(stderr, _("Could not allocate memory for proxy table name\n"));
			rtdealloc_config(config);
			exit(1);
		}
		strcpy(file, config->rt_filename[0]);

		for (ptr = file + strlen(file); ptr > file; ptr--) {
			if (*ptr == '.') {
				*ptr = '\0';
				break;
			}
		}

		config->table = rtalloc(sizeof(char) * (strlen(file) + 1));
		if (config->table == NULL) {
			fprintf(stderr, _("Could not allocate memory for proxy table name\n"));
			rtdealloc_config(config);
			exit(1);
		}
		strcpy(config->table, file);
		rtdealloc(file);
	}

	/* raster_column not specified, default to "rast" */
	if (config->raster_column == NULL) {
		config->raster_column = rtalloc(sizeof(char) * (strlen("rast") + 1));
		if (config->raster_column == NULL) {
			fprintf(stderr, _("Could not allocate memory for default raster column name\n"));
			rtdealloc_config(config);
			exit(1);
		}
		strcpy(config->raster_column, "rast");
	}

	/****************************************************************************
	* literal PostgreSQL identifiers disabled
	****************************************************************************/

	/* no quotes, lower case everything */
	if (!config->quoteident) {
		if (config->schema != NULL)
			config->schema = strtolower(config->schema);
		if (config->table != NULL)
			config->table = strtolower(config->table);
		if (config->raster_column != NULL)
			config->raster_column = strtolower(config->raster_column);
		if (config->tablespace != NULL)
			config->tablespace = strtolower(config->tablespace);
		if (config->idx_tablespace != NULL)
			config->idx_tablespace = strtolower(config->idx_tablespace);
	}

	/****************************************************************************
	* overview table names
	****************************************************************************/

	if (config->overview_count) {
		char factor[4];
		config->overview_table = rtalloc(sizeof(char *) * config->overview_count);
		if (config->overview_table == NULL) {
			fprintf(stderr, _("Could not allocate memory for overview table names\n"));
			rtdealloc_config(config);
			exit(1);
		}

		for (i = 0; i < config->overview_count; i++) {
			sprintf(factor, "%d", config->overview[i]);

			config->overview_table[i] = rtalloc(sizeof(char) * (strlen("o__") + strlen(factor) + strlen(config->table) + 1));
			if (config->overview_table[i] == NULL) {
				fprintf(stderr, _("Could not allocate memory for overview table name\n"));
				rtdealloc_config(config);
				exit(1);
			}
			sprintf(config->overview_table[i], "o_%d_%s", config->overview[i], config->table);
		}
	}

	/****************************************************************************
	* check that identifiers won't get truncated
	****************************************************************************/

	if (config->schema != NULL && strlen(config->schema) > MAXNAMELEN) {
		fprintf(stderr, _("The schema name \"%s\" may exceed the maximum string length permitted for PostgreSQL identifiers (%d).\n"),
			config->schema,
			MAXNAMELEN
		);
	}
	if (config->table != NULL && strlen(config->table) > MAXNAMELEN) {
		fprintf(stderr, _("The table name \"%s\" may exceed the maximum string length permitted for PostgreSQL identifiers (%d).\n"),
			config->table,
			MAXNAMELEN
		);
	}
	if (config->raster_column != NULL && strlen(config->raster_column) > MAXNAMELEN) {
		fprintf(stderr, _("The column name \"%s\" may exceed the maximum string length permitted for PostgreSQL identifiers (%d).\n"),
			config->raster_column,
			MAXNAMELEN
		);
	}
	if (config->tablespace != NULL && strlen(config->tablespace) > MAXNAMELEN) {
		fprintf(stderr, _("The tablespace name \"%s\" may exceed the maximum string length permitted for PostgreSQL identifiers (%d).\n"),
			config->tablespace,
			MAXNAMELEN
		);
	}
	if (config->idx_tablespace != NULL && strlen(config->idx_tablespace) > MAXNAMELEN) {
		fprintf(stderr, _("The index tablespace name \"%s\" may exceed the maximum string length permitted for PostgreSQL identifiers (%d).\n"),
			config->idx_tablespace,
			MAXNAMELEN
		);
	}
	if (config->overview_count) {
		for (i = 0; i < config->overview_count; i++) {
			if (strlen(config->overview_table[i]) > MAXNAMELEN) {
				fprintf(stderr, _("The overview table name \"%s\" may exceed the maximum string length permitted for PostgreSQL identifiers (%d).\n"),
					config->overview_table[i],
					MAXNAMELEN
				);
			}
		}
	}

	/****************************************************************************
	* double quote identifiers
	****************************************************************************/

	if (config->schema != NULL) {
		tmp = rtalloc(sizeof(char) * (strlen(config->schema) + 4));
		if (tmp == NULL) {
			fprintf(stderr, _("Could not allocate memory for quoting schema name\n"));
			rtdealloc_config(config);
			exit(1);
		}

		sprintf(tmp, "\"%s\".", config->schema);
		rtdealloc(config->schema);
		config->schema = tmp;
	}
	if (config->table != NULL) {
		tmp = rtalloc(sizeof(char) * (strlen(config->table) + 3));
		if (tmp == NULL) {
			fprintf(stderr, _("Could not allocate memory for quoting table name\n"));
			rtdealloc_config(config);
			exit(1);
		}

		sprintf(tmp, "\"%s\"", config->table);
		rtdealloc(config->table);
		config->table = tmp;
	}
	if (config->raster_column != NULL) {
		tmp = rtalloc(sizeof(char) * (strlen(config->raster_column) + 3));
		if (tmp == NULL) {
			fprintf(stderr, _("Could not allocate memory for quoting raster column name\n"));
			rtdealloc_config(config);
			exit(1);
		}

		sprintf(tmp, "\"%s\"", config->raster_column);
		rtdealloc(config->raster_column);
		config->raster_column = tmp;
	}
	if (config->tablespace != NULL) {
		tmp = rtalloc(sizeof(char) * (strlen(config->tablespace) + 3));
		if (tmp == NULL) {
			fprintf(stderr, _("Could not allocate memory for quoting tablespace name\n"));
			rtdealloc_config(config);
			exit(1);
		}

		sprintf(tmp, "\"%s\"", config->tablespace);
		rtdealloc(config->tablespace);
		config->tablespace = tmp;
	}
	if (config->idx_tablespace != NULL) {
		tmp = rtalloc(sizeof(char) * (strlen(config->idx_tablespace) + 3));
		if (tmp == NULL) {
			fprintf(stderr, _("Could not allocate memory for quoting index tablespace name\n"));
			rtdealloc_config(config);
			exit(1);
		}

		sprintf(tmp, "\"%s\"", config->idx_tablespace);
		rtdealloc(config->idx_tablespace);
		config->idx_tablespace = tmp;
	}
	if (config->overview_count) {
		for (i = 0; i < config->overview_count; i++) {
			tmp = rtalloc(sizeof(char) * (strlen(config->overview_table[i]) + 3));
			if (tmp == NULL) {
				fprintf(stderr, _("Could not allocate memory for quoting overview table name\n"));
				rtdealloc_config(config);
				exit(1);
			}

			sprintf(tmp, "\"%s\"", config->overview_table[i]);
			rtdealloc(config->overview_table[i]);
			config->overview_table[i] = tmp;
		}
	}

	/****************************************************************************
	* processing of rasters
	****************************************************************************/

	/* initialize string buffer */
	buffer = rtalloc(sizeof(STRINGBUFFER));
	if (buffer == NULL) {
		fprintf(stderr, _("Could not allocate memory for output string buffer\n"));
		rtdealloc_config(config);
		exit(1);
	}
	init_stringbuffer(buffer);

	/* pass off to processing function */
	if (!process_rasters(config, buffer)) {
		fprintf(stderr, _("Unable to process rasters\n"));
		rtdealloc_stringbuffer(buffer, 1);
		rtdealloc_config(config);
		exit(1);
	}

	flush_stringbuffer(buffer);

	rtdealloc_stringbuffer(buffer, 1);
	rtdealloc_config(config);

	return 0;
}
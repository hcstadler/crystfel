/*
 * image.h
 *
 * Handle images and image features
 *
 * Copyright © 2012-2013 Deutsches Elektronen-Synchrotron DESY,
 *                       a research centre of the Helmholtz Association.
 *
 * Authors:
 *   2009-2013 Thomas White <taw@physics.org>
 *
 * This file is part of CrystFEL.
 *
 * CrystFEL is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * CrystFEL is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with CrystFEL.  If not, see <http://www.gnu.org/licenses/>.
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#ifndef IMAGE_H
#define IMAGE_H

#include <stdint.h>
#include <complex.h>
#include <sys/types.h>

#include "utils.h"
#include "cell.h"
#include "detector.h"
#include "reflist.h"
#include "crystal.h"
#include "index.h"


/* Structure describing a feature in an image */
struct imagefeature {

	struct image                    *parent;
	double                          fs;
	double                          ss;
	double                          intensity;

	/* Reciprocal space coordinates (m^-1 of course) of this feature */
	double                          rx;
	double                          ry;
	double                          rz;

	/* Internal use only */
	int                             valid;

	const char                      *name;
};

/* An opaque type representing a list of image features */
typedef struct _imagefeaturelist ImageFeatureList;


/**
 * image:
 *
 * <programlisting>
 * struct image
 * {
 *    float                   *data;
 *    uint16_t                *flags;
 *    double                  *twotheta;
 *
 *    Crystal                 **crystals;
 *    int                     n_crystals;
 *    IndexingMethod          indexed_by;
 *
 *    struct detector         *det;
 *    struct beam_params      *beam;
 *    char                    *filename;
 *    const struct copy_hdf5_field *copyme;
 *
 *    int                     id;
 *
 *    double                  lambda;
 *    double                  div;
 *    double                  bw;
 *
 *    int                     width;
 *    int                     height;
 *
 *    long long int           num_peaks;
 *    long long int           num_saturated_peaks;
 *    ImageFeatureList        *features;
 * };
 * </programlisting>
 *
 * The field <structfield>data</structfield> contains the raw image data, if it
 * is currently available.  The data might be available throughout the
 * processing of an experimental pattern, but it might not be available when
 * simulating, scaling or merging patterns.  Similarly,
 * <structfield>flags</structfield> contains an array of the same dimensions
 * as <structfield>data</structfield> to contain the bad pixel flags.
 * <structfield>twotheta</structfield> likewise contains an array of 2*theta
 * (scattering angle) values in radians, since these values are generated as a
 * by-product of the scattering vector calculation and can be used later for
 * calculating intensities from differential scattering cross sections.
 *
 * <structfield>crystals</structfield> is an array of %Crystal directly
 * returned by the low-level indexing system. <structfield>n_crystals</structfield>
 * is the number of crystals which were found in the image.
 *
 * <structfield>copyme</structfield> represents a list of HDF5 fields to copy
 * to the output stream.
 **/
struct image;

struct image {

	float                   *data;
	uint16_t                *flags;
	double                  *twotheta;

	Crystal                 **crystals;
	int                     n_crystals;
	IndexingMethod          indexed_by;

	struct detector         *det;
	struct beam_params      *beam;  /* The nominal beam parameters */
	char                    *filename;
	const struct copy_hdf5_field *copyme;

	int                     id;   /* ID number of the thread
	                               * handling this image */

	/* Per-shot radiation values */
	double                  lambda;        /* Wavelength in m */
	double                  div;           /* Divergence in radians */
	double                  bw;            /* Bandwidth as a fraction */

	int                     width;
	int                     height;

	/* Detected peaks */
	long long int           num_peaks;
	long long int           num_saturated_peaks;
	ImageFeatureList        *features;

};


/* Feature lists */
extern ImageFeatureList *image_feature_list_new(void);

extern void image_feature_list_free(ImageFeatureList *flist);

extern void image_add_feature(ImageFeatureList *flist, double x, double y,
                              struct image *parent, double intensity,
                              const char *name);

extern void image_remove_feature(ImageFeatureList *flist, int idx);

extern struct imagefeature *image_feature_closest(ImageFeatureList *flist,
                                                  double fs, double ss,
                                                  double *d, int *idx);

extern int image_feature_count(ImageFeatureList *flist);
extern struct imagefeature *image_get_feature(ImageFeatureList *flist, int idx);

extern void image_add_crystal(struct image *image, Crystal *cryst);
extern void free_all_crystals(struct image *image);

#endif	/* IMAGE_H */

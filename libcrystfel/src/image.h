/*
 * image.h
 *
 * Handle images and image features
 *
 * Copyright © 2012-2018 Deutsches Elektronen-Synchrotron DESY,
 *                       a research centre of the Helmholtz Association.
 *
 * Authors:
 *   2009-2018 Thomas White <taw@physics.org>
 *   2014      Valerio Mariani
 *
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

struct detector;

#include <stdint.h>
#include <complex.h>
#include <sys/types.h>

struct imagefeature;
struct sample;
struct image;
struct imagefile;
struct imagefile_field_list;

#include "utils.h"
#include "cell.h"
#include "detector.h"
#include "reflist.h"
#include "crystal.h"
#include "index.h"
#include "events.h"

/**
 * \file image.h
 *
 * Information about images
 */

/** Represents a peak in an image. */
struct imagefeature {

	struct image                    *parent;   /**< Pointer to image */

	/** \name Coordinates on panel (fast scan, slow scan)
	 *  Note carefully that these are the distances, measured in pixels,
	 *  from the corner of the panel.  They are NOT pixel indices.
	 *  If the peak is in the middle of the first pixel, its coordinates would be
	 *  0.5,0.5. */
	/**@{*/
	double                          fs;
	double                          ss;
	/**@}*/

	struct panel                    *p;         /**< Pointer to panel */
	double                          intensity;  /**< Intensity */

	/** \name Reciprocal space coordinates (m^-1) of this feature */
	/** @{ */
	double                          rx;
	double                          ry;
	double                          rz;
	/** @} */

	const char                      *name;  /**< Text name, e.g. "5,3,-1" */
};


/** An enum representing the image file formats we can handle */
enum imagefile_type
{
	IMAGEFILE_HDF5,   /**< HDF5 file (single or multiple frames per file) */
	IMAGEFILE_CBF,    /**< CBF file */
	IMAGEFILE_CBFGZ   /**< gzipped CBF file, i.e. "file.cbf.gz" */
};


/** An opaque type representing a list of image features */
typedef struct _imagefeaturelist ImageFeatureList;


struct spectrum
{
	int n;
	double *ks; /* 1/m */
	double *weights;
};


/** Structure describing a wavelength sample from a spectrum.
 * \deprecated Use struct spectrum instead. */
struct sample
{
	double k;         /**< Wavevector in m^-1 */
	double weight;    /**< Relative weight */
};


struct beam_params
{
	double photon_energy;       /**< eV per photon */
	char  *photon_energy_from;  /**< HDF5 dataset name */
	double photon_energy_scale; /**< Scale factor for photon energy, if it
	                             *   comes from an image header */
};


struct image
{
	/** The image data, by panel */
	float                   **dp;

	/** The bad pixel mask, by panel */
	int                     **bad;

	/** The per-pixel saturation values, by panel */
	float                   **sat;

	/** Non-zero if the frame was determined to be a "hit" */
	int                     hit;

	/**Array of crystals in the image */
	Crystal                 **crystals;

	/** The number of crystals in the image (size of \p crystals) */
	int                     n_crystals;

	/** Indexing method which indexed this pattern */
	IndexingMethod          indexed_by;

	/** Number of times the indexer was tried before succeeding */
	int                     n_indexing_tries;

	/** The detector structure */
	struct detector         *det;

	/** The nominal beam parameters (or where to get them) */
	struct beam_params      *beam;

	/** \name The filename and event ID for the image
	 * @{ */
	char                    *filename;
	struct event            *event;
	/** @} */

	/** A list of image file headers to copy to the stream */
	const struct imagefile_field_list *copyme;

	/** A list of metadata read from the stream */
	struct stuff_from_stream *stuff_from_stream;

	/** Mean of the camera length values for all panels */
	double                  avg_clen;

	/** ID number of the worker processing handling this image */
	int                     id;

	/** Monotonically increasing serial number for this image */
	int                     serial;

	/** Spectrum information (new format) */
	struct spectrum *spectrum;

	/** \name Spectrum information (old format)
	 * @{
	 * Array of samples, number of samples, and size of the array (may be
	 * larger than nsamples) */
	struct sample           *spectrum0;
	int                     nsamples;
	int                     spectrum_size;
	/** @} */

	/** Wavelength of the incident radiation, in metres */
	double                  lambda;

	/** Convergence angle of the incident ration, in radians (full angle) */
	double                  div;

	/** Full-width half-maximum bandwidth as a fraction, applied to wavelength */
	double                  bw;

	/** \name Numbers of peaks.  To be deleted.
	 * @{ */
	long long               num_peaks;
	long long               num_saturated_peaks;
	double                  peak_resolution;
	/** @} */

	/** List of peaks found in the image */
	ImageFeatureList        *features;

};

#ifdef __cplusplus
extern "C" {
#endif

/* Feature lists */
extern ImageFeatureList *image_feature_list_new(void);

extern void image_feature_list_free(ImageFeatureList *flist);

extern void image_add_feature(ImageFeatureList *flist, double x, double y,
                              struct panel *p,
                              struct image *parent, double intensity,
                              const char *name);

extern void image_remove_feature(ImageFeatureList *flist, int idx);

extern struct imagefeature *image_feature_closest(ImageFeatureList *flist,
                                                  double fs, double ss,
                                                  struct panel *p,
                                                  double *d, int *idx);

extern Reflection *image_reflection_closest(RefList *rlist,
                                            double fs, double ss,
                                            struct panel *p,
                                            struct detector *det,
                                            double *d);

extern int image_feature_count(ImageFeatureList *flist);
extern struct imagefeature *image_get_feature(ImageFeatureList *flist, int idx);
extern ImageFeatureList *sort_peaks(ImageFeatureList *flist);

extern void image_add_crystal(struct image *image, Crystal *cryst);
extern int remove_flagged_crystals(struct image *image);
extern void free_all_crystals(struct image *image);

/* Image files */
extern struct imagefile *imagefile_open(const char *filename);
extern int imagefile_read(struct imagefile *f, struct image *image,
                          struct event *event);
extern int imagefile_read_simple(struct imagefile *f, struct image *image);
extern struct hdfile *imagefile_get_hdfile(struct imagefile *f);
extern enum imagefile_type imagefile_get_type(struct imagefile *f);
extern void imagefile_copy_fields(struct imagefile *f,
                                  const struct imagefile_field_list *copyme,
                                  FILE *fh, struct event *ev);
extern void imagefile_close(struct imagefile *f);
extern signed int is_cbf_file(const char *filename);

/* Field lists */
extern struct imagefile_field_list *new_imagefile_field_list(void);
extern void free_imagefile_field_list(struct imagefile_field_list *f);

extern void add_imagefile_field(struct imagefile_field_list *copyme,
                                const char *name);

#ifdef __cplusplus
}
#endif

#endif	/* IMAGE_H */

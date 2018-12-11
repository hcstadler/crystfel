/*
 * zmq.c
 *
 * ZMQ data interface
 *
 * Copyright © 2017-2018 Deutsches Elektronen-Synchrotron DESY,
 *                       a research centre of the Helmholtz Association.
 *
 * Authors:
 *   2018 Thomas White <taw@physics.org>
 *   2014 Valerio Mariani
 *   2017 Stijna de Graaf
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

#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <hdf5.h>
#include <assert.h>
#include <unistd.h>
#include <msgpack.h>

#include "events.h"
#include "image.h"
#include "hdf5-file.h"
#include "utils.h"


/**
 * get_peaks_onda:
 * @obj: A %msgpack_object containing data in OnDA format
 * @image: An %image structure
 * @half_pixel_shift: Non-zero if 0.5 should be added to all peak coordinates
 *
 * Get peaks from msgpack_object. The data should be in a map, with the value
 * given by "peak_list" as an array of arrays. The first of these should contain
 * the list of fs positions of the peaks, the second the ss positions, and the
 * third the intensities of the peaks.
 *
 * http://c.msgpack.org/c/ provides documentation on msgpack objects
 *
 * CrystFEL considers all peak locations to be distances from the corner of the
 * detector panel, in pixel units, consistent with its description of detector
 * geometry (see 'man crystfel_geometry').  The software which generates the
 * CXI files, including Cheetah, may instead consider the peak locations to be
 * pixel indices in the data array.  In this case, the peak coordinates should
 * have 0.5 added to them.  This will be done if @half_pixel_shift is non-zero.
 *
 * Returns: non-zero on error, zero otherwise.
 *
 */
int get_peaks_onda(msgpack_object *obj, struct image *image,
                   int half_pixel_shift)
{

	int num_peaks;
	int pk;
	double peak_offset = half_pixel_shift ? 0.5 : 0.0;

	int entry;
	char *key_str;
	msgpack_object map_val, peak_list;

	/* iterate over key-value pairs in msgpack_object
	 * object has structure:
	 * 	{"peak_list": [[peak_x], [peak_y], [peak_i]],"key2":val2,...}
	 */
	for ( entry = 0; entry < obj->via.map.size; entry++ ) {
		key_str = (char *)obj->via.map.ptr[entry].key.via.str.ptr;
		/* check if key matches "peak_list" */
		if (strncmp(key_str, "peak_list", 9) == 0) {
			map_val = obj->via.map.ptr[entry].val;
			/* length of peak_x array gives number of peaks */
			num_peaks = map_val.via.array.ptr[0].via.array.size;
			peak_list = map_val;
		}
	}

	if ( image->features != NULL ) {
		image_feature_list_free(image->features);
	}
	image->features = image_feature_list_new();
	image->num_peaks = num_peaks;

	for ( pk = 0; pk<num_peaks; pk++ ) {

		float fs, ss, val;
		struct panel *p;

		/* retrieve data from peak_list and apply half_pixel_shift,
		 * if appropriate */
		fs = peak_list.via.array.ptr[0].via.array.ptr[pk].via.f64 + peak_offset;
		ss = peak_list.via.array.ptr[1].via.array.ptr[pk].via.f64 + peak_offset;
		val = peak_list.via.array.ptr[2].via.array.ptr[pk].via.f64;

		p = find_orig_panel(image->det, fs, ss);
		if ( p == NULL ) continue;
		if ( p->no_index ) continue;

		/* Convert coordinates to panel-relative */
		fs = fs - p->orig_min_fs;
		ss = ss - p->orig_min_ss;

		image_add_feature(image->features, fs, ss, p, image, val, NULL);
	}

	return 0;
}


static void onda_fill_in_clen(struct detector *det)
{
    int i = 0;

    for ( i=0; i<det->n_panels; i++) {

        struct panel *p = &det->panels[i];

        if ( p->clen_from != NULL ) {

            ERROR("Can't get clen from OnDA yet.\n");
        }

        adjust_centering_for_rail(p);

    }
}


/* Equivalent to fill_in_beam_parameters but without reference to imagefiles */
static void onda_fill_in_beam_parameters(struct beam_params *beam,
                                         struct image *image)
{
    double eV;

    if (beam->photon_energy_from == NULL ) {

        /* Explicit value given */
        eV = beam->photon_energy;

    } else {

        ERROR("Can't get photon energy from OnDA yet.\n");
        eV = 0.0;

    }

	image->lambda = ph_en_to_lambda(eV_to_J(eV))*beam->photon_energy_scale;
}


/* Unpacks the raw panel data from a msgpack_object, appliespanel geometry,
 * and stores the resulting data in an image struct. Object has structure
 * {
 *	 "corr_data":
 *   {
 *	     "data": binary_data,
 *       "shape": [data_height, data_width],
 *             ...
 *             ...
 *   },
 *   "key2": val2,
 *        ...
 *        ...
 * }
 */
int obj_read(msgpack_object *obj, struct image *image)
{

	uint16_t *flags = NULL;
	float *sat = NULL;
	int pi;
	int entry, sub_entry;
	int data_width, data_height;
	double *data;
	char *key_str;
	msgpack_object map_val;

	// Iterate over key-value pairs in msgpack_object
	for ( entry=0; entry<obj->via.map.size; entry++ ) {
		key_str = (char *)obj->via.map.ptr[entry].key.via.str.ptr;
		// Check for key is "corr_data"
		if ( strncmp(key_str, "corr_data", 9) == 0 ) {
			map_val = obj->via.map.ptr[entry].val;
			// Iterate over key-value pairs in inner map
			for ( sub_entry=0; sub_entry<map_val.via.map.size; sub_entry++ ) {
				key_str = (char *)map_val.via.map.ptr[sub_entry].key.via.str.ptr;
				// Check for key is "data"
				if ( strncmp(key_str, "data", 4) == 0 ) {
					data = (double *)map_val.via.map.ptr[sub_entry].val.via.bin.ptr;
				// Check for key is "shape"
				} else if ( strncmp(key_str, "shape", 5) == 0 ) {
					data_height = map_val.via.map.ptr[sub_entry].val.via.array.ptr[0].via.i64;
					data_width = map_val.via.map.ptr[sub_entry].val.via.array.ptr[1].via.i64;
				}
			}
		}
	}

	if ( image->det == NULL ) {
		ERROR("Geometry not available.\n");
		return 1;
	}

	image->dp = malloc(image->det->n_panels*sizeof(float *));
    image->bad = malloc(image->det->n_panels*sizeof(int *));
    image->sat = malloc(image->det->n_panels*sizeof(float *));
    if ( (image->dp == NULL) || (image->bad == NULL) || (image->sat == NULL) ) {
        ERROR("Failed to allocate data arrays.\n");
        return 1;
    }

    for ( pi=0; pi<image->det->n_panels; pi++ ) {

        struct panel *p;
        int fs, ss;

        p = &image->det->panels[pi];
        image->dp[pi] = malloc(p->w*p->h*sizeof(float));
        image->bad[pi] = malloc(p->w*p->h*sizeof(int));
        image->sat[pi] = malloc(p->w*p->h*sizeof(float));
        if ( (image->dp[pi] == NULL) || (image->bad[pi] == NULL) || (image->sat[pi] == NULL) )
        {
            ERROR("Failed to allocate panel\n");
            return 1;
        }

        if ( (p->orig_min_fs + p->w > data_width)
            || (p->orig_min_ss + p->h > data_height) )
        {
            ERROR("Panel %s is outside range of data provided\n",
                  p->name);
            return 1;
        }

        for ( ss=0; ss<p->h; ss++) {
        for ( fs=0; fs<p->w; fs++) {

            int idx;
            int cfs, css;
            int bad = 0;

            cfs = fs+p->orig_min_fs;
            css = ss+p->orig_min_ss;
            idx = cfs + css*data_width;

            image->dp[pi][fs+p->w*ss] = data[idx];

            if ( sat != NULL ) {
                image->sat[pi][fs+p->w*ss] = sat[idx];
            } else {
                image->sat[pi][fs+p->w*ss] = INFINITY;
            }

            if ( p->no_index ) bad = 1;

            if ( in_bad_region(image->det, p, cfs, css) ) {
                bad = 1;
            }

            if ( flags != NULL ) {

                int f;

                f = flags[idx];

                if ( (f & image->det->mask_good)
                    != image->det->mask_good ) bad = 1;

                if ( f & image->det->mask_bad ) bad = 1;

            }
            image->bad[pi][fs+p->w*ss] = bad;
        }
        }

    }

    // might need to do some freeing of memory for msgpack object here

    if ( image->beam != NULL ) {
        onda_fill_in_beam_parameters(image->beam, image);
        if ( image->lambda > 1000 ) {
            ERROR("Warning: Missing or nonsensical wavelength "
                  "(%e m).\n",
                  image->lambda);
        }
    }
    onda_fill_in_clen(image->det);
    fill_in_adu(image);

    return 0;
}

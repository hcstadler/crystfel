/*
 * partial_sim.c
 *
 * Generate partials for testing scaling
 *
 * Copyright © 2012-2013 Deutsches Elektronen-Synchrotron DESY,
 *                       a research centre of the Helmholtz Association.
 *
 * Authors:
 *   2011-2013 Thomas White <taw@physics.org>
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

#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <getopt.h>
#include <assert.h>
#include <pthread.h>

#include "image.h"
#include "utils.h"
#include "reflist-utils.h"
#include "symmetry.h"
#include "beam-parameters.h"
#include "geometry.h"
#include "stream.h"
#include "thread-pool.h"
#include "cell-utils.h"

/* Number of bins for partiality graph */
#define NBINS 50


static void mess_up_cell(Crystal *cr, double cnoise)
{
	double ax, ay, az;
	double bx, by, bz;
	double cx, cy, cz;
	UnitCell *cell = crystal_get_cell(cr);

	//STATUS("Real:\n");
	//cell_print(cell);

	cell_get_reciprocal(cell, &ax, &ay, &az, &bx, &by, &bz, &cx, &cy, &cz);
	ax = flat_noise(ax, cnoise*fabs(ax)/100.0);
	ay = flat_noise(ay, cnoise*fabs(ay)/100.0);
	az = flat_noise(az, cnoise*fabs(az)/100.0);
	bx = flat_noise(bx, cnoise*fabs(bx)/100.0);
	by = flat_noise(by, cnoise*fabs(by)/100.0);
	bz = flat_noise(bz, cnoise*fabs(bz)/100.0);
	cx = flat_noise(cx, cnoise*fabs(cx)/100.0);
	cy = flat_noise(cy, cnoise*fabs(cy)/100.0);
	cz = flat_noise(cz, cnoise*fabs(cz)/100.0);
	cell_set_reciprocal(cell, ax, ay, az, bx, by, bz, cx, cy, cz);

	//STATUS("Changed:\n");
	//cell_print(cell);
}


/* For each reflection in "partial", fill in what the intensity would be
 * according to "full" */
static void calculate_partials(Crystal *cr,
                               RefList *full, const SymOpList *sym,
                               int random_intensities,
                               pthread_rwlock_t *full_lock,
                               unsigned long int *n_ref, double *p_hist,
                               double *p_max, double max_q, double full_stddev,
                               double noise_stddev)
{
	Reflection *refl;
	RefListIterator *iter;
	double res;

	for ( refl = first_refl(crystal_get_reflections(cr), &iter);
	      refl != NULL;
	      refl = next_refl(refl, iter) )
	{
		signed int h, k, l;
		Reflection *rfull;
		double L, p, Ip, If;
		int bin;

		get_indices(refl, &h, &k, &l);
		get_asymm(sym, h, k, l, &h, &k, &l);
		p = get_partiality(refl);
		L = get_lorentz(refl);

		pthread_rwlock_rdlock(full_lock);
		rfull = find_refl(full, h, k, l);
		pthread_rwlock_unlock(full_lock);

		if ( rfull == NULL ) {
			if ( random_intensities ) {

				pthread_rwlock_wrlock(full_lock);

				/* In the gap between the unlock and the wrlock,
				 * the reflection might have been created by
				 * another thread.  So, we must check again */
				rfull = find_refl(full, h, k, l);
				if ( rfull == NULL ) {
					rfull = add_refl(full, h, k, l);
					If = fabs(gaussian_noise(0.0,
					                         full_stddev));
					set_intensity(rfull, If);
					set_redundancy(rfull, 1);
				} else {
					If = get_intensity(rfull);
				}
				pthread_rwlock_unlock(full_lock);

			} else {
				set_redundancy(refl, 0);
				If = 0.0;
			}
		} else {
			If = get_intensity(rfull);
			if ( random_intensities ) {
				lock_reflection(rfull);
				int red = get_redundancy(rfull);
				set_redundancy(rfull, red+1);
				unlock_reflection(rfull);
			}
		}

		Ip = crystal_get_osf(cr) * L * p * If;

		res = resolution(crystal_get_cell(cr), h, k, l);
		bin = NBINS*2.0*res/max_q;
		if ( (bin < NBINS) && (bin>=0) ) {
			p_hist[bin] += p;
			n_ref[bin]++;
			if ( p > p_max[bin] ) p_max[bin] = p;
		} else {
			STATUS("Reflection out of histogram range: %e %i %f\n",
			       res, bin,  p);
		}

		Ip = gaussian_noise(Ip, noise_stddev);

		set_intensity(refl, Ip);
		set_esd_intensity(refl, noise_stddev);
	}
}


static void show_help(const char *s)
{
	printf("Syntax: %s [options]\n\n", s);
	printf(
"Generate a stream containing partials from a reflection list.\n"
"\n"
" -h, --help              Display this help message.\n"
"\n"
"You need to provide the following basic options:\n"
" -i, --input=<file>       Read reflections from <file>.\n"
"                           Default: generate random ones instead (see -r).\n"
" -o, --output=<file>      Write partials in stream format to <file>.\n"
" -g. --geometry=<file>    Get detector geometry from file.\n"
" -b, --beam=<file>        Get beam parameters from file\n"
" -p, --pdb=<file>         PDB file from which to get the unit cell.\n"
"\n"
" -y, --symmetry=<sym>     Symmetry of the input reflection list.\n"
" -n <n>                   Simulate <n> patterns.  Default: 2.\n"
" -r, --save-random=<file> Save randomly generated intensities to file.\n"
"     --pgraph=<file>      Save a histogram of partiality values to file.\n"
" -c, --cnoise=<val>       Add random noise, with a flat distribution, to the\n"
"                          reciprocal lattice vector components given in the\n"
"                          stream, with maximum error +/- <val> percent.\n"
"     --osf-stddev=<val>   Set the standard deviation of the scaling factors.\n"
"     --full-stddev=<val>  Set the standard deviation of the randomly\n"
"                           generated full intensities, if not using -i.\n"
"     --noise-stddev=<val>  Set the standard deviation of the noise.\n"
"\n"
);
}


struct queue_args
{
	RefList *full;
	pthread_rwlock_t full_lock;

	int n_done;
	int n_started;
	int n_to_do;

	SymOpList *sym;
	int random_intensities;
	UnitCell *cell;
	double cnoise;
	double osf_stddev;
	double full_stddev;
	double noise_stddev;

	struct image *template_image;
	double max_q;

	/* The overall histogram */
	double p_hist[NBINS];
	unsigned long int n_ref[NBINS];
	double p_max[NBINS];

	Stream *stream;
};


struct worker_args
{
	struct queue_args *qargs;
	Crystal *crystal;
	struct image image;

	/* Histogram for this image */
	double p_hist[NBINS];
	unsigned long int n_ref[NBINS];
	double p_max[NBINS];
};


static void *create_job(void *vqargs)
{
	struct worker_args *wargs;
	struct queue_args *qargs = vqargs;

	/* All done already? */
	if ( qargs->n_started == qargs->n_to_do ) return NULL;

	wargs = malloc(sizeof(struct worker_args));

	wargs->qargs = qargs;
	wargs->image = *qargs->template_image;

	qargs->n_started++;

	return wargs;
}


static void run_job(void *vwargs, int cookie)
{
	struct quaternion orientation;
	struct worker_args *wargs = vwargs;
	struct queue_args *qargs = wargs->qargs;
	int i;
	Crystal *cr;
	RefList *reflections;
	double osf;

	cr = crystal_new();
	if ( cr == NULL ) {
		ERROR("Failed to create crystal.\n");
		return;
	}
	wargs->crystal = cr;
	crystal_set_image(cr, &wargs->image);

	do {
		osf = gaussian_noise(1.0, qargs->osf_stddev);
	} while ( osf <= 0.0 );
	crystal_set_osf(cr, osf);
	crystal_set_mosaicity(cr, 0.0);
	crystal_set_profile_radius(cr, wargs->image.beam->profile_radius);

	/* Set up a random orientation */
	orientation = random_quaternion();
	crystal_set_cell(cr, cell_rotate(qargs->cell, orientation));

	snprintf(wargs->image.filename, 255, "dummy.h5");
	reflections = find_intersections(&wargs->image, cr);
	crystal_set_reflections(cr, reflections);

	for ( i=0; i<NBINS; i++ ) {
		wargs->n_ref[i] = 0;
		wargs->p_hist[i] = 0.0;
		wargs->p_max[i] = 0.0;
	}

	calculate_partials(cr, qargs->full,
	                   qargs->sym, qargs->random_intensities,
	                   &qargs->full_lock,
	                   wargs->n_ref, wargs->p_hist, wargs->p_max,
	                   qargs->max_q, qargs->full_stddev,
	                   qargs->noise_stddev);

	/* Give a slightly incorrect cell in the stream */
	mess_up_cell(cr, qargs->cnoise);

	image_add_crystal(&wargs->image, cr);
}


static void finalise_job(void *vqargs, void *vwargs)
{
	struct worker_args *wargs = vwargs;
	struct queue_args *qargs = vqargs;
	int i;

	write_chunk(qargs->stream, &wargs->image, NULL, 0, 1);

	for ( i=0; i<NBINS; i++ ) {
		qargs->n_ref[i] += wargs->n_ref[i];
		qargs->p_hist[i] += wargs->p_hist[i];
		if ( wargs->p_max[i] > qargs->p_max[i] ) {
			qargs->p_max[i] = wargs->p_max[i];
		}
	}

	qargs->n_done++;
	progress_bar(qargs->n_done, qargs->n_to_do, "Simulating");

	crystal_free(wargs->crystal);
	free(wargs);
}


int main(int argc, char *argv[])
{
	int c;
	char *input_file = NULL;
	char *output_file = NULL;
	char *beamfile = NULL;
	char *geomfile = NULL;
	char *cellfile = NULL;
	struct detector *det = NULL;
	struct beam_params *beam = NULL;
	RefList *full = NULL;
	char *sym_str = NULL;
	SymOpList *sym;
	UnitCell *cell = NULL;
	Stream *stream;
	int n = 2;
	int random_intensities = 0;
	char *save_file = NULL;
	struct queue_args qargs;
	struct image image;
	int n_threads = 1;
	double cnoise = 0.0;
	char *rval;
	int i;
	FILE *fh;
	char *phist_file = NULL;
	double osf_stddev = 2.0;
	double full_stddev = 1000.0;
	double noise_stddev = 20.0;

	/* Long options */
	const struct option longopts[] = {
		{"help",               0, NULL,               'h'},
		{"output",             1, NULL,               'o'},
		{"input",              1, NULL,               'i'},
		{"beam",               1, NULL,               'b'},
		{"pdb",                1, NULL,               'p'},
		{"geometry",           1, NULL,               'g'},
		{"symmetry",           1, NULL,               'y'},
		{"save-random",        1, NULL,               'r'},
		{"cnoise",             1, NULL,               'c'},

		{"pgraph",             1, NULL,                2},
		{"osf-stddev",         1, NULL,                3},
		{"full-stddev",        1, NULL,                4},
		{"noise-stddev",       1, NULL,                5},

		{0, 0, NULL, 0}
	};

	/* Short options */
	while ((c = getopt_long(argc, argv, "hi:o:b:p:g:y:n:r:j:c:",
	                        longopts, NULL)) != -1)
	{
		switch (c) {

			case 'h' :
			show_help(argv[0]);
			return 0;

			case 'o' :
			output_file = strdup(optarg);
			break;

			case 'i' :
			input_file = strdup(optarg);
			break;

			case 'b' :
			beamfile = strdup(optarg);
			break;

			case 'p' :
			cellfile = strdup(optarg);
			break;

			case 'g' :
			geomfile = strdup(optarg);
			break;

			case 'y' :
			sym_str = strdup(optarg);
			break;

			case 'n' :
			n = atoi(optarg);
			break;

			case 'r' :
			save_file = strdup(optarg);
			break;

			case 'j' :
			n_threads = atoi(optarg);
			break;

			case 'c' :
			cnoise = strtod(optarg, &rval);
			if ( *rval != '\0' ) {
				ERROR("Invalid cell noise value.\n");
				return 1;
			}
			break;

			case 2 :
			phist_file = strdup(optarg);
			break;

			case 3 :
			osf_stddev = strtod(optarg, &rval);
			if ( *rval != '\0' ) {
				ERROR("Invalid OSF standard deviation.\n");
				return 1;
			}
			if ( osf_stddev < 0.0 ) {
				ERROR("Invalid OSF standard deviation.");
				ERROR(" (must be positive).\n");
				return 1;
			}
			break;

			case 4 :
			full_stddev = strtod(optarg, &rval);
			if ( *rval != '\0' ) {
				ERROR("Invalid full standard deviation.\n");
				return 1;
			}
			if ( full_stddev < 0.0 ) {
				ERROR("Invalid full standard deviation.");
				ERROR(" (must be positive).\n");
				return 1;
			}
			break;

			case 5 :
			noise_stddev = strtod(optarg, &rval);
			if ( *rval != '\0' ) {
				ERROR("Invalid noise standard deviation.\n");
				return 1;
			}
			if ( noise_stddev < 0.0 ) {
				ERROR("Invalid noise standard deviation.");
				ERROR(" (must be positive).\n");
				return 1;
			}
			break;

			case 0 :
			break;

			case '?' :
			break;

			default :
			ERROR("Unhandled option '%c'\n", c);
			break;

		}
	}

	if ( n_threads < 1 ) {
		ERROR("Invalid number of threads.\n");
		return 1;
	}

	/* Load beam */
	if ( beamfile == NULL ) {
		ERROR("You need to provide a beam parameters file.\n");
		return 1;
	}
	beam = get_beam_parameters(beamfile);
	if ( beam == NULL ) {
		ERROR("Failed to load beam parameters from '%s'\n", beamfile);
		return 1;
	}
	free(beamfile);

	/* Load cell */
	if ( cellfile == NULL ) {
		ERROR("You need to give a PDB file with the unit cell.\n");
		return 1;
	}
	cell = load_cell_from_pdb(cellfile);
	if ( cell == NULL ) {
		ERROR("Failed to get cell from '%s'\n", cellfile);
		return 1;
	}
	free(cellfile);

	if ( !cell_is_sensible(cell) ) {
		ERROR("Invalid unit cell parameters:\n");
		cell_print(cell);
		return 1;
	}

	/* Load geometry */
	if ( geomfile == NULL ) {
		ERROR("You need to give a geometry file.\n");
		return 1;
	}
	det = get_detector_geometry(geomfile);
	if ( det == NULL ) {
		ERROR("Failed to read geometry from '%s'\n", geomfile);
		return 1;
	}
	free(geomfile);

	if ( sym_str == NULL ) sym_str = strdup("1");
	sym = get_pointgroup(sym_str);
	free(sym_str);

	if ( save_file == NULL ) save_file = strdup("partial_sim.hkl");

	/* Load (full) reflections */
	if ( input_file != NULL ) {

		full = read_reflections(input_file);
		if ( full == NULL ) {
			ERROR("Failed to read reflections from '%s'\n",
			      input_file);
			return 1;
		}
		free(input_file);
		if ( check_list_symmetry(full, sym) ) {
			ERROR("The input reflection list does not appear to"
			      " have symmetry %s\n", symmetry_name(sym));
			return 1;
		}

	} else {
		random_intensities = 1;
	}

	if ( n < 1 ) {
		ERROR("Number of patterns must be at least 1.\n");
		return 1;
	}

	if ( output_file == NULL ) {
		ERROR("You must give a filename for the output.\n");
		return 1;
	}
	stream = open_stream_for_write(output_file);
	if ( stream == NULL ) {
		ERROR("Couldn't open output file '%s'\n", output_file);
		return 1;
	}
	free(output_file);

	image.det = det;
	image.width = det->max_fs;
	image.height = det->max_ss;

	image.lambda = ph_en_to_lambda(eV_to_J(beam->photon_energy));
	image.div = beam->divergence;
	image.bw = beam->bandwidth;
	image.beam = beam;
	image.filename = malloc(256);
	image.copyme = NULL;
	image.crystals = NULL;
	image.n_crystals = 0;
	image.indexed_by = INDEXING_SIMULATION;
	image.num_peaks = 0;
	image.num_saturated_peaks = 0;

	if ( random_intensities ) {
		full = reflist_new();
	}

	qargs.full = full;
	pthread_rwlock_init(&qargs.full_lock, NULL);
	qargs.n_to_do = n;
	qargs.n_done = 0;
	qargs.n_started = 0;
	qargs.sym = sym;
	qargs.random_intensities = random_intensities;
	qargs.cell = cell;
	qargs.template_image = &image;
	qargs.stream = stream;
	qargs.cnoise = cnoise;
	qargs.osf_stddev = osf_stddev;
	qargs.full_stddev = full_stddev;
	qargs.noise_stddev = noise_stddev;
	qargs.max_q = largest_q(&image);

	for ( i=0; i<NBINS; i++ ) {
		qargs.n_ref[i] = 0;
		qargs.p_hist[i] = 0.0;
		qargs.p_max[i] = 0.0;
	}

	run_threads(n_threads, run_job, create_job, finalise_job,
	            &qargs, n, 0, 0, 0);

	if ( random_intensities ) {
		STATUS("Writing full intensities to %s\n", save_file);
		write_reflist(save_file, full);
	}

	if ( phist_file != NULL ) {

		fh = fopen(phist_file, "w");

		if ( fh != NULL ) {

			for ( i=0; i<NBINS; i++ ) {

				double rcen;

				rcen = i/(double)NBINS*qargs.max_q
					  + qargs.max_q/(2.0*NBINS);
				fprintf(fh, "%.2f %7li %.3f %.3f\n", rcen/1.0e9,
					qargs.n_ref[i],
					qargs.p_hist[i]/qargs.n_ref[i],
					qargs.p_max[i]);

			}

			fclose(fh);

		} else {
			ERROR("Failed to open file '%s' for writing.\n",
			      phist_file);
		}

	}

	pthread_rwlock_destroy(&qargs.full_lock);
	close_stream(stream);
	cell_free(cell);
	free_detector_geometry(det);
	free(beam);
	free_symoplist(sym);
	reflist_free(full);
	free(image.filename);

	return 0;
}

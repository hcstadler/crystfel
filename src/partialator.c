/*
 * partialator.c
 *
 * Scaling and post refinement for coherent nanocrystallography
 *
 * (c) 2006-2011 Thomas White <taw@physics.org>
 *
 * Part of CrystFEL - crystallography with a FEL
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
#include <gsl/gsl_errno.h>

#include "utils.h"
#include "hdf5-file.h"
#include "symmetry.h"
#include "stream.h"
#include "geometry.h"
#include "peaks.h"
#include "thread-pool.h"
#include "beam-parameters.h"
#include "post-refinement.h"
#include "hrs-scaling.h"
#include "reflist.h"
#include "reflist-utils.h"
#include "scaling-report.h"


static void show_help(const char *s)
{
	printf("Syntax: %s [options]\n\n", s);
	printf(
"Scaling and post refinement for coherent nanocrystallography.\n"
"\n"
"  -h, --help                 Display this help message.\n"
"\n"
"  -i, --input=<filename>     Specify the name of the input 'stream'.\n"
"                              (must be a file, not e.g. stdin)\n"
"  -o, --output=<filename>    Output filename.  Default: facetron.hkl.\n"
"  -g. --geometry=<file>      Get detector geometry from file.\n"
"  -b, --beam=<file>          Get beam parameters from file, which provides\n"
"                              initial values for parameters, and nominal\n"
"                              wavelengths if no per-shot value is found in \n"
"                              an HDF5 file.\n"
"  -y, --symmetry=<sym>       Merge according to symmetry <sym>.\n"
"  -n, --iterations=<n>       Run <n> cycles of scaling and post-refinement.\n"
"      --reference=<file>     Refine images against reflections in <file>,\n"
"                              instead of taking the mean of the intensity\n"
"                              estimates.\n"
"\n"
"  -j <n>                     Run <n> analyses in parallel.\n");
}


struct refine_args
{
	RefList *full;
	struct image *image;
	FILE *graph;
	FILE *pgraph;
};


struct queue_args
{
	int n;
	int n_done;
	int n_total_patterns;
	struct image *images;
	struct refine_args task_defaults;
};


static void refine_image(void *task, int id)
{
	struct refine_args *pargs = task;
	struct image *image = pargs->image;
	image->id = id;

	pr_refine(image, pargs->full);
}


static void *get_image(void *vqargs)
{
	struct refine_args *task;
	struct queue_args *qargs = vqargs;

	task = malloc(sizeof(struct refine_args));
	memcpy(task, &qargs->task_defaults, sizeof(struct refine_args));

	task->image = &qargs->images[qargs->n];

	qargs->n++;

	return task;
}


static void done_image(void *vqargs, void *task)
{
	struct queue_args *qargs = vqargs;

	qargs->n_done++;

	progress_bar(qargs->n_done, qargs->n_total_patterns, "Refining");
	free(task);
}


static void refine_all(struct image *images, int n_total_patterns,
                       struct detector *det,
                       RefList *full, int nthreads,
                       FILE *graph, FILE *pgraph)
{
	struct refine_args task_defaults;
	struct queue_args qargs;

	task_defaults.full = full;
	task_defaults.image = NULL;
	task_defaults.graph = graph;
	task_defaults.pgraph = pgraph;

	qargs.task_defaults = task_defaults;
	qargs.n = 0;
	qargs.n_done = 0;
	qargs.n_total_patterns = n_total_patterns;
	qargs.images = images;

	/* Don't have threads which are doing nothing */
	if ( n_total_patterns < nthreads ) nthreads = n_total_patterns;

	run_threads(nthreads, refine_image, get_image, done_image,
	            &qargs, n_total_patterns, 0, 0, 0);
}


/* Decide which reflections can be scaled */
static int select_scalable_reflections(RefList *list, RefList *reference)
{
	Reflection *refl;
	RefListIterator *iter;
	int nobs = 0;

	for ( refl = first_refl(list, &iter);
	      refl != NULL;
	      refl = next_refl(refl, iter) ) {

		int sc = 1;
		double v;

		if ( get_partiality(refl) < 0.1 ) sc = 0;
		v = fabs(get_intensity(refl));
		if ( v < 0.1 ) sc = 0;

		/* If we are scaling against a reference set, we additionally
		 * require that this reflection is in the reference list. */
		if ( reference != NULL ) {
			signed int h, k, l;
			get_indices(refl, &h, &k, &l);
			if ( find_refl(reference, h, k, l) == NULL ) sc = 0;
		}

		set_scalable(refl, sc);

		if ( sc ) nobs++;
	}

	return nobs;
}


static void select_reflections_for_refinement(struct image *images, int n,
                                              RefList *full, int have_reference)
{
	int i;

	for ( i=0; i<n; i++ ) {

		Reflection *refl;
		RefListIterator *iter;
		int n_acc = 0;
		int n_nomatch = 0;
		int n_noscale = 0;
		int n_fewmatch = 0;
		int n_ref = 0;

		for ( refl = first_refl(images[i].reflections, &iter);
		      refl != NULL;
		      refl = next_refl(refl, iter) )
		{
			signed int h, k, l;
			int sc;

			n_ref++;

			/* We require that the reflection itself is scalable
			 * (i.e. sensible partiality and intensity) and that
			 * the "full" estimate of this reflection is made from
			 * at least two parts. */
			get_indices(refl, &h, &k, &l);
			sc = get_scalable(refl);
			if ( !sc ) {

				n_noscale++;
				set_refinable(refl, 0);

			} else {

				Reflection *f = find_refl(full, h, k, l);

				if ( f != NULL ) {

					int r = get_redundancy(f);
					if ( (r >= 2) || have_reference ) {
						set_refinable(refl, 1);
						n_acc++;
					} else {
						n_fewmatch++;
					}

				} else {
					n_nomatch++;
					set_refinable(refl, 0);
				}

			}
		}

		STATUS("Image %4i: %i guide reflections accepted "
		       "(%i not scalable, %i few matches, %i total)\n",
		       i, n_acc, n_noscale, n_fewmatch, n_ref);

		/* This would be a silly situation, since there must be a match
		 * if THIS pattern has a scalable part of the reflection! */
		assert(n_nomatch == 0);

	}
}


int main(int argc, char *argv[])
{
	int c;
	char *infile = NULL;
	char *outfile = NULL;
	char *geomfile = NULL;
	char *sym = NULL;
	FILE *fh;
	int nthreads = 1;
	struct detector *det;
	int i;
	int n_total_patterns;
	struct image *images;
	int n_iter = 10;
	struct beam_params *beam = NULL;
	RefList *full;
	int n_found = 0;
	int n_expected = 0;
	int n_notfound = 0;
	int n_usable_patterns = 0;
	int nobs;
	char *reference_file = NULL;
	RefList *reference = NULL;
	int n_dud;
	int have_reference = 0;

	/* Long options */
	const struct option longopts[] = {
		{"help",               0, NULL,               'h'},
		{"input",              1, NULL,               'i'},
		{"output",             1, NULL,               'o'},
		{"geometry",           1, NULL,               'g'},
		{"beam",               1, NULL,               'b'},
		{"symmetry",           1, NULL,               'y'},
		{"iterations",         1, NULL,               'n'},
		{"reference",          1, NULL,                1},
		{0, 0, NULL, 0}
	};

	/* Short options */
	while ((c = getopt_long(argc, argv, "hi:g:x:j:y:o:b:",
	                        longopts, NULL)) != -1)
	{

		switch (c) {
		case 'h' :
			show_help(argv[0]);
			return 0;

		case 'i' :
			infile = strdup(optarg);
			break;

		case 'g' :
			geomfile = strdup(optarg);
			break;

		case 'j' :
			nthreads = atoi(optarg);
			break;

		case 'y' :
			sym = strdup(optarg);
			break;

		case 'o' :
			outfile = strdup(optarg);
			break;

		case 'n' :
			n_iter = atoi(optarg);
			break;

		case 'b' :
			beam = get_beam_parameters(optarg);
			if ( beam == NULL ) {
				ERROR("Failed to load beam parameters"
				      " from '%s'\n", optarg);
				return 1;
			}
			break;

		case 1 :
			reference_file = strdup(optarg);
			break;

		case 0 :
			break;

		default :
			return 1;
		}

	}

	/* Sanitise input filename and open */
	if ( infile == NULL ) {
		infile = strdup("-");
	}
	if ( strcmp(infile, "-") == 0 ) {
		fh = stdin;
	} else {
		fh = fopen(infile, "r");
	}
	if ( fh == NULL ) {
		ERROR("Failed to open input file '%s'\n", infile);
		return 1;
	}

	/* Sanitise output filename */
	if ( outfile == NULL ) {
		outfile = strdup("partialator.hkl");
	}

	if ( sym == NULL ) sym = strdup("1");

	/* Get detector geometry */
	det = get_detector_geometry(geomfile);
	if ( det == NULL ) {
		ERROR("Failed to read detector geometry from '%s'\n", geomfile);
		return 1;
	}
	free(geomfile);

	if ( beam == NULL ) {
		ERROR("You must provide a beam parameters file.\n");
		return 1;
	}

	if ( reference_file != NULL ) {

		RefList *list;

		list = read_reflections(reference_file);
		free(reference_file);
		if ( list == NULL ) return 1;
		reference = asymmetric_indices(list, sym);
		reflist_free(list);
		have_reference = 1;

	}

	n_total_patterns = count_patterns(fh);
	if ( n_total_patterns == 0 ) {
		ERROR("No patterns to process.\n");
		return 1;
	}
	STATUS("There are %i patterns to process\n", n_total_patterns);

	gsl_set_error_handler_off();

	images = malloc(n_total_patterns * sizeof(struct image));
	if ( images == NULL ) {
		ERROR("Couldn't allocate memory for images.\n");
		return 1;
	}

	/* Fill in what we know about the images so far */
	rewind(fh);
	nobs = 0;
	for ( i=0; i<n_total_patterns; i++ ) {

		RefList *as;
		struct image *cur;

		cur = &images[n_usable_patterns];

		cur->det = det;

		if ( read_chunk(fh, cur) != 0 ) {
			/* Should not happen, because we counted the patterns
			 * earlier. */
			ERROR("Failed to read chunk from the input stream.\n");
			return 1;
		}

		/* Won't be needing this, if it exists */
		image_feature_list_free(cur->features);
		cur->features = NULL;

		/* "n_usable_patterns" will not be incremented in this case */
		if ( cur->indexed_cell == NULL ) continue;

		/* Fill in initial estimates of stuff */
		cur->div = beam->divergence;
		cur->bw = beam->bandwidth;
		cur->width = det->max_fs;
		cur->height = det->max_ss;
		cur->osf = 1.0;
		cur->profile_radius = 0.003e9;
		cur->pr_dud = 0;

		/* Muppet proofing */
		cur->data = NULL;
		cur->flags = NULL;
		cur->beam = NULL;

		/* This is the raw list of reflections */
		as = asymmetric_indices(cur->reflections, sym);
		reflist_free(cur->reflections);
		cur->reflections = as;

		update_partialities(cur, &n_expected, &n_found, &n_notfound);

		nobs += select_scalable_reflections(cur->reflections,
		                                    reference);

		progress_bar(i, n_total_patterns-1, "Loading pattern data");
		n_usable_patterns++;

	}
	fclose(fh);
	STATUS("Found %5.2f%% of the expected peaks (missed %i of %i).\n",
	       100.0 * (double)n_found / n_expected, n_notfound, n_expected);

	/* Make initial estimates */
	STATUS("Performing initial scaling.\n");
	full = scale_intensities(images, n_usable_patterns, reference);

	select_reflections_for_refinement(images, n_usable_patterns, full,
	                                  have_reference);

	/* Iterate */
	for ( i=0; i<n_iter; i++ ) {

		FILE *fhg;
		FILE *fhp;
		char filename[1024];
		int j;
		RefList *comp;

		STATUS("Post refinement cycle %i of %i\n", i+1, n_iter);

		snprintf(filename, 1023, "p-iteration-%i.dat", i+1);
		fhg = fopen(filename, "w");
		if ( fhg == NULL ) {
			ERROR("Failed to open '%s'\n", filename);
			/* Nothing will be written later */
		}

		snprintf(filename, 1023, "g-iteration-%i.dat", i+1);
		fhp = fopen(filename, "w");
		if ( fhp == NULL ) {
			ERROR("Failed to open '%s'\n", filename);
			/* Nothing will be written later */
		}

		if ( reference == NULL ) {
			comp = full;
		} else {
			comp = reference;
		}

		/* Refine the geometry of all patterns to get the best fit */
		refine_all(images, n_usable_patterns, det,
		           comp, nthreads, fhg, fhp);

		nobs = 0;
		for ( j=0; j<n_usable_patterns; j++ ) {

			struct image *cur = &images[j];

			update_partialities(cur, &n_expected,
			                    &n_found, &n_notfound);

			nobs += select_scalable_reflections(cur->reflections,
			                                    reference);

		}

		/* Re-estimate all the full intensities */
		reflist_free(full);
		full = scale_intensities(images, n_usable_patterns,
		                         reference);

		select_reflections_for_refinement(images, n_usable_patterns,
		                                  full, have_reference);

		fclose(fhg);
		fclose(fhp);

	}

	STATUS("Final scale factors:\n");
	n_dud = 0;
	for ( i=0; i<n_usable_patterns; i++ ) {
		if ( images[i].pr_dud ) n_dud++;
		STATUS("%4i : %5.2f\n", i, images[i].osf);
	}
	STATUS("%i images could not be refined on the last cycle.\n", n_dud);

	/* Output results */
	write_reflist(outfile, full, images[0].indexed_cell);

	scaling_report("scaling-report.pdf", images, n_usable_patterns, infile);

	/* Clean up */
	for ( i=0; i<n_usable_patterns; i++ ) {
		reflist_free(images[i].reflections);
	}
	reflist_free(full);
	free(sym);
	free(outfile);
	free_detector_geometry(det);
	free(beam);
	if ( reference != NULL ) {
		reflist_free(reference);
	}
	for ( i=0; i<n_usable_patterns; i++ ) {
		cell_free(images[i].indexed_cell);
		free(images[i].filename);
	}
	free(images);
	free(infile);

	return 0;
}

/*
 * pr_gradient_check.c
 *
 * Check gradients for post refinement
 *
 * Copyright © 2012 Thomas White <taw@physics.org>
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

#include <image.h>
#include <cell.h>
#include <geometry.h>
#include <reflist.h>
#include "../src/post-refinement.h"


static void scan_partialities(RefList *reflections, RefList *compare,
                              int *valid, long double *vals[3], int idx)
{
	int i;
	Reflection *refl;
	RefListIterator *iter;

	i = 0;
	for ( refl = first_refl(reflections, &iter);
	      refl != NULL;
	      refl = next_refl(refl, iter) )
	{
		signed int h, k, l;
		Reflection *refl2;
		double r1, r2, p;
		int clamp_low, clamp_high;

		get_indices(refl, &h, &k, &l);
		refl2 = find_refl(compare, h, k, l);
		if ( refl2 == NULL ) {
			valid[i] = 0;
			i++;
			continue;
		}

		get_partial(refl2, &r1, &r2, &p, &clamp_low, &clamp_high);
		if ( clamp_low && clamp_high ) {
			if ( !within_tolerance(p, 1.0, 0.001) ) {

				signed int h, k, l;

				get_indices(refl, &h, &k, &l);

				ERROR("%3i %3i %3i - double clamped but"
				      " partiality not close to 1.0 (%5.2f)\n",
				      h, k, l, p);

			}
			valid[i] = 0;
		}

		vals[idx][i] = p;
		i++;
	}
}


static UnitCell *new_shifted_cell(UnitCell *input, int k, double shift)
{
	UnitCell *cell;
	double asx, asy, asz;
	double bsx, bsy, bsz;
	double csx, csy, csz;

	cell = cell_new();
	cell_get_reciprocal(input, &asx, &asy, &asz, &bsx, &bsy, &bsz,
	                    &csx, &csy, &csz);
	switch ( k )
	{
		case REF_ASX :  asx += shift;  break;
		case REF_ASY :  asy += shift;  break;
		case REF_ASZ :  asz += shift;  break;
		case REF_BSX :  bsx += shift;  break;
		case REF_BSY :  bsy += shift;  break;
		case REF_BSZ :  bsz += shift;  break;
		case REF_CSX :  csx += shift;  break;
		case REF_CSY :  csy += shift;  break;
		case REF_CSZ :  csz += shift;  break;
	}
	cell_set_reciprocal(cell, asx, asy, asz, bsx, bsy, bsz, csx, csy, csz);

	return cell;
}


static void shift_parameter(struct image *image, int k, double shift)
{
	switch ( k )
	{
		case REF_DIV : image->div += shift;  break;
	}
}


static void calc_either_side(struct image *image, double incr_val,
                             int *valid, long double *vals[3], int refine)
{
	RefList *compare;
	UnitCell *cell;

	if ( (refine != REF_DIV) && (refine != REF_R) ) {

		cell = new_shifted_cell(image->indexed_cell, refine, -incr_val);
		compare = find_intersections(image, cell);
		scan_partialities(image->reflections, compare, valid, vals, 0);
		cell_free(cell);
		reflist_free(compare);

		cell = new_shifted_cell(image->indexed_cell, refine, +incr_val);
		compare = find_intersections(image, cell);
		scan_partialities(image->reflections, compare, valid, vals, 2);
		cell_free(cell);
		reflist_free(compare);

	} else {

		struct image im_moved;

		im_moved = *image;
		shift_parameter(&im_moved, refine, -incr_val);
		compare = find_intersections(&im_moved, im_moved.indexed_cell);
		scan_partialities(im_moved.reflections, compare,
		                  valid, vals, 0);
		reflist_free(compare);

		im_moved = *image;
		shift_parameter(&im_moved, refine, +incr_val);
		compare = find_intersections(&im_moved, im_moved.indexed_cell);
		scan_partialities(im_moved.reflections, compare,
		                  valid, vals, 2);
		reflist_free(compare);

	}
}



static int test_gradients(struct image *image, double incr_val, int refine,
                          const char *str)
{
	Reflection *refl;
	RefListIterator *iter;
	long double *vals[3];
	int i;
	int *valid;
	int nref;
	int n_acc, n_valid;
	//FILE *fh;

	image->reflections = find_intersections(image, image->indexed_cell);

	nref = num_reflections(image->reflections);
	if ( nref < 10 ) {
		ERROR("Too few reflections found.  Failing test by default.\n");
		return -1;
	}

	vals[0] = malloc(nref*sizeof(long double));
	vals[1] = malloc(nref*sizeof(long double));
	vals[2] = malloc(nref*sizeof(long double));
	if ( (vals[0] == NULL) || (vals[1] == NULL) || (vals[2] == NULL) ) {
		ERROR("Couldn't allocate memory.\n");
		return -1;
	}

	valid = malloc(nref*sizeof(int));
	if ( valid == NULL ) {
		ERROR("Couldn't allocate memory.\n");
		return -1;
	}
	for ( i=0; i<nref; i++ ) valid[i] = 1;

	scan_partialities(image->reflections, image->reflections,
	                  valid, vals, 1);

	calc_either_side(image, incr_val, valid, vals, refine);

	//fh = fopen("wrongness.dat", "a");

	n_valid = nref;  n_acc = 0;
	i = 0;
	for ( refl = first_refl(image->reflections, &iter);
	      refl != NULL;
	      refl = next_refl(refl, iter) )
	{

		long double grad1, grad2, grad;
		double cgrad;
		signed int h, k, l;

		get_indices(refl, &h, &k, &l);

		if ( !valid[i] ) {
			n_valid--;
		} else {

			double r1, r2, p;
			int cl, ch;
			double tt, dstar;

			dstar = 2.0 * resolution(image->indexed_cell, h, k, l),
			tt = 2.0*asin(image->lambda/(2.0/dstar));

			grad1 = (vals[1][i] - vals[0][i]) / incr_val;
			grad2 = (vals[2][i] - vals[1][i]) / incr_val;
			grad = (grad1 + grad2) / 2.0;

			cgrad = gradient(image, refine, refl,
			                 image->profile_radius);

			get_partial(refl, &r1, &r2, &p, &cl, &ch);

			if ( (fabs(cgrad) > 5e-8) &&
			     !within_tolerance(grad, cgrad, 10.0) )
			{

				STATUS("!- %s %3i %3i %3i"
				       " %10.2Le %10.2e ratio = %5.2Lf"
				       " %10.2e %10.2e\n",
				       str, h, k, l, grad, cgrad, cgrad/grad,
				       r1, r2);

			} else {

				//STATUS("OK %s %3i %3i %3i"
				//       " %10.2Le %10.2e ratio = %5.2Lf"
				//       " %10.2e %10.2e\n",
				//       str, h, k, l, grad, cgrad, cgrad/grad,
				//       r1, r2);

				n_acc++;

			}

			//fprintf(fh, "%e %f\n",
			        //resolution(image->indexed_cell, h, k, l),
			        //rad2deg(tt),
			//        cgrad,
			//        fabs((grad-cgrad)/grad));

		}

		i++;

	}

	STATUS("%s: %i out of %i valid gradients were accurate.\n",
	       str, n_acc, n_valid);
	//fclose(fh);

	if ( n_acc != n_valid ) return 1;

	return 0;
}


int main(int argc, char *argv[])
{
	struct image image;
	const double incr_frac = 1.0/1000000.0;
	double incr_val;
	double ax, ay, az;
	double bx, by, bz;
	double cx, cy, cz;
	UnitCell *cell;
	struct quaternion orientation;
	int i;
	int val;

	image.width = 1024;
	image.height = 1024;
	image.det = simple_geometry(&image);
	image.det->panels[0].res = 13333.3;
	image.det->panels[0].clen = 80e-3;
	image.det->panels[0].coffset = 0.0;

	image.lambda = ph_en_to_lambda(eV_to_J(8000.0));
	image.div = 1e-3;
	image.bw = 0.01;
	image.m = 0.0;
	image.profile_radius = 0.005e9;
	image.filename = malloc(256);

	cell = cell_new_from_parameters(10.0e-9, 10.0e-9, 10.0e-9,
	                                deg2rad(90.0),
	                                deg2rad(90.0),
	                                deg2rad(90.0));

	val = 0;

	for ( i=0; i<1; i++ ) {

		orientation = random_quaternion();
		image.indexed_cell = cell_rotate(cell, orientation);

		cell_get_reciprocal(image.indexed_cell,
			            &ax, &ay, &az, &bx, &by,
			            &bz, &cx, &cy, &cz);

		incr_val = incr_frac * image.div;
		val += test_gradients(&image, incr_val, REF_DIV, "div");

		incr_val = incr_frac * ax;
		val += test_gradients(&image, incr_val, REF_ASX, "ax*");
		incr_val = incr_frac * ay;
		val += test_gradients(&image, incr_val, REF_ASY, "ay*");
		incr_val = incr_frac * az;
		val += test_gradients(&image, incr_val, REF_ASZ, "az*");

		incr_val = incr_frac * bx;
		val += test_gradients(&image, incr_val, REF_BSX, "bx*");
		incr_val = incr_frac * by;
		val += test_gradients(&image, incr_val, REF_BSY, "by*");
		incr_val = incr_frac * bz;
		val += test_gradients(&image, incr_val, REF_BSZ, "bz*");

		incr_val = incr_frac * cx;
		val += test_gradients(&image, incr_val, REF_CSX, "cx*");
		incr_val = incr_frac * cy;
		val += test_gradients(&image, incr_val, REF_CSY, "cy*");
		incr_val = incr_frac * cz;
		val += test_gradients(&image, incr_val, REF_CSZ, "cz*");

	}

	STATUS("Returning 0 by default: gradients only needed for experimental"
	       " features of CrystFEL.\n");
	return 0;
}

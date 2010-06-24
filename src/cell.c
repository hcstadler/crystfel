/*
 * cell.c
 *
 * Unit Cell Calculations
 *
 * (c) 2006-2010 Thomas White <taw@physics.org>
 *
 * Part of CrystFEL - crystallography with a FEL
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <math.h>
#include <stdlib.h>
#include <stdio.h>
#include <gsl/gsl_matrix.h>
#include <gsl/gsl_blas.h>
#include <gsl/gsl_linalg.h>

#include "cell.h"
#include "utils.h"
#include "image.h"


/* Weighting factor of lengths relative to angles */
#define LWEIGHT (10.0e-9)

typedef enum {
	CELL_REP_CRYST,
	CELL_REP_CART,
	CELL_REP_RECIP
} CellRepresentation;

struct _unitcell {

	CellRepresentation rep;

	/* Crystallographic representation */
	double a;	/* m */
	double b;	/* m */
	double c;	/* m */
	double alpha;	/* Radians */
	double beta;	/* Radians */
	double gamma;	/* Radians */

	/* Cartesian representation */
	double ax;	double bx;	double cx;
	double ay;	double by;	double cy;
	double az;	double bz;	double cz;

	/* Cartesian representation of reciprocal axes */
	double axs;	double bxs;	double cxs;
	double ays;	double bys;	double cys;
	double azs;	double bzs;	double czs;

};


/************************** Setters and Constructors **************************/

UnitCell *cell_new()
{
	UnitCell *cell;

	cell = malloc(sizeof(UnitCell));
	if ( cell == NULL ) return NULL;

	cell->a = 1.0;
	cell->b = 1.0;
	cell->c = 1.0;
	cell->alpha = M_PI_2;
	cell->beta = M_PI_2;
	cell->gamma = M_PI_2;

	cell->rep = CELL_REP_CRYST;

	return cell;
}


void cell_set_parameters(UnitCell *cell, double a, double b, double c,
                         double alpha, double beta, double gamma)
{
	if ( cell == NULL ) return;

	cell->a = a;
	cell->b = b;
	cell->c = c;
	cell->alpha = alpha;
	cell->beta = beta;
	cell->gamma = gamma;

	cell->rep = CELL_REP_CRYST;
}


void cell_set_cartesian(UnitCell *cell,
			double ax, double ay, double az,
			double bx, double by, double bz,
			double cx, double cy, double cz)
{
	if ( cell == NULL ) return;

	cell->ax = ax;  cell->ay = ay;  cell->az = az;
	cell->bx = bx;  cell->by = by;  cell->bz = bz;
	cell->cx = cx;  cell->cy = cy;  cell->cz = cz;

	cell->rep = CELL_REP_CART;
}


void cell_set_cartesian_a(UnitCell *cell, double ax, double ay, double az)
{
	if ( cell == NULL ) return;
	cell->ax = ax;  cell->ay = ay;  cell->az = az;
	cell->rep = CELL_REP_CART;
}


void cell_set_cartesian_b(UnitCell *cell, double bx, double by, double bz)
{
	if ( cell == NULL ) return;
	cell->bx = bx;  cell->by = by;  cell->bz = bz;
	cell->rep = CELL_REP_CART;
}


void cell_set_cartesian_c(UnitCell *cell, double cx, double cy, double cz)
{
	if ( cell == NULL ) return;
	cell->cx = cx;  cell->cy = cy;  cell->cz = cz;
	cell->rep = CELL_REP_CART;
}


UnitCell *cell_new_from_parameters(double a, double b, double c,
                                   double alpha, double beta, double gamma)
{
	UnitCell *cell;

	cell = cell_new();
	if ( cell == NULL ) return NULL;

	cell_set_parameters(cell, a, b, c, alpha, beta, gamma);

	return cell;
}


static UnitCell *cell_new_from_axes(struct rvec as, struct rvec bs,
                                    struct rvec cs)
{
	UnitCell *cell;

	cell = cell_new();
	if ( cell == NULL ) return NULL;

	cell->axs = as.u;  cell->ays = as.v;  cell->azs = as.w;
	cell->bxs = bs.u;  cell->bys = bs.v;  cell->bzs = bs.w;
	cell->cxs = cs.u;  cell->cys = cs.v;  cell->czs = cs.w;

	cell->rep = CELL_REP_RECIP;

	return cell;
}


UnitCell *cell_new_from_cell(UnitCell *orig)
{
	UnitCell *new;

	new = malloc(sizeof(UnitCell));

	*new = *orig;

	return new;
}


/************************* Getter helper functions ****************************/

static int cell_crystallographic_to_cartesian(UnitCell *cell,
                                             double *ax, double *ay, double *az,
                                             double *bx, double *by, double *bz,
                                             double *cx, double *cy, double *cz)
{
	double tmp, V, cosalphastar, cstar;

	/* Firstly: Get a in terms of x, y and z
	 * +a (cryst) is defined to lie along +x (cart) */
	*ax = cell->a;
	*ay = 0.0;
	*az = 0.0;

	/* b in terms of x, y and z
	 * b (cryst) is defined to lie in the xy (cart) plane */
	*bx = cell->b*cos(cell->gamma);
	*by = cell->b*sin(cell->gamma);
	*bz = 0.0;

	tmp = cos(cell->alpha)*cos(cell->alpha)
		+ cos(cell->beta)*cos(cell->beta)
		+ cos(cell->gamma)*cos(cell->gamma)
		- 2.0*cos(cell->alpha)*cos(cell->beta)*cos(cell->gamma);
	V = cell->a * cell->b * cell->c * sqrt(1.0 - tmp);

	cosalphastar = cos(cell->beta)*cos(cell->gamma) - cos(cell->alpha);
	cosalphastar /= sin(cell->beta)*sin(cell->gamma);

	cstar = (cell->a * cell->b * sin(cell->gamma))/V;

	/* c in terms of x, y and z */
	*cx = cell->c*cos(cell->beta);
	*cy = -cell->c*sin(cell->beta)*cosalphastar;
	*cz = 1.0/cstar;

	return 0;
}


/* Why yes, I do enjoy long argument lists...! */
static int cell_invert(double ax, double ay, double az,
                       double bx, double by, double bz,
                       double cx, double cy, double cz,
                       double *asx, double *asy, double *asz,
                       double *bsx, double *bsy, double *bsz,
                       double *csx, double *csy, double *csz)
{
	int s;
	gsl_matrix *m;
	gsl_matrix *inv;
	gsl_permutation *perm;

	m = gsl_matrix_alloc(3, 3);
	if ( m == NULL ) {
		ERROR("Couldn't allocate memory for matrix\n");
		return 1;
	}
	gsl_matrix_set(m, 0, 0, ax);
	gsl_matrix_set(m, 0, 1, bx);
	gsl_matrix_set(m, 0, 2, cx);
	gsl_matrix_set(m, 1, 0, ay);
	gsl_matrix_set(m, 1, 1, by);
	gsl_matrix_set(m, 1, 2, cy);
	gsl_matrix_set(m, 2, 0, az);
	gsl_matrix_set(m, 2, 1, bz);
	gsl_matrix_set(m, 2, 2, cz);

	/* Invert */
	perm = gsl_permutation_alloc(m->size1);
	if ( perm == NULL ) {
		ERROR("Couldn't allocate permutation\n");
		gsl_matrix_free(m);
		return 1;
	}
	inv = gsl_matrix_alloc(m->size1, m->size2);
	if ( inv == NULL ) {
		ERROR("Couldn't allocate inverse\n");
		gsl_matrix_free(m);
		gsl_permutation_free(perm);
		return 1;
	}
	if ( gsl_linalg_LU_decomp(m, perm, &s) ) {
		ERROR("Couldn't decompose matrix\n");
		gsl_matrix_free(m);
		gsl_permutation_free(perm);
		return 1;
	}
	if ( gsl_linalg_LU_invert(m, perm, inv)  ) {
		ERROR("Couldn't invert matrix\n");
		gsl_matrix_free(m);
		gsl_permutation_free(perm);
		return 1;
	}
	gsl_permutation_free(perm);
	gsl_matrix_free(m);

	/* Transpose */
	gsl_matrix_transpose(inv);

	*asx = gsl_matrix_get(inv, 0, 0);
	*bsx = gsl_matrix_get(inv, 0, 1);
	*csx = gsl_matrix_get(inv, 0, 2);
	*asy = gsl_matrix_get(inv, 1, 0);
	*bsy = gsl_matrix_get(inv, 1, 1);
	*csy = gsl_matrix_get(inv, 1, 2);
	*asz = gsl_matrix_get(inv, 2, 0);
	*bsz = gsl_matrix_get(inv, 2, 1);
	*csz = gsl_matrix_get(inv, 2, 2);

	gsl_matrix_free(inv);

	return 0;
}


/********************************** Getters ***********************************/

int cell_get_parameters(UnitCell *cell, double *a, double *b, double *c,
                        double *alpha, double *beta, double *gamma)
{
	double ax, ay, az, bx, by, bz, cx, cy, cz;

	if ( cell == NULL ) return 1;

	switch ( cell->rep ) {

	case CELL_REP_CRYST:
		/* Direct response */
		*a = cell->a;
		*b = cell->b;
		*c = cell->c;
		*alpha = cell->alpha;
		*beta = cell->beta;
		*gamma = cell->gamma;
		return 0;

	case CELL_REP_CART:
		/* Convert cartesian -> crystallographic */
		*a = modulus(cell->ax, cell->ay, cell->az);
		*b = modulus(cell->bx, cell->by, cell->bz);
		*c = modulus(cell->cx, cell->cy, cell->cz);

		*alpha = angle_between(cell->bx, cell->by, cell->bz,
		                       cell->cx, cell->cy, cell->cz);
		*beta = angle_between(cell->ax, cell->ay, cell->az,
		                      cell->cx, cell->cy, cell->cz);
		*gamma = angle_between(cell->ax, cell->ay, cell->az,
		                       cell->bx, cell->by, cell->bz);
		return 0;

	case CELL_REP_RECIP:
		/* Convert reciprocal -> crystallographic.
                 * Start by converting reciprocal -> cartesian */
		cell_invert(cell->axs, cell->ays, cell->azs,
		            cell->bxs, cell->bys, cell->bzs,
		            cell->cxs, cell->cys, cell->czs,
		            &ax, &ay, &az, &bx, &by, &bz, &cx, &cy, &cz);

		/* Now convert cartesian -> crystallographic */
		*a = modulus(ax, ay, az);
		*b = modulus(bx, by, bz);
		*c = modulus(cx, cy, cz);

		*alpha = angle_between(bx, by, bz, cx, cy, cz);
		*beta = angle_between(ax, ay, az, cx, cy, cz);
		*gamma = angle_between(ax, ay, az, bx, by, bz);
		return 0;
	}

	return 1;
}


int cell_get_cartesian(UnitCell *cell,
                       double *ax, double *ay, double *az,
                       double *bx, double *by, double *bz,
                       double *cx, double *cy, double *cz)
{
	if ( cell == NULL ) return 1;

	switch ( cell->rep ) {

	case CELL_REP_CRYST:
		/* Convert crystallographic -> cartesian. */
		return cell_crystallographic_to_cartesian(cell,
		                                          ax, ay, az,
		                                          bx, by, bz,
		                                          cx, cy, cz);

	case CELL_REP_CART:
		/* Direct response */
		*ax = cell->ax;  *ay = cell->ay;  *az = cell->az;
		*bx = cell->bx;  *by = cell->by;  *bz = cell->bz;
		*cx = cell->cx;  *cy = cell->cy;  *cz = cell->cz;
		return 0;

	case CELL_REP_RECIP:
		/* Convert reciprocal -> cartesian */
		return cell_invert(cell->axs, cell->ays, cell->azs,
		                   cell->bxs, cell->bys, cell->bzs,
		                   cell->cxs, cell->cys, cell->czs,
		                   ax, ay, az, bx, by, bz, cx, cy, cz);

	}

	return 1;
}


int cell_get_reciprocal(UnitCell *cell,
                        double *asx, double *asy, double *asz,
                        double *bsx, double *bsy, double *bsz,
                        double *csx, double *csy, double *csz)
{
	int r;
	double ax, ay, az, bx, by, bz, cx, cy, cz;
	if ( cell == NULL ) return 1;

	switch ( cell->rep ) {

	case CELL_REP_CRYST:
		/* Convert crystallographic -> reciprocal */
		r = cell_crystallographic_to_cartesian(cell,
		                                       &ax, &ay, &az,
		                                       &bx, &by, &bz,
		                                       &cx, &cy, &cz);
		if ( r ) return r;
		return cell_invert(ax, ay, az,bx, by, bz, cx, cy, cz,
		                   asx, asy, asz, bsx, bsy, bsz, csx, csy, csz);

	case CELL_REP_CART:
		/* Convert cartesian -> reciprocal */
		cell_invert(cell->ax, cell->ay, cell->az,
		            cell->bx, cell->by, cell->bz,
		            cell->cx, cell->cy, cell->cz,
		            asx, asy, asz, bsx, bsy, bsz, csx, csy, csz);
		return 0;

	case CELL_REP_RECIP:
		/* Direct response */
		*asx = cell->axs;  *asy = cell->ays;  *asz = cell->azs;
		*bsx = cell->bxs;  *bsy = cell->bys;  *bsz = cell->bzs;
		*csx = cell->cxs;  *csy = cell->cys;  *csz = cell->czs;
		return 0;

	}

	return 1;
}


/********************************* Utilities **********************************/

void cell_print(UnitCell *cell)
{
	double asx, asy, asz;
	double bsx, bsy, bsz;
	double csx, csy, csz;
	double angles[3];
	double a, b, c, alpha, beta, gamma;
	double ax, ay, az, bx, by, bz, cx, cy, cz;

	cell_get_parameters(cell, &a, &b, &c, &alpha, &beta, &gamma);

	STATUS("  a     b     c         alpha   beta  gamma\n");
	STATUS("%5.2f %5.2f %5.2f nm    %6.2f %6.2f %6.2f deg\n",
	       a*1e9, b*1e9, c*1e9,
	       rad2deg(alpha), rad2deg(beta), rad2deg(gamma));

	cell_get_cartesian(cell, &ax, &ay, &az, &bx, &by, &bz, &cx, &cy, &cz);

	STATUS("a = %10.3e %10.3e %10.3e m\n", ax, ay, az);
	STATUS("b = %10.3e %10.3e %10.3e m\n", bx, by, bz);
	STATUS("c = %10.3e %10.3e %10.3e m\n", cx, cy, cz);

	cell_get_reciprocal(cell, &asx, &asy, &asz,
	                          &bsx, &bsy, &bsz,
	                          &csx, &csy, &csz);

	STATUS("astar = %10.3e %10.3e %10.3e m^-1 (modulus = %10.3e m^-1)\n",
	                             asx, asy, asz, modulus(asx, asy, asz));
	STATUS("bstar = %10.3e %10.3e %10.3e m^-1 (modulus = %10.3e m^-1)\n",
	                             bsx, bsy, bsz, modulus(bsx, bsy, bsz));
	STATUS("cstar = %10.3e %10.3e %10.3e m^-1 (modulus = %10.3e m^-1)\n",
	                             csx, csy, csz, modulus(csx, csy, csz));

	angles[0] = angle_between(bsx, bsy, bsz, csx, csy, csz);
	angles[1] = angle_between(asx, asy, asz, csx, csy, csz);
	angles[2] = angle_between(asx, asy, asz, bsx, bsy, bsz);
}


#define MAX_CAND (1024)

static int within_tolerance(double a, double b, double percent)
{
	double tol = a * (percent/100.0);
	if ( fabs(b-a) < tol ) return 1;
	return 0;
}


struct cvec {
	struct rvec vec;
	float na;
	float nb;
	float nc;
	float fom;
};


static int same_vector(struct cvec a, struct cvec b)
{
	if ( a.na != b.na ) return 0;
	if ( a.nb != b.nb ) return 0;
	if ( a.nc != b.nc ) return 0;
	return 1;
}


/* Attempt to make 'cell' fit into 'template' somehow */
UnitCell *match_cell(UnitCell *cell, UnitCell *template, int verbose)
{
	signed int n1l, n2l, n3l;
	double asx, asy, asz;
	double bsx, bsy, bsz;
	double csx, csy, csz;
	int i, j;
	double lengths[3];
	double angles[3];
	struct cvec *cand[3];
	UnitCell *new_cell = NULL;
	float best_fom = +999999999.9; /* Large number.. */
	int ncand[3] = {0,0,0};
	float ltl = 5.0;     /* percent */
	float angtol = deg2rad(1.5);

	if ( verbose ) {
		STATUS("Matching with this model cell: "
		       "----------------------------\n");
		cell_print(template);
		STATUS("-------------------------------"
		       "----------------------------\n");
	}

	if ( cell_get_reciprocal(template, &asx, &asy, &asz,
	                         &bsx, &bsy, &bsz,
	                         &csx, &csy, &csz) ) {
		ERROR("Couldn't get reciprocal cell for template.\n");
		return NULL;
	}

	lengths[0] = modulus(asx, asy, asz);
	lengths[1] = modulus(bsx, bsy, bsz);
	lengths[2] = modulus(csx, csy, csz);

	angles[0] = angle_between(bsx, bsy, bsz, csx, csy, csz);
	angles[1] = angle_between(asx, asy, asz, csx, csy, csz);
	angles[2] = angle_between(asx, asy, asz, bsx, bsy, bsz);

	cand[0] = malloc(MAX_CAND*sizeof(struct cvec));
	cand[1] = malloc(MAX_CAND*sizeof(struct cvec));
	cand[2] = malloc(MAX_CAND*sizeof(struct cvec));

	if ( cell_get_reciprocal(cell, &asx, &asy, &asz,
	                         &bsx, &bsy, &bsz,
	                         &csx, &csy, &csz) ) {
		ERROR("Couldn't get reciprocal cell.\n");
		return NULL;
	}

	/* Negative values mean 1/n, positive means n, zero means zero */
	for ( n1l=-2; n1l<=4; n1l++ ) {
	for ( n2l=-2; n2l<=4; n2l++ ) {
	for ( n3l=-2; n3l<=4; n3l++ ) {

		float n1, n2, n3;
		signed int b1, b2, b3;

		n1 = (n1l>=0) ? (n1l) : (1.0/n1l);
		n2 = (n2l>=0) ? (n2l) : (1.0/n2l);
		n3 = (n3l>=0) ? (n3l) : (1.0/n3l);

		/* 'bit' values can be +1 or -1 */
		for ( b1=-1; b1<=1; b1+=2 ) {
		for ( b2=-1; b2<=1; b2+=2 ) {
		for ( b3=-1; b3<=1; b3+=2 ) {

			double tx, ty, tz;
			double tlen;
			int i;

			n1 *= b1;  n2 *= b2;  n3 *= b3;

			tx = n1*asx + n2*bsx + n3*csx;
			ty = n1*asy + n2*bsy + n3*csy;
			tz = n1*asz + n2*bsz + n3*csz;
			tlen = modulus(tx, ty, tz);

			/* Test modulus for agreement with moduli of template */
			for ( i=0; i<3; i++ ) {

				if ( !within_tolerance(lengths[i], tlen, ltl) )
					continue;

				cand[i][ncand[i]].vec.u = tx;
				cand[i][ncand[i]].vec.v = ty;
				cand[i][ncand[i]].vec.w = tz;
				cand[i][ncand[i]].na = n1;
				cand[i][ncand[i]].nb = n2;
				cand[i][ncand[i]].nc = n3;
				cand[i][ncand[i]].fom = fabs(lengths[i] - tlen);
				if ( ncand[i] == MAX_CAND ) {
					ERROR("Too many candidates\n");
				} else {
					ncand[i]++;
				}
			}

		}
		}
		}

	}
	}
	}

	if ( verbose ) {
		STATUS("Candidates: %i %i %i\n", ncand[0], ncand[1], ncand[2]);
	}

	for ( i=0; i<ncand[0]; i++ ) {
	for ( j=0; j<ncand[1]; j++ ) {

		double ang;
		int k;
		float fom1;

		if ( same_vector(cand[0][i], cand[1][j]) ) continue;

		/* Measure the angle between the ith candidate for axis 0
		 * and the jth candidate for axis 1 */
		ang = angle_between(cand[0][i].vec.u, cand[0][i].vec.v,
		                    cand[0][i].vec.w, cand[1][j].vec.u,
		                    cand[1][j].vec.v, cand[1][j].vec.w);

		/* Angle between axes 0 and 1 should be angle 2 */
		if ( fabs(ang - angles[2]) > angtol ) continue;

		fom1 = fabs(ang - angles[2]);

		for ( k=0; k<ncand[2]; k++ ) {

			float fom2, fom3;

			if ( same_vector(cand[1][j], cand[2][k]) ) continue;

			/* Measure the angle between the current candidate for
			 * axis 0 and the kth candidate for axis 2 */
			ang = angle_between(cand[0][i].vec.u, cand[0][i].vec.v,
			                    cand[0][i].vec.w, cand[2][k].vec.u,
			                    cand[2][k].vec.v, cand[2][k].vec.w);

			/* ... it should be angle 1 ... */
			if ( fabs(ang - angles[1]) > angtol ) continue;

			fom2 = fom1 + fabs(ang - angles[1]);

			/* Finally, the angle between the current candidate for
			 * axis 1 and the kth candidate for axis 2 */
			ang = angle_between(cand[1][j].vec.u, cand[1][j].vec.v,
			                    cand[1][j].vec.w, cand[2][k].vec.u,
			                    cand[2][k].vec.v, cand[2][k].vec.w);

			/* ... it should be angle 0 ... */
			if ( fabs(ang - angles[0]) > angtol ) continue;

			fom3 = fom2 + fabs(ang - angles[0]);
			fom3 += LWEIGHT * (cand[0][i].fom + cand[1][j].fom
			                   + cand[2][k].fom);

			if ( fom3 < best_fom ) {
				if ( new_cell != NULL ) free(new_cell);
				new_cell = cell_new_from_axes(cand[0][i].vec,
			                                      cand[1][j].vec,
			                                      cand[2][k].vec);
				best_fom = fom3;
			}

		}

	}
	}

	if ( new_cell != NULL ) {
		STATUS("Success! --------------- \n");
		cell_print(new_cell);
	}

	free(cand[0]);
	free(cand[1]);
	free(cand[2]);

	return new_cell;
}


/* Return sin(theta)/lambda = 1/2d.  Multiply by two if you want 1/d */
double resolution(UnitCell *cell, signed int h, signed int k, signed int l)
{
	double a, b, c, alpha, beta, gamma;

	cell_get_parameters(cell, &a, &b, &c, &alpha, &beta, &gamma);

	const double Vsq = a*a*b*b*c*c*(1 - cos(alpha)*cos(alpha)
	                                  - cos(beta)*cos(beta)
	                                  - cos(gamma)*cos(gamma)
				          + 2*cos(alpha)*cos(beta)*cos(gamma) );

	const double S11 = b*b*c*c*sin(alpha)*sin(alpha);
	const double S22 = a*a*c*c*sin(beta)*sin(beta);
	const double S33 = a*a*b*b*sin(gamma)*sin(gamma);
	const double S12 = a*b*c*c*(cos(alpha)*cos(beta) - cos(gamma));
	const double S23 = a*a*b*c*(cos(beta)*cos(gamma) - cos(alpha));
	const double S13 = a*b*b*c*(cos(gamma)*cos(alpha) - cos(beta));

	const double brackets = S11*h*h + S22*k*k + S33*l*l
	                         + 2*S12*h*k + 2*S23*k*l + 2*S13*h*l;
	const double oneoverdsq = brackets / Vsq;
	const double oneoverd = sqrt(oneoverdsq);

	return oneoverd / 2;
}


UnitCell *load_cell_from_pdb(const char *filename)
{
	FILE *fh;
	char *rval;
	UnitCell *cell = NULL;

	fh = fopen(filename, "r");
	if ( fh == NULL ) {
		ERROR("Couldn't open '%s'\n", filename);
		return NULL;
	}

	do {

		char line[1024];

		rval = fgets(line, 1023, fh);

		if ( strncmp(line, "CRYST1", 6) == 0 ) {

			float a, b, c, al, be, ga;
			int r;

			r = sscanf(line+7, "%f %f %f %f %f %f",
			           &a, &b, &c, &al, &be, &ga);
			if ( r != 6 ) {
				ERROR("Couldn't understand CRYST1 line\n");
				return NULL;
			}

			cell = cell_new_from_parameters(a*1e-10,
			                                b*1e-10, c*1e-10,
	                                                deg2rad(al),
	                                                deg2rad(be),
	                                                deg2rad(ga));

		}

	} while ( rval != NULL );

	fclose(fh);

	return cell;
}

/*
 * peaks.c
 *
 * Peak search and other image analysis
 *
 * (c) 2006-2010 Thomas White <taw@physics.org>
 *
 * Part of CrystFEL - crystallography with a FEL
 *
 */


#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdlib.h>
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <assert.h>
#include <gsl/gsl_statistics_int.h>

#include "image.h"
#include "utils.h"
#include "index.h"
#include "peaks.h"
#include "detector.h"
#include "filters.h"


#define PEAK_WINDOW_SIZE (10)
#define MAX_PEAKS (2048)
#define INTEGRATION_RADIUS (10)

static int in_streak(int x, int y)
{
	if ( (y>512) && (y<600) && (abs(x-489)<15) ) return 1;
	if ( (y>600) && (abs(x-480)<25) ) return 1;
	return 0;
}


struct peak {
	int x;
	int y;
	int i;
	int invalid;
};


int image_fom(struct image *image)
{
	int x, y;
	int integr, n;
	float fintegr, mean, sd, th;
	struct peak *peaks;
	int i, n_peaks, n_nearby, n_valid;

	peaks = malloc(MAX_PEAKS * sizeof(struct peak));

	/* Measure mean */
	integr = 0;
	n = 0;
	for ( x=0; x<1024; x++ ) {
	for ( y=600; y<1024; y++ ) {

		int val;
		if ( (x>400) && (x<600) ) continue;
		val = image->data[x+image->height*y];
		if ( val < 0 ) continue;
		integr += val;
		n++;

	}
	}
	mean = (float)integr / n;  /* As integer to keep maths fast */

	/* Standard deviation */
	fintegr = 0;
	for ( x=0; x<1024; x++ ) {
	for ( y=600; y<1024; y++ ) {

		float val;

		if ( (x>400) && (x<600) ) continue;
		val = (float)image->data[x+image->height*y];
		if ( val < 0 ) continue;

		val -= mean;
		val = powf(val, 2.0);
		fintegr += val;

	}
	}
	sd = sqrtf(fintegr / n);

	/* Threshold */
	th = mean + 5*sd;

	/* Find pixels above threshold */
	n_peaks = 0;
	for ( x=0; x<1024; x++ ) {
	for ( y=600; y<1024; y++ ) {

		int val;

		/* Chop out streaky region */
		if ( (x>400) && (x<600) ) continue;

		val = image->data[x+image->height*y];

		if ( val > th ) {
			if ( n_peaks < MAX_PEAKS ) {
				peaks[n_peaks].x = x;
				peaks[n_peaks].y = y;
				peaks[n_peaks].i = val;
				peaks[n_peaks].invalid = 0;
				n_peaks++;
			}
		}

	}
	}
	if ( n_peaks < 1 ) return 0;

	do {

		int max, max_i = -1;
		int adjacent;

		n_nearby = 0;

		/* Find brightest peak */
		max = 0;
		for ( i=0; i<n_peaks; i++ ) {
			if ( peaks[i].i > max ) {
				max = peaks[i].i;
				max_i = i;
			}
		}

		/* Must be at least one adjacent bright pixel */
		adjacent = 0;
		for ( i=0; i<n_peaks; i++ ) {

			int td;

			td = abs(peaks[i].x - peaks[max_i].x) +
			     abs(peaks[i].y - peaks[max_i].y);
			if ( td == 1 ) {
				adjacent++;
				break;  /* One is enough */
			}
		}
		if ( adjacent < 1 ) {
			peaks[max_i].invalid = 1;
			/* If invalidated, don't remove nearby peaks */
			continue;
		}

		/* Remove nearby (non-invalidated) peaks from list */
		n_nearby = 0;
		for ( i=0; i<n_peaks; i++ ) {

			int dx, dy, ds;

			if ( peaks[i].invalid ) continue;

			dx = abs(peaks[i].x - peaks[max_i].x);
			dy = abs(peaks[i].y - peaks[max_i].y);
			ds = dx*dx + dy*dy;
			if ( ds < 36 ) {
				n_nearby++;
				peaks[i].invalid = 1;
			}

		}

	} while ( n_nearby );

	n_valid = 0;
	for ( i=0; i<n_peaks; i++ ) {
		if ( peaks[i].invalid ) continue;
		//printf("%i %i\n", peaks[i].x, peaks[i].y);
		n_valid++;
	}

	free(peaks);

	return n_valid;
}


static int is_hot_pixel(struct image *image, int x, int y)
{
	int dx, dy;
	int w, v;

	w = image->width;
	v = (1*image->data[x+w*y])/2;

	if ( x+1 >= image->width ) return 0;
	if ( x-1 < 0 ) return 0;
	if ( y+1 >= image->height ) return 0;
	if ( y-1 < 0 ) return 0;

	/* Must be at least one adjacent bright pixel */
	for ( dx=-1; dx<=+1; dx++ ) {
	for ( dy=-1; dy<=+1; dy++ ) {
		if ( (dx==0) && (dy==0) ) continue;
		if ( image->data[(x+dx)+w*(y+dy)] >= v ) return 0;
	}
	}

	return 1;
}


/* Post-processing of the peak list to remove noise */
static void cull_peaks(struct image *image)
{
	int i, n;
	int nelim = 0;

	n = image_feature_count(image->features);

	for ( i=0; i<n; i++ ) {

		struct imagefeature *f;
		int j, ncol;

		f = image_get_feature(image->features, i);
		if ( f == NULL ) continue;

		/* How many peaks are in exactly the same column? */
		ncol = 0;
		for ( j=0; j<n; j++ ) {

			struct imagefeature *g;

			if ( i==j ) continue;

			g = image_get_feature(image->features, j);
			if ( g == NULL ) continue;
			if ( f->x == g->x ) ncol++;

		}

		/* More than three? */
		if ( ncol <= 3 ) continue;

		/* Yes?  Delete them all... */
		nelim = 0;
		for ( j=0; j<n; j++ ) {
			struct imagefeature *g;
			g = image_get_feature(image->features, j);
			if ( g == NULL ) continue;
			if ( f->x == g->x ) {
				image_remove_feature(image->features, j);
				nelim++;
			}
		}

	}

	//STATUS("%i peaks eliminated\n", nelim);
}


static void integrate_peak(struct image *image, int xp, int yp,
                           float *xc, float *yc, float *intensity)
{
	signed int x, y;
	const int lim = INTEGRATION_RADIUS * INTEGRATION_RADIUS;
	int total = 0;
	int xct = 0;
	int yct = 0;

	for ( x=-INTEGRATION_RADIUS; x<+INTEGRATION_RADIUS; x++ ) {
	for ( y=-INTEGRATION_RADIUS; y<+INTEGRATION_RADIUS; y++ ) {

		int val;

		/* Circular mask */
		if ( x*x + y*y > lim ) continue;

		if ( ((x+xp)>=image->width) || ((x+xp)<0) ) continue;
		if ( ((y+yp)>=image->height) || ((y+yp)<0) ) continue;

		val = image->data[(x+xp)+image->width*(y+yp)];

		total += val;

		xct += val*(xp+x);
		yct += val*(yp+y);

	}
	}

	*xc = (float)xct / total;
	*yc = (float)yct / total;
	*intensity = total;
}


void search_peaks(struct image *image)
{
	int x, y, width, height;
	float *data;
	double d;
	int idx;
	float fx = 0.0;
	float fy = 0.0;
	float intensity = 0.0;
	int nrej_dis = 0;
	int nrej_hot = 0;
	int nrej_pro = 0;
	int nrej_fra = 0;
	int nacc = 0;

	data = image->data;
	width = image->width;
	height = image->height;

	if ( image->features != NULL ) {
		image_feature_list_free(image->features);
	}
	image->features = image_feature_list_new();

	for ( x=1; x<image->width-1; x++ ) {
	for ( y=1; y<image->height-1; y++ ) {

		double dx1, dx2, dy1, dy2;
		double dxs, dys;
		double grad;
		int mask_x, mask_y;
		int sx, sy;
		double max;
		unsigned int did_something;

		/* Overall threshold */
		if ( data[x+width*y] < 800 ) continue;

		/* Ignore streak */
		if ( in_streak(x, y) ) continue;

		/* Get gradients */
		dx1 = data[x+width*y] - data[(x+1)+width*y];
		dx2 = data[(x-1)+width*y] - data[x+width*y];
		dy1 = data[x+width*y] - data[(x+1)+width*(y+1)];
		dy2 = data[x+width*(y-1)] - data[x+width*y];

		/* Average gradient measurements from both sides */
		dxs = ((dx1*dx1) + (dx2*dx2)) / 2;
		dys = ((dy1*dy1) + (dy2*dy2)) / 2;

		/* Calculate overall gradient */
		grad = dxs + dys;

		if ( grad < 100000 ) continue;

		mask_x = x;
		mask_y = y;

		do {

			max = data[mask_x+width*mask_y];
			did_something = 0;

			for ( sy=biggest(mask_y-PEAK_WINDOW_SIZE/2, 0);
			      sy<smallest(mask_y+PEAK_WINDOW_SIZE/2, height-1);
			      sy++ ) {
			for ( sx=biggest(mask_x-PEAK_WINDOW_SIZE/2, 0);
			      sx<smallest(mask_x+PEAK_WINDOW_SIZE/2, width-1);
			      sx++ ) {

				if ( data[sx+width*sy] > max ) {
					max = data[sx+width*sy];
					mask_x = sx;
					mask_y = sy;
					did_something = 1;
				}

			}
			}

			/* Abort if drifted too far from the foot point */
			if ( distance(mask_x, mask_y, x, y) > 50.0 ) break;

		} while ( did_something );

		/* Too far from foot point? */
		if ( distance(mask_x, mask_y, x, y) > 50.0 ) {
			nrej_dis++;
			continue;
		}

		/* Should be enforced by bounds used above.  Muppet check. */
		assert(mask_x < image->width);
		assert(mask_y < image->height);
		assert(mask_x >= 0);
		assert(mask_y >= 0);

		/* Isolated hot pixel? */
		if ( is_hot_pixel(image, mask_x, mask_y) ) {
			nrej_hot++;
			continue;
		}

		/* Centroid peak and get better coordinates */
		integrate_peak(image, mask_x, mask_y, &fx, &fy, &intensity);

		/* It is possible for the centroid to fall outside the image */
		if ( (fx < 0.0) || (fx > image->width)
		  || (fy < 0.0) || (fy > image->height) ) {
			nrej_fra++;
			continue;
		}

		/* Check for a nearby feature */
		image_feature_closest(image->features, fx, fy, &d, &idx);
		if ( d < 15.0 ) {
			nrej_pro++;
			continue;
		}

		/* Add using "better" coordinates */
		image_add_feature(image->features, fx, fy, image, intensity,
		                  NULL);
		nacc++;

	}
	}

	STATUS("%i accepted, %i box, %i hot, %i proximity, %i outside frame\n",
	       nacc, nrej_dis, nrej_hot, nrej_pro, nrej_fra);

	cull_peaks(image);
}


void dump_peaks(struct image *image)
{
	int i;

	printf("x/px\ty/px\t(1/d)/nm^-1\n");

	for ( i=0; i<image_feature_count(image->features); i++ ) {

		double q, rx, ry, rz;
		struct imagefeature *f;

		f = image_get_feature(image->features, i);
		if ( f == NULL ) continue;

		map_position(image, f->x, f->y, &rx, &ry, &rz);
		q = modulus(rx, ry, rz);

		printf("%7.3f\t%7.3f\t%7.3f\t%7.3f\n", f->x, f->y, q/1.0e9, 1.0);

	}
}

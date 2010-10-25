/*
 * diffraction-gpu.c
 *
 * Calculate diffraction patterns by Fourier methods (GPU version)
 *
 * (c) 2006-2010 Thomas White <taw@physics.org>
 *
 * Part of CrystFEL - crystallography with a FEL
 *
 */


#include <stdlib.h>
#include <math.h>
#include <stdio.h>
#include <string.h>
#include <complex.h>
#include <cl.h>

#include "image.h"
#include "utils.h"
#include "cell.h"
#include "diffraction.h"
#include "sfac.h"
#include "cl-utils.h"
#include "beam-parameters.h"


#define SAMPLING (4)
#define BWSAMPLING (10)

#define SINC_LUT_ELEMENTS (4096)


struct gpu_context
{
	cl_context ctx;
	cl_command_queue cq;
	cl_program prog;
	cl_kernel kern;
	cl_mem intensities;

	cl_mem tt;
	size_t tt_size;

	cl_mem diff;
	size_t diff_size;

	/* Array of sinc LUTs */
	cl_mem *sinc_luts;
	cl_float **sinc_lut_ptrs;
	int max_sinc_lut;  /* Number of LUTs, i.e. one greater than the maximum
	                    * index.  This equals the highest allowable "n". */
};


static void check_sinc_lut(struct gpu_context *gctx, int n)
{
	cl_int err;
	cl_image_format fmt;
	int i;

	if ( n > gctx->max_sinc_lut ) {

		gctx->sinc_luts = realloc(gctx->sinc_luts,
		                          n*sizeof(*gctx->sinc_luts));
		gctx->sinc_lut_ptrs = realloc(gctx->sinc_lut_ptrs,
		                              n*sizeof(*gctx->sinc_lut_ptrs));

		for ( i=gctx->max_sinc_lut; i<n; i++ ) {
			gctx->sinc_lut_ptrs[i] = NULL;
		}

		gctx->max_sinc_lut = n;
	}

	if ( gctx->sinc_lut_ptrs[n-1] != NULL ) return;

	/* Create a new sinc LUT */
	gctx->sinc_lut_ptrs[n-1] = malloc(SINC_LUT_ELEMENTS*sizeof(cl_float));
	gctx->sinc_lut_ptrs[n-1][0] = n;
	if ( n == 1 ) {
		for ( i=1; i<SINC_LUT_ELEMENTS; i++ ) {
			gctx->sinc_lut_ptrs[n-1][i] = 1.0;
		}
	} else {
		for ( i=1; i<SINC_LUT_ELEMENTS; i++ ) {
			double x, val;
			x = (double)i/SINC_LUT_ELEMENTS;
			val = fabs(sin(M_PI*n*x)/sin(M_PI*x));
			gctx->sinc_lut_ptrs[n-1][i] = val;
		}
	}

	fmt.image_channel_order = CL_INTENSITY;
	fmt.image_channel_data_type = CL_FLOAT;

	gctx->sinc_luts[n-1] = clCreateImage2D(gctx->ctx,
	                                CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
	                                &fmt, SINC_LUT_ELEMENTS, 1, 0,
	                                gctx->sinc_lut_ptrs[n-1], &err);
	if ( err != CL_SUCCESS ) {
		ERROR("Couldn't create LUT for %i\n", n);
		return;
	}
}


void get_diffraction_gpu(struct gpu_context *gctx, struct image *image,
                         int na, int nb, int nc, UnitCell *ucell)
{
	cl_int err;
	double ax, ay, az;
	double bx, by, bz;
	double cx, cy, cz;
	float klow, khigh;
	cl_event *event;
	int p;
	float *tt_ptr;
	int x, y;
	cl_float16 cell;
	float *diff_ptr;
	cl_float4 orientation;
	cl_int4 ncells;
	const int sampling = SAMPLING;
	cl_float bwstep;

	if ( gctx == NULL ) {
		ERROR("GPU setup failed.\n");
		return;
	}

	cell_get_cartesian(ucell, &ax, &ay, &az, &bx, &by, &bz, &cx, &cy, &cz);
	cell.s[0] = ax;  cell.s[1] = ay;  cell.s[2] = az;
	cell.s[3] = bx;  cell.s[4] = by;  cell.s[5] = bz;
	cell.s[6] = cx;  cell.s[7] = cy;  cell.s[8] = cz;

	/* Calculate wavelength */
	klow = 1.0/(image->lambda*(1.0 + image->beam->bandwidth/2.0));
	khigh = 1.0/(image->lambda*(1.0 - image->beam->bandwidth/2.0));
	bwstep = (khigh-klow) / BWSAMPLING;

	/* Orientation */
	orientation.s[0] = image->orientation.w;
	orientation.s[1] = image->orientation.x;
	orientation.s[2] = image->orientation.y;
	orientation.s[3] = image->orientation.z;

	ncells.s[0] = na;
	ncells.s[1] = nb;
	ncells.s[2] = nc;
	ncells.s[3] = 0;  /* unused */

	/* Ensure all required LUTs are available */
	check_sinc_lut(gctx, na);
	check_sinc_lut(gctx, nb);
	check_sinc_lut(gctx, nc);

	err = clSetKernelArg(gctx->kern, 0, sizeof(cl_mem), &gctx->diff);
	if ( err != CL_SUCCESS ) {
		ERROR("Couldn't set arg 0: %s\n", clError(err));
		return;
	}
	clSetKernelArg(gctx->kern, 1, sizeof(cl_mem), &gctx->tt);
	if ( err != CL_SUCCESS ) {
		ERROR("Couldn't set arg 1: %s\n", clError(err));
		return;
	}
	clSetKernelArg(gctx->kern, 2, sizeof(cl_float), &klow);
	if ( err != CL_SUCCESS ) {
		ERROR("Couldn't set arg 2: %s\n", clError(err));
		return;
	}
	clSetKernelArg(gctx->kern, 3, sizeof(cl_int), &image->width);
	if ( err != CL_SUCCESS ) {
		ERROR("Couldn't set arg 3: %s\n", clError(err));
		return;
	}
	clSetKernelArg(gctx->kern, 8, sizeof(cl_float16), &cell);
	if ( err != CL_SUCCESS ) {
		ERROR("Couldn't set arg 8: %s\n", clError(err));
		return;
	}
	clSetKernelArg(gctx->kern, 9, sizeof(cl_mem), &gctx->intensities);
	if ( err != CL_SUCCESS ) {
		ERROR("Couldn't set arg 9: %s\n", clError(err));
		return;
	}
	clSetKernelArg(gctx->kern, 10, sizeof(cl_float4), &orientation);
	if ( err != CL_SUCCESS ) {
		ERROR("Couldn't set arg 10: %s\n", clError(err));
		return;
	}
	clSetKernelArg(gctx->kern, 13, sizeof(cl_int), &sampling);
	if ( err != CL_SUCCESS ) {
		ERROR("Couldn't set arg 13: %s\n", clError(err));
		return;
	}
	/* Local memory for reduction */
	clSetKernelArg(gctx->kern, 14,
	               BWSAMPLING*SAMPLING*SAMPLING*sizeof(cl_float), NULL);
	if ( err != CL_SUCCESS ) {
		ERROR("Couldn't set arg 14: %s\n", clError(err));
		return;
	}
	/* Bandwidth sampling step */
	clSetKernelArg(gctx->kern, 15, sizeof(cl_float), &bwstep);
	if ( err != CL_SUCCESS ) {
		ERROR("Couldn't set arg 15: %s\n", clError(err));
		return;
	}

	/* LUT in 'a' direction */
	clSetKernelArg(gctx->kern, 16, sizeof(cl_mem), &gctx->sinc_luts[na-1]);
	if ( err != CL_SUCCESS ) {
		ERROR("Couldn't set arg 16: %s\n", clError(err));
		return;
	}

	/* LUT in 'b' direction */
	clSetKernelArg(gctx->kern, 17, sizeof(cl_mem), &gctx->sinc_luts[nb-1]);
	if ( err != CL_SUCCESS ) {
		ERROR("Couldn't set arg 17: %s\n", clError(err));
		return;
	}

	/* LUT in 'c' direction */
	clSetKernelArg(gctx->kern, 18, sizeof(cl_mem), &gctx->sinc_luts[nc-1]);
	if ( err != CL_SUCCESS ) {
		ERROR("Couldn't set arg 18: %s\n", clError(err));
		return;
	}

	/* Iterate over panels */
	event = malloc(image->det->n_panels * sizeof(cl_event));
	for ( p=0; p<image->det->n_panels; p++ ) {

		size_t dims[3];
		size_t ldims[3] = {SAMPLING, SAMPLING, BWSAMPLING};

		/* In a future version of OpenCL, this could be done
		 * with a global work offset.  But not yet... */
		dims[0] = 1+image->det->panels[0].max_x-image->det->panels[0].min_x;
		dims[1] = 1+image->det->panels[0].max_y-image->det->panels[0].min_y;
		dims[0] *= SAMPLING;
		dims[1] *= SAMPLING;
		dims[2] = BWSAMPLING;

		clSetKernelArg(gctx->kern, 4, sizeof(cl_float),
		               &image->det->panels[p].cx);
		if ( err != CL_SUCCESS ) {
			ERROR("Couldn't set arg 4: %s\n", clError(err));
			return;
		}
		clSetKernelArg(gctx->kern, 5, sizeof(cl_float),
		               &image->det->panels[p].cy);
		if ( err != CL_SUCCESS ) {
			ERROR("Couldn't set arg 5: %s\n", clError(err));
			return;
		}
		clSetKernelArg(gctx->kern, 6, sizeof(cl_float),
		               &image->det->panels[p].res);
		if ( err != CL_SUCCESS ) {
			ERROR("Couldn't set arg 6: %s\n", clError(err));
			return;
		}
		clSetKernelArg(gctx->kern, 7, sizeof(cl_float),
		               &image->det->panels[p].clen);
		if ( err != CL_SUCCESS ) {
			ERROR("Couldn't set arg 7: %s\n", clError(err));
			return;
		}
		clSetKernelArg(gctx->kern, 11, sizeof(cl_int),
		               &image->det->panels[p].min_x);
		if ( err != CL_SUCCESS ) {
			ERROR("Couldn't set arg 11: %s\n", clError(err));
			return;
		}
		clSetKernelArg(gctx->kern, 12, sizeof(cl_int),
		               &image->det->panels[p].min_y);
		if ( err != CL_SUCCESS ) {
			ERROR("Couldn't set arg 12: %s\n", clError(err));
			return;
		}

		err = clEnqueueNDRangeKernel(gctx->cq, gctx->kern, 3, NULL,
		                             dims, ldims, 0, NULL, &event[p]);
		if ( err != CL_SUCCESS ) {
			ERROR("Couldn't enqueue diffraction kernel: %s\n",
			      clError(err));
			return;
		}
	}

	diff_ptr = clEnqueueMapBuffer(gctx->cq, gctx->diff, CL_TRUE,
	                              CL_MAP_READ, 0, gctx->diff_size,
	                              image->det->n_panels, event, NULL, &err);
	if ( err != CL_SUCCESS ) {
		ERROR("Couldn't map diffraction buffer: %s\n", clError(err));
		return;
	}
	tt_ptr = clEnqueueMapBuffer(gctx->cq, gctx->tt, CL_TRUE, CL_MAP_READ, 0,
	                            gctx->tt_size, image->det->n_panels, event,
	                            NULL, &err);
	if ( err != CL_SUCCESS ) {
		ERROR("Couldn't map tt buffer\n");
		return;
	}

	free(event);

	image->data = calloc(image->width * image->height, sizeof(float));
	image->twotheta = calloc(image->width * image->height, sizeof(double));

	for ( x=0; x<image->width; x++ ) {
	for ( y=0; y<image->height; y++ ) {

		float val, tt;

		val = diff_ptr[x + image->width*y];
		if ( isinf(val) ) {
			ERROR("Extracting infinity at %i,%i\n", x, y);
		}
		if ( val < 0.0 ) {
			ERROR("Extracting negative at %i,%i\n", x, y);
		}
		if ( isnan(val) ) {
			ERROR("Extracting NaN at %i,%i\n", x, y);
		}
		tt = tt_ptr[x + image->width*y];

		image->data[x + image->width*y] = val;
		image->twotheta[x + image->width*y] = tt;

	}
	}

	clEnqueueUnmapMemObject(gctx->cq, gctx->diff, diff_ptr, 0, NULL, NULL);
	clEnqueueUnmapMemObject(gctx->cq, gctx->tt, tt_ptr, 0, NULL, NULL);
}


/* Setup the OpenCL stuff, create buffers, load the structure factor table */
struct gpu_context *setup_gpu(int no_sfac, struct image *image,
                              const double *intensities)
{
	struct gpu_context *gctx;
	cl_uint nplat;
	cl_platform_id platforms[8];
	cl_context_properties prop[3];
	cl_int err;
	cl_device_id dev;
	size_t intensities_size;
	float *intensities_ptr;
	size_t maxwgsize;
	int i;

	STATUS("Setting up GPU..."); fflush(stderr);

	err = clGetPlatformIDs(8, platforms, &nplat);
	if ( err != CL_SUCCESS ) {
		ERROR("Couldn't get platform IDs: %i\n", err);
		return NULL;
	}
	if ( nplat == 0 ) {
		ERROR("Couldn't find at least one platform!\n");
		return NULL;
	}
	prop[0] = CL_CONTEXT_PLATFORM;
	prop[1] = (cl_context_properties)platforms[0];
	prop[2] = 0;

	gctx = malloc(sizeof(*gctx));
	gctx->ctx = clCreateContextFromType(prop, CL_DEVICE_TYPE_GPU,
	                                    NULL, NULL, &err);
	if ( err != CL_SUCCESS ) {
		ERROR("Couldn't create OpenCL context: %i\n", err);
		free(gctx);
		return NULL;
	}

	dev = get_first_dev(gctx->ctx);

	gctx->cq = clCreateCommandQueue(gctx->ctx, dev, 0, &err);
	if ( err != CL_SUCCESS ) {
		ERROR("Couldn't create OpenCL command queue\n");
		free(gctx);
		return NULL;
	}

	/* Create buffer for the picture */
	gctx->diff_size = image->width*image->height*sizeof(cl_float);
	gctx->diff = clCreateBuffer(gctx->ctx, CL_MEM_WRITE_ONLY,
	                            gctx->diff_size, NULL, &err);
	if ( err != CL_SUCCESS ) {
		ERROR("Couldn't allocate diffraction memory\n");
		free(gctx);
		return NULL;
	}

	/* Create a single-precision version of the scattering factors */
	intensities_size = IDIM*IDIM*IDIM*sizeof(cl_float);
	intensities_ptr = malloc(intensities_size);
	if ( intensities != NULL ) {
		for ( i=0; i<IDIM*IDIM*IDIM; i++ ) {
			intensities_ptr[i] = intensities[i];
		}
	} else {
		for ( i=0; i<IDIM*IDIM*IDIM; i++ ) {
			intensities_ptr[i] = 10000.0;
		}
	}
	gctx->intensities = clCreateBuffer(gctx->ctx,
	                             CL_MEM_READ_ONLY | CL_MEM_COPY_HOST_PTR,
	                             intensities_size, intensities_ptr, &err);
	if ( err != CL_SUCCESS ) {
		ERROR("Couldn't allocate intensities memory\n");
		free(gctx);
		return NULL;
	}
	free(intensities_ptr);

	gctx->tt_size = image->width*image->height*sizeof(cl_float);
	gctx->tt = clCreateBuffer(gctx->ctx, CL_MEM_WRITE_ONLY, gctx->tt_size,
	                          NULL, &err);
	if ( err != CL_SUCCESS ) {
		ERROR("Couldn't allocate twotheta memory\n");
		free(gctx);
		return NULL;
	}

	gctx->prog = load_program(DATADIR"/crystfel/diffraction.cl", gctx->ctx,
	                          dev, &err);
	if ( err != CL_SUCCESS ) {
		free(gctx);
		return NULL;
	}

	gctx->kern = clCreateKernel(gctx->prog, "diffraction", &err);
	if ( err != CL_SUCCESS ) {
		ERROR("Couldn't create kernel\n");
		free(gctx);
		return NULL;
	}

	STATUS("done\n");

	gctx->max_sinc_lut = 0;
	gctx->sinc_lut_ptrs = NULL;
	gctx->sinc_luts = NULL;

	clGetDeviceInfo(dev, CL_DEVICE_MAX_WORK_GROUP_SIZE,
	                sizeof(size_t), &maxwgsize, NULL);
	STATUS("Maximum work group size = %lli\n", (long long int)maxwgsize);

	return gctx;
}


void cleanup_gpu(struct gpu_context *gctx)
{
	int i;

	clReleaseProgram(gctx->prog);
	clReleaseMemObject(gctx->diff);
	clReleaseMemObject(gctx->tt);
	clReleaseMemObject(gctx->intensities);

	/* Release LUTs */
	for ( i=1; i<=gctx->max_sinc_lut; i++ ) {
		if ( gctx->sinc_lut_ptrs[i-1] != NULL ) {
			clReleaseMemObject(gctx->sinc_luts[i-1]);
			free(gctx->sinc_lut_ptrs[i-1]);
		}
	}

	clReleaseCommandQueue(gctx->cq);
	clReleaseContext(gctx->ctx);
	free(gctx);
}

/*
 * diffraction.h
 *
 * Calculate diffraction patterns by Fourier methods
 *
 * (c) 2007-2009 Thomas White <thomas.white@desy.de>
 *
 * pattern_sim - Simulate diffraction patterns from small crystals
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#ifndef DIFFRACTION_H
#define DIFFRACTION_H

#include "image.h"
#include "cell.h"

extern void get_diffraction(struct image *image, int na, int nb, int nc);
extern double water_intensity(struct threevec q, double en,
                              double beam_r, double water_r);

#endif	/* DIFFRACTION_H */

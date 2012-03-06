/*
 * reflist.h
 *
 * Fast reflection/peak list
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

#ifndef REFLIST_H
#define REFLIST_H

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

/**
 * RefList:
 *
 * A %RefList represents a list of Bragg reflections.
 *
 * This data structure is opaque.  You must use the available accessor functions
 * to read and write its contents.
 *
 **/
typedef struct _reflist RefList;

/**
 * Reflection:
 *
 * A %Reflection represents a single Bragg reflection.
 *
 * This data structure is opaque.  You must use the available accessor functions
 * to read and write its contents.
 *
 **/
typedef struct _reflection Reflection;

/**
 * RefListIterator:
 *
 * A %RefListIterator is an opaque data type used when iterating over a
 * %RefList.
 *
 **/
typedef struct _reflistiterator RefListIterator;

/* Creation/deletion */
extern RefList *reflist_new(void);
extern void reflist_free(RefList *list);
extern Reflection *reflection_new(signed int h, signed int k, signed int l);
extern void reflection_free(Reflection *refl);

/* Search */
extern Reflection *find_refl(const RefList *list, signed int h, signed int k, signed int l);
extern Reflection *next_found_refl(Reflection *refl);

/* Get */
extern double get_excitation_error(const Reflection *refl);
extern void get_detector_pos(const Reflection *refl, double *fs, double *ss);
extern double get_partiality(const Reflection *refl);
extern void get_indices(const Reflection *refl,
                        signed int *h, signed int *k, signed int *l);
extern void get_symmetric_indices(const Reflection *refl,
                                  signed int *hs, signed int *ks,
                                  signed int *ls);
extern double get_intensity(const Reflection *refl);
extern void get_partial(const Reflection *refl, double *r1, double *r2,
                        double *p, int *clamp_low, int *clamp_high);
extern int get_scalable(const Reflection *refl);
extern int get_refinable(const Reflection *refl);
extern int get_redundancy(const Reflection *refl);
extern double get_temp1(const Reflection *refl);
extern double get_temp2(const Reflection *refl);
extern double get_esd_intensity(const Reflection *refl);
extern double get_phase(const Reflection *refl, int *have_phase);

/* Set */
extern void copy_data(Reflection *to, const Reflection *from);
extern void set_detector_pos(Reflection *refl, double exerr,
                             double fs, double ss);
extern void set_partial(Reflection *refl, double r1, double r2, double p,
                        double clamp_low, double clamp_high);
extern void set_intensity(Reflection *refl, double intensity);
extern void set_scalable(Reflection *refl, int scalable);
extern void set_refinable(Reflection *refl, int refinable);
extern void set_redundancy(Reflection *refl, int red);
extern void set_temp1(Reflection *refl, double temp);
extern void set_temp2(Reflection *refl, double temp);
extern void set_esd_intensity(Reflection *refl, double esd);
extern void set_phase(Reflection *refl, double phase);
extern void set_symmetric_indices(Reflection *refl,
                                  signed int hs, signed int ks, signed int ls);

/* Insertion */
extern Reflection *add_refl(RefList *list,
                            signed int h, signed int k, signed int l);
extern Reflection *add_refl_to_list(Reflection *refl, RefList *list);

/* Iteration */
extern Reflection *first_refl(RefList *list, RefListIterator **piter);
extern Reflection *next_refl(Reflection *refl, RefListIterator *iter);

/* Misc */
extern int num_reflections(RefList *list);
extern int tree_depth(RefList *list);
extern void lock_reflection(Reflection *refl);
extern void unlock_reflection(Reflection *refl);

#endif	/* REFLIST_H */

/*--------------------------------------------------------------------
 *	$Id$
 *
 *	Copyright (c) 1991-2016 by P. Wessel, W. H. F. Smith, R. Scharroo, J. Luis and F. Wobbe
 *	See LICENSE.TXT file for copying and redistribution conditions.
 *
 *	This program is free software; you can redistribute it and/or modify
 *	it under the terms of the GNU Lesser General Public License as published by
 *	the Free Software Foundation; version 3 or any later version.
 *
 *	This program is distributed in the hope that it will be useful,
 *	but WITHOUT ANY WARRANTY; without even the implied warranty of
 *	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *	GNU Lesser General Public License for more details.
 *
 *	Contact info: gmt.soest.hawaii.edu
 *--------------------------------------------------------------------*/
/* Brief synopsis: pscontour will read a file of points in the plane, performs the
 * Delaunay triangulation, and contours these triangles.  As an option
 * the user may provide a file with indices of which vertices constitute
 * the triangles.
 *
 * Author:	Paul Wessel
 * Date:	1-JAN-2010
 * Version:	5 API
 */

#define THIS_MODULE_NAME	"pscontour"
#define THIS_MODULE_LIB		"core"
#define THIS_MODULE_PURPOSE	"Contour table data by direct triangulation"
#define THIS_MODULE_KEYS	"<D{,CC(,ED(,DDD,>X},RG-"

#include "gmt_dev.h"

#define GMT_PROG_OPTIONS "-:>BJKOPRUVXYbcdhipstxy" GMT_OPT("EMm")

struct PSCONTOUR_CTRL {
	struct GMT_CONTOUR contour;
	struct PSCONT_A {	/* -A[-][labelinfo] */
		bool active;
		unsigned int mode;	/* 1 turns off all labels */
		double interval;
		double single_cont;
	} A;
	struct PSCONT_C {	/* -C<cpt> */
		bool active;
		bool cpt;
		char *file;
		double interval;
		double single_cont;
	} C;
	struct PSCONT_D {	/* -D<dumpfile> */
		bool active;
		char *file;
	} D;
	struct PSCONT_E {	/* -E<indexfile> */
		bool active;
		char *file;
	} E;
	struct PSCONT_G {	/* -G[d|f|n|l|L|x|X]<params> */
		bool active;
	} G;
	struct PSCONT_I {	/* -I */
		bool active;
	} I;
	struct PSCONT_L {	/* -L<pen> */
		bool active;
		struct GMT_PEN pen;
	} L;
	struct PSCONT_N {	/* -N */
		bool active;
	} N;
	struct PSCONT_S {	/* -S[p|t] */
		bool active;
		unsigned int mode;	/* 0 skip points; 1 skip triangles */
	} S;
	struct PSCONT_T {	/* -T[+|-][+d<gap>[c|i|p][/<length>[c|i|p]]][+lLH|"low,high"] */
		bool active;
		bool label;
		bool low, high;	/* true to tick low and high locals */
		double dim[2];	/* spacing, length */
		char *txt[2];	/* Low and high label */
	} T;
	struct PSCONT_Q {	/* -Q<cut> */
		bool active;
		unsigned int min;
	} Q;
	struct PSCONT_W {	/* -W[+|-]<type><pen> */
		bool active;
		bool color_cont;
		bool color_text;
		struct GMT_PEN pen[2];
	} W;
};

#define PSCONTOUR_MIN_LENGTH 0.01	/* Contours shorter than this are skipped */
#define TICKED_SPACING	15.0		/* Spacing between ticked contour ticks (in points) */
#define TICKED_LENGTH	3.0		/* Length of ticked contour ticks (in points) */

struct SAVE {
	double *x, *y;
	double cval;
	unsigned int n;
	struct GMT_PEN pen;
	bool do_it, high;
};

/* Returns the id of the node common to the two edges */
#define get_node_index(edge_1, edge_2) (((pscontour_sum = (edge_1) + (edge_2)) == 1) ? 1 : ((pscontour_sum == 2) ? 0 : 2))
#define get_other_node(node1, node2) (((pscontour_sum = (node1 + node2)) == 3) ? 0 : ((pscontour_sum == 2) ? 1 : 2))	/* The other node needed */

struct PSCONTOUR_LINE {	/* Beginning and end of straight contour segment */
	double x0, y0;
	double x1, y1;
};

struct PSCONTOUR {
	double val;
	double angle;
	size_t n_alloc;
	unsigned int nl;
	bool do_tick;
	struct PSCONTOUR_LINE *L;
	char type;
};

struct PSCONTOUR_PT {
	double x, y;
	struct PSCONTOUR_PT *next;
};

struct PSCONTOUR_CHAIN {
	struct PSCONTOUR_PT *begin;
	struct PSCONTOUR_PT *end;
	struct PSCONTOUR_CHAIN *next;
};

GMT_LOCAL void *New_Ctrl (struct GMT_CTRL *GMT) {	/* Allocate and initialize a new control structure */
	struct PSCONTOUR_CTRL *C;
	
	C = gmt_memory (GMT, NULL, 1, struct PSCONTOUR_CTRL);
	
	/* Initialize values whose defaults are not 0/false/NULL */
	gmt_contlabel_init (GMT, &C->contour, 1);
	C->A.single_cont = GMT->session.d_NaN;
	C->C.single_cont = GMT->session.d_NaN;
	C->L.pen = GMT->current.setting.map_default_pen;
	C->T.dim[GMT_X] = TICKED_SPACING * GMT->session.u2u[GMT_PT][GMT_INCH];	/* 14p */
	C->T.dim[GMT_Y] = TICKED_LENGTH  * GMT->session.u2u[GMT_PT][GMT_INCH];	/* 3p */
	C->W.pen[0] = C->W.pen[1] = GMT->current.setting.map_default_pen;
	C->W.pen[1].width *= 3.0;

	return (C);
}

GMT_LOCAL void Free_Ctrl (struct GMT_CTRL *GMT, struct PSCONTOUR_CTRL *C) {	/* Deallocate control structure */
	if (!C) return;
	gmt_str_free (C->C.file);	
	gmt_str_free (C->D.file);	
	gmt_str_free (C->E.file);	
	gmt_str_free (C->T.txt[0]);	
	gmt_str_free (C->T.txt[1]);	
	gmt_free (GMT, C);	
}

GMT_LOCAL int get_triangle_crossings (struct GMT_CTRL *GMT, struct PSCONTOUR *P, unsigned int n_conts, double *x, double *y, double *z, int *ind, double small, double **xc, double **yc, double **zc, unsigned int **v, unsigned int **cindex) {
	/* This routine finds all the contour crossings for this triangle.  Each contour consists of
	 * linesegments made up of two points, with coordinates xc, yc, and contour level zc.
	 */
	 
	unsigned int i, j, k, k2, i1, nx, n_ok, *vout = NULL, *cind = NULL, *ctmp = NULL;
	bool ok;
	double xx[3], yy[3], zz[3], zmin, zmax, dz, frac, *xout = NULL, *yout = NULL, *zout = NULL, *ztmp = NULL;
	size_t n_alloc;

	xx[0] = x[ind[0]];	yy[0] = y[ind[0]];	zz[0] = z[ind[0]];
	xx[1] = x[ind[1]];	yy[1] = y[ind[1]];	zz[1] = z[ind[1]];
	xx[2] = x[ind[2]];	yy[2] = y[ind[2]];	zz[2] = z[ind[2]];

	if (GMT_is_dnan (zz[0]) || GMT_is_dnan (zz[1]) || GMT_is_dnan (zz[2])) return (0);	/* Cannot have crossings if NaNs are present */
	if (zz[0] == zz[1] && zz[1] == zz[2]) return (0);	/* Cannot have crossings if all nodes are equal */

	zmin = MIN (zz[0], MIN (zz[1], zz[2]));	/* Min z vertex */
	zmax = MAX (zz[0], MAX (zz[1], zz[2]));	/* Max z vertex */

	i = 0;	j = n_conts - 1;
	while (P[i].val < zmin && i < n_conts) i++;
	while (P[j].val > zmax && j > 0) j--;

	nx = j - i + 1;	/* Total number of contours */

	if (nx <= 0) return (0);

	n_alloc = 2 * nx;
	xout = gmt_memory (GMT, NULL, n_alloc, double);
	yout = gmt_memory (GMT, NULL, n_alloc, double);
	ztmp = gmt_memory (GMT, NULL, n_alloc, double);
	zout = gmt_memory (GMT, NULL, n_alloc, double);
	vout = gmt_memory (GMT, NULL, n_alloc, unsigned int);
	ctmp = gmt_memory (GMT, NULL, nx, unsigned int);
	cind = gmt_memory (GMT, NULL, nx, unsigned int);

	/* Fill out array zout which holds the nx contour levels */

	k = k2 = 0;
	while (i <= j) {
		ztmp[k2] = ztmp[k2+1] = P[i].val;
		ctmp[k++] = i;
		k2 += 2;
		i++;
	}

	/* Loop over the contour levels and determine the line segments */

	for (k = k2 = j = n_ok = 0; k < nx; k++, k2 += 2) {
		ok = false;
		for (i = 0; i < 3; i++) if (zz[i] == ztmp[k2]) zz[i] += small;	/* Refuse to go through nodes */
		for (i = 0; i < 3; i++) {	/* Try each side in turn 0-1, 1-2, 2-0 */
			i1 = (i == 2) ? 0 : i + 1;
			if ((ztmp[k2] >= zz[i] && ztmp[k2] < zz[i1]) || (ztmp[k2] <= zz[i] && ztmp[k2] > zz[i1])) {
				dz = zz[i1] - zz[i];
				if (dz == 0.0) {	/* Contour goes along edge */
					xout[j] = xx[i];	yout[j] = yy[i];
				}
				else {
					frac = (ztmp[k2] - zz[i]) / dz;
					xout[j] = xx[i] + frac * (xx[i1] - xx[i]);
					yout[j] = yy[i] + frac * (yy[i1] - yy[i]);
				}
				zout[j] = ztmp[k2];
				vout[j++] = i;	/* Keep track of the side number */
				ok = true;	/* Wish to add this segment */
			}
		}
		if (j%2)
			j--;	/* Contour went through a single vertex only, skip this */
		else if (ok)
			cind[n_ok++] = ctmp[k];
	}

	nx = j / 2;	/* Since j might have changed */
	if (nx) {
		*xc = xout;
		*yc = yout;
		*zc = zout;
		*v = vout;
		*cindex = cind;
	} else {
		gmt_free (GMT, xout);
		gmt_free (GMT, yout);
		gmt_free (GMT, zout);
		gmt_free (GMT, vout);
		gmt_free (GMT, cind);
	}
	gmt_free (GMT, ztmp);
	gmt_free (GMT, ctmp);
	return (nx);
}

GMT_LOCAL void paint_it_pscontour (struct GMT_CTRL *GMT, struct PSL_CTRL *PSL, struct GMT_PALETTE *P, double x[], double y[], int n, double z) {
	int index;
	double rgb[4];
	struct GMT_FILL *f = NULL;

	if (n < 3) return;	/* Need at least 3 points to make a polygon */

	index = gmt_get_rgb_from_z (GMT, P, z, rgb);
	if (P->skip) return;	/* Skip this z-slice */

	/* Now we must paint, with colors or patterns */

	if ((index >= 0 && (f = P->range[index].fill) != NULL) || (index < 0 && (f = P->patch[index+3].fill) != NULL))
		gmt_setfill (GMT, f, false);
	else
		PSL_setfill (PSL, rgb, -2);
	/* Contours drawn separately later if desired */
	PSL_plotpolygon (PSL, x, y, n);
}

GMT_LOCAL void sort_and_plot_ticks (struct GMT_CTRL *GMT, struct PSL_CTRL *PSL, struct SAVE *save, size_t n, double *x, double *y, double *z, unsigned int nn, double tick_gap, double tick_length, bool tick_low, bool tick_high, bool tick_label, char *in_lbl[], unsigned int mode, FILE *fp) {
	/* Labeling and ticking of inner-most contours cannot happen until all contours are found and we can determine
	which are the innermost ones.
	
	   Note: mode = 1 (plot only), 2 (save labels only), 3 (both).
	*/
	unsigned int np, pol, pol2, j, kk, inside, n_ticks, form;
	int way, k;
	char *lbl[2], *def[2] = {"-", "+"};
	double add, dx, dy, x_back, y_back, x_end, y_end, sa, ca, s;
	double x_mean, y_mean, a, xmin, xmax, ymin, ymax, length;

	lbl[0] = (in_lbl[0]) ? in_lbl[0] : def[0];
	lbl[1] = (in_lbl[1]) ? in_lbl[1] : def[1];

	/* The x/y coordinates in SAVE are now all projected to map inches */

	for (pol = 0; pol < n; pol++) {	/* Mark polygons that have other polygons inside them */
		np = save[pol].n;
		for (pol2 = 0; save[pol].do_it && pol2 < n; pol2++) {
			inside = gmt_non_zero_winding (GMT, save[pol2].x[0], save[pol2].y[0], save[pol].x, save[pol].y, np);
			if (inside == 2) save[pol].do_it = false;
		}
	}

	form = gmt_setfont (GMT, &GMT->current.setting.font_annot[GMT_PRIMARY]);

	/* Here, only the polygons that are innermost (containing the local max/min, will have do_it = true */

	for (pol = 0; pol < n; pol++) {
		if (!save[pol].do_it) continue;
		np = save[pol].n;

		/* Here we need to figure out if this is a local high or low. */

		/* First determine the bounding box for this contour */
		xmin = xmax = save[pol].x[0];	ymin = ymax = save[pol].y[0];
		for (j = 1; j < np; j++) {
			xmin = MIN (xmin, save[pol].x[j]);
			xmax = MAX (xmax, save[pol].x[j]);
			ymin = MIN (ymin, save[pol].y[j]);
			ymax = MAX (ymax, save[pol].y[j]);
		}
		
		/* Now try to find a data point inside this contour */
		
		for (j = 0, k = -1; k < 0 && j < nn; j++) {
			if (GMT_y_is_outside (GMT, y[j], ymin, ymax)) continue;	/* Outside y-range */
			if (GMT_y_is_outside (GMT, x[j], xmin, xmax)) continue;	/* Outside x-range (YES, use GMT_y_is_outside since projected x-coordinates)*/
			
			inside = gmt_non_zero_winding (GMT, x[j], y[j], save[pol].x, save[pol].y, np);
			if (inside == 2) k = j;	/* OK, this point is inside */
		}
		if (k < 0) continue;	/* Unable to determine */
		save[pol].high = (z[k] > save[pol].cval);

		if (save[pol].high && !tick_high) continue;	/* Do not tick highs */
		if (!save[pol].high && !tick_low) continue;	/* Do not tick lows */

		for (j = 1, s = 0.0; j < np; j++) {	/* Compute distance along the contour */
			s += hypot (save[pol].x[j]-save[pol].x[j-1], save[pol].y[j]-save[pol].y[j-1]);
		}
		if (s < PSCONTOUR_MIN_LENGTH) continue;	/* Contour is too short to be ticked or labeled */

		n_ticks = urint (floor (s / tick_gap));
		if (n_ticks == 0) continue;	/* Too short to be ticked or labeled */

		gmt_setpen (GMT, &save[pol].pen);
		way = gmt_polygon_centroid (GMT, save[pol].x, save[pol].y, np, &x_mean, &y_mean);	/* -1 is CCW, +1 is CW */
		if (tick_label) {	/* Compute mean location of closed contour ~hopefully a good point inside to place label. */
			if (mode & 1) PSL_plottext (PSL, x_mean, y_mean, GMT->current.setting.font_annot[GMT_PRIMARY].size, lbl[save[pol].high], 0.0, 6, form);
			if (mode & 2) gmt_write_label_record (GMT, fp, x_mean, y_mean, 0.0, lbl[save[pol].high], mode & 4);
		}
		if (mode & 1) {	/* Tick the innermost contour */
			add = M_PI_2 * ((save[pol].high) ? -way : +way);	/* So that tick points in the right direction */
			for (j = 1; j < np; j++) {	/* Consider each segment from point j-1 to j */
				dx = save[pol].x[j] - save[pol].x[j-1];
				dy = save[pol].y[j] - save[pol].y[j-1];
				length = hypot (dx, dy);
				n_ticks = urint (ceil (length / tick_gap));	/* At least one per side */
				a = atan2 (dy, dx) + add;
				sincos (a, &sa, &ca);
				for (kk = 0; kk <= n_ticks; kk++) {
					x_back = save[pol].x[j-1] + kk * dx / (n_ticks + 1);
					y_back = save[pol].y[j-1] + kk * dy / (n_ticks + 1);
					x_end = x_back + tick_length * ca;
					y_end = y_back + tick_length * sa;
					PSL_plotsegment (PSL, x_back, y_back, x_end, y_end);
				}
			}
		}
	}
}

GMT_LOCAL int usage (struct GMTAPI_CTRL *API, int level) {
	struct GMT_PEN P;

	GMT_show_name_and_purpose (API, THIS_MODULE_LIB, THIS_MODULE_NAME, THIS_MODULE_PURPOSE);
	if (level == GMT_MODULE_PURPOSE) return (GMT_NOERROR);
	GMT_Message (API, GMT_TIME_NONE, "usage: pscontour <table> -C[+]<cont_int>|<cpt> %s\n", GMT_J_OPT);
	GMT_Message (API, GMT_TIME_NONE, "\t%s [-A[-|[+]<annot_int>][<labelinfo>]\n\t[%s] [-D<template>] ", GMT_Rgeoz_OPT, GMT_B_OPT);
	GMT_Message (API, GMT_TIME_NONE, "[-E<indextable>] [%s] [-I] [%s] [-K] [-L<pen>] [-N]\n", GMT_CONTG, GMT_Jz_OPT);
	GMT_Message (API, GMT_TIME_NONE, "\t[-O] [-P] [-Q<cut>] [-S[p|t]] [%s]\n", GMT_CONTT);
	GMT_Message (API, GMT_TIME_NONE, "\t[%s] [-W[+]<pen>] [%s] [%s]\n", GMT_U_OPT, GMT_V_OPT, GMT_X_OPT);
	GMT_Message (API, GMT_TIME_NONE, "\t[%s] [%s] [%s] [%s]\n\t[%s] [%s]\n\t[%s] [%s]\n\t[%s] [%s]\n\n",
	             GMT_Y_OPT, GMT_b_OPT, GMT_d_OPT, GMT_c_OPT, GMT_h_OPT, GMT_i_OPT, GMT_p_OPT, GMT_t_OPT, GMT_s_OPT, GMT_colon_OPT);

	if (level == GMT_SYNOPSIS) return (EXIT_FAILURE);

	GMT_Message (API, GMT_TIME_NONE, "\t-C Contours to be drawn can be specified in one of three ways:\n");
	GMT_Message (API, GMT_TIME_NONE, "\t   1. Fixed contour interval, or a single contour if prepended with a + sign.\n");
	GMT_Message (API, GMT_TIME_NONE, "\t   2. File with contour levels in col 1 and C(ont) or A(nnot) in col 2\n");
	GMT_Message (API, GMT_TIME_NONE, "\t      [and optionally an individual annotation angle in col 3].\n");
	GMT_Message (API, GMT_TIME_NONE, "\t   3. Name of a CPT file.\n");
	GMT_Message (API, GMT_TIME_NONE, "\t   If -T is used, only contours with upper case C or A is ticked\n");
	GMT_Message (API, GMT_TIME_NONE, "\t     [CPT file contours are set to C unless the CPT flags are set;\n");
	GMT_Message (API, GMT_TIME_NONE, "\t     Use -A to force all to become A].\n");
	GMT_Option (API, "J-Z,R");
	GMT_Message (API, GMT_TIME_NONE, "\n\tOPTIONS:\n");
	GMT_Option (API, "<");
	GMT_Message (API, GMT_TIME_NONE, "\t-A Annotation label information. [Default is no annotated contours].\n");
	GMT_Message (API, GMT_TIME_NONE, "\t   Give annotation interval OR - to disable all contour annotations\n");
	GMT_Message (API, GMT_TIME_NONE, "\t   implied by the information provided in -C.\n");
	GMT_Message (API, GMT_TIME_NONE, "\t   Alternatively prepend + to annotation interval to plot that as a single contour.\n");
	GMT_Message (API, GMT_TIME_NONE, "\t   <labelinfo> controls the specifics of the labels.  Choose from:\n");
	gmt_label_syntax (API->GMT, 5, 0);
	GMT_Option (API, "B-");
	GMT_Message (API, GMT_TIME_NONE, "\t-D Dump contours as data line segments; no plotting takes place.\n");
	GMT_Message (API, GMT_TIME_NONE, "\t   Append filename template which may contain C-format specifiers.\n");
	GMT_Message (API, GMT_TIME_NONE, "\t   If no filename template is given we write all lines to stdout.\n");
	GMT_Message (API, GMT_TIME_NONE, "\t   If filename has no specifiers then we write all lines to a single file.\n");
	GMT_Message (API, GMT_TIME_NONE, "\t   If a float format (e.g., %%6.2f) is found we substitute the contour z-value.\n");
	GMT_Message (API, GMT_TIME_NONE, "\t   If an integer format (e.g., %%06d) is found we substitute a running segment count.\n");
	GMT_Message (API, GMT_TIME_NONE, "\t   If an char format (%%c) is found we substitute C or O for closed and open contours.\n");
	GMT_Message (API, GMT_TIME_NONE, "\t   The 1-3 specifiers may be combined and appear in any order to produce the\n");
	GMT_Message (API, GMT_TIME_NONE, "\t   the desired number of output files (e.g., just %%c gives two files, just %%f would.\n");
	GMT_Message (API, GMT_TIME_NONE, "\t   separate segments into one file per contour level, and %%d would write all segments.\n");
	GMT_Message (API, GMT_TIME_NONE, "\t   to individual files; see manual page for more examples.\n");
	GMT_Message (API, GMT_TIME_NONE, "\t-E File with triplets of point indices for each triangle\n");
	GMT_Message (API, GMT_TIME_NONE, "\t   [Default performs the Delaunay triangulation on xyz-data].\n");
	GMT_Message (API, GMT_TIME_NONE, "\t-G Control placement of labels along contours.  Choose among five algorithms:\n");
	gmt_cont_syntax (API->GMT, 3, 0);
	GMT_Message (API, GMT_TIME_NONE, "\t-I Color triangles using the CPT file.\n");
	GMT_Option (API, "K");
	gmt_pen_syntax (API->GMT, 'L', "Draws the triangular mesh with the specified pen.", 0);
	GMT_Message (API, GMT_TIME_NONE, "\t-N Do NOT clip contours/image at the border [Default clips].\n");
	GMT_Option (API, "O,P");
	GMT_Message (API, GMT_TIME_NONE, "\t-Q Do not draw closed contours with less than <cut> points [Draw all contours].\n");
	GMT_Message (API, GMT_TIME_NONE, "\t-S (or -Sp) Skip xyz points outside region [Default keeps all].\n");
	GMT_Message (API, GMT_TIME_NONE, "\t   Use -St to instead skip triangles whose 3 vertices are outside.\n");
	GMT_Message (API, GMT_TIME_NONE, "\t-T Will embellish innermost, closed contours with ticks pointing in\n");
	GMT_Message (API, GMT_TIME_NONE, "\t   the downward direction.  User may specify to tick only highs.\n");
	GMT_Message (API, GMT_TIME_NONE, "\t   (-T+) or lows (-T-) [-T implies both extrema].\n");
	GMT_Message (API, GMT_TIME_NONE, "\t   Append +d<spacing>[/<ticklength>] (with units) to change defaults [%gp/%gp].\n", TICKED_SPACING, TICKED_LENGTH);
	GMT_Message (API, GMT_TIME_NONE, "\t   Append +lXY (or +l\"low,high\") to place X and Y (or low and high) at the center\n");
	GMT_Message (API, GMT_TIME_NONE, "\t   of local lows and highs.  If no labels are given we default to - and +.\n");
	GMT_Message (API, GMT_TIME_NONE, "\t   If two characters are passed (e.g., +lLH) we use the as L and H.\n");
	GMT_Message (API, GMT_TIME_NONE, "\t   For string labels, simply give two strings separated by a comma (e.g., +llo,hi).\n");
	GMT_Option (API, "U,V");
	gmt_pen_syntax (API->GMT, 'W', "Set pen attributes. Append a<pen> for annotated or c<pen> for regular contours [Default].", 0);
	GMT_Message (API, GMT_TIME_NONE, "\t   The default settings are\n");
	P = API->GMT->current.setting.map_default_pen;
	GMT_Message (API, GMT_TIME_NONE, "\t   Contour pen: %s.\n", gmt_putpen (API->GMT, P));
	P.width *= 3.0;
	GMT_Message (API, GMT_TIME_NONE, "\t   Annotate pen: %s.\n", gmt_putpen (API->GMT, P));
	GMT_Message (API, GMT_TIME_NONE, "\t   Prepend + to draw colored contours based on the CPT file.\n");
	GMT_Message (API, GMT_TIME_NONE, "\t   Prepend - to color contours and annotations based on the CPT file.\n");
	GMT_Option (API, "X,bi3,bo,c,d,h,i,p,s,t,:,.");
	
	return (EXIT_FAILURE);
}

GMT_LOCAL unsigned int pscontour_old_T_parser (struct GMT_CTRL *GMT, char *arg, struct PSCONTOUR_CTRL *Ctrl) {
	/* The backwards compatible parser for old-style -T option: */
	/* -T[+|-][<gap>[c|i|p]/<length>[c|i|p]][:LH] */
	int n, j;
	unsigned int n_errors = 0;
	char txt_a[GMT_LEN256] = {""}, txt_b[GMT_LEN256] = {""};
	if (strchr (arg, '/')) {	/* Gave gap/length */
		n = sscanf (arg, "%[^/]/%[^:]", txt_a, txt_b);
		if (n == 2) {
			Ctrl->T.dim[GMT_X] = GMT_to_inch (GMT, txt_a);
			Ctrl->T.dim[GMT_Y] = GMT_to_inch (GMT, txt_b);
		}
	}
	n = 0;
	for (j = 0; arg[j] && arg[j] != ':'; j++);
	if (arg[j] == ':') Ctrl->T.label = true, j++;
	if (arg[j]) {	/* Override high/low markers */
		if (strlen (&(arg[j])) == 2) {	/* Standard :LH syntax */
			txt_a[0] = arg[j++];	txt_a[1] = '\0';
			txt_b[0] = arg[j++];	txt_b[1] = '\0';
			n = 2;
		}
		else if (strchr (&(arg[j]), ',')) {	/* Found :<labellow>,<labelhigh> */
			n = sscanf (&(arg[j]), "%[^,],%s", txt_a, txt_b);
		}
		else {
			GMT_Report (GMT->parent, GMT_MSG_NORMAL, "Syntax error -T option: Give low and high labels either as +lLH or +l<low>,<high>.\n");
			Ctrl->T.label = false;
			n_errors++;
		}
		if (Ctrl->T.label) {	/* Replace defaults */
			Ctrl->T.txt[0] = strdup (txt_a);
			Ctrl->T.txt[1] = strdup (txt_b);
		}
	}
	return (n_errors);
}

GMT_LOCAL int parse (struct GMT_CTRL *GMT, struct PSCONTOUR_CTRL *Ctrl, struct GMT_OPTION *options) {
	/* This parses the options provided to pscontour and sets parameters in Ctrl.
	 * Note Ctrl has already been initialized and non-zero default values set.
	 * Any GMT common options will override values set previously by other commands.
	 * It also replaces any file names specified as input or output with the data ID
	 * returned when registering these sources/destinations with the API.
	 */

	unsigned int n_errors = 0, id;
	int k, j, n;
	char txt_a[GMT_LEN64] = {""}, txt_b[GMT_LEN64] = {""}, string[GMT_LEN64] = {""};
	struct GMT_OPTION *opt = NULL;
	struct GMTAPI_CTRL *API = GMT->parent;

	for (opt = options; opt; opt = opt->next) {	/* Process all the options given */

		switch (opt->option) {

			case '<':	/* Skip input files */
				if (!gmt_check_filearg (GMT, '<', opt->arg, GMT_IN, GMT_IS_DATASET)) n_errors++;
				break;

			/* Processes program-specific parameters */

			case 'A':	/* Annotation control */
				Ctrl->A.active = true;
				if (gmt_contlabel_specs (GMT, opt->arg, &Ctrl->contour)) {
					GMT_Report (API, GMT_MSG_NORMAL, "Syntax error -A option: Expected\n\t-A[-|<aint>][+a<angle>|n|p[u|d]][+c<dx>[/<dy>]][+d][+e][+f<font>][+g<fill>][+j<just>][+l<label>][+n|N<dx>[/<dy>]][+o][+p<pen>][+r<min_rc>][+t[<file>]][+u<unit>][+v][+w<width>][+=<prefix>]\n");
					n_errors ++;
				}
				else if (opt->arg[0] == '-')
					Ctrl->A.mode = 1;	/* Turn off all labels */
				else if (opt->arg[0] == '+') {
					Ctrl->A.single_cont = atof (&opt->arg[1]);
					Ctrl->contour.annot = true;
				}
				else {
					Ctrl->A.interval = atof (opt->arg);
					Ctrl->contour.annot = true;
				}
				break;
			case 'C':	/* CPT table */
				Ctrl->C.active = true;
				if (GMT_File_Is_Memory (opt->arg)) {	/* Passed a memory reference from a module */
					Ctrl->C.interval = 1.0;
					Ctrl->C.cpt = true;
					gmt_str_free (Ctrl->C.file);
					Ctrl->C.file = strdup (opt->arg);
				}
				else if (!gmt_access (GMT, opt->arg, R_OK)) {	/* Gave a readable file */
					Ctrl->C.interval = 1.0;
					Ctrl->C.cpt = (!strncmp (&opt->arg[strlen(opt->arg)-4], ".cpt", 4U)) ? true : false;
					gmt_str_free (Ctrl->C.file);
					Ctrl->C.file = strdup (opt->arg);
				}
				else if (opt->arg[0] == '+')
					Ctrl->C.single_cont = atof (&opt->arg[1]);
				else
					Ctrl->C.interval = atof (opt->arg);
				break;
			case 'D':	/* Dump contours */
				Ctrl->D.active = true;
				if (opt->arg[0]) Ctrl->D.file = strdup (opt->arg);
				break;
			case 'E':	/* Triplet file */
				Ctrl->E.file = strdup (opt->arg);
				Ctrl->E.active = true;
				break;
			case 'G':	/* contour annotation settings */
				Ctrl->G.active = true;
				n_errors += gmt_contlabel_info (GMT, 'G', opt->arg, &Ctrl->contour);
				break;
			case 'I':	/* Image triangles */
				Ctrl->I.active = true;
				break;
			case 'L':	/* Draw triangular mesh lines */
				Ctrl->L.active = true;
				if (gmt_getpen (GMT, opt->arg, &Ctrl->L.pen)) {
					gmt_pen_syntax (GMT, 'L', " ", 0);
					n_errors++;
				}
				break;
			case 'N':	/* Do not clip at boundary */
				Ctrl->N.active = true;
				break;
			case 'Q':	/* Skip small closed contours */
				if (!gmt_access (GMT, opt->arg, F_OK) && GMT_compat_check (GMT, 4)) {	/* Must be the now old -Q<indexfile> option, set to -E */
					GMT_Report (API, GMT_MSG_COMPAT, "Warning: Option -Q<indexfile> is deprecated; use -E instead.\n");
					Ctrl->E.file = strdup (opt->arg);
					Ctrl->E.active = true;
					break;
				}
				Ctrl->Q.active = true;
				n = atoi (opt->arg);
				n_errors += GMT_check_condition (GMT, n < 0, "Syntax error -Q option: Value must be >= 0\n");
				Ctrl->Q.min = n;
				break;
			case 'S':	/* Skip points outside border */
				Ctrl->S.active = true;
				if (opt->arg[0] == 'p') Ctrl->S.mode = 0;
				else if (opt->arg[0] == 't') Ctrl->S.mode = 1;
				break;
			case 'T':	/* Embellish innermost closed contours */
				if (!gmt_access (GMT, opt->arg, F_OK) && GMT_compat_check (GMT, 4)) {	/* Must be the old -T<indexfile> option, set to -E */
					GMT_Report (API, GMT_MSG_COMPAT, "Warning: Option -T<indexfile> is deprecated; use -E instead.\n");
					Ctrl->E.file = strdup (opt->arg);
					Ctrl->E.active = true;
					break;
				}
				Ctrl->T.active = Ctrl->T.high = Ctrl->T.low = true;	/* Default if just -T is given */
				if (opt->arg[0]) {	/* But here we gave more options */
					if (opt->arg[0] == '+' && !strchr ("dl", opt->arg[1]))	/* Only tick local highs */
						Ctrl->T.low = false, j = 1;
					else if (opt->arg[0] == '-')		/* Only tick local lows */
						Ctrl->T.high = false, j = 1;
					else
						j = 0;
					if (strstr (opt->arg, "+d") || strstr (opt->arg, "+l")) {	/* New parser */
						if (gmt_get_modifier (opt->arg, 'd', string))
							if ((n = gmt_get_pair (GMT, string, GMT_PAIR_DIM_NODUP, Ctrl->T.dim)) < 1) n_errors++;
						if (gmt_get_modifier (opt->arg, 'l', string)) {	/* Want to label innermost contours */
							Ctrl->T.label = true;
							if (string[0] == 0)
								;	/* Use default labels */
							else if (strlen (string) == 2) {	/* Standard +lLH syntax */
								char A[2] = {0, 0};
								A[0] = string[0];	Ctrl->T.txt[0] = strdup (A);
								A[0] = string[1];	Ctrl->T.txt[1] = strdup (A);
							}
							else if (strchr (&(opt->arg[j]), ',') && (n = sscanf (&(opt->arg[j]), "%[^,],%s", txt_a, txt_b)) == 2) {	/* Found :<labellow>,<labelhigh> */
								Ctrl->T.txt[0] = strdup (txt_a);
								Ctrl->T.txt[1] = strdup (txt_b);
							}
							else {
								GMT_Report (API, GMT_MSG_NORMAL, "Syntax error -T option: Give low and high labels either as +lLH or +l<low>,<high>.\n");
								n_errors++;
							}
						}
					}
					else
						n_errors += pscontour_old_T_parser (GMT, &opt->arg[j], Ctrl);
					n_errors += GMT_check_condition (GMT, Ctrl->T.dim[GMT_X] <= 0.0 || Ctrl->T.dim[GMT_Y] == 0.0, "Syntax error -T option: Expected\n\t-T[+|-][+d<tick_gap>[%s][/<tick_length>[%s]]][+lLH], <tick_gap> must be > 0\n", GMT_DIM_UNITS_DISPLAY, GMT_DIM_UNITS_DISPLAY);
				}
				break;
			case 'W':	/* Sets pen attributes */
				Ctrl->W.active = true;
				k = 0;
				if (opt->arg[k] == '+') Ctrl->W.color_cont = true, k++;
				if (opt->arg[k] == '-') Ctrl->W.color_cont = Ctrl->W.color_text = true, k++;
				j = (opt->arg[k] == 'a' || opt->arg[k] == 'c') ? k+1 : k;
				if (j == k) {	/* Set both */
					if (gmt_getpen (GMT, &opt->arg[j], &Ctrl->W.pen[0])) {
						gmt_pen_syntax (GMT, 'W', " ", 0);
						n_errors++;
					}
					else Ctrl->W.pen[1] = Ctrl->W.pen[0];
				}
				else {	/* Gave a or c.  Because the user may say -Wcyan we must prevent this from being seen as -Wc and color yan! */
					/* Get the argument following a or c and up to first comma, slash (or to the end) */
					n = k+1;
					while (!(opt->arg[n] == ',' || opt->arg[n] == '/' || opt->arg[n] == '\0')) n++;
					strncpy (txt_a, &opt->arg[k], (size_t)(n-k));	txt_a[n-k] = '\0';
					if (gmt_colorname2index (GMT, txt_a) >= 0) j = k;	/* Found a colorname; wind j back by 1 */
					id = (opt->arg[k] == 'a') ? 1 : 0;
					if (gmt_getpen (GMT, &opt->arg[j], &Ctrl->W.pen[id])) {
						gmt_pen_syntax (GMT, 'W', " ", 0);
						n_errors++;
					}
					if (j == k) Ctrl->W.pen[1] = Ctrl->W.pen[0];	/* Must copy since it was not -Wc nor -Wa after all */
				}
				break;

			default:	/* Report bad options */
				n_errors += gmt_default_error (GMT, opt->option);
				break;
		}
	}

	if (Ctrl->A.interval > 0.0 && (!Ctrl->C.file && Ctrl->C.interval == 0.0)) Ctrl->C.interval = Ctrl->A.interval;

	/* Check that the options selected are mutually consistent */

	n_errors += GMT_check_condition (GMT, !GMT->common.J.active && !Ctrl->D.active,
	                                 "Syntax error: Must specify a map projection with the -J option\n");
	n_errors += GMT_check_condition (GMT, !GMT->common.R.active && !Ctrl->D.active, "Syntax error: Must specify a region with the -R option\n");
	n_errors += GMT_check_condition (GMT, !Ctrl->C.file && Ctrl->C.interval <= 0.0 && GMT_is_dnan (Ctrl->C.single_cont) && GMT_is_dnan (Ctrl->A.single_cont), 
	                                 "Syntax error -C option: Must specify contour interval, file name with levels, or CPT file\n");
	n_errors += GMT_check_condition (GMT, !Ctrl->D.active && !Ctrl->E.active && !(Ctrl->W.active || Ctrl->I.active),
	                                 "Syntax error: Must specify one of -W or -I\n");
	n_errors += GMT_check_condition (GMT, Ctrl->D.active && (Ctrl->I.active || Ctrl->L.active || Ctrl->N.active || Ctrl->G.active || Ctrl->W.active),
	                                 "Syntax error: Cannot use -G, -I, -L, -N, -W with -D\n");
	n_errors += GMT_check_condition (GMT, Ctrl->I.active && !Ctrl->C.file, "Syntax error -I option: Must specify a color palette table via -C\n");
	n_errors += GMT_check_condition (GMT, Ctrl->E.active && !Ctrl->E.file, "Syntax error -E option: Must specify an index file\n");
	n_errors += GMT_check_condition (GMT, Ctrl->E.active && Ctrl->E.file && gmt_access (GMT, Ctrl->E.file, F_OK),
	                                 "Syntax error -E option: Cannot find file %s\n", Ctrl->E.file);
	n_errors += GMT_check_condition (GMT, Ctrl->W.color_cont && !Ctrl->C.cpt, "Syntax error -W option: + or - only valid if -C sets a CPT file\n");
	n_errors += gmt_check_binary_io (GMT, 3);

	return (n_errors ? GMT_PARSE_ERROR : GMT_OK);
}

#define bailout(code) {GMT_Free_Options (mode); return (code);}
#define Return(code) {Free_Ctrl (GMT, Ctrl); gmt_end_module (GMT, GMT_cpy); bailout (code);}

int GMT_pscontour (void *V_API, int mode, void *args) {
	int add, error = 0;
	bool two_only = false, make_plot, skip = false, convert, get_contours, is_closed, skip_points, skip_triangles;
	
	unsigned int pscontour_sum, m, n, nx, k2, k3, node1, node2, c, cont_counts[2] = {0, 0};
	unsigned int label_mode = 0, last_entry, last_exit, fmt[3] = {0, 0, 0}, n_skipped, n_out;
	unsigned int i, low, high, n_contours = 0, n_tables = 0, tbl_scl = 0, io_mode = 0, tbl, id, *vert = NULL, *cind = NULL;
	
	size_t n_alloc, n_save = 0, n_save_alloc = 0, *n_seg_alloc = NULL, c_alloc = 0;
	
	uint64_t k, np, ij, *n_seg = NULL;
	
	int *ind = NULL;	/* Must remain int due to triangle */
	
	double xx[3], yy[3], zz[3], xout[5], yout[5], xyz[2][3], rgb[4], z_range, small;
	double *xc = NULL, *yc = NULL, *zc = NULL, *x = NULL, *y = NULL, *z = NULL;
	double current_contour = -DBL_MAX, *in = NULL, *xp = NULL, *yp = NULL;

	char cont_label[GMT_LEN256] = {""}, format[GMT_LEN256] = {""};
	char *tri_algorithm[2] = {"Watson", "Shewchuk"};

	struct PSCONTOUR *cont = NULL;
	struct GMT_DATASET *D = NULL;
	struct GMT_DATASEGMENT *S = NULL;
	struct GMT_PALETTE *P = NULL;
	struct SAVE *save = NULL;
	struct PSCONTOUR_CTRL *Ctrl = NULL;
	struct GMT_CTRL *GMT = NULL, *GMT_cpy = NULL;		/* General GMT interal parameters */
	struct GMT_OPTION *options = NULL;
	struct PSL_CTRL *PSL = NULL;		/* General PSL interal parameters */
	struct GMTAPI_CTRL *API = GMT_get_API_ptr (V_API);	/* Cast from void to GMTAPI_CTRL pointer */

	/*----------------------- Standard module initialization and parsing ----------------------*/

	if (API == NULL) return (GMT_NOT_A_SESSION);
	if (mode == GMT_MODULE_PURPOSE) return (usage (API, GMT_MODULE_PURPOSE));	/* Return the purpose of program */
	options = GMT_Create_Options (API, mode, args);	if (API->error) return (API->error);	/* Set or get option list */

	if (!options || options->option == GMT_OPT_USAGE) bailout (usage (API, GMT_USAGE));	/* Return the usage message */
	if (options->option == GMT_OPT_SYNOPSIS) bailout (usage (API, GMT_SYNOPSIS));	/* Return the synopsis */

	/* Parse the command-line arguments; return if errors are encountered */

	GMT = gmt_begin_module (API, THIS_MODULE_LIB, THIS_MODULE_NAME, &GMT_cpy); /* Save current state */
	if (GMT_Parse_Common (API, GMT_PROG_OPTIONS, options)) Return (API->error);
	Ctrl = New_Ctrl (GMT);	/* Allocate and initialize a new control structure */
	if ((error = parse (GMT, Ctrl, options)) != 0) Return (error);

	/*---------------------------- This is the pscontour main code ----------------------------*/

	GMT_Report (API, GMT_MSG_VERBOSE, "Processing input table data\n");
	if (Ctrl->D.active && !Ctrl->D.file) GMT_Report (API, GMT_MSG_VERBOSE, "Contours will be written to standard output\n");
	if ((error = gmt_set_cols (GMT, GMT_IN, 3)) != GMT_OK) {
		Return (error);
	}
	if (GMT_Init_IO (API, GMT_IS_DATASET, GMT_IS_POINT, GMT_IN, GMT_ADD_DEFAULT, 0, options) != GMT_OK) {	/* Register data input */
		Return (API->error);
	}
	if (GMT_Begin_IO (API, GMT_IS_DATASET, GMT_IN, GMT_HEADER_ON) != GMT_OK) {	/* Enables data input and sets access mode */
		Return (API->error);
	}

	if (Ctrl->C.cpt) {	/* Presumably got a CPT file; read it here so we can crash if no-such-file before we process input data */
		if ((P = GMT_Read_Data (API, GMT_IS_CPT, GMT_IS_FILE, GMT_IS_NONE, GMT_READ_NORMAL, NULL, Ctrl->C.file, NULL)) == NULL) {
			Return (API->error);
		}
		if (Ctrl->I.active && P->is_continuous) {
			GMT_Report (API, GMT_MSG_NORMAL, "-I option requires constant color between contours!\n");
			Return (GMT_OK);
		}
		if (P->categorical) {
			GMT_Report (API, GMT_MSG_NORMAL, "Warning: Categorical data (as implied by CPT file) do not have contours.  Check plot.\n");
		}
	}
	make_plot = !Ctrl->D.active;	/* Turn off plotting if -D was used */
	convert = (make_plot || (GMT->common.R.active && GMT->common.J.active));
	get_contours = (Ctrl->D.active || Ctrl->W.active);

	if (GMT->common.J.active && GMT_err_pass (GMT, gmt_map_setup (GMT, GMT->common.R.wesn), "")) Return (GMT_PROJECTION_ERROR);

	n_alloc = GMT_INITIAL_MEM_ROW_ALLOC;
	x = gmt_memory (GMT, NULL, n_alloc, double);
	y = gmt_memory (GMT, NULL, n_alloc, double);
	z = gmt_memory (GMT, NULL, n_alloc, double);

	xyz[0][GMT_Z] = DBL_MAX;	xyz[1][GMT_Z] = -DBL_MAX;
	n = 0;
	skip_points = (Ctrl->S.active && Ctrl->S.mode == 0);
	skip_triangles = (Ctrl->S.active && Ctrl->S.mode == 1);

	do {	/* Keep returning records until we reach EOF */
		if ((in = GMT_Get_Record (API, GMT_READ_DOUBLE, NULL)) == NULL) {	/* Read next record, get NULL if special case */
			if (GMT_REC_IS_ERROR (GMT)) 		/* Bail if there are any read errors */
				Return (GMT_RUNTIME_ERROR);
			if (GMT_REC_IS_ANY_HEADER (GMT)) 	/* Skip all table and segment headers */
				continue;
			if (GMT_REC_IS_EOF (GMT)) 		/* Reached end of file */
				break;
		}

		/* Data record to process */
		
		if (skip_points) {	/* Must check if points are inside plot region */
			gmt_map_outside (GMT, in[GMT_X], in[GMT_Y]);
			skip = (abs (GMT->current.map.this_x_status) > 1 || abs (GMT->current.map.this_y_status) > 1);
		}

		if (!(skip || GMT_is_dnan (in[GMT_Z]))) {	/* Unless outside or z = NaN */

			x[n] = in[GMT_X];
			y[n] = in[GMT_Y];
			z[n] = in[GMT_Z];
			if (z[n] < xyz[0][GMT_Z]) {	/* New minimum */
				xyz[0][GMT_X] = x[n];
				xyz[0][GMT_Y] = y[n];
				xyz[0][GMT_Z] = z[n];
			}
			if (z[n] > xyz[1][GMT_Z]) {	/* New maximum */
				xyz[1][GMT_X] = x[n];
				xyz[1][GMT_Y] = y[n];
				xyz[1][GMT_Z] = z[n];
			}
			n++;

			if (n == n_alloc) {
				n_alloc <<= 1;
				x = gmt_memory (GMT, x, n_alloc, double);
				y = gmt_memory (GMT, y, n_alloc, double);
				z = gmt_memory (GMT, z, n_alloc, double);
			}
			if (n == INT_MAX) {
				GMT_Report (API, GMT_MSG_NORMAL, "Error: Cannot triangulate more than %d points\n", INT_MAX);
				gmt_free (GMT, x);
				gmt_free (GMT, y);
				gmt_free (GMT, z);
				Return (EXIT_FAILURE);
			}	
		}
	} while (true);

	x = gmt_memory (GMT, x, n, double);
	y = gmt_memory (GMT, y, n, double);
	z = gmt_memory (GMT, z, n, double);

	if (n == 0) {
		GMT_Report (API, GMT_MSG_NORMAL, "Error: No data points given - so no triangulation can take effect\n");
		Return (EXIT_FAILURE);
	}

	if (make_plot && gmt_contlabel_prep (GMT, &Ctrl->contour, xyz)) Return (EXIT_FAILURE);	/* Prep for crossing lines, if any */

	/* Map transform */

	if (convert) for (i = 0; i < n; i++) gmt_geo_to_xy (GMT, x[i], y[i], &x[i], &y[i]);

	if (Ctrl->E.active) {	/* Read precalculated triangulation indices */
		uint64_t seg, row, col;
		unsigned int save_col_type[3];
		double d_n = (double)n - 0.5;	/* So we can use > in test near line 806 */
		struct GMT_DATASET *Tin = NULL;
		struct GMT_DATATABLE *T = NULL;

  		/* Must switch to Cartesian input and save whatever original input type we have since we are reading integer triplets */
  		for (k = 0; k < 3; k++) {
  			save_col_type[k] = GMT->current.io.col_type[GMT_IN][k];	/* Remember what we have */
  			GMT->current.io.col_type[GMT_IN][k] = GMT_IS_FLOAT;	/* And temporarily set to FLOAT */
  		}
		if ((Tin = GMT_Read_Data (API, GMT_IS_DATASET, GMT_IS_FILE, GMT_IS_NONE, GMT_READ_NORMAL, NULL, Ctrl->E.file, NULL)) == NULL) {
			Return (API->error);
		}
		for (k = 0; k < 3; k++) GMT->current.io.col_type[GMT_IN][k] = save_col_type[k];	/* Undo the damage above */

 		if (Tin->n_columns < 3) {	/* Trouble */
			GMT_Report (API, GMT_MSG_NORMAL, "Syntax error -E: %s does not have at least 3 columns with indices\n", Ctrl->E.file);
			if (GMT_Destroy_Data (API, &Tin) != GMT_OK) {
				Return (API->error);
			}
			Return (EXIT_FAILURE);
		}
		T = Tin->table[0];	/* Since we only have one table here */
		np = T->n_records;
		ind = gmt_memory (GMT, NULL, 3 * np, int);	/* Allocate the integer index array */
		for (seg = ij = n_skipped = 0; seg < T->n_segments; seg++) {
			for (row = 0; row < T->segment[seg]->n_rows; row++) {
				if (T->segment[seg]->coord[0][row] > d_n || T->segment[seg]->coord[1][row] > d_n || T->segment[seg]->coord[2][row] > d_n)
					n_skipped++;	/* Outside point range */
				else {
					for (col = 0; col < 3; col++) ind[ij++] = irint (T->segment[seg]->coord[col][row]);
				}
			}
		}
		np = ij / 3;	/* The actual number of vertices passing the test */
		if (GMT_Destroy_Data (API, &Tin) != GMT_OK) {
			Return (API->error);
		}
		GMT_Report (API, GMT_MSG_VERBOSE, "Read %d indices triplets from %s.\n", np, Ctrl->E.file);
		if (n_skipped) GMT_Report (API, GMT_MSG_VERBOSE, "Found %d indices triplets exceeding range of known vertices - skipped.\n", n_skipped);
	}
	else {	/* Do our own Delaunay triangulation */
		np = gmt_delaunay (GMT, x, y, n, &ind);
		GMT_Report (API, GMT_MSG_VERBOSE, "Obtained %d indices triplets via Delauney triangulation [%s].\n",
		            np, tri_algorithm[GMT->current.setting.triangulate]);
	}

	if (GMT_End_IO (API, GMT_IN, 0) != GMT_OK) {	/* Disables further data input */
		Return (API->error);
	}

	/* Determine if some triangles are outside the region and should be removed entirely */
	
	if (skip_triangles) {	/* Must check if triangles are outside plot region */
		for (k = i = n_skipped = 0; i < np; i++) {	/* For all triangles */
			k2 = (unsigned int)k;
			for (k3 = n_out = 0; k3 < 3; k3++, k++) {
				if (gmt_cart_outside (GMT, x[ind[k]], y[ind[k]])) {
					n_out++;	/* Count how many vertices are outside */
				}
			}
			if (n_out == 3) {
				ind[k2] = -1;	/* Flag so no longer to be used */
				n_skipped++;
			}
		}
		if (n_skipped) GMT_Report (API, GMT_MSG_VERBOSE, "Skipped %u triangles whose verticies are all outside the domain.\n", n_skipped);
	}
	
	if (Ctrl->C.cpt) {	/* We already read the CPT file */
		/* Set up which contours to draw based on the CPT slices and their attributes */
		cont = gmt_memory (GMT, NULL, P->n_colors + 1, struct PSCONTOUR);
		for (i = c = 0; i < P->n_colors; i++) {
			if (P->range[i].skip) continue;
			cont[c].val = P->range[i].z_low;
			if (Ctrl->A.mode)
				cont[c].type = 'C';
			else if (P->range[i].annot)
				cont[c].type = 'A';
			else
				cont[c].type = (Ctrl->contour.annot) ? 'A' : 'C';
			cont[c].type = (P->range[i].annot && !Ctrl->A.mode) ? 'A' : 'C';
			cont[c].angle = (Ctrl->contour.angle_type == 2) ? Ctrl->contour.label_angle : GMT->session.d_NaN;
			cont[c].do_tick = Ctrl->T.active;
			GMT_Report (API, GMT_MSG_DEBUG, "Contour slice %d: Value = %g type = %c angle = %g\n", c, cont[c].val, cont[c].type, cont[c].angle);
			c++;
		}
		cont[c].val = P->range[P->n_colors-1].z_high;
		if (Ctrl->A.mode)
			cont[c].type = 'C';
		else if (P->range[P->n_colors-1].annot & 2)
			cont[c].type = 'A';
		else
			cont[c].type = (Ctrl->contour.annot) ? 'A' : 'C';
		cont[c].angle = (Ctrl->contour.angle_type == 2) ? Ctrl->contour.label_angle : GMT->session.d_NaN;
		cont[c].do_tick = Ctrl->T.active;
		GMT_Report (API, GMT_MSG_DEBUG, "Contour slice %d: Value = %g type = %c angle = %g\n", c, cont[c].val, cont[c].type, cont[c].angle);
		n_contours = c + 1;
	}
	else if (Ctrl->C.file) {	/* read contour info from file with cval C|A [angle] records */
		char *record = NULL;
		int got, in_ID, NL;
		double tmp;

		/* Must register Ctrl->C.file first since we are going to read rec-by-rec from all available source */
		if ((in_ID = GMT_Register_IO (API, GMT_IS_TEXTSET, GMT_IS_FILE, GMT_IS_NONE, GMT_IN, NULL, Ctrl->C.file)) == GMT_NOTSET) {
			GMT_Report (API, GMT_MSG_NORMAL, "Error registering contour info file %s\n", Ctrl->C.file);
			Return (EXIT_FAILURE);
		}
		if (GMT_Begin_IO (API, GMT_IS_TEXTSET, GMT_IN, GMT_HEADER_ON) != GMT_OK) {	/* Enables text input and sets access mode */
			Return (API->error);
		}
		c = 0;
		do {	/* Keep returning records until we reach EOF */
			if ((record = GMT_Get_Record (API, GMT_READ_TEXT, &NL)) == NULL) {	/* Read next record, get NULL if special case */
				if (GMT_REC_IS_ERROR (GMT)) 		/* Bail if there are any read errors */
					Return (GMT_RUNTIME_ERROR);
				if (GMT_REC_IS_ANY_HEADER (GMT)) 	/* Skip all table and segment headers */
					continue;
				if (GMT_REC_IS_EOF (GMT)) 		/* Reached end of file */
					break;
			}
			if (gmt_is_a_blank_line (record)) continue;	/* Nothing in this record */

			/* Data record to process */

			if (c == c_alloc) cont = gmt_malloc (GMT, cont, c, &c_alloc, struct PSCONTOUR);
			GMT_memset (&cont[c], 1, struct PSCONTOUR);
			got = sscanf (record, "%lf %c %lf", &cont[c].val, &cont[c].type, &tmp);
			if (cont[c].type == '\0') cont[c].type = 'C';
			cont[c].do_tick = (Ctrl->T.active && ((cont[c].type == 'C') || (cont[c].type == 'A'))) ? true : false;
			cont[c].angle = (got == 3) ? tmp : GMT->session.d_NaN;
			if (got == 3) Ctrl->contour.angle_type = 2;	/* Must set this directly if angles are provided */
			cont[c].do_tick = Ctrl->T.active;
			c++;
		} while (true);
		
		if (GMT_End_IO (API, GMT_IN, 0) != GMT_OK) {	/* Disables further data input */
			Return (API->error);
		}
		n_contours = c;
	}
	else if (!GMT_is_dnan (Ctrl->C.single_cont) || !GMT_is_dnan (Ctrl->A.single_cont)) {	/* Plot one or two contours only */
		n_contours = 0;
		cont = gmt_malloc (GMT, cont, 2, &c_alloc, struct PSCONTOUR);
		if (!GMT_is_dnan (Ctrl->C.single_cont)) {
			cont[n_contours].type = 'C';
			cont[n_contours++].val = Ctrl->C.single_cont;
		}
		if (!GMT_is_dnan (Ctrl->A.single_cont)) {
			cont[n_contours].type = 'A';
			cont[n_contours].val = Ctrl->A.single_cont;
			cont[n_contours].do_tick = Ctrl->T.active;
			cont[n_contours].angle = (Ctrl->contour.angle_type == 2) ? Ctrl->contour.label_angle : GMT->session.d_NaN;
			n_contours++;
		}
	}
	else {	/* Set up contour intervals automatically from Ctrl->C.interval and Ctrl->A.interval */
		int ic;
		double min, max, aval;
		min = floor (xyz[0][GMT_Z] / Ctrl->C.interval) * Ctrl->C.interval; if (min < xyz[0][GMT_Z]) min += Ctrl->C.interval;
		max = ceil (xyz[1][GMT_Z] / Ctrl->C.interval) * Ctrl->C.interval; if (max > xyz[1][GMT_Z]) max -= Ctrl->C.interval;
		if (Ctrl->contour.annot) {	/* Want annotated contours */
			/* Determine the first annotated contour level */
			aval = floor (xyz[0][GMT_Z] / Ctrl->A.interval) * Ctrl->A.interval;
			if (aval < xyz[0][GMT_Z]) aval += Ctrl->A.interval;
		}
		else	/* No annotations, set aval outside range */
			aval = xyz[1][GMT_Z] + 1.0;
		for (ic = irint (min/Ctrl->C.interval), c = 0; ic <= irint (max/Ctrl->C.interval); ic++, c++) {
			if (c == c_alloc) cont = gmt_malloc (GMT, cont, c, &c_alloc, struct PSCONTOUR);
			GMT_memset (&cont[c], 1, struct PSCONTOUR);
			cont[c].val = ic * Ctrl->C.interval;
			if (Ctrl->contour.annot && (cont[c].val - aval) > GMT_CONV4_LIMIT) aval += Ctrl->A.interval;
			cont[c].type = (fabs (cont[c].val - aval) < GMT_CONV4_LIMIT) ? 'A' : 'C';
			cont[c].angle = (Ctrl->contour.angle_type == 2) ? Ctrl->contour.label_angle : GMT->session.d_NaN;
			cont[c].do_tick = Ctrl->T.active;
		}
		n_contours = c;
	}
	if (n_contours == 0) {
		GMT_Report (API, GMT_MSG_VERBOSE, "Warning: No contours found\n");
	}
	c_alloc = n_contours;
	cont = gmt_malloc (GMT, cont, 0, &c_alloc, struct PSCONTOUR);

	if (Ctrl->D.active) {
		uint64_t dim[4] = {0, 0, 0, 3};
		if (!Ctrl->D.file || !strchr (Ctrl->D.file, '%')) {	/* No file given or filename without C-format specifiers means a single output file */
			io_mode = GMT_WRITE_SET;
			n_tables = 1;
		}
		else {	/* Must determine the kind of output organization */
			i = 0;
			while (Ctrl->D.file[i]) {
				if (Ctrl->D.file[i++] == '%') {	/* Start of format */
					while (Ctrl->D.file[i] && !strchr ("cdf", Ctrl->D.file[i])) i++;	/* Scan past any format modifiers, like in %7.4f */
					if (Ctrl->D.file[i] == 'c') fmt[0] = i;
					if (Ctrl->D.file[i] == 'd') fmt[1] = i;
					if (Ctrl->D.file[i] == 'f') fmt[2] = i;
					i++;
				}
			}
			n_tables = 1;
			if (fmt[2]) {	/* Want files with the contour level in the name */
				if (fmt[1])	/* f+d[+c]: Write segment files named after contour level and running numbers, with or without C|O modifier */
					io_mode = GMT_WRITE_SEGMENT;
				else {	/* f[+c]: Write one table with all segments for each contour level, possibly one each for C|O */	
					io_mode = GMT_WRITE_TABLE;
					tbl_scl = (fmt[0]) ? 2 : 1;
					n_tables = n_contours * tbl_scl;
				}
			}
			else if (fmt[1])	/* d[+c]: Want individual files with running numbers only, with or without C|O modifier  */
				io_mode = GMT_WRITE_SEGMENT;
			else if (fmt[0]) {	/* c: Want two files: one for open and one for closed contours */
				io_mode = GMT_WRITE_TABLE;
				n_tables = 2;
				two_only = true;
			}
		}
		gmt_set_segmentheader (GMT, GMT_OUT, true);	/* Turn on segment headers on output */
		dim[GMT_TBL] = n_tables;
		if ((D = GMT_Create_Data (API, GMT_IS_DATASET, GMT_IS_LINE, 0, dim, NULL, NULL, 0, 0, NULL)) == NULL) Return (API->error);	/* An empty dataset */
		n_seg_alloc = gmt_memory (GMT, NULL, n_tables, size_t);
		n_seg = gmt_memory (GMT, NULL, n_tables, uint64_t);
		if ((error = gmt_set_cols (GMT, GMT_OUT, 3)) != 0) Return (error);
	}
	
	if (make_plot) {
		if (Ctrl->contour.delay) GMT->current.ps.nclip = (Ctrl->N.active) ? +1 : +2;	/* Signal that this program initiates clipping that will outlive this process */
		if ((PSL = gmt_plotinit (GMT, options)) == NULL) Return (GMT_RUNTIME_ERROR);
		gmt_plane_perspective (GMT, GMT->current.proj.z_project.view_plane, GMT->current.proj.z_level);
		gmt_plotcanvas (GMT);	/* Fill canvas if requested */
		if (Ctrl->contour.delay) gmt_map_basemap (GMT);	/* If delayed clipping the basemap must be done before clipping */
		if (!Ctrl->N.active) gmt_map_clip_on (GMT, GMT->session.no_rgb, 3);
		Ctrl->contour.line_pen = Ctrl->W.pen[0];
	}

	if (Ctrl->L.active) {	/* Draw triangular mesh */

		gmt_setpen (GMT, &Ctrl->L.pen);

		for (k = i = 0; i < np; i++) {	/* For all triangles */
			if (ind[k] < 0) { k += 3; continue; }	/* Skip triangles that are fully outside */
			xx[0] = x[ind[k]];	yy[0] = y[ind[k++]];
			xx[1] = x[ind[k]];	yy[1] = y[ind[k++]];
			xx[2] = x[ind[k]];	yy[2] = y[ind[k++]];

			PSL_plotline (PSL, xx, yy, 3, PSL_MOVE + PSL_STROKE + PSL_CLOSE);
		}
	}

	/* Get PSCONTOUR structs */

	if (get_contours) {
		for (i = 0; i < n_contours; i++) {
			cont[i].n_alloc = GMT_SMALL_CHUNK;
			cont[i].L = gmt_memory (GMT, NULL, GMT_SMALL_CHUNK, struct PSCONTOUR_LINE);
		}
	}

	z_range = xyz[1][GMT_Z] - xyz[0][GMT_Z];
	small = MIN (Ctrl->C.interval, z_range) * 1.0e-6;	/* Our float noise threshold */

	for (ij = i = 0; n_contours && i < np; i++, ij += 3) {	/* For all triangles */

		if (ind[ij] < 0) continue;	/* Skip triangles that are fully outside */

		k = ij;
		xx[0] = x[ind[k]];	yy[0] = y[ind[k]];	zz[0] = z[ind[k++]];
		xx[1] = x[ind[k]];	yy[1] = y[ind[k]];	zz[1] = z[ind[k++]];
		xx[2] = x[ind[k]];	yy[2] = y[ind[k]];	zz[2] = z[ind[k]];

		nx = get_triangle_crossings (GMT, cont, n_contours, x, y, z, &ind[ij], small, &xc, &yc, &zc, &vert, &cind);

		if (Ctrl->I.active) {	/* Must color the triangle slices according to CPT file */

			if (nx == 0) {	/* No contours go through - easy, but must check for NaNs */
				int kzz;
				double zzz;
				for (k = kzz = 0, zzz = 0.0; k < 3; k++) {
					if (GMT_is_dnan (zz[k])) continue;
					zzz += zz[k];
					kzz++;
				}
				if (kzz) paint_it_pscontour (GMT, PSL, P, xx, yy, 3, zzz / kzz);
			}
			else {	/* Must paint all those slices separately */

				/* Find vertices with the lowest and highest values */

				for (k = 1, low = high = 0; k < 3; k++) {
					if (zz[k] < zz[low])   low = (unsigned int)k;
					if (zz[k] > zz[high]) high = (unsigned int)k;
				}

				/* Paint the piece delimited by the low node and the first contour */

				xout[0] = xx[low];	yout[0] = yy[low];
				node1 = get_node_index (vert[0], vert[1]);	/* Find single vertex opposing this contour segment */
				if (node1 == low) {	/* Contour and low node make up a triangle */
					xout[1] = xc[0];	yout[1] = yc[0];
					xout[2] = xc[1];	yout[2] = yc[1];
					m = 3;
				}
				else {	/* Need the other two vertices to form a 4-sided polygon */
					node2 = get_other_node (node1, low);	/* The other node needed */
					xout[1] = xx[node2];	yout[1] = yy[node2];
					if (low == vert[0] || node2 == vert[1]) {	/* Add segment in opposite order */
						xout[2] = xc[1];	yout[2] = yc[1];
						xout[3] = xc[0];	yout[3] = yc[0];
					}
					else  {	/* Add in regular order */
						xout[2] = xc[0];	yout[2] = yc[0];
						xout[3] = xc[1];	yout[3] = yc[1];
					}
					m = 4;
				}
				paint_it_pscontour (GMT, PSL, P, xout, yout, m, 0.5 * (zz[low] + zc[1]));	/* z is contour value */

				/* Then loop over contours and paint the part between contours */

				for (k = 1, k2 = 2, k3 = 3; k < nx; k++, k2 += 2, k3 += 2) {
					xout[0] = xc[k2-2];	yout[0] = yc[k2-2];
					xout[1] = xc[k3-2];	yout[1] = yc[k3-2];
					m = 2;
					last_entry = vert[k2-2];
					last_exit  = vert[k3-2];
					if (last_exit == vert[k2]) {
						xout[m] = xc[k2];	yout[m] = yc[k2];	m++;
						xout[m] = xc[k3];	yout[m] = yc[k3];	m++;
						if (vert[k3] != last_entry) {	/* Need to add an intervening corner */
							node1 = get_node_index (last_entry, vert[k3]);	/* Find corner id */
							xout[m] = xx[node1];	yout[m] = yy[node1];	m++;
						}
					}
					else if (last_exit == vert[k3]) {
						xout[m] = xc[k3];	yout[m] = yc[k3];	m++;
						xout[m] = xc[k2];	yout[m] = yc[k2];	m++;
						if (vert[k2] != last_entry) {	/* Need to add an intervening corner */
							node1 = get_node_index (last_entry, vert[k2]);	/* Find corner id */
							xout[m] = xx[node1];	yout[m] = yy[node1];	m++;
						}
					}
					else if (last_entry == vert[k2]) {
						node1 = get_node_index (last_exit, vert[k3]);	/* Find corner id */
						xout[m] = xx[node1];	yout[m] = yy[node1];	m++;
						xout[m] = xc[k3];	yout[m] = yc[k3];	m++;
						xout[m] = xc[k2];	yout[m] = yc[k2];	m++;
					}
					else {
						node1 = get_node_index (last_exit, vert[k2]);	/* Find corner id */
						xout[m] = xx[node1];	yout[m] = yy[node1];	m++;
						xout[m] = xc[k2];	yout[m] = yc[k2];	m++;
						xout[m] = xc[k3];	yout[m] = yc[k3];	m++;
					}
					paint_it_pscontour (GMT, PSL, P, xout, yout, m, 0.5 * (zc[k2]+zc[k2-2]));
				}

				/* Add the last piece between last contour and high node */

				k2 -= 2;	k3 -= 2;
				xout[0] = xx[high];	yout[0] = yy[high];
				node1 = get_node_index (vert[k2], vert[k3]);	/* Find corner id */
				if (node1 == high) {	/* Cut off a triangular piece */
					xout[1] = xc[k2];	yout[1] = yc[k2];
					xout[2] = xc[k3];	yout[2] = yc[k3];
					m = 3;
				}
				else {	/* Need a 4-sided polygon */
					node2 = get_other_node (node1, high);	/* The other node needed */
					xout[1] = xx[node2];	yout[1] = yy[node2];
					if (high == vert[0] || node2 == vert[1]) {	/* On same side, start here */
						xout[2] = xc[k3];	yout[2] = yc[k3];
						xout[3] = xc[k2];	yout[3] = yc[k2];
					}
					else  {	/* On same side, start here */
						xout[2] = xc[k2];	yout[2] = yc[k2];
						xout[3] = xc[k3];	yout[3] = yc[k3];
					}
					m = 4;
				}
				paint_it_pscontour (GMT, PSL, P, xout, yout, m, 0.5 * (zz[high] + zc[k2]));	/* z is contour value */
			}
		}

		if (get_contours && nx > 0) {	/* Save contour line segments L for later */
			for (k = k2 = 0; k < nx; k++) {
				c = cind[k];
				m = cont[c].nl;
				cont[c].L[m].x0 = xc[k2];
				cont[c].L[m].y0 = yc[k2++];
				cont[c].L[m].x1 = xc[k2];
				cont[c].L[m].y1 = yc[k2++];
				m++;
				if (m >= cont[c].n_alloc) {
					cont[c].n_alloc <<= 1;
					cont[c].L = gmt_memory (GMT, cont[c].L, cont[c].n_alloc, struct PSCONTOUR_LINE);
				}
				cont[c].nl = m;
			}
		}

		if (nx > 0) {
			gmt_free (GMT, xc);
			gmt_free (GMT, yc);
			gmt_free (GMT, zc);
			gmt_free (GMT, vert);
			gmt_free (GMT, cind);
		}
	}
	
	/* Draw or dump contours */

	if (get_contours) {
		bool use_it;
		struct PSCONTOUR_CHAIN *head_c = NULL, *last_c = NULL, *this_c = NULL;
		struct PSCONTOUR_PT *p = NULL, *q = NULL;
				
		if (Ctrl->contour.half_width == 5) Ctrl->contour.half_width = 0;	/* Since contours are straight line segments we override the default */

		for (c = 0; c < n_contours; c++) {	/* For all selected contour levels */

			if (cont[c].nl == 0) {	/* No contours at this level */
				gmt_free (GMT, cont[c].L);
				continue;
			}

			GMT_Report (API, GMT_MSG_VERBOSE, "Tracing the %g contour\n", cont[c].val);

			id = (cont[c].type == 'A' || cont[c].type == 'a') ? 1 : 0;

			Ctrl->contour.line_pen = Ctrl->W.pen[id];	/* Load current pen into contour structure */
			if (Ctrl->W.color_cont) {	/* Override pen color according to CPT file */
				gmt_get_rgb_from_z (GMT, P, cont[c].val, rgb);
				GMT_rgb_copy (&Ctrl->contour.line_pen.rgb, rgb);
			}
			if (Ctrl->W.color_text)	/* Override label color according to CPT file */
				GMT_rgb_copy (&Ctrl->contour.font_label.fill.rgb, rgb);
			
			head_c = last_c = gmt_memory (GMT, NULL, 1, struct PSCONTOUR_CHAIN);

			while (cont[c].nl) {	/* Still more line segments at this contour level */
				/* Must hook all the segments into continuous contours. Start with first segment L */
				this_c = last_c->next = gmt_memory (GMT, NULL, 1, struct PSCONTOUR_CHAIN);
				k = 0;
				this_c->begin = gmt_memory (GMT, NULL, 1, struct PSCONTOUR_PT);
				this_c->end = gmt_memory (GMT, NULL, 1, struct PSCONTOUR_PT);
				this_c->begin->x = cont[c].L[k].x0;
				this_c->begin->y = cont[c].L[k].y0;
				this_c->end->x = cont[c].L[k].x1;
				this_c->end->y = cont[c].L[k].y1;
				this_c->begin->next = this_c->end;
				cont[c].nl--;	/* Used one segment now */
				cont[c].L[k] = cont[c].L[cont[c].nl];
				while (k < cont[c].nl) {	/* As long as there are more */
					add = 0;
					if (fabs(cont[c].L[k].x0 - this_c->begin->x) < GMT_CONV4_LIMIT && fabs(cont[c].L[k].y0 - this_c->begin->y) < GMT_CONV4_LIMIT) {	/* L matches previous */
						p = gmt_memory (GMT, NULL, 1, struct PSCONTOUR_PT);
						p->x = cont[c].L[k].x1;
						p->y = cont[c].L[k].y1;
						p->next = this_c->begin;
						add = -1;
					}
					else if (fabs(cont[c].L[k].x1 - this_c->begin->x) < GMT_CONV4_LIMIT && fabs(cont[c].L[k].y1 - this_c->begin->y) < GMT_CONV4_LIMIT) {	/* L matches previous */
						p = gmt_memory (GMT, NULL, 1, struct PSCONTOUR_PT);
						p->x = cont[c].L[k].x0;
						p->y = cont[c].L[k].y0;
						p->next = this_c->begin;
						add = -1;
					}
					else if (fabs(cont[c].L[k].x0 - this_c->end->x) < GMT_CONV4_LIMIT && fabs(cont[c].L[k].y0 - this_c->end->y) < GMT_CONV4_LIMIT) {	/* L matches previous */
						p = gmt_memory (GMT, NULL, 1, struct PSCONTOUR_PT);
						p->x = cont[c].L[k].x1;
						p->y = cont[c].L[k].y1;
						this_c->end->next = p;
						add = 1;
					}
					else if (fabs(cont[c].L[k].x1 - this_c->end->x) < GMT_CONV4_LIMIT && fabs(cont[c].L[k].y1 - this_c->end->y) < GMT_CONV4_LIMIT) {	/* L matches previous */
						p = gmt_memory (GMT, NULL, 1, struct PSCONTOUR_PT);
						p->x = cont[c].L[k].x0;
						p->y = cont[c].L[k].y0;
						this_c->end->next = p;
						add = 1;
					}
					if (add) {	/* Got one, check if we must reverse it */
						if (add == -1)
							this_c->begin = p;
						else if (add == 1)
							this_c->end = p;
						cont[c].nl--;
						cont[c].L[k] = cont[c].L[cont[c].nl];
						k = 0;
					}
					else	/* No match, go to next */
						k++;
				}
				last_c = this_c;
			}
			gmt_free (GMT, cont[c].L);	/* Done with temporary contour segment array for this level */

			/* Now, turn this linked list of segment pointers into x,y arrays */
			
			this_c = head_c->next;
			while (this_c) {
				p = this_c->begin;
				m = 1;
				while ((p = p->next) != NULL) m++;	/* Find total number of points in this linked list */
				use_it = (m >= Ctrl->Q.min);	/* True if we will use this line */

				if (use_it) {	/* Need arrays to hold the coordinates */
					xp = gmt_memory (GMT, NULL, m, double);
					yp = gmt_memory (GMT, NULL, m, double);
					m = 0;
				}
				p = this_c->begin;	/* Start over and maybe get the points this time */
				while ((q = p) != NULL) {	/* More points to add */
					if (use_it) {xp[m] = p->x; yp[m++] = p->y;}
					p = p->next;
					gmt_free (GMT, q);	/* Free linked list as we go along */
				}
				last_c = this_c;
				this_c = this_c->next;
				gmt_free (GMT, last_c);	/* Free last item */
				if (!use_it) continue;	/* No, go to next */
				
				is_closed = !gmt_polygon_is_open (GMT, xp, yp, m);

				if (current_contour != cont[c].val) {
					if (make_plot) {
						if (Ctrl->W.color_cont) {	/* Override pen color according to CPT file */
							gmt_get_rgb_from_z (GMT, P, cont[c].val, rgb);
							PSL_setcolor (PSL, rgb, PSL_IS_STROKE);
							GMT_rgb_copy (&Ctrl->contour.line_pen.rgb, rgb);
						}
						if (Ctrl->W.color_text && Ctrl->contour.curved_text) {	/* Override label color according to CPT file */
							GMT_rgb_copy (&Ctrl->contour.font_label.fill.rgb, rgb);
						}
					}
					current_contour = cont[c].val;
				}

				if (make_plot && (cont[c].type == 'A' || cont[c].type == 'a')) {	/* Annotated contours */
					gmt_get_format (GMT, cont[c].val, Ctrl->contour.unit, NULL, format);
					sprintf (cont_label, format, cont[c].val);
				}
				if (Ctrl->D.active) {
					size_t count;
					unsigned int closed;
					if (convert) {	/* Must first apply inverse map transform */
						double *xtmp = NULL, *ytmp = NULL;
						xtmp = gmt_memory (GMT, NULL, m, double);
						ytmp = gmt_memory (GMT, NULL, m, double);
						for (count = 0; count < m; count++) gmt_xy_to_geo (GMT, &xtmp[count], &ytmp[count], xp[count], yp[count]);
						S = gmt_prepare_contour (GMT, xtmp, ytmp, m, cont[c].val);
						gmt_free (GMT, xtmp);
						gmt_free (GMT, ytmp);
					}
					else
						S = gmt_prepare_contour (GMT, xp, yp, m, cont[c].val);
					/* Select which table this segment should be added to */
					closed = (is_closed) ? 1 : 0;
					tbl = (io_mode == GMT_WRITE_TABLE) ? ((two_only) ? closed : tbl_scl * c) : 0;
					if (n_seg[tbl] == n_seg_alloc[tbl]) {
						size_t n_old_alloc = n_seg_alloc[tbl];
						D->table[tbl]->segment = gmt_memory (GMT, D->table[tbl]->segment, (n_seg_alloc[tbl] += GMT_SMALL_CHUNK), struct GMT_DATASEGMENT *);
						GMT_memset (&(D->table[tbl]->segment[n_old_alloc]), n_seg_alloc[tbl] - n_old_alloc, struct GMT_DATASEGMENT *);	/* Set to NULL */
					}
					D->table[tbl]->segment[n_seg[tbl]++] = S;
					D->table[tbl]->n_segments++;	D->n_segments++;
					D->table[tbl]->n_records += m;	D->n_records += m;
					/* Generate a file name and increment cont_counts, if relevant */
					if (io_mode == GMT_WRITE_TABLE && !D->table[tbl]->file[GMT_OUT])
						D->table[tbl]->file[GMT_OUT] = gmt_make_filename (GMT, Ctrl->D.file, fmt, cont[c].val, is_closed, cont_counts);
					else if (io_mode == GMT_WRITE_SEGMENT)
						S->file[GMT_OUT] = gmt_make_filename (GMT, Ctrl->D.file, fmt, cont[c].val, is_closed, cont_counts);
				}

				if (make_plot) {
					if (cont[c].do_tick && is_closed) {	/* Must store the entire contour for later processing */
						if (n_save == n_save_alloc) save = gmt_malloc (GMT, save, n_save, &n_save_alloc, struct SAVE);
						n_alloc = 0;
						GMT_memset (&save[n_save], 1, struct SAVE);
						gmt_malloc2 (GMT, save[n_save].x, save[n_save].y, m, &n_alloc, double);
						GMT_memcpy (save[n_save].x, xp, m, double);
						GMT_memcpy (save[n_save].y, yp, m, double);
						save[n_save].n = m;
						GMT_memcpy (&save[n_save].pen, &Ctrl->W.pen[id], 1, struct GMT_PEN);
						save[n_save].do_it = true;
						save[n_save].cval = cont[c].val;
						n_save++;
					}
					gmt_hold_contour (GMT, &xp, &yp, m, cont[c].val, cont_label, cont[c].type, cont[c].angle, is_closed, true, &Ctrl->contour);
				}

				gmt_free (GMT, xp);
				gmt_free (GMT, yp);
			}
			gmt_free (GMT, head_c);
		}
		if (make_plot) label_mode |= 1;		/* Would want to plot ticks and labels if -T is set */
		if (Ctrl->contour.save_labels) {	/* Want to save the contour label locations (lon, lat, angle, label) */
			label_mode |= 2;
			if (Ctrl->contour.save_labels == 2) label_mode |= 4;
			if ((error = gmt_contlabel_save_begin (GMT, &Ctrl->contour)) != 0) Return (error);
		}
		if (Ctrl->T.active && n_save) {	/* Finally sort and plot ticked innermost contoursand plot/save L|H labels */
			size_t kk;
			save = gmt_malloc (GMT, save, 0, &n_save, struct SAVE);

			sort_and_plot_ticks (GMT, PSL, save, n_save, x, y, z, n, Ctrl->T.dim[GMT_X], Ctrl->T.dim[GMT_Y], Ctrl->T.low, Ctrl->T.high, Ctrl->T.label, Ctrl->T.txt, label_mode, Ctrl->contour.fp);
			for (kk = 0; kk < n_save; kk++) {
				gmt_free (GMT, save[kk].x);
				gmt_free (GMT, save[kk].y);
			}
			gmt_free (GMT, save);
		}
		if (make_plot) {
			gmt_contlabel_plot (GMT, &Ctrl->contour);
			gmt_contlabel_free (GMT, &Ctrl->contour);
		}
		if (Ctrl->contour.save_labels) {	/* Close file with the contour label locations (lon, lat, angle, label) */
			if ((error = gmt_contlabel_save_end (GMT, &Ctrl->contour)) != 0) Return (error);
		}
	}

	if (Ctrl->D.active) {	/* Write the contour line output file(s) */
		for (tbl = 0; tbl < D->n_tables; tbl++) D->table[tbl]->segment = gmt_memory (GMT, D->table[tbl]->segment, n_seg[tbl], struct GMT_DATASEGMENT *);
		if (GMT_Write_Data (API, GMT_IS_DATASET, GMT_IS_FILE, GMT_IS_LINE, io_mode, NULL, Ctrl->D.file, D) != GMT_OK) {
			Return (API->error);
		}
		gmt_free (GMT, n_seg_alloc);
		gmt_free (GMT, n_seg);
	}

	if (make_plot) {
		if (!(Ctrl->N.active || Ctrl->contour.delay)) gmt_map_clip_off (GMT);
		if (!Ctrl->contour.delay) gmt_map_basemap (GMT);	/* If delayed clipping the basemap is done before plotting, else here */
		gmt_plane_perspective (GMT, -1, 0.0);
		gmt_plotend (GMT);
	}

	gmt_free (GMT, x);
	gmt_free (GMT, y);
	gmt_free (GMT, z);
	gmt_free (GMT, cont);
	if (Ctrl->E.active)
		gmt_free (GMT, ind);	/* Allocated above by gmt_memory */
	else
		gmt_delaunay_free (GMT, &ind);

	Return (GMT_OK);
}

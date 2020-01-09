/* gnome-rr.c
 *
 * Copyright 2007, 2008, Red Hat, Inc.
 * 
 * This file is part of the Gnome Library.
 * 
 * The Gnome Library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * The Gnome Library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 * 
 * You should have received a copy of the GNU Library General Public
 * License along with the Gnome Library; see the file COPYING.LIB.  If not,
 * write to the Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 * 
 * Author: Soren Sandmann <sandmann@redhat.com>
 */

#define GNOME_DESKTOP_USE_UNSTABLE_API

#include <config.h>
#include <glib/gi18n-lib.h>
#include "libgnomeui/gnome-rr.h"
#include <string.h>
#include <X11/Xlib.h>
#include <X11/extensions/Xrandr.h>
#include <gtk/gtk.h>
#include <gdk/gdkx.h>
#include <X11/Xatom.h>

#include "private.h"
#include "gnome-rr-private.h"

#define DISPLAY(o) ((o)->info->screen->xdisplay)

struct GnomeRROutput
{
    ScreenInfo *	info;
    RROutput		id;
    
    char *		name;
    GnomeRRCrtc *	current_crtc;
    gboolean		connected;
    gulong		width_mm;
    gulong		height_mm;
    GnomeRRCrtc **	possible_crtcs;
    GnomeRROutput **	clones;
    GnomeRRMode **	modes;
    int			n_preferred;
    guint8 *		edid_data;
};

struct GnomeRROutputWrap
{
    RROutput		id;
};

struct GnomeRRCrtc
{
    ScreenInfo *	info;
    RRCrtc		id;
    
    GnomeRRMode *	current_mode;
    GnomeRROutput **	current_outputs;
    GnomeRROutput **	possible_outputs;
    int			x;
    int			y;
    
    GnomeRRRotation	current_rotation;
    GnomeRRRotation	rotations;
};

struct GnomeRRMode
{
    ScreenInfo *	info;
    RRMode		id;
    char *		name;
    int			width;
    int			height;
    int			freq;		/* in mHz */
};

/* GnomeRRCrtc */
static GnomeRRCrtc *  crtc_new          (ScreenInfo         *info,
					 RRCrtc              id);
static void           crtc_free         (GnomeRRCrtc        *crtc);
static gboolean       crtc_initialize   (GnomeRRCrtc        *crtc,
					 XRRScreenResources *res,
					 GError            **error);

/* GnomeRROutput */
static GnomeRROutput *output_new        (ScreenInfo         *info,
					 RROutput            id);
static gboolean       output_initialize (GnomeRROutput      *output,
					 XRRScreenResources *res,
					 GError            **error);
static void           output_free       (GnomeRROutput      *output);

/* GnomeRRMode */
static GnomeRRMode *  mode_new          (ScreenInfo         *info,
					 RRMode              id);
static void           mode_initialize   (GnomeRRMode        *mode,
					 XRRModeInfo        *info);
static void           mode_free         (GnomeRRMode        *mode);


/* Errors */

/**
 * gnome_rr_error_quark:
 *
 * Returns the #GQuark that will be used for #GError values returned by the
 * GnomeRR API.
 *
 * Return value: a #GQuark used to identify errors coming from the GnomeRR API.
 */
GQuark
gnome_rr_error_quark (void)
{
    return g_quark_from_static_string ("gnome-rr-error-quark");
}

/* Screen */
static GnomeRROutput *
gnome_rr_output_by_id (ScreenInfo *info, RROutput id)
{
    GnomeRROutput **output;
    
    g_assert (info != NULL);
    
    for (output = info->outputs; *output; ++output)
    {
	if ((*output)->id == id)
	    return *output;
    }
    
    return NULL;
}

static GnomeRRCrtc *
crtc_by_id (ScreenInfo *info, RRCrtc id)
{
    GnomeRRCrtc **crtc;
    
    if (!info)
        return NULL;
    
    for (crtc = info->crtcs; *crtc; ++crtc)
    {
	if ((*crtc)->id == id)
	    return *crtc;
    }
    
    return NULL;
}

static GnomeRRMode *
mode_by_id (ScreenInfo *info, RRMode id)
{
    GnomeRRMode **mode;
    
    g_assert (info != NULL);
    
    for (mode = info->modes; *mode; ++mode)
    {
	if ((*mode)->id == id)
	    return *mode;
    }
    
    return NULL;
}

static void
screen_info_free (ScreenInfo *info)
{
    GnomeRROutput **output;
    GnomeRRCrtc **crtc;
    GnomeRRMode **mode;
    
    g_assert (info != NULL);
    
    if (info->resources)
    {
	XRRFreeScreenResources (info->resources);
	
	info->resources = NULL;
    }
    
    if (info->outputs)
    {
	for (output = info->outputs; *output; ++output)
	    output_free (*output);
	g_free (info->outputs);
    }
    
    if (info->crtcs)
    {
	for (crtc = info->crtcs; *crtc; ++crtc)
	    crtc_free (*crtc);
	g_free (info->crtcs);
    }
    
    if (info->modes)
    {
	for (mode = info->modes; *mode; ++mode)
	    mode_free (*mode);
	g_free (info->modes);
    }

    if (info->clone_modes)
    {
	/* The modes themselves were freed above */
	g_free (info->clone_modes);
    }
    
    g_free (info);
}

static gboolean
has_similar_mode (GnomeRROutput *output, GnomeRRMode *mode)
{
    int i;
    GnomeRRMode **modes = gnome_rr_output_list_modes (output);
    int width = gnome_rr_mode_get_width (mode);
    int height = gnome_rr_mode_get_height (mode);

    for (i = 0; modes[i] != NULL; ++i)
    {
	GnomeRRMode *m = modes[i];

	if (gnome_rr_mode_get_width (m) == width	&&
	    gnome_rr_mode_get_height (m) == height)
	{
	    return TRUE;
	}
    }

    return FALSE;
}

static void
gather_clone_modes (ScreenInfo *info)
{
    int i;
    GPtrArray *result = g_ptr_array_new ();

    for (i = 0; info->outputs[i] != NULL; ++i)
    {
	int j;
	GnomeRROutput *output1, *output2;

	output1 = info->outputs[i];
	
	if (!output1->connected)
	    continue;
	
	for (j = 0; output1->modes[j] != NULL; ++j)
	{
	    GnomeRRMode *mode = output1->modes[j];
	    gboolean valid;
	    int k;

	    valid = TRUE;
	    for (k = 0; info->outputs[k] != NULL; ++k)
	    {
		output2 = info->outputs[k];
		
		if (!output2->connected)
		    continue;
		
		if (!has_similar_mode (output2, mode))
		{
		    valid = FALSE;
		    break;
		}
	    }

	    if (valid)
		g_ptr_array_add (result, mode);
	}
    }

    g_ptr_array_add (result, NULL);
    
    info->clone_modes = (GnomeRRMode **)g_ptr_array_free (result, FALSE);
}

static gboolean
fill_screen_info_from_resources (ScreenInfo *info,
				 XRRScreenResources *resources,
				 GError **error)
{
    int i;
    GPtrArray *a;
    GnomeRRCrtc **crtc;
    GnomeRROutput **output;

    info->resources = resources;

    /* We create all the structures before initializing them, so
     * that they can refer to each other.
     */
    a = g_ptr_array_new ();
    for (i = 0; i < resources->ncrtc; ++i)
    {
	GnomeRRCrtc *crtc = crtc_new (info, resources->crtcs[i]);

	g_ptr_array_add (a, crtc);
    }
    g_ptr_array_add (a, NULL);
    info->crtcs = (GnomeRRCrtc **)g_ptr_array_free (a, FALSE);

    a = g_ptr_array_new ();
    for (i = 0; i < resources->noutput; ++i)
    {
	GnomeRROutput *output = output_new (info, resources->outputs[i]);

	g_ptr_array_add (a, output);
    }
    g_ptr_array_add (a, NULL);
    info->outputs = (GnomeRROutput **)g_ptr_array_free (a, FALSE);

    a = g_ptr_array_new ();
    for (i = 0;  i < resources->nmode; ++i)
    {
	GnomeRRMode *mode = mode_new (info, resources->modes[i].id);

	g_ptr_array_add (a, mode);
    }
    g_ptr_array_add (a, NULL);
    info->modes = (GnomeRRMode **)g_ptr_array_free (a, FALSE);

    /* Initialize */
    for (crtc = info->crtcs; *crtc; ++crtc)
    {
	if (!crtc_initialize (*crtc, resources, error))
	    return FALSE;
    }

    for (output = info->outputs; *output; ++output)
    {
	if (!output_initialize (*output, resources, error))
	    return FALSE;
    }

    for (i = 0; i < resources->nmode; ++i)
    {
	GnomeRRMode *mode = mode_by_id (info, resources->modes[i].id);

	mode_initialize (mode, &(resources->modes[i]));
    }

    gather_clone_modes (info);

    return TRUE;
}

static gboolean
fill_out_screen_info (Display *xdisplay,
		      Window xroot,
		      ScreenInfo *info,
		      gboolean needs_reprobe,
		      GError **error)
{
    XRRScreenResources *resources;
    
    g_assert (xdisplay != NULL);
    g_assert (info != NULL);

    /* First update the screen resources */

    if (needs_reprobe)
        resources = XRRGetScreenResources (xdisplay, xroot);
    else
    {
	/* XRRGetScreenResourcesCurrent is less expensive than
	 * XRRGetScreenResources, however it is available only
	 * in RandR 1.3 or higher
	 */
#if (RANDR_MAJOR > 1 || (RANDR_MAJOR == 1 && RANDR_MINOR >= 3))
        /* Runtime check for RandR 1.3 or higher */
        if (info->screen->rr_major_version == 1 && info->screen->rr_minor_version >= 3)
            resources = XRRGetScreenResourcesCurrent (xdisplay, xroot);
        else
            resources = XRRGetScreenResources (xdisplay, xroot);
#else
        resources = XRRGetScreenResources (xdisplay, xroot);
#endif
    }

    if (resources)
    {
	if (!fill_screen_info_from_resources (info, resources, error))
	    return FALSE;
    }
    else
    {
	g_set_error (error, GNOME_RR_ERROR, GNOME_RR_ERROR_RANDR_ERROR,
		     _("could not get the screen resources (CRTCs, outputs, modes)"));
	return FALSE;
    }

    /* Then update the screen size range.  We do this after XRRGetScreenResources() so that
     * the X server will already have an updated view of the outputs.
     */

    if (needs_reprobe) {
	gboolean success;

        gdk_error_trap_push ();
	success = XRRGetScreenSizeRange (xdisplay, xroot,
					 &(info->min_width),
					 &(info->min_height),
					 &(info->max_width),
					 &(info->max_height));
	gdk_flush ();
	if (gdk_error_trap_pop ()) {
	    g_set_error (error, GNOME_RR_ERROR, GNOME_RR_ERROR_UNKNOWN,
			 _("unhandled X error while getting the range of screen sizes"));
	    return FALSE;
	}

	if (!success) {
	    g_set_error (error, GNOME_RR_ERROR, GNOME_RR_ERROR_RANDR_ERROR,
			 _("could not get the range of screen sizes"));
            return FALSE;
        }
    }
    else
    {
        gnome_rr_screen_get_ranges (info->screen, 
					 &(info->min_width),
					 &(info->max_width),
					 &(info->min_height),
					 &(info->max_height));
    }

    info->primary = None;
#if (RANDR_MAJOR > 1 || (RANDR_MAJOR == 1 && RANDR_MINOR >= 3))
    /* Runtime check for RandR 1.3 or higher */
    if (info->screen->rr_major_version == 1 && info->screen->rr_minor_version >= 3) {
        gdk_error_trap_push ();
        info->primary = XRRGetOutputPrimary (xdisplay, xroot);
	gdk_flush ();
	gdk_error_trap_pop (); /* ignore error */
    }
#endif

    return TRUE;
}

static ScreenInfo *
screen_info_new (GnomeRRScreen *screen, gboolean needs_reprobe, GError **error)
{
    ScreenInfo *info = g_new0 (ScreenInfo, 1);
    
    g_assert (screen != NULL);
    
    info->outputs = NULL;
    info->crtcs = NULL;
    info->modes = NULL;
    info->screen = screen;
    
    if (fill_out_screen_info (screen->xdisplay, screen->xroot, info, needs_reprobe, error))
    {
	return info;
    }
    else
    {
	screen_info_free (info);
	return NULL;
    }
}

static gboolean
screen_update (GnomeRRScreen *screen, gboolean force_callback, gboolean needs_reprobe, GError **error)
{
    ScreenInfo *info;
    gboolean changed = FALSE;
    
    g_assert (screen != NULL);

    info = screen_info_new (screen, needs_reprobe, error);
    if (!info)
	    return FALSE;

    if (info->resources->configTimestamp != screen->info->resources->configTimestamp)
	    changed = TRUE;
	
    screen_info_free (screen->info);
	
    screen->info = info;

    if ((changed || force_callback) && screen->callback)
	screen->callback (screen, screen->data);
    
    return changed;
}

static GdkFilterReturn
screen_on_event (GdkXEvent *xevent,
		 GdkEvent *event,
		 gpointer data)
{
    GnomeRRScreen *screen = data;
    XEvent *e = xevent;
    int event_num;

    if (!e)
	return GDK_FILTER_CONTINUE;

    event_num = e->type - screen->randr_event_base;

    if (event_num == RRScreenChangeNotify) {
	/* We don't reprobe the hardware; we just fetch the X server's latest
	 * state.  The server already knows the new state of the outputs; that's
	 * why it sent us an event!
	 */
        screen_update (screen, TRUE, FALSE, NULL); /* NULL-GError */
#if 0
	/* Enable this code to get a dialog showing the RANDR timestamps, for debugging purposes */
	{
	    GtkWidget *dialog;
	    XRRScreenChangeNotifyEvent *rr_event;
	    static int dialog_num;

	    rr_event = (XRRScreenChangeNotifyEvent *) e;

	    dialog = gtk_message_dialog_new (NULL,
					     0,
					     GTK_MESSAGE_INFO,
					     GTK_BUTTONS_CLOSE,
					     "RRScreenChangeNotify timestamps (%d):\n"
					     "event change: %u\n"
					     "event config: %u\n"
					     "event serial: %lu\n"
					     "----------------------"
					     "screen change: %u\n"
					     "screen config: %u\n",
					     dialog_num++,
					     (guint32) rr_event->timestamp,
					     (guint32) rr_event->config_timestamp,
					     rr_event->serial,
					     (guint32) screen->info->resources->timestamp,
					     (guint32) screen->info->resources->configTimestamp);
	    g_signal_connect (dialog, "response",
			      G_CALLBACK (gtk_widget_destroy), NULL);
	    gtk_widget_show (dialog);
	}
#endif
    }
#if 0
    /* WHY THIS CODE IS DISABLED:
     *
     * Note that in gnome_rr_screen_new(), we only select for
     * RRScreenChangeNotifyMask.  We used to select for other values in
     * RR*NotifyMask, but we weren't really doing anything useful with those
     * events.  We only care about "the screens changed in some way or another"
     * for now.
     *
     * If we ever run into a situtation that could benefit from processing more
     * detailed events, we can enable this code again.
     *
     * Note that the X server sends RRScreenChangeNotify in conjunction with the
     * more detailed events from RANDR 1.2 - see xserver/randr/randr.c:TellChanged().
     */
    else if (event_num == RRNotify)
    {
	/* Other RandR events */

	XRRNotifyEvent *event = (XRRNotifyEvent *)e;

	/* Here we can distinguish between RRNotify events supported
	 * since RandR 1.2 such as RRNotify_OutputProperty.  For now, we
	 * don't have anything special to do for particular subevent types, so
	 * we leave this as an empty switch().
	 */
	switch (event->subtype)
	{
	default:
	    break;
	}

	/* No need to reprobe hardware here */
	screen_update (screen, TRUE, FALSE, NULL); /* NULL-GError */
    }
#endif

    /* Pass the event on to GTK+ */
    return GDK_FILTER_CONTINUE;
}

/* Returns NULL if screen could not be created.  For instance, if
 * the driver does not support Xrandr 1.2.
 */
GnomeRRScreen *
gnome_rr_screen_new (GdkScreen *gdk_screen,
		     GnomeRRScreenChanged callback,
		     gpointer data,
		     GError **error)
{
    Display *dpy = GDK_SCREEN_XDISPLAY (gdk_screen);
    int event_base;
    int ignore;

    g_return_val_if_fail (error == NULL || *error == NULL, NULL);
    
    _gnome_desktop_init_i18n ();

    if (XRRQueryExtension (dpy, &event_base, &ignore))
    {
	GnomeRRScreen *screen = g_new0 (GnomeRRScreen, 1);
	
	screen->gdk_screen = gdk_screen;
	screen->gdk_root = gdk_screen_get_root_window (gdk_screen);
	screen->xroot = gdk_x11_drawable_get_xid (screen->gdk_root);
	screen->xdisplay = dpy;
	screen->xscreen = gdk_x11_screen_get_xscreen (screen->gdk_screen);
	
	screen->callback = callback;
	screen->data = data;
	
	screen->randr_event_base = event_base;

	XRRQueryVersion (dpy, &screen->rr_major_version, &screen->rr_minor_version);

	screen->info = screen_info_new (screen, TRUE, error);
	
	if (!screen->info) {
	    g_free (screen);
	    return NULL;
	}

	if (screen->callback) {
	    XRRSelectInput (screen->xdisplay,
			    screen->xroot,
			    RRScreenChangeNotifyMask);

	    gdk_x11_register_standard_event_type (gdk_screen_get_display (gdk_screen),
						  event_base,
						  RRNotify + 1);

	    gdk_window_add_filter (screen->gdk_root, screen_on_event, screen);
	}

	return screen;
    }
    else
    {
	g_set_error (error, GNOME_RR_ERROR, GNOME_RR_ERROR_NO_RANDR_EXTENSION,
		     _("RANDR extension is not present"));
	
	return NULL;
   }
}

void
gnome_rr_screen_destroy (GnomeRRScreen *screen)
{
	g_return_if_fail (screen != NULL);

	gdk_window_remove_filter (screen->gdk_root, screen_on_event, screen);

	screen_info_free (screen->info);
	screen->info = NULL;

	g_free (screen);
}

void
gnome_rr_screen_set_size (GnomeRRScreen *screen,
			  int	      width,
			  int       height,
			  int       mm_width,
			  int       mm_height)
{
    g_return_if_fail (screen != NULL);
    
    XRRSetScreenSize (screen->xdisplay, screen->xroot,
		      width, height, mm_width, mm_height);
}

void
gnome_rr_screen_get_ranges (GnomeRRScreen *screen,
			    int	          *min_width,
			    int	          *max_width,
			    int           *min_height,
			    int	          *max_height)
{
    g_return_if_fail (screen != NULL);
    
    if (min_width)
	*min_width = screen->info->min_width;
    
    if (max_width)
	*max_width = screen->info->max_width;
    
    if (min_height)
	*min_height = screen->info->min_height;
    
    if (max_height)
	*max_height = screen->info->max_height;
}

/**
 * gnome_rr_screen_get_timestamps
 * @screen: a #GnomeRRScreen
 * @change_timestamp_ret: Location in which to store the timestamp at which the RANDR configuration was last changed
 * @config_timestamp_ret: Location in which to store the timestamp at which the RANDR configuration was last obtained
 *
 * Queries the two timestamps that the X RANDR extension maintains.  The X
 * server will prevent change requests for stale configurations, those whose
 * timestamp is not equal to that of the latest request for configuration.  The
 * X server will also prevent change requests that have an older timestamp to
 * the latest change request.
 */
void
gnome_rr_screen_get_timestamps (GnomeRRScreen *screen,
				guint32       *change_timestamp_ret,
				guint32       *config_timestamp_ret)
{
    g_return_if_fail (screen != NULL);

    if (change_timestamp_ret)
	*change_timestamp_ret = screen->info->resources->timestamp;

    if (config_timestamp_ret)
	*config_timestamp_ret = screen->info->resources->configTimestamp;
}


/**
 * gnome_rr_screen_refresh
 * @screen: a #GnomeRRScreen
 * @error: location to store error, or %NULL
 *
 * Refreshes the screen configuration, and calls the screen's callback if it
 * exists and if the screen's configuration changed.
 *
 * Return value: TRUE if the screen's configuration changed; otherwise, the
 * function returns FALSE and a NULL error if the configuration didn't change,
 * or FALSE and a non-NULL error if there was an error while refreshing the
 * configuration.
 */
gboolean
gnome_rr_screen_refresh (GnomeRRScreen *screen,
			 GError       **error)
{
    g_return_val_if_fail (error == NULL || *error == NULL, FALSE);
    return screen_update (screen, FALSE, TRUE, error);
}

GnomeRRMode **
gnome_rr_screen_list_modes (GnomeRRScreen *screen)
{
    g_return_val_if_fail (screen != NULL, NULL);
    g_return_val_if_fail (screen->info != NULL, NULL);
    
    return screen->info->modes;
}

GnomeRRMode **
gnome_rr_screen_list_clone_modes   (GnomeRRScreen *screen)
{
    g_return_val_if_fail (screen != NULL, NULL);
    g_return_val_if_fail (screen->info != NULL, NULL);

    return screen->info->clone_modes;
}

GnomeRRCrtc **
gnome_rr_screen_list_crtcs (GnomeRRScreen *screen)
{
    g_return_val_if_fail (screen != NULL, NULL);
    g_return_val_if_fail (screen->info != NULL, NULL);
    
    return screen->info->crtcs;
}

GnomeRROutput **
gnome_rr_screen_list_outputs (GnomeRRScreen *screen)
{
    g_return_val_if_fail (screen != NULL, NULL);
    g_return_val_if_fail (screen->info != NULL, NULL);
    
    return screen->info->outputs;
}

GnomeRRCrtc *
gnome_rr_screen_get_crtc_by_id (GnomeRRScreen *screen,
				guint32        id)
{
    int i;
    
    g_return_val_if_fail (screen != NULL, NULL);
    g_return_val_if_fail (screen->info != NULL, NULL);
    
    for (i = 0; screen->info->crtcs[i] != NULL; ++i)
    {
	if (screen->info->crtcs[i]->id == id)
	    return screen->info->crtcs[i];
    }
    
    return NULL;
}

GnomeRROutput *
gnome_rr_screen_get_output_by_id (GnomeRRScreen *screen,
				  guint32        id)
{
    int i;
    
    g_return_val_if_fail (screen != NULL, NULL);
    g_return_val_if_fail (screen->info != NULL, NULL);
    
    for (i = 0; screen->info->outputs[i] != NULL; ++i)
    {
	if (screen->info->outputs[i]->id == id)
	    return screen->info->outputs[i];
    }
    
    return NULL;
}

/* GnomeRROutput */
static GnomeRROutput *
output_new (ScreenInfo *info, RROutput id)
{
    GnomeRROutput *output = g_new0 (GnomeRROutput, 1);
    
    output->id = id;
    output->info = info;
    
    return output;
}

static guint8 *
get_property (Display *dpy,
	      RROutput output,
	      Atom atom,
	      int *len)
{
    unsigned char *prop;
    int actual_format;
    unsigned long nitems, bytes_after;
    Atom actual_type;
    guint8 *result;
    
    XRRGetOutputProperty (dpy, output, atom,
			  0, 100, False, False,
			  AnyPropertyType,
			  &actual_type, &actual_format,
			  &nitems, &bytes_after, &prop);
    
    if (actual_type == XA_INTEGER && actual_format == 8)
    {
	result = g_memdup (prop, nitems);
	if (len)
	    *len = nitems;
    }
    else
    {
	result = NULL;
    }
    
    XFree (prop);
    
    return result;
}

static guint8 *
read_edid_data (GnomeRROutput *output)
{
    Atom edid_atom;
    guint8 *result;
    int len;

    edid_atom = XInternAtom (DISPLAY (output), "EDID", FALSE);
    result = get_property (DISPLAY (output),
			   output->id, edid_atom, &len);

    if (!result)
    {
	edid_atom = XInternAtom (DISPLAY (output), "EDID_DATA", FALSE);
	result = get_property (DISPLAY (output),
			       output->id, edid_atom, &len);
    }

    if (result)
    {
	if (len % 128 == 0)
	    return result;
	else
	    g_free (result);
    }
    
    return NULL;
}

static gboolean
output_initialize (GnomeRROutput *output, XRRScreenResources *res, GError **error)
{
    XRROutputInfo *info = XRRGetOutputInfo (
	DISPLAY (output), res, output->id);
    GPtrArray *a;
    int i;
    
#if 0
    g_print ("Output %lx Timestamp: %u\n", output->id, (guint32)info->timestamp);
#endif
    
    if (!info || !output->info)
    {
	/* FIXME: see the comment in crtc_initialize() */
	/* Translators: here, an "output" is a video output */
	g_set_error (error, GNOME_RR_ERROR, GNOME_RR_ERROR_RANDR_ERROR,
		     _("could not get information about output %d"),
		     (int) output->id);
	return FALSE;
    }
    
    output->name = g_strdup (info->name); /* FIXME: what is nameLen used for? */
    output->current_crtc = crtc_by_id (output->info, info->crtc);
    output->width_mm = info->mm_width;
    output->height_mm = info->mm_height;
    output->connected = (info->connection == RR_Connected);
    
    /* Possible crtcs */
    a = g_ptr_array_new ();
    
    for (i = 0; i < info->ncrtc; ++i)
    {
	GnomeRRCrtc *crtc = crtc_by_id (output->info, info->crtcs[i]);
	
	if (crtc)
	    g_ptr_array_add (a, crtc);
    }
    g_ptr_array_add (a, NULL);
    output->possible_crtcs = (GnomeRRCrtc **)g_ptr_array_free (a, FALSE);
    
    /* Clones */
    a = g_ptr_array_new ();
    for (i = 0; i < info->nclone; ++i)
    {
	GnomeRROutput *gnome_rr_output = gnome_rr_output_by_id (output->info, info->clones[i]);
	
	if (gnome_rr_output)
	    g_ptr_array_add (a, gnome_rr_output);
    }
    g_ptr_array_add (a, NULL);
    output->clones = (GnomeRROutput **)g_ptr_array_free (a, FALSE);
    
    /* Modes */
    a = g_ptr_array_new ();
    for (i = 0; i < info->nmode; ++i)
    {
	GnomeRRMode *mode = mode_by_id (output->info, info->modes[i]);
	
	if (mode)
	    g_ptr_array_add (a, mode);
    }
    g_ptr_array_add (a, NULL);
    output->modes = (GnomeRRMode **)g_ptr_array_free (a, FALSE);
    
    output->n_preferred = info->npreferred;
    
    /* Edid data */
    output->edid_data = read_edid_data (output);
    
    XRRFreeOutputInfo (info);

    return TRUE;
}

static void
output_free (GnomeRROutput *output)
{
    g_free (output->clones);
    g_free (output->modes);
    g_free (output->possible_crtcs);
    g_free (output->edid_data);
    g_free (output->name);
    g_free (output);
}

guint32
gnome_rr_output_get_id (GnomeRROutput *output)
{
    g_assert(output != NULL);
    
    return output->id;
}

const guint8 *
gnome_rr_output_get_edid_data (GnomeRROutput *output)
{
    g_return_val_if_fail (output != NULL, NULL);
    
    return output->edid_data;
}

GnomeRROutput *
gnome_rr_screen_get_output_by_name (GnomeRRScreen *screen,
				    const char    *name)
{
    int i;
    
    g_return_val_if_fail (screen != NULL, NULL);
    g_return_val_if_fail (screen->info != NULL, NULL);
    
    for (i = 0; screen->info->outputs[i] != NULL; ++i)
    {
	GnomeRROutput *output = screen->info->outputs[i];
	
	if (strcmp (output->name, name) == 0)
	    return output;
    }
    
    return NULL;
}

GnomeRRCrtc *
gnome_rr_output_get_crtc (GnomeRROutput *output)
{
    g_return_val_if_fail (output != NULL, NULL);
    
    return output->current_crtc;
}

GnomeRRMode *
gnome_rr_output_get_current_mode (GnomeRROutput *output)
{
    GnomeRRCrtc *crtc;
    
    g_return_val_if_fail (output != NULL, NULL);
    
    if ((crtc = gnome_rr_output_get_crtc (output)))
	return gnome_rr_crtc_get_current_mode (crtc);
    
    return NULL;
}

void
gnome_rr_output_get_position (GnomeRROutput   *output,
			      int             *x,
			      int             *y)
{
    GnomeRRCrtc *crtc;
    
    g_return_if_fail (output != NULL);
    
    if ((crtc = gnome_rr_output_get_crtc (output)))
	gnome_rr_crtc_get_position (crtc, x, y);
}

const char *
gnome_rr_output_get_name (GnomeRROutput *output)
{
    g_assert (output != NULL);
    return output->name;
}

int
gnome_rr_output_get_width_mm (GnomeRROutput *output)
{
    g_assert (output != NULL);
    return output->width_mm;
}

int
gnome_rr_output_get_height_mm (GnomeRROutput *output)
{
    g_assert (output != NULL);
    return output->height_mm;
}

GnomeRRMode *
gnome_rr_output_get_preferred_mode (GnomeRROutput *output)
{
    g_return_val_if_fail (output != NULL, NULL);
    if (output->n_preferred)
	return output->modes[0];
    
    return NULL;
}

GnomeRRMode **
gnome_rr_output_list_modes (GnomeRROutput *output)
{
    g_return_val_if_fail (output != NULL, NULL);
    return output->modes;
}

gboolean
gnome_rr_output_is_connected (GnomeRROutput *output)
{
    g_return_val_if_fail (output != NULL, FALSE);
    return output->connected;
}

gboolean
gnome_rr_output_supports_mode (GnomeRROutput *output,
			       GnomeRRMode   *mode)
{
    int i;
    
    g_return_val_if_fail (output != NULL, FALSE);
    g_return_val_if_fail (mode != NULL, FALSE);
    
    for (i = 0; output->modes[i] != NULL; ++i)
    {
	if (output->modes[i] == mode)
	    return TRUE;
    }
    
    return FALSE;
}

gboolean
gnome_rr_output_can_clone (GnomeRROutput *output,
			   GnomeRROutput *clone)
{
    int i;
    
    g_return_val_if_fail (output != NULL, FALSE);
    g_return_val_if_fail (clone != NULL, FALSE);
    
    for (i = 0; output->clones[i] != NULL; ++i)
    {
	if (output->clones[i] == clone)
	    return TRUE;
    }
    
    return FALSE;
}

gboolean
gnome_rr_output_get_is_primary (GnomeRROutput *output)
{
    return output->info->primary == output->id;
}

void
gnome_rr_screen_set_primary_output (GnomeRRScreen *screen,
                                    GnomeRROutput *output)
{
#if (RANDR_MAJOR > 1 || (RANDR_MAJOR == 1 && RANDR_MINOR >= 3))
    RROutput id;

    if (output)
        id = output->id;
    else
        id = None;

        /* Runtime check for RandR 1.3 or higher */
    if (screen->rr_major_version == 1 && screen->rr_minor_version >= 3)
        XRRSetOutputPrimary (screen->xdisplay, screen->xroot, id);
#endif
}

/* GnomeRRCrtc */
typedef struct
{
    Rotation xrot;
    GnomeRRRotation rot;
} RotationMap;

static const RotationMap rotation_map[] =
{
    { RR_Rotate_0, GNOME_RR_ROTATION_0 },
    { RR_Rotate_90, GNOME_RR_ROTATION_90 },
    { RR_Rotate_180, GNOME_RR_ROTATION_180 },
    { RR_Rotate_270, GNOME_RR_ROTATION_270 },
    { RR_Reflect_X, GNOME_RR_REFLECT_X },
    { RR_Reflect_Y, GNOME_RR_REFLECT_Y },
};

static GnomeRRRotation
gnome_rr_rotation_from_xrotation (Rotation r)
{
    int i;
    GnomeRRRotation result = 0;
    
    for (i = 0; i < G_N_ELEMENTS (rotation_map); ++i)
    {
	if (r & rotation_map[i].xrot)
	    result |= rotation_map[i].rot;
    }
    
    return result;
}

static Rotation
xrotation_from_rotation (GnomeRRRotation r)
{
    int i;
    Rotation result = 0;
    
    for (i = 0; i < G_N_ELEMENTS (rotation_map); ++i)
    {
	if (r & rotation_map[i].rot)
	    result |= rotation_map[i].xrot;
    }
    
    return result;
}

#ifndef GNOME_DISABLE_DEPRECATED_SOURCE
gboolean
gnome_rr_crtc_set_config (GnomeRRCrtc      *crtc,
			  int               x,
			  int               y,
			  GnomeRRMode      *mode,
			  GnomeRRRotation   rotation,
			  GnomeRROutput   **outputs,
			  int               n_outputs,
			  GError          **error)
{
    return gnome_rr_crtc_set_config_with_time (crtc, GDK_CURRENT_TIME, x, y, mode, rotation, outputs, n_outputs, error);
}
#endif

gboolean
gnome_rr_crtc_set_config_with_time (GnomeRRCrtc      *crtc,
				    guint32           timestamp,
				    int               x,
				    int               y,
				    GnomeRRMode      *mode,
				    GnomeRRRotation   rotation,
				    GnomeRROutput   **outputs,
				    int               n_outputs,
				    GError          **error)
{
    ScreenInfo *info;
    GArray *output_ids;
    Status status;
    gboolean result;
    int i;
    
    g_return_val_if_fail (crtc != NULL, FALSE);
    g_return_val_if_fail (mode != NULL || outputs == NULL || n_outputs == 0, FALSE);
    g_return_val_if_fail (error == NULL || *error == NULL, FALSE);
    
    info = crtc->info;
    
    if (mode)
    {
	if (x + mode->width > info->max_width
	    || y + mode->height > info->max_height)
	{
	    g_set_error (error, GNOME_RR_ERROR, GNOME_RR_ERROR_BOUNDS_ERROR,
			 /* Translators: the "position", "size", and "maximum"
			  * words here are not keywords; please translate them
			  * as usual.  A CRTC is a CRT Controller (this is X terminology) */
			 _("requested position/size for CRTC %d is outside the allowed limit: "
			   "position=(%d, %d), size=(%d, %d), maximum=(%d, %d)"),
			 (int) crtc->id,
			 x, y,
			 mode->width, mode->height,
			 info->max_width, info->max_height);
	    return FALSE;
	}
    }
    
    output_ids = g_array_new (FALSE, FALSE, sizeof (RROutput));
    
    if (outputs)
    {
	for (i = 0; i < n_outputs; ++i)
	    g_array_append_val (output_ids, outputs[i]->id);
    }
    
    status = XRRSetCrtcConfig (DISPLAY (crtc), info->resources, crtc->id,
			       timestamp, 
			       x, y,
			       mode ? mode->id : None,
			       xrotation_from_rotation (rotation),
			       (RROutput *)output_ids->data,
			       output_ids->len);
    
    g_array_free (output_ids, TRUE);

    if (status == RRSetConfigSuccess)
	result = TRUE;
    else {
	result = FALSE;
	/* Translators: CRTC is a CRT Controller (this is X terminology).
	 * It is *very* unlikely that you'll ever get this error, so it is
	 * only listed for completeness. */
	g_set_error (error, GNOME_RR_ERROR, GNOME_RR_ERROR_RANDR_ERROR,
		     _("could not set the configuration for CRTC %d"),
		     (int) crtc->id);
    }
    
    return result;
}

GnomeRRMode *
gnome_rr_crtc_get_current_mode (GnomeRRCrtc *crtc)
{
    g_return_val_if_fail (crtc != NULL, NULL);
    
    return crtc->current_mode;
}

guint32
gnome_rr_crtc_get_id (GnomeRRCrtc *crtc)
{
    g_return_val_if_fail (crtc != NULL, 0);
    
    return crtc->id;
}

gboolean
gnome_rr_crtc_can_drive_output (GnomeRRCrtc   *crtc,
				GnomeRROutput *output)
{
    int i;
    
    g_return_val_if_fail (crtc != NULL, FALSE);
    g_return_val_if_fail (output != NULL, FALSE);
    
    for (i = 0; crtc->possible_outputs[i] != NULL; ++i)
    {
	if (crtc->possible_outputs[i] == output)
	    return TRUE;
    }
    
    return FALSE;
}

/* FIXME: merge with get_mode()? */
void
gnome_rr_crtc_get_position (GnomeRRCrtc *crtc,
			    int         *x,
			    int         *y)
{
    g_return_if_fail (crtc != NULL);
    
    if (x)
	*x = crtc->x;
    
    if (y)
	*y = crtc->y;
}

/* FIXME: merge with get_mode()? */
GnomeRRRotation
gnome_rr_crtc_get_current_rotation (GnomeRRCrtc *crtc)
{
    g_assert(crtc != NULL);
    return crtc->current_rotation;
}

GnomeRRRotation
gnome_rr_crtc_get_rotations (GnomeRRCrtc *crtc)
{
    g_assert(crtc != NULL);
    return crtc->rotations;
}

gboolean
gnome_rr_crtc_supports_rotation (GnomeRRCrtc *   crtc,
				 GnomeRRRotation rotation)
{
    g_return_val_if_fail (crtc != NULL, FALSE);
    return (crtc->rotations & rotation);
}

static GnomeRRCrtc *
crtc_new (ScreenInfo *info, RROutput id)
{
    GnomeRRCrtc *crtc = g_new0 (GnomeRRCrtc, 1);
    
    crtc->id = id;
    crtc->info = info;
    
    return crtc;
}

static gboolean
crtc_initialize (GnomeRRCrtc        *crtc,
		 XRRScreenResources *res,
		 GError            **error)
{
    XRRCrtcInfo *info = XRRGetCrtcInfo (DISPLAY (crtc), res, crtc->id);
    GPtrArray *a;
    int i;
    
#if 0
    g_print ("CRTC %lx Timestamp: %u\n", crtc->id, (guint32)info->timestamp);
#endif
    
    if (!info)
    {
	/* FIXME: We need to reaquire the screen resources */
	/* FIXME: can we actually catch BadRRCrtc, and does it make sense to emit that? */

	/* Translators: CRTC is a CRT Controller (this is X terminology).
	 * It is *very* unlikely that you'll ever get this error, so it is
	 * only listed for completeness. */
	g_set_error (error, GNOME_RR_ERROR, GNOME_RR_ERROR_RANDR_ERROR,
		     _("could not get information about CRTC %d"),
		     (int) crtc->id);
	return FALSE;
    }
    
    /* GnomeRRMode */
    crtc->current_mode = mode_by_id (crtc->info, info->mode);
    
    crtc->x = info->x;
    crtc->y = info->y;
    
    /* Current outputs */
    a = g_ptr_array_new ();
    for (i = 0; i < info->noutput; ++i)
    {
	GnomeRROutput *output = gnome_rr_output_by_id (crtc->info, info->outputs[i]);
	
	if (output)
	    g_ptr_array_add (a, output);
    }
    g_ptr_array_add (a, NULL);
    crtc->current_outputs = (GnomeRROutput **)g_ptr_array_free (a, FALSE);
    
    /* Possible outputs */
    a = g_ptr_array_new ();
    for (i = 0; i < info->npossible; ++i)
    {
	GnomeRROutput *output = gnome_rr_output_by_id (crtc->info, info->possible[i]);
	
	if (output)
	    g_ptr_array_add (a, output);
    }
    g_ptr_array_add (a, NULL);
    crtc->possible_outputs = (GnomeRROutput **)g_ptr_array_free (a, FALSE);
    
    /* Rotations */
    crtc->current_rotation = gnome_rr_rotation_from_xrotation (info->rotation);
    crtc->rotations = gnome_rr_rotation_from_xrotation (info->rotations);
    
    XRRFreeCrtcInfo (info);

    return TRUE;
}

static void
crtc_free (GnomeRRCrtc *crtc)
{
    g_free (crtc->current_outputs);
    g_free (crtc->possible_outputs);
    g_free (crtc);
}

/* GnomeRRMode */
static GnomeRRMode *
mode_new (ScreenInfo *info, RRMode id)
{
    GnomeRRMode *mode = g_new0 (GnomeRRMode, 1);
    
    mode->id = id;
    mode->info = info;
    
    return mode;
}

guint32
gnome_rr_mode_get_id (GnomeRRMode *mode)
{
    g_return_val_if_fail (mode != NULL, 0);
    return mode->id;
}

guint
gnome_rr_mode_get_width (GnomeRRMode *mode)
{
    g_return_val_if_fail (mode != NULL, 0);
    return mode->width;
}

int
gnome_rr_mode_get_freq (GnomeRRMode *mode)
{
    g_return_val_if_fail (mode != NULL, 0);
    return (mode->freq) / 1000;
}

guint
gnome_rr_mode_get_height (GnomeRRMode *mode)
{
    g_return_val_if_fail (mode != NULL, 0);
    return mode->height;
}

static void
mode_initialize (GnomeRRMode *mode, XRRModeInfo *info)
{
    g_assert (mode != NULL);
    g_assert (info != NULL);
    
    mode->name = g_strdup (info->name);
    mode->width = info->width;
    mode->height = info->height;
    mode->freq = ((info->dotClock / (double)info->hTotal) / info->vTotal + 0.5) * 1000;
}

static void
mode_free (GnomeRRMode *mode)
{
    g_free (mode->name);
    g_free (mode);
}

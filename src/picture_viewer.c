/*
 *  Copyright (C) Stephan Arts 2006-2009 <stephan@xfce.org>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU Library General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA 02111-1307, USA.
 *
 *  Drag-n-Drop support taken from Thunar, written by Benedict Meurer
 */

#include <config.h>
#include <gtk/gtk.h>
#include <gtk/gtkmarshal.h>
#include <string.h>
#include <gio/gio.h>
#include <libexif/exif-data.h>

#include "image.h"
#include "image_list.h"
#include "picture_viewer.h"
#include "settings.h"

typedef enum
{
    RSTTO_PICTURE_VIEWER_STATE_NORMAL = 0,
    RSTTO_PICTURE_VIEWER_STATE_PREVIEW
} RsttoPictureViewerState;

typedef enum
{
    RSTTO_PICTURE_VIEWER_MOTION_STATE_NORMAL = 0,
    RSTTO_PICTURE_VIEWER_MOTION_STATE_BOX_ZOOM,
    RSTTO_PICTURE_VIEWER_MOTION_STATE_MOVE
} RsttoPictureViewerMotionState;

typedef enum
{
    RSTTO_ZOOM_MODE_CUSTOM,
    RSTTO_ZOOM_MODE_100,
    RSTTO_ZOOM_MODE_FIT
} RsttoZoomMode;

enum
{
    TARGET_TEXT_URI_LIST,
};

static const GtkTargetEntry drop_targets[] = {
    {"text/uri-list", 0, TARGET_TEXT_URI_LIST},
};


struct _RsttoPictureViewerPriv
{
    RsttoImage              *image;
    RsttoImageListIter      *iter;
    GtkMenu                 *menu;
    RsttoPictureViewerState  state;
    RsttoZoomMode            zoom_mode;


    GdkPixbuf        *dst_pixbuf; /* The pixbuf which ends up on screen */
    void             (*cb_value_changed)(GtkAdjustment *, RsttoPictureViewer *);
    GdkColor         *bg_color;

    struct
    {
        gdouble x;
        gdouble y;
        gdouble current_x;
        gdouble current_y;
        gint h_val;
        gint v_val;
        RsttoPictureViewerMotionState state;
    } motion;

    struct
    {
        gint idle_id;
        gboolean refresh;
    } repaint;
};

static void
rstto_picture_viewer_init(RsttoPictureViewer *);
static void
rstto_picture_viewer_class_init(RsttoPictureViewerClass *);
static void
rstto_picture_viewer_destroy(GtkObject *object);

static void
rstto_picture_viewer_set_state (RsttoPictureViewer *viewer, RsttoPictureViewerState state);
static RsttoPictureViewerState
rstto_picture_viewer_get_state (RsttoPictureViewer *viewer);
static void
rstto_picture_viewer_set_motion_state (RsttoPictureViewer *viewer, RsttoPictureViewerMotionState state);
static RsttoPictureViewerMotionState
rstto_picture_viewer_get_motion_state (RsttoPictureViewer *viewer);

static void
rstto_picture_viewer_set_zoom_mode (RsttoPictureViewer *viewer, RsttoZoomMode mode);

static void
rstto_picture_viewer_size_request(GtkWidget *, GtkRequisition *);
static void
rstto_picture_viewer_size_allocate(GtkWidget *, GtkAllocation *);
static void
rstto_picture_viewer_realize(GtkWidget *);
static gboolean 
rstto_picture_viewer_expose(GtkWidget *, GdkEventExpose *);
static void
rstto_picture_viewer_paint (GtkWidget *widget);
static void 
rstto_picture_viewer_queued_repaint (RsttoPictureViewer *viewer, gboolean refresh);

static gboolean
rstto_picture_viewer_set_scroll_adjustments(RsttoPictureViewer *, GtkAdjustment *, GtkAdjustment *);

static void
rstto_marshal_VOID__OBJECT_OBJECT (GClosure     *closure,
                                   GValue       *return_value,
                                   guint         n_param_values,
                                   const GValue *param_values,
                                   gpointer      invocation_hint,
                                   gpointer      marshal_data);

static void
cb_rstto_picture_viewer_value_changed(GtkAdjustment *, RsttoPictureViewer *);
static void
cb_rstto_picture_viewer_nav_iter_changed (RsttoImageListIter *iter, gpointer user_data);

static void
cb_rstto_picture_viewer_image_updated (RsttoImage *image, RsttoPictureViewer *viewer);
static void
cb_rstto_picture_viewer_image_prepared (RsttoImage *image, RsttoPictureViewer *viewer);

static gboolean 
cb_rstto_picture_viewer_queued_repaint (RsttoPictureViewer *viewer);

static void
cb_rstto_picture_viewer_scroll_event (RsttoPictureViewer *viewer, GdkEventScroll *event);
static void
cb_rstto_picture_viewer_button_press_event (RsttoPictureViewer *viewer, GdkEventButton *event);
static void
cb_rstto_picture_viewer_button_release_event (RsttoPictureViewer *viewer, GdkEventButton *event);
static gboolean 
cb_rstto_picture_viewer_motion_notify_event (RsttoPictureViewer *viewer,
                                             GdkEventMotion *event,
                                             gpointer user_data);
static void
cb_rstto_picture_viewer_popup_menu (RsttoPictureViewer *viewer, gboolean user_data);

static GtkWidgetClass *parent_class = NULL;

GType
rstto_picture_viewer_get_type (void)
{
    static GType rstto_picture_viewer_type = 0;

    if (!rstto_picture_viewer_type)
    {
        static const GTypeInfo rstto_picture_viewer_info = 
        {
            sizeof (RsttoPictureViewerClass),
            (GBaseInitFunc) NULL,
            (GBaseFinalizeFunc) NULL,
            (GClassInitFunc) rstto_picture_viewer_class_init,
            (GClassFinalizeFunc) NULL,
            NULL,
            sizeof (RsttoPictureViewer),
            0,
            (GInstanceInitFunc) rstto_picture_viewer_init,
            NULL
        };

        rstto_picture_viewer_type = g_type_register_static (GTK_TYPE_WIDGET, "RsttoPictureViewer", &rstto_picture_viewer_info, 0);
    }
    return rstto_picture_viewer_type;
}

static void
rstto_picture_viewer_init(RsttoPictureViewer *viewer)
{
    viewer->priv = g_new0(RsttoPictureViewerPriv, 1);
    viewer->priv->cb_value_changed = cb_rstto_picture_viewer_value_changed;

    viewer->priv->dst_pixbuf = NULL;
    viewer->priv->zoom_mode = RSTTO_ZOOM_MODE_CUSTOM;
    gtk_widget_set_redraw_on_allocate(GTK_WIDGET(viewer), TRUE);
    gtk_widget_set_events (GTK_WIDGET(viewer),
                           GDK_BUTTON_PRESS_MASK |
                           GDK_BUTTON_RELEASE_MASK |
                           GDK_BUTTON1_MOTION_MASK |
                           GDK_POINTER_MOTION_MASK);

    g_signal_connect(G_OBJECT(viewer), "scroll_event", G_CALLBACK(cb_rstto_picture_viewer_scroll_event), NULL);
    g_signal_connect(G_OBJECT(viewer), "button_press_event", G_CALLBACK(cb_rstto_picture_viewer_button_press_event), NULL);
    g_signal_connect(G_OBJECT(viewer), "button_release_event", G_CALLBACK(cb_rstto_picture_viewer_button_release_event), NULL);
    g_signal_connect(G_OBJECT(viewer), "motion_notify_event", G_CALLBACK(cb_rstto_picture_viewer_motion_notify_event), NULL);
    g_signal_connect(G_OBJECT(viewer), "popup-menu", G_CALLBACK(cb_rstto_picture_viewer_popup_menu), NULL);

    gtk_drag_dest_set(GTK_WIDGET(viewer), 0, drop_targets, G_N_ELEMENTS(drop_targets),
                      GDK_ACTION_COPY | GDK_ACTION_LINK | GDK_ACTION_MOVE | GDK_ACTION_PRIVATE);
}

/**
 * rstto_marshal_VOID__OBJECT_OBJECT:
 * @closure:
 * @return_value:
 * @n_param_values:
 * @param_values:
 * @invocation_hint:
 * @marshal_data:
 *
 * A marshaller for the set_scroll_adjustments signal.
 */
static void
rstto_marshal_VOID__OBJECT_OBJECT (GClosure     *closure,
                                   GValue       *return_value,
                                   guint         n_param_values,
                                   const GValue *param_values,
                                   gpointer      invocation_hint,
                                   gpointer      marshal_data)
{
    typedef void (*GMarshalFunc_VOID__OBJECT_OBJECT) (gpointer data1,
                                                      gpointer arg_1,
                                                      gpointer arg_2,
                                                      gpointer data2);
    register GMarshalFunc_VOID__OBJECT_OBJECT callback;
    register GCClosure *cc = (GCClosure*) closure;
    register gpointer data1, data2;

    g_return_if_fail (n_param_values == 3);

    if (G_CCLOSURE_SWAP_DATA (closure))
    {
        data1 = closure->data;
        data2 = g_value_get_object (param_values + 0);
    }
    else
    {
        data1 = g_value_get_object (param_values + 0);
        data2 = closure->data;
    }
    callback = (GMarshalFunc_VOID__OBJECT_OBJECT) (marshal_data ?
    marshal_data : cc->callback);

    callback (data1,
              g_value_get_object (param_values + 1),
              g_value_get_object (param_values + 2),
              data2);
}

/**
 * rstto_picture_viewer_class_init:
 * @viewer_class:
 *
 * Initialize pictureviewer class
 */
static void
rstto_picture_viewer_class_init(RsttoPictureViewerClass *viewer_class)
{
    GtkWidgetClass *widget_class;
    GtkObjectClass *object_class;

    widget_class = (GtkWidgetClass*)viewer_class;
    object_class = (GtkObjectClass*)viewer_class;

    parent_class = g_type_class_peek_parent(viewer_class);

    viewer_class->set_scroll_adjustments = rstto_picture_viewer_set_scroll_adjustments;

    widget_class->realize = rstto_picture_viewer_realize;
    widget_class->expose_event = rstto_picture_viewer_expose;
    widget_class->size_request = rstto_picture_viewer_size_request;
    widget_class->size_allocate = rstto_picture_viewer_size_allocate;

    object_class->destroy = rstto_picture_viewer_destroy;

    widget_class->set_scroll_adjustments_signal =
                  g_signal_new ("set_scroll_adjustments",
                                G_TYPE_FROM_CLASS (object_class),
                                G_SIGNAL_RUN_LAST | G_SIGNAL_ACTION,
                                G_STRUCT_OFFSET (RsttoPictureViewerClass, set_scroll_adjustments),
                                NULL, NULL,
                                rstto_marshal_VOID__OBJECT_OBJECT,
                                G_TYPE_NONE, 2,
                                GTK_TYPE_ADJUSTMENT,
                                GTK_TYPE_ADJUSTMENT);
}

/**
 * rstto_picture_viewer_realize:
 * @widget:
 *
 */
static void
rstto_picture_viewer_realize(GtkWidget *widget)
{
    GdkWindowAttr attributes;
    gint attributes_mask;

    g_return_if_fail (widget != NULL);
    g_return_if_fail (RSTTO_IS_PICTURE_VIEWER(widget));

    GTK_WIDGET_SET_FLAGS (widget, GTK_REALIZED);

    attributes.x = widget->allocation.x;
    attributes.y = widget->allocation.y;
    attributes.width = widget->allocation.width;
    attributes.height = widget->allocation.height;
    attributes.wclass = GDK_INPUT_OUTPUT;
    attributes.window_type = GDK_WINDOW_CHILD;
    attributes.event_mask = gtk_widget_get_events (widget) | 
    GDK_EXPOSURE_MASK | GDK_BUTTON_PRESS_MASK;
    attributes.visual = gtk_widget_get_visual (widget);
    attributes.colormap = gtk_widget_get_colormap (widget);

    attributes_mask = GDK_WA_X | GDK_WA_Y | GDK_WA_VISUAL | GDK_WA_COLORMAP;
    widget->window = gdk_window_new (gtk_widget_get_parent_window(widget), &attributes, attributes_mask);

    widget->style = gtk_style_attach (widget->style, widget->window);
    gdk_window_set_user_data (widget->window, widget);

    gtk_style_set_background (widget->style, widget->window, GTK_STATE_ACTIVE);
}

/**
 * rstto_picture_viewer_size_request:
 * @widget:
 * @requisition:
 *
 * Request a default size of 300 by 400 pixels
 */
static void
rstto_picture_viewer_size_request(GtkWidget *widget, GtkRequisition *requisition)
{
    requisition->width = 400;
    requisition->height= 300;
}


/**
 * rstto_picture_viewer_size_allocate:
 * @widget:
 * @allocation:
 *
 *
 */
static void
rstto_picture_viewer_size_allocate(GtkWidget *widget, GtkAllocation *allocation)
{
    RsttoPictureViewer *viewer = RSTTO_PICTURE_VIEWER(widget);
    gint border_width =  0;
    widget->allocation = *allocation;

    if (GTK_WIDGET_REALIZED (widget))
    {
         gdk_window_move_resize (widget->window,
            allocation->x + border_width,
            allocation->y + border_width,
            allocation->width - border_width * 2,
            allocation->height - border_width * 2);
    }

    /** 
     * TODO: Check if we really nead a refresh
     */
    rstto_picture_viewer_queued_repaint (viewer, TRUE);
}

/**
 * rstto_picture_viewer_expose:
 * @widget:
 * @event:
 *
 */
static gboolean
rstto_picture_viewer_expose(GtkWidget *widget, GdkEventExpose *event)
{
    RsttoPictureViewer *viewer = RSTTO_PICTURE_VIEWER (widget);

    /** 
     * TODO: Check if we really nead a refresh
     */
    rstto_picture_viewer_queued_repaint (viewer, TRUE);
    return FALSE;
}

/**
 * rstto_picture_viewer_paint:
 * @widget:
 *
 * Paint the picture_viewer widget contents
 */
static void
rstto_picture_viewer_paint (GtkWidget *widget)
{
    RsttoSettings *settings_manager = rstto_settings_new();
    RsttoPictureViewer *viewer = RSTTO_PICTURE_VIEWER(widget);
    GdkPixbuf *pixbuf = viewer->priv->dst_pixbuf;
    GdkColor color;
    GdkColor line_color;
    gint i, a, height, width;
    GdkColor *bg_color = NULL;
    gdouble m_x1, m_x2, m_y1, m_y2;
    gint x1, x2, y1, y2;
    GValue val_bg_color = {0, }, val_bg_color_override = {0, }, val_bg_color_fs = {0, };
    g_value_init (&val_bg_color, GDK_TYPE_COLOR);
    g_value_init (&val_bg_color_fs, GDK_TYPE_COLOR);
    g_value_init (&val_bg_color_override, G_TYPE_BOOLEAN);

    g_object_get_property (G_OBJECT(settings_manager), "bgcolor", &val_bg_color);
    g_object_get_property (G_OBJECT(settings_manager), "bgcolor-override", &val_bg_color_override);

    g_object_get_property (G_OBJECT(settings_manager), "bgcolor-fullscreen", &val_bg_color_fs);


    color.pixel = 0x0;
    line_color.pixel = 0x0;

    /* required for transparent pixbufs... add double buffering to fix flickering*/
    if(GTK_WIDGET_REALIZED(widget))
    {          
        GdkPixmap *buffer = gdk_pixmap_new(NULL, widget->allocation.width, widget->allocation.height, gdk_drawable_get_depth(widget->window));
        GdkGC *gc = gdk_gc_new(GDK_DRAWABLE(buffer));

        if(gdk_window_get_state(gdk_window_get_toplevel(GTK_WIDGET(viewer)->window)) & GDK_WINDOW_STATE_FULLSCREEN)
        {
           bg_color = g_value_get_boxed (&val_bg_color_fs);
        }
        else
        {
            if (g_value_get_boxed (&val_bg_color) && g_value_get_boolean (&val_bg_color_override))
            {
                bg_color = g_value_get_boxed (&val_bg_color);
            }
            else
            {
                bg_color = &(widget->style->bg[GTK_STATE_NORMAL]);
            }
        }
        gdk_colormap_alloc_color (gdk_gc_get_colormap (gc), bg_color, FALSE, TRUE);
        gdk_gc_set_rgb_fg_color (gc, bg_color);

        gdk_draw_rectangle(GDK_DRAWABLE(buffer), gc, TRUE, 0, 0, widget->allocation.width, widget->allocation.height);

        /* Check if there is a destination pixbuf */
        if(pixbuf)
        {
            x1 = (widget->allocation.width-gdk_pixbuf_get_width(pixbuf))<0?0:(widget->allocation.width-gdk_pixbuf_get_width(pixbuf))/2;
            y1 = (widget->allocation.height-gdk_pixbuf_get_height(pixbuf))<0?0:(widget->allocation.height-gdk_pixbuf_get_height(pixbuf))/2;
            x2 = gdk_pixbuf_get_width(pixbuf);
            y2 = gdk_pixbuf_get_height(pixbuf);
            
            /* We only need to paint a checkered background if the image is transparent */
            if(gdk_pixbuf_get_has_alpha(pixbuf))
            {
                for(i = 0; i <= x2/10; i++)
                {
                    if(i == x2/10)
                    {
                        width = x2-10*i;
                    }
                    else
                    {   
                        width = 10;
                    }
                    for(a = 0; a <= y2/10; a++)
                    {
                        if(a%2?i%2:!(i%2))
                            color.pixel = 0xcccccccc;
                        else
                            color.pixel = 0xdddddddd;
                        gdk_gc_set_foreground(gc, &color);
                        if(a == y2/10)
                        {
                            height = y2-10*a;
                        }
                        else
                        {   
                            height = 10;
                        }

                        gdk_draw_rectangle(GDK_DRAWABLE(buffer),
                                        gc,
                                        TRUE,
                                        x1+10*i,
                                        y1+10*a,
                                        width,
                                        height);
                    }
                }
            }
            gdk_draw_pixbuf(GDK_DRAWABLE(buffer), 
                            NULL, 
                            pixbuf,
                            0,
                            0,
                            x1,
                            y1,
                            x2, 
                            y2,
                            GDK_RGB_DITHER_NONE,
                            0,0);
            if(viewer->priv->motion.state == RSTTO_PICTURE_VIEWER_MOTION_STATE_BOX_ZOOM)
            {
                gdk_gc_set_foreground(gc,
                        &(widget->style->fg[GTK_STATE_SELECTED]));

                if (viewer->priv->motion.x < viewer->priv->motion.current_x)
                {
                    m_x1 = viewer->priv->motion.x;
                    m_x2 = viewer->priv->motion.current_x;
                }
                else
                {
                    m_x1 = viewer->priv->motion.current_x;
                    m_x2 = viewer->priv->motion.x;
                }
                if (viewer->priv->motion.y < viewer->priv->motion.current_y)
                {
                    m_y1 = viewer->priv->motion.y;
                    m_y2 = viewer->priv->motion.current_y;
                }
                else
                {
                    m_y1 = viewer->priv->motion.current_y;
                    m_y2 = viewer->priv->motion.y;
                }
                if (m_y1 < y1)
                    m_y1 = y1;
                if (m_x1 < x1)
                    m_x1 = x1;

                if (m_x2 > x2 + x1)
                    m_x2 = x2 + x1;
                if (m_y2 > y2 + y1)
                    m_y2 = y2 + y1;

                if ((m_x2 - m_x1 >= 2) && (m_y2 - m_y1 >= 2))
                {
                    GdkPixbuf *sub = gdk_pixbuf_new_subpixbuf(pixbuf,
                                                              m_x1-x1,
                                                              m_y1-y1,
                                                              m_x2-m_x1,
                                                              m_y2-m_y1);
                    if(sub)
                    {
                        sub = gdk_pixbuf_composite_color_simple(sub,
                                                          m_x2-m_x1,
                                                          m_y2-m_y1,
                                                          GDK_INTERP_BILINEAR,
                                                          200,
                                                          200,
                                                          widget->style->bg[GTK_STATE_SELECTED].pixel,
                                                          widget->style->bg[GTK_STATE_SELECTED].pixel);

                        gdk_draw_pixbuf(GDK_DRAWABLE(buffer),
                                        gc,
                                        sub,
                                        0,0,
                                        m_x1,
                                        m_y1,
                                        -1, -1,
                                        GDK_RGB_DITHER_NONE,
                                        0, 0);

                        gdk_pixbuf_unref(sub);
                        sub = NULL;
                    }
                }

                gdk_draw_rectangle(GDK_DRAWABLE(buffer),
                                gc,
                                FALSE,
                                m_x1,
                                m_y1,
                                m_x2 - m_x1,
                                m_y2 - m_y1);
            }

        }
        else
        {

            /* HACK HACK HACK HACK */
            guint size = 0;
            if ((GTK_WIDGET (viewer)->allocation.width) < (GTK_WIDGET (viewer)->allocation.height))
            {
                size = GTK_WIDGET (viewer)->allocation.width;
            }
            else
            {
                size = GTK_WIDGET (viewer)->allocation.height;
            }
            pixbuf = gtk_icon_theme_load_icon (gtk_icon_theme_get_default(), 
                                               "ristretto", 
                                               (size*0.8),
                                               GTK_ICON_LOOKUP_FORCE_SIZE, NULL);
            gdk_pixbuf_saturate_and_pixelate (pixbuf, pixbuf, 0, TRUE);
            pixbuf = gdk_pixbuf_composite_color_simple (pixbuf, (size*0.8), (size*0.8), GDK_INTERP_BILINEAR, 40, 40, bg_color->pixel, bg_color->pixel);

            x1 = (widget->allocation.width-gdk_pixbuf_get_width(pixbuf))<0?0:(widget->allocation.width-gdk_pixbuf_get_width(pixbuf))/2;
            y1 = (widget->allocation.height-gdk_pixbuf_get_height(pixbuf))<0?0:(widget->allocation.height-gdk_pixbuf_get_height(pixbuf))/2;
            x2 = gdk_pixbuf_get_width(pixbuf);
            y2 = gdk_pixbuf_get_height(pixbuf);

            gdk_draw_pixbuf(GDK_DRAWABLE(buffer), 
                            NULL, 
                            pixbuf,
                            0,
                            0,
                            x1,
                            y1,
                            x2, 
                            y2,
                            GDK_RGB_DITHER_NONE,
                            0,0);
        }
        gdk_draw_drawable(GDK_DRAWABLE(widget->window), 
                        gdk_gc_new(widget->window), 
                        buffer,
                        0,
                        0,
                        0,
                        0,
                        widget->allocation.width,
                        widget->allocation.height);
        g_object_unref(buffer);
   }
   g_object_unref (settings_manager);
}

static void
rstto_picture_viewer_destroy(GtkObject *object)
{

}

static gboolean  
rstto_picture_viewer_set_scroll_adjustments(RsttoPictureViewer *viewer, GtkAdjustment *hadjustment, GtkAdjustment *vadjustment)
{
    if(viewer->hadjustment)
    {
        g_signal_handlers_disconnect_by_func(viewer->hadjustment, viewer->priv->cb_value_changed, viewer);
        g_object_unref(viewer->hadjustment);
    }
    if(viewer->vadjustment)
    {
        g_signal_handlers_disconnect_by_func(viewer->vadjustment, viewer->priv->cb_value_changed, viewer);
        g_object_unref(viewer->vadjustment);
    }

    viewer->hadjustment = hadjustment;
    viewer->vadjustment = vadjustment;

    if(viewer->hadjustment)
    {
        g_signal_connect(G_OBJECT(viewer->hadjustment), "value-changed", (GCallback)viewer->priv->cb_value_changed, viewer);
        g_object_ref(viewer->hadjustment);
    }
    if(viewer->vadjustment)
    {
        g_signal_connect(G_OBJECT(viewer->vadjustment), "value-changed", (GCallback)viewer->priv->cb_value_changed, viewer);
        g_object_ref(viewer->vadjustment);
    }
    return TRUE;
}

static void
cb_rstto_picture_viewer_value_changed(GtkAdjustment *adjustment, RsttoPictureViewer *viewer)
{
    /** 
     * A new subpixbuf needs to be blown up
     */
    rstto_picture_viewer_queued_repaint (viewer, TRUE);
}

GtkWidget *
rstto_picture_viewer_new (void)
{
    GtkWidget *widget;

    widget = g_object_new(RSTTO_TYPE_PICTURE_VIEWER, NULL);

    return widget;
}

void
rstto_picture_viewer_set_scale (RsttoPictureViewer *viewer, gdouble scale)
{
    gdouble *img_scale;
    GdkPixbuf *src_pixbuf = NULL;

    if (viewer->priv->image)
    {
        src_pixbuf = rstto_image_get_pixbuf (viewer->priv->image);
        img_scale = g_object_get_data (G_OBJECT (viewer->priv->image), "viewer-scale");

        if (src_pixbuf)
        {
            gdouble image_width = (gdouble)rstto_image_get_width (viewer->priv->image);
            gdouble image_height = (gdouble)rstto_image_get_height (viewer->priv->image);

            viewer->hadjustment->upper = image_width *scale;
            gtk_adjustment_changed(viewer->hadjustment);

            viewer->vadjustment->upper = image_height * scale;
            gtk_adjustment_changed(viewer->vadjustment);

            viewer->hadjustment->value = (((viewer->hadjustment->value +
                                          (viewer->hadjustment->page_size / 2)) *
                                           (scale)) / (*img_scale)) - (viewer->hadjustment->page_size / 2);
            viewer->vadjustment->value = (((viewer->vadjustment->value +
                                          (viewer->vadjustment->page_size / 2)) *
                                           (scale)) / (*img_scale)) - (viewer->vadjustment->page_size / 2);

            if((viewer->hadjustment->value + viewer->hadjustment->page_size) > viewer->hadjustment->upper)
            {
                viewer->hadjustment->value = viewer->hadjustment->upper - viewer->hadjustment->page_size;
            }
            if(viewer->hadjustment->value < viewer->hadjustment->lower)
            {
                viewer->hadjustment->value = viewer->hadjustment->lower;
            }
            if((viewer->vadjustment->value + viewer->vadjustment->page_size) > viewer->vadjustment->upper)
            {
                viewer->vadjustment->value = viewer->vadjustment->upper - viewer->vadjustment->page_size;
            }
            if(viewer->vadjustment->value < viewer->vadjustment->lower)
            {
                viewer->vadjustment->value = viewer->vadjustment->lower;
            }

            gtk_adjustment_value_changed(viewer->hadjustment);
            gtk_adjustment_value_changed(viewer->vadjustment);

            /** 
             * Set these settings at the end of the function, 
             * since the old and new values are required in the above code
             */
            *img_scale = scale;

            rstto_picture_viewer_queued_repaint (viewer, TRUE);
        }
    }
}

gdouble
rstto_picture_viewer_get_scale(RsttoPictureViewer *viewer)
{
    gdouble *scale;
    if (viewer->priv->image)
    {
        scale = g_object_get_data (G_OBJECT (viewer->priv->image), "viewer-scale");
        return *scale;
    }
    return 0;
}

/**
 * rstto_picture_viewer_calculate_scale:
 * @viewer:
 *
 */
static gdouble
rstto_picture_viewer_calculate_scale (RsttoPictureViewer *viewer)
{
    gint width = 0, height = 0;

    if (viewer->priv->image != NULL)
    {   
        switch(rstto_image_get_orientation (viewer->priv->image))
        {
            default:
                width = rstto_image_get_width (viewer->priv->image);
                height = rstto_image_get_height (viewer->priv->image);
                break;
            case RSTTO_IMAGE_ORIENT_270:
            case RSTTO_IMAGE_ORIENT_90:
                height = rstto_image_get_width (viewer->priv->image);
                width = rstto_image_get_height (viewer->priv->image);
                break;
        }
    }

    if (width > 0 && height > 0)
    {
        if ((gdouble)(GTK_WIDGET (viewer)->allocation.width / (gdouble)width) <
            ((gdouble)GTK_WIDGET (viewer)->allocation.height / (gdouble)height))
        {
            return (gdouble)GTK_WIDGET (viewer)->allocation.width / (gdouble)width;
        }
        else
        {
            return (gdouble)GTK_WIDGET (viewer)->allocation.height / (gdouble)height;
        }
    }
    return -1;
}

static void
cb_rstto_picture_viewer_scroll_event (RsttoPictureViewer *viewer, GdkEventScroll *event)
{
    /*
    RsttoImageListEntry *entry = rstto_image_list_get_file(viewer->priv->image_list);

    if (entry == NULL)
    {
        return;
    }

    gdouble scale = rstto_image_list_entry_get_scale(entry);
    viewer->priv->zoom_mode = RSTTO_ZOOM_MODE_CUSTOM;
    switch(event->direction)
    {
        case GDK_SCROLL_UP:
        case GDK_SCROLL_LEFT:
            if (scale= 0.05)
                return;
            if (viewer->priv->refresh.idle_id > 0)
            {
                g_source_remove(viewer->priv->refresh.idle_id);
            }
            rstto_image_list_entry_set_scale(entry, scale / 1.1);
            rstto_image_list_entry_set_fit_to_screen (entry, FALSE);

            viewer->vadjustment->value = ((viewer->vadjustment->value + event->y) / 1.1) - event->y;
            viewer->hadjustment->value = ((viewer->hadjustment->value + event->x) / 1.1) - event->x;

            viewer->priv->refresh.idle_id = g_idle_add((GSourceFunc)cb_rstto_picture_viewer_queued_repaint, viewer);
            break;
        case GDK_SCROLL_DOWN:
        case GDK_SCROLL_RIGHT:
            if (scale >= 16)
                return;
            if (viewer->priv->refresh.idle_id > 0)
            {
                g_source_remove(viewer->priv->refresh.idle_id);
            }
            rstto_image_list_entry_set_scale(entry, scale * 1.1);
            rstto_image_list_entry_set_fit_to_screen (entry, FALSE);


            viewer->vadjustment->value = ((viewer->vadjustment->value + event->y) * 1.1) - event->y;
            viewer->hadjustment->value = ((viewer->hadjustment->value + event->x) * 1.1) - event->x;

            gtk_adjustment_value_changed(viewer->hadjustment);
            gtk_adjustment_value_changed(viewer->vadjustment);

            viewer->priv->refresh.idle_id = g_idle_add((GSourceFunc)cb_rstto_picture_viewer_queued_repaint, viewer);
            break;
    }
    */
}

static gboolean 
cb_rstto_picture_viewer_motion_notify_event (RsttoPictureViewer *viewer,
                                             GdkEventMotion *event,
                                             gpointer user_data)
{
    if (event->state & GDK_BUTTON1_MASK)
    {
        viewer->priv->motion.current_x = event->x;
        viewer->priv->motion.current_y = event->y;

        switch (viewer->priv->motion.state)
        {
            case RSTTO_PICTURE_VIEWER_MOTION_STATE_MOVE:
                if (viewer->priv->motion.x != viewer->priv->motion.current_x)
                {
                    gint val = viewer->hadjustment->value;
                    viewer->hadjustment->value = viewer->priv->motion.h_val + (viewer->priv->motion.x - viewer->priv->motion.current_x);
                    if((viewer->hadjustment->value + viewer->hadjustment->page_size) > viewer->hadjustment->upper)
                    {
                        viewer->hadjustment->value = viewer->hadjustment->upper - viewer->hadjustment->page_size;
                    }
                    if((viewer->hadjustment->value) < viewer->hadjustment->lower)
                    {
                        viewer->hadjustment->value = viewer->hadjustment->lower;
                    }
                    if (val != viewer->hadjustment->value)
                        gtk_adjustment_value_changed(viewer->hadjustment);
                }

                if (viewer->priv->motion.y != viewer->priv->motion.current_y)
                {
                    gint val = viewer->vadjustment->value;
                    viewer->vadjustment->value = viewer->priv->motion.v_val + (viewer->priv->motion.y - viewer->priv->motion.current_y);
                    if((viewer->vadjustment->value + viewer->vadjustment->page_size) > viewer->vadjustment->upper)
                    {
                        viewer->vadjustment->value = viewer->vadjustment->upper - viewer->vadjustment->page_size;
                    }
                    if((viewer->vadjustment->value) < viewer->vadjustment->lower)
                    {
                        viewer->vadjustment->value = viewer->vadjustment->lower;
                    }
                    if (val != viewer->vadjustment->value)
                        gtk_adjustment_value_changed(viewer->vadjustment);
                }
                break;
            case RSTTO_PICTURE_VIEWER_MOTION_STATE_BOX_ZOOM:
                rstto_picture_viewer_queued_repaint (viewer, FALSE);
                break;
            default:
                break;
        }
    }
    return FALSE;
}

static void
rstto_picture_viewer_calculate_adjustments (RsttoPictureViewer *viewer, gdouble scale)
{
    GdkPixbuf *p_src_pixbuf;
    GtkWidget *widget = GTK_WIDGET (viewer);
    gdouble image_width, image_height;
    gdouble pixbuf_width, pixbuf_height;
    gdouble image_scale;
    gboolean vadjustment_changed = FALSE;
    gboolean hadjustment_changed = FALSE;

    if (viewer->priv->image != NULL)
    {   
        p_src_pixbuf = rstto_image_get_pixbuf (viewer->priv->image);
        if (p_src_pixbuf != NULL)
        {
            image_width = (gdouble)rstto_image_get_width (viewer->priv->image);
            image_height = (gdouble)rstto_image_get_height (viewer->priv->image);

            pixbuf_width = (gdouble)gdk_pixbuf_get_width (p_src_pixbuf);
            pixbuf_height = (gdouble)gdk_pixbuf_get_height (p_src_pixbuf);

            image_scale = pixbuf_width / image_width;

            switch (rstto_image_get_orientation (viewer->priv->image))
            {
                default:
                    if(viewer->hadjustment)
                    {
                        viewer->hadjustment->page_size = widget->allocation.width / image_scale;
                        viewer->hadjustment->upper = image_width * (scale / image_scale);
                        viewer->hadjustment->lower = 0;
                        viewer->hadjustment->step_increment = 1;
                        viewer->hadjustment->page_increment = 100;
                        if((viewer->hadjustment->value + viewer->hadjustment->page_size) > viewer->hadjustment->upper)
                        {
                            viewer->hadjustment->value = viewer->hadjustment->upper - viewer->hadjustment->page_size;
                            hadjustment_changed = TRUE;
                        }
                        if(viewer->hadjustment->value < viewer->hadjustment->lower)
                        {
                            viewer->hadjustment->value = viewer->hadjustment->lower;
                            hadjustment_changed = TRUE;
                        }
                    }
                    if(viewer->vadjustment)
                    {
                        viewer->vadjustment->page_size = widget->allocation.height / image_scale;
                        viewer->vadjustment->upper = image_height * (scale / image_scale);
                        viewer->vadjustment->lower = 0;
                        viewer->vadjustment->step_increment = 1;
                        viewer->vadjustment->page_increment = 100;
                        if((viewer->vadjustment->value + viewer->vadjustment->page_size) > viewer->vadjustment->upper)
                        {
                            viewer->vadjustment->value = viewer->vadjustment->upper - viewer->vadjustment->page_size;
                            vadjustment_changed = TRUE;
                        }
                        if(viewer->vadjustment->value < viewer->vadjustment->lower)
                        {
                            viewer->vadjustment->value = viewer->vadjustment->lower;
                            vadjustment_changed = TRUE;
                        }
                    }
                    break;
                case RSTTO_IMAGE_ORIENT_270:
                case RSTTO_IMAGE_ORIENT_90:
                    if(viewer->hadjustment)
                    {
                        viewer->hadjustment->page_size = widget->allocation.width / image_scale;
                        viewer->hadjustment->upper = image_height * (scale / image_scale);
                        viewer->hadjustment->lower = 0;
                        viewer->hadjustment->step_increment = 1;
                        viewer->hadjustment->page_increment = 100;
                        if((viewer->hadjustment->value + viewer->hadjustment->page_size) > viewer->hadjustment->upper)
                        {
                            viewer->hadjustment->value = viewer->hadjustment->upper - viewer->hadjustment->page_size;
                            hadjustment_changed = TRUE;
                        }
                        if(viewer->hadjustment->value < viewer->hadjustment->lower)
                        {
                            viewer->hadjustment->value = viewer->hadjustment->lower;
                            hadjustment_changed = TRUE;
                        }
                    }
                    if(viewer->vadjustment)
                    {
                        viewer->vadjustment->page_size = widget->allocation.height / image_scale;
                        viewer->vadjustment->upper = image_width * (scale / image_scale);
                        viewer->vadjustment->lower = 0;
                        viewer->vadjustment->step_increment = 1;
                        viewer->vadjustment->page_increment = 100;
                        if((viewer->vadjustment->value + viewer->vadjustment->page_size) > viewer->vadjustment->upper)
                        {
                            viewer->vadjustment->value = viewer->vadjustment->upper - viewer->vadjustment->page_size;
                            vadjustment_changed = TRUE;
                        }
                        if(viewer->vadjustment->value < viewer->vadjustment->lower)
                        {
                            viewer->vadjustment->value = viewer->vadjustment->lower;
                            vadjustment_changed = TRUE;
                        }
                    }
                    break;
            }

            if (viewer->vadjustment && viewer->hadjustment)
            {
                gtk_adjustment_changed(viewer->hadjustment);
                gtk_adjustment_changed(viewer->vadjustment);
            }
            if (hadjustment_changed == TRUE)
                gtk_adjustment_value_changed(viewer->hadjustment);
            if (vadjustment_changed == TRUE)
                gtk_adjustment_value_changed(viewer->vadjustment);
        }
    }

}

static void
rstto_picture_viewer_queued_repaint (RsttoPictureViewer *viewer, gboolean refresh)
{
    if (viewer->priv->repaint.idle_id > 0)
    {
        g_source_remove(viewer->priv->repaint.idle_id);
    }
    if (refresh)
    {
        viewer->priv->repaint.refresh = TRUE;
    }
    viewer->priv->repaint.idle_id = g_idle_add((GSourceFunc)cb_rstto_picture_viewer_queued_repaint, viewer);
}

static gboolean 
cb_rstto_picture_viewer_queued_repaint (RsttoPictureViewer *viewer)
{
    GdkPixbuf *p_src_pixbuf = NULL;
    GdkPixbuf *p_tmp_pixbuf = NULL;
    GdkPixbuf *p_tmp_pixbuf2 = NULL;
    gdouble *p_scale = NULL;
    gboolean *p_fit_to_screen= NULL;
    gdouble scale = 1;
    gdouble image_scale = 1;
    gdouble thumb_scale = 1;
    gdouble thumb_width = 0;
    gboolean fit_to_screen = FALSE;
    gdouble image_width = 0, image_height = 0;
    gdouble pixbuf_width, pixbuf_height;
    GtkWidget *widget = GTK_WIDGET (viewer);

    if (viewer->priv->image != NULL)
    {   
        image_width = (gdouble)rstto_image_get_width (viewer->priv->image);
        image_height = (gdouble)rstto_image_get_height (viewer->priv->image);

        switch (viewer->priv->state)
        {
            case RSTTO_PICTURE_VIEWER_STATE_NORMAL:
                p_src_pixbuf = rstto_image_get_pixbuf (viewer->priv->image);
                if (p_src_pixbuf)
                {
                    pixbuf_width = (gdouble)gdk_pixbuf_get_width (p_src_pixbuf);
                    pixbuf_height = (gdouble)gdk_pixbuf_get_height (p_src_pixbuf);

                    image_scale = pixbuf_width / image_width;
                }
                break;
            case RSTTO_PICTURE_VIEWER_STATE_PREVIEW:
                p_src_pixbuf = rstto_image_get_thumbnail (viewer->priv->image);
                if (p_src_pixbuf)
                {
                    thumb_width = (gdouble)gdk_pixbuf_get_width (p_src_pixbuf);
                    thumb_scale = (thumb_width / image_width);
                }
                else
                    return FALSE;
                break;
            default:
                break;
        }

        p_scale = g_object_get_data (G_OBJECT (viewer->priv->image), "viewer-scale");
        p_fit_to_screen = g_object_get_data (G_OBJECT (viewer->priv->image), "viewer-fit-to-screen");
        scale = *p_scale;
        fit_to_screen = *p_fit_to_screen;

        if ((scale <= 0) || (fit_to_screen == TRUE))
        {
            scale = rstto_picture_viewer_calculate_scale (viewer);
            *p_fit_to_screen = TRUE;
            *p_scale = scale;
        }
    }


    rstto_picture_viewer_calculate_adjustments (viewer, scale);


    if (viewer->priv->repaint.refresh)
    {
        if(viewer->priv->dst_pixbuf)
        {
            g_object_unref(viewer->priv->dst_pixbuf);
            viewer->priv->dst_pixbuf = NULL;
        }
        if (p_src_pixbuf)
        {
            gdouble x, y;

            switch (rstto_image_get_orientation (viewer->priv->image))
            {
                default:
                case RSTTO_IMAGE_ORIENT_NONE:
                    x = viewer->hadjustment->value * image_scale;
                    y = viewer->vadjustment->value * image_scale;
                    p_tmp_pixbuf = gdk_pixbuf_new_subpixbuf (p_src_pixbuf,
                                               (gint)(x/scale * thumb_scale * image_scale), 
                                               (gint)(y/scale * thumb_scale * image_scale),
                                               (gint)((widget->allocation.width / scale) < image_width?
                                                      (widget->allocation.width / scale)*thumb_scale*image_scale:image_width*thumb_scale*image_scale),
                                               (gint)((widget->allocation.height / scale) < image_height?
                                                      (widget->allocation.height / scale)*image_scale*thumb_scale:image_height*thumb_scale*image_scale));
                    break;
                case RSTTO_IMAGE_ORIENT_90:
                    x = viewer->vadjustment->value * image_scale;
                    y = (viewer->hadjustment->upper - (viewer->hadjustment->value + viewer->hadjustment->page_size)) * image_scale;
                    if (y < 0) y = 0;
                    //y = viewer->hadjustment->value * image_scale;
                    p_tmp_pixbuf = gdk_pixbuf_new_subpixbuf (p_src_pixbuf,
                                               (gint)(x/scale * thumb_scale * image_scale), 
                                               (gint)(y/scale * thumb_scale * image_scale),
                                               (gint)((widget->allocation.height/ scale) < image_width?
                                                      (widget->allocation.height/ scale)*thumb_scale*image_scale:image_width*thumb_scale*image_scale),
                                               (gint)((widget->allocation.width/ scale) < image_height?
                                                      (widget->allocation.width/ scale)*image_scale*thumb_scale:image_height*thumb_scale*image_scale));
                    if (p_tmp_pixbuf)
                    {
                        p_tmp_pixbuf2 = gdk_pixbuf_rotate_simple (p_tmp_pixbuf, GDK_PIXBUF_ROTATE_CLOCKWISE);
                        g_object_unref (p_tmp_pixbuf);
                        p_tmp_pixbuf = p_tmp_pixbuf2;
                    }
                    break;
                case RSTTO_IMAGE_ORIENT_180:
                    x = (viewer->hadjustment->upper - (viewer->hadjustment->value + viewer->hadjustment->page_size)) * image_scale;
                    if (x < 0) x = 0;
                    y = (viewer->vadjustment->upper - (viewer->vadjustment->value + viewer->vadjustment->page_size)) * image_scale;
                    if (y < 0) y = 0;
                    //y = viewer->hadjustment->value * image_scale;
                    p_tmp_pixbuf = gdk_pixbuf_new_subpixbuf (p_src_pixbuf,
                                               (gint)(x/scale * thumb_scale * image_scale), 
                                               (gint)(y/scale * thumb_scale * image_scale),
                                               (gint)((widget->allocation.width / scale) < image_width?
                                                      (widget->allocation.width / scale)*thumb_scale*image_scale:image_width*thumb_scale*image_scale),
                                               (gint)((widget->allocation.height/ scale) < image_width?
                                                      (widget->allocation.height/ scale)*image_scale*thumb_scale:image_height*thumb_scale*image_scale));
                    if (p_tmp_pixbuf)
                    {
                        p_tmp_pixbuf2 = gdk_pixbuf_rotate_simple (p_tmp_pixbuf, GDK_PIXBUF_ROTATE_UPSIDEDOWN);
                        g_object_unref (p_tmp_pixbuf);
                        p_tmp_pixbuf = p_tmp_pixbuf2;
                    }
                    break;
                case RSTTO_IMAGE_ORIENT_270:
                    x = (viewer->vadjustment->upper - (viewer->vadjustment->value + viewer->vadjustment->page_size)) * image_scale;
                    if (x < 0) x = 0;
                    y = viewer->hadjustment->value * image_scale;
                    p_tmp_pixbuf = gdk_pixbuf_new_subpixbuf (p_src_pixbuf,
                                               (gint)(x/scale * thumb_scale * image_scale), 
                                               (gint)(y/scale * thumb_scale * image_scale),
                                               (gint)((widget->allocation.height/ scale) < image_width?
                                                      (widget->allocation.height/ scale)*thumb_scale*image_scale:image_width*thumb_scale*image_scale),
                                               (gint)((widget->allocation.width/ scale) < image_height?
                                                      (widget->allocation.width/ scale)*image_scale*thumb_scale:image_height*thumb_scale*image_scale));
                    if (p_tmp_pixbuf)
                    {
                        p_tmp_pixbuf2 = gdk_pixbuf_rotate_simple (p_tmp_pixbuf, GDK_PIXBUF_ROTATE_COUNTERCLOCKWISE);
                        g_object_unref (p_tmp_pixbuf);
                        p_tmp_pixbuf = p_tmp_pixbuf2;
                    }
                    break;
            }
            /**
             *  tmp_scale is the factor between the original image and the thumbnail,
             *  when looking at the actual image, tmp_scale == 1.0
             */
            /*
            gdouble x = viewer->hadjustment->value * image_scale;
            gdouble y = viewer->vadjustment->value * image_scale;

            p_tmp_pixbuf = gdk_pixbuf_new_subpixbuf (p_src_pixbuf,
                                               (gint)(x/scale * thumb_scale * image_scale), 
                                               (gint)(y/scale * thumb_scale * image_scale),
                                               (gint)((widget->allocation.width / scale) < image_width?
                                                      (widget->allocation.width / scale)*thumb_scale*image_scale:image_width*thumb_scale*image_scale),
                                               (gint)((widget->allocation.height / scale) < image_height?
                                                      (widget->allocation.height / scale)*image_scale*thumb_scale:image_height*thumb_scale*image_scale));
            */

            if(p_tmp_pixbuf)
            {
                gint dst_width = gdk_pixbuf_get_width (p_tmp_pixbuf)*(scale/thumb_scale/image_scale);
                gint dst_height = gdk_pixbuf_get_height (p_tmp_pixbuf)*(scale/thumb_scale/image_scale);
                viewer->priv->dst_pixbuf = gdk_pixbuf_scale_simple (p_tmp_pixbuf,
                                        dst_width>0?dst_width:1,
                                        dst_height>0?dst_height:1,
                                        GDK_INTERP_BILINEAR);
                g_object_unref (p_tmp_pixbuf);
                p_tmp_pixbuf = NULL;
            }
        }
    }


    rstto_picture_viewer_paint (GTK_WIDGET (viewer));

    g_source_remove (viewer->priv->repaint.idle_id);
    viewer->priv->repaint.idle_id = -1;
    viewer->priv->repaint.refresh = FALSE;
    return FALSE;
}

static RsttoPictureViewerState
rstto_picture_viewer_get_state (RsttoPictureViewer *viewer)
{
    return viewer->priv->state;
}


static void
rstto_picture_viewer_set_state (RsttoPictureViewer *viewer, RsttoPictureViewerState state)
{
    viewer->priv->state = state;
}

static void
rstto_picture_viewer_set_motion_state (RsttoPictureViewer *viewer, RsttoPictureViewerMotionState state)
{
    viewer->priv->motion.state = state;
}

static RsttoPictureViewerMotionState
rstto_picture_viewer_get_motion_state (RsttoPictureViewer *viewer)
{
    return viewer->priv->motion.state;
}

static void
cb_rstto_picture_viewer_button_press_event (RsttoPictureViewer *viewer, GdkEventButton *event)
{
    if(event->button == 1)
    {
        viewer->priv->motion.x = event->x;
        viewer->priv->motion.y = event->y;
        viewer->priv->motion.current_x = event->x;
        viewer->priv->motion.current_y = event->y;
        viewer->priv->motion.h_val = viewer->hadjustment->value;
        viewer->priv->motion.v_val = viewer->vadjustment->value;

        if (viewer->priv->image != NULL && rstto_picture_viewer_get_state (viewer) == RSTTO_PICTURE_VIEWER_STATE_NORMAL)
        {

            if (!(event->state & (GDK_CONTROL_MASK)))
            {
                GtkWidget *widget = GTK_WIDGET(viewer);
                GdkCursor *cursor = gdk_cursor_new(GDK_FLEUR);
                gdk_window_set_cursor(widget->window, cursor);
                gdk_cursor_unref(cursor);

                rstto_picture_viewer_set_motion_state (viewer, RSTTO_PICTURE_VIEWER_MOTION_STATE_MOVE);
            }

            if (event->state & GDK_CONTROL_MASK)
            {
                GtkWidget *widget = GTK_WIDGET(viewer);
                GdkCursor *cursor = gdk_cursor_new(GDK_UL_ANGLE);
                gdk_window_set_cursor(widget->window, cursor);
                gdk_cursor_unref(cursor);

                rstto_picture_viewer_set_motion_state (viewer, RSTTO_PICTURE_VIEWER_MOTION_STATE_BOX_ZOOM);
            }
        }

        
    }
    if(event->button == 3)
    {
        if (viewer->priv->menu)
        {
            gtk_widget_show_all(GTK_WIDGET(viewer->priv->menu));
            gtk_menu_popup(viewer->priv->menu,
                           NULL,
                           NULL,
                           NULL,
                           NULL,
                           3,
                           event->time);
        }
    }
}

static void
cb_rstto_picture_viewer_button_release_event (RsttoPictureViewer *viewer, GdkEventButton *event)
{
    GtkWidget *widget = GTK_WIDGET(viewer);
    switch (event->button)
    {
        case 1:
            gdk_window_set_cursor(widget->window, NULL);
            switch (rstto_picture_viewer_get_motion_state (viewer))
            {
                case RSTTO_PICTURE_VIEWER_MOTION_STATE_BOX_ZOOM:
                    rstto_picture_viewer_set_zoom_mode (viewer, RSTTO_ZOOM_MODE_CUSTOM);
                    if(GTK_WIDGET_REALIZED(widget))
                    {

                    }
                    break;
                default:
                    break;
            }
            rstto_picture_viewer_set_motion_state (viewer, RSTTO_PICTURE_VIEWER_MOTION_STATE_NORMAL);
            rstto_picture_viewer_queued_repaint (viewer, FALSE);
            break;
    }

}

static void
cb_rstto_picture_viewer_popup_menu (RsttoPictureViewer *viewer, gboolean user_data)
{
    if (viewer->priv->menu)
    {
        gtk_widget_show_all(GTK_WIDGET(viewer->priv->menu));
        gtk_menu_popup(viewer->priv->menu,
                       NULL,
                       NULL,
                       NULL,
                       NULL,
                       0,
                       gtk_get_current_event_time());
    }
}

void
rstto_picture_viewer_set_menu (RsttoPictureViewer *viewer, GtkMenu *menu)
{
    if (viewer->priv->menu)
    {
        gtk_menu_detach (viewer->priv->menu);
        gtk_widget_destroy (GTK_WIDGET(viewer->priv->menu));
    }

    viewer->priv->menu = menu;

    if (viewer->priv->menu)
    {
        gtk_menu_attach_to_widget (viewer->priv->menu, GTK_WIDGET(viewer), NULL);
    }
}

static void
rstto_picture_viewer_set_zoom_mode(RsttoPictureViewer *viewer, RsttoZoomMode mode)
{
    gdouble scale;
    gboolean *p_fit_to_screen;
    viewer->priv->zoom_mode = mode;

    switch (viewer->priv->zoom_mode)
    {
        case RSTTO_ZOOM_MODE_CUSTOM:
            if (viewer->priv->image)
            {
                p_fit_to_screen = g_object_get_data (G_OBJECT (viewer->priv->image), "viewer-fit-to-screen");
                *p_fit_to_screen = FALSE;
                g_object_set_data (G_OBJECT (viewer->priv->image), "viewer-fit-to-screen", p_fit_to_screen);
            }
            break;
        case RSTTO_ZOOM_MODE_FIT:
            if (viewer->priv->image)
            {
                p_fit_to_screen = g_object_get_data (G_OBJECT (viewer->priv->image), "viewer-fit-to-screen");
                *p_fit_to_screen = TRUE;
                g_object_set_data (G_OBJECT (viewer->priv->image), "viewer-fit-to-screen", p_fit_to_screen);
            }
            scale = rstto_picture_viewer_calculate_scale (viewer);
            if (scale != -1.0)
                rstto_picture_viewer_set_scale (viewer, scale);
            break;
        case RSTTO_ZOOM_MODE_100:
            if (viewer->priv->image)
            {
                p_fit_to_screen = g_object_get_data (G_OBJECT (viewer->priv->image), "viewer-fit-to-screen");
                *p_fit_to_screen = FALSE;
                g_object_set_data (G_OBJECT (viewer->priv->image), "viewer-fit-to-screen", p_fit_to_screen);
            }
            rstto_picture_viewer_set_scale (viewer, 1);
            break;
    }
}

/**
 *  rstto_picture_viewer_set_image:
 *  @viewer :
 *  @image  :
 *
 *
 */
static void
rstto_picture_viewer_set_image (RsttoPictureViewer *viewer, RsttoImage *image)
{
    gdouble *scale = NULL;
    gboolean *fit_to_screen = NULL;

    RsttoSettings *settings_manager = rstto_settings_new();
    GValue max_size = {0,};

    g_value_init (&max_size, G_TYPE_UINT);
    g_object_get_property (G_OBJECT(settings_manager), "image-quality", &max_size);

    if (viewer->priv->image)
    {
        g_signal_handlers_disconnect_by_func (viewer->priv->image, cb_rstto_picture_viewer_image_updated, viewer);
        g_signal_handlers_disconnect_by_func (viewer->priv->image, cb_rstto_picture_viewer_image_prepared, viewer);
        g_object_remove_weak_pointer (G_OBJECT (viewer->priv->image), (gpointer *)&viewer->priv->image);
    }

    viewer->priv->image = image;

    if (viewer->priv->image)
    {
        g_object_add_weak_pointer (G_OBJECT (viewer->priv->image), (gpointer *)&viewer->priv->image);

        g_signal_connect (G_OBJECT (viewer->priv->image), "updated", G_CALLBACK (cb_rstto_picture_viewer_image_updated), viewer);
        g_signal_connect (G_OBJECT (viewer->priv->image), "prepared", G_CALLBACK (cb_rstto_picture_viewer_image_prepared), viewer);

        scale = g_object_get_data (G_OBJECT (viewer->priv->image), "viewer-scale");
        fit_to_screen = g_object_get_data (G_OBJECT (viewer->priv->image), "viewer-fit-to-screen");

        if (scale == NULL)
        {
            scale = g_new0 (gdouble, 1);
            *scale = -1.0;
            g_object_set_data (G_OBJECT (viewer->priv->image), "viewer-scale", scale);
        }
        if (fit_to_screen == NULL)
        {
            fit_to_screen = g_new0 (gboolean, 1);
            g_object_set_data (G_OBJECT (viewer->priv->image), "viewer-fit-to-screen", fit_to_screen);
        }

        rstto_image_load (viewer->priv->image, FALSE, g_value_get_uint (&max_size), FALSE, NULL);
    }
    else
    {
        rstto_picture_viewer_queued_repaint (viewer, TRUE);
    }
    g_object_unref (settings_manager);
}

/**
 * cb_rstto_picture_viewer_image_updated:
 * @image:
 * @viewer:
 *
 */
static void
cb_rstto_picture_viewer_image_updated (RsttoImage *image, RsttoPictureViewer *viewer)
{
    rstto_picture_viewer_set_state (viewer, RSTTO_PICTURE_VIEWER_STATE_NORMAL);

    rstto_picture_viewer_queued_repaint (viewer, TRUE);
}

/**
 * cb_rstto_picture_viewer_image_prepared:
 * @image:
 * @viewer:
 *
 */
static void
cb_rstto_picture_viewer_image_prepared (RsttoImage *image, RsttoPictureViewer *viewer)
{
    rstto_picture_viewer_set_state (viewer, RSTTO_PICTURE_VIEWER_STATE_PREVIEW);

    rstto_picture_viewer_queued_repaint (viewer, TRUE);
}

/**
 * rstto_picture_viewer_zoom_fit:
 * @window:
 *
 * Adjust the scale to make the image fit the window
 */
void
rstto_picture_viewer_zoom_fit (RsttoPictureViewer *viewer)
{
    rstto_picture_viewer_set_zoom_mode (viewer, RSTTO_ZOOM_MODE_FIT);
}

/**
 * rstto_picture_viewer_zoom_100:
 * @viewer:
 *
 * Set the scale to 1, meaning a zoom-factor of 100%
 */
void
rstto_picture_viewer_zoom_100 (RsttoPictureViewer *viewer)
{
    rstto_picture_viewer_set_zoom_mode (viewer, RSTTO_ZOOM_MODE_100);
}

/**
 * rstto_picture_viewer_zoom_in:
 * @viewer:
 * @factor:
 *
 * Zoom in the scale with a certain factor
 */
void
rstto_picture_viewer_zoom_in (RsttoPictureViewer *viewer, gdouble factor)
{
    gdouble scale;

    rstto_picture_viewer_set_zoom_mode (viewer, RSTTO_ZOOM_MODE_CUSTOM);
    scale = rstto_picture_viewer_get_scale (viewer);
    rstto_picture_viewer_set_scale (viewer, scale * factor);
}

/**
 * rstto_picture_viewer_zoom_out:
 * @viewer:
 * @factor:
 *
 * Zoom out the scale with a certain factor
 */
void
rstto_picture_viewer_zoom_out (RsttoPictureViewer *viewer, gdouble factor)
{
    gdouble scale;

    rstto_picture_viewer_set_zoom_mode (viewer, RSTTO_ZOOM_MODE_CUSTOM);
    scale = rstto_picture_viewer_get_scale (viewer);
    rstto_picture_viewer_set_scale (viewer, scale / factor);
}


/******************************************************************************************/


void
rstto_picture_viewer_set_iter (RsttoPictureViewer *viewer, RsttoImageListIter *iter)
{
    if (viewer->priv->iter)
    {
        g_signal_handlers_disconnect_by_func (viewer->priv->iter, cb_rstto_picture_viewer_nav_iter_changed, viewer);
        g_object_unref (viewer->priv->iter);
        viewer->priv->iter = NULL;
    }
    if (iter)
    {
        viewer->priv->iter = iter;
        g_object_ref (viewer->priv->iter);
        g_signal_connect (G_OBJECT (viewer->priv->iter), "changed", G_CALLBACK (cb_rstto_picture_viewer_nav_iter_changed), viewer);
    }
}

static void
cb_rstto_picture_viewer_nav_iter_changed (RsttoImageListIter *iter, gpointer user_data)
{
    RsttoPictureViewer *viewer = RSTTO_PICTURE_VIEWER (user_data);
    rstto_picture_viewer_set_image (viewer, rstto_image_list_iter_get_image (iter));
}

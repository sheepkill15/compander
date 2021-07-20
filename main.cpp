/*
 * Copyright © 2003 Keith Packard
 *
 * Permission to use, copy, modify, distribute, and sell this software and its
 * documentation for any purpose is hereby granted without fee, provided that
 * the above copyright notice appear in all copies and that both that
 * copyright notice and this permission notice appear in supporting
 * documentation, and that the name of Keith Packard not be used in
 * advertising or publicity pertaining to distribution of the software without
 * specific, written prior permission.  Keith Packard makes no
 * representations about the suitability of this software for any purpose.  It
 * is provided "as is" without express or implied warranty.
 *
 * KEITH PACKARD DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE,
 * INCLUDING ALL IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS, IN NO
 * EVENT SHALL KEITH PACKARD BE LIABLE FOR ANY SPECIAL, INDIRECT OR
 * CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE,
 * DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER
 * TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */


/* Modified by Matthew Hawn. I don't know what to say here so follow what it
   says above. Not that I can really do anything about it
*/

#ifdef HAVE_CONFIG_H
#include "config.h"
#endif

#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <sys/poll.h>
#include <getopt.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/Xatom.h>
#include <X11/extensions/Xcomposite.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xrender.h>
#include <X11/extensions/shape.h>
#include <vector>
#include <list>

#if COMPOSITE_MAJOR > 0 || COMPOSITE_MINOR >= 2
#define HAS_NAME_WINDOW_PIXMAP 1
#endif

#define CAN_DO_USABLE 0

struct ignore {
    ignore *next;
    unsigned long sequence;
};

struct win {
    Window id;
#if HAS_NAME_WINDOW_PIXMAP
    Pixmap pixmap;
#endif
    XWindowAttributes a;
#if CAN_DO_USABLE
    Bool		usable;		    /* mapped and all damaged at one point */
    XRectangle		damage_bounds;	    /* bounds of damage */
#endif
    int mode;
    int damaged;
    Damage damage;
    Picture picture;
    Picture alphaPict;
    XserverRegion borderSize;
    XserverRegion extents;
    unsigned int opacity;
    Atom windowType;
    unsigned long damage_sequence;    /* sequence when damage was created */
    Bool shaped;
    XRectangle shape_bounds;

    /* for drawing translucent windows */
    XserverRegion borderClip;
};
using win_it = std::_List_iterator<win>;

static std::list<win> win_list;
static int scr;
static Window root;
static Picture rootPicture;
static Picture rootBuffer;
static Picture blackPicture;
static Picture rootTile;
static XserverRegion allDamage;
static Bool clipChanged;
#if HAS_NAME_WINDOW_PIXMAP
static Bool hasNamePixmap;
#endif
static int root_height, root_width;
static ignore *ignore_head, **ignore_tail = &ignore_head;
static int xfixes_event, xfixes_error;
static int damage_event, damage_error;
static int composite_event, composite_error;
static int render_event, render_error;
static int xshape_event, xshape_error;
static Bool synchronize;
static int composite_opcode;

/* find these once and be done with it */
static Atom opacityAtom;
static Atom winTypeAtom;
static Atom winDesktopAtom;
static Atom winDockAtom;
static Atom winToolbarAtom;
static Atom winMenuAtom;
static Atom winUtilAtom;
static Atom winSplashAtom;
static Atom winDialogAtom;
static Atom winNormalAtom;

/* opacity property name; sometime soon I'll write up an EWMH spec for it */
#define OPACITY_PROP    "_NET_WM_WINDOW_OPACITY"

#define TRANSLUCENT    0xe0000000
#define OPAQUE        0xffffffff

#define WINDOW_SOLID    0
#define WINDOW_TRANS    1
#define WINDOW_ARGB    2

#define TRANS_OPACITY    0.75

#define DEBUG_REPAINT 0
#define DEBUG_EVENTS  0
#define DEBUG_SHAPE   0
#define MONITOR_REPAINT 0

enum CompMode {
    CompSimple,        /* looks like a regular X server */
};

static void
determine_mode(Display *dpy, win_it w);

static double
get_opacity_percent(Display *dpy, win_it w, double def);

static XserverRegion
win_extents(Display *dpy, win_it w);

static CompMode compMode = CompSimple;

static Bool autoRedirect = False;

static Picture
solid_picture(Display *dpy, Bool argb, double a, double r, double g, double b) {
    Pixmap pixmap;
    Picture picture;
    XRenderPictureAttributes pa;
    pixmap = XCreatePixmap(dpy, root, 1, 1, argb ? 32 : 8);
    if (!pixmap)
        return None;

    pa.repeat = True;
    picture = XRenderCreatePicture(dpy, pixmap,
                                   XRenderFindStandardFormat(dpy, argb ? PictStandardARGB32 : PictStandardA8),
                                   CPRepeat,
                                   &pa);
    if (!picture) {
        XFreePixmap(dpy, pixmap);
        return None;
    }

    XRenderColor c = {static_cast<unsigned short>(r * 0xffff),
                      static_cast<unsigned short>(g * 0xffff),
                      static_cast<unsigned short>(b * 0xffff),
                      static_cast<unsigned short>(a * 0xffff)};
    XRenderFillRectangle(dpy, PictOpSrc, picture, &c, 0, 0, 1, 1);
    XFreePixmap(dpy, pixmap);
    return picture;
}

static void
discard_ignore(Display *dpy, unsigned long sequence) {
    while (ignore_head) {
        if ((long) (sequence - ignore_head->sequence) > 0) {
            ignore *next = ignore_head->next;
            delete ignore_head;
            ignore_head = next;
            if (!ignore_head)
                ignore_tail = &ignore_head;
        } else
            break;
    }
}

static void
set_ignore(Display *dpy, unsigned long sequence) {
    auto *i = new ignore;
    i->sequence = sequence;
    i->next = nullptr;
    *ignore_tail = i;
    ignore_tail = &i->next;
}

static int
should_ignore(Display *dpy, unsigned long sequence) {
    discard_ignore(dpy, sequence);
    return ignore_head && ignore_head->sequence == sequence;
}

static win_it
find_win(Window id) {
    for (auto it = win_list.begin(); it != win_list.end(); it++) {
        if (it->id == id) {
            return it;
        }
    }
    return win_list.end();
}

static const char *backgroundProps[] = {
        "_XROOTPMAP_ID",
        "_XSETROOT_ID",
        nullptr,
};

static Picture
root_tile(Display *dpy) {
    Picture picture;
    Atom actual_type;
    Pixmap pixmap;
    int actual_format;
    unsigned long nitems;
    unsigned long bytes_after;
    unsigned char *prop;
    Bool fill;
    XRenderPictureAttributes pa;
    int p;

    pixmap = None;
    for (p = 0; backgroundProps[p]; p++) {
        if (XGetWindowProperty(dpy, root, XInternAtom(dpy, backgroundProps[p], False),
                               0, 4, False, AnyPropertyType,
                               &actual_type, &actual_format, &nitems, &bytes_after, &prop) == Success &&
            actual_type == XInternAtom(dpy, "PIXMAP", False) && actual_format == 32 && nitems == 1) {
            memcpy(&pixmap, prop, 4);
            XFree(prop);
            fill = False;
            break;
        }
    }
    if (!pixmap) {
        pixmap = XCreatePixmap(dpy, root, 1, 1, DefaultDepth (dpy, scr));
        fill = True;
    }
    pa.repeat = True;
    picture = XRenderCreatePicture(dpy, pixmap,
                                   XRenderFindVisualFormat(dpy,
                                                           DefaultVisual (dpy, scr)),
                                   CPRepeat, &pa);
    if (fill) {
        XRenderColor c;

        c.red = c.green = c.blue = 0x8080;
        c.alpha = 0xffff;
        XRenderFillRectangle(dpy, PictOpSrc, picture, &c,
                             0, 0, 1, 1);
    }
    return picture;
}

static void
paint_root(Display *dpy) {
    if (!rootTile)
        rootTile = root_tile(dpy);

    XRenderComposite(dpy, PictOpSrc,
                     rootTile, None, rootBuffer,
                     0, 0, 0, 0, 0, 0, root_width, root_height);
}

static XserverRegion
win_extents(Display *dpy, win_it w) {
    XRectangle r = {static_cast<short>(w->a.x),
                    static_cast<short>(w->a.y),
                    static_cast<unsigned short>(w->a.width + w->a.border_width * 2),
                    static_cast<unsigned short>(w->a.height + w->a.border_width * 2)};
    return XFixesCreateRegion(dpy, &r, 1);
}

static XserverRegion
border_size(Display *dpy, win_it w) {
    XserverRegion border;
    /*
     * if window doesn't exist anymore,  this will generate an error
     * as well as not generate a region.  Perhaps a better XFixes
     * architecture would be to have a request that copies instead
     * of creates, that way you'd just end up with an empty region
     * instead of an invalid XID.
     */
    set_ignore(dpy, NextRequest (dpy));
    border = XFixesCreateRegionFromWindow(dpy, w->id, WindowRegionBounding);
    /* translate this */
    set_ignore(dpy, NextRequest (dpy));
    XFixesTranslateRegion(dpy, border,
                          w->a.x + w->a.border_width,
                          w->a.y + w->a.border_width);
    return border;
}

static void
paint_all(Display *dpy, XserverRegion region) {

    if (!region) {
        XRectangle r = {0, 0, static_cast<unsigned short>(root_width), static_cast<unsigned short>(root_height)};
        region = XFixesCreateRegion(dpy, &r, 1);
    }
#if MONITOR_REPAINT
    rootBuffer = rootPicture;
#else
    if (!rootBuffer) {
        Pixmap rootPixmap = XCreatePixmap(dpy, root, root_width, root_height,
                                          DefaultDepth (dpy, scr));
        rootBuffer = XRenderCreatePicture(dpy, rootPixmap,
                                          XRenderFindVisualFormat(dpy,
                                                                  DefaultVisual (dpy, scr)),
                                          0, nullptr);
        XFreePixmap(dpy, rootPixmap);
    }
#endif
    XFixesSetPictureClipRegion(dpy, rootPicture, 0, 0, region);
#if MONITOR_REPAINT
    XRenderComposite (dpy, PictOpSrc, blackPicture, None, rootPicture,
              0, 0, 0, 0, 0, 0, root_width, root_height);
#endif
#if DEBUG_REPAINT
    printf ("paint:");
#endif

    std::list<win *> transparent;
    for (auto w = win_list.begin(); w != win_list.end(); w++) {
#if CAN_DO_USABLE
        if (!w->usable)
        continue;
#endif
        /* never painted, ignore it */
        if (!w->damaged)
            continue;
        /* if invisible, ignore it */
        if (w->a.x + w->a.width < 1 || w->a.y + w->a.height < 1
            || w->a.x >= root_width || w->a.y >= root_height)
            continue;
        if (!w->picture) {
            XRenderPictureAttributes pa;
            XRenderPictFormat *format;
            Drawable draw = w->id;

#if HAS_NAME_WINDOW_PIXMAP
            if (hasNamePixmap && !w->pixmap)
                w->pixmap = XCompositeNameWindowPixmap(dpy, w->id);
            if (w->pixmap)
                draw = w->pixmap;
#endif
            format = XRenderFindVisualFormat(dpy, w->a.visual);
            pa.subwindow_mode = IncludeInferiors;
            w->picture = XRenderCreatePicture(dpy, draw,
                                             format,
                                             CPSubwindowMode,
                                             &pa);
        }
#if DEBUG_REPAINT
        printf (" 0x%x", w->id);
#endif
        if (clipChanged) {
            if (w->borderSize) {
                set_ignore(dpy, NextRequest (dpy));
                XFixesDestroyRegion(dpy, w->borderSize);
                w->borderSize = None;
            }
            if (w->extents) {
                XFixesDestroyRegion(dpy, w->extents);
                w->extents = None;
            }
            if (w->borderClip) {
                XFixesDestroyRegion(dpy, w->borderClip);
                w->borderClip = None;
            }
        }
        if (!w->borderSize)
            w->borderSize = border_size(dpy, w);
        if (!w->extents)
            w->extents = win_extents(dpy, w);

        if (w->mode == WINDOW_SOLID) {
            int x, y, wid, hei;
#if HAS_NAME_WINDOW_PIXMAP
            x = w->a.x;
            y = w->a.y;
            wid = w->a.width + w->a.border_width * 2;
            hei = w->a.height + w->a.border_width * 2;
#else
            x = w->a.x + w->a.border_width;
        y = w->a.y + w->a.border_width;
        wid = w->a.width;
        hei = w->a.height;
#endif
            XFixesSetPictureClipRegion(dpy, rootBuffer, 0, 0, region);
            set_ignore(dpy, NextRequest (dpy));
            XFixesSubtractRegion(dpy, region, region, w->borderSize);
            set_ignore(dpy, NextRequest (dpy));
            XRenderComposite(dpy, PictOpSrc, w->picture, None, rootBuffer,
                             0, 0, 0, 0,
                             x, y, wid, hei);
        }
        if (!w->borderClip) {
            w->borderClip = XFixesCreateRegion(dpy, nullptr, 0);
            XFixesCopyRegion(dpy, w->borderClip, region);
            XFixesIntersectRegion(dpy, w->borderClip, w->borderClip, w->borderSize);
        }
        transparent.push_front(&*w);
    }
#if DEBUG_REPAINT
    printf ("\n");
    fflush (stdout);
#endif
    XFixesSetPictureClipRegion(dpy, rootBuffer, 0, 0, region);
    paint_root(dpy);
    for (win *w : transparent) {
        XFixesSetPictureClipRegion(dpy, rootBuffer, 0, 0, w->borderClip);
        switch (compMode) {
            case CompSimple:
                break;
        }
        if (w->opacity != OPAQUE && !w->alphaPict)
            w->alphaPict = solid_picture(dpy, False,
                                         (double) w->opacity / OPAQUE, 0, 0, 0);
        if (w->mode == WINDOW_TRANS) {
            int x, y, wid, hei;
#if HAS_NAME_WINDOW_PIXMAP
            x = w->a.x;
            y = w->a.y;
            wid = w->a.width + w->a.border_width * 2;
            hei = w->a.height + w->a.border_width * 2;
#else
            x = w->a.x + w->a.border_width;
        y = w->a.y + w->a.border_width;
        wid = w->a.width;
        hei = w->a.height;
#endif
            set_ignore(dpy, NextRequest (dpy));
            XRenderComposite(dpy, PictOpOver, w->picture, w->alphaPict, rootBuffer,
                             0, 0, 0, 0,
                             x, y, wid, hei);
        } else if (w->mode == WINDOW_ARGB) {
            int x, y, wid, hei;
#if HAS_NAME_WINDOW_PIXMAP
            x = w->a.x;
            y = w->a.y;
            wid = w->a.width + w->a.border_width * 2;
            hei = w->a.height + w->a.border_width * 2;
#else
            x = w->a.x + w->a.border_width;
        y = w->a.y + w->a.border_width;
        wid = w->a.width;
        hei = w->a.height;
#endif
            set_ignore(dpy, NextRequest (dpy));
            XRenderComposite(dpy, PictOpOver, w->picture, w->alphaPict, rootBuffer,
                             0, 0, 0, 0,
                             x, y, wid, hei);
        }
        XFixesDestroyRegion(dpy, w->borderClip);
        w->borderClip = None;
    }
    XFixesDestroyRegion(dpy, region);
    if (rootBuffer != rootPicture) {
        XFixesSetPictureClipRegion(dpy, rootBuffer, 0, 0, None);
        XRenderComposite(dpy, PictOpSrc, rootBuffer, None, rootPicture,
                         0, 0, 0, 0, 0, 0, root_width, root_height);
    }
}

static void
add_damage(Display *dpy, XserverRegion damage) {
    if (allDamage) {
        XFixesUnionRegion(dpy, allDamage, allDamage, damage);
        XFixesDestroyRegion(dpy, damage);
    } else
        allDamage = damage;
}

static void
repair_win(Display *dpy, win_it w) {
    XserverRegion parts;

    if (!w->damaged) {
        parts = win_extents(dpy, w);
        set_ignore(dpy, NextRequest (dpy));
        XDamageSubtract(dpy, w->damage, None, None);
    } else {
        parts = XFixesCreateRegion(dpy, nullptr, 0);
        set_ignore(dpy, NextRequest (dpy));
        XDamageSubtract(dpy, w->damage, None, parts);
        XFixesTranslateRegion(dpy, parts,
                              w->a.x + w->a.border_width,
                              w->a.y + w->a.border_width);
    }
    add_damage(dpy, parts);
    w->damaged = 1;
}

static unsigned int
get_opacity_prop(Display *dpy, win_it w, unsigned int def);

static void
map_win(Display *dpy, Window id) {
    auto w = find_win(id);

    if (w == win_list.end())
        return;

    w->a.map_state = IsViewable;

    /* This needs to be here or else we lose transparency messages */
    XSelectInput(dpy, id, PropertyChangeMask);

    /* This needs to be here since we don't get PropertyNotify when unmapped */
    w->opacity = get_opacity_prop(dpy, w, OPAQUE);
    determine_mode(dpy, w);

#if CAN_DO_USABLE
    w->damage_bounds.x = w->damage_bounds.y = 0;
    w->damage_bounds.width = w->damage_bounds.height = 0;
#endif
    w->damaged = 0;
}

static void
finish_unmap_win(Display *dpy, win_it w) {
    w->damaged = 0;
#if CAN_DO_USABLE
    w->usable = False;
#endif
    if (w->extents != None) {
        add_damage(dpy, w->extents);    /* destroys region */
        w->extents = None;
    }

#if HAS_NAME_WINDOW_PIXMAP
    if (w->pixmap) {
        XFreePixmap(dpy, w->pixmap);
        w->pixmap = None;
    }
#endif

    if (w->picture) {
        set_ignore(dpy, NextRequest (dpy));
        XRenderFreePicture(dpy, w->picture);
        w->picture = None;
    }

    /* don't care about properties anymore */
    set_ignore(dpy, NextRequest (dpy));
    XSelectInput(dpy, w->id, 0);

    if (w->borderSize) {
        set_ignore(dpy, NextRequest (dpy));
        XFixesDestroyRegion(dpy, w->borderSize);
        w->borderSize = None;
    }
    if (w->borderClip) {
        XFixesDestroyRegion(dpy, w->borderClip);
        w->borderClip = None;
    }

    clipChanged = True;
}

#if HAS_NAME_WINDOW_PIXMAP

static void
unmap_callback(Display *dpy, win_it w, Bool gone) {
    finish_unmap_win(dpy, w);
}

#endif

static void
unmap_win(Display *dpy, Window id, Bool fade) {
    win_it w = find_win(id);
    if (w == win_list.end())
        return;
    w->a.map_state = IsUnmapped;
    finish_unmap_win(dpy, w);
}

/* Get the opacity prop from window
   not found: default
   otherwise the value
 */
static unsigned int
get_opacity_prop(Display *dpy, win_it w, unsigned int def) {
    Atom actual;
    int format;
    unsigned long n, left;

    unsigned char *data;
    int result = XGetWindowProperty(dpy, w->id, opacityAtom, 0L, 1L, False,
                                    XA_CARDINAL, &actual, &format,
                                    &n, &left, &data);
    if (result == Success && data != nullptr) {
        unsigned int i;
        i = *data;
//        memcpy(&i, data, sizeof(unsigned int));
        XFree(data);
        return i;
    }
    return def;
}

/* Get the opacity property from the window in a percent format
   not found: default
   otherwise: the value
*/
static double
get_opacity_percent(Display *dpy, win_it w, double def) {
    unsigned int opacity = get_opacity_prop(dpy, w, (unsigned int) (OPAQUE * def));

    return opacity * 1.0 / OPAQUE;
}

/* determine mode for window all in one place.
   Future might check for menu flag and other cool things
*/

static Atom
get_wintype_prop(Display *dpy, Window w) {
    Atom actual;
    int format;
    unsigned long n, left;

    unsigned char *data;
    int result = XGetWindowProperty(dpy, w, winTypeAtom, 0L, 1L, False,
                                    XA_ATOM, &actual, &format,
                                    &n, &left, &data);

    if (result == Success && data != (unsigned char *) None) {
        Atom a;
//        memcpy(&a, data, sizeof(Atom));
        a = *data;
        XFree(data);
        return a;
    }
    return winNormalAtom;
}

static void
determine_mode(Display *dpy, win_it w) {
    int mode;
    XRenderPictFormat *format;

    /* if trans prop == -1 fall back on  previous tests*/

    if (w->alphaPict) {
        XRenderFreePicture(dpy, w->alphaPict);
        w->alphaPict = None;
    }
    format = w->a.c_class == InputOnly ? nullptr : XRenderFindVisualFormat(dpy, w->a.visual);

    if (format && format->type == PictTypeDirect && format->direct.alphaMask) {
        mode = WINDOW_ARGB;
    } else if (w->opacity != OPAQUE) {
        mode = WINDOW_TRANS;
    } else {
        mode = WINDOW_SOLID;
    }
    w->mode = mode;
    if (w->extents) {
        XserverRegion damage;
        damage = XFixesCreateRegion(dpy, nullptr, 0);
        XFixesCopyRegion(dpy, damage, w->extents);
        add_damage(dpy, damage);
    }
}

static Atom
determine_wintype(Display *dpy, Window w) {
    Window root_return, parent_return;
    Window *children = nullptr;
    unsigned int nchildren, i;
    Atom type;

    type = get_wintype_prop(dpy, w);
    if (type != winNormalAtom)
        return type;

    if (!XQueryTree(dpy, w, &root_return, &parent_return, &children,
                    &nchildren)) {
        /* XQueryTree failed. */
        if (children)
            XFree(children);
        return winNormalAtom;
    }

    for (i = 0; i < nchildren; i++) {
        type = determine_wintype(dpy, children[i]);
        if (type != winNormalAtom)
            return type;
    }

    if (children)
        XFree(children);

    return winNormalAtom;
}

#pragma clang diagnostic push
#pragma ide diagnostic ignored "DanglingPointers"

static void
add_win(Display *dpy, Window id, Window prev) {

    win placeholder = {.id = id};
    set_ignore(dpy, NextRequest (dpy));
    if (!XGetWindowAttributes(dpy, id, &placeholder.a)) {
        return;
    }

    placeholder.shaped = False;
    placeholder.shape_bounds.x = placeholder.a.x;
    placeholder.shape_bounds.y = placeholder.a.y;
    placeholder.shape_bounds.width = placeholder.a.width;
    placeholder.shape_bounds.height = placeholder.a.height;
    placeholder.damaged = 0;
#if CAN_DO_USABLE
    placeholder.usable = False;
#endif
#if HAS_NAME_WINDOW_PIXMAP
    placeholder.pixmap = None;
#endif
    placeholder.picture = None;
    if (placeholder.a.c_class == InputOnly) {
        placeholder.damage_sequence = 0;
        placeholder.damage = None;
    } else {
        placeholder.damage_sequence = NextRequest (dpy);
        placeholder.damage = XDamageCreate(dpy, id, XDamageReportNonEmpty);
        XShapeSelectInput(dpy, id, ShapeNotifyMask);
    }
    placeholder.alphaPict = None;
    placeholder.borderSize = None;
    placeholder.extents = None;
    placeholder.opacity = OPAQUE;

    placeholder.borderClip = None;

    placeholder.windowType = determine_wintype(dpy, placeholder.id);

    if (prev) {
        for (auto it = win_list.begin(); it != win_list.end(); ++it) {
            if (it->id == prev) {
                win_list.insert(it, placeholder);
                break;
            }
        }
    } else
        win_list.push_front(placeholder);

    if (placeholder.a.map_state == IsViewable)
        map_win(dpy, id);
}

#pragma clang diagnostic pop

static void
restack_win(Display *dpy, win_it w, win_it new_above) {
    auto old_above = win_list.begin();
    auto next_w = std::next(w);
    if (next_w != win_list.end())
        old_above = next_w;

    if (old_above != new_above) {
//        win_list.splice(next_w, win_list, new_above);
        win_list.splice(std::prev(new_above), win_list, w);
    }
}

static void
configure_win(Display *dpy, XConfigureEvent *ce) {
    win_it w = find_win(ce->window);
    XserverRegion damage = None;

    if (w == win_list.end()) {
        if (ce->window == root) {
            if (rootBuffer) {
                XRenderFreePicture(dpy, rootBuffer);
                rootBuffer = None;
            }
            root_width = ce->width;
            root_height = ce->height;
        }
        return;
    }
#if CAN_DO_USABLE
    if (w->usable)
#endif
    {
        damage = XFixesCreateRegion(dpy, nullptr, 0);
        if (w->extents != None)
            XFixesCopyRegion(dpy, damage, w->extents);
    }
    w->shape_bounds.x -= w->a.x;
    w->shape_bounds.y -= w->a.y;
    w->a.x = ce->x;
    w->a.y = ce->y;
    if (w->a.width != ce->width || w->a.height != ce->height) {
#if HAS_NAME_WINDOW_PIXMAP
        if (w->pixmap) {
            XFreePixmap(dpy, w->pixmap);
            w->pixmap = None;
            if (w->picture) {
                XRenderFreePicture(dpy, w->picture);
                w->picture = None;
            }
        }
#endif
    }
    w->a.width = ce->width;
    w->a.height = ce->height;
    w->a.border_width = ce->border_width;
    w->a.override_redirect = ce->override_redirect;
    restack_win(dpy, w, find_win(ce->above));
    if (damage) {
        XserverRegion extents = win_extents(dpy, w);
        XFixesUnionRegion(dpy, damage, damage, extents);
        XFixesDestroyRegion(dpy, extents);
        add_damage(dpy, damage);
    }
    w->shape_bounds.x += w->a.x;
    w->shape_bounds.y += w->a.y;
    if (!w->shaped) {
        w->shape_bounds.width = w->a.width;
        w->shape_bounds.height = w->a.height;
    }

    clipChanged = True;
}

static void
circulate_win(Display *dpy, XCirculateEvent *ce) {
    win_it w = find_win(ce->window);
    win_it new_above;

    if (w == win_list.end())
        return;

    if (ce->place == PlaceOnTop)
        new_above = win_list.begin();
    else
        new_above = win_list.end();
    restack_win(dpy, w, new_above);
    clipChanged = True;
}

static void
finish_destroy_win(Display *dpy, win_it w, Bool gone) {

    if (gone)
        finish_unmap_win(dpy, w);
    if (w->picture) {
        set_ignore(dpy, NextRequest (dpy));
        XRenderFreePicture(dpy, w->picture);
        w->picture = None;
    }
    if (w->alphaPict) {
        XRenderFreePicture(dpy, w->alphaPict);
        w->alphaPict = None;
    }
    if (w->damage != None) {
        set_ignore(dpy, NextRequest (dpy));
        XDamageDestroy(dpy, w->damage);
        w->damage = None;
    }
    win_list.erase(w);
}

#if HAS_NAME_WINDOW_PIXMAP

static void
destroy_callback(Display *dpy, win_it w, Bool gone) {
    finish_destroy_win(dpy, w, gone);
}

#endif

static void
destroy_win(Display *dpy, Window id, Bool gone) {
    win_it w = find_win(id);
    {
        finish_destroy_win(dpy, w, gone);
    }
}

/*
static void
dump_win (win *w)
{
    printf ("\t%08lx: %d x %d + %d + %d (%d)\n", w->id,
	    w->a.width, w->a.height, w->a.x, w->a.y, w->a.border_width);
}


static void
dump_wins (void)
{
    win	*w;

    printf ("windows:\n");
    for (w = list; w; w = w->next)
	dump_win (w);
}
*/

static void
damage_win(Display *dpy, XDamageNotifyEvent *de) {
    win_it w = find_win(de->drawable);

    if (w == win_list.end())
        return;
#if CAN_DO_USABLE
    if (!w->usable)
    {
    if (w->damage_bounds.width == 0 || w->damage_bounds.height == 0)
    {
        w->damage_bounds = de->area;
    }
    else
    {
        if (de->area.x < w->damage_bounds.x)
        {
        w->damage_bounds.width += (w->damage_bounds.x - de->area.x);
        w->damage_bounds.x = de->area.x;
        }
        if (de->area.y < w->damage_bounds.y)
        {
        w->damage_bounds.height += (w->damage_bounds.y - de->area.y);
        w->damage_bounds.y = de->area.y;
        }
        if (de->area.x + de->area.width > w->damage_bounds.x + w->damage_bounds.width)
        w->damage_bounds.width = de->area.x + de->area.width - w->damage_bounds.x;
        if (de->area.y + de->area.height > w->damage_bounds.y + w->damage_bounds.height)
        w->damage_bounds.height = de->area.y + de->area.height - w->damage_bounds.y;
    }
#if 0
    printf ("unusable damage %d, %d: %d x %d bounds %d, %d: %d x %d\n",
        de->area.x,
        de->area.y,
        de->area.width,
        de->area.height,
        w->damage_bounds.x,
        w->damage_bounds.y,
        w->damage_bounds.width,
        w->damage_bounds.height);
#endif
    if (w->damage_bounds.x <= 0 &&
        w->damage_bounds.y <= 0 &&
        w->a.width <= w->damage_bounds.x + w->damage_bounds.width &&
        w->a.height <= w->damage_bounds.y + w->damage_bounds.height)
    {
        clipChanged = True;
        w->usable = True;
    }
    }
    if (w->usable)
#endif
    repair_win(dpy, w);
}

#if DEBUG_SHAPE
static const char *
shape_kind(int kind)
{
  static char	buf[128];

  switch(kind){
  case ShapeBounding:
    return "ShapeBounding";
  case ShapeClip:
    return "ShapeClip";
  case ShapeInput:
    return "ShapeInput";
  default:
    sprintf (buf, "Shape %d", kind);
    return buf;
  }
}
#endif

static void
shape_win(Display *dpy, XShapeEvent *se) {
    win_it w = find_win(se->window);

    if (w == win_list.end())
        return;

    if (se->kind == ShapeClip || se->kind == ShapeBounding) {
        XserverRegion region0;
        XserverRegion region1;

#if DEBUG_SHAPE
        printf("win 0x%lx %s:%s %ux%u+%d+%d\n",
         (unsigned long) se->window,
         shape_kind(se->kind),
         (se->shaped == True) ? "true" : "false",
         se->width, se->height,
         se->x, se->y);
#endif

        clipChanged = True;

        region0 = XFixesCreateRegion(dpy, &w->shape_bounds, 1);

        if (se->shaped == True) {
            w->shaped = True;
            w->shape_bounds = {static_cast<short>(w->a.x + se->x),
                               static_cast<short>(w->a.y + se->y),
                               static_cast<unsigned short>(se->width),
                               static_cast<unsigned short>(se->height)};
        } else {
            w->shaped = False;
            w->shape_bounds = {static_cast<short>(w->a.x),
                               static_cast<short>(w->a.y),
                               static_cast<unsigned short>(w->a.width),
                               static_cast<unsigned short>(w->a.height)};
        }

        region1 = XFixesCreateRegion(dpy, &w->shape_bounds, 1);
        XFixesUnionRegion(dpy, region0, region0, region1);
        XFixesDestroyRegion(dpy, region1);

        /* ask for repaint of the old and new region */
        paint_all(dpy, region0);
    }
}

static int
error(Display *dpy, XErrorEvent *ev) {
    int o;
    const char *name = nullptr;
    static char buffer[256];

    if (should_ignore(dpy, ev->serial))
        return 0;

    if (ev->request_code == composite_opcode &&
        ev->minor_code == X_CompositeRedirectSubwindows) {
        fprintf(stderr, "Another composite manager is already running\n");
        exit(1);
    }

    o = ev->error_code - xfixes_error;
    switch (o) {
        case BadRegion:
            name = "BadRegion";
            break;
        default:
            break;
    }
    o = ev->error_code - damage_error;
    switch (o) {
        case BadDamage:
            name = "BadDamage";
            break;
        default:
            break;
    }
    o = ev->error_code - render_error;
    switch (o) {
        case BadPictFormat:
            name = "BadPictFormat";
            break;
        case BadPicture:
            name = "BadPicture";
            break;
        case BadPictOp:
            name = "BadPictOp";
            break;
        case BadGlyphSet:
            name = "BadGlyphSet";
            break;
        case BadGlyph:
            name = "BadGlyph";
            break;
        default:
            break;
    }

    if (name == nullptr) {
        buffer[0] = '\0';
        XGetErrorText(dpy, ev->error_code, buffer, sizeof(buffer));
        name = buffer;
    }

    fprintf(stderr, "error %d: %s request %d minor %d serial %lu\n",
            ev->error_code, (strlen(name) > 0) ? name : "unknown",
            ev->request_code, ev->minor_code, ev->serial);

/*    abort ();	    this is just annoying to most people */
    return 0;
}

static void
expose_root(Display *dpy, Window rootWin, XRectangle *rects, int nrects) {
    XserverRegion region = XFixesCreateRegion(dpy, rects, nrects);

    add_damage(dpy, region);
}

#if DEBUG_EVENTS
static int
ev_serial (XEvent *ev)
{
    if (ev->type & 0x7f != KeymapNotify)
    return ev->xany.serial;
    return NextRequest (ev->xany.display);
}

static char *
ev_name (XEvent *ev)
{
    static char	buf[128];
    switch (ev->type & 0x7f) {
    case Expose:
    return "Expose";
    case MapNotify:
    return "Map";
    case UnmapNotify:
    return "Unmap";
    case ReparentNotify:
    return "Reparent";
    case CirculateNotify:
    return "Circulate";
    default:
        if (ev->type == damage_event + XDamageNotify)
    {
        return "Damage";
    }
    else if (ev->type == xshape_event + ShapeNotify)
    {
        return "Shape";
    }
    sprintf (buf, "Event %d", ev->type);
    return buf;
    }
}

static Window
ev_window (XEvent *ev)
{
    switch (ev->type) {
    case Expose:
    return ev->xexpose.window;
    case MapNotify:
    return ev->xmap.window;
    case UnmapNotify:
    return ev->xunmap.window;
    case ReparentNotify:
    return ev->xreparent.window;
    case CirculateNotify:
    return ev->xcirculate.window;
    default:
        if (ev->type == damage_event + XDamageNotify)
    {
        return ((XDamageNotifyEvent *) ev)->drawable;
    }
    else if (ev->type == xshape_event + ShapeNotify)
    {
        return ((XShapeEvent *) ev)->window;
    }
    return 0;
    }
}
#endif

static void
usage(const char *program) {
    fprintf(stderr, "%s\n", program);
    fprintf(stderr, "usage: %s [options]\n%s\n", program,
            "Options:\n"
            "   -d display\n"
            "      Specifies which display should be managed.\n"
            "   -a\n"
            "      Use automatic server-side compositing. Faster, but no special effects.\n"
            "   -c\n"
            "      Draw client-side shadows with fuzzy edges.\n"
            "   -C\n"
            "      Avoid drawing shadows on dock/panel windows.\n"
            "   -f\n"
            "      Fade windows in/out when opening/closing.\n"
            "   -F\n"
            "      Fade windows during opacity changes.\n"
            "   -n\n"
            "      Normal client-side compositing with transparency support\n"
            "   -s\n"
            "      Draw server-side shadows with sharp edges.\n"
            "   -S\n"
            "      Enable synchronous operation (for debugging).\n"
    );
    exit(1);
}

static Bool
register_cm(Display *dpy) {
    Window w;
    Atom a;
    static char net_wm_cm[] = "_NET_WM_CM_Sxx";

    snprintf(net_wm_cm, sizeof(net_wm_cm), "_NET_WM_CM_S%d", scr);
    a = XInternAtom(dpy, net_wm_cm, False);

    w = XGetSelectionOwner(dpy, a);
    if (w != None) {
        XTextProperty tp;
        char **strs;
        int count;
        Atom winNameAtom = XInternAtom(dpy, "_NET_WM_NAME", False);

        if (!XGetTextProperty(dpy, w, &tp, winNameAtom) &&
            !XGetTextProperty(dpy, w, &tp, XA_WM_NAME)) {
            fprintf(stderr,
                    "Another composite manager is already running (0x%lx)\n",
                    (unsigned long) w);
            return False;
        }
        if (XmbTextPropertyToTextList(dpy, &tp, &strs, &count) == Success) {
            fprintf(stderr,
                    "Another composite manager is already running (%s)\n",
                    strs[0]);

            XFreeStringList(strs);
        }

        XFree(tp.value);

        return False;
    }

    w = XCreateSimpleWindow(dpy, RootWindow (dpy, scr), 0, 0, 1, 1, 0, None,
                            None);

    Xutf8SetWMProperties(dpy, w, "xcompmgr", "xcompmgr", nullptr, 0, nullptr, nullptr,
                         nullptr);

    XSetSelectionOwner(dpy, a, w, 0);

    return True;
}

int
main(int argc, char **argv) {
    Display *dpy;
    XEvent ev;
    Window root_return, parent_return;
    Window *children;
    unsigned int nchildren;
    int i;
    XRenderPictureAttributes pa;
    std::vector<XRectangle> expose_rects;
    int size_expose = 0;
    int n_expose = 0;
    pollfd ufd{};
    int p;
    int composite_major, composite_minor;
    char *display = nullptr;
    int o;

    while ((o = getopt(argc, argv, "D:I:O:d:r:o:l:t:scnfFCaS")) != -1) {
        switch (o) {
            case 'd':
                display = optarg;
                break;
            case 'n':
                compMode = CompSimple;
                break;
            case 'a':
                autoRedirect = True;
                break;
            case 'S':
                synchronize = True;
                break;
            default:
                usage(argv[0]);
                break;
        }
    }

    dpy = XOpenDisplay(display);
    if (!dpy) {
        fprintf(stderr, "Can't open display\n");
        exit(1);
    }
    XSetErrorHandler(error);
    if (synchronize)
        XSynchronize(dpy, 1);
    scr = DefaultScreen (dpy);
    root = RootWindow (dpy, scr);

    if (!XRenderQueryExtension(dpy, &render_event, &render_error)) {
        fprintf(stderr, "No render extension\n");
        exit(1);
    }
    if (!XQueryExtension(dpy, COMPOSITE_NAME, &composite_opcode,
                         &composite_event, &composite_error)) {
        fprintf(stderr, "No composite extension\n");
        exit(1);
    }
    XCompositeQueryVersion(dpy, &composite_major, &composite_minor);
#if HAS_NAME_WINDOW_PIXMAP
    if (composite_major > 0 || composite_minor >= 2)
        hasNamePixmap = True;
#endif

    if (!XDamageQueryExtension(dpy, &damage_event, &damage_error)) {
        fprintf(stderr, "No damage extension\n");
        exit(1);
    }
    if (!XFixesQueryExtension(dpy, &xfixes_event, &xfixes_error)) {
        fprintf(stderr, "No XFixes extension\n");
        exit(1);
    }
    if (!XShapeQueryExtension(dpy, &xshape_event, &xshape_error)) {
        fprintf(stderr, "No XShape extension\n");
        exit(1);
    }

    if (!register_cm(dpy)) {
        exit(1);
    }

    /* get atoms */
    opacityAtom = XInternAtom(dpy, OPACITY_PROP, False);
    winTypeAtom = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE", False);
    winDesktopAtom = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DESKTOP", False);
    winDockAtom = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DOCK", False);
    winToolbarAtom = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_TOOLBAR", False);
    winMenuAtom = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_MENU", False);
    winUtilAtom = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_UTILITY", False);
    winSplashAtom = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_SPLASH", False);
    winDialogAtom = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_DIALOG", False);
    winNormalAtom = XInternAtom(dpy, "_NET_WM_WINDOW_TYPE_NORMAL", False);

    pa.subwindow_mode = IncludeInferiors;

    root_width = DisplayWidth (dpy, scr);
    root_height = DisplayHeight (dpy, scr);

    rootPicture = XRenderCreatePicture(dpy, root,
                                       XRenderFindVisualFormat(dpy,
                                                               DefaultVisual (dpy, scr)),
                                       CPSubwindowMode,
                                       &pa);
    blackPicture = solid_picture(dpy, True, 1, 0, 0, 0);
    allDamage = None;
    clipChanged = True;
    XGrabServer(dpy);
    if (autoRedirect)
        XCompositeRedirectSubwindows(dpy, root, CompositeRedirectAutomatic);
    else {
        XCompositeRedirectSubwindows(dpy, root, CompositeRedirectManual);
        XSelectInput(dpy, root,
                     SubstructureNotifyMask |
                     ExposureMask |
                     StructureNotifyMask |
                     PropertyChangeMask);
        XShapeSelectInput(dpy, root, ShapeNotifyMask);
        XQueryTree(dpy, root, &root_return, &parent_return, &children, &nchildren);
        for (i = 0; i < nchildren; i++)
            add_win(dpy, children[i], i ? children[i - 1] : None);
        XFree(children);
    }
    XUngrabServer(dpy);
    ufd.fd = ConnectionNumber (dpy);
    ufd.events = POLLIN;
    if (!autoRedirect)
        paint_all(dpy, None);
    while (true) {
        /*	dump_wins (); */
        do {
            if (autoRedirect)
                XFlush(dpy);

            XNextEvent(dpy, &ev);
            if ((ev.type & 0x7f) != KeymapNotify)
                discard_ignore(dpy, ev.xany.serial);
#if DEBUG_EVENTS
            printf ("event %10.10s serial 0x%08x window 0x%08x\n",
            ev_name(&ev), ev_serial (&ev), ev_window (&ev));
#endif
            if (!autoRedirect)
                switch (ev.type) {
                    case CreateNotify:
                        add_win(dpy, ev.xcreatewindow.window, 0);
                        break;
                    case ConfigureNotify:
                        configure_win(dpy, &ev.xconfigure);
                        break;
                    case DestroyNotify:
                        destroy_win(dpy, ev.xdestroywindow.window, True);
                        break;
                    case MapNotify:
                        map_win(dpy, ev.xmap.window);
                        break;
                    case UnmapNotify:
                        unmap_win(dpy, ev.xunmap.window, True);
                        break;
                    case ReparentNotify:
                        if (ev.xreparent.parent == root)
                            add_win(dpy, ev.xreparent.window, 0);
                        else
                            destroy_win(dpy, ev.xreparent.window, False);
                        break;
                    case CirculateNotify:
                        circulate_win(dpy, &ev.xcirculate);
                        break;
                    case Expose:
                        if (ev.xexpose.window == root) {
                            int more = ev.xexpose.count + 1;
                            if (n_expose == size_expose) {
                                expose_rects.resize(size_expose + more);
                                size_expose += more;
                            }
                            expose_rects[n_expose].x = ev.xexpose.x;
                            expose_rects[n_expose].y = ev.xexpose.y;
                            expose_rects[n_expose].width = ev.xexpose.width;
                            expose_rects[n_expose].height = ev.xexpose.height;
                            n_expose++;
                            if (ev.xexpose.count == 0) {
                                expose_root(dpy, root, &expose_rects[0], n_expose);
                                n_expose = 0;
                            }
                        }
                        break;
                    case PropertyNotify:
                        for (p = 0; backgroundProps[p]; p++) {
                            if (ev.xproperty.atom == XInternAtom(dpy, backgroundProps[p], False)) {
                                if (rootTile) {
                                    XClearArea(dpy, root, 0, 0, 0, 0, True);
                                    XRenderFreePicture(dpy, rootTile);
                                    rootTile = None;
                                    break;
                                }
                            }
                        }
                        /* check if Trans property was changed */
                        if (ev.xproperty.atom == opacityAtom) {
                            /* reset mode and redraw window */
                            win_it w = find_win(ev.xproperty.window);
                            if (w != win_list.end()) {
                                w->opacity = get_opacity_prop(dpy, w, OPAQUE);
                                determine_mode(dpy, w);
                            }
                        }
                        break;
                    default:
                        if (ev.type == damage_event + XDamageNotify) {
                            damage_win(dpy, (XDamageNotifyEvent *) &ev);
                        } else if (ev.type == xshape_event + ShapeNotify) {
                            shape_win(dpy, (XShapeEvent *) &ev);
                        }
                        break;
                }
        } while (QLength (dpy));
        if (allDamage && !autoRedirect) {
            static int paint;
            paint_all(dpy, allDamage);
            paint++;
            XSync(dpy, False);
            allDamage = None;
            clipChanged = False;
        }
    }
}

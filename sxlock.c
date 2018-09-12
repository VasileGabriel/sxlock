/*
 * MIT/X Consortium License
 *
 * © 2013 Jakub Klinkovský <kuba.klinkovsky at gmail dot com>
 * © 2010-2011 Ben Ruijl
 * © 2006-2008 Anselm R Garbe <garbeam at gmail dot com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the "Software"),
 * to deal in the Software without restriction, including without limitation
 * the rights to use, copy, modify, merge, publish, distribute, sublicense,
 * and/or sell copies of the Software, and to permit persons to whom the
 * Software is furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
 * FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
 * DEALINGS IN THE SOFTWARE.
 *
 */

#include <stdarg.h>     // variable arguments number
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>      // isprint()
#include <time.h>       // time()
#include <getopt.h>     // getopt_long()
#include <unistd.h>
#include <signal.h>
#include <sys/mman.h>   // mlock()
#include <X11/keysym.h>
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/dpms.h>
#include <X11/extensions/Xrandr.h>
#include <security/pam_appl.h>

#ifdef __GNUC__
    #define UNUSED(x) UNUSED_ ## x __attribute__((__unused__))
#else
    #define UNUSED(x) UNUSED_ ## x
#endif

typedef struct Dpms {
    BOOL state;
    CARD16 level;  // why?
    CARD16 standby, suspend, off;
} Dpms;

typedef struct WindowPositionInfo {
    int display_width, display_height;
    int output_x, output_y;
    int output_width, output_height;
} WindowPositionInfo;

typedef struct Rect {
    short x, y, size;
    Bool enabled;
} Rect;

typedef struct Amalgam {
    Window w;
    WindowPositionInfo *info;
    int line_x_left, base_y;
    GC *gcs;
} Amalgam;

static int conv_callback(int num_msgs, const struct pam_message **msg, struct pam_response **resp, void *appdata_ptr);


/* command-line arguments */
static char* opt_font;
static char* opt_username;
static char* opt_passchar;
static Bool  opt_hidelength;

/* need globals for signal handling */
Display *dpy;
Dpms dpms_original = { .state = True, .level = 0, .standby = 600, .suspend = 600, .off = 600 };  // holds original values
int dpms_timeout = 1000;  // dpms timeout until program exits
Bool using_dpms;
const int gccount = 5, whiteGC = 0, redGC = 1, blueGC = 2, greenGC = 3, yellowGC = 4, greyGC = 5;

pam_handle_t *pam_handle;
struct pam_conv conv = { conv_callback, NULL };

/* Holds the password you enter */
static char password[256];


static void
die(const char *errstr, ...) {
    va_list ap;
    va_start(ap, errstr);
    fprintf(stderr, "%s: ", PROGNAME);
    vfprintf(stderr, errstr, ap);
    va_end(ap);
    exit(EXIT_FAILURE);
}

/*
 * Clears the memory which stored the password to be a bit safer against
 * cold-boot attacks.
 *
 */
static void
clear_password_memory(void) {
    /* A volatile pointer to the password buffer to prevent the compiler from
     * optimizing this out. */
    volatile char *vpassword = password;
    for (unsigned int c = 0; c < sizeof(password); c++)
        /* rewrite with random values */
        vpassword[c] = rand();
}

/*
 * Callback function for PAM. We only react on password request callbacks.
 *
 */
static int
conv_callback(int num_msgs, const struct pam_message **msg, struct pam_response **resp, void *UNUSED(appdata_ptr)) {
    if (num_msgs == 0)
        return PAM_BUF_ERR;

    // PAM expects an array of responses, one for each message
    if ((*resp = calloc(num_msgs, sizeof(struct pam_message))) == NULL)
        return PAM_BUF_ERR;

    for (int i = 0; i < num_msgs; i++) {
        if (msg[i]->msg_style != PAM_PROMPT_ECHO_OFF &&
            msg[i]->msg_style != PAM_PROMPT_ECHO_ON)
            continue;

        // return code is currently not used but should be set to zero
        resp[i]->resp_retcode = 0;
        if ((resp[i]->resp = strdup(password)) == NULL) {
            free(*resp);
            return PAM_BUF_ERR;
        }
    }

    return PAM_SUCCESS;
}

void
handle_signal(int sig) {
    /* restore dpms settings */
    if (using_dpms) {
        DPMSSetTimeouts(dpy, dpms_original.standby, dpms_original.suspend, dpms_original.off);
        if (!dpms_original.state)
            DPMSDisable(dpy);
    }

    die("Caught signal %d; dying\n", sig);
}

void
position_rects(Rect* rects, int start_x, int start_y, int size, int gutter, int lights) {
    int rows = 3;
    int cols = 1;

    if (lights == 6) {
        cols = 2;
    }
    if (lights == 9) {
        cols = 3;
    }

    for (int i = 0; i < cols; i++) {
        for (int j = 0; j < rows; j++) {
            rects[j*cols+i].x = i*(size+gutter) + start_x;
            rects[j*cols+i].y = j*(size+gutter) + start_y;
            rects[j*cols+i].size = size;
        }
    }
}

void
enable_rects(Rect* rects, int n, int active_lights) {
    if (active_lights > n) {
        return;
    }

    while(active_lights) {
        int x = rand() % n;
        if (!rects[x].enabled) {
            rects[x].enabled = True;
            active_lights--;
        }
    }
}

void
reset_rects(Rect* rects, int n) {
    for (int i = 0; i < n; i++) {
        rects[i].enabled = False;
    }
}

void
draw_rects(Rect* rects, Window w, int n, GC activeGC, GC inactiveGC) {
    for (int i = 0; i < n; i++) {
        if (rects[i].enabled) {
            XFillRectangle(dpy, w, activeGC, rects[i].x, rects[i].y, rects[i].size, rects[i].size);
        } else {
            XFillRectangle(dpy, w, inactiveGC, rects[i].x, rects[i].y, rects[i].size, rects[i].size);
        }
    }
}

void
run_clock(Amalgam *a) {
    Window w = (*a).w;
     GC *gcs = (*a).gcs;
     WindowPositionInfo* info = (*a).info;
     int line_x_left = (*a).line_x_left;
     int base_y = (*a).base_y;

    clock_t begin = -1e9;
    double time_spent;
    int clock_interval = 1;

    int rect_size = info->output_width / 48;
    int gutter = 10, inter_sections = 20;
    int h1n = 3, h2n = 9, m1n = 6, m2n = 9;
    int rects_x_left = line_x_left+10;
    Rect *h1 = malloc(100*sizeof(Rect));
    Rect *h2 = malloc(100*sizeof(Rect));
    Rect *m1 = malloc(100*sizeof(Rect));
    Rect *m2 = malloc(100*sizeof(Rect));

    position_rects(h1, rects_x_left, base_y / 2, rect_size, gutter, h1n);
    position_rects(h2, rects_x_left+rect_size+gutter+inter_sections, base_y / 2, rect_size, gutter, h2n);
    position_rects(m1, rects_x_left+4*(rect_size+gutter)+2*inter_sections, base_y / 2, rect_size, gutter, m1n);
    position_rects(m2, rects_x_left+6*(rect_size+gutter)+3*inter_sections, base_y / 2, rect_size, gutter, m2n);


    while (1) {
        time_spent = (double)(clock() - begin) / CLOCKS_PER_SEC*25;
        if (time_spent>=clock_interval) {
            time_t now;
            struct tm *now_tm;
            int hour, minute;
            now = time(NULL);
            now_tm = localtime(&now);
            int tm_h2 = now_tm->tm_hour % 10;
            int tm_h1 = now_tm->tm_hour / 10;
            int tm_m2 = now_tm->tm_min % 10;
            int tm_m1 = now_tm->tm_min / 10;

            begin = clock();
            reset_rects(h1, h1n);
            draw_rects(h1, w, h1n, gcs[greyGC], gcs[greyGC]);
            enable_rects(h1, h1n, tm_h1);
            draw_rects(h1, w, h1n, gcs[redGC], gcs[greyGC]);

            reset_rects(h2, h2n);
            draw_rects(h2, w, h2n, gcs[greyGC], gcs[greyGC]);
            enable_rects(h2, h2n, tm_h2);
            draw_rects(h2, w, h2n, gcs[greenGC], gcs[greyGC]);

            reset_rects(m1, m1n);
            draw_rects(m1, w, m1n, gcs[greyGC], gcs[greyGC]);
            enable_rects(m1, m1n, tm_m1);
            draw_rects(m1, w, m1n, gcs[blueGC], gcs[greyGC]);

            reset_rects(m2, m2n);
            draw_rects(m2, w, m2n, gcs[greyGC], gcs[greyGC]);
            enable_rects(m2, m2n, tm_m2);
            draw_rects(m2, w, m2n, gcs[yellowGC], gcs[greyGC]);
        }
    }
}

void
main_loop(Window w, GC gcs[], XFontStruct* font, WindowPositionInfo* info, char passdisp[256], char* username, XColor UNUSED(black), XColor white, XColor red, Bool hidelength) {
    XEvent event;
    KeySym ksym;
    GC gc = gcs[whiteGC];

    unsigned int len = 0;
    Bool running = True;
    Bool sleepmode = False;
    Bool failed = False;

    XSync(dpy, False);

    /* define base coordinates - middle of screen */
    int base_x = info->output_x + info->output_width / 2;
    int base_y = info->output_y + info->output_height / 2;    /* y-position of the line */

    /* not changed in the loop */
    int line_x_left = base_x - info->output_width / 8;
    int line_x_right = base_x + info->output_width / 8;

    /* font properties */
    int ascent, descent;
    {
        int dir;
        XCharStruct overall;
        XTextExtents(font, passdisp, strlen(username), &dir, &ascent, &descent, &overall);
    }



    Amalgam *a = malloc(sizeof(Amalgam));
    pthread_t tid;
    (*a).w = w;
    (*a).gcs = gcs;
    (*a).info = info;
    (*a).base_y = base_y;
    (*a).line_x_left = line_x_left;

    pthread_create(&tid, NULL, &run_clock, a);

    /* main event loop */
    while(running && !XNextEvent(dpy, &event)) {
        if (sleepmode && using_dpms)
            DPMSForceLevel(dpy, DPMSModeOff);

        /* update window if no events pending */
        if (!XPending(dpy)) {
            int x;
            /* draw username and line */
            x = base_x - XTextWidth(font, username, strlen(username)) / 2;
            XDrawString(dpy, w, gc, x, base_y - 10, username, strlen(username));
            XDrawLine(dpy, w, gc, line_x_left, base_y, line_x_right, base_y);
            XDrawLine(dpy, w, gc, line_x_left, base_y, 0, info->output_height);
            XDrawLine(dpy, w, gc, line_x_right, base_y, info->output_width, 0);

            /* clear old passdisp */

            /* draw new passdisp or 'auth failed' */
            if (failed) {
                x = base_x - XTextWidth(font, "authentication failed", 21) / 2;
                XSetForeground(dpy, gc, red.pixel);
                XDrawString(dpy, w, gc, x, base_y + ascent + 20, "authentication failed", 21);
                XSetForeground(dpy, gc, white.pixel);
            } else {
                int lendisp = len;
                if (hidelength && len > 0)
                    lendisp += (passdisp[len] * len) % 5;
                x = base_x - XTextWidth(font, passdisp, lendisp) / 2;
            }
        }

        if (event.type == MotionNotify) {
            sleepmode = False;
            failed = False;
        }

        if (event.type == KeyPress) {
            sleepmode = False;
            failed = False;

            char inputChar = 0;
            XLookupString(&event.xkey, &inputChar, sizeof(inputChar), &ksym, 0);

            switch (ksym) {
                case XK_Return:
                case XK_KP_Enter:
                    password[len] = 0;
                    if (pam_authenticate(pam_handle, 0) == PAM_SUCCESS) {
                        clear_password_memory();
                        running = False;
                    } else {
                        failed = True;
                    }
                    len = 0;
                    break;
                case XK_Escape:
                    len = 0;
                    sleepmode = True;
                    break;
                case XK_BackSpace:
                    if (len)
                        --len;
                    break;
                default:
                    if (isprint(inputChar) && (len + sizeof(inputChar) < sizeof password)) {
                        memcpy(password + len, &inputChar, sizeof(inputChar));
                        len += sizeof(inputChar);
                    }
                    break;
            }
        }
    }
    pthread_exit(&tid);
}

Bool
parse_options(int argc, char** argv)
{
    static struct option opts[] = {
        { "font",           required_argument, 0, 'f' },
        { "help",           no_argument,       0, 'h' },
        { "passchar",       required_argument, 0, 'p' },
        { "username",       required_argument, 0, 'u' },
        { "hidelength",     no_argument,       0, 'l' },
        { "version",        no_argument,       0, 'v' },
        { 0, 0, 0, 0 },
    };

    for (;;) {
        int opt = getopt_long(argc, argv, "f:hp:u:v:l", opts, NULL);
        if (opt == -1)
            break;

        switch (opt) {
            case 'f':
                opt_font = optarg;
                break;
            case 'h':
                die("usage: "PROGNAME" [-hvl] [-p passchars] [-f fontname] [-u username]\n");
                break;
            case 'p':
                opt_passchar = optarg;
                break;
            case 'u':
                opt_username = optarg;
                break;
            case 'l':
                opt_hidelength = True;
                break;
            case 'v':
                die(PROGNAME"-"VERSION", © 2013 Jakub Klinkovský\n");
                break;
            default:
                return False;
        }
    }

    return True;
}

int
main(int argc, char** argv) {
    char passdisp[256];
    int screen_num;
    WindowPositionInfo info;

    Cursor invisible;
    Window root, w;
    XColor black, red, white, blue, green, yellow, grey;
    XFontStruct* font;

    GC gcs[gccount];
    memset(gcs, 0, gccount*sizeof(GC));

    /* get username (used for PAM authentication) */
    char* username;
    if ((username = getenv("USER")) == NULL)
        die("USER environment variable not set, please set it.\n");

    /* set default values for command-line arguments */
    opt_passchar = "*";
    opt_font = "-misc-fixed-medium-r-*--17-120-*-*-*-*-iso8859-1";
    opt_username = username;
    opt_hidelength = False;

    if (!parse_options(argc, argv))
        exit(EXIT_FAILURE);

    /* register signal handler function */
    if (signal (SIGINT, handle_signal) == SIG_IGN)
        signal (SIGINT, SIG_IGN);
    if (signal (SIGHUP, handle_signal) == SIG_IGN)
        signal (SIGHUP, SIG_IGN);
    if (signal (SIGTERM, handle_signal) == SIG_IGN)
        signal (SIGTERM, SIG_IGN);

    /* fill with password characters */
    for (unsigned int i = 0; i < sizeof(passdisp); i += strlen(opt_passchar))
        for (unsigned int j = 0; j < strlen(opt_passchar) && i + j < sizeof(passdisp); j++)
            passdisp[i + j] = opt_passchar[j];

    /* initialize random number generator */
    srand(time(NULL));

    if (!(dpy = XOpenDisplay(NULL)))
        die("cannot open dpy\n");

    if (!(font = XLoadQueryFont(dpy, opt_font)))
        die("error: could not find font. Try using a full description.\n");

    screen_num = DefaultScreen(dpy);
    root = DefaultRootWindow(dpy);

    /* get display/output size and position */
    {
        XRRScreenResources* screen = NULL;
        RROutput output;
        XRROutputInfo* output_info = NULL;
        XRRCrtcInfo* crtc_info = NULL;

        screen = XRRGetScreenResources (dpy, root);
        output = XRRGetOutputPrimary(dpy, root);

        /* When there is no primary output, the return value of XRRGetOutputPrimary
         * is undocumented, probably it is 0. Fall back to the first output in this
         * case, connected state will be checked later.
         */
        if (output == 0) {
            output = screen->outputs[0];
        }
        output_info = XRRGetOutputInfo(dpy, screen, output);

        /* Iterate through screen->outputs until connected output is found. */
        int i = 0;
        while (output_info->connection != RR_Connected || output_info->crtc == 0) {
            XRRFreeOutputInfo(output_info);
            output_info = XRRGetOutputInfo(dpy, screen, screen->outputs[i++]);
            fprintf(stderr, "Warning: no primary output detected, trying %s.\n", output_info->name);
            if (i == screen->noutput)
                die("error: no connected output detected.\n");
        }

        crtc_info = XRRGetCrtcInfo (dpy, screen, output_info->crtc);

        info.output_x = crtc_info->x;
        info.output_y = crtc_info->y;
        info.output_width = crtc_info->width;
        info.output_height = crtc_info->height;
        info.display_width = DisplayWidth(dpy, screen_num);
        info.display_height = DisplayHeight(dpy, screen_num);

        XRRFreeScreenResources(screen);
        XRRFreeOutputInfo(output_info);
        XRRFreeCrtcInfo(crtc_info);
    }

    /* allocate colors */
    {
        XColor dummy;
        Colormap cmap = DefaultColormap(dpy, screen_num);
        XAllocNamedColor(dpy, cmap, "orange red", &red, &dummy);
        XAllocNamedColor(dpy, cmap, "black", &black, &dummy);
        XAllocNamedColor(dpy, cmap, "white", &white, &dummy);
        XAllocNamedColor(dpy, cmap, "blue", &blue, &dummy);
        XAllocNamedColor(dpy, cmap, "green", &green, &dummy);
        XAllocNamedColor(dpy, cmap, "yellow", &yellow, &dummy);
        XAllocNamedColor(dpy, cmap, "grey", &grey, &dummy);
    }

    /* create window */
    {
        XSetWindowAttributes wa;
        wa.override_redirect = 1;
        wa.background_pixel = black.pixel;
        w = XCreateWindow(dpy, root, 0, 0, info.display_width, info.display_height,
                0, DefaultDepth(dpy, screen_num), CopyFromParent,
                DefaultVisual(dpy, screen_num), CWOverrideRedirect | CWBackPixel, &wa);
        XMapRaised(dpy, w);
    }

    /* define cursor */
    {
        char curs[] = {0, 0, 0, 0, 0, 0, 0, 0};
        Pixmap pmap = XCreateBitmapFromData(dpy, w, curs, 8, 8);
        invisible = XCreatePixmapCursor(dpy, pmap, pmap, &black, &black, 0, 0);
        XDefineCursor(dpy, w, invisible);
        XFreePixmap(dpy, pmap);
    }

    /* create Graphics Contexts */
    {
        XGCValues values;
        gcs[redGC] = XCreateGC(dpy, w, (unsigned long)0, &values);
        XSetFont(dpy, gcs[redGC], font->fid);
        XSetForeground(dpy, gcs[redGC], red.pixel);
    }
    {
        XGCValues values;
        gcs[whiteGC] = XCreateGC(dpy, w, (unsigned long)0, &values);
        XSetFont(dpy, gcs[whiteGC], font->fid);
        XSetForeground(dpy, gcs[whiteGC], white.pixel);
    }
    {
        XGCValues values;
        gcs[blueGC] = XCreateGC(dpy, w, (unsigned long)0, &values);
        XSetFont(dpy, gcs[blueGC], font->fid);
        XSetForeground(dpy, gcs[blueGC], blue.pixel);
    }
    {
        XGCValues values;
        gcs[greenGC] = XCreateGC(dpy, w, (unsigned long)0, &values);
        XSetFont(dpy, gcs[greenGC], font->fid);
        XSetForeground(dpy, gcs[greenGC], green.pixel);
    }
    {
        XGCValues values;
        gcs[yellowGC] = XCreateGC(dpy, w, (unsigned long)0, &values);
        XSetFont(dpy, gcs[yellowGC], font->fid);
        XSetForeground(dpy, gcs[yellowGC], yellow.pixel);
    }
    {
        XGCValues values;
        gcs[greyGC] = XCreateGC(dpy, w, (unsigned long)0, &values);
        XSetFont(dpy, gcs[greyGC], font->fid);
        XSetForeground(dpy, gcs[greyGC], grey.pixel);
    }

    /* grab pointer and keyboard */
    int len = 1000;
    while (len-- > 0) {
        if (XGrabPointer(dpy, root, False, ButtonPressMask | ButtonReleaseMask | PointerMotionMask,
                    GrabModeAsync, GrabModeAsync, None, invisible, CurrentTime) == GrabSuccess)
            break;
        usleep(50);
    }
    while (len-- > 0) {
        if (XGrabKeyboard(dpy, root, True, GrabModeAsync, GrabModeAsync, CurrentTime) == GrabSuccess)
            break;
        usleep(50);
    }
    if (len <= 0)
        die("Cannot grab pointer/keyboard\n");

    /* set up PAM */
    {
        int ret = pam_start("sxlock", username, &conv, &pam_handle);
        if (ret != PAM_SUCCESS)
            die("PAM: %s\n", pam_strerror(pam_handle, ret));
    }

    /* Lock the area where we store the password in memory, we don’t want it to
     * be swapped to disk. Since Linux 2.6.9, this does not require any
     * privileges, just enough bytes in the RLIMIT_MEMLOCK limit. */
    if (mlock(password, sizeof(password)) != 0)
        die("Could not lock page in memory, check RLIMIT_MEMLOCK\n");

    /* handle dpms */
    using_dpms = DPMSCapable(dpy);
    if (using_dpms) {
        /* save dpms timeouts to restore on exit */
        DPMSGetTimeouts(dpy, &dpms_original.standby, &dpms_original.suspend, &dpms_original.off);
        DPMSInfo(dpy, &dpms_original.level, &dpms_original.state);

        /* set program specific dpms timeouts */
        DPMSSetTimeouts(dpy, dpms_timeout, dpms_timeout, dpms_timeout);

        /* force dpms enabled until exit */
        DPMSEnable(dpy);
    }

    /* run main loop */
    main_loop(w, gcs, font, &info, passdisp, opt_username, black, white, red, opt_hidelength);

    /* restore dpms settings */
    if (using_dpms) {
        DPMSSetTimeouts(dpy, dpms_original.standby, dpms_original.suspend, dpms_original.off);
        if (!dpms_original.state)
            DPMSDisable(dpy);
    }

    XUngrabPointer(dpy, CurrentTime);
    XFreeFont(dpy, font);
    XFreeGC(dpy, gcs[whiteGC]);
    XDestroyWindow(dpy, w);
    XCloseDisplay(dpy);
    return 0;
}

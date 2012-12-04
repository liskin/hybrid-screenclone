#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <functional>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>

#include <X11/Xcursor/Xcursor.h>
#include <X11/Xlib.h>
#include <X11/Xproto.h>
#include <X11/cursorfont.h>
#include <X11/extensions/XShm.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xinerama.h>
#include <X11/extensions/record.h>

#define STR2(x) #x
#define STR(x) STR2( x )
#define ERR throw std::runtime_error( std::string() + __FILE__ + ":" + STR( __LINE__ ) + " " + __FUNCTION__ )

#if 1 // debugging off
# define DBG(x)
#else // debugging on
# define DBG(x) x
#endif

uint64_t microtime() {
    struct timeval tv;
    gettimeofday( &tv, NULL );
    return tv.tv_sec * 1000000 + tv.tv_usec;
}

#if 1 // time tracing off
# define TC(x) x
#else // time tracing on
# define TC(x) \
    ({ uint64_t t1, t2; t1 = microtime(); auto ret = ( x ); t2 = microtime(); \
    printf( "%15llu - %5d: %s\n", t2 - t1, __LINE__, #x ); ret; })
#endif

struct window;
struct xinerama_screen;

struct display {
    Display *dpy;
    int damage_event, damage_error;
    int xfixes_event, xfixes_error;

    display( const std::string &name );
    display clone() const;
    window root() const;
    XEvent next_event();
    int pending();

    template < typename Fun > void record_pointer_events( Fun *callback );
    void select_cursor_input( const window &win );

    typedef std::vector< xinerama_screen > screens_vector;
    screens_vector xinerama_screens();
};

struct window {
    const display *d;
    Window win;
    Damage dmg;

    window( const display &_d, Window _win ) : d( &_d ), win( _win ), dmg( 0 ) {}
    void create_damage();
    void clear_damage();
    void warp_pointer( int x, int y );
    void define_cursor( Cursor c );
};

struct xinerama_screen {
    const display *d;
    XineramaScreenInfo info;

    xinerama_screen( const display &_d, const XineramaScreenInfo &_info )
	: d( &_d ), info( _info ) {}

    bool in_screen( int x, int y ) const;
    bool intersect_rectangle( const XRectangle &rec ) const;
};

display::display( const std::string &name ) {
    dpy = XOpenDisplay( name.c_str() );
    if ( !dpy ) ERR;

    if ( !XDamageQueryExtension( dpy, &damage_event, &damage_error ) )
	ERR;
    if ( !XFixesQueryExtension( dpy, &xfixes_event, &xfixes_error ) )
	ERR;
}

display display::clone() const {
    return display( DisplayString( dpy ) );
}

window display::root() const {
    return window( *this, DefaultRootWindow( dpy ) );
}

XEvent display::next_event() {
    XEvent e;
    if ( XNextEvent( dpy, &e ) ) ERR;
    return e;
}

int display::pending() {
    return XPending( dpy );
}

template < typename Fun >
void record_callback( XPointer priv, XRecordInterceptData *data ) {
    Fun *f = (Fun *) priv;
    (*f)( data );
}

template < typename Fun >
void record_thread( display data, Fun *callback ) {
    int fd = ConnectionNumber( data.dpy );
    fd_set fds;
    FD_ZERO( &fds );

    for ( ;; ) {
	FD_SET( fd, &fds );
	select( fd + 1, &fds, NULL, NULL, NULL );
	XRecordProcessReplies( data.dpy );
    }
}

template < typename Fun >
void display::record_pointer_events( Fun *callback ) {
    display data = clone();

    XRecordRange *rr = XRecordAllocRange();
    if ( !rr ) ERR;
    rr->device_events.first = rr->device_events.last = MotionNotify;

    XRecordClientSpec rcs = XRecordAllClients;

    XRecordContext rc = XRecordCreateContext( dpy, 0, &rcs, 1, &rr, 1 );
    if ( !rc ) ERR;

    // sync, otherwise XRecordEnableContextAsync fails
    XSync( dpy, false );
    XSync( data.dpy, false );

    if ( !XRecordEnableContextAsync( data.dpy, rc, &record_callback< Fun >, (XPointer) callback ) )
	ERR;

    std::thread( &record_thread< Fun >, data, callback ).detach();
}

void display::select_cursor_input( const window &win ) {
    XFixesSelectCursorInput( dpy, win.win, XFixesDisplayCursorNotifyMask );
}

display::screens_vector display::xinerama_screens() {
    int number;
    XineramaScreenInfo *screens = XineramaQueryScreens( dpy, &number );
    if ( !screens ) ERR;

    screens_vector vec;
    for ( int i = 0; i < number; ++i )
	vec.push_back( xinerama_screen( *this, screens[ i ] ) );

    XFree( screens );
    return vec;
}

void window::create_damage() {
    if ( !( dmg = XDamageCreate( d->dpy, win, XDamageReportRawRectangles ) ) )
	ERR;
}

void window::clear_damage() {
    if ( !dmg ) ERR;

    XDamageSubtract( d->dpy, dmg, None, None );
}

void window::warp_pointer( int x, int y ) {
    TC( XWarpPointer( d->dpy, None, win, 0, 0, 0, 0, x, y ) );
}

void window::define_cursor( Cursor c ) {
    TC( XDefineCursor( d->dpy, win, c ) );
}

bool xinerama_screen::in_screen( int x, int y ) const {
    return x >= info.x_org && x < info.x_org + info.width
	&& y >= info.y_org && y < info.y_org + info.height;
}

bool segment_intersect( int a1, int a2, int b1, int b2 ) {
    return a1 < b1 ? a2 > b1 : b2 > a1;
}

bool xinerama_screen::intersect_rectangle( const XRectangle &rec ) const {
    return segment_intersect( rec.x, rec.x + rec.width,  info.x_org, info.x_org + info.width  )
	&& segment_intersect( rec.y, rec.y + rec.height, info.y_org, info.y_org + info.height );
}

struct image_replayer {
    const display *src, *dst;
    const xinerama_screen *src_screen;
    window src_window, dst_window;
    XShmSegmentInfo src_info, dst_info;
    XImage *src_image, *dst_image;
    GC dst_gc;
    bool damaged;

    image_replayer( const display &_src, const display &_dst, const xinerama_screen &_src_screen )
	: src( &_src ), dst( &_dst), src_screen( &_src_screen )
	, src_window( src->root() ), dst_window( dst->root() )
	, damaged( true )
    {	
	size_t sz = src_screen->info.width * src_screen->info.height * 4;
	src_info.shmid = dst_info.shmid = shmget( IPC_PRIVATE, sz, IPC_CREAT | 0666 );
	src_info.shmaddr = dst_info.shmaddr = (char *) shmat( src_info.shmid, 0, 0);
	src_info.readOnly = dst_info.readOnly = false;
	shmctl( src_info.shmid, IPC_RMID, NULL );

	src_image = XShmCreateImage( src->dpy, DefaultVisual( src->dpy, DefaultScreen( src->dpy ) ),
	    DefaultDepth( src->dpy, DefaultScreen( src->dpy ) ), ZPixmap, src_info.shmaddr,
	    &src_info, src_screen->info.width, src_screen->info.height );
	dst_image = XShmCreateImage( dst->dpy, DefaultVisual( dst->dpy, DefaultScreen( dst->dpy ) ),
	    DefaultDepth( dst->dpy, DefaultScreen( dst->dpy ) ), ZPixmap, dst_info.shmaddr,
	    &dst_info, src_screen->info.width, src_screen->info.height );

	XShmAttach( src->dpy, &src_info );
	XShmAttach( dst->dpy, &dst_info );

	dst_gc = DefaultGC( dst->dpy, DefaultScreen( dst->dpy ) );
    }

    void copy_if_damaged() {
	if ( !damaged )
	    return;

	TC( XShmGetImage( src->dpy, src_window.win, src_image,
		src_screen->info.x_org, src_screen->info.y_org, AllPlanes) );
	TC( XShmPutImage( dst->dpy, dst_window.win, dst_gc, dst_image, 0, 0, 0, 0,
		dst_image->width, dst_image->height, False ) );
	TC( XFlush( dst->dpy ) );

	DBG( std::cout << "damaged" << std::endl );

	damaged = false;
    }

    void damage( const XRectangle &rec ) {
	damaged = damaged || src_screen->intersect_rectangle( rec );
    }
};

struct mouse_replayer {
    const display src, dst;
    const xinerama_screen src_screen;
    window dst_window;
    Cursor invisibleCursor;
    volatile bool on;
    std::recursive_mutex cursor_mutex;

    mouse_replayer( const display &_src, const display &_dst, const xinerama_screen &_src_screen )
	: src( _src ), dst( _dst), src_screen( _src_screen ), dst_window( dst.root() )
	, on( false )
    {
	// create invisible cursor
	Pixmap bitmapNoData;
	XColor black;
	static char noData[] = { 0,0,0,0,0,0,0,0 };
	black.red = black.green = black.blue = 0;

	bitmapNoData = XCreateBitmapFromData( dst.dpy, dst_window.win, noData, 8, 8 );
	invisibleCursor = XCreatePixmapCursor( dst.dpy, bitmapNoData, bitmapNoData,
		&black, &black, 0, 0);

	dst_window.define_cursor( invisibleCursor );
    }

    void operator() ( XRecordInterceptData *data ) {
	if ( data->category == XRecordFromServer ) {
	    const xEvent &e = * (const xEvent *) data->data;

	    if ( e.u.u.type == MotionNotify ) {
		mouse_moved( e.u.keyButtonPointer.rootX, e.u.keyButtonPointer.rootY );
	    }
	}

	XRecordFreeData( data );
    }

    void mouse_moved( int x, int y ) {
	std::lock_guard< std::recursive_mutex > guard( cursor_mutex );

	bool old_on = on;
	on = src_screen.in_screen( x, y );

	if ( on )
	    dst_window.warp_pointer( x - src_screen.info.x_org, y - src_screen.info.y_org );
	else
	    // wiggle the cursor a bit to keep screensaver away
	    dst_window.warp_pointer( x % 50, y % 50 );

	if ( old_on != on ) {
	    if ( on )
		cursor_changed();
	    else
		dst_window.define_cursor( invisibleCursor );
	}

	TC( XFlush( dst.dpy ) );

	DBG( std::cout << "mouse moved" << std::endl );
    }

    void cursor_changed() {
	std::lock_guard< std::recursive_mutex > guard( cursor_mutex );

	if ( !on )
	    return;

	XFixesCursorImage *cur;
	XcursorImage image;
	Cursor cursor;

	cur = TC( XFixesGetCursorImage( src.dpy ) );
	memset( &image, 0, sizeof( image ) );
	image.width  = cur->width;
	image.height = cur->height;
	image.size   = std::max( image.width, image.height );
	image.xhot   = cur->xhot;
	image.yhot   = cur->yhot;

	if ( 0 && sizeof( * image.pixels ) == sizeof( * cur->pixels ) ) {
	    // 32-bit machines where int is long
	    image.pixels = (unsigned int *) cur->pixels;
	} else {
	    image.pixels = (unsigned int *) alloca(
		image.width * image.height * sizeof( unsigned int ) );
	    for ( unsigned i = 0; i < image.width * image.height; ++i )
		image.pixels[ i ] = cur->pixels[ i ];
	}

	cursor = TC( XcursorImageLoadCursor( dst.dpy, &image ) );
	XFree( cur );

	TC( XDefineCursor( dst.dpy, dst_window.win, cursor ) );
	XFreeCursor( dst.dpy, cursor );

	TC( XFlush( dst.dpy ) );

	DBG( std::cout << "cursor changed" << std::endl );
    }
};

void usage( const char *name )
{
    std::cerr
	<< "Usage: " << name << " <options>" << std::endl
	<< "Options:" << std::endl
	<< " -s <display name> (default :0)" << std::endl
	<< " -d <display name> (default :1)" << std::endl
	<< " -x <xinerama screen number> (default 0)" << std::endl;
    exit( 0 );
}

int main( int argc, char *argv[] )
{
    XInitThreads();

    std::string src_name( ":0" ), dst_name( ":1" );
    unsigned screen_number = 0;

    int opt;
    while ( ( opt = getopt( argc, argv, "s:d:x:h" ) ) != -1 )
	switch ( opt ) {
	case 's':
	    src_name = optarg;
	    break;
	case 'd':
	    dst_name = optarg;
	    break;
	case 'x':
	    screen_number = atoi( optarg );
	    break;
	default:
	    usage( argv[ 0 ] );
	}

    if ( src_name == dst_name )
	ERR;
    display src( src_name ), dst( dst_name );

    auto screens = src.xinerama_screens();
    if ( screen_number < 0 || screen_number >= screens.size() )
	ERR;
    auto &screen = screens[ screen_number ];

    // Clone src not to fight with the blocking loop.
    mouse_replayer mouse( src.clone(), dst, screen );
    image_replayer image( src, dst, screen );

    window root = src.root();
    root.create_damage();

    src.record_pointer_events( &mouse );
    src.select_cursor_input( root );

    for ( ;; ) {
	do {
	    const XEvent e = src.next_event();
	    if ( e.type == src.damage_event + XDamageNotify ) {
		const XDamageNotifyEvent &de = * (const XDamageNotifyEvent *) &e;
		image.damage( de.area );
	    } else if ( e.type == src.xfixes_event + XFixesCursorNotify ) {
		mouse.cursor_changed();
	    }
	} while ( src.pending() );

	root.clear_damage();
	image.copy_if_damaged();
    }
}

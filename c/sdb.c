/* -*- Mode: C; c-file-style: "stroustrup" -*- */

#include <stdio.h>
#include <time.h>
#include <stdlib.h>
#if defined( unix ) || defined(_WIN32 )
    #include <unistd.h>
#elif defined( VMS )
    #include <unixio.h>
#endif
#include <string.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include "sdb.h"
#include "types.h"
#include "utct.h"
#include "lock.h"

#if defined( VMS )
    #define unlink(x) delete(x)
#endif

#if defined( _WIN32 )
    #define ftruncate chsize
#endif

/* simple though inefficient implementation of a database function */

static int sdb_insert( DB*, const char* key, const char* val, int* err );

int sdb_open( DB* h, const char* filename, int* err ) 
{
    int fd;
    FILE* fp;

    *err = 0;
    h->file = NULL;
    if ( filename == NULL ) { return 0; }
    fd = open( filename, O_RDWR | O_CREAT, S_IREAD | S_IWRITE );
    if ( fd == -1 ) { goto fail; }
    fp = fdopen( fd, "w+" );
    if ( fp == NULL ) { goto fail; }
    h->file = fp;
    if ( !lock_write( h->file ) ) { goto fail; }
    strncpy( h->filename, filename, PATH_MAX ); h->filename[PATH_MAX] = '\0';
    h->write_pos = 0;
    return 1;
 fail:
    *err = errno;
    return 0;
}

int sdb_close( DB* h, int* err ) 
{
    if ( h == NULL || h->file == NULL ) { return 0; }
    if ( fclose( h->file ) == EOF ) { *err = errno; return 0; }
    *err = 0; return 1;
}

int sdb_add( DB* h, const char* key, const char* val, int* err )
{
    *err = 0;

    if ( h == NULL || h->file == NULL ) { return 0; }
    if ( strlen( key ) > MAX_KEY ) { return 0; }
    if ( strlen( val ) > MAX_VAL ) { return 0; }
    if ( fseek( h->file, 0, SEEK_END ) == -1 ) { goto fail; }
    if ( fprintf( h->file, "%s %s\n", key, val ) == 0 ) { goto fail; }
    return 1;
 fail:
    *err = errno;
    return 0;
}

#define MAX_LINE (MAX_KEY+MAX_VAL) 

static void sdb_rewind( DB* h ) 
{
    rewind( h->file );
    h->write_pos = 0;
}

int sdb_findfirst( DB* h, char* key, int klen, char* val, int vlen, int* err ) 
{
    sdb_rewind( h );
    return sdb_findnext( h, key, klen, val, vlen, err );
}

int sdb_findnext( DB* h, char* key, int klen, char* val, int vlen, int* err ) 
{
    char line[MAX_LINE+1];
    int line_len;
    char* fkey = line;
    char* fval;

    *err = 0;
    if ( h->file == NULL ) { return 0; }
    if ( feof( h->file ) ) { return 0; }
    if ( fgets( line, MAX_LINE, h->file ) == NULL ) { return 0; }
    line_len = strlen( line );
    if ( line_len == 0 ) { return 0; }

    /* remove unix, DOS, and MAC linefeeds */

    if ( line[line_len-1] == '\n' ) { line[--line_len] = '\0'; }
    if ( line[line_len-1] == '\r' ) { line[--line_len] = '\0'; }
    if ( line[line_len-1] == '\n' ) { line[--line_len] = '\0'; }

    fval = strchr( line, ' ' );
    if ( fval != NULL ) { *fval = '\0'; fval++; } 
    else { fval = ""; }		/* empty */
    strncpy( key, fkey, klen ); key[klen] = '\0';
    strncpy( val, fval, vlen ); val[vlen] = '\0';
    return 1;
}

static int sdb_cb_notkeymatch( const char* key, const char* val, 
			    void* arg, int* err ) 
{
    *err = 0;
    if ( strncmp( key, arg, MAX_KEY ) != 0 ) { return 1; }
    return 0;
} 

static int sdb_cb_keymatch( const char* key, const char* val, 
			    void* arg, int* err ) 
{
    *err = 0;
    if ( strncmp( key, arg, MAX_KEY ) == 0 ) { return 1; }
    return 0;
} 

int sdb_del( DB* h, const char* key, int* err ) 
{
    return sdb_updateiterate( h, (sdb_wcallback)sdb_cb_notkeymatch, 
			      (void*)key, err );
}

int sdb_lookup( DB* h, const char* key, char* val, int vlen, int* err ) 
{
    char fkey[MAX_KEY+1];
    return sdb_callbacklookup( h, sdb_cb_keymatch, (void*)key, 
			       fkey, MAX_KEY, val, vlen, err );
}

int sdb_lookupnext( DB* h, const char* key, char* val, int vlen, int* err ) 
{
    char fkey[MAX_KEY+1];
    return sdb_callbacklookupnext( h, sdb_cb_keymatch, (void*)key,
				   fkey, MAX_KEY, val, vlen, err );
}

int sdb_callbacklookup( DB* h, sdb_rcallback cb, void* arg, 
			char* key, int klen, char* val, int vlen, 
			int* err ) 
{
    rewind( h->file );
    return sdb_callbacklookupnext( h, cb, arg, key, klen, val, vlen, err );
}

int sdb_callbacklookupnext( DB* h, sdb_rcallback cb, void* arg, 
			    char* key, int klen, char* val, int vlen, 
			    int* err ) 
{
    char fkey[MAX_KEY+1];

    while ( sdb_findnext( h, fkey, MAX_KEY, val, vlen, err ) ) {
	if ( cb( fkey, val, arg, err ) ) {
	    strncpy( key, fkey, klen ); key[klen] = '\0';
	    return 1;
	}
    }
    return 0;
}

int sdb_insert( DB* db, const char* key, const char* val, int* err ) 
{
    int res;
    *err = 0;

    res = ftell( db->file ); 
    if ( res < 0 ) { goto fail; }
    db->read_pos = res;

    if ( fseek( db->file, db->write_pos, SEEK_SET ) == -1 ) { goto fail; }
    if ( fprintf( db->file, "%s %s\n", key, val ) == 0 ) { goto fail; }
    
    res = ftell( db->file );
    if ( res < 0 ) { goto fail; }
    db->write_pos = res;

    if ( fseek( db->file, db->read_pos, SEEK_SET ) == -1 ) { goto fail; }
    return 1;
 fail:
    *err = errno;
    return 0;
}

int sdb_updateiterate( DB* h, sdb_wcallback cb, void* arg, int* err )
{
    int found;
    char fkey[MAX_KEY+1];
    char fval[MAX_VAL+1];

    for ( found = sdb_findfirst( h, fkey, MAX_KEY, fval, MAX_VAL, err );
	  found;
	  found = sdb_findnext( h, fkey, MAX_KEY, fval, MAX_VAL, err ) ) {
	if ( cb( fkey, fval, arg, err ) ) {
	    if ( *err ) { goto fail; }
	    if ( !sdb_insert( h, fkey, fval, err ) ) { goto fail; }
	}
	else if ( *err ) { goto fail; }
    }

    ftruncate( fileno( h->file ), h->write_pos );
    return 1;
 fail:
    return 0;
}

typedef struct {
    char* key;
    char* nval;
} sdb_cb_update_arg;

static int sdb_cb_update( const char* key, char* val, void* arg, int* err )
{
    sdb_cb_update_arg* argt  = (sdb_cb_update_arg*)arg;
    *err = 0;
    if ( strncmp( argt->key, key, MAX_KEY ) == 0 ) {
	strncpy( val, argt->nval, MAX_VAL ); val[MAX_VAL] = '\0';
    }
    return 1;
}

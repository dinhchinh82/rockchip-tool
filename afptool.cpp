
/*
 * original "C version" author is unknown
 * Copyright (C) 2016 SoftPLC Corporation, Dick Hollenbeck <dick@softplc.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 3
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, you may find one here:
 * http://www.gnu.org/licenses/old-licenses/gpl-2.0.html
 * or you may search the http://www.gnu.org website for the version 2 license,
 * or you may write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA
 */


#include <stdio.h>
#include <string.h>
#include <err.h>
#include <stdlib.h>
#include <limits.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <string>
#include <vector>

#include "rkcrc.h"
#include "rkafp.h"
#include "rkrom.h"

#if defined(DEBUG)
 #define D(x)   x
#else
 #define D(x)
#endif


const char* appname;



uint32_t filestream_crc( FILE* fs, size_t stream_len )
{
    char buffer[1024*16];

    uint32_t crc = 0;

    unsigned read_len;
    while( stream_len &&
            (read_len = fread( buffer, 1, sizeof(buffer), fs ) ) != 0 )
    {
        RKCRC( crc, buffer, read_len );
        stream_len -= read_len;
    }

    return crc;
}


//////////////////////////////////////////////////////////////////////////////////////////////////////////////////
// unpack functions

int create_dir( char* dir )
{
    char* sep = dir;

    while( ( sep = strchr( sep, '/' ) ) != NULL )
    {
        *sep = '\0';

        if( mkdir( dir, 0755 ) != 0 && errno != EEXIST )
        {
            printf( "Can't create directory: %s\n", dir );
            return -1;
        }

        *sep++ = '/';
    }

    return 0;
}


int extract_file( FILE* fp, off_t offset, size_t len, const char* fullpath )
{
    char    buffer[1024*16];

    FILE*   fp_out = fopen( fullpath, "wb" );

    if( !fp_out )
    {
        printf( "Can't open/create file: %s\n", fullpath );
        return -1;
    }

    fseeko( fp, offset, SEEK_SET );

    while( len )
    {
        size_t  ask = len < sizeof(buffer) ? len : sizeof(buffer);
        size_t  got = fread( buffer, 1, ask, fp );

        if( ask != got )
        {
            printf(
                "extraction of file '%s' is bad; insufficient length in container image file\n"
                "ask=%zu got=%zu\n",
                fullpath,
                ask,
                got
                );
            break;
        }

        fwrite( buffer, got, 1, fp_out );
        len -= got;
    }

    fclose( fp_out );

    return 0;
}


int unpack_update( const char* srcfile, const char* dstdir )
{
    UPDATE_HEADER header;

    FILE* fp = fopen( srcfile, "rb" );

    if( !fp )
    {
        err( EXIT_FAILURE, "%s: can't open file '%s'", __func__, srcfile );
    }

    if( sizeof(header) != fread( &header, 1, sizeof(header), fp ) )
    {
        err( EXIT_FAILURE, "%s: can't read image header from file '%s'", __func__, srcfile );
    }

    if( strncmp( header.magic, RKAFP_MAGIC, sizeof(header.magic) ) != 0 )
    {
        err( EXIT_FAILURE, "%s: invalid header magic id in file '%s'", __func__, srcfile );
    }

    fseek( fp, 0, SEEK_END );

    unsigned filesize = ftell(fp);

    if( filesize - 4 < header.length )
    {
        printf( "update_header has length greater than file's length, cannot check crc\n" );
    }
    else
    {
        fseek( fp, header.length, SEEK_SET );

        uint32_t crc;

        unsigned readcount;
        readcount = fread( &crc, 1, sizeof(crc), fp );

        if( sizeof(crc) != readcount )
        {
            fprintf( stderr, "Can't read crc checksum, readcount=%d header.len=%u\n",
                readcount, header.length );
        }

        printf( "Checking file's crc..." );
        fflush( stdout );

        fseek( fp, 0, SEEK_SET );

        if( crc != filestream_crc( fp, header.length ) )
        {
            if( filesize-4 > header.length )
                fprintf( stderr, "CRC mismatch, however the file's size was bigger than header indicated\n");
            else
                err( EXIT_FAILURE, "%s: invalid crc for file '%s'", __func__, srcfile );
        }
        else
            printf( "OK\n" );
    }

    printf( "------- UNPACKING %d parts -------\n", header.num_parts );

    if( header.num_parts )
    {
        char dir[4096];

        for( unsigned i = 0; i < header.num_parts; i++ )
        {
            UPDATE_PART* part = &header.parts[i];

            printf( "%-32s0x%08x  0x%08x",
                    part->fullpath,
                    part->image_offset,
                    part->file_size
                    );

            D(printf( " (%-7u) image_size:0x%08x (%-6u)  image_size:0x%08x (%-6u)",
                part->file_size,
                part->image_size,
                part->image_size,
                part->image_size,
                part->image_size
                );)

            printf( "\n" );

            if( !strcmp( part->fullpath, "SELF" ) )
            {
                printf( "Skip SELF file.\n" );
                continue;
            }

            if( !strcmp( part->fullpath, "RESERVED" ) )
            {
                printf( "Skip RESERVED file.\n" );
                continue;
            }

            if( memcmp( part->name, "parameter", 9 ) == 0 )
            {
                part->image_offset   += 8;
                part->file_size  -= 12;
            }

            snprintf( dir, sizeof(dir), "%s/%s", dstdir, part->fullpath );

            if( -1 == create_dir( dir ) )
                continue;

            if( part->image_offset + part->file_size > header.length )
            {
                fprintf( stderr, "Invalid part: %s\n", part->name );
                continue;
            }

            extract_file( fp, part->image_offset, part->file_size, dir );
        }
    }

    fclose( fp );

    return 0;

unpack_fail:

    if( fp )
    {
        fclose( fp );
    }

    return -1;
}


////////////////////////////////////////////////////////////////////////////////////////////////////////////
// pack functions

struct PACKAGE
{
    char        name[32];
    char        fullpath[60];
    uint32_t    image_offset;
    uint32_t    image_size;

    PACKAGE()
    {
        memset( this, 0, sizeof(*this) );
    }

    void Show()
    {
        printf( "name:%-34s image_offset:0x%08x  image_size:0x%08x fullpath:%s\n",
            name, image_offset, image_size, fullpath );
    }
};


struct PARTITION
{
    char        name[32];
    uint32_t    start;
    uint32_t    size;


    PARTITION()
    {
        memset( name, 0, sizeof(name) );
        start = 0;
        size  = 0;
    }

    PARTITION( const char* aName, unsigned aStart, unsigned aSize ) :
        start( aStart ),
        size( aSize )
    {
        strncpy( name, aName, sizeof(name) );
    }

    void Show()
    {
        printf( "name:%-34s start:0x%08x size:0x%08x\n",
            name, start, size );
    }
};

typedef std::vector<PACKAGE>    PACKAGES_BASE;
typedef std::vector<PARTITION>  PARTITIONS_BASE;

PARTITION FirstPartition( "parameter", 0, 0x2000 );


/**
 * Struct PARAMETERS
 * holds stuff from the parameter file.
 */
struct PARAMETERS
{
    unsigned        version;

    std::string     machine_model;
    std::string     machine_id;
    std::string     manufacturer;

    void Show()
    {
        printf( "version:%d.%d.%d  machine:%s  mfg:%s\n",
            version >> 24,  0xff & (version >> 16),  0xffff & version,
            machine_model.c_str(),
            manufacturer.c_str()
            );
    }
};


struct PACKAGES : public PACKAGES_BASE
{
    int GetPackages( const char* package_file );

    void Show()
    {
        printf( "num_packages:%zu\n", size() );

        for( unsigned i=0; i < size();  ++i )
            (*this)[i].Show();
    }

    PACKAGE* FindByName( const char* name )
    {
        for( unsigned i = 0; i < size(); ++i )
        {
            PACKAGE* pack = &(*this)[i];

            if( strcmp( pack->name, name ) == 0 )
                return pack;
        }

        return NULL;
    }

};


struct PARTITIONS : public PARTITIONS_BASE
{
    void Show()
    {
        printf( "num_partitions:%zu\n", size() );

        for( unsigned i=0;  i < size();  ++i )
            (*this)[i].Show();
    }

    PARTITION* FindByName( const char* name )
    {
        for( unsigned i=0;  i < size();  ++i )
        {
            PARTITION* part = &(*this)[i];

            if( !strcmp( part->name, name ) )
                return part;
        }

        if( !strcmp( name, FirstPartition.name ) )
        {
            return &FirstPartition;
        }

        return NULL;
    }
};


PARAMETERS  Parameters;

PACKAGES    Packages;

PARTITIONS  Partitions;


int parse_partitions( char* str )
{
    char*   parts = strchr( str, ':' );

    if( parts )
    {
        char*   token1 = NULL;

        ++parts;

        char* tok = strtok_r( parts, ",", &token1 );

        for( ; tok; tok = strtok_r( NULL, ",", &token1 ) )
        {
            int     i;
            char*   ptr;

            PARTITION   part;

            part.size = strtol( tok, &ptr, 16 );

            ptr = strchr( ptr, '@' );

            if( !ptr )
                continue;

            ++ptr;
            part.start = strtol( ptr, &ptr, 16 );

            for( ; *ptr && *ptr != '('; ptr++ )
                ;

            ++ptr;

            for( i = 0; i < sizeof(part.name) && *ptr && *ptr != ')'; ++i )
            {
                part.name[i] = *ptr++;
            }

            if( i < sizeof(part.name) )
                part.name[i] = '\0';
            else
                part.name[i - 1] = '\0';

            Partitions.push_back( part );
        }
    }

    return 0;
}


int action_parse_key( char* key, char* value )
{
    if( strcmp( key, "FIRMWARE_VER" ) == 0 )
    {
        unsigned a, b, c;

        sscanf( value, "%d.%d.%d", &a, &b, &c );
        Parameters.version = ROM_VERSION(a,b,c);
    }
    else if( strcmp( key, "MACHINE_MODEL" ) == 0 )
    {
        Parameters.machine_model = value;
    }
    else if( strcmp( key, "MACHINE_ID" ) == 0 )
    {
        Parameters.machine_id = value;
    }
    else if( strcmp( key, "MANUFACTURER" ) == 0 )
    {
        Parameters.manufacturer = value;
    }
    else if( strcmp( key, "CMDLINE" ) == 0 )
    {
        char*   token1 = NULL;
        char*   param = strtok_r( value, " ", &token1 );

        while( param )
        {
            char*   param_key = param;
            char*   param_value = strchr( param, '=' );

            if( param_value )
            {
                *param_value++ = '\0';

                if( strcmp( param_key, "mtdparts" ) == 0 )
                {
                    parse_partitions( param_value );
                }
            }

            param = strtok_r( NULL, " ", &token1 );
        }
    }

    return 0;
}


int parse_parameter( const char* fname )
{
    char    line[4096];
    char*   startp;
    char*   endp;
    char*   key;
    char*   value;
    FILE*   fp;

    if( ( fp = fopen( fname, "r" ) ) == NULL )
    {
        printf( "Can't open file: %s\n", fname );
        return -1;
    }

    while( fgets( line, sizeof(line), fp ) != NULL )
    {
        startp = line;
        endp = line + strlen( line ) - 1;

        if( *endp != '\n' && *endp != '\r' && !feof( fp ) )
            break;

        // trim line
        while( isspace( *startp ) )
            ++startp;

        while( isspace( *endp ) )
            --endp;

        endp[1] = 0;

        // skip UTF-8 BOM
        if( startp[0] == (char)0xEF && startp[1] == (char)0xBB && startp[2] == (char)0xBF)
            startp += 3;

        if( *startp == '#' || *startp == 0 )
            continue;

        key = startp;
        value = strchr( startp, ':' );

        if( !value )
            continue;

        *value = '\0';
        value++;

        action_parse_key( key, value );
    }

    if( !feof( fp ) )
    {
        printf( "parameter file has a very long line that I cannot handle!\n" );
        fclose( fp );
        return -3;
    }

    fclose( fp );

    return 0;
}


void append_package( const char* name, const char* path )
{
    PACKAGE pack;

    strncpy( pack.name, name, sizeof(pack.name) );
    strncpy( pack.fullpath, path, sizeof(pack.fullpath) );

    PARTITION* part = Partitions.FindByName( name );

    if( part )
    {
        pack.image_offset = part->start;
        pack.image_size   = part->size;
    }
    else
    {
        pack.image_offset = (unsigned) -1;
        pack.image_size   = 0;
    }

    Packages.push_back( pack );
}


int get_packages( const char* fname )
{
    char    line[4096];
    char*   startp;
    char*   endp;
    char*   name;
    char*   path;

    FILE*   fp = fopen( fname, "r" );

    if( !fp )
    {
        printf( "Can't open file '%s'\n", fname );
        return -1;
    }

    while( fgets( line, sizeof(line), fp ) != NULL )
    {
        startp = line;
        endp = line + strlen( line ) - 1;

        if( *endp != '\n' && *endp != '\r' && !feof( fp ) )
            break;

        // trim line
        while( isspace( *startp ) )
            ++startp;

        while( isspace( *endp ) )
            --endp;

        endp[1] = 0;

        if( *startp == '#' || *startp == 0 )
            continue;

        name = startp;

        while( *startp && *startp != ' ' && *startp != '\t' )
            startp++;

        while( *startp == ' ' || *startp == '\t' )
        {
            *startp = '\0';
            startp++;
        }

        path = startp;

        append_package( name, path );
    }

    if( !feof( fp ) )
    {
        printf( "File '%s' has a long which is too long for me, sorry!\n", fname );
        fclose( fp );
        return -3;
    }

    fclose( fp );

    return 0;
}


int import_package( FILE* fp_out, UPDATE_PART* pack, const char* path )
{
    char    buf[2048];      // must be 2048 for param part
    size_t  readlen;

    pack->image_offset = ftell( fp_out );

    FILE*   fp_in = fopen( path, "rb" );

    if( !fp_in )
    {
        fprintf( stderr, "Cannot open input file '%s'\n", path );
        return -1;
    }

    if( strcmp( pack->name, "parameter" ) == 0 )
    {
        uint32_t crc = 0;
        PARAM_HEADER* header = (PARAM_HEADER*) buf;

        memcpy( header->magic, "PARM", sizeof(header->magic) );

        readlen = fread( buf + sizeof(*header), 1,
                        sizeof(buf) - sizeof(*header) - sizeof(crc), fp_in );

        header->length = readlen;
        RKCRC( crc, buf + sizeof(*header), readlen );

        readlen += sizeof(*header);

        memcpy( buf + readlen, &crc, sizeof(crc) );
        readlen += sizeof(crc);
        memset( buf + readlen, 0, sizeof(buf) - readlen );

        fwrite( buf, 1, sizeof(buf), fp_out );
        pack->file_size += readlen;
        pack->image_size += sizeof(buf);
    }
    else
    {
        do {
            readlen = fread( buf, 1, sizeof(buf), fp_in );

            if( readlen == 0 )
                break;

            if( readlen < sizeof(buf) )
                memset( buf + readlen, 0, sizeof(buf) - readlen );

            fwrite( buf, 1, sizeof(buf), fp_out );
            pack->file_size += readlen;
            pack->image_size += sizeof(buf);
        } while( !feof( fp_in ) );
    }

    fclose( fp_in );

    return 0;
}


void append_crc( FILE* fp )
{
    off_t file_len = 0;

    fseeko( fp, 0, SEEK_END );
    file_len = ftello( fp );

    if( file_len == (off_t) -1 )
        return;

    fseek( fp, 0, SEEK_SET );

    printf( "Adding CRC...\n" );

    uint32_t crc = filestream_crc( fp, file_len );

    fseek( fp, 0, SEEK_END );
    fwrite( &crc, 1, sizeof(crc), fp );
}


int compute_cmdline( const char* srcdir )
{
    char    buf[4096];

    snprintf( buf, sizeof(buf), "%s/%s", srcdir, "package-file" );

    if( get_packages( buf ) )
        return -1;

    struct stat st;

    unsigned flash_offset = 0;

    fprintf( stderr, "fragment for CMDLINE:\n" );

    printf( "mtdparts=rk29xxnand:" );

    for( unsigned i=0, out=0; i < Packages.size();  ++i )
    {
        stat( Packages[i].fullpath, &st );

        // pad for a bigger boot loader that might be programmed/stuffed via dd
        if( !strcmp( Packages[i].name, "bootloader" ) )
            st.st_size += 1024*1024;

        Packages[i].image_size   = ((st.st_size + 511)/512) * 512 ;

        Packages[i].image_offset = flash_offset;

        flash_offset += Packages[i].image_size;

        // These should all be put early on in your package-file so that they
        // can come before the first visible partition after the 4mbyte boundary.

        if(    !strcmp( Packages[i].name, "SELF" )
            || !strcmp( Packages[i].name, "package-file" )
            || !strcmp( Packages[i].name, "parameter" )
            || !strcmp( Packages[i].name, "update-script" )
            || !strcmp( Packages[i].name, "RESERVED" )
            || !strcmp( Packages[i].name, "recover-script" )
            /*
            || !strcmp( Packages[i].name, "bootloader" )
            || !strcmp( Packages[i].name, "backup" )
            || !strcmp( Packages[i].name, "misc" )
            || !strcmp( Packages[i].name, "recovery" )
            */
          )
        {
            continue;
        }

        if( !out )
        {
            // first exported partition should be @ >= 4 Mbyte
            if( Packages[i].image_offset < 0x2000 * 512 )
            {
                Packages[i].image_offset = 0x2000 * 512;
                flash_offset = Packages[i].image_offset + Packages[i].image_size;
            }
        }
        else
            printf( "," );

        if( i == Packages.size()-1 )
        {
            // last partition is set to expand on first boot, so
            // make sure of this partition name in your "package-file", should
            // be linuxroot for a linux ROM.
            printf( "-@0x%08x(%s)",
                Packages[i].image_offset / 512,
                Packages[i].name
                );
        }
        else
        {
            // 0x00008000@0x00002000(resource),0x00008000@0x0000A000(boot),-@0x00036000(linuxroot)
            printf( "0x%08x@0x%08x(%s)",
                (Packages[i].image_size + 512 - 1) / 512,
                Packages[i].image_offset / 512,
                Packages[i].name
                );
        }

        ++out;      // output count
    }

    printf( "\n" );

    return 0;
}


int pack_update( const char* srcdir, const char* dstfile )
{
    UPDATE_HEADER header;

    char    buf[4096];

    printf( "------ PACKAGE ------\n" );
    memset( &header, 0, sizeof(header) );

    snprintf( buf, sizeof(buf), "%s/%s", srcdir, "parameter" );

    if( parse_parameter( buf ) )
        return -1;

    snprintf( buf, sizeof(buf), "%s/%s", srcdir, "package-file" );

    if( get_packages( buf ) )
        return -1;

    FILE* fp = fopen( dstfile, "wb+" );

    if( !fp )
    {
        printf( "Can't open file \"%s\": %s\n", dstfile, strerror( errno ) );
        goto pack_failed;
    }

    // put out an inaccurate place holder, come back later and update it.
    fwrite( &header, sizeof(header), 1, fp );

    for( unsigned i=0;  i < Packages.size();  ++i )
    {
        strcpy( header.parts[i].name, Packages[i].name );
        strcpy( header.parts[i].fullpath, Packages[i].fullpath );

        header.parts[i].image_offset = Packages[i].image_offset;
        header.parts[i].image_size   = Packages[i].image_size;

        if( !strcmp( Packages[i].fullpath, "SELF" ) )
            continue;

        if( !strcmp( Packages[i].fullpath, "RESERVED" ) )
            continue;

        snprintf( buf, sizeof(buf), "%s/%s", srcdir, header.parts[i].fullpath );
        printf( "Adding file: %s\n", buf );
        import_package( fp, &header.parts[i], buf );
    }

    memcpy( header.magic, "RKAF", sizeof(header.magic) );
    strcpy( header.manufacturer, Parameters.manufacturer.c_str() );
    strcpy( header.model, Parameters.machine_model.c_str() );
    strcpy( header.id, Parameters.machine_id.c_str() );

    header.length = ftell( fp );
    header.num_parts = Packages.size();
    header.version = Parameters.version;

    for( int i = header.num_parts - 1; i >= 0; --i )
    {
        if( strcmp( header.parts[i].fullpath, "SELF" ) == 0 )
        {
            header.parts[i].file_size  = header.length + 4;
            header.parts[i].image_size = (header.parts[i].file_size + 511) / 512 * 512;
        }
    }

    fseek( fp, 0, SEEK_SET );
    fwrite( &header, sizeof(header), 1, fp );

    append_crc( fp );

    fclose( fp );

    printf( "------ OK ------\n" );

    return 0;

pack_failed:

    if( fp )
    {
        fclose( fp );
    }

    return -1;
}


void usage()
{
    printf( "USAGE:\n"
            "\t%s <-pack | -unpack> <src_dir> <out_dir>\n"
            "\t\t or\n"
            "\t%s -CMDLINE <src_dir>\n\n"
            "Examples:\n"
            "\t%s -pack src_dir update.img\tpack files\n"
            "\t%s -unpack update.img out_dir\tunpack files\n"
            "\t%s -CMDLINE src_dir > cmdline\tcapture CMDLINE fragment into cmdline\n",
            appname, appname, appname, appname, appname
            );
}


int main( int argc, char** argv )
{
    int ret = 0;

    appname = strrchr( argv[0], '/' );;

    if( appname )
        ++appname;
    else
        appname = argv[0];

    fprintf( stderr, "%s version: " __DATE__ "\n\n", appname );

    if( argc < 3 )
    {
        usage();
        return 1;
    }

    if( strcmp( argv[1], "-pack" ) == 0 && argc == 4 )
    {
        ret = pack_update( argv[2], argv[3] ) ;

        if( ret == 0 )
            printf( "Packed OK.\n" );
        else
            printf( "Packing failed!\n" );
    }

    else if( strcmp( argv[1], "-unpack" ) == 0 && argc == 4 )
    {
        ret = unpack_update( argv[2], argv[3] );

        if( ret == 0 )
            printf( "UnPacked OK.\n" );
        else
            printf( "UnPack failed!\n" );
    }

    else if( strcmp( argv[1], "-CMDLINE" ) == 0 && argc == 3 )
    {
        ret = compute_cmdline( argv[2] );
    }

    else
    {
        usage();
        ret = 2;
    }

    return ret;
}

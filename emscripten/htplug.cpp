/*
	"Highly Theoretical" (SegaCore - Saturn and Dreamcast sound emulation).
	Allows to play Sega Dreamcast Sound Format (.DSF/.MINIDSF) files and Sega Saturn Sound 
    Format (.SSF/.MINISSF) files.
	
	Based on kode54's: https://gitlab.kode54.net/kode54/foo_input_ht/

	As compared to kode54's original psf.cpp, the code has been patched to NOT
	rely on any fubar2000 base impls (same kind of changes that I used in "webn64").
	
	Copyright (C) 2018 Juergen Wothke
	
	Credits: The real work is in the core emulator and in whatever changes kode54 had 
	to apply to use it as a music player. I just added a bit of glue so the code can 
	now also be used on the Web.
	

    This program is free software; you can redistribute it and/or
    modify it under the terms of the GNU General Public License
    as published by the Free Software Foundation; either version 2
    of the License, or (at your option) any later version.
    
    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.
    
    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

#include <stdio.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <math.h>

#include <stdexcept>
#include <set>

#include <codecvt>
#include <locale>
#include <string>

#include <string.h>

#include <zlib.h>

#include <psflib.h>
#include <psf2fs.h>

#include "../Core/sega.h"
#include "../Core/dcsound.h"
#include "../Core/satsound.h"
#include "../Core/yam.h"
#include "circular_buffer.h"


#define t_int16   signed short
#define DWORD uint32_t
#define stricmp strcasecmp


// FIXME unused: maybe useful to eventually implement utf8 conversions..
std::wstring stringToWstring(const std::string& t_str) {
    //setup converter
    typedef std::codecvt_utf8<wchar_t> convert_type;
    std::wstring_convert<convert_type, wchar_t> converter;

    //use converter (.to_bytes: wstr->str, .from_bytes: str->wstr)
    return converter.from_bytes(t_str);
}

// implemented on JavaScript side (also see callback.js) for "on-demand" file load:
extern "C" int ht_request_file(const char *filename);

// just some fillers to change kode54's below code as little as possible
class exception_io_data: public std::runtime_error {
public:
	exception_io_data(const char *what= "exception_io_data") : std::runtime_error(what) {}
};
class exception_io_unsupported_format: public std::runtime_error {
public:
	exception_io_unsupported_format(const char *what= "exception_io_data") : std::runtime_error(what) {}
};
int stricmp_utf8(std::string const& s1, const char* s2) {	
    return strcasecmp(s1.c_str(), s2);
}
int stricmp_utf8(const char* s1, const char* s2) {	
    return strcasecmp(s1, s2);
}
int stricmp_utf8_partial(std::string const& s1,  const char* s2) {
	std::string s1pref= s1.substr(0, strlen(s2));	
    return strcasecmp(s1pref.c_str(), s2);
}

# define strdup(s)							      \
  (__extension__							      \
    ({									      \
      const char *__old = (s);						      \
      size_t __len = strlen (__old) + 1;				      \
      char *__new = (char *) malloc (__len);			      \
      (char *) memcpy (__new, __old, __len);				      \
    }))


#define trace(...) { fprintf(stderr, __VA_ARGS__); }
//#define trace(fmt,...)

// callback defined elsewhere 
extern void ht_meta_set(const char * name, const char * value);


#define DB_FILE FILE
	
struct FileAccess_t {	
	void* (*fopen)( const char * uri );
	size_t (*fread)( void * buffer, size_t size, size_t count, void * handle );
	int (*fseek)( void * handle, int64_t offset, int whence );

	long int (*ftell)( void * handle );
	int (*fclose)( void * handle  );

	size_t (*fgetlength)( FILE * f);
};
static struct FileAccess_t *g_file= 0;

static void * psf_file_fopen( const char * uri ) {
    return g_file->fopen( uri );
}
static size_t psf_file_fread( void * buffer, size_t size, size_t count, void * handle ) {
    return g_file->fread( buffer, size, count, handle );
}
static int psf_file_fseek( void * handle, int64_t offset, int whence ) {
    return g_file->fseek( handle, offset, whence );	
}
static int psf_file_fclose( void * handle ) {
    g_file->fclose( handle );
    return 0;	
}
static long psf_file_ftell( void * handle ) {
    return g_file->ftell( handle );	
}
const psf_file_callbacks psf_file_system = {
    "\\/|:",
    psf_file_fopen,
    psf_file_fread,
    psf_file_fseek,
    psf_file_fclose,
    psf_file_ftell
};


// ------------------ stripped down version based on foobar2000 plugin (see psf.cpp)  -------------------- 

//#define DBG(a) OutputDebugString(a)
#define DBG(a)

static int initialized = 0;
volatile long ssf_count = 0, dsf_count = 0;

static unsigned int cfg_deflength= 170000;
static unsigned int cfg_deffade= 10000;
static unsigned int cfg_suppressopeningsilence = 0; //1; XXX
static unsigned int cfg_suppressendsilence= 1;	// XXXX was 1
static unsigned int cfg_endsilenceseconds= 5;
static unsigned int cfg_dry= 1;
static unsigned int cfg_dsp= 1;
static unsigned int cfg_dsp_dynarec= 0;		// =1 NOT supported XXX?

static const char field_length[]="xsf_length";
static const char field_fade[]="xsf_fade";


void InterlockedIncrement(volatile long *in) {
	(*in)+=1;	// this sould be good enough here
}

#define BORK_TIME 0xC0CAC01A

static unsigned long parse_time_crap(const char *input)
{
    if (!input) return BORK_TIME;
    int len = strlen(input);
    if (!len) return BORK_TIME;
    int value = 0;
    {
        int i;
        for (i = len - 1; i >= 0; i--)
        {
            if ((input[i] < '0' || input[i] > '9') && input[i] != ':' && input[i] != ',' && input[i] != '.')
            {
                return BORK_TIME;
            }
        }
    }

    char * foo = strdup( input );

    if ( !foo )
        return BORK_TIME;

    char * bar = foo;
    char * strs = bar + strlen( foo ) - 1;
    char * end;
    while (strs > bar && (*strs >= '0' && *strs <= '9'))
    {
        strs--;
    }
    if (*strs == '.' || *strs == ',')
    {
        // fraction of a second
        strs++;
        if (strlen(strs) > 3) strs[3] = 0;
        value = strtoul(strs, &end, 10);
        switch (strlen(strs))
        {
        case 1:
            value *= 100;
            break;
        case 2:
            value *= 10;
            break;
        }
        strs--;
        *strs = 0;
        strs--;
    }
    while (strs > bar && (*strs >= '0' && *strs <= '9'))
    {
        strs--;
    }
    // seconds
    if (*strs < '0' || *strs > '9') strs++;
    value += strtoul(strs, &end, 10) * 1000;
    if (strs > bar)
    {
        strs--;
        *strs = 0;
        strs--;
        while (strs > bar && (*strs >= '0' && *strs <= '9'))
        {
            strs--;
        }
        if (*strs < '0' || *strs > '9') strs++;
        value += strtoul(strs, &end, 10) * 60000;
        if (strs > bar)
        {
            strs--;
            *strs = 0;
            strs--;
            while (strs > bar && (*strs >= '0' && *strs <= '9'))
            {
                strs--;
            }
            value += strtoul(strs, &end, 10) * 3600000;
        }
    }
    free( foo );
    return value;
}

// hack: dummy impl to replace foobar2000 stuff
const int MAX_INFO_LEN= 10;

class file_info {
	double len;
	
	// no other keys implemented
	const char* sampleRate;
	const char* channels;
	
	std::vector<std::string> requiredLibs;
	
public:
	file_info() {
		sampleRate = (const char*)malloc(MAX_INFO_LEN);
		channels = (const char*)malloc(MAX_INFO_LEN);
	}
	~file_info() {
		free((void*)channels);
		free((void*)sampleRate);
	}
	void reset() {
		requiredLibs.resize(0);
	}
	std::vector<std::string> get_required_libs() {
		return requiredLibs;
	}
	void info_set_int(const char *tag, int value) {
		if (!stricmp_utf8(tag, "samplerate")) {
			snprintf((char*)sampleRate, MAX_INFO_LEN, "%d", value);
		} else if (!stricmp_utf8(tag, "channels")) {
			snprintf((char*)channels, MAX_INFO_LEN, "%d", value);
		}
		// who cares.. just ignore
	}
	const char *info_get(std::string &t) {
		const char *tag= t.c_str();
		
		if (!stricmp_utf8(tag, "samplerate")) {
			return sampleRate;
		} else if (!stricmp_utf8(tag, "channels")) {
			return channels;
		}
		return "unavailable";
	}

	void set_length(double l) {
		len= l;
	}
	
	void info_set_lib(std::string& tag, const char * value) {
		// EMSCRIPTEN depends on all libs being loaded before the song can be played!
		requiredLibs.push_back(std::string(value));	
	}

	// 
	unsigned int meta_get_count() { return 0;}
	unsigned int meta_enum_value_count(unsigned int i) { return 0; }
	const char* meta_enum_value(unsigned int i, unsigned int k) { return "dummy";}
	void meta_modify_value(unsigned int i, unsigned int k, const char *value) {}
	unsigned int info_get_count() {return 0;}
	const char* info_enum_name(unsigned int i) { return "dummy";}

	void info_set(const char * name, const char * value) {}
	void info_set(std::string& name, const char * value) {}
	const char* info_enum_value(unsigned int i) {return "dummy";}
	void info_set_replaygain(const char *tag, const char *value) {}
	void info_set_replaygain(std::string &tag, const char *value) {}
	void meta_add(const char *tag, const char *value) {}
	void meta_add(std::string &tag, const char *value) {}
};


static void info_meta_ansi( file_info & info )
{
/* FIXME eventually migrate original impl
 
	for ( unsigned i = 0, j = info.meta_get_count(); i < j; i++ )
	{
		for ( unsigned k = 0, l = info.meta_enum_value_count( i ); k < l; k++ )
		{
			const char * value = info.meta_enum_value( i, k );
			info.meta_modify_value( i, k, pfc::stringcvt::string_utf8_from_ansi( value ) );
		}
	}
	for ( unsigned i = 0, j = info.info_get_count(); i < j; i++ )
	{
		const char * name = info.info_enum_name( i );
		
		if ( name[ 0 ] == '_' )
			info.info_set( std::string( name ), pfc::stringcvt::string_utf8_from_ansi( info.info_enum_value( i ) ) );
	}
*/
}

struct psf_info_meta_state
{
	file_info * info;

	std::string name;

	bool utf8;

	int tag_song_ms;
	int tag_fade_ms;

	psf_info_meta_state()
		: info( 0 ), utf8( false ), tag_song_ms( 0 ), tag_fade_ms( 0 ) {}
};

static int psf_info_meta(void * context, const char * name, const char * value) {
	// typical tags: _lib, _enablecompare(on), _enableFIFOfull(on), fade, volume
	// game, genre, year, copyright, track, title, length(x:x.xxx), artist
	
	// FIXME: various "_"-settings are currently not used to configure the emulator
//fprintf(stderr, "%s %s\n", name, value);	
	psf_info_meta_state * state = ( psf_info_meta_state * ) context;

	std::string & tag = state->name;

	tag.assign(name);

	if (!stricmp_utf8(tag, "game"))
	{
		DBG("reading game as album");
		tag.assign("album");
	}
	else if (!stricmp_utf8(tag, "year"))
	{
		DBG("reading year as date");
		tag.assign("date");
	}

	if (!stricmp_utf8_partial(tag, "replaygain_"))	// FIXME: how does relate to the "volume"?
	{
		DBG("reading RG info");
		state->info->info_set_replaygain(tag, value);
	}
	else if (!stricmp_utf8(tag, "length"))
	{
		DBG("reading length");
		int temp = parse_time_crap(value);
		if (temp != BORK_TIME)
		{
			state->tag_song_ms = temp;
			state->info->info_set_int(field_length, state->tag_song_ms);
		}
	}
	else if (!stricmp_utf8(tag, "fade"))
	{
		DBG("reading fade");
		int temp = parse_time_crap(value);
		if (temp != BORK_TIME)
		{
			state->tag_fade_ms = temp;
			state->info->info_set_int(field_fade, state->tag_fade_ms);
		}
	}
	else if (!stricmp_utf8(tag, "utf8"))
	{
		state->utf8 = true;
	}
	else if (!stricmp_utf8_partial(tag, "_lib"))
	{
		DBG("found _lib");
		state->info->info_set_lib(tag, value);
	}
	else if (tag[0] == '_')
	{
		DBG("found unknown required tag, failing");
		return -1;
	}
	else
	{
		state->info->meta_add( tag, value );
	}

	// handle description stuff elsewhere
	ht_meta_set(tag.c_str(), value);
	
	return 0;
}

// preserve API used by original fubar2000 array
class char_array
{
	uint8_t *buf;	
	int length;
public:
	char_array() {
		length= 0;
		buf= NULL;
		set_size(length);
	}
	~char_array() {
		free((void*)buf);
	}
	uint8_t* get_ptr() {
		return buf;
	}
	int get_size() {
		return length;
	}
	void set_size(int size) {
		length= size;
		if (buf != NULL) free(buf);
		
		buf= (uint8_t*)malloc(length); 
	}
};


struct sdsf_load_state
{
	char_array state;
};

static unsigned get_le32( void const* p )
{
    return  (unsigned) ((unsigned char const*) p) [3] << 24 |
            (unsigned) ((unsigned char const*) p) [2] << 16 |
            (unsigned) ((unsigned char const*) p) [1] <<  8 |
            (unsigned) ((unsigned char const*) p) [0];
}
static uint32_t byteswap_if_be_t(uint32_t in) {
	// mimick original fubar2000 API
#ifdef EMU_BIG_ENDIAN
	// should be dead code in EMSCRIPTEN
	in= get_le32((void const*)&in);
#endif
	return in;
}

static int sdsf_load(void * context, const uint8_t * exe, size_t exe_size,
                                  const uint8_t * reserved, size_t reserved_size)
{
// note: this will be called for all the libs 	
	
	if ( exe_size < 4 ) return -1;

    sdsf_load_state * state = ( sdsf_load_state * ) context;

	char_array & dst = state->state;

	if ( dst.get_size() < 4 )		// 1st file is just copied and later stuff gets appended?
	{
		dst.set_size( exe_size );
		memcpy( dst.get_ptr(), exe, exe_size );
		return 0;
	}

	uint32_t dst_start = byteswap_if_be_t( *(uint32_t*)(dst.get_ptr()) );
	uint32_t src_start = byteswap_if_be_t( *(uint32_t*)(exe) );
    dst_start &= 0x7FFFFF;
    src_start &= 0x7FFFFF;
	DWORD dst_len = dst.get_size() - 4;
	DWORD src_len = exe_size - 4;
	if ( dst_len > 0x800000 ) dst_len = 0x800000;
	if ( src_len > 0x800000 ) src_len = 0x800000;

	if ( src_start < dst_start )
	{
		DWORD diff = dst_start - src_start;
		dst.set_size( dst_len + 4 + diff );
		memmove( dst.get_ptr() + 4 + diff, dst.get_ptr() + 4, dst_len );
		memset( dst.get_ptr() + 4, 0, diff );
		dst_len += diff;
		dst_start = src_start;
		*(uint32_t*)(dst.get_ptr()) = byteswap_if_be_t( dst_start );
	}
	if ( ( src_start + src_len ) > ( dst_start + dst_len ) )
	{
		DWORD diff = ( src_start + src_len ) - ( dst_start + dst_len );
		dst.set_size( dst_len + 4 + diff );
		memset( dst.get_ptr() + 4 + dst_len, 0, diff );
		dst_len += diff;
	}

	memcpy( dst.get_ptr() + 4 + ( src_start - dst_start ), exe + 4, src_len );

	return 0;
}

struct usf_loader_state
{
	uint32_t enable_compare;
	uint32_t enable_fifo_full;
	void * emu_state;

	usf_loader_state()
		: enable_compare(0), enable_fifo_full(0),
		  emu_state(0)
	{ }

	~usf_loader_state()
	{
		if ( emu_state )
			free( emu_state );
	}
};

/*
	removed: inheritance, remove_tags(), retag(), etc
	
	also "migrated" the "seek_decode" and "decode_run" impls to the 
	one used in lazyusf2 (assuming that it is the more recent/better impl)
*/
class input_xsf
{
	const static t_int16 SEEK_BUF_SIZE= 1024;
	t_int16 seek_buffer[SEEK_BUF_SIZE * 2];	// discarded sample output during "seek"

	bool no_loop, eof;

	char_array sega_state;

	std::string m_path;

	int32_t sample_rate;

	int err;

	int xsf_version;

	int data_written,remainder;
	
	double startsilence,silence;

	int song_len,fade_len;
	int tag_song_ms,tag_fade_ms;

	file_info m_info;

	bool do_suppressendsilence;

	circular_buffer<t_int16> m_buffer;
public:
	input_xsf() : sample_rate(44100) {}

	~input_xsf() {
		if ( sega_state.get_size() )
		{
			void * yam = 0;
			if ( xsf_version == 0x12 )
			{
				void * dcsound = sega_get_dcsound_state( sega_state.get_ptr() );
				yam = dcsound_get_yam_state( dcsound );
			}
			else
			{
				void * satsound = sega_get_satsound_state( sega_state.get_ptr() );
				yam = satsound_get_yam_state( satsound );
			}
			if ( yam ) yam_unprepare_dynacode( yam );
		}
	}

	int32_t getDataWritten() { return data_written; }
	int32_t getSamplesToPlay() { return song_len + fade_len; }
	int32_t getSamplesRate() { return sample_rate; }
	
	
	std::vector<std::string> splitpath(const std::string& str, 
								const std::set<char> &delimiters) {

		std::vector<std::string> result;

		char const* pch = str.c_str();
		char const* start = pch;
		for(; *pch; ++pch) {
			if (delimiters.find(*pch) != delimiters.end()) {
				if (start != pch) {
					std::string str(start, pch);
					result.push_back(str);
				} else {
					result.push_back("");
				}
				start = pch + 1;
			}
		}
		result.push_back(start);

		return result;
	}
	
	int open(const char * p_path ) {
		m_info.reset();

		m_path = p_path;
		
		xsf_version = psf_load( p_path, &psf_file_system, 0, 0, 0, 0, 0, 0 );
		if ( xsf_version <= 0 ) throw exception_io_unsupported_format( "Not a PSF file" );

		if (xsf_version == 0x11) InterlockedIncrement(&ssf_count);
		else if (xsf_version == 0x12) InterlockedIncrement(&dsf_count);
		else throw exception_io_data( "Not a SSF or DSF file" );

		psf_info_meta_state info_state;
		info_state.info = &m_info;

		if ( psf_load( p_path, &psf_file_system, xsf_version, 0, 0, psf_info_meta, &info_state, 0 ) <= 0 )
			throw exception_io_data( "Failed to load tags" );

		if ( !info_state.utf8 )
			info_meta_ansi( m_info );

		tag_song_ms = info_state.tag_song_ms;
		tag_fade_ms = info_state.tag_fade_ms;

		if (!tag_song_ms)
		{
			tag_song_ms = cfg_deflength;
			tag_fade_ms = cfg_deffade;
		}

		m_info.set_length( (double)( tag_song_ms + tag_fade_ms ) * .001 );
		m_info.info_set_int( "samplerate", sample_rate );
		m_info.info_set_int( "channels", 2 );

		// song may depend on some lib-file(s) that first must be loaded! 
		// (enter "retry-mode" if something is missing)
		std::set<char> delims; delims.insert('\\'); delims.insert('/');
		
		std::vector<std::string> p = splitpath(m_path, delims);		
		std::string path= m_path.substr(0, m_path.length()-p.back().length());
		
		std::vector<std::string>libs= m_info.get_required_libs();
		for (std::vector<std::string>::const_iterator iter = libs.begin(); iter != libs.end(); ++iter) {
			const std::string& libName= *iter;
			const std::string libFile= path + libName;

			int r= ht_request_file(libFile.c_str());	// trigger load & check if ready
			if (r <0) {
				return -1; // file not ready
			}
		}	
		return 0;
	}


	void decode_initialize(t_int16 *output_buffer, int out_size) {
		{
			if (!initialized)
			{
				DBG("sega_init()");
				if (sega_init()) {
					throw exception_io_data("Sega emulator static initialization failed");
				}
				initialized = 1;
			}
		}

		if ( sega_state.get_size() )
		{
			void * yam = 0;
			if ( xsf_version == 0x12 )
			{
				void * dcsound = sega_get_dcsound_state( sega_state.get_ptr() );
				yam = dcsound_get_yam_state( dcsound );
			}
			else
			{
				void * satsound = sega_get_satsound_state( sega_state.get_ptr() );
				yam = satsound_get_yam_state( satsound );			
			}
			if ( yam ) yam_unprepare_dynacode( yam );
		}

		sega_state.set_size( sega_get_state_size( xsf_version - 0x10 ) );

		void * pEmu = sega_state.get_ptr();

		sega_clear_state( pEmu, xsf_version - 0x10 );

		sega_enable_dry( pEmu, cfg_dry ? 1 : !cfg_dsp );
		sega_enable_dsp( pEmu, cfg_dsp );

		int dynarec = cfg_dsp_dynarec;
		sega_enable_dsp_dynarec( pEmu, dynarec );

		if ( dynarec )
		{
			void * yam = 0;
			if ( xsf_version == 0x12 )
			{
				void * dcsound = sega_get_dcsound_state( pEmu );
				yam = dcsound_get_yam_state( dcsound );
			}
			else
			{
				void * satsound = sega_get_satsound_state( pEmu );
				yam = satsound_get_yam_state( satsound );
			}
			if ( yam ) yam_prepare_dynacode( yam );
		}

		sdsf_load_state state;

		if ( psf_load( m_path.c_str(), &psf_file_system, xsf_version, sdsf_load, &state, 0, 0, 0 ) < 0 ) {
fprintf(stderr, "ERROR psf_load [%s]\n", m_path.c_str());		
			throw exception_io_data( "Invalid SSF/DSF" );
		}
		uint32_t start = byteswap_if_be_t( *(uint32_t*)(state.state.get_ptr()) );
		DWORD length = state.state.get_size();
		DWORD max_length = ( xsf_version == 0x12 ) ? 0x800000 : 0x80000;
		if ((start + (length-4)) > max_length)
		{
			length = max_length - start + 4;
		}
		sega_upload_program( pEmu, state.state.get_ptr(), length );

		startsilence = silence = 0;

		eof = 0;
		err = 0;
		data_written = 0;
		remainder = 0;
		no_loop = 1; //( p_flags & input_flag_no_looping ) || !cfg_infinite;

		calcfade(); // XXXXdone at end

		do_suppressendsilence = !! cfg_suppressendsilence;

		unsigned skip_max = cfg_endsilenceseconds * 44100;

		if ( cfg_suppressopeningsilence ) // ohcrap
		{
			for (;;)
			{
				unsigned skip_howmany = skip_max - silence;
				if ( skip_howmany > out_size ) skip_howmany = out_size;

				int rtn = sega_execute( pEmu, 0x7FFFFFFF, output_buffer, & skip_howmany );
				if ( rtn < 0 ) { 
fprintf(stderr, "ERROR sega_execute\n");		
					throw exception_io_data();
				}
				short * foo = (short *)output_buffer;
				unsigned i;
				for ( i = 0; i < skip_howmany; ++i )
				{
					if ( foo[ 0 ] || foo[ 1 ] ) break;
					foo += 2;
				}
				silence += i;
				if ( i < skip_howmany )
				{
					remainder = skip_howmany - i;
					memmove( output_buffer, foo, remainder * sizeof( short ) * 2 );
					break;
				}
				if ( silence >= skip_max )
				{
					eof = 1;
					break;
				}
			}

			startsilence += silence;
			silence = 0;
		}

		if ( do_suppressendsilence )
		{
			m_buffer.resize(skip_max * 2);	// WTF? 44100*2*2 bytes for silence detect???
			if (remainder)
			{
				unsigned long count;
				t_int16 * ptr = m_buffer.get_write_ptr(count);
				memcpy(ptr, output_buffer, remainder * sizeof(t_int16) * 2);
				m_buffer.samples_written(remainder * 2);
				remainder = 0;
			}
		}

		calcfade();
	}

	int decode_run( int16_t* output_buffer, uint16_t size) {
		// note: original impl used a separate output buffer (that needed to be copied later)
		// reused impl from n64 rather than the (older) HT one
		if ( eof || err < 0 ) { 
fprintf(stderr, " decode_run ERROR [%d] [%d]\n", eof, err);		
			return -1;
		}

		if ( no_loop && tag_song_ms && sample_rate && data_written >= (song_len + fade_len) ) {
fprintf(stderr, " decode_run END\n");		
			return -1;
		}
		unsigned int written = 0;

		int samples = size;

		short * ptr;

		if ( do_suppressendsilence )
		{
			unsigned long max_fill = no_loop ? ((song_len + fade_len) - data_written) * 2 : 0;

			unsigned long done = 0;
			unsigned long count= 0;
			ptr = m_buffer.get_write_ptr(count);
			if (max_fill && count > max_fill)
				count = max_fill;

			count /= 2;

			while (count)
			{
				unsigned int todo = size;
				if (todo > count)
					todo = (unsigned int)count;
								
				err = sega_execute( sega_state.get_ptr(), 0x7FFFFFFF, ptr, & todo );
				if ( err < 0 ) { 
fprintf(stderr, "ERROR sega_execute 2\n");		
					throw exception_io_data( "Execution halted with an error." );
				}
				if ( !todo ) { 
fprintf(stderr, "ERROR sega_execute 3\n");		
					throw exception_io_data();
				}
								
				ptr += todo * 2;
				done += todo;
				count -= todo;
			}

			m_buffer.samples_written(done * 2);

			if ( m_buffer.test_silence() )
			{
				eof = 1;
				return -1;
			}

			written = m_buffer.data_available() / 2;
			if (written > size)
				written = size;

			m_buffer.read(output_buffer, written * 2);
			ptr = output_buffer;
		}
		else
		{
			if ( remainder )
			{
				written = remainder;
				remainder = 0;
			}
			else
			{
				written = size;
				err = sega_execute( sega_state.get_ptr(), 0x7FFFFFFF, output_buffer, & written );
				if ( err < 0 ) { 
fprintf(stderr, "ERROR sega_execute 5\n");		
					throw exception_io_data( "Execution halted with an error." );
				}
				if ( !written ) { 
fprintf(stderr, "ERROR sega_execute 6\n");		
					throw exception_io_data(); 
				}
			}

			ptr = (short *) output_buffer;
		}

		int d_start, d_end;
		d_start = data_written;
		data_written += written;
		d_end = data_written;

		calcfade();	// sets song_len

		if (tag_song_ms && d_end > song_len && no_loop )
		{
			short * foo = output_buffer;
			int n;
			for( n = d_start; n < d_end; ++n )
			{
				if ( n > song_len )
				{
					if ( n > song_len + fade_len )
					{
						foo[ 0 ] = 0;
						foo[ 1 ] = 0;						
					}
					else
					{
						int bleh = song_len + fade_len - n;
						foo[ 0 ] = MulDiv( foo[ 0 ], bleh, fade_len );
						foo[ 1 ] = MulDiv( foo[ 1 ], bleh, fade_len );
					}
				}
				foo += 2;
			}
		}

		return written;
	}

	void decode_seek( double p_seconds ) {
		eof = 0;
		
		void * pEmu = sega_state.get_ptr();

		double usfemu_pos = 0.0;

		if ( sample_rate )
		{
			usfemu_pos = double(data_written) / double(sample_rate);

			if ( do_suppressendsilence )
			{
				double buffered_time = (double)(m_buffer.data_available() / 2) / (double)(sample_rate);

				m_buffer.reset();

				usfemu_pos += buffered_time;
			}
		}

		if ( p_seconds < usfemu_pos )		// reset to beginnung
		{
			decode_initialize(seek_buffer, SEEK_BUF_SIZE);
			/*
			usf_restart( m_state->emu_state );
			*/
			usfemu_pos = -startsilence;
			data_written = 0;
		}

		p_seconds -= usfemu_pos;

		// more abortable, and emu doesn't like doing huge numbers of samples per call anyway
		while ( p_seconds )
		{
			unsigned int todo = SEEK_BUF_SIZE;

			int rtn = sega_execute( pEmu, 0x7FFFFFFF, 0, & todo );
			if ( rtn < 0 || ! todo )
			{
				eof = 10;	// XXXXX
				return;
			}			
			
			usfemu_pos += ((double)rtn) / double(sample_rate);

			data_written += rtn;

			p_seconds -= ((double)rtn) / double(sample_rate);

			if ( p_seconds < 0 )
			{
				remainder = (unsigned int)(-p_seconds * double(sample_rate));

				data_written -= remainder;

				memmove( seek_buffer, &seek_buffer[ ( rtn - remainder ) * 2 ], remainder * 2 * sizeof(int16_t) );

				break;
			}
		}

		if (do_suppressendsilence && remainder)
		{
			unsigned long count;
			t_int16 * ptr = m_buffer.get_write_ptr(count);
			memcpy(ptr, seek_buffer, remainder * sizeof(t_int16) * 2);
			m_buffer.samples_written(remainder * 2);
			remainder = 0;
		}
	}
private:
	double MulDiv(int ms, int sampleRate, int d) {
		return ((double)ms)*sampleRate/d;
	}
	void calcfade()
	{
		song_len=MulDiv(tag_song_ms,sample_rate,1000);
		fade_len=MulDiv(tag_fade_ms,sample_rate,1000);
	}
};
static input_xsf g_input_xsf;	
// ------------------------------------------------------------------------------------------------------- 


void ht_boost_volume(unsigned char b) { /*noop*/}

int32_t ht_get_sample_rate() {
	return g_input_xsf.getSamplesRate();
}

int32_t ht_get_samples_to_play() {
	// base for seeking
	return g_input_xsf.getSamplesToPlay();	// in samples (one channel)	
}

int32_t ht_get_samples_played() {
	return g_input_xsf.getDataWritten();
}

int ht_load_file(const char *uri, int16_t *output_buffer, uint16_t outSize) {
	try {
		int retVal= g_input_xsf.open(uri);
		if (retVal < 0) return retVal;	// trigger retry later
		
		g_input_xsf.decode_initialize(output_buffer, outSize);

		return 0;
	} catch(...) {
		return -1;
	}
}

int ht_read(int16_t *output_buffer, uint16_t outSize) {
	return g_input_xsf.decode_run( output_buffer, outSize);	
}

int ht_seek_sample(int sampleTime) {
	// time measured in 1 channel samples
	g_input_xsf.decode_seek( ((double) sampleTime)/(double)ht_get_sample_rate());
    return 0;
}

// use "regular" file ops - which are provided by Emscripten (just make sure all files are previously loaded)

std::string stringToUpper(std::string strToConvert) {
    std::transform(strToConvert.begin(), strToConvert.end(), strToConvert.begin(), ::toupper);

    return strToConvert;
}
void* em_fopen( const char * uri ) {
	// use of upper/lower case is a total mess with these music files..
	std::string file= stringToUpper(std::string(uri));
	return (void*)fopen(file.c_str(), "r");
}
size_t em_fread( void * buffer, size_t size, size_t count, void * handle ) {
	return fread(buffer, size, count, (FILE*)handle );
}
int em_fseek( void * handle, int64_t offset, int whence ) {
	return fseek( (FILE*) handle, offset, whence );
}
long int em_ftell( void * handle ) {
	return  ftell( (FILE*) handle );
}
int em_fclose( void * handle  ) {
	return fclose( (FILE *) handle  );
}

size_t em_fgetlength( FILE * f) {
	int fd= fileno(f);
	struct stat buf;
	fstat(fd, &buf);
	return buf.st_size;	
}	

void ht_setup (void) {
	if (!g_file) {
		g_file = (struct FileAccess_t*) malloc(sizeof( struct FileAccess_t ));
		
		g_file->fopen= em_fopen;
		g_file->fread= em_fread;
		g_file->fseek= em_fseek;
		g_file->ftell= em_ftell;
		g_file->fclose= em_fclose;		
		g_file->fgetlength= em_fgetlength;
	}
}



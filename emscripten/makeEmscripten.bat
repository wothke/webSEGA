::  POOR MAN'S DOS PROMPT BUILD SCRIPT.. make sure to delete the respective built/*.bc files before building!
::  existing *.bc files will not be recompiled. 

:: DO NOT -DUSE_STARSCREAM since EMSCRIPTEN does not handle the x86 assembly code ENABLE_DYNAREC must not be used for same reason
:: (the Mabuse emu also present in the kode54's project does not seem to work here and it was therefore removed)
setlocal enabledelayedexpansion

SET ERRORLEVEL
VERIFY > NUL

:: **** use the "-s WASM" switch to compile WebAssembly output. warning: the SINGLE_FILE approach does NOT currently work in Chrome 63.. ****
set "OPT=   -s WASM=0 -s ASSERTIONS=1  -Wcast-align -fno-strict-aliasing  -s FORCE_FILESYSTEM=1 -s VERBOSE=0 -s SAFE_HEAP=0 -s DISABLE_EXCEPTION_CATCHING=0 -DEMU_COMPILE -DEMU_LITTLE_ENDIAN -DHAVE_STDINT_H -DNO_DEBUG_LOGS -Wno-pointer-sign -I. -I.. -I../Core -I../psflib -I../zlib  -Os -O3 "

if not exist "built/extra.bc" (
	call emcc.bat %OPT% ../psflib/psflib.c ../psflib/psf2fs.c ../zlib/adler32.c ../zlib/compress.c ../zlib/crc32.c ../zlib/gzio.c ../zlib/uncompr.c ../zlib/deflate.c ../zlib/trees.c ../zlib/zutil.c ../zlib/inflate.c ../zlib/infback.c ../zlib/inftrees.c ../zlib/inffast.c -o built/extra.bc
	IF !ERRORLEVEL! NEQ 0 goto :END
)

if not exist "built/core.bc" (
	call emcc.bat -DUSE_M68K -DLSB_FIRST %OPT%   ../Core/sega.c ../Core/dcsound.c ../Core/satsound.c ../Core/yam.c ../Core/arm.c ../Core/m68k/m68kops.c ../Core/m68k/m68kcpu.c  -o built/core.bc
	IF !ERRORLEVEL! NEQ 0 goto :END
)

call emcc.bat %OPT% -s TOTAL_MEMORY=134217728 --memory-init-file 0 --closure 1 --llvm-lto 1  built/extra.bc  built/core.bc  htplug.cpp  adapter.cpp --js-library callback.js  -s EXPORTED_FUNCTIONS="['_emu_setup', '_emu_init','_emu_teardown','_emu_get_current_position','_emu_seek_position','_emu_get_max_position','_emu_set_subsong','_emu_get_track_info','_emu_get_sample_rate','_emu_get_audio_buffer','_emu_get_audio_buffer_length','_emu_compute_audio_samples', '_malloc', '_free']"  -o htdocs/sega.js  -s SINGLE_FILE=0 -s EXTRA_EXPORTED_RUNTIME_METHODS="['ccall', 'Pointer_stringify']"  -s BINARYEN_ASYNC_COMPILATION=1 -s BINARYEN_TRAP_MODE='clamp' && copy /b shell-pre.js + htdocs\sega.js + shell-post.js htdocs\web_sega3.js && del htdocs\sega.js && copy /b htdocs\web_sega3.js + sega_adapter.js htdocs\backend_sega.js && del htdocs\web_sega3.js
:END

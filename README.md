# webSega

Copyright (C) 2018 Juergen Wothke

This is a JavaScript/WebAudio plugin of kode54's "Highly Theoretical" (SegaCore - Saturn and Dreamcast sound emulation). This 
plugin is designed to work with my generic WebAudio ScriptProcessor music player (see separate project). 

It allows to play Sega Dreamcast Sound Format (.DSF/.MINIDSF) files and Sega Saturn Sound 
Format (.SSF/.MINISSF) files.

A live demo of this program can be found here: http://www.wothke.ch/webHT/


## Credits
The project is based on: https://gitlab.kode54.net/kode54/foo_input_ht/


## Project
It includes most of the original "Highly Theoretical" code. All the "Web" specific 
additions (i.e. the whole point of this project) are contained in the "emscripten" subfolder.


## Howto build

You'll need Emscripten (http://kripken.github.io/emscripten-site/docs/getting_started/downloads.html). The make script 
is designed for use of emscripten version 1.37.29 (unless you want to create WebAssembly output, older versions might 
also still work).

The below instructions assume that the webn64 project folder has been moved into the main emscripten 
installation folder (maybe not necessary) and that a command prompt has been opened within the 
project's "emscripten" sub-folder, and that the Emscripten environment vars have been previously 
set (run emsdk_env.bat).

The Web version is then built using the makeEmscripten.bat that can be found in this folder. The 
script will compile directly into the "emscripten/htdocs" example web folder, were it will create 
the backend_HT.js library. (To create a clean-build you have to delete any previously built libs in the 
'built' sub-folder!) The content of the "htdocs" can be tested by first copying it into some 
document folder of a web server. 


## Depencencies

The current version requires version 1.02 (older versions will not
support WebAssembly) of my https://github.com/wothke/webaudio-player.

This project comes without any music files, so you'll also have to get your own and place them
in the htdocs/music folder (you can configure them in the 'songs' list in index.html).


## License

This library is free software; you can redistribute it and/or modify it
under the terms of the GNU Lesser General Public License as published by
the Free Software Foundation; either version 2.1 of the License, or (at
your option) any later version. This library is distributed in the hope
that it will be useful, but WITHOUT ANY WARRANTY; without even the implied
warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
GNU Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public
License along with this library; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301 USA

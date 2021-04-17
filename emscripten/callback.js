// additional emscripten *.js library must be in this dumbshit format..
mergeInto(LibraryManager.library, {
	// returns 0 means file is ready; -1 if file is not yet available
	ht_request_file: function(name) {	
		var r= window['fileRequestCallback'](name);
		return r;
	},
});
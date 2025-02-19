# $Id$
# Author:  Paul Thiessen

# Build application "cn3d"
#################################

WATCHERS = lanczyck

REQUIRES = objects wxWidgets OpenGL PNG MESA

APP = cn3d_nowin

SRC =	aaa_dummy_pch \
	alignment_manager \
	alignment_set \
	animation_controls \
	annotate_dialog \
	atom_set \
	block_multiple_alignment \
	bond \
	cdd_annot_dialog \
	cdd_book_ref_dialog \
	cdd_ref_dialog \
	cdd_splash_dialog \
	chemical_graph \
	cn3d_ba_interface \
	cn3d_blast \
	cn3d_cache \
	cn3d_colors \
	cn3d_glcanvas \
	cn3d_png_nowin \
	cn3d_pssm \
	cn3d_threader \
	cn3d_tools \
	command_processor \
	conservation_colorer \
	coord_set \
	data_manager \
	dist_select_dialog \
	file_messaging \
	messenger \
	molecule \
	molecule_identifier \
	multitext_dialog \
	nowin_main \
	object_3d \
	opengl_renderer \
	pattern_dialog \
	periodic_table \
	preferences_dialog \
	progress_meter \
	residue \
	sequence_display \
	sequence_set \
	sequence_viewer \
	sequence_viewer_widget \
	sequence_viewer_window \
	show_hide_dialog \
	show_hide_manager \
	structure_base \
	structure_set \
	structure_window \
	style_dialog \
	style_manager \
	taxonomy_tree \
	update_viewer \
	update_viewer_window \
	vector_math \
	viewer_base \
	viewer_window_base

LIB =   xbma_refiner_gui wx_tools xbma_refiner xcd_utils xstruct_util xstruct_dp xstruct_thread \
        xblast xalgowinmask xalgodustmask xalgoblastdbindex composition_adjustment seqmasks_io seqdb $(OBJREAD_LIBS) \
  	entrez2 entrez2cli xconnserv xcgi xhtml xcompress xconnect xconnext xser xutil xncbi \
	xalnmgr tables xobjutil id1cli id1 ncbimime cdd cn3d mmdb blast_services xnetblastcli xnetblast scoremat \
	utrtprof taxon1 blastdb xregexp entrez2cli xconnect entrez2 $(SOBJMGR_LIBS) $(Z_LIB) $(PCRE_LIB) $(LMDB_LIB) 

# removed xmlwrapp from LIB

CPPFLAGS = $(PLATFORM_DB_SYMBOLS) \
	$(ORIG_CPPFLAGS) \
	$(WXWIDGETS_INCLUDE) $(OPENGL_INCLUDE) \
	$(Z_INCLUDE) $(PNG_INCLUDE) $(PCRE_INCLUDE)

CXXFLAGS = $(FAST_CXXFLAGS)

LDFLAGS = $(FAST_LDFLAGS)

LIBS = $(PLATFORM_DB_LIBS) \
	$(WXWIDGETS_STATIC_LIBS) $(WXWIDGETS_GL_STATIC_LIBS) $(OSMESA_STATIC_LIBS) \
	$(Z_LIBS) $(PNG_LIBS) $(PCRE_LIBS) $(NETWORK_LIBS) \
	$(BLAST_THIRD_PARTY_LIBS) $(ORIG_LIBS)


# for distribution on linux/gcc, do:
#   gcc  ....  -lpthread -Wl,-Bstatic -lstdc++ -Wl,-Bdynamic

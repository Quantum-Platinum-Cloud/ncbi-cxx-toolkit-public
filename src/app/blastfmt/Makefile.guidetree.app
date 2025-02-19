WATCHERS = camacho boratyng

APP = guidetree
SRC = guide_tree_app

LIB_ = phytree_format xalgoalignnw xalgophytree fastme align_format gene_info \
       xalgoalignutil xhtml xcgi $(BLAST_LIBS) biotree taxon1 $(OBJMGR_LIBS)
LIB = $(LIB_:%=%$(STATIC))

LIBS = $(PSG_CLIENT_LIBS) $(BLAST_THIRD_PARTY_LIBS) $(CMPRS_LIBS) $(NETWORK_LIBS) $(DL_LIBS) $(ORIG_LIBS)

CXXFLAGS = $(FAST_CXXFLAGS)
LDFLAGS  = $(FAST_LDFLAGS)

CPPFLAGS = $(ORIG_CPPFLAGS)

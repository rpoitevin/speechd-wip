gsettings_SCHEMAS = org.freebsoft.speechd.server.gschema.xml \
                    org.freebsoft.speechd.modules.cicero.gschema.xml \
                    org.freebsoft.speechd.modules.festival.gschema.xml \
                    org.freebsoft.speechd.modules.generic.gschema.xml

if flite_support
gsettings_SCHEMAS += org.freebsoft.speechd.modules.flite.gschema.xml
endif

# if ibmtts_support
# gsettings_SCHEMAS += org.freebsoft.speechd.modules.ibmtts.gschema.xml
# endif

if espeak_support
gsettings_SCHEMAS += org.freebsoft.speechd.modules.espeak.gschema.xml
endif

if espeak_ng_support
gsettings_SCHEMAS += org.freebsoft.speechd.modules.espeak-ng.gschema.xml
endif

if ivona_support
gsettings_SCHEMAS += org.freebsoft.speechd.modules.ivona.gschema.xml
endif

if pico_support
gsettings_SCHEMAS += org.freebsoft.speechd.modules.pico.gschema.xml
endif

@INTLTOOL_XML_NOMERGE_RULE@

@GSETTINGS_RULES@

# We need to compile schemas at make time
# to run from source tree
gschemas.compiled: $(gsettings_SCHEMAS:.xml=.valid)
	$(AM_V_GEN) $(GLIB_COMPILE_SCHEMAS) --targetdir=. .

all-local: gschemas.compiled

EXTRA_DIST = org.freebsoft.speechd.modules.cicero.gschema.xml.in \
             org.freebsoft.speechd.modules.espeak.gschema.xml.in \
             org.freebsoft.speechd.modules.espeak-ng.gschema.xml.in \
             org.freebsoft.speechd.modules.festival.gschema.xml.in \
             org.freebsoft.speechd.modules.flite.gschema.xml.in \
             org.freebsoft.speechd.modules.generic.gschema.xml.in \
             org.freebsoft.speechd.modules.ibmtts.gschema.xml.in \
             org.freebsoft.speechd.modules.ivona.gschema.xml.in \
             org.freebsoft.speechd.modules.pico.gschema.xml.in \
             org.freebsoft.speechd.server.gschema.xml.in

DISTCLEANFILES = $(gsettings_SCHEMAS) gschemas.compiled

-include $(top_srcdir)/git.mk

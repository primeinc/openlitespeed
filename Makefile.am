
SUBDIRS = src
AUTOMAKE_OPTIONS = foreign

install-data-hook:
	( cd dist; ./install.sh "$(prefix)" "$(OPENLSWS_USER)" "$(OPENLSWS_GROUP)" "$(OPENLSWS_ADMIN)" "$(OPENLSWS_PASSWORD)" "$(OPENLSWS_EMAIL)" "$(OPENLSWS_ADMINSSL)";)
	( rm -f $(prefix)/modules/cache.la  $(prefix)/modules/mod_lua.la)
	
ACLOCAL_AMFLAGS = ${ACLOCAL_FLAGS}

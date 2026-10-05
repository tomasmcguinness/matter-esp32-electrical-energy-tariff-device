# Parked for now, so not built:
# - dynamic_tariff_delegate.cpp publishes the Commodity Tariff cluster.
# - web_server.cpp served the local web UI (html_app); prices now come from the cloud page.
# - octopus_provider.cpp fetched Octopus tariffs, which were chosen in that UI. Its HTTP helper
#   (http_fetch) has been removed, so it needs one again before it can be built.
idf_component_register(SRC_DIRS          "." "providers"
                       EXCLUDE_SRCS      "dynamic_tariff_delegate.cpp"
                                         "web_server.cpp"
                                         "providers/octopus_provider.cpp"
                       INCLUDE_DIRS      "."
                       PRIV_INCLUDE_DIRS "."
                       LDFRAGMENTS       "linker.lf")

target_compile_options(${COMPONENT_LIB} PRIVATE "-DCHIP_HAVE_CONFIG_H")

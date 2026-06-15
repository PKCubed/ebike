idf_component_register(SRCS "main.c" "pcf8575.c"
                       PRIV_REQUIRES spi_flash driver
                       INCLUDE_DIRS ".")

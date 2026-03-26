find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBGPIOD REQUIRED libgpiod)

################################################################################
#
# net build
#
################################################################################

# nerfnet ######################################################################

add_executable(nerfnet
  nerfnet_main.cc
  radio_interface.cc
  primary_radio_interface.cc
  secondary_radio_interface.cc
)

target_include_directories(nerfnet PRIVATE
  ${tclap_INCLUDE_DIRS}
  ${LIBGPIOD_INCLUDE_DIRS}
)

target_include_directories(nerfnet PUBLIC
  ${PROJECT_SOURCE_DIR}
)

target_link_libraries(nerfnet PUBLIC
  pthread
  rf24
  util
  ${LIBGPIOD_LIBRARIES}
)

install(TARGETS nerfnet DESTINATION bin)

cmake_minimum_required(VERSION 3.28...3.30)

add_library(nz-bootstrap MODULE)
add_library(OBS::nz-bootstrap ALIAS nz-bootstrap)

target_sources(nz-bootstrap PRIVATE plugin-main.c)

target_link_libraries(nz-bootstrap PRIVATE OBS::libobs)

set_target_properties_obs(nz-bootstrap PROPERTIES FOLDER plugins PREFIX "")
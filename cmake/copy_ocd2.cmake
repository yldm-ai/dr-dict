# Copies OpenCC's compiled dictionaries (*.ocd2) from SOURCE to DEST.
file(GLOB dictionaries "${SOURCE}/*.ocd2")
if(NOT dictionaries)
  message(FATAL_ERROR "No OpenCC dictionaries in ${SOURCE}")
endif()
file(COPY ${dictionaries} DESTINATION "${DEST}")

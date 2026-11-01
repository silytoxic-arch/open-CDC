# a copy of examples/cdc_advanced without its waiver file (as the tutorial says)
file(REMOVE_RECURSE "${WORK}")
file(COPY "${EXAMPLE_DIR}/" DESTINATION "${WORK}")
file(REMOVE "${WORK}/waivers.json" "${WORK}/cdc_advanced_waived.ocdc.json")

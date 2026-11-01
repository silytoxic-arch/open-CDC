# the README's commands run from the repository root: a copy of examples/ in WORK
file(REMOVE_RECURSE "${WORK}")
file(COPY "${SOURCE_DIR}/examples" DESTINATION "${WORK}")

/*
PROFILE_SECTIONS.H

The profiling build's own CPU sections (configure.py --profile), declared
and entered as profile.h's are. A normal build's preprocessed sources must
stay as they are (tools/test_profile.py compares them), so in a normal
build these leave no token behind, and are written with no semicolon after
them. profile_scope times one statement, and is the statement alone in a
normal build: it can stand where the statement stood, as an if's branch
without braces.
*/

#ifndef __PROFILE_SECTIONS_H
#define __PROFILE_SECTIONS_H

#ifdef HALO_PROFILE

#include "cseries/profile.h"

#define PROFILE_SECTION(variable, name) static struct profile_section variable = { name, NONE, TRUE };
#define profile_scope_enter(variable) profile_enter(variable)
#define profile_scope_exit(variable) profile_exit(variable)
#define profile_scope(variable, statement) { profile_enter(variable) statement profile_exit(variable) }

#else

#define PROFILE_SECTION(variable, name)
#define profile_scope_enter(variable)
#define profile_scope_exit(variable)
#define profile_scope(variable, statement) statement

#endif

#endif

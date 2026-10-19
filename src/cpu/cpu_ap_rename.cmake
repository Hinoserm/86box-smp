#
# 86Box    A hypervisor and IBM PC system emulator that specializes in
#          running old operating systems and software designed for IBM
#          PC systems and compatibles from 1981 through fairly recent
#          system designs based on the PCI bus.
#
#          This file is part of the 86Box distribution.
#
#          Another processor's core: every symbol the second build of the
#          core defines gets the prefix PREFIX (s1_), in its definitions
#          and in the references to it, so the copy's state and code sit
#          at addresses of their own. What it uses without defining -- the
#          machine, RAM, the devices -- it keeps sharing; its port I/O and
#          the machine work it asks for by name it reaches through
#          cpu_thread.c (cpu_ap_inb and the rest).
#
#          Run as: cmake -DNM= -DOBJCOPY= -DRANLIB= -DOBJDUMP= -DIN= -DOUT=
#                        -DPREFIX= -DLIST= -DUNDEF= -DSHARED= [-DSYMPREFIX=_]
#                        -P cpu_ap_rename.cmake
#
#          SYMPREFIX is what the object format puts in front of every C
#          name (an underscore on Mach-O): the prefix goes after it, so C
#          code names the copies s1_... on every host, and the names in
#          SHARED are C names.
#
# Authors: Michael Pratte, <mpratte@makefox.group>
#
#          Copyright 2026 Michael Pratte.
#

# A COMDAT section is merged with the first core's by name at link time,
# and would hand the other core the first one's variable. The other core
# is built with -mcmodel=small so there are none; make sure.
execute_process(COMMAND ${OBJDUMP} -h ${IN}
                OUTPUT_VARIABLE sections RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "objdump failed on ${IN}")
endif()
if(sections MATCHES "LINK_ONCE")
    message(FATAL_ERROR "The other processors' core has COMDAT sections; it would share them with the first")
endif()

execute_process(COMMAND ${NM} -g --defined-only ${IN}
                OUTPUT_VARIABLE defined RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "nm failed on ${IN}")
endif()

string(REPLACE "\n" ";" lines "${defined}")
set(names "")
foreach(line IN LISTS lines)
    if(line MATCHES "^[0-9a-fA-F]+ [A-Z] ([^ ]+)$")
        set(n "${CMAKE_MATCH_1}")
        if(SYMPREFIX)
            string(LENGTH "${SYMPREFIX}" plen)
            string(SUBSTRING "${n}" 0 ${plen} head)
            if(NOT head STREQUAL SYMPREFIX)
                continue() # not a C name: the compiler's own, left alone
            endif()
            string(SUBSTRING "${n}" ${plen} -1 n)
        endif()
        list(APPEND names "${n}")
    endif()
endforeach()
list(REMOVE_DUPLICATES names)
list(SORT names)

set(out "")
foreach(n IN LISTS names)
    string(APPEND out "${SYMPREFIX}${n} ${SYMPREFIX}${PREFIX}${n}\n")
endforeach()
# Its port I/O, and the machine work it asks for by name, go to the
# machine's thread when it runs on a thread of its own (cpu_thread.c): one
# set of functions for every copy, which finds the processor asking by its
# thread.
foreach(n inb inw inl outb outw outl picint_common smram_backup_all smram_recalc_all
          cpu_apic_base_msr_read_current cpu_apic_base_msr_write_current cpu_cpuid_apic_id_ebx
          mem_writeb_phys mem_writel_phys)
    string(APPEND out "${SYMPREFIX}${n} ${SYMPREFIX}cpu_ap_${n}\n")
endforeach()
file(WRITE ${LIST} "${out}")

execute_process(COMMAND ${OBJCOPY} --redefine-syms=${LIST} ${IN} ${OUT}
                RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "objcopy failed renaming ${IN}")
endif()
execute_process(COMMAND ${RANLIB} ${OUT} RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "ranlib failed on ${OUT}")
endif()

# What the other core reaches outside itself, for review: each is shared
# with the first core, so none may be processor state.
execute_process(COMMAND ${NM} -u ${OUT} OUTPUT_VARIABLE undef)
string(REPLACE "\n" ";" ulines "${undef}")
set(ulist "")
foreach(line IN LISTS ulines)
    # GNU nm prints "U name"; llvm-nm on Mach-O prints the bare name, under
    # an "object.o:" heading. One test at a time: a second MATCHES that
    # fails clears what the first matched.
    set(n "")
    if(line MATCHES "^ +U ([^ ]+)$")
        set(n "${CMAKE_MATCH_1}")
    elseif(line MATCHES "^([^ :]+)$")
        set(n "${CMAKE_MATCH_1}")
    endif()
    if(NOT n STREQUAL "")
        if(SYMPREFIX)
            string(LENGTH "${SYMPREFIX}" plen)
            string(SUBSTRING "${n}" 0 ${plen} head)
            if(head STREQUAL SYMPREFIX)
                string(SUBSTRING "${n}" ${plen} -1 n)
            endif()
        endif()
        list(APPEND ulist "${n}")
    endif()
endforeach()
list(REMOVE_DUPLICATES ulist)
list(SORT ulist)
# Defined in the other core itself (another member): not outside it.
foreach(n IN LISTS names)
    list(REMOVE_ITEM ulist "${PREFIX}${n}")
endforeach()
string(REPLACE ";" "\n" undef "${ulist}")
file(WRITE ${UNDEF} "${undef}\n")

# Each of those has been decided to be the machine's (SHARED); anything
# new has to be decided the same way before the other core may use it.
file(STRINGS ${SHARED} allowed REGEX "^[^#]")
set(unknown "")
foreach(n IN LISTS ulist)
    list(FIND allowed "${n}" at)
    if(at EQUAL -1)
        list(APPEND unknown "${n}")
    endif()
endforeach()
if(unknown)
    string(REPLACE ";" " " unknown "${unknown}")
    message(FATAL_ERROR "The other processor core reaches symbols nobody has decided are the machine's "
                        "(see src/cpu/cpu_ap_shared.txt): ${unknown}")
endif()

list(LENGTH names nnames)
message(STATUS "Processor core ${PREFIX}: ${nnames} symbols renamed")

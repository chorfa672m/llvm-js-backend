; This fails because the linker renames the non-opaque type not the opaque 
; one...

; RUN: echo "%X = linkonce global int 8" | as > %t.2.bc
; RUN: as < %s > %t.1.bc
; RUN: link %t.[12].bc | dis

%X = linkonce global int 7

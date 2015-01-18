; Cuckoo Sandbox - Automated Malware Analysis.
; Copyright (C) 2010-2015 Cuckoo Foundation.
;
; This program is free software: you can redistribute it and/or modify
; it under the terms of the GNU General Public License as published by
; the Free Software Foundation, either version 3 of the License, or
; (at your option) any later version.
;
; This program is distributed in the hope that it will be useful,
; but WITHOUT ANY WARRANTY; without even the implied warranty of
; MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
; GNU General Public License for more details.
;
; You should have received a copy of the GNU General Public License
; along with this program.  If not, see <http://www.gnu.org/licenses/>.

%include "misc.inc"

global hook_info_wrapper
extern hook_info

; Wrapper around hook_info() - the wrapper merely ensures no registers but
; rax are affected after calling it.
_hook_info_wrapper:
    push rax
    pushad
    call qword [hook_info_addr]
    mov qword [rsp+128], rax
    popad
    pop rax
    ret

hook_info_wrapper dq _hook_info_wrapper
hook_info_addr dq hook_info